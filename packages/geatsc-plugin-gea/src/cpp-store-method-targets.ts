import type { GeaIrStoreExpr, GeaIrStoreMethod } from './types.js'
import { sanitizeCppIdentifier } from './utils.js'

export function thisFieldName(expr: GeaIrStoreExpr): string | null {
  return expr.kind === 'member' && expr.object.kind === 'this' ? expr.property : null
}

export function thisArrayLengthTarget(expr: GeaIrStoreExpr): string | null {
  return expr.kind === 'member' && expr.property === 'length' ? thisFieldName(expr.object) : null
}

export function thisArrayItemFieldTarget(expr: GeaIrStoreExpr): { arrayField: string; index: GeaIrStoreExpr; itemField: string } | null {
  if (expr.kind !== 'member' || expr.object.kind !== 'index') return null
  const arrayField = thisFieldName(expr.object.object)
  return arrayField ? { arrayField, index: expr.object.index, itemField: expr.property } : null
}

export function thisArrayPushTarget(expr: GeaIrStoreExpr): string | null {
  return expr.kind === 'member' && expr.property === 'push' ? thisFieldName(expr.object) : null
}

export function thisArrayUnshiftTarget(expr: GeaIrStoreExpr): string | null {
  return expr.kind === 'member' && expr.property === 'unshift' ? thisFieldName(expr.object) : null
}

export function thisArraySpliceTarget(expr: GeaIrStoreExpr): string | null {
  return expr.kind === 'member' && expr.property === 'splice' ? thisFieldName(expr.object) : null
}

export function thisArrayPopTarget(expr: GeaIrStoreExpr): string | null {
  return expr.kind === 'member' && expr.property === 'pop' ? thisFieldName(expr.object) : null
}

export function thisArrayShiftTarget(expr: GeaIrStoreExpr): string | null {
  return expr.kind === 'member' && expr.property === 'shift' ? thisFieldName(expr.object) : null
}

export function exprPath(expr: GeaIrStoreExpr): string | null {
  if (expr.kind === 'identifier') return expr.name
  if (expr.kind === 'member') {
    const object = exprPath(expr.object)
    return object ? `${object}.${expr.property}` : null
  }
  return null
}

export function methodPrefix(method: GeaIrStoreMethod): string {
  return sanitizeCppIdentifier(method.name)
}

export function replaceClassMethodBody(source: string, className: string, methodName: string, body: string[]): string {
  return replaceClassMethodDefinition(source, className, methodName, null, body)
}

export function readClassMethodBody(source: string, className: string, methodName: string): string | null {
  const region = findMethodSignatureRegion(source, className, methodName)
  return region ? source.slice(region.methodOpen + 1, region.methodClose) : null
}

// Replace a store method's PARAMETER LIST (and body) while preserving geatsc's
// original return type and `virtual`/`const` qualifiers. Used to give a typed
// store method native primitive parameters (`double timestampMs`) without
// touching its return type — geatsc already declared the correct one, and
// rewriting it risks demoting value-returning methods to `void`. Falls back to
// a body-only replacement if the original parameter list can't be located.
export function replaceClassMethodParameters(
  source: string,
  className: string,
  methodName: string,
  typedParams: string,
  body: string[],
): string {
  const region = findMethodSignatureRegion(source, className, methodName)
  if (!region) return source
  const { methodStart, methodOpen, methodClose } = region
  const signatureText = source.slice(methodStart, methodOpen)
  // A deferred method's declaration lives in the generated header while this
  // transform is currently visiting only its out-of-line .cpp definition.
  // Rewriting that definition from geatsc's abbreviated-template parameters
  // to concrete primitive parameters would leave the header declaring a
  // different function. Preserve the original out-of-line signature and only
  // replace its body; inline definitions can safely receive typed parameters.
  //
  // The preserved template still needs the CONCRETE instantiation: geatsc's
  // closed-world planner treats this store method as generic (the plugin
  // strips its annotations pre-compile) and only plans the boxed
  // gea_cpp_value runtime-bridge variant, while native call sites deduce the
  // primitive types this transform just lowered the body for (e.g.
  // BallStore::tick<double> from a rAF callback's double timestamp). Emit
  // that explicit instantiation next to the definition so those call sites
  // link.
  if (signatureText.includes(`${className}::${methodName}`)) {
    const replaced = `${source.slice(0, methodOpen)}${body.join('\n')}`
    const rest = source.slice(methodClose + 1)
    const instantiation = outOfLineConcreteInstantiation(signatureText, methodName, typedParams)
    if (instantiation && !source.includes(instantiation)) {
      return `${replaced}\n${instantiation}${rest}`
    }
    return `${replaced}${rest}`
  }
  // The original signature is `<prefix> <methodName> ( <params> ) <suffix>`.
  // Swap only the parameter list inside the first balanced `(...)` after the
  // method name; keep `<prefix>` (return type + `virtual`) and `<suffix>`
  // (`const`, ref-qualifiers) verbatim.
  const nameIndex = signatureText.lastIndexOf(methodName)
  const parenOpen = nameIndex >= 0 ? signatureText.indexOf('(', nameIndex) : -1
  if (parenOpen < 0) return source
  const parenClose = matchParen(signatureText, parenOpen)
  if (parenClose < 0) return `${source.slice(0, methodOpen)}${body.join('\n')}${source.slice(methodClose + 1)}`
  const rebuiltSignature = `${signatureText.slice(0, parenOpen + 1)}${typedParams}${signatureText.slice(parenClose)}`
  return `${source.slice(0, methodStart)}${rebuiltSignature}${body.join('\n')}${source.slice(methodClose + 1)}`
}

