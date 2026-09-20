#!/usr/bin/env node

import crypto from 'node:crypto'
import fs from 'node:fs'
import path from 'node:path'
import process from 'node:process'
import { fileURLToPath } from 'node:url'
import ts from 'typescript'

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url))
const packageDirectory = path.resolve(scriptDirectory, '../..')
const sourceDirectory = path.join(packageDirectory, 'src')
const defaultBaselinePath = path.join(scriptDirectory, 'gea-rendered-cpp-ratchet-baseline.json')
const defaultOutputPath = path.join(scriptDirectory, '.artifacts', 'gea-rendered-cpp-ratchet-current.json')

const schemaVersion = 1
const scannerVersion = '1.0.0'

// These are the independently audited ceilings. They stay fixed while rows are
// removed; reaching zero does not require rewriting the baseline.
const hardCeilings = Object.freeze({
  terminalHooks: 1,
  renderedSourceReads: 29,
  namedActiveRoots: 19,
  sourceRewriterExportedRoots: 16,
  dormantInventoryRoots: 17,
  dormantInventoryClosureFunctions: 84,
  dormantOnlyFunctions: 72,
})

const auditedActiveRootNames = new Set([
  'isModuleFirstCppOutput',
  'findModuleFirstCompilerRuntimeHeader',
  'findModuleFirstRuntimeGlobalsHeader',
  'topLevelValueAccessorNamesFromSource',
  'decideStoreHubEmission',
  'enrichNamedArrayShapesFromSource',
  'reconcileInterfaceStructReuse',
  'typedStoreAccessorTypesFromSource',
  'transformModuleFirstGeneratedSource',
  'splitModuleFirstMountedRenderers',
  'includeRecordAliasTypeHeaders',
  'appendMountedRenderersToModuleSources',
  'stubNativeReplacedModuleSources',
  'removeNativeStubTemplateCacheResets',
  'externalizeDrainMicrotasksDeclaration',
  'ensureModuleFirstPluginRuntimeSource',
  'replaceOutOfLineGeneratedRenderers',
  'bridgeGeneratedComponentRenderers',
])

const auditedDormantRootNames = new Set([
  'insertIrComponentShells',
  'removeGeneratedComponentDefinitions',
  'removeUnusedRuntimePrototypes',
  'removeUnusedDynamicUiRuntime',
  'removeUnusedRecordBoxingSurface',
  'removeNoThrowHostGuards',
  'inlineDirectMountIifes',
  'inlineRequestAnimationFrameSelfLoopIifes',
  'removeUnusedGeaCppKeyUsing',
  'removeUnusedGeaDocumentRuntimeInclude',
  'removeUnusedDynamicPropertySurfaces',
  'removeUnusedNativeDomValueBridge',
  'removeUnusedFunctionRenderers',
  'insertAfterRuntimeInclude',
  'insertAfterGeneratedNamespaceOpen',
  'insertBeforeGeneratedNamespaceOpen',
  'insertBeforeGeneratedNamespaceClose',
])

function parseArguments(argv) {
  const result = {
    baselinePath: defaultBaselinePath,
    outputPath: defaultOutputPath,
    initializeBaseline: false,
    pruneBaseline: false,
  }
  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index]
    if (argument === '--baseline') result.baselinePath = path.resolve(requireValue(argv, ++index, argument))
    else if (argument === '--output') result.outputPath = path.resolve(requireValue(argv, ++index, argument))
    else if (argument === '--initialize-baseline') result.initializeBaseline = true
    else if (argument === '--prune-baseline') result.pruneBaseline = true
    else if (argument === '--help') {
      process.stdout.write(
        [
          'Usage: ratchet-gea-rendered-cpp-source.mjs [options]',
          '  --output <path>              Complete canonical diagnostic artifact',
          '  --baseline <path>            Baseline JSON path',
          '  --initialize-baseline        Create a missing baseline from the current tree',
          '  --prune-baseline             Remove absent rows; additions remain forbidden',
          '',
        ].join('\n'),
      )
      process.exit(0)
    } else throw new Error(`Unknown argument: ${argument}`)
  }
  if (result.initializeBaseline && result.pruneBaseline) {
    throw new Error('--initialize-baseline and --prune-baseline are mutually exclusive')
  }
  return result
}

function requireValue(argv, index, option) {
  const value = argv[index]
  if (!value || value.startsWith('--')) throw new Error(`${option} requires a value`)
  return value
}

function sha256(value) {
  return crypto.createHash('sha256').update(value).digest('hex')
}

function canonicalJson(value) {
  return `${JSON.stringify(value, null, 2)}\n`
}

function normalizedPath(fileName) {
  return path.relative(packageDirectory, fileName).split(path.sep).join('/')
}

