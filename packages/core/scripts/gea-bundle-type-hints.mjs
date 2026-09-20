// JSDoc type-hint RESTORATION for the bundled-app pipeline.
//
// Vite bundling erases TS annotations; build-gea-vite-geatsc.mjs collects the
// source annotations (collectBundledTypeHints) and this module re-applies them
// to the bundle as JSDoc so the geatsc checker recovers the static types.
// Extracted from build-gea-vite-geatsc.mjs so the apply side is unit-testable
// (see packages/geatsc/test/gea-bundle-type-hints.test.ts) — a silent
// restoration miss downgrades typed slots to gea_cpp_value with no compile
// error, only boxed codegen.
import fs from 'node:fs'
import path from 'node:path'
import { createRequire } from 'node:module'

const requireFromLib = createRequire(new URL('../package.json', import.meta.url))
const { parse } = requireFromLib('@babel/parser')
const traverseModule = requireFromLib('@babel/traverse')
const t = requireFromLib('@babel/types')
const traverse = traverseModule.default || traverseModule

// Scope key for a module-level (non-function) typed binding in the hint maps.
export const MODULE_HINT_SCOPE = '<module>'

export function parseTsx(code) {
  return parse(code, {
    sourceType: 'module',
    plugins: ['jsx', 'typescript'],
  })
}

function nearestPackageJson(startDir) {
  let current = startDir
  while (current && current !== path.dirname(current)) {
    const candidate = path.join(current, 'package.json')
    if (fs.existsSync(candidate)) return candidate
    current = path.dirname(current)
  }
  return null
}

function selectPackageImportTarget(value) {
  if (typeof value === 'string') return value
  if (Array.isArray(value)) {
    for (const item of value) {
      const target = selectPackageImportTarget(item)
      if (target) return target
    }
    return null
  }
  if (!value || typeof value !== 'object') return null
  for (const key of ['default', 'import', 'node', 'types']) {
    const target = selectPackageImportTarget(value[key])
    if (target) return target
  }
  for (const item of Object.values(value)) {
    const target = selectPackageImportTarget(item)
    if (target) return target
  }
  return null
}

function resolvePackageImportTarget(imports, source) {
  if (!imports || typeof imports !== 'object') return null
  const exactTarget = selectPackageImportTarget(imports[source])
  if (exactTarget) return exactTarget
  for (const [key, value] of Object.entries(imports)) {
    const starIndex = key.indexOf('*')
    if (starIndex === -1) continue
    const prefix = key.slice(0, starIndex)
    const suffix = key.slice(starIndex + 1)
    if (!source.startsWith(prefix) || !source.endsWith(suffix)) continue
    const target = selectPackageImportTarget(value)
    if (!target) continue
    const matched = source.slice(prefix.length, source.length - suffix.length)
    return target.replaceAll('*', matched)
  }
  return null
}

function parseBarePackageSpecifier(source) {
  if (!source || source.startsWith('.') || source.startsWith('#') || source.startsWith('/')) return null
  const parts = source.split('/')
  if (source.startsWith('@')) {
    if (parts.length < 2) return null
    return {
      packageName: `${parts[0]}/${parts[1]}`,
      subpath: parts.length > 2 ? `./${parts.slice(2).join('/')}` : '.',
    }
  }
  return {
    packageName: parts[0],
    subpath: parts.length > 1 ? `./${parts.slice(1).join('/')}` : '.',
  }
}

function packageDependencySpec(packageJson, packageName) {
  for (const field of ['dependencies', 'devDependencies', 'optionalDependencies', 'peerDependencies']) {
    const value = packageJson?.[field]?.[packageName]
    if (typeof value === 'string') return value
  }
  return null
}

