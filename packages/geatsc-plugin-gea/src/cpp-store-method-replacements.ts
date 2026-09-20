import {
  arrayElementAliasFieldTarget,
  arrayItemAliasKey,
  arrayItemFieldAliasKey,
  arrayItemFieldHint,
  constantMap,
  fieldHint,
  localArrayCppType,
  nextStoreMethodTemp,
  typedArrayItemFieldAccess,
  type StoreMethodHint,
  type StoreMethodLowerContext
} from './cpp-store-method-context.js'
import { parseStoreMethodBody } from './cpp-store-method-body-parser.js'
import { assignmentHint, globalStoreFieldTarget, lowerExpr, lowerForUpdate } from './cpp-store-method-expressions.js'
import {
  exprPath,
  methodPrefix,
  readClassMethodBody,
  replaceClassMethodBody,
  replaceClassMethodParameters,
  thisArrayItemFieldTarget,
  thisArrayLengthTarget,
  thisArrayPushTarget,
  thisFieldName
} from './cpp-store-method-targets.js'
import { collectStoreFields, collectStoreMethods, type StoreFieldPlan, type StoreMethodPlan } from './cpp-stores.js'
import type { GeaIrBundleV1, GeaIrConstant, GeaIrStore, GeaIrStoreExpr, GeaIrStoreMethod, GeaIrStoreStmt } from './types.js'
import { sanitizeCppIdentifier } from './utils.js'
import { storeDynamicFallbackEnabled, storeHubEmissionEnabled } from './cpp-store-hub.js'

export function replaceStoreMethodsFromIr(
  source: string,
  ir: GeaIrBundleV1,
  constants: ReadonlyMap<string, GeaIrConstant> = new Map(),
  topLevelValueNames: ReadonlyMap<string, string> = new Map(),
): string {
  let next = source
  const storeFields = collectStoreFields(ir.stores)
  const storeMethods = collectStoreMethods(ir.stores)
  for (const store of ir.stores) {
    // A self-store's class is the geatsc-compiled typed ReactiveComponent: its
    // method bodies are already correct (typed Signal writes notify subscribers).
    // This re-lowering targets the dynamic CompiledStore dirty-field protocol and
    // would corrupt the base-less typed class — skip.
    if (store.selfStore) continue
    for (const method of store.methods ?? []) {
      const className = sanitizeCppIdentifier(store.className)
      const methodName = methodPrefix(method)
      const topLevelFunctionNames = topLevelFunctionNameOverrides(next, className, methodName, method)
      const methodTopLevelValueNames = mergeMaps(topLevelValueNames, topLevelValueNameOverrides(next, className, methodName))
      const body = lowerStoreMethod(store, method, storeFields, storeMethods, constants, topLevelFunctionNames, methodTopLevelValueNames)
      if (body) {
        const typedParams = typedStoreMethodParams(method)
        next = typedParams
          ? replaceClassMethodParameters(next, className, methodName, typedParams, body)
          : replaceClassMethodBody(next, className, methodName, body)
      }
    }
  }
  return next
}

export function topLevelValueAccessorNamesFromSource(source: string): Map<string, string> {
  const accessors = new Map<string, string>()
  const pattern = /\b__gea_global_([A-Za-z_][A-Za-z0-9_]*)\s*\(\s*\)/g
  let match: RegExpExecArray | null
  while ((match = pattern.exec(source)) !== null) {
    const accessor = `__gea_global_${match[1]}()`
    accessors.set(match[1], accessor)
  }
  return accessors
}

function mergeMaps<K, V>(...maps: readonly ReadonlyMap<K, V>[]): Map<K, V> {
  const merged = new Map<K, V>()
  for (const map of maps) {
    for (const [key, value] of map) merged.set(key, value)
  }
  return merged
}

function topLevelValueNameOverrides(source: string, className: string, methodName: string): Map<string, string> {
  const originalBody = readClassMethodBody(source, className, methodName)
  if (!originalBody) return new Map()
  return topLevelValueAccessorNamesFromSource(originalBody)
}

function topLevelFunctionNameOverrides(source: string, className: string, methodName: string, method: GeaIrStoreMethod): Map<string, string> {
  const originalBody = readClassMethodBody(source, className, methodName)
  if (!originalBody) return new Map()
  const ops = method.ops ?? parseStoreMethodBody(method.body)
  if (!ops) return new Map()
  const names = topLevelFunctionCallNames(ops)
  const overrides = new Map<string, string>()
  for (const name of names) {
    const pattern = generatedFunctionNamePattern(name)
    const matches = new Set<string>()
    let match: RegExpExecArray | null
    while ((match = pattern.exec(originalBody)) !== null) matches.add(match[1])
    if (matches.size === 1) overrides.set(name, [...matches][0])
  }
  return overrides
}

function topLevelFunctionCallNames(statements: GeaIrStoreStmt[]): Set<string> {
  const names = new Set<string>()
  const visitStatement = (statement: GeaIrStoreStmt): void => {
    if (statement.kind === 'var') {
      if (statement.init) visitExpr(statement.init)
    } else if (statement.kind === 'assign') {
      visitExpr(statement.target)
      visitExpr(statement.value)
    } else if (statement.kind === 'expr') {
      visitExpr(statement.expr)
    } else if (statement.kind === 'if') {
      visitExpr(statement.test)
      statement.consequent.forEach(visitStatement)
      statement.alternate?.forEach(visitStatement)
    } else if (statement.kind === 'for') {
      if (statement.init) visitStatement(statement.init)
      if (statement.test) visitExpr(statement.test)
      if (statement.update) visitExpr(statement.update)
      statement.body.forEach(visitStatement)
    } else if (statement.kind === 'return') {
      if (statement.value) visitExpr(statement.value)
    }
  }
  const visitExpr = (expr: GeaIrStoreExpr): void => {
    if (expr.kind === 'call') {
      if (expr.callee.kind === 'identifier') names.add(expr.callee.name)
      visitExpr(expr.callee)
      expr.args.forEach(visitExpr)
    } else if (expr.kind === 'member') {
      visitExpr(expr.object)
    } else if (expr.kind === 'index') {
      visitExpr(expr.object)
      visitExpr(expr.index)
    } else if (expr.kind === 'object') {
      expr.fields.forEach((field) => visitExpr(field.value))
    } else if (expr.kind === 'array') {
      expr.elements.forEach(visitExpr)
    } else if (expr.kind === 'unary' || expr.kind === 'update') {
      visitExpr(expr.arg)
    } else if (expr.kind === 'binary' || expr.kind === 'logical') {
      visitExpr(expr.left)
      visitExpr(expr.right)
    } else if (expr.kind === 'conditional') {
      visitExpr(expr.test)
      visitExpr(expr.consequent)
      visitExpr(expr.alternate)
    }
  }
  statements.forEach(visitStatement)
  return names
}

function escapeRegExp(text: string): string {
  return text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
}

function generatedFunctionNamePattern(name: string): RegExp {
  const safeName = escapeRegExp(sanitizeCppIdentifier(name))
  const legacyName = `fn_${safeName}`
  const moduleQualifiedName = `fn___gea_mod_[A-Za-z0-9_]+_x2e_${safeName}`
  const collisionSuffix = '(?:_x24_\\d+|_x[0-9a-fA-F]+_[A-Za-z0-9_]+)?'
  return new RegExp(`\\b((?:${legacyName}|${moduleQualifiedName})${collisionSuffix})\\b(?:\\s*\\()?`, 'g')
}

// The typed C++ parameter list for a store method whose every parameter has a
// known primitive value type (e.g. `tick(timestampMs: number)` →
// `double timestampMs`). Returns null when the method has no params or any
// param lacks a value type — those keep geatsc's original `gea_cpp_value`
// signature (only the body is replaced). Crucially this rewrites ONLY the
// parameter list; the method's RETURN TYPE (and `virtual`/`const` qualifiers)
// is preserved from geatsc's original declaration. An earlier version
// hardcoded `virtual void …`, which silently demoted value-returning methods
// (e.g. `tick(): boolean`) to `void` and broke their callers
// (`if (this->tick(...))`).
function typedStoreMethodParams(method: GeaIrStoreMethod): string | null {
  if (method.params.length === 0) return null
  if (method.params.some((param) => !param.valueType)) return null
  return method.params
    .map((param) => `${cppParamType(param.valueType as 'number' | 'string' | 'boolean')} ${sanitizeCppIdentifier(param.name)}`)
    .join(', ')
}

function cppParamType(valueType: 'number' | 'string' | 'boolean'): string {
  if (valueType === 'number') return 'double'
  if (valueType === 'boolean') return 'bool'
  return 'std::string'
}

// `this.<arrayField>[<index>]` shape, or null.
function thisArrayIndexInit(expr: GeaIrStoreExpr | undefined): { arrayField: string; index: GeaIrStoreExpr } | null {
  if (!expr || expr.kind !== 'index') return null
  const obj = expr.object
  if (obj.kind !== 'member' || obj.object.kind !== 'this') return null
  return { arrayField: obj.property, index: expr.index }
}

// Whether `name` is the target of an assignment / ++ / -- anywhere in stmts.
// Structural mutations of `this.<arrayField>`: whole-element assignment,
// `.length` assignment, or a mutating array method call. Item-FIELD writes
// (`this.arr[i].prop = v`) are not structural — element positions are stable.
function arrayStructurallyMutated(stmts: GeaIrStoreStmt[], arrayField: string): boolean {
  const isThisArray = (e: GeaIrStoreExpr): boolean =>
    e.kind === 'member' && e.object.kind === 'this' && e.property === arrayField
  const mutatingCall = (e: GeaIrStoreExpr): boolean => {
    if (e.kind !== 'call' || e.callee.kind !== 'member' || !isThisArray(e.callee.object)) return false
    return ['push', 'pop', 'shift', 'unshift', 'splice', 'sort', 'reverse'].includes(e.callee.property)
  }
  const inExpr = (e: GeaIrStoreExpr): boolean => {
    if (mutatingCall(e)) return true
    switch (e.kind) {
      case 'member':
        return inExpr(e.object)
      case 'index':
        return inExpr(e.object) || inExpr(e.index)
      case 'call':
        return inExpr(e.callee) || e.args.some(inExpr)
      case 'object':
        return e.fields.some((f) => inExpr(f.value))
      case 'array':
        return e.elements.some(inExpr)
      case 'unary':
      case 'update':
        return inExpr(e.arg)
      case 'binary':
      case 'logical':
        return inExpr(e.left) || inExpr(e.right)
      case 'conditional':
        return inExpr(e.test) || inExpr(e.consequent) || inExpr(e.alternate)
      default:
        return false
    }
  }
  const inStmt = (s: GeaIrStoreStmt): boolean => {
    switch (s.kind) {
      case 'var':
        return s.init ? inExpr(s.init) : false
      case 'assign': {
        // Whole-element write: this.arr[i] = …
        if (s.target.kind === 'index' && isThisArray(s.target.object)) return true
        // Length write: this.arr.length = …
        if (s.target.kind === 'member' && s.target.property === 'length' && isThisArray(s.target.object)) return true
        return inExpr(s.target) || inExpr(s.value)
      }
      case 'expr':
        return inExpr(s.expr)
      case 'return':
        return s.value ? inExpr(s.value) : false
      case 'if':
        return inExpr(s.test) || s.consequent.some(inStmt) || (s.alternate?.some(inStmt) ?? false)
      case 'for':
        return (
          (s.init ? inStmt(s.init) : false) ||
          (s.test ? inExpr(s.test) : false) ||
          (s.update ? inExpr(s.update) : false) ||
          s.body.some(inStmt)
        )
      default:
        return false
    }
  }
  return stmts.some(inStmt)
}