function readProgram() {
  const configPath = path.join(packageDirectory, 'tsconfig.json')
  const read = ts.readConfigFile(configPath, ts.sys.readFile)
  if (read.error) throw new Error(formatDiagnostics([read.error]))
  const parsed = ts.parseJsonConfigFileContent(read.config, ts.sys, packageDirectory, undefined, configPath)
  if (parsed.errors.length > 0) throw new Error(formatDiagnostics(parsed.errors))
  const program = ts.createProgram({ rootNames: parsed.fileNames, options: { ...parsed.options, noEmit: true } })
  const diagnostics = ts.getPreEmitDiagnostics(program)
  if (diagnostics.length > 0) throw new Error(formatDiagnostics(diagnostics))
  return program
}

function formatDiagnostics(diagnostics) {
  return ts.formatDiagnosticsWithColorAndContext(diagnostics, {
    getCanonicalFileName: (fileName) => fileName,
    getCurrentDirectory: () => packageDirectory,
    getNewLine: () => '\n',
  })
}

function isFunctionBoundary(node) {
  return (
    ts.isFunctionDeclaration(node) ||
    ts.isMethodDeclaration(node) ||
    ts.isGetAccessorDeclaration(node) ||
    ts.isSetAccessorDeclaration(node) ||
    ((ts.isArrowFunction(node) || ts.isFunctionExpression(node)) &&
      (ts.isVariableDeclaration(node.parent) || ts.isPropertyAssignment(node.parent)))
  )
}

function declarationName(node) {
  if (ts.isFunctionDeclaration(node) || ts.isMethodDeclaration(node) || ts.isGetAccessorDeclaration(node) || ts.isSetAccessorDeclaration(node)) {
    return node.name && ts.isIdentifier(node.name) ? node.name.text : node.name?.getText()
  }
  if (ts.isVariableDeclaration(node.parent) && ts.isIdentifier(node.parent.name)) return node.parent.name.text
  if (ts.isPropertyAssignment(node.parent)) {
    return ts.isIdentifier(node.parent.name) || ts.isStringLiteral(node.parent.name) ? node.parent.name.text : node.parent.name.getText()
  }
  return undefined
}

function enclosingNamedFunctionNames(node) {
  const names = []
  for (let current = node.parent; current; current = current.parent) {
    if (!isFunctionBoundary(current)) continue
    const name = declarationName(current)
    if (name) names.push(name)
  }
  return names.reverse()
}

function hasExportModifier(node) {
  const declaration = ts.isArrowFunction(node) || ts.isFunctionExpression(node) ? node.parent : node
  return ts.canHaveModifiers(declaration) && !!ts.getModifiers(declaration)?.some((modifier) => modifier.kind === ts.SyntaxKind.ExportKeyword)
}

function canonicalSymbol(checker, node) {
  const symbol = checker.getSymbolAtLocation(node)
  if (!symbol) return undefined
  return symbol.flags & ts.SymbolFlags.Alias ? checker.getAliasedSymbol(symbol) : symbol
}

function collectOwners(program) {
  const checker = program.getTypeChecker()
  const owners = []
  const ownerByNode = new Map()
  const ownerByDeclaration = new Map()
  const ownerBySymbol = new Map()

  for (const sourceFile of program.getSourceFiles()) {
    if (!sourceFile.fileName.startsWith(`${sourceDirectory}${path.sep}`) || sourceFile.isDeclarationFile) continue
    const visit = (node) => {
      if (isFunctionBoundary(node)) {
        const name = declarationName(node)
        if (name) {
          const file = normalizedPath(sourceFile.fileName)
          const owner = {
            id: `${file}#${[...enclosingNamedFunctionNames(node), name].join('.')}`,
            file,
            name,
            node,
            exported: hasExportModifier(node),
          }
          owners.push(owner)
          ownerByNode.set(node, owner)
          ownerByDeclaration.set(node, owner)
          if (node.name) {
            const symbol = canonicalSymbol(checker, node.name)
            if (symbol) ownerBySymbol.set(symbol, owner)
          } else if (ts.isVariableDeclaration(node.parent) && ts.isIdentifier(node.parent.name)) {
            const symbol = canonicalSymbol(checker, node.parent.name)
            if (symbol) ownerBySymbol.set(symbol, owner)
            ownerByDeclaration.set(node.parent, owner)
          } else if (ts.isPropertyAssignment(node.parent)) {
            const symbol = canonicalSymbol(checker, node.parent.name)
            if (symbol) ownerBySymbol.set(symbol, owner)
            ownerByDeclaration.set(node.parent, owner)
          }
        }
      }
      ts.forEachChild(node, visit)
    }
    visit(sourceFile)
  }

  const duplicates = new Map()
  for (const owner of owners) {
    const prior = duplicates.get(owner.id) ?? []
    prior.push(owner)
    duplicates.set(owner.id, prior)
  }
  for (const [id, group] of duplicates) {
    if (group.length < 2) continue
    for (const owner of group) owner.id = `${id}@${sha256(normalizeSignature(owner.node, checker, ownerBySymbol)).slice(0, 12)}`
  }
  return { checker, owners, ownerByNode, ownerByDeclaration, ownerBySymbol }
}