function resolvePackageExportBase(packageDir, packageJson, subpath) {
  const exportsTarget = resolvePackageImportTarget(packageJson.exports, subpath)
  if (exportsTarget && exportsTarget.startsWith('.')) return path.resolve(packageDir, exportsTarget)
  if (subpath !== '.') return path.resolve(packageDir, subpath.slice(2))
  for (const key of ['source', 'module', 'main', 'types']) {
    const target = packageJson[key]
    if (typeof target === 'string') return path.resolve(packageDir, target)
  }
  return path.resolve(packageDir, 'index')
}

function resolveFileDependencyImportBase(source, srcId) {
  const parsed = parseBarePackageSpecifier(source)
  if (!parsed) return null
  const importingPackageJsonPath = nearestPackageJson(path.dirname(srcId))
  if (!importingPackageJsonPath) return null
  let importingPackageJson
  try {
    importingPackageJson = JSON.parse(fs.readFileSync(importingPackageJsonPath, 'utf8'))
  } catch {
    return null
  }
  const spec = packageDependencySpec(importingPackageJson, parsed.packageName)
  if (!spec?.startsWith('file:')) return null
  const packageDir = path.resolve(path.dirname(importingPackageJsonPath), spec.slice('file:'.length))
  const packageJsonPath = path.join(packageDir, 'package.json')
  if (!fs.existsSync(packageJsonPath)) return null
  let packageJson
  try {
    packageJson = JSON.parse(fs.readFileSync(packageJsonPath, 'utf8'))
  } catch {
    return null
  }
  return resolvePackageExportBase(packageDir, packageJson, parsed.subpath)
}

function resolveLocalImportBase(source, srcId) {
  if (source.startsWith('.')) return path.resolve(path.dirname(srcId), source)
  if (!source.startsWith('#')) return resolveFileDependencyImportBase(source, srcId)
  const packageJsonPath = nearestPackageJson(path.dirname(srcId))
  if (!packageJsonPath) return null
  let packageJson
  try {
    packageJson = JSON.parse(fs.readFileSync(packageJsonPath, 'utf8'))
  } catch {
    return null
  }
  const target = resolvePackageImportTarget(packageJson.imports, source)
  if (!target || !target.startsWith('.')) return null
  return path.resolve(path.dirname(packageJsonPath), target)
}

export function discoverLocalImport(source, srcId, localImports) {
  if (source.endsWith('.css')) return
  const base = resolveLocalImportBase(source, srcId)
  if (!base) return
  for (const candidate of [
    base,
    base + '.tsx',
    base + '.ts',
    base + '.jsx',
    base + '.js',
    path.resolve(base, 'index.tsx'),
    path.resolve(base, 'index.ts'),
    path.resolve(base, 'index.jsx'),
    path.resolve(base, 'index.js'),
  ]) {
    // A directory import resolves `base` to the directory itself — only a
    // FILE candidate is loadable; directories fall through to index.*.
    if (fs.statSync(candidate, { throwIfNoEntry: false })?.isFile()) {
      // Only enqueue JS/TS modules. The bare `base` candidate can be an
      // extension-carrying ASSET import (troika fonts: `import url from
      // './x.woff'`) — every walker that consumes this list parses the file,
      // and feeding a binary asset to the TS parser blows up on its first
      // non-ASCII byte (`wOFF` header → "Unexpected character" at column 4).
      // Assets aren't parseable modules; skip them (the added-extension
      // candidates below are always JS/TS, so a genuine module still resolves).
      if (!/\.(?:mjs|cjs|jsx?|tsx?)$/i.test(candidate)) break
      localImports.push(candidate)
      break
    }
  }
}

export function collectSourceFiles(appDir) {
  const transformable = new Set(['.js', '.jsx', '.ts', '.tsx'])
  const files = []
  const stack = [appDir]
  while (stack.length > 0) {
    const current = stack.pop()
    if (!current || !fs.existsSync(current)) continue
    for (const entry of fs.readdirSync(current, { withFileTypes: true })) {
      const full = path.join(current, entry.name)
      if (entry.isDirectory()) {
        if (['node_modules', 'dist', 'build', '.vite'].includes(entry.name)) continue
        stack.push(full)
        continue
      }
      if (entry.isFile() && transformable.has(path.extname(entry.name))) files.push(full)
    }
  }
  files.sort()
  return files
}