// Build `template <ret> <Class>::<method>(<concrete params>) <quals>;` for a
// preserved out-of-line abbreviated-template definition by substituting each
// parameter's `auto` with the concrete type from typedParams (positional, per
// parameter — a parameter geatsc already typed concretely is left untouched).
// Returns null when the shapes don't line up; the caller then changes nothing.
function outOfLineConcreteInstantiation(signatureText: string, methodName: string, typedParams: string): string | null {
  const nameIndex = signatureText.lastIndexOf(methodName)
  const parenOpen = nameIndex >= 0 ? signatureText.indexOf('(', nameIndex) : -1
  if (parenOpen < 0) return null
  const parenClose = matchParen(signatureText, parenOpen)
  if (parenClose < 0) return null
  const originalParams = splitTopLevelParams(signatureText.slice(parenOpen + 1, parenClose))
  // Only abbreviated-template definitions (at least one `auto` parameter) are
  // templates that can — and must — be explicitly instantiated. geatsc emits
  // fully-concrete store methods (`double savedPage(std::string id)`) as plain
  // non-template definitions; prefixing `template ...;` to one is an explicit
  // instantiation of a non-template ("template-id does not match any template
  // declaration"). No `auto` param ⇒ nothing to instantiate.
  if (!originalParams.some((param) => /\bauto\b/.test(param))) return null
  const concreteTypes = splitTopLevelParams(typedParams).map((param) =>
    param.replace(/\s+[A-Za-z_][A-Za-z0-9_]*$/, '').trim(),
  )
  if (originalParams.length !== concreteTypes.length) return null
  const concreteParams: string[] = []
  for (let index = 0; index < originalParams.length; index += 1) {
    const param = originalParams[index]
    if (!/\bauto\b/.test(param)) {
      concreteParams.push(param.trim())
      continue
    }
    if (!concreteTypes[index]) return null
    concreteParams.push(param.replace(/\bauto\b/, concreteTypes[index]).trim())
  }
  const signature =
    `${signatureText.slice(0, parenOpen + 1)}${concreteParams.join(', ')}${signatureText.slice(parenClose)}`.trim()
  if (/\bauto\b/.test(signature)) return null
  return `template ${signature};`
}

// Split a C++ parameter list on top-level commas (template arguments keep
// their commas).
function splitTopLevelParams(params: string): string[] {
  const trimmed = params.trim()
  if (!trimmed) return []
  const parts: string[] = []
  let depth = 0
  let start = 0
  for (let index = 0; index < trimmed.length; index += 1) {
    const char = trimmed[index]
    if (char === '<' || char === '(' || char === '[') depth += 1
    else if (char === '>' || char === ')' || char === ']') depth -= 1
    else if (char === ',' && depth === 0) {
      parts.push(trimmed.slice(start, index))
      start = index + 1
    }
  }
  parts.push(trimmed.slice(start))
  return parts
}

export function replaceClassMethodDefinition(
  source: string,
  className: string,
  methodName: string,
  signature: string | null,
  body: string[],
): string {
  const region = findMethodSignatureRegion(source, className, methodName)
  if (!region) return source
  const { methodStart, methodOpen, methodClose } = region
  if (signature) {
    return `${source.slice(0, methodStart)}${signature}\n${body.join('\n')}${source.slice(methodClose + 1)}`
  }
  return `${source.slice(0, methodOpen)}${body.join('\n')}${source.slice(methodClose + 1)}`
}