function lowerStoreMethod(
  store: GeaIrStore,
  method: GeaIrStoreMethod,
  storeFields: StoreFieldPlan[],
  storeMethods: StoreMethodPlan[],
  constants: ReadonlyMap<string, GeaIrConstant>,
  topLevelFunctionNames: Map<string, string> = new Map(),
  topLevelValueNames: Map<string, string> = new Map(),
): string[] | null {
  const rawOps = method.ops ?? parseStoreMethodBody(method.body)
  if (!rawOps) return null
  const ops = rawOps
  const context: StoreMethodLowerContext = {
    store,
    storeFields,
    storeMethods,
    params: new Set(method.params.map((param) => param.name)),
    paramHints: new Map(
      method.params
        .filter((param): param is typeof param & { valueType: 'string' | 'number' | 'boolean' } => !!param.valueType)
        .map((param) => [param.name, param.valueType])
    ),
    topLevelFunctionNames,
    topLevelValueNames,
    locals: new Set(),
    constants: constantMap(store, constants),
    localTypes: new Map(),
    tempIndex: 0,
    arrayElementAliases: new Map(),
    arrayItemAliases: new Map(),
    arrayItemFieldAliases: new Map(),
    dirtyArrayFields: new Set(),
    needsFieldSet: false,
    needsArrayLengthSet: false,
    needsArrayItemSet: false,
    needsDynamicArrayItemSet: false,
    needsArrayPush: false,
    needsArrayUnshift: false,
    needsArraySplice: false,
    needsArrayPop: false,
    needsArrayShift: false,
  }

  let body = lowerStatements(context, ops, 2)
  if (!body) return null
  if (needsMutationHelpers(context)) {
    // Mutation flags can be discovered AFTER an early `return` was lowered —
    // `flushBeforeReturn` only emits flush_dirty_fields()/storeDirectBatchEnd()
    // when the flags are already set, so a leading guard return would skip the
    // batch end and leak storeDirectBatchDepth for the rest of the session.
    // Re-lower with the discovered flags so every return carries the flush.
    body = lowerStatements(context, ops, 2)
    if (!body) return null
  }
  return ['{', ...mutationHelpers(context), ...body, ...mutationFooter(context), '  }']
}

function lowerStatements(context: StoreMethodLowerContext, statements: GeaIrStoreStmt[], indent: number): string[] | null {
  const lines: string[] = []
  for (let index = 0; index < statements.length; index += 1) {
    const statement = statements[index]
    const lowered = lowerStatement(context, statement, indent, statements.slice(index + 1))
    if (!lowered) return null
    lines.push(...lowered)
  }
  return lines
}

function lowerStatement(context: StoreMethodLowerContext, statement: GeaIrStoreStmt, indent: number, rest: GeaIrStoreStmt[] = []): string[] | null {
  const pad = ' '.repeat(indent)
  if (statement.kind === 'var') {
    context.locals.add(statement.name)
    if (statement.localType) context.localTypes.set(statement.name, statement.localType)
    context.arrayElementAliases.delete(statement.name)
    const name = sanitizeCppIdentifier(statement.name)
    if (!statement.mutable) {
      const aliasDecl = lowerArrayElementAliasDeclaration(context, statement, rest, pad, name)
      if (aliasDecl) return aliasDecl
    }
    if (!statement.mutable && statement.init) {
      const constexprLine = lowerConstLiteralMatrixDecl(name, statement.init, pad)
      if (constexprLine) return constexprLine
    }
    const localArrayType = localArrayCppType(context, statement.name)
    if (localArrayType && statement.init?.kind === 'array' && statement.init.elements.length === 0) {
      return [`${pad}${localArrayType} ${name}{};`]
    }
    if (localArrayType && !statement.init) {
      return [`${pad}${localArrayType} ${name}{};`]
    }
    const init = statement.init ? lowerExpr(context, statement.init, 'any') : 'gea_cpp_value::missing()'
    return init ? [`${pad}auto ${name} = ${init};`] : null
  }
  if (statement.kind === 'assign') {
    const assignment = lowerAssignment(context, statement.target, statement.value, true)
    return assignment ? [`${pad}${assignment}${assignment.endsWith('}') ? '' : ';'}`] : null
  }
  if (statement.kind === 'expr') {
    const expr = lowerExpr(context, statement.expr, 'any')
    return expr ? [`${pad}(void)(${expr});`] : null
  }
  if (statement.kind === 'return') {
    const expr = statement.value ? lowerExpr(context, statement.value, 'any') : ''
    return expr !== null ? [...flushBeforeReturn(context, pad), `${pad}return${expr ? ` ${expr}` : ''};`] : null
  }
  if (statement.kind === 'if') return lowerIfStatement(context, statement, indent)
  if (statement.kind === 'for') return lowerForStatement(context, statement, indent)
  return null
}

function lowerArrayElementAliasDeclaration(
  context: StoreMethodLowerContext,
  statement: Extract<GeaIrStoreStmt, { kind: 'var' }>,
  rest: GeaIrStoreStmt[],
  pad: string,
  name: string
): string[] | null {
  const aliasInit = thisArrayIndexInit(statement.init)
  if (!aliasInit) return null
  if (arrayStructurallyMutated(rest, aliasInit.arrayField)) return null
  const index = lowerExpr(context, aliasInit.index, 'number')
  if (!index) return null
  const safeArray = sanitizeCppIdentifier(aliasInit.arrayField)
  const array = `(*this).${safeArray}`
  const indexTemp = nextStoreMethodTemp(context, 'index')
  const markDirtyOnce = `mark_dirty_array_field_once_${safeArray}`
  context.needsArrayItemSet = true
  context.dirtyArrayFields.add(aliasInit.arrayField)
  context.arrayElementAliases.set(statement.name, {
    arrayField: aliasInit.arrayField,
    itemExpression: name,
    indexExpression: indexTemp,
  })
  return [
    `${pad}auto ${indexTemp} = static_cast<std::size_t>(${index});`,
    `${pad}if (${indexTemp} >= ${array}.size()) { ${array}.resize(${indexTemp} + 1); ${markDirtyOnce}(); }`,
    `${pad}auto &${name} = ${array}[${indexTemp}];`,
  ]
}

function lowerIfStatement(context: StoreMethodLowerContext, statement: Extract<GeaIrStoreStmt, { kind: 'if' }>, indent: number): string[] | null {
  const pad = ' '.repeat(indent)
  const test = lowerExpr(context, statement.test, 'boolean')
  const consequent = lowerStatements(context, statement.consequent, indent + 2)
  if (!test || !consequent) return null
  const lines = [`${pad}if (${test}) {`, ...consequent, `${pad}}`]
  if (statement.alternate?.length) {
    const alternate = lowerStatements(context, statement.alternate, indent + 2)
    if (!alternate) return null
    lines[lines.length - 1] = `${pad}} else {`
    lines.push(...alternate, `${pad}}`)
  }
  return lines
}

function lowerForStatement(context: StoreMethodLowerContext, statement: Extract<GeaIrStoreStmt, { kind: 'for' }>, indent: number): string[] | null {
  const pad = ' '.repeat(indent)
  const loopItem = loopArrayItemReadCache(context, statement, indent + 2)
  const init = loopItem ? lowerForIndexInit(context, statement.init, loopItem.indexName) : lowerForInit(context, statement.init)
  const test = loopItem ? `${sanitizeCppIdentifier(loopItem.indexName)} < (*this).${sanitizeCppIdentifier(loopItem.arrayField)}.size()` : statement.test ? lowerExpr(context, statement.test, 'boolean') : ''
  const update = statement.update ? lowerForUpdate(context, statement.update) : ''
  if (loopItem) {
    context.arrayItemAliases.set(arrayItemAliasKey(loopItem.arrayField, loopItem.indexName), loopItem.itemAlias)
    for (const field of loopItem.fields) {
      context.arrayItemFieldAliases.set(arrayItemFieldAliasKey(loopItem.arrayField, loopItem.indexName, field), loopItem.fieldAliases.get(field)!)
    }
  }
  const body = lowerStatements(context, statement.body, indent + 2)
  if (loopItem) {
    context.arrayItemAliases.delete(arrayItemAliasKey(loopItem.arrayField, loopItem.indexName))
    for (const field of loopItem.fields) context.arrayItemFieldAliases.delete(arrayItemFieldAliasKey(loopItem.arrayField, loopItem.indexName, field))
  }
  if (init === null || test === null || update === null || !body) return null
  return [`${pad}for (${init}; ${test}; ${update}) {`, ...(loopItem?.prelude ?? []), ...body, `${pad}}`]
}

function lowerForInit(context: StoreMethodLowerContext, statement: GeaIrStoreStmt | undefined): string | null {
  if (!statement) return ''
  if (statement.kind === 'var') {
    context.locals.add(statement.name)
    if (statement.localType) context.localTypes.set(statement.name, statement.localType)
    const init = statement.init ? lowerExpr(context, statement.init, 'number') : '0'
    return init ? `double ${sanitizeCppIdentifier(statement.name)} = ${init}` : null
  }
  if (statement.kind === 'assign') return lowerAssignment(context, statement.target, statement.value)
  return null
}