// Only the modules actually REACHABLE from the build entry contribute hints.
// Apps ship alternate entries (maps' index.vector.tsx next to index.device.tsx)
// whose same-named functions otherwise OVERWRITE the live module's hints in the
// name-keyed maps — the restored JSDoc then carries the other entry's param
// names/types, silently fails to match the bundled function, and the param
// widens to gea_cpp_value.
export function collectReachableSourceFiles(appDir, entry) {
  const start = path.resolve(appDir, entry)
  if (!fs.existsSync(start)) return collectSourceFiles(appDir)
  const pending = [start]
  const seen = new Set()
  while (pending.length > 0) {
    const file = pending.pop()
    if (!file || seen.has(file) || !fs.existsSync(file)) continue
    seen.add(file)
    let ast
    try {
      ast = parseTsx(fs.readFileSync(file, 'utf8'))
    } catch {
      continue
    }
    traverse(ast, {
      ImportDeclaration(astPath) {
        discoverLocalImport(astPath.node.source.value, file, pending)
      },
      ExportNamedDeclaration(astPath) {
        const source = astPath.node.source?.value
        if (typeof source === 'string') discoverLocalImport(source, file, pending)
      },
      ExportAllDeclaration(astPath) {
        const source = astPath.node.source?.value
        if (typeof source === 'string') discoverLocalImport(source, file, pending)
      },
    })
  }
  return [...seen].sort()
}

export function insertionIndent(code, offset) {
  const lineStart = code.lastIndexOf('\n', offset - 1) + 1
  return code.slice(lineStart, offset).match(/^[ \t]*/)?.[0] ?? ''
}

// Resolve a class member's owning class name in the BUNDLE, where source
// `class WeatherStore extends Store {}` is emitted as the anonymous expression
// `var WeatherStore = class extends Store {}`. Prefer the class node's own id,
// then fall back to the enclosing `var <Name> = class …` declarator.
export function resolveBundledClassName(astPath) {
  const classPath =
    astPath.isClassDeclaration?.() || astPath.isClassExpression?.()
      ? astPath
      : astPath.findParent((p) => p.isClassDeclaration() || p.isClassExpression())
  if (!classPath) return undefined
  if (classPath.node?.id?.name) return classPath.node.id.name
  const declaratorPath = classPath.findParent((p) => p.isVariableDeclarator())
  if (declaratorPath && t.isIdentifier(declaratorPath.node.id)) return declaratorPath.node.id.name
  return undefined
}

export function resolveFunctionHintName(astPath) {
  const functionPath = astPath.getFunctionParent?.()
  const node = functionPath?.node
  if (!node) return undefined
  if (t.isFunctionDeclaration(node) && node.id?.name) return node.id.name
  if ((t.isFunctionExpression(node) || t.isArrowFunctionExpression(node)) && functionPath) {
    const declaratorPath = functionPath.findParent((p) => p.isVariableDeclarator())
    if (declaratorPath && t.isIdentifier(declaratorPath.node.id)) return declaratorPath.node.id.name
  }
  if (functionPath?.isClassMethod?.() || functionPath?.isClassPrivateMethod?.()) {
    const method = functionPath.node
    if (!t.isIdentifier(method.key)) return undefined
    const className = resolveBundledClassName(functionPath)
    return className ? `${className}#${method.key.name}` : undefined
  }
  return undefined
}