function findMethodSignatureRegion(
  source: string,
  className: string,
  methodName: string,
): { methodStart: number; methodOpen: number; methodClose: number } | null {
  const classOpen = findClassOpenBrace(source, className)
  if (classOpen >= 0) {
    const classClose = findMatchingBrace(source, classOpen)
    if (classClose >= 0) {
      const inline = findInlineMethodRegion(source, methodName, classOpen, classClose)
      if (inline) return inline
    }
  }

  // geatsc moves sufficiently large method bodies into the module .cpp and
  // leaves only `method(...);` in the class header. Store re-lowering runs on
  // every generated source, so find that out-of-line definition as well.
  const outOfLinePattern = new RegExp(
    `(?:^|\\n)\\s*[A-Za-z_][A-Za-z0-9_:<>,*&\\s]*\\b${escapeRegExp(className)}::${escapeRegExp(methodName)}\\s*\\(`,
    'g',
  )
  let match: RegExpExecArray | null
  while ((match = outOfLinePattern.exec(source)) !== null) {
    const methodStart = match.index + (match[0].startsWith('\n') ? 1 : 0)
    const region = definitionRegionAt(source, methodStart, methodName, source.length)
    if (region) return region
  }
  return null
}

// Index of the `)` matching the `(` at `open` within `text` (no nesting beyond
// the parameter list's own default-value parens, which it tracks by depth).
function matchParen(text: string, open: number): number {
  let depth = 0
  let quote: string | null = null
  let escaped = false
  for (let index = open; index < text.length; index += 1) {
    const char = text[index]
    if (quote) {
      if (escaped) escaped = false
      else if (char === '\\') escaped = true
      else if (char === quote) quote = null
    } else if (char === '"' || char === "'") quote = char
    else if (char === '(') depth += 1
    else if (char === ')' && --depth === 0) return index
  }
  return -1
}

function findInlineMethodRegion(
  source: string,
  methodName: string,
  classOpen: number,
  classClose: number,
): { methodStart: number; methodOpen: number; methodClose: number } | null {
  const pattern = new RegExp(
    `\\n\\s*(?:virtual\\s+)?[A-Za-z_][A-Za-z0-9_:<>,*&\\s]*\\b${escapeRegExp(methodName)}\\s*\\(`,
    'g',
  )
  pattern.lastIndex = classOpen
  let match: RegExpExecArray | null
  while ((match = pattern.exec(source)) !== null) {
    if (match.index > classClose) break
    const methodStart = match.index + 1
    const region = definitionRegionAt(source, methodStart, methodName, classClose)
    if (region) return region
  }
  return null
}

function definitionRegionAt(
  source: string,
  methodStart: number,
  methodName: string,
  limit: number,
): { methodStart: number; methodOpen: number; methodClose: number } | null {
  const nameIndex = source.indexOf(methodName, methodStart)
  if (nameIndex < 0 || nameIndex >= limit) return null
  const parenOpen = source.indexOf('(', nameIndex + methodName.length)
  if (parenOpen < 0 || parenOpen >= limit) return null
  const parenClose = matchParen(source, parenOpen)
  if (parenClose < 0 || parenClose >= limit) return null

  // A declaration ends at `;`. Never let its search drift into the next
  // member's `{` — that used to replace a following getter with the deferred
  // method body.
  for (let index = parenClose + 1; index < limit; index += 1) {
    const char = source[index]
    if (char === ';') return null
    if (char !== '{') continue
    const methodClose = findMatchingBrace(source, index)
    if (methodClose < 0 || methodClose > limit) return null
    return { methodStart, methodOpen: index, methodClose }
  }
  return null
}

function findClassOpenBrace(source: string, className: string): number {
  const pattern = new RegExp(`\\bclass\\s+${escapeRegExp(className)}\\b`, 'g')
  let match: RegExpExecArray | null
  while ((match = pattern.exec(source)) !== null) {
    const brace = source.indexOf('{', match.index)
    const semicolon = source.indexOf(';', match.index)
    if (brace >= 0 && (semicolon < 0 || brace < semicolon)) return brace
  }
  return -1
}

function findMatchingBrace(source: string, open: number): number {
  let depth = 0
  let quote: string | null = null
  let escaped = false
  for (let index = open; index < source.length; index += 1) {
    const char = source[index]
    if (quote) {
      if (escaped) escaped = false
      else if (char === '\\') escaped = true
      else if (char === quote) quote = null
    } else if (char === '"' || char === "'") quote = char
    else if (char === '{') depth += 1
    else if (char === '}' && --depth === 0) return index
  }
  return -1
}

function escapeRegExp(text: string): string {
  return text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
}