function lowerForIndexInit(context: StoreMethodLowerContext, statement: GeaIrStoreStmt | undefined, indexName: string): string | null {
  if (!statement) return ''
  if (statement.kind !== 'var' || statement.name !== indexName) return lowerForInit(context, statement)
  context.locals.add(statement.name)
  if (statement.localType) context.localTypes.set(statement.name, statement.localType)
  const init = statement.init ? lowerExpr(context, statement.init, 'number') : '0'
  return init ? `std::size_t ${sanitizeCppIdentifier(statement.name)} = static_cast<std::size_t>(${init})` : null
}

interface LoopArrayItemReadCache {
  arrayField: string
  indexName: string
  itemAlias: string
  fields: string[]
  fieldAliases: Map<string, string>
  prelude: string[]
}

function loopArrayItemReadCache(
  context: StoreMethodLowerContext,
  statement: Extract<GeaIrStoreStmt, { kind: 'for' }>,
  indent: number
): LoopArrayItemReadCache | null {
  if (!statement.test || statement.test.kind !== 'binary' || statement.test.op !== '<') return null
  if (statement.test.left.kind !== 'identifier') return null
  const indexName = statement.test.left.name
  const arrayField = thisArrayLengthTarget(statement.test.right)
  if (!arrayField || mutatesArrayShape(statement.body, arrayField)) return null
  const fields = Array.from(collectArrayItemFieldReads(statement.body, arrayField, indexName)).sort()
  if (fields.length === 0) return null
  const contextualHints = collectArrayItemFieldReadHints(context, statement.body, arrayField, indexName)

  const arrayName = sanitizeCppIdentifier(arrayField)
  const indexId = sanitizeCppIdentifier(indexName)
  const itemAlias = `__gea_${arrayName}_${indexId}_item`
  const fieldAliases = new Map<string, string>()
  const pad = ' '.repeat(indent)
  const prelude = [`${pad}auto &${itemAlias} = (*this).${arrayName}[static_cast<std::size_t>(${indexId})];`]
  for (const field of fields) {
    const staticHint = arrayItemFieldHint(context.store, arrayField, field)
    const hint = staticHint === 'any' ? contextualHints.get(field) ?? 'any' : staticHint
    if (hint === 'any') continue
    const alias = `__gea_${arrayName}_${indexId}_${sanitizeCppIdentifier(field)}`
    fieldAliases.set(field, alias)
    const directAccess = typedArrayItemFieldAccess(context.store, arrayField, field, itemAlias)
    const fieldKey = JSON.stringify(field)
    const dynamicRead =
      hint === 'string'
        ? `gea_ir::read_string_field(${itemAlias}, ${fieldKey})`
        : hint === 'boolean'
          ? `gea::runtime::coerce::to_boolean(${itemAlias}.record_get_literal(${fieldKey}))`
          : `gea_ir::read_number_field(${itemAlias}, ${fieldKey})`
    prelude.push(`${pad}auto ${alias} = ${directAccess ?? dynamicRead};`)
  }
  if (fieldAliases.size === 0) return null
  return { arrayField, indexName, itemAlias, fields: [...fieldAliases.keys()], fieldAliases, prelude }
}

function collectArrayItemFieldReads(statements: GeaIrStoreStmt[], arrayField: string, indexName: string): Set<string> {
  const fields = new Set<string>()
  const visitStatement = (statement: GeaIrStoreStmt): void => {
    if (statement.kind === 'var') {
      if (statement.init) visitExpr(statement.init)
    } else if (statement.kind === 'assign') {
      visitExpr(statement.value)
    } else if (statement.kind === 'expr') {
      visitExpr(statement.expr)
    } else if (statement.kind === 'return') {
      if (statement.value) visitExpr(statement.value)
    } else if (statement.kind === 'if') {
      visitExpr(statement.test)
      statement.consequent.forEach(visitStatement)
      statement.alternate?.forEach(visitStatement)
    } else if (statement.kind === 'for') {
      if (statement.init) visitStatement(statement.init)
      if (statement.test) visitExpr(statement.test)
      if (statement.update) visitExpr(statement.update)
      statement.body.forEach(visitStatement)
    }
  }
  const visitExpr = (expr: GeaIrStoreExpr): void => {
    const item = thisArrayItemFieldTarget(expr)
    if (item && item.arrayField === arrayField && item.index.kind === 'identifier' && item.index.name === indexName) {
      fields.add(item.itemField)
    }
    if (expr.kind === 'member') visitExpr(expr.object)
    else if (expr.kind === 'index') {
      visitExpr(expr.object)
      visitExpr(expr.index)
    } else if (expr.kind === 'call') {
      visitExpr(expr.callee)
      expr.args.forEach(visitExpr)
    } else if (expr.kind === 'object') {
      expr.fields.forEach((field) => visitExpr(field.value))
    } else if (expr.kind === 'array') {
      expr.elements.forEach(visitExpr)
    } else if (expr.kind === 'unary' || expr.kind === 'update') {
      visitExpr(expr.arg)
    } else if (expr.kind === 'binary' || expr.kind === 'logical') {
      visitExpr(expr.left)
      visitExpr(expr.right)
    } else if (expr.kind === 'conditional') {
      visitExpr(expr.test)
      visitExpr(expr.consequent)
      visitExpr(expr.alternate)
    }
  }
  statements.forEach(visitStatement)
  return fields
}

function collectArrayItemFieldReadHints(
  context: StoreMethodLowerContext,
  statements: GeaIrStoreStmt[],
  arrayField: string,
  indexName: string
): Map<string, StoreMethodHint> {
  const hints = new Map<string, StoreMethodHint>()
  const merge = (field: string, hint: StoreMethodHint): void => {
    if (hint === 'any') return
    const previous = hints.get(field)
    if (!previous) {
      hints.set(field, hint)
    } else if (previous !== hint) {
      hints.set(field, 'any')
    }
  }
  const assignmentTargetHint = (target: GeaIrStoreExpr): StoreMethodHint => {
    const field = thisFieldName(target)
    if (field) return fieldHint(context.store, field)
    const item = thisArrayItemFieldTarget(target)
    if (item) return arrayItemFieldHint(context.store, item.arrayField, item.itemField)
    return assignmentHint(target)
  }
  const visitStatement = (statement: GeaIrStoreStmt): void => {
    if (statement.kind === 'var') {
      if (statement.init) visitExpr(statement.init, 'any')
    } else if (statement.kind === 'assign') {
      visitExpr(statement.value, assignmentTargetHint(statement.target))
    } else if (statement.kind === 'expr') {
      visitExpr(statement.expr, 'any')
    } else if (statement.kind === 'return') {
      if (statement.value) visitExpr(statement.value, 'any')
    } else if (statement.kind === 'if') {
      visitExpr(statement.test, 'boolean')
      statement.consequent.forEach(visitStatement)
      statement.alternate?.forEach(visitStatement)
    } else if (statement.kind === 'for') {
      if (statement.init) visitStatement(statement.init)
      if (statement.test) visitExpr(statement.test, 'boolean')
      if (statement.update) visitExpr(statement.update, 'number')
      statement.body.forEach(visitStatement)
    }
  }
  const visitExpr = (expr: GeaIrStoreExpr, hint: StoreMethodHint): void => {
    const item = thisArrayItemFieldTarget(expr)
    if (item && item.arrayField === arrayField && item.index.kind === 'identifier' && item.index.name === indexName) {
      merge(item.itemField, hint)
    }
    if (expr.kind === 'member') {
      visitExpr(expr.object, 'any')
    } else if (expr.kind === 'index') {
      visitExpr(expr.object, 'any')
      visitExpr(expr.index, 'number')
    } else if (expr.kind === 'call') {
      visitExpr(expr.callee, 'any')
      expr.args.forEach((arg) => visitExpr(arg, 'any'))
    } else if (expr.kind === 'object') {
      expr.fields.forEach((field) => visitExpr(field.value, 'any'))
    } else if (expr.kind === 'array') {
      expr.elements.forEach((element) => visitExpr(element, 'any'))
    } else if (expr.kind === 'unary' || expr.kind === 'update') {
      visitExpr(expr.arg, expr.kind === 'unary' && expr.op === '!' ? 'boolean' : 'number')
    } else if (expr.kind === 'binary' || expr.kind === 'logical') {
      const op = expr.op === '===' ? '==' : expr.op === '!==' ? '!=' : expr.op
      if (op === '+' && (hint === 'string' || expressionContainsKnownString(context, expr.left) || expressionContainsKnownString(context, expr.right))) {
        visitExpr(expr.left, 'string')
        visitExpr(expr.right, 'string')
      } else if ((op === '==' || op === '!=') && (expressionContainsKnownString(context, expr.left) || expressionContainsKnownString(context, expr.right))) {
        visitExpr(expr.left, 'string')
        visitExpr(expr.right, 'string')
      } else if (op === '&&' || op === '||') {
        visitExpr(expr.left, 'boolean')
        visitExpr(expr.right, 'boolean')
      } else {
        visitExpr(expr.left, 'number')
        visitExpr(expr.right, 'number')
      }
    } else if (expr.kind === 'conditional') {
      visitExpr(expr.test, 'boolean')
      visitExpr(expr.consequent, hint)
      visitExpr(expr.alternate, hint)
    }
  }
  statements.forEach(visitStatement)
  return hints
}

function expressionContainsKnownString(context: StoreMethodLowerContext, expr: GeaIrStoreExpr): boolean {
  if (expr.kind === 'string') return true
  if (expr.kind === 'identifier') {
    if (typeof context.constants.get(expr.name) === 'string') return true
    if (context.paramHints.get(expr.name) === 'string') return true
    return false
  }
  if (expr.kind === 'member') {
    const field = thisFieldName(expr)
    if (field && fieldHint(context.store, field) === 'string') return true
    const item = thisArrayItemFieldTarget(expr)
    if (item && arrayItemFieldHint(context.store, item.arrayField, item.itemField) === 'string') return true
    const aliasField = arrayElementAliasFieldTarget(context, expr)
    if (aliasField && arrayItemFieldHint(context.store, aliasField.alias.arrayField, aliasField.itemField) === 'string') return true
    return expressionContainsKnownString(context, expr.object)
  }
  if (expr.kind === 'binary' || expr.kind === 'logical') {
    return expressionContainsKnownString(context, expr.left) || expressionContainsKnownString(context, expr.right)
  }
  if (expr.kind === 'conditional') {
    return expressionContainsKnownString(context, expr.consequent) || expressionContainsKnownString(context, expr.alternate)
  }
  return false
}