// Module-graph-snapshot counterpart of the `Class#el` bundle hint. The lean
// ReactiveComponent transform (vite-plugin-gea) strips the
// `extends ReactiveComponent<T>` heritage and re-injects the inherited `el`
// slot as a bare `el = null` class field — annotation-less, because the
// transform has no type context. The snapshot is otherwise fully typed
// (captured pre-type-strip), so this one field is what separates `this.el`
// from its native NodeHandle lowering: an unannotated `el` types as `any`,
// stores boxed, and the boxed record carries no `__gea_node_id` — so
// `this.el.getContext('2d')` binds the canvas context to node -1 and every
// draw silently no-ops. Re-attach the `T | null` annotation recovered from
// the app source's heritage generic as a REAL TS annotation: snapshots are
// .tsx, where JSDoc `@type` is ignored by the checker.
export function restoreModuleGraphElAnnotations(moduleGraphDir, propertyHints) {
  const manifestPath = path.join(moduleGraphDir, 'gea-module-graph.json')
  if (!fs.existsSync(manifestPath)) return
  const elHints = new Map()
  for (const [key, typeText] of propertyHints ?? []) {
    const match = /^([A-Za-z_$][\w$]*)#el$/.exec(key)
    if (match) elHints.set(match[1], typeText)
  }
  if (elHints.size === 0) return
  const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'))
  const manifestDir = path.dirname(manifestPath)
  const snapshotPaths = new Set()
  for (const module of manifest.modules ?? []) {
    for (const source of [module.originalSource, module.transformedSource]) {
      if (source) snapshotPaths.add(path.resolve(manifestDir, source))
    }
  }
  for (const snapshotPath of snapshotPaths) {
    if (!fs.existsSync(snapshotPath)) continue
    const code = fs.readFileSync(snapshotPath, 'utf8')
    if (!code.includes('el = null')) continue
    const patched = annotateInjectedElFields(code, elHints)
    if (patched !== code) fs.writeFileSync(snapshotPath, patched)
  }
}

function annotateInjectedElFields(code, elHints) {
  let ast
  try {
    ast = parseTsx(code)
  } catch {
    return code
  }
  const insertions = []
  traverse(ast, {
    Class(astPath) {
      const className = resolveBundledClassName(astPath)
      const typeText = className ? elHints.get(className) : undefined
      if (!typeText) return
      // Every type name the annotation references must already resolve in the
      // snapshot (surviving `import type` line, or a local alias/interface) —
      // an unresolvable name would trade a boxed `el` for a checker error.
      if (!hintTypeNamesResolvable(code, typeText)) return
      for (const member of astPath.node.body.body) {
        if (
          t.isClassProperty(member) &&
          !member.computed &&
          t.isIdentifier(member.key, { name: 'el' }) &&
          !member.typeAnnotation &&
          t.isNullLiteral(member.value) &&
          member.key.end != null
        ) {
          insertions.push({ offset: member.key.end, text: `: ${typeText}` })
        }
      }
    },
  })
  let patched = code
  for (const { offset, text } of insertions.sort((a, b) => b.offset - a.offset)) {
    patched = `${patched.slice(0, offset)}${text}${patched.slice(offset)}`
  }
  return patched
}

// DOM-lib element names resolve ambiently (no import) in the geatsc checker.
const AMBIENT_HINT_TYPE_NAMES = new Set([
  'Node',
  'Element',
  'HTMLElement',
  'HTMLCanvasElement',
  'DocumentFragment',
  'Text',
  'Comment',
])

function hintTypeNamesResolvable(code, typeText) {
  const names = new Set(typeText.match(/\b[A-Za-z_$][\w$]*\b/g) ?? [])
  names.delete('null')
  names.delete('undefined')
  for (const name of names) {
    if (AMBIENT_HINT_TYPE_NAMES.has(name)) continue
    const namePattern = name.replace(/\$/g, '\\$')
    const importedOrDeclared = new RegExp(
      `(^|\\n)\\s*import\\b[^\\n;]*\\b${namePattern}\\b|\\b(?:type|interface|class)\\s+${namePattern}\\b`
    )
    if (!importedOrDeclared.test(code)) return false
  }
  return true
}