function ownerForDeclaration(declaration, indexes) {
  for (let current = declaration; current; current = current.parent) {
    const direct = indexes.ownerByDeclaration.get(current) ?? indexes.ownerByNode.get(current)
    if (direct) return direct
    if (ts.isSourceFile(current)) break
  }
  return undefined
}

function calledOwner(call, indexes) {
  const target = ts.isPropertyAccessExpression(call.expression) ? call.expression.name : call.expression
  const symbol = canonicalSymbol(indexes.checker, target)
  if (!symbol) return undefined
  const direct = indexes.ownerBySymbol.get(symbol)
  if (direct) return direct
  for (const declaration of symbol.declarations ?? []) {
    const owner = ownerForDeclaration(declaration, indexes)
    if (owner) return owner
  }
  return undefined
}

function visitOwnerBody(owner, indexes, callback) {
  const visit = (node) => {
    if (node !== owner.node && indexes.ownerByNode.has(node)) return
    callback(node)
    ts.forEachChild(node, visit)
  }
  visit(owner.node)
}

function buildCallGraph(indexes) {
  const graph = new Map(indexes.owners.map((owner) => [owner.id, new Set()]))
  for (const owner of indexes.owners) {
    visitOwnerBody(owner, indexes, (node) => {
      if (!ts.isCallExpression(node)) return
      const target = calledOwner(node, indexes)
      if (target && target.id !== owner.id) graph.get(owner.id).add(target.id)
    })
  }
  return graph
}

function transitiveClosure(rootIds, graph) {
  const reached = new Set()
  const queue = [...rootIds]
  while (queue.length > 0) {
    const id = queue.shift()
    if (!id || reached.has(id)) continue
    reached.add(id)
    for (const target of graph.get(id) ?? []) if (!reached.has(target)) queue.push(target)
  }
  return reached
}

function typeIncludesString(type) {
  if (type.flags & ts.TypeFlags.StringLike) return true
  if (type.isUnionOrIntersection()) return type.types.some(typeIncludesString)
  return false
}

function typeIsRenderedSourceCarrier(type, checker) {
  if (type.isUnionOrIntersection()) return type.types.some((part) => typeIsRenderedSourceCarrier(part, checker))
  const source = type.getProperty('source')
  const fileName = type.getProperty('fileName')
  if (!source || !fileName) return false
  const sourceDeclaration = source.valueDeclaration ?? source.declarations?.[0]
  const fileDeclaration = fileName.valueDeclaration ?? fileName.declarations?.[0]
  return (
    !!sourceDeclaration &&
    !!fileDeclaration &&
    typeIncludesString(checker.getTypeOfSymbolAtLocation(source, sourceDeclaration)) &&
    typeIncludesString(checker.getTypeOfSymbolAtLocation(fileName, fileDeclaration))
  )
}

function ownerHasSourceLikeParameter(owner, checker) {
  return owner.node.parameters.some((parameter) => {
    const type = checker.getTypeAtLocation(parameter)
    const name = parameter.name.getText()
    return typeIncludesString(type) || typeIsRenderedSourceCarrier(type, checker) || /(?:source|sources|rendered|cpp)/i.test(name)
  })
}

const textMethodNames = new Set([
  'charAt',
  'charCodeAt',
  'endsWith',
  'exec',
  'includes',
  'indexOf',
  'lastIndexOf',
  'match',
  'matchAll',
  'replace',
  'replaceAll',
  'search',
  'slice',
  'split',
  'startsWith',
  'substring',
  'substr',
  'test',
  'trim',
])

function ownerHasTextOperation(owner, indexes) {
  let found = false
  visitOwnerBody(owner, indexes, (node) => {
    if (
      ts.isCallExpression(node) &&
      ts.isPropertyAccessExpression(node.expression) &&
      textMethodNames.has(node.expression.name.text)
    ) {
      found = true
    }
  })
  return found
}