function mutatesArrayShape(statements: GeaIrStoreStmt[], arrayField: string): boolean {
  const visitStatement = (statement: GeaIrStoreStmt): boolean => {
    if (statement.kind === 'assign') return thisArrayLengthTarget(statement.target) === arrayField || visitExpr(statement.value)
    if (statement.kind === 'var') return statement.init ? visitExpr(statement.init) : false
    if (statement.kind === 'expr') return visitExpr(statement.expr)
    if (statement.kind === 'return') return statement.value ? visitExpr(statement.value) : false
    if (statement.kind === 'if') return visitExpr(statement.test) || statement.consequent.some(visitStatement) || Boolean(statement.alternate?.some(visitStatement))
    if (statement.kind === 'for') {
      return Boolean(statement.init && visitStatement(statement.init)) || Boolean(statement.test && visitExpr(statement.test)) || Boolean(statement.update && visitExpr(statement.update)) || statement.body.some(visitStatement)
    }
    return false
  }
  const visitExpr = (expr: GeaIrStoreExpr): boolean => {
    if (thisArrayPushTarget(expr) === arrayField) return true
    if (expr.kind === 'member') return visitExpr(expr.object)
    if (expr.kind === 'index') return visitExpr(expr.object) || visitExpr(expr.index)
    if (expr.kind === 'call') return visitExpr(expr.callee) || expr.args.some(visitExpr)
    if (expr.kind === 'object') return expr.fields.some((field) => visitExpr(field.value))
    if (expr.kind === 'array') return expr.elements.some(visitExpr)
    if (expr.kind === 'unary' || expr.kind === 'update') return visitExpr(expr.arg)
    if (expr.kind === 'binary' || expr.kind === 'logical') return visitExpr(expr.left) || visitExpr(expr.right)
    if (expr.kind === 'conditional') return visitExpr(expr.test) || visitExpr(expr.consequent) || visitExpr(expr.alternate)
    return false
  }
  return statements.some(visitStatement)
}

function arrayItemFieldAlias(
  context: StoreMethodLowerContext,
  item: { arrayField: string; index: GeaIrStoreExpr; itemField: string }
): string | null {
  return item.index.kind === 'identifier'
    ? context.arrayItemFieldAliases.get(arrayItemFieldAliasKey(item.arrayField, item.index.name, item.itemField)) ?? null
    : null
}

// True when the named store array field reuses geatsc's `::__gea_type_<Name>`
// struct (the interface-reuse path) rather than a synthesized
// `<Store>_<field>_item`. The reused struct presence-gates its dynamic surface
// (`__gea_to_value`/`__gea_json_into`/`gea_cpp_key`) on `__gea_has_<field>`, so
// a direct member write must ALSO set that flag or the change is invisible to
// snapshots/change-records/JSON.stringify. Synthesized structs have no such
// member, so this returns false for them and nothing is appended.
function arrayFieldReusesInterfaceStruct(store: GeaIrStore, arrayFieldName: string): boolean {
  const field = store.fields.find((candidate) => candidate.name === arrayFieldName)
  return field?.typedStorage?.useInterfaceStruct === true
}

// `<itemObjectExpr>.__gea_has_<field> = true` — the presence-flag write that
// must accompany a direct member assignment on a reused interface struct.
// `itemObjectExpr` is the SAME object reference the direct field write goes
// through (an alias or the bounds-checked array item temp).
function presenceFlagAssignment(itemObjectExpr: string, itemFieldName: string): string {
  return `${itemObjectExpr}.__gea_has_${sanitizeCppIdentifier(itemFieldName)} = true`
}

function lowerAssignment(context: StoreMethodLowerContext, target: GeaIrStoreExpr, value: GeaIrStoreExpr, statementOnly = false): string | null {
  const field = thisFieldName(target)
  if (field) {
    context.needsFieldSet = true
    // Empty array literal → the field's OWN declared (typed) empty value, not a
    // generic vector<gea_cpp_value>: the per-field item struct isn't constructible
    // from gea_cpp_value, so the generic form wouldn't convert. decltype yields the
    // exact element type (e.g. std::vector<Store_field_item>{}).
    if (value.kind === 'array' && value.elements.length === 0) {
      const member = `(*this).${sanitizeCppIdentifier(field)}`
      return `set_field(${JSON.stringify(field)}, ${member}, decltype(${member}){})`
    }
    const expr = lowerExpr(context, value, fieldHint(context.store, field))
    return expr ? `set_field(${JSON.stringify(field)}, (*this).${sanitizeCppIdentifier(field)}, ${expr})` : null
  }

  const lengthField = thisArrayLengthTarget(target)
  if (lengthField) {
    context.needsArrayLengthSet = true
    const expr = lowerExpr(context, value, 'number')
    return expr ? `set_array_length(${JSON.stringify(lengthField)}, (*this).${sanitizeCppIdentifier(lengthField)}, static_cast<std::size_t>(${expr}))` : null
  }

  const item = thisArrayItemFieldTarget(target)
  if (item) {
    context.needsArrayItemSet = true
    const index = lowerExpr(context, item.index, 'number')
    const expr = lowerExpr(context, value, arrayItemFieldHint(context.store, item.arrayField, item.itemField))
    if (!index || !expr) return null
    const alias = arrayItemFieldAlias(context, item)
    const stored = alias ? `(${alias} = ${expr}, ${alias})` : expr
    const array = `(*this).${sanitizeCppIdentifier(item.arrayField)}`
    const itemAlias = item.index.kind === 'identifier' ? context.arrayItemAliases.get(arrayItemAliasKey(item.arrayField, item.index.name)) : undefined
    const indexTemp = nextStoreMethodTemp(context, 'index')
    const itemObjectExpr = itemAlias ?? `${array}[${indexTemp}]`
    const directAccess = typedArrayItemFieldAccess(context.store, item.arrayField, item.itemField, itemObjectExpr)
    if (directAccess) {
      context.dirtyArrayFields.add(item.arrayField)
      const markDirtyOnce = `mark_dirty_array_field_once_${sanitizeCppIdentifier(item.arrayField)}`
      // Interface-reuse structs presence-gate their dynamic surface on
      // `__gea_has_<field>`; set it alongside the member write so the value is
      // visible to snapshots/change-records/JSON.stringify. Synthesized structs
      // have no such member, so this is empty for them.
      const reusesInterfaceStruct = arrayFieldReusesInterfaceStruct(context.store, item.arrayField)
      if (itemAlias) {
        const setPresence = reusesInterfaceStruct ? `, ${presenceFlagAssignment(itemObjectExpr, item.itemField)}` : ''
        if (statementOnly) {
          const setPresenceStmt = reusesInterfaceStruct ? ` ${presenceFlagAssignment(itemObjectExpr, item.itemField)};` : ''
          return `{ ${markDirtyOnce}(); ${directAccess} = ${stored};${setPresenceStmt} }`
        }
        return `(${markDirtyOnce}(), ${directAccess} = ${stored}${setPresence})`
      }
      const setPresence = reusesInterfaceStruct ? ` ${presenceFlagAssignment(itemObjectExpr, item.itemField)};` : ''
      if (statementOnly) {
        return `{ ${markDirtyOnce}(); auto ${indexTemp} = static_cast<std::size_t>(${index}); if (${indexTemp} >= ${array}.size()) ${array}.resize(${indexTemp} + 1); auto __gea_value = ${stored}; ${directAccess} = __gea_value;${setPresence} }`
      }
      return `([&]() { ${markDirtyOnce}(); auto ${indexTemp} = static_cast<std::size_t>(${index}); if (${indexTemp} >= ${array}.size()) ${array}.resize(${indexTemp} + 1); auto __gea_value = ${stored}; ${directAccess} = __gea_value;${setPresence} return __gea_value; })()`
    }
    context.needsDynamicArrayItemSet = true
    return `set_array_item_field(${JSON.stringify(item.arrayField)}, (*this).${sanitizeCppIdentifier(item.arrayField)}, static_cast<std::size_t>(${index}), ${JSON.stringify(item.itemField)}, ${stored})`
  }

  const aliasField = arrayElementAliasFieldTarget(context, target)
  if (aliasField) {
    const { alias, itemField } = aliasField
    context.needsArrayItemSet = true
    const expr = lowerExpr(context, value, arrayItemFieldHint(context.store, alias.arrayField, itemField))
    if (!expr) return null
    const directAccess = typedArrayItemFieldAccess(context.store, alias.arrayField, itemField, alias.itemExpression)
    if (directAccess) {
      context.dirtyArrayFields.add(alias.arrayField)
      const markDirtyOnce = `mark_dirty_array_field_once_${sanitizeCppIdentifier(alias.arrayField)}`
      const reusesInterfaceStruct = arrayFieldReusesInterfaceStruct(context.store, alias.arrayField)
      const setPresence = reusesInterfaceStruct ? `, ${presenceFlagAssignment(alias.itemExpression, itemField)}` : ''
      if (statementOnly) {
        const setPresenceStmt = reusesInterfaceStruct ? ` ${presenceFlagAssignment(alias.itemExpression, itemField)};` : ''
        return `{ ${markDirtyOnce}(); ${directAccess} = ${expr};${setPresenceStmt} }`
      }
      return `(${markDirtyOnce}(), ${directAccess} = ${expr}${setPresence})`
    }
    context.needsDynamicArrayItemSet = true
    return `set_array_item_field(${JSON.stringify(alias.arrayField)}, (*this).${sanitizeCppIdentifier(alias.arrayField)}, ${alias.indexExpression}, ${JSON.stringify(itemField)}, ${expr})`
  }

  // Whole-element assignment `this.arr[i] = v`. Without this case the target
  // falls through to the generic lowering, whose index read is a bounds-checked
  // BY-VALUE lambda — assigning into that temporary silently no-ops (weather's
  // fetch-queue shift returned the same head forever, so only the first city of
  // every queue rebuild ever fetched). Emit a real lvalue write with the same
  // dirty-once marking as item-field sets.
  if (target.kind === 'index') {
    const arrayField = thisFieldName(target.object)
    if (arrayField) {
      context.needsArrayItemSet = true
      context.dirtyArrayFields.add(arrayField)
      const index = lowerExpr(context, target.index, 'number')
      const expr = lowerExpr(context, value, 'any')
      if (!index || !expr) return null
      const array = `(*this).${sanitizeCppIdentifier(arrayField)}`
      const markDirtyOnce = `mark_dirty_array_field_once_${sanitizeCppIdentifier(arrayField)}`
      const indexTemp = nextStoreMethodTemp(context, 'index')
      if (statementOnly) {
        return `{ ${markDirtyOnce}(); auto ${indexTemp} = static_cast<std::size_t>(${index}); if (${indexTemp} >= ${array}.size()) ${array}.resize(${indexTemp} + 1); auto __gea_value = ${expr}; ${array}[${indexTemp}] = __gea_value; }`
      }
      return `([&]() { ${markDirtyOnce}(); auto ${indexTemp} = static_cast<std::size_t>(${index}); if (${indexTemp} >= ${array}.size()) ${array}.resize(${indexTemp} + 1); auto __gea_value = ${expr}; ${array}[${indexTemp}] = __gea_value; return __gea_value; })()`
    }
  }

  const hostSetter = hostFacadePropertySetter(exprPath(target))
  if (hostSetter) {
    const rhs = lowerExpr(context, value, hostSetter.hint)
    return rhs ? `${hostSetter.target}(${rhs})` : null
  }

  const globalField = globalStoreFieldTarget(context, target)
  if (globalField) {
    const rhs = value.kind === 'array' && value.elements.length === 0
      ? `decltype(${globalField.access}){}`
      : lowerExpr(context, value, globalField.hint)
    if (!rhs) return null
    if (statementOnly) {
      return `{ auto &__gea_store = ${globalField.storeAccess}; auto __gea_value = ${rhs}; __gea_store->${globalField.fieldName} = __gea_value; if constexpr (requires { __gea_store->__gea_hub.notify(${JSON.stringify(globalField.fieldName)}); }) __gea_store->__gea_hub.notify(${JSON.stringify(globalField.fieldName)}); }`
    }
    return `([&]() { auto &__gea_store = ${globalField.storeAccess}; auto __gea_value = ${rhs}; __gea_store->${globalField.fieldName} = __gea_value; if constexpr (requires { __gea_store->__gea_hub.notify(${JSON.stringify(globalField.fieldName)}); }) __gea_store->__gea_hub.notify(${JSON.stringify(globalField.fieldName)}); return __gea_store->${globalField.fieldName}; })()`
  }

  const lhs = lowerExpr(context, target, 'any')
  const rhs = lowerExpr(context, value, assignmentHint(target))
  return lhs && rhs ? `${lhs} = ${rhs}` : null
}