export function applyBundledTypeHints(code, hints, referencePath) {
  const propertyHints = hints.propertyHints ?? new Map()
  const localHints = hints.localHints ?? new Map()
  const inlineCallableHints = hints.inlineCallableHints ?? new Map()
  const classHints = hints.classHints ?? new Map()
  if (
    hints.returnHints.size === 0 &&
    hints.paramHints.size === 0 &&
    propertyHints.size === 0 &&
    localHints.size === 0 &&
    inlineCallableHints.size === 0 &&
    classHints.size === 0
  )
    return code
  const ast = parse(code, {
    sourceType: 'unambiguous',
    plugins: ['jsx'],
  })
  const insertions = new Map()
  const jsDocForFunction = (name) => {
    const params = hints.paramHints.get(name) ?? []
    const returnType = hints.returnHints.get(name)
    if (params.length === 0 && !returnType) return null
    const lines = ['/**']
    // JSDoc bracket syntax marks optional params, so the checker types them
    // `T | undefined` and geatsc lowers a null-capable native slot.
    for (const param of params) lines.push(` * @param {${param.type}} ${param.optional ? `[${param.name}]` : param.name}`)
    if (returnType) lines.push(` * @returns {${returnType}}`)
    lines.push(' */')
    return lines.join('\n')
  }
  // SINGLE-LINE JSDoc for inline callables: a newline between `return` and the
  // arrow it precedes would trigger ASI (`return;`), so the comment and its
  // trailing separator must stay on the callable's own line. TS still parses
  // multiple `@param` tags packed on one line.
  const jsDocForInlineCallable = (entry) => {
    const parts = entry.params.map((param) => `@param {${param.type}} ${param.optional ? `[${param.name}]` : param.name}`)
    if (entry.returnType) parts.push(`@returns {${entry.returnType}}`)
    if (parts.length === 0) return null
    return `/** ${parts.join(' ')} */`
  }
  const jsDocForClass = (name) => {
    const tag = classHints.get(name)
    return tag ? `/** ${tag} */` : null
  }
  const inlineConsumed = new Map()
  const normalizedJsDocLine = (line) => line.trim().replace(/\s+/g, ' ')
  const mergeIntoLeadingJsDoc = (offset, jsDocText) => {
    const tagLines = jsDocText
      .split('\n')
      .slice(1, -1)
      .filter((line) => line.trim().startsWith('* @'))
    if (tagLines.length === 0) return false
    const before = code.slice(0, offset)
    const bodyEnd = before.trimEnd().length
    if (bodyEnd < 2 || before.slice(bodyEnd - 2, bodyEnd) !== '*/') return false
    const blockStart = before.lastIndexOf('/**', bodyEnd - 2)
    if (blockStart < 0) return false
    const block = code.slice(blockStart, bodyEnd)
    if (!block.includes('\n')) return false
    const existingLines = new Set(block.split('\n').map(normalizedJsDocLine))
    const missingLines = tagLines.filter((line) => !existingLines.has(normalizedJsDocLine(line)))
    if (missingLines.length === 0) return true
    const closeLineStart = code.lastIndexOf('\n', bodyEnd - 2) + 1
    if (closeLineStart <= blockStart) return false
    const current = insertions.get(closeLineStart) ?? ''
    insertions.set(closeLineStart, `${current}${missingLines.join('\n')}\n`)
    return true
  }
  const visitInlineCallable = (astPath) => {
    const node = astPath.node
    const parent = astPath.parent
    if (parent && t.isVariableDeclarator(parent) && parent.init === node) return
    if (!node.params.every((p) => t.isIdentifier(p))) return
    const scope = resolveFunctionHintName(astPath) ?? MODULE_HINT_SCOPE
    const key = `${scope}|${node.params.map((p) => p.name).join(',')}`
    const entries = inlineCallableHints.get(key)
    if (!entries || entries.length === 0) return
    const index = inlineConsumed.get(key) ?? 0
    if (index >= entries.length) return
    inlineConsumed.set(key, index + 1)
    const jsDocText = jsDocForInlineCallable(entries[index])
    if (jsDocText && node.start != null && !insertions.has(node.start)) {
      insertions.set(node.start, `${jsDocText} `)
    }
  }
  const addInsertion = (offset, jsDocText) => {
    if (offset == null || insertions.has(offset)) return
    if (jsDocText.startsWith('/**') && mergeIntoLeadingJsDoc(offset, jsDocText)) return
    insertions.set(offset, `${jsDocText}\n${insertionIndent(code, offset)}`)
  }
  traverse(ast, {
    FunctionDeclaration(astPath) {
      const node = astPath.node
      const name = node.id?.name
      if (!name) return
      const jsDocText = jsDocForFunction(name)
      if (jsDocText) addInsertion(node.start, jsDocText)
    },
    ArrowFunctionExpression(astPath) {
      visitInlineCallable(astPath)
    },
    FunctionExpression(astPath) {
      visitInlineCallable(astPath)
    },
    VariableDeclaration(astPath) {
      for (const declaration of astPath.node.declarations) {
        if (!t.isIdentifier(declaration.id)) continue
        if (!declaration.init || (!t.isArrowFunctionExpression(declaration.init) && !t.isFunctionExpression(declaration.init))) continue
        const jsDocText = jsDocForFunction(declaration.id.name)
        if (jsDocText) addInsertion(astPath.node.start, jsDocText)
      }
    },
    ClassMethod(astPath) {
      const node = astPath.node
      if ((node.kind !== 'method' && node.kind !== 'constructor') || !t.isIdentifier(node.key)) return
      // The bundle emits `var TetrisStore = class extends Store {}` — an
      // ANONYMOUS class expression. Resolve through the enclosing declarator
      // (resolveBundledClassName), or no class-method hint ever applies and
      // unannotated method RETURNS fall back to gea_cpp_value (params often
      // survive via call-site inference, masking the loss).
      const className = resolveBundledClassName(astPath)
      if (!className) return
      const memberName = node.kind === 'constructor' ? 'constructor' : node.key.name
      const jsDocText = jsDocForFunction(`${className}#${memberName}`)
      if (jsDocText) addInsertion(node.start, jsDocText)
    },
    Class(astPath) {
      const node = astPath.node
      const className = resolveBundledClassName(astPath)
      if (!className) return
      const jsDocText = jsDocForClass(className)
      if (jsDocText) addInsertion(node.start, jsDocText)
    },
    ClassProperty(astPath) {
      const node = astPath.node
      if (node.computed || !t.isIdentifier(node.key)) return
      const className = resolveBundledClassName(astPath)
      if (!className) return
      const typeText = propertyHints.get(`${className}#${node.key.name}`)
      if (typeText) addInsertion(node.start, `/** @type {${typeText}} */`)
    },
    VariableDeclarator(astPath) {
      const node = astPath.node
      if (!t.isIdentifier(node.id)) return
      const declarationPath = astPath.parentPath
      if (!declarationPath?.isVariableDeclaration?.()) return
      if (declarationPath.node.declarations.length !== 1) return
      const functionName = resolveFunctionHintName(astPath) ?? MODULE_HINT_SCOPE
      const typeText = localHints.get(`${functionName}#${node.id.name}`)
      if (typeText) addInsertion(declarationPath.node.start, `/** @type {${typeText}} */`)
    },
  })
  let out = code
  for (const [offset, text] of [...insertions].sort((a, b) => b[0] - a[0])) {
    out = `${out.slice(0, offset)}${text}${out.slice(offset)}`
  }
  if (!referencePath) return out
  return `/// <reference path="${referencePath}" />\n${out}`
}