function sourceRewriterExportCandidate(owner, indexes) {
  if (!owner.exported || !ownerHasSourceLikeParameter(owner, indexes.checker) || !ownerHasTextOperation(owner, indexes)) return false
  let cppEvidence = false
  visitOwnerBody(owner, indexes, (node) => {
    if (
      (ts.isStringLiteralLike(node) || ts.isRegularExpressionLiteral(node)) &&
      /(?:\.cpp|\.hpp|#include|\bnamespace\b|\bclass\s+[A-Za-z_]|std::|gea_|[A-Za-z_][\w]*::[A-Za-z_])/.test(node.text)
    ) {
      cppEvidence = true
    }
  })
  return cppEvidence || owner.node.parameters.some((parameter) => typeIsRenderedSourceCarrier(indexes.checker.getTypeAtLocation(parameter), indexes.checker))
}

const renderedTextTaint = 1
const selfRenderedTextTaint = 2
const selfRenderedProducerNames = new Set(['generateCppIrMountedSource', 'generateModuleFirstGeaIrHeader'])
const selfRenderReparseFiles = new Set(['src/index.ts', 'src/cpp-mounted.ts', 'src/cpp-ir.ts'])
const selfRenderedParameterIndexes = new Map([
  ['rendererReadsStoreParam', [0]],
  ['mountedRendererDeclarationFromLines', [0]],
  ['withComponentLocalConstAccessors', [1]],
  ['mountedRendererGlobalAccessorDeclarations', [0]],
  ['mountedRendererAssetDeclarations', [0]],
])

function bindingSymbol(checker, name) {
  return ts.isIdentifier(name) ? canonicalSymbol(checker, name) : undefined
}

function mergeMask(map, key, mask) {
  if (!key || mask === 0) return false
  const prior = map.get(key) ?? 0
  const next = prior | mask
  if (next === prior) return false
  map.set(key, next)
  return true
}

function callName(call) {
  if (ts.isIdentifier(call.expression)) return call.expression.text
  if (ts.isPropertyAccessExpression(call.expression)) return call.expression.name.text
  return ''
}

function expressionTaint(node, state) {
  if (!node) return 0
  if (ts.isIdentifier(node)) return state.symbolTaint.get(canonicalSymbol(state.indexes.checker, node)) ?? 0
  if (ts.isPropertyAccessExpression(node)) {
    if (propertyAccessIsRenderedSource(node, state.indexes.checker)) return renderedTextTaint
    return expressionTaint(node.expression, state)
  }
  if (ts.isElementAccessExpression(node)) {
    return expressionTaint(node.expression, state) | expressionTaint(node.argumentExpression, state)
  }
  if (ts.isCallExpression(node) || ts.isNewExpression(node)) {
    let mask = 0
    if (ts.isCallExpression(node)) {
      const target = calledOwner(node, state.indexes)
      if (target) {
        mask |= state.returnTaint.get(target.id) ?? 0
      } else {
        mask |= expressionTaint(node.expression, state)
        for (const argument of node.arguments) mask |= expressionTaint(argument, state)
      }
      if (selfRenderedProducerNames.has(callName(node))) mask |= selfRenderedTextTaint
    } else {
      mask |= expressionTaint(node.expression, state)
      for (const argument of node.arguments ?? []) mask |= expressionTaint(argument, state)
    }
    return mask
  }
  if (ts.isBinaryExpression(node)) return expressionTaint(node.left, state) | expressionTaint(node.right, state)
  if (ts.isConditionalExpression(node)) {
    return expressionTaint(node.condition, state) | expressionTaint(node.whenTrue, state) | expressionTaint(node.whenFalse, state)
  }
  if (ts.isParenthesizedExpression(node) || ts.isAsExpression(node) || ts.isTypeAssertionExpression(node) || ts.isNonNullExpression(node)) {
    return expressionTaint(node.expression, state)
  }
  if (ts.isTemplateExpression(node)) {
    return node.templateSpans.reduce((mask, span) => mask | expressionTaint(span.expression, state), 0)
  }
  if (ts.isArrayLiteralExpression(node)) return node.elements.reduce((mask, element) => mask | expressionTaint(element, state), 0)
  if (ts.isObjectLiteralExpression(node)) {
    return node.properties.reduce((mask, property) => {
      if (ts.isPropertyAssignment(property)) return mask | expressionTaint(property.initializer, state)
      if (ts.isShorthandPropertyAssignment(property)) return mask | expressionTaint(property.name, state)
      if (ts.isSpreadAssignment(property)) return mask | expressionTaint(property.expression, state)
      return mask
    }, 0)
  }
  if (ts.isAwaitExpression(node) || ts.isYieldExpression(node)) return expressionTaint(node.expression, state)
  return 0
}

function analyzeTextTaint(indexes, relevantOwners, hookOwners, dormantExportRoots) {
  const symbolTaint = new Map()
  const returnTaint = new Map()
  const state = { indexes, symbolTaint, returnTaint }
  for (const hook of hookOwners) {
    const sources = hook.node.parameters[0]
    mergeMask(symbolTaint, sources ? bindingSymbol(indexes.checker, sources.name) : undefined, renderedTextTaint)
  }
  for (const root of dormantExportRoots) {
    for (const parameter of root.node.parameters) {
      const type = indexes.checker.getTypeAtLocation(parameter)
      if (typeIncludesString(type) || typeIsRenderedSourceCarrier(type, indexes.checker) || /(?:source|sources|rendered|cpp)/i.test(parameter.name.getText())) {
        mergeMask(symbolTaint, bindingSymbol(indexes.checker, parameter.name), renderedTextTaint)
      }
    }
  }
  for (const owner of indexes.owners) {
    for (const index of selfRenderedParameterIndexes.get(owner.name) ?? []) {
      const parameter = owner.node.parameters[index]
      mergeMask(symbolTaint, parameter ? bindingSymbol(indexes.checker, parameter.name) : undefined, selfRenderedTextTaint)
    }
  }

  let changed = true
  while (changed) {
    changed = false
    for (const owner of indexes.owners) {
      if (!relevantOwners.has(owner.id)) continue
      visitOwnerBody(owner, indexes, (node) => {
        if (ts.isVariableDeclaration(node) && node.initializer) {
          changed = mergeMask(symbolTaint, bindingSymbol(indexes.checker, node.name), expressionTaint(node.initializer, state)) || changed
        }
        if (ts.isBinaryExpression(node) && node.operatorToken.kind === ts.SyntaxKind.EqualsToken && ts.isIdentifier(node.left)) {
          changed = mergeMask(symbolTaint, canonicalSymbol(indexes.checker, node.left), expressionTaint(node.right, state)) || changed
        }
        if (ts.isReturnStatement(node) && node.expression) {
          changed = mergeMask(returnTaint, owner.id, expressionTaint(node.expression, state)) || changed
        }
        if (ts.isCallExpression(node)) {
          const target = calledOwner(node, indexes)
          if (!target) return
          node.arguments.forEach((argument, index) => {
            const parameter = target.node.parameters[index]
            if (!parameter) return
            changed =
              mergeMask(symbolTaint, bindingSymbol(indexes.checker, parameter.name), expressionTaint(argument, state)) || changed
          })
        }
      })
    }
  }
  return state
}

function identifierRole(identifier, checker, ownerBySymbol) {
  const symbol = canonicalSymbol(checker, identifier)
  const called = symbol ? ownerBySymbol.get(symbol) : undefined
  if (called) return `function:${called.id}`
  const declaration = symbol?.valueDeclaration ?? symbol?.declarations?.[0]
  if (declaration && ts.isParameter(declaration)) {
    const index = declaration.parent.parameters.indexOf(declaration)
    return `parameter:${index}:${checker.typeToString(checker.getTypeAtLocation(declaration), undefined, ts.TypeFormatFlags.NoTruncation)}`
  }
  if (declaration && ts.isVariableDeclaration(declaration)) {
    return `local:${checker.typeToString(checker.getTypeAtLocation(declaration), undefined, ts.TypeFormatFlags.NoTruncation)}`
  }
  if (symbol) {
    const qualified = checker.getFullyQualifiedName(symbol).replace(/"[^"]+"\./g, '')
    return `symbol:${qualified}`
  }
  return `identifier:${identifier.text}`
}

function normalizeAst(node, checker, ownerBySymbol) {
  if (ts.isIdentifier(node)) {
    if (
      (ts.isPropertyAccessExpression(node.parent) && node.parent.name === node) ||
      ((ts.isPropertyAssignment(node.parent) || ts.isMethodDeclaration(node.parent)) && node.parent.name === node)
    ) {
      return ['property', node.text]
    }
    return [ts.SyntaxKind[node.kind], identifierRole(node, checker, ownerBySymbol)]
  }
  if (ts.isStringLiteralLike(node) || ts.isNumericLiteral(node) || ts.isRegularExpressionLiteral(node)) {
    return [ts.SyntaxKind[node.kind], node.text]
  }
  if (node.kind === ts.SyntaxKind.TrueKeyword || node.kind === ts.SyntaxKind.FalseKeyword || node.kind === ts.SyntaxKind.NullKeyword) {
    return [ts.SyntaxKind[node.kind]]
  }
  const children = []
  ts.forEachChild(node, (child) => {
    children.push(normalizeAst(child, checker, ownerBySymbol))
  })
  return [ts.SyntaxKind[node.kind], children]
}

function normalizeSignature(node, checker, ownerBySymbol) {
  return JSON.stringify([
    declarationName(node),
    node.parameters.map((parameter) => checker.typeToString(checker.getTypeAtLocation(parameter), undefined, ts.TypeFormatFlags.NoTruncation)),
    node.type ? node.type.getText() : undefined,
    ownerBySymbol ? normalizeAst(node.name ?? node, checker, ownerBySymbol) : undefined,
  ])
}

function operationFingerprint(node, indexes) {
  return sha256(JSON.stringify(normalizeAst(node, indexes.checker, indexes.ownerBySymbol)))
}

function operationContainer(node, owner) {
  for (let current = node; current && current !== owner.node; current = current.parent) {
    if (
      ts.isCallExpression(current) ||
      ts.isNewExpression(current) ||
      ts.isBinaryExpression(current) ||
      ts.isReturnStatement(current) ||
      ts.isPropertyAssignment(current) ||
      ts.isVariableDeclaration(current)
    ) {
      return current
    }
  }
  return node
}

function isAssignmentTarget(node) {
  return ts.isBinaryExpression(node.parent) && node.parent.left === node && node.parent.operatorToken.kind === ts.SyntaxKind.EqualsToken
}

function propertyAccessIsRenderedSource(node, checker) {
  return node.name.text === 'source' && typeIsRenderedSourceCarrier(checker.getTypeAtLocation(node.expression), checker)
}

function textOperationCategory(call) {
  if (!ts.isPropertyAccessExpression(call.expression)) return undefined
  const method = call.expression.name.text
  if (method === 'replace' || method === 'replaceAll') return 'text-rewrite'
  if (textMethodNames.has(method)) return 'text-semantic-extraction'
  return undefined
}

function objectSynthesizesGeneratedSource(node) {
  if (!ts.isObjectLiteralExpression(node)) return false
  const names = new Set(
    node.properties.flatMap((property) => {
      if (!('name' in property) || !property.name) return []
      if (ts.isIdentifier(property.name) || ts.isStringLiteral(property.name)) return [property.name.text]
      return []
    }),
  )
  return names.has('fileName') && names.has('source')
}

function aggregateRows(rows) {
  const byCategory = {}
  const byFile = {}
  const byReachability = {}
  for (const row of rows) {
    byCategory[row.category] = (byCategory[row.category] ?? 0) + 1
    byFile[row.file] = (byFile[row.file] ?? 0) + 1
    byReachability[row.reachability] = (byReachability[row.reachability] ?? 0) + 1
  }
  return { byCategory: sortObject(byCategory), byFile: sortObject(byFile), byReachability: sortObject(byReachability) }
}

function sortObject(value) {
  return Object.fromEntries(Object.entries(value).sort(([left], [right]) => left.localeCompare(right)))
}

function collectInventory(program) {
  const indexes = collectOwners(program)
  const graph = buildCallGraph(indexes)
  const hookOwners = indexes.owners.filter((owner) => owner.name === 'transformGeneratedSources')
  const activeClosure = transitiveClosure(hookOwners.map((owner) => owner.id), graph)
  const exportedTextRoots = indexes.owners.filter((owner) => sourceRewriterExportCandidate(owner, indexes))
  const dormantExportRoots = exportedTextRoots.filter((owner) => !activeClosure.has(owner.id))
  const dormantClosure = transitiveClosure(dormantExportRoots.map((owner) => owner.id), graph)
  const relevantOwners = new Set([...activeClosure, ...dormantClosure])
  const selfRenderReparseClosure = transitiveClosure(
    indexes.owners
      .filter((owner) => selfRenderedParameterIndexes.has(owner.name) || owner.name === 'splitModuleFirstMountedRenderers')
      .map((owner) => owner.id),
    graph,
  )
  const taintState = analyzeTextTaint(indexes, relevantOwners, hookOwners, dormantExportRoots)
  const rows = []

  const addRow = (owner, category, node, detail) => {
    const reachability = activeClosure.has(owner.id) ? 'active-hook' : dormantClosure.has(owner.id) ? 'dormant-export' : 'unconnected'
    rows.push({
      file: owner.file,
      owner: owner.id,
      category,
      reachability,
      fingerprint: operationFingerprint(node, indexes),
      detail,
    })
  }

  for (const hook of hookOwners) addRow(hook, 'terminal-hook', hook.node.name ?? hook.node, 'transformGeneratedSources implementation')
  for (const root of dormantExportRoots) addRow(root, 'dormant-export-root', root.node.name ?? root.node, 'exported rendered-text root outside active hook closure')

  for (const owner of indexes.owners) {
    if (!relevantOwners.has(owner.id)) continue
    visitOwnerBody(owner, indexes, (node) => {
      if (ts.isPropertyAccessExpression(node) && propertyAccessIsRenderedSource(node, indexes.checker)) {
        addRow(
          owner,
          isAssignmentTarget(node) ? 'rendered-source-write' : 'rendered-source-read',
          operationContainer(node, owner),
          isAssignmentTarget(node) ? 'write to rendered source carrier' : 'read from rendered source carrier',
        )
      }
      if (ts.isCallExpression(node)) {
        const category = textOperationCategory(node)
        const taint = expressionTaint(node, taintState)
        if (category && (taint & renderedTextTaint) !== 0) {
          addRow(owner, category, node, `${node.expression.name.text} on source-like text path`)
        }
        if (
          category &&
          selfRenderReparseFiles.has(owner.file) &&
          ((taint & selfRenderedTextTaint) !== 0 || selfRenderReparseClosure.has(owner.id))
        ) {
          addRow(owner, 'self-render-reparse', node, 'plugin-rendered text is parsed after rendering')
        }
      }
      if (objectSynthesizesGeneratedSource(node)) {
        const sourceProperty = node.properties.find(
          (property) =>
            ts.isPropertyAssignment(property) &&
            (ts.isIdentifier(property.name) || ts.isStringLiteral(property.name)) &&
            property.name.text === 'source',
        )
        if (
          sourceProperty &&
          ts.isPropertyAssignment(sourceProperty) &&
          (expressionTaint(sourceProperty.initializer, taintState) & (renderedTextTaint | selfRenderedTextTaint)) !== 0
        ) {
          addRow(owner, 'post-render-source-synthesis', node, 'construct generated { fileName, source } artifact after rendering')
        }
      }
    })
  }

  rows.sort((left, right) => {
    const leftKey = [left.file, left.owner, left.category, left.reachability, left.fingerprint].join('\0')
    const rightKey = [right.file, right.owner, right.category, right.reachability, right.fingerprint].join('\0')
    return leftKey.localeCompare(rightKey)
  })
  const duplicateCounts = new Map()
  const canonicalRows = rows.map((row) => {
    const base = [row.file, row.owner, row.category, row.reachability, row.fingerprint].join('|')
    const duplicate = (duplicateCounts.get(base) ?? 0) + 1
    duplicateCounts.set(base, duplicate)
    return Object.freeze({ ...row, duplicate, key: `${base}|${duplicate}` })
  })

  const ownerByName = new Map()
  for (const owner of indexes.owners) {
    const values = ownerByName.get(owner.name) ?? []
    values.push(owner)
    ownerByName.set(owner.name, values)
  }
  const namedActiveRoots = [...auditedActiveRootNames].filter((name) => (ownerByName.get(name) ?? []).some((owner) => activeClosure.has(owner.id)))
  const presentDormantInventoryRoots = [...auditedDormantRootNames]
    .flatMap((name) => ownerByName.get(name) ?? [])
    .filter((owner) => !activeClosure.has(owner.id))
  const presentDormantRootClosure = transitiveClosure(presentDormantInventoryRoots.map((owner) => owner.id), graph)
  const activeFromAuditedRoots = transitiveClosure(
    [...auditedActiveRootNames].flatMap((name) => (ownerByName.get(name) ?? []).map((owner) => owner.id)),
    graph,
  )
  const dormantOnlyFunctions = [...presentDormantRootClosure].filter((id) => !activeFromAuditedRoots.has(id))
  const sourceRewriterExportedRoots = indexes.owners.filter(
    (owner) => owner.exported && owner.file === 'src/cpp-source-replacements.ts' && ownerHasSourceLikeParameter(owner, indexes.checker),
  )
  const renderedSourceReads = canonicalRows.filter((row) => row.category === 'rendered-source-read').length
  const metrics = {
    terminalHooks: hookOwners.length,
    renderedSourceReads,
    namedActiveRoots: namedActiveRoots.length,
    sourceRewriterExportedRoots: sourceRewriterExportedRoots.length,
    dormantInventoryRoots: presentDormantInventoryRoots.length,
    dormantInventoryClosureFunctions: presentDormantRootClosure.size,
    dormantOnlyFunctions: dormantOnlyFunctions.length,
    activeClosureFunctions: activeClosure.size,
    discoveredDormantExportRoots: dormantExportRoots.length,
  }
  const roots = {
    activeHooks: hookOwners.map((owner) => owner.id).sort(),
    activeExportedTextRoots: exportedTextRoots.filter((owner) => activeClosure.has(owner.id)).map((owner) => owner.id).sort(),
    dormantExportedTextRoots: dormantExportRoots.map((owner) => owner.id).sort(),
    auditedActiveRootsPresent: namedActiveRoots.sort(),
    auditedDormantRootsPresent: presentDormantInventoryRoots.map((owner) => owner.id).sort(),
  }
  return { rows: canonicalRows, metrics, roots, aggregates: aggregateRows(canonicalRows) }
}

function baselineFromInventory(inventory) {
  return {
    schemaVersion,
    scannerVersion,
    policy: 'current rows must be a subset; removals are accepted; additions require architectural removal instead of baseline growth',
    hardCeilings,
    rows: inventory.rows,
    roots: inventory.roots,
    aggregates: inventory.aggregates,
  }
}

function compareWithBaseline(inventory, baseline) {
  if (baseline.schemaVersion !== schemaVersion) {
    throw new Error(`Unsupported baseline schema ${baseline.schemaVersion}; expected ${schemaVersion}`)
  }
  const baselineKeys = new Set(baseline.rows.map((row) => row.key))
  const currentKeys = new Set(inventory.rows.map((row) => row.key))
  const additions = inventory.rows.filter((row) => !baselineKeys.has(row.key))
  const removals = baseline.rows.filter((row) => !currentKeys.has(row.key))
  const rootAdditions = {}
  for (const key of ['activeHooks', 'activeExportedTextRoots', 'dormantExportedTextRoots']) {
    const allowed = new Set(baseline.roots?.[key] ?? [])
    const added = (inventory.roots[key] ?? []).filter((root) => !allowed.has(root))
    if (added.length > 0) rootAdditions[key] = added
  }
  const ceilingViolations = Object.entries(hardCeilings).flatMap(([metric, ceiling]) =>
    inventory.metrics[metric] > ceiling ? [{ metric, current: inventory.metrics[metric], ceiling }] : [],
  )
  return { additions, removals, rootAdditions, ceilingViolations }
}

function writeJson(fileName, value) {
  fs.mkdirSync(path.dirname(fileName), { recursive: true })
  fs.writeFileSync(fileName, canonicalJson(value))
}

function fileStats(fileName) {
  const bytes = fs.readFileSync(fileName)
  return {
    path: fileName,
    lines: bytes.length === 0 ? 0 : bytes.toString('utf8').split('\n').length - 1,
    bytes: bytes.length,
    sha256: sha256(bytes),
  }
}

function main() {
  const options = parseArguments(process.argv.slice(2))
  const inventory = collectInventory(readProgram())
  if (options.initializeBaseline) {
    if (fs.existsSync(options.baselinePath)) throw new Error(`Baseline already exists: ${options.baselinePath}`)
    writeJson(options.baselinePath, baselineFromInventory(inventory))
  }
  if (!fs.existsSync(options.baselinePath)) {
    throw new Error(`Missing baseline ${options.baselinePath}; use --initialize-baseline exactly once`) 
  }
  const baseline = JSON.parse(fs.readFileSync(options.baselinePath, 'utf8'))
  const comparison = compareWithBaseline(inventory, baseline)
  const failed =
    comparison.additions.length > 0 ||
    Object.keys(comparison.rootAdditions).length > 0 ||
    comparison.ceilingViolations.length > 0
  const report = {
    schemaVersion,
    scannerVersion,
    status: failed ? 'fail' : 'pass',
    package: '@geastack/geatsc-plugin-gea',
    sourceRoot: 'src',
    baseline: normalizedPath(options.baselinePath),
    metrics: inventory.metrics,
    hardCeilings,
    roots: inventory.roots,
    aggregates: inventory.aggregates,
    comparison,
    rows: inventory.rows,
  }
  writeJson(options.outputPath, report)

  if (options.pruneBaseline) {
    if (failed) throw new Error('Refusing to prune: current inventory contains additions or ceiling violations')
    writeJson(options.baselinePath, baselineFromInventory(inventory))
  }

  const stats = fileStats(options.outputPath)
  process.stdout.write(
    [
      `Gea rendered-C++ architecture ratchet: ${report.status.toUpperCase()}`,
      `rows=${inventory.rows.length} additions=${comparison.additions.length} removals=${comparison.removals.length}`,
      `active-hook-functions=${inventory.metrics.activeClosureFunctions} dormant-export-roots=${inventory.metrics.discoveredDormantExportRoots}`,
      `artifact=${stats.path}`,
      `artifact-lines=${stats.lines} artifact-bytes=${stats.bytes} artifact-sha256=${stats.sha256}`,
      '',
    ].join('\n'),
  )
  if (failed) process.exitCode = 1
}

try {
  main()
} catch (error) {
  process.stderr.write(`${error instanceof Error ? error.stack ?? error.message : String(error)}\n`)
  process.exitCode = 1
}