function hostFacadePropertySetter(path: string | null): { target: string; hint: StoreMethodHint } | null {
  if (!path) return null
  const setters: Record<string, { target: string; hint: StoreMethodHint }> = {
    'Display.orientation': { target: 'gea::host::Display.setOrientation', hint: 'string' },
    'display.orientation': { target: 'gea::host::Display.setOrientation', hint: 'string' },
    '__gea_Display.orientation': { target: 'gea::host::Display.setOrientation', hint: 'string' },
    'Display.supportedOrientations': { target: 'gea::host::Display.setSupportedOrientations', hint: 'any' },
    'display.supportedOrientations': { target: 'gea::host::Display.setSupportedOrientations', hint: 'any' },
    '__gea_Display.supportedOrientations': { target: 'gea::host::Display.setSupportedOrientations', hint: 'any' },
    'Display.autoRotate': { target: 'gea::host::Display.setAutoRotate', hint: 'boolean' },
    'display.autoRotate': { target: 'gea::host::Display.setAutoRotate', hint: 'boolean' },
    '__gea_Display.autoRotate': { target: 'gea::host::Display.setAutoRotate', hint: 'boolean' },
    'Display.pixelFormat': { target: 'gea::host::Display.setPixelFormat', hint: 'string' },
    'display.pixelFormat': { target: 'gea::host::Display.setPixelFormat', hint: 'string' },
    '__gea_Display.pixelFormat': { target: 'gea::host::Display.setPixelFormat', hint: 'string' },
  }
  return setters[path] ?? null
}

// Exported for test coverage: the change-record snapshot gating
// (has_queued_consumers) must never regress to eager gea_cpp_key(array)
// whole-vector boxing.
export function mutationHelpers(context: StoreMethodLowerContext): string[] {
  if (!needsMutationHelpers(context)) return []
  const typedOnlyStore = storeHubEmissionEnabled() && !storeDynamicFallbackEnabled()
  // Dirty keys are always string LITERALS from the re-lowered bodies (static
  // lifetime), so the per-call scratch list holds pointers — no heap alloc per
  // marked field per method invocation.
  const lines = [
    '    std::vector<const char *> dirty_fields;',
    ...(context.needsArrayItemSet && !typedOnlyStore ? ['    std::vector<std::pair<std::string, gea_cpp_value>> dirty_array_fields;'] : []),
    '    auto mark_dirty_field = [&dirty_fields](const char *key) mutable -> void {',
    '      const char *dirty_key = key ? key : "";',
    '      for (const auto &existing : dirty_fields) {',
    '        if (std::strcmp(existing, dirty_key) == 0) return;',
    '      }',
    '      dirty_fields.push_back(dirty_key);',
    '    };',
  ]
  if (context.needsArrayItemSet && typedOnlyStore) {
    for (const field of [...context.dirtyArrayFields].sort()) {
      const safeField = sanitizeCppIdentifier(field)
      lines.push(
        `    bool __gea_dirty_array_field_${safeField} = false;`,
        `    auto mark_dirty_array_field_once_${safeField} = [&mark_dirty_field, &__gea_dirty_array_field_${safeField}]() mutable -> void {`,
        `      if (__gea_dirty_array_field_${safeField}) return;`,
        `      mark_dirty_field(${JSON.stringify(field)});`,
        `      __gea_dirty_array_field_${safeField} = true;`,
        '    };',
      )
    }
  } else if (context.needsArrayItemSet) {
    lines.push(
      '    auto mark_dirty_array_field = [this, &dirty_array_fields](const char *key, const auto &array) mutable -> void {',
      '      const char *dirty_key = key ? key : "";',
      '      for (const auto &existing : dirty_array_fields) {',
      '        if (existing.first == dirty_key) return;',
      '      }',
      '      auto self_value = this->__gea_ctor_return_value;',
      '      auto state = gea_cpp_key(__gea_global__priv().get(self_value));',
      '      auto previous = gea_ir::storeHasQueuedConsumers(state, key) ? gea_cpp_snapshot_value(gea_cpp_key(array)) : gea_cpp_value::missing();',
      '      dirty_array_fields.push_back({std::string(dirty_key), std::move(previous)});',
      '    };',
    )
    for (const field of [...context.dirtyArrayFields].sort()) {
      const safeField = sanitizeCppIdentifier(field)
      lines.push(
        `    bool __gea_dirty_array_field_${safeField} = false;`,
        `    auto mark_dirty_array_field_once_${safeField} = [this, &mark_dirty_field, &mark_dirty_array_field, &__gea_dirty_array_field_${safeField}]() mutable -> void {`,
        `      if (__gea_dirty_array_field_${safeField}) return;`,
        `      mark_dirty_array_field(${JSON.stringify(field)}, (*this).${safeField});`,
        `      mark_dirty_field(${JSON.stringify(field)});`,
        `      __gea_dirty_array_field_${safeField} = true;`,
        '    };',
      )
    }
  }
  lines.push(
    ...(context.needsArrayItemSet && !typedOnlyStore
      ? ['    auto flush_dirty_fields = [this, &dirty_fields, &dirty_array_fields]() mutable -> void {', '      if (dirty_fields.empty() && dirty_array_fields.empty()) return;']
      : ['    auto flush_dirty_fields = [this, &dirty_fields]() mutable -> void {', '      if (dirty_fields.empty()) return;']),
    // Typed channel FIRST and unconditionally — the dynamic `direct` map may
    // not exist (no boxed observers) while typed hub subscribers do. Only
    // emitted when this program carries the hub (see decideStoreHubEmission).
    ...(storeHubEmissionEnabled() ? ['      for (const auto &__gea_hub_key : dirty_fields) (*this).__gea_hub.notify(__gea_hub_key);'] : []),
    ...(typedOnlyStore
      ? [
          '      dirty_fields.clear();',
          '      return;',
          '    };',
        ]
      : [
          '      auto self_value = this->__gea_ctor_return_value;',
          '      auto state = gea_cpp_key(__gea_global__priv().get(self_value));',
        ]),
    ...(context.needsArrayItemSet && !typedOnlyStore
      ? [
          '      for (const auto &entry : dirty_array_fields) {',
          '        if (entry.second.kind != gea_cpp_value::kind_t::missing) {',
          '          (void)(fn__queue(self_value, state, entry.first, gea_ir::changeRecord(entry.second, self_value.record_get_literal(entry.first.c_str()))));',
          '        }',
          '      }',
        ]
      : []),
    ...(typedOnlyStore ? [] : [
    '      auto direct_map = gea_cpp_key(state.record_get_literal("direct"));',
    '      if (!gea::runtime::coerce::to_boolean(direct_map)) {',
    '        dirty_fields.clear();',
    ...(context.needsArrayItemSet ? ['        dirty_array_fields.clear();'] : []),
    '        return;',
    '      }',
    '      for (const auto &key : dirty_fields) {',
    '        auto direct = gea_cpp_key(gea_cpp_map_like_get_literal(direct_map, key));',
    '        if (!gea::runtime::coerce::to_boolean(direct)) continue;',
    // Direct handlers registered via `bindReactiveApply` are zero-arg
    // observers (`[apply, pending]() { schedule(apply, pending); }`) —
    // they ignore the value entirely. Pre-fetching `self_value
    // .record_get_literal(key)` here would trigger the store's
    // `gea_cpp_key(typed_vec)` getter, which heap-allocates one record
    // per element to box typed items into a gea_cpp_value array snapshot.
    // For BallStore.balls (32 items) that's 32 wasted heap allocations
    // per `flush_dirty_fields` call (i.e. per BallStore.tick() frame),
    // observed as ~6 ms in raf. Pass `gea_cpp_value::missing()` instead
    // — handlers that need the live value already re-read it themselves
    // via the typed store pointer (see keyed-list `apply_typed_path`),
    // and the dynamic-record proxy fallback path still snapshots when it
    // calls handlers from its `set` trap with the actual new value.
    '        for (auto &handler : direct) (void)handler(gea_cpp_value::missing());',
    '      }',
    '      dirty_fields.clear();',
    ...(context.needsArrayItemSet && !typedOnlyStore ? ['      dirty_array_fields.clear();'] : []),
    '    };',
    ]),
  )
  if (context.needsFieldSet) {
    lines.push(
      '    auto set_field = [this, &mark_dirty_field]<typename Field, typename Value>(const char *key, Field &field, Value value) mutable -> Value {',
      '      auto values_equal = []<typename Left, typename Right>(const Left &left, const Right &right) -> bool {',
      '        using L = std::decay_t<Left>;',
      '        using R = std::decay_t<Right>;',
      '        if constexpr (std::is_arithmetic_v<L> && std::is_arithmetic_v<R>) {',
      '          return gea_cpp_same_value_zero(static_cast<double>(left), static_cast<double>(right));',
      '        } else if constexpr (std::is_same_v<L, std::string> && std::is_same_v<R, std::string>) {',
      '          return left == right;',
      '        } else {',
      '          return false;',
      '        }',
      '      };',
      '      if (!values_equal(field, value)) {',
      ...(typedOnlyStore
        ? [
            '        gea_ir::storeAssignField(field, value);',
            '        mark_dirty_field(key);',
          ]
        : [
            '        auto self_value = this->__gea_ctor_return_value;',
            '        auto state = gea_cpp_key(__gea_global__priv().get(self_value));',
            '        const bool has_queued_consumers = gea_ir::storeHasQueuedConsumers(state, key);',
            '        auto old_value = has_queued_consumers ? gea_cpp_key(field) : gea_cpp_value::missing();',
            '        auto stored_value = has_queued_consumers ? gea_cpp_key(value) : gea_cpp_value::missing();',
            '        gea_ir::storeAssignField(field, value);',
            '        mark_dirty_field(key);',
            '        if (has_queued_consumers) {',
            '          (void)(fn__queue(self_value, state, std::string(key ? key : ""), gea_ir::changeRecord(old_value, stored_value)));',
            '        }',
          ]),
      '      }',
      '      return value;',
      '    };',
    )
  }
  if (context.needsArrayLengthSet) {
    lines.push(
      // Change-record snapshots (gea_cpp_key(array)) box the ENTIRE typed
      // vector; only take them when a queued consumer will actually read the
      // change record — mirrors set_field's has_queued_consumers guard.
      '    auto set_array_length = [this, &mark_dirty_field]<typename Array>(const char *array_key, Array &array, std::size_t length) mutable -> double {',
      '      if (array.size() == length) return static_cast<double>(length);',
      '      array.resize(length);',
      '      mark_dirty_field(array_key);',
      ...(typedOnlyStore
        ? []
        : [
            '      auto self_value = this->__gea_ctor_return_value;',
            '      auto state = gea_cpp_key(__gea_global__priv().get(self_value));',
            '      const bool has_queued_consumers = gea_ir::storeHasQueuedConsumers(state, array_key);',
            '      auto old_value = has_queued_consumers ? gea_cpp_key(array) : gea_cpp_value::missing();',
            '      auto next_value = has_queued_consumers ? gea_cpp_key(array) : gea_cpp_value::missing();',
            '      if (has_queued_consumers) {',
            '        (void)(fn__queue(self_value, state, std::string(array_key ? array_key : ""), gea_ir::changeRecord(old_value, next_value)));',
            '      }',
          ]),
      '      return static_cast<double>(length);',
      '    };',
    )
  }
  if (context.needsDynamicArrayItemSet) {
    // Dispatches statically on the array's element type. `gea_cpp_value`
    // arrays keep the dynamic record-slot path (back-compat for stores not
    // yet promoted to typed storage). Typed arrays (whose element type is a
    // synthesised `${storeClass}_${fieldName}_item` POD) write through
    // `gea_ir::set_typed_field`, which is generated per-item-type by the
    // gea plugin (see `generateTypedItemHelpers` in cpp-stores.ts) and
    // dispatches the property-name string to a real C++ field assignment.
    lines.push(
      ...(typedOnlyStore
        ? ['    auto set_array_item_field = [this, &mark_dirty_field]<typename Array, typename Value>(const char *array_key, Array &array, std::size_t index, const char *property, Value value) mutable -> Value {']
        : ['    auto set_array_item_field = [this, &mark_dirty_field, &mark_dirty_array_field]<typename Array, typename Value>(const char *array_key, Array &array, std::size_t index, const char *property, Value value) mutable -> Value {']),
      '      using __GeaSlotElement = typename std::decay_t<Array>::value_type;',
      ...(typedOnlyStore ? [] : ['      mark_dirty_array_field(array_key, array);']),
      '      if (index >= array.size()) array.resize(index + 1);',
      '      auto &slot = array[index];',
      ...(typedOnlyStore
        ? ['      (void)sizeof(__GeaSlotElement);', '      gea_ir::set_typed_field(slot, property, value);']
        : [
            '      if constexpr (std::is_same_v<__GeaSlotElement, gea_cpp_value>) {',
            '        if (slot.is_nullish()) {',
            '          slot.kind = gea_cpp_value::kind_t::record;',
            '          slot.entries = std::make_shared<std::vector<std::pair<std::string, gea_cpp_value>>>();',
            '        }',
            '        slot.record_set_literal(property, gea_cpp_key(value));',
            '        slot.record_set(gea_ir::dirtySymbol(), gea_cpp_value(true));',
            '      } else {',
            '        gea_ir::set_typed_field(slot, property, value);',
            '      }',
          ]),
      '      mark_dirty_field(array_key);',
      '      return value;',
      '    };',
    )
  }
  if (context.needsArrayPush) {
    lines.push(
      '    auto push_array_items = [this, &mark_dirty_field]<typename Array, typename... Values>(const char *array_key, Array &array, Values... values) mutable -> double {',
      '      if constexpr (sizeof...(Values) > 0) {',
      // A typed-only store notifies through the SignalHub (mark_dirty_field →
      // flush_dirty_fields). The boxed change-record / queued-consumer notify
      // (self_value/_priv/storeHasQueuedConsumers/fn__queue) only exists in the
      // dynamic-fallback runtime, so gate it on !typedOnlyStore — otherwise it
      // references members a typed store does not carry.
      ...(typedOnlyStore
        ? []
        : [
            '        auto self_value = this->__gea_ctor_return_value;',
            '        auto state = gea_cpp_key(__gea_global__priv().get(self_value));',
            '        const bool has_queued_consumers = gea_ir::storeHasQueuedConsumers(state, array_key);',
            '        auto old_value = has_queued_consumers ? gea_cpp_key(array) : gea_cpp_value::missing();',
          ]),
      // Already-Element values (typed literal construction at the call site)
      // push straight through — no box + __gea_from_value round trip.
      '        auto push_one = [&]<typename Value>(Value value) mutable -> void {',
      '          using Element = typename std::decay_t<Array>::value_type;',
      '          if constexpr (std::is_same_v<std::decay_t<Value>, Element>) {',
      '            array.push_back(std::move(value));',
      '          } else if constexpr (requires(const gea_cpp_value &__gea_v) { Element::__gea_from_value(__gea_v); }) {',
      '            array.push_back(Element::__gea_from_value(gea_cpp_key(value)));',
      '          } else {',
      '            array.push_back(static_cast<Element>(gea_cpp_key(value)));',
      '          }',
      '        };',
      '        (push_one(values), ...);',
      ...(typedOnlyStore ? [] : ['        auto next_value = has_queued_consumers ? gea_cpp_key(array) : gea_cpp_value::missing();']),
      '        mark_dirty_field(array_key);',
      ...(typedOnlyStore
        ? []
        : [
            '        if (has_queued_consumers) {',
            '          (void)(fn__queue(self_value, state, std::string(array_key ? array_key : ""), gea_ir::changeRecord(old_value, next_value)));',
            '        }',
          ]),
      '      }',
      '      return static_cast<double>(array.size());',
      '    };',
    )
  }
  if (context.needsArrayUnshift) {
    lines.push(
      '    auto unshift_array_items = [this, &mark_dirty_field]<typename Array, typename... Values>(const char *array_key, Array &array, Values... values) mutable -> double {',
      '      if constexpr (sizeof...(Values) > 0) {',
      ...(typedOnlyStore
        ? []
        : [
            '        auto self_value = this->__gea_ctor_return_value;',
            '        auto state = gea_cpp_key(__gea_global__priv().get(self_value));',
            '        const bool has_queued_consumers = gea_ir::storeHasQueuedConsumers(state, array_key);',
            '        auto old_value = has_queued_consumers ? gea_cpp_key(array) : gea_cpp_value::missing();',
          ]),
      '        using Element = typename std::decay_t<Array>::value_type;',
      // Already-Element values pass through; decodable elements use
      // __gea_from_value (the bare static_cast requires a converting ctor
      // interface-reuse structs do not have); everything else casts.
      '        auto __gea_make_insert = [&]<typename Value>(Value value) -> Element { if constexpr (std::is_same_v<std::decay_t<Value>, Element>) { return value; } else if constexpr (requires(const gea_cpp_value &__gea_v) { Element::__gea_from_value(__gea_v); }) { return Element::__gea_from_value(gea_cpp_key(value)); } else { return static_cast<Element>(gea_cpp_key(value)); } };',
      '        std::array<Element, sizeof...(Values)> __gea_inserts = {__gea_make_insert(values)...};',
      '        array.insert(array.begin(), std::make_move_iterator(__gea_inserts.begin()), std::make_move_iterator(__gea_inserts.end()));',
      ...(typedOnlyStore ? [] : ['        auto next_value = has_queued_consumers ? gea_cpp_key(array) : gea_cpp_value::missing();']),
      '        mark_dirty_field(array_key);',
      ...(typedOnlyStore
        ? []
        : [
            '        if (has_queued_consumers) {',
            '          (void)(fn__queue(self_value, state, std::string(array_key ? array_key : ""), gea_ir::changeRecord(old_value, next_value)));',
            '        }',
          ]),
      '      }',
      '      return static_cast<double>(array.size());',
      '    };',
    )
  }
  if (context.needsArraySplice) {
    lines.push(
      '    auto splice_array_items = [this, &mark_dirty_field]<typename Array, typename... Values>(const char *array_key, Array &array, double start_raw, double delete_count_raw, Values... values) mutable -> Array {',
      ...(typedOnlyStore
        ? []
        : [
            '      auto self_value = this->__gea_ctor_return_value;',
            '      auto state = gea_cpp_key(__gea_global__priv().get(self_value));',
            '      const bool has_queued_consumers = gea_ir::storeHasQueuedConsumers(state, array_key);',
            '      auto old_value = has_queued_consumers ? gea_cpp_key(array) : gea_cpp_value::missing();',
          ]),
      '      const std::ptrdiff_t signed_size = static_cast<std::ptrdiff_t>(array.size());',
      '      std::ptrdiff_t start = static_cast<std::ptrdiff_t>(start_raw);',
      '      if (start < 0) start = std::max<std::ptrdiff_t>(0, signed_size + start);',
      '      if (start > signed_size) start = signed_size;',
      '      std::ptrdiff_t delete_count = static_cast<std::ptrdiff_t>(delete_count_raw);',
      '      if (delete_count < 0) delete_count = 0;',
      '      if (delete_count > signed_size - start) delete_count = signed_size - start;',
      '      auto removed_begin = array.begin() + start;',
      '      auto removed_end = removed_begin + delete_count;',
      '      Array removed(std::make_move_iterator(removed_begin), std::make_move_iterator(removed_end));',
      '      array.erase(removed_begin, removed_end);',
      '      if constexpr (sizeof...(Values) > 0) {',
      '        using Element = typename std::decay_t<Array>::value_type;',
      '        auto __gea_make_insert = [&]<typename Value>(Value value) -> Element { if constexpr (std::is_same_v<std::decay_t<Value>, Element>) { return value; } else if constexpr (requires(const gea_cpp_value &__gea_v) { Element::__gea_from_value(__gea_v); }) { return Element::__gea_from_value(gea_cpp_key(value)); } else { return static_cast<Element>(gea_cpp_key(value)); } };',
      '        std::array<Element, sizeof...(Values)> __gea_inserts = {__gea_make_insert(values)...};',
      '        array.insert(array.begin() + start, std::make_move_iterator(__gea_inserts.begin()), std::make_move_iterator(__gea_inserts.end()));',
      '      }',
      ...(typedOnlyStore ? [] : ['      auto next_value = has_queued_consumers ? gea_cpp_key(array) : gea_cpp_value::missing();']),
      '      mark_dirty_field(array_key);',
      ...(typedOnlyStore
        ? []
        : [
            '      if (has_queued_consumers) {',
            '        (void)(fn__queue(self_value, state, std::string(array_key ? array_key : ""), gea_ir::changeRecord(old_value, next_value)));',
            '      }',
          ]),
      '      return removed;',
      '    };',
    )
  }
  if (context.needsArrayPop || context.needsArrayShift) {
    const endIter = context.needsArrayPop
    const beginIter = context.needsArrayShift
    if (endIter) {
      lines.push(
        '    auto pop_array_item = [this, &mark_dirty_field]<typename Array>(const char *array_key, Array &array) mutable -> typename Array::value_type {',
        '      using Element = typename Array::value_type;',
        '      if (array.empty()) return Element{};',
        ...(typedOnlyStore
          ? []
          : [
              '      auto self_value = this->__gea_ctor_return_value;',
              '      auto state = gea_cpp_key(__gea_global__priv().get(self_value));',
              '      const bool has_queued_consumers = gea_ir::storeHasQueuedConsumers(state, array_key);',
              '      auto old_value = has_queued_consumers ? gea_cpp_key(array) : gea_cpp_value::missing();',
            ]),
        '      Element removed = std::move(array.back());',
        '      array.pop_back();',
        ...(typedOnlyStore ? [] : ['      auto next_value = has_queued_consumers ? gea_cpp_key(array) : gea_cpp_value::missing();']),
        '      mark_dirty_field(array_key);',
        ...(typedOnlyStore
          ? []
          : [
              '      if (has_queued_consumers) {',
              '        (void)(fn__queue(self_value, state, std::string(array_key ? array_key : ""), gea_ir::changeRecord(old_value, next_value)));',
              '      }',
            ]),
        '      return removed;',
        '    };',
      )
    }
    if (beginIter) {
      lines.push(
        '    auto shift_array_item = [this, &mark_dirty_field]<typename Array>(const char *array_key, Array &array) mutable -> typename Array::value_type {',
        '      using Element = typename Array::value_type;',
        '      if (array.empty()) return Element{};',
        ...(typedOnlyStore
          ? []
          : [
              '      auto self_value = this->__gea_ctor_return_value;',
              '      auto state = gea_cpp_key(__gea_global__priv().get(self_value));',
              '      const bool has_queued_consumers = gea_ir::storeHasQueuedConsumers(state, array_key);',
              '      auto old_value = has_queued_consumers ? gea_cpp_key(array) : gea_cpp_value::missing();',
            ]),
        '      Element removed = std::move(array.front());',
        '      array.erase(array.begin());',
        ...(typedOnlyStore ? [] : ['      auto next_value = has_queued_consumers ? gea_cpp_key(array) : gea_cpp_value::missing();']),
        '      mark_dirty_field(array_key);',
        ...(typedOnlyStore
          ? []
          : [
              '      if (has_queued_consumers) {',
              '        (void)(fn__queue(self_value, state, std::string(array_key ? array_key : ""), gea_ir::changeRecord(old_value, next_value)));',
              '      }',
            ]),
        '      return removed;',
        '    };',
      )
    }
  }
  // Open a keyed-list direct-dispatch batch for the duration of this method so
  // per-item writes (e.g. re-filling forecast rows) don't re-run each list's
  // .map on every write. Balanced by storeDirectBatchEnd() at every exit
  // (mutationFooter + flushBeforeReturn) — nested store calls share the
  // outermost batch and it flushes once when that one exits.
  if (!typedOnlyStore) lines.push('    gea_ir::storeDirectBatchBegin();')
  return rewritePrivStateLookups(lines)
}

// When the program carries the hub member block (which also hosts the
// per-instance `_priv` state cache — see cpp-store-hub.ts), the mutation
// helpers read the cached state record instead of re-probing the boxed
// WeakMap on every mutating method call:
//   `auto self_value = <ctor value copy>`  → const ref (no boxed copy; kept
//     because the fn__queue call sites pass it; gea_cpp_value's non-trivial
//     dtor keeps clang from warning where a site no longer uses it)
//   `auto state = key(_priv().get(self_value))` → `auto &state = cached ref`
function rewritePrivStateLookups(lines: string[]): string[] {
  if (!storeHubEmissionEnabled() || !storeDynamicFallbackEnabled()) return lines
  return lines.map((line) => {
    const indent = line.slice(0, line.length - line.trimStart().length)
    const body = line.trim()
    if (body === 'auto self_value = this->__gea_ctor_return_value;') {
      return `${indent}const auto &self_value = this->__gea_ctor_return_value;`
    }
    if (body === 'auto state = gea_cpp_key(__gea_global__priv().get(self_value));') {
      return `${indent}auto &state = (*this).__gea_priv_state_ref();`
    }
    return line
  })
}

function mutationFooter(context: StoreMethodLowerContext): string[] {
  if (!needsMutationHelpers(context)) return []
  return storeDynamicFallbackEnabled() ? ['    flush_dirty_fields();', '    gea_ir::storeDirectBatchEnd();'] : ['    flush_dirty_fields();']
}

function flushBeforeReturn(context: StoreMethodLowerContext, pad: string): string[] {
  if (!needsMutationHelpers(context)) return []
  return storeDynamicFallbackEnabled() ? [`${pad}flush_dirty_fields();`, `${pad}gea_ir::storeDirectBatchEnd();`] : [`${pad}flush_dirty_fields();`]
}

function needsMutationHelpers(context: StoreMethodLowerContext): boolean {
  return (
    context.needsFieldSet ||
    context.needsArrayLengthSet ||
    context.needsArrayItemSet ||
    context.needsArrayPush ||
    context.needsArrayUnshift ||
    context.needsArraySplice ||
    context.needsArrayPop ||
    context.needsArrayShift
  )
}

// Mirror of `tryEmitConstNumericArrayLiteral` from the geatsc emitter
// (`packages/geatsc/src/targets/cpp/emitter/variables.ts`), specialized for
// the IR shape used by store-method body replacement. The plugin's
// `replaceStoreMethodsFromIr` rebuilds method bodies from the captured IR
// rather than from the geatsc-emitted text, so the geatsc-side fast path
// never gets a chance to fire on locals declared inside store methods —
// which is exactly where const piece-shape tables live in apps like
// button-tetris. Without this mirror, the dynamic-vector lowering wins for
// store-method bodies even after the geatsc pass has been taught the
// constexpr form.
function lowerConstLiteralMatrixDecl(name: string, init: GeaIrStoreExpr, pad: string): string[] | null {
  const numeric = readNumericMatrixFromIr(init)
  if (numeric) {
    const elementType = narrowestNumericCppType(numeric.flat)
    if (numeric.shape === '2d') {
      const rowType = `std::array<${elementType}, ${numeric.cols}>`
      const tableType = `std::array<${rowType}, ${numeric.rows}>`
      const rows = numeric.values.map((row) => `${rowType}{${row.map((v) => formatNumericLiteral(v, elementType)).join(', ')}}`).join(', ')
      return [`${pad}static constexpr ${tableType} ${name} = {${rows}};`]
    }
    const items = numeric.values.map((v) => formatNumericLiteral(v, elementType)).join(', ')
    return [`${pad}static constexpr std::array<${elementType}, ${numeric.rows}> ${name} = {${items}};`]
  }
  const strings = readStringMatrixFromIr(init)
  if (strings) {
    // `const char*` is universally consumable: every place a string literal
    // is currently accepted (`std::string` ctor, `gea_cpp_value` ctor, the
    // typed `set_typed_field` `const char*` branch in the record helpers)
    // takes a `const char*` without any extra overload. The pointers
    // themselves live in .rodata; only the array of pointers is
    // materialized.
    if (strings.shape === '2d') {
      const rowType = `std::array<const char*, ${strings.cols}>`
      const tableType = `std::array<${rowType}, ${strings.rows}>`
      const rows = strings.values.map((row) => `${rowType}{${row.map(formatStringLiteral).join(', ')}}`).join(', ')
      return [`${pad}static constexpr ${tableType} ${name} = {${rows}};`]
    }
    const items = strings.values.map(formatStringLiteral).join(', ')
    return [`${pad}static constexpr std::array<const char*, ${strings.rows}> ${name} = {${items}};`]
  }
  const objects = readObjectMatrixFromIr(init)
  if (objects) {
    // Mirror of the geatsc top-level path
    // (`packages/geatsc/src/targets/cpp/emitter/variables.ts`'s object-matrix
    // branch). Lower a homogeneous `const X = [{ a: 1, b: 'x' }, ...]` to a
    // function-local POD struct + `static constexpr std::array<__row, N>`.
    // Per-field element type is the narrowest signed integer for numeric
    // columns, `bool` for boolean columns, `const char*` for string columns.
    // The struct is a literal type so the array sits in .rodata and
    // `tbl[i].field` compiles to one load.
    //
    // The struct also exposes a `__gea_to_value()` member so the
    // `gea_cpp_value(const T&)` catch-all template ctor (which probes for
    // `value.__gea_to_value()`) can box a row when generic emitter paths
    // wrap array access in `static_cast<gea_cpp_value>(__array[i])`. Typed
    // field reads (`row.x`) bypass this and go through the access lambda's
    // typed branch.
    const rowTypeName = `__const_${name}_row`
    const fieldDecls = objects.fields
      .map((f) => `${cppTypeForObjectMatrixFieldFromIr(objects, f)} ${f.name};`)
      .join(' ')
    const setEntries = objects.fields
      .map((f) => `__gea_out.record_set_literal(${formatStringLiteral(f.name)}, gea_cpp_value(this->${f.name}));`)
      .join(' ')
    const toValueBody =
      `gea_cpp_value __gea_out; __gea_out.kind = gea_cpp_value::kind_t::record; ` +
      `__gea_out.entries = std::make_shared<std::vector<std::pair<std::string, gea_cpp_value>>>(); ` +
      `${setEntries} return __gea_out;`
    const rowInits = objects.values
      .map((row) => {
        const cells = objects.fields.map((f) => formatObjectMatrixCellFromIr(row[f.name] as number | string | boolean, f, objects))
        return `${rowTypeName}{${cells.join(', ')}}`
      })
      .join(', ')
    return [
      `${pad}struct ${rowTypeName} { ${fieldDecls} gea_cpp_value __gea_to_value() const { ${toValueBody} } };`,
      `${pad}static constexpr std::array<${rowTypeName}, ${objects.rows}> ${name} = {${rowInits}};`,
    ]
  }
  return null
}

type IrNumericMatrix =
  | { shape: '1d'; rows: number; cols: 0; values: number[]; flat: number[] }
  | { shape: '2d'; rows: number; cols: number; values: number[][]; flat: number[] }

type IrStringMatrix =
  | { shape: '1d'; rows: number; cols: 0; values: string[] }
  | { shape: '2d'; rows: number; cols: number; values: string[][] }

function readNumericMatrixFromIr(expr: GeaIrStoreExpr): IrNumericMatrix | null {
  if (expr.kind !== 'array' || expr.elements.length === 0) return null
  if (expr.elements.every((e) => e.kind === 'array')) {
    const rowExprs = expr.elements as Array<Extract<GeaIrStoreExpr, { kind: 'array' }>>
    const cols = rowExprs[0].elements.length
    if (cols === 0) return null
    const rows: number[][] = []
    const flat: number[] = []
    for (const row of rowExprs) {
      if (row.elements.length !== cols) return null
      const out: number[] = []
      for (const cell of row.elements) {
        const v = readNumericLeafFromIr(cell)
        if (v === null) return null
        out.push(v)
        flat.push(v)
      }
      rows.push(out)
    }
    return { shape: '2d', rows: rowExprs.length, cols, values: rows, flat }
  }
  const flat: number[] = []
  for (const cell of expr.elements) {
    const v = readNumericLeafFromIr(cell)
    if (v === null) return null
    flat.push(v)
  }
  return { shape: '1d', rows: flat.length, cols: 0, values: flat, flat }
}

function readStringMatrixFromIr(expr: GeaIrStoreExpr): IrStringMatrix | null {
  if (expr.kind !== 'array' || expr.elements.length === 0) return null
  if (expr.elements.every((e) => e.kind === 'array')) {
    const rowExprs = expr.elements as Array<Extract<GeaIrStoreExpr, { kind: 'array' }>>
    const cols = rowExprs[0].elements.length
    if (cols === 0) return null
    const rows: string[][] = []
    for (const row of rowExprs) {
      if (row.elements.length !== cols) return null
      const out: string[] = []
      for (const cell of row.elements) {
        if (cell.kind !== 'string') return null
        out.push(cell.value)
      }
      rows.push(out)
    }
    return { shape: '2d', rows: rowExprs.length, cols, values: rows }
  }
  const flat: string[] = []
  for (const cell of expr.elements) {
    if (cell.kind !== 'string') return null
    flat.push(cell.value)
  }
  return { shape: '1d', rows: flat.length, cols: 0, values: flat }
}

interface IrObjectMatrixField {
  name: string
  valueType: 'number' | 'string' | 'boolean'
}

interface IrObjectMatrix {
  shape: '1d'
  rows: number
  fields: IrObjectMatrixField[]
  values: Array<Record<string, number | string | boolean>>
}

function readObjectMatrixFromIr(expr: GeaIrStoreExpr): IrObjectMatrix | null {
  if (expr.kind !== 'array' || expr.elements.length === 0) return null
  if (!expr.elements.every((e) => e.kind === 'object')) return null
  const objExprs = expr.elements as Array<Extract<GeaIrStoreExpr, { kind: 'object' }>>

  const schema: IrObjectMatrixField[] = []
  for (const prop of objExprs[0].fields) {
    const valueType = readObjectFieldValueTypeFromIr(prop.value)
    if (!valueType) return null
    schema.push({ name: prop.name, valueType })
  }
  if (schema.length === 0) return null

  const values: Array<Record<string, number | string | boolean>> = []
  for (const obj of objExprs) {
    if (obj.fields.length !== schema.length) return null
    const row: Record<string, number | string | boolean> = {}
    for (let i = 0; i < schema.length; i++) {
      const prop = obj.fields[i]
      if (prop.name !== schema[i].name) return null
      const value = readObjectFieldValueFromIr(prop.value, schema[i].valueType)
      if (value === null) return null
      row[schema[i].name] = value
    }
    values.push(row)
  }
  return { shape: '1d', rows: values.length, fields: schema, values }
}

function readObjectFieldValueTypeFromIr(expr: GeaIrStoreExpr): 'number' | 'string' | 'boolean' | null {
  if (expr.kind === 'number') return 'number'
  if (expr.kind === 'unary' && (expr.op === '-' || expr.op === '+') && expr.arg.kind === 'number') return 'number'
  if (expr.kind === 'string') return 'string'
  if (expr.kind === 'boolean') return 'boolean'
  return null
}

function readObjectFieldValueFromIr(expr: GeaIrStoreExpr, type: 'number' | 'string' | 'boolean'): number | string | boolean | null {
  if (type === 'number') return readNumericLeafFromIr(expr)
  if (type === 'string') return expr.kind === 'string' ? expr.value : null
  if (type === 'boolean') return expr.kind === 'boolean' ? expr.value : null
  return null
}

function cppTypeForObjectMatrixFieldFromIr(matrix: IrObjectMatrix, field: IrObjectMatrixField): string {
  if (field.valueType === 'string') return 'const char*'
  if (field.valueType === 'boolean') return 'bool'
  const column: number[] = []
  for (const row of matrix.values) column.push(row[field.name] as number)
  return narrowestNumericCppType(column)
}

function formatObjectMatrixCellFromIr(value: number | string | boolean, field: IrObjectMatrixField, matrix: IrObjectMatrix): string {
  if (field.valueType === 'string') return formatStringLiteral(value as string)
  if (field.valueType === 'boolean') return value ? 'true' : 'false'
  return formatNumericLiteral(value as number, cppTypeForObjectMatrixFieldFromIr(matrix, field))
}

function readNumericLeafFromIr(expr: GeaIrStoreExpr): number | null {
  if (expr.kind === 'number') return Number.isFinite(expr.value) ? expr.value : null
  if (expr.kind === 'unary' && (expr.op === '-' || expr.op === '+') && expr.arg.kind === 'number') {
    if (!Number.isFinite(expr.arg.value)) return null
    return expr.op === '-' ? -expr.arg.value : expr.arg.value
  }
  return null
}

function narrowestNumericCppType(values: readonly number[]): string {
  let allIntegers = true
  let min = 0
  let max = 0
  let first = true
  for (const v of values) {
    if (!Number.isFinite(v) || !Number.isInteger(v)) allIntegers = false
    if (first) {
      min = v
      max = v
      first = false
    } else {
      if (v < min) min = v
      if (v > max) max = v
    }
  }
  if (!allIntegers) return 'double'
  if (min >= -128 && max <= 127) return 'std::int8_t'
  if (min >= -32768 && max <= 32767) return 'std::int16_t'
  if (min >= -2147483648 && max <= 2147483647) return 'std::int32_t'
  if (min >= -(2 ** 53) && max <= 2 ** 53) return 'std::int64_t'
  return 'double'
}

function formatNumericLiteral(value: number, cppType: string): string {
  if (cppType === 'double') {
    if (Number.isInteger(value)) return `${value}.0`
    return String(value)
  }
  return String(value)
}

function formatStringLiteral(value: string): string {
  // Mirror of `formatStringLiteral` in
  // `packages/geatsc/src/targets/cpp/emitter/variables.ts`. Escape so the
  // resulting C++ literal parses to the exact same byte sequence; non-ASCII
  // is encoded as UTF-8 byte escapes so the output is encoding-stable.
  let out = '"'
  for (let i = 0; i < value.length; i++) {
    const code = value.charCodeAt(i)
    const ch = value[i]
    if (ch === '\\' || ch === '"') {
      out += '\\' + ch
      continue
    }
    if (code === 0x0a) {
      out += '\\n'
      continue
    }
    if (code === 0x0d) {
      out += '\\r'
      continue
    }
    if (code === 0x09) {
      out += '\\t'
      continue
    }
    if (code < 0x20 || code === 0x7f) {
      out += `\\x${code.toString(16).padStart(2, '0')}" "`
      continue
    }
    if (code <= 0x7f) {
      out += ch
      continue
    }
    const utf8 = utf8EncodeChar(ch)
    for (const byte of utf8) out += `\\x${byte.toString(16).padStart(2, '0')}" "`
  }
  out += '"'
  return out
}

function utf8EncodeChar(ch: string): number[] {
  const bytes: number[] = []
  for (const c of ch) {
    let cp = c.codePointAt(0)!
    if (cp < 0x80) bytes.push(cp)
    else if (cp < 0x800) bytes.push(0xc0 | (cp >> 6), 0x80 | (cp & 0x3f))
    else if (cp < 0x10000) bytes.push(0xe0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f))
    else bytes.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3f), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f))
  }
  return bytes
}
