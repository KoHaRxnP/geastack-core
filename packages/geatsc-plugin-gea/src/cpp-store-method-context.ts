import type { GeaIrConstant, GeaIrStore, GeaIrStoreExpr, GeaIrStoreLocalType } from './types.js'
import type { StoreFieldPlan, StoreMethodPlan } from './cpp-stores.js'
import { sanitizeCppIdentifier } from './utils.js'

export interface StoreMethodLowerContext {
  store: GeaIrStore
  storeFields: StoreFieldPlan[]
  storeMethods: StoreMethodPlan[]
  params: Set<string>
  // Per-param TS type annotation (when present in the source). Used to keep
  // string params from being coerced to number in equality comparisons; an
  // entry only exists when the original source had an explicit `: string` /
  // `: number` / `: boolean` annotation on that parameter.
  paramHints: Map<string, StoreMethodHint>
  topLevelFunctionNames: Map<string, string>
  topLevelValueNames: Map<string, string>
  locals: Set<string>
  constants: Map<string, number | string | boolean | null>
  localTypes: Map<string, GeaIrStoreLocalType>
  tempIndex: number
  arrayElementAliases: Map<string, StoreArrayElementAlias>
  arrayItemAliases: Map<string, string>
  arrayItemFieldAliases: Map<string, string>
  dirtyArrayFields: Set<string>
  needsFieldSet: boolean
  needsArrayLengthSet: boolean
  needsArrayItemSet: boolean
  needsDynamicArrayItemSet: boolean
  needsArrayPush: boolean
  needsArrayUnshift: boolean
  needsArraySplice: boolean
  needsArrayPop: boolean
  needsArrayShift: boolean
}

export type StoreMethodHint = 'any' | 'number' | 'string' | 'boolean'

export interface StoreArrayElementAlias {
  arrayField: string
  itemExpression: string
  indexExpression: string
}

export interface StoreArrayElementAliasField {
  alias: StoreArrayElementAlias
  itemField: string
}

export function arrayElementAliasFieldTarget(
  context: StoreMethodLowerContext,
  expr: GeaIrStoreExpr
): StoreArrayElementAliasField | null {
  if (expr.kind !== 'member' || expr.object.kind !== 'identifier') return null
  const alias = context.arrayElementAliases.get(expr.object.name)
  return alias ? { alias, itemField: expr.property } : null
}

export function arrayItemAliasKey(arrayFieldName: string, indexName: string): string {
  return `${arrayFieldName}\0${indexName}`
}

export function arrayItemFieldAliasKey(arrayFieldName: string, indexName: string, itemFieldName: string): string {
  return `${arrayFieldName}\0${indexName}\0${itemFieldName}`
}

export function constantMap(
  store: GeaIrStore,
  globalConstants: ReadonlyMap<string, GeaIrConstant> = new Map(),
): Map<string, number | string | boolean | null> {
  const map = new Map<string, number | string | boolean | null>()
  for (const constant of globalConstants.values()) addConstant(map, constant)
  for (const constant of store.constants ?? []) {
    addConstant(map, constant)
  }
  return map
}

function addConstant(map: Map<string, number | string | boolean | null>, constant: GeaIrConstant): void {
  if (constant.valueType === 'number') map.set(constant.name, Number(constant.value))
  else if (constant.valueType === 'boolean') map.set(constant.name, constant.value === 'true')
  else if (constant.valueType === 'null') map.set(constant.name, null)
  else if (constant.valueType === 'string') map.set(constant.name, constant.value)
}

export function fieldHint(store: GeaIrStore, fieldName: string): StoreMethodHint {
  const field = store.fields.find((candidate) => candidate.name === fieldName)
  if (field?.shape?.kind === 'literal') {
    if (field.shape.valueType === 'boolean') return 'boolean'
    if (field.shape.valueType === 'number') return 'number'
    if (field.shape.valueType === 'string') return 'string'
  }
  return 'any'
}

export function arrayItemFieldHint(store: GeaIrStore, arrayFieldName: string, itemFieldName: string): StoreMethodHint {
  const field = store.fields.find((candidate) => candidate.name === arrayFieldName)
  const itemField = field?.shape?.kind === 'array' && field.shape.element?.kind === 'object'
    ? field.shape.element.fields.find((candidate) => candidate.name === itemFieldName)
    : undefined
  if (itemField?.shape?.kind === 'literal') {
    if (itemField.shape.valueType === 'boolean') return 'boolean'
    if (itemField.shape.valueType === 'number') return 'number'
    if (itemField.shape.valueType === 'string') return 'string'
  }
  return 'any'
}

export function typedArrayItemFieldAccess(store: GeaIrStore, arrayFieldName: string, itemFieldName: string, itemExpression: string): string | null {
  const field = store.fields.find((candidate) => candidate.name === arrayFieldName)
  if (!field) return null
  const shape = field.shape
  // A named TS element type is already emitted by geatsc as a typed record
  // struct, even when one of its fields is itself an array or record. The Gea
  // plugin only attaches typedStorage metadata to the all-primitive fast path,
  // so requiring typedStorage here incorrectly demoted valid accesses such as
  // `this.pages[i].chapterStarts` to a numeric dynamic-field read.
  if (!field.typedStorage && !(shape?.kind === 'array' && !!shape.elementTypeName)) return null
  const itemField = field.shape?.kind === 'array' && field.shape.element?.kind === 'object'
    ? field.shape.element.fields.find((candidate) => candidate.name === itemFieldName)
    : undefined
  // Named-type enrichment is intentionally partial today: it records the
  // primitive members it can use for plugin fast paths and omits nested
  // arrays/records. That omission must not override TypeScript's validated
  // named record. Only anonymous inline object arrays require the field to be
  // present in the IR shape.
  if (shape?.kind === 'array' && shape.element?.kind === 'object' && !shape.elementTypeName && !itemField) return null
  return `${itemExpression}.${sanitizeCppIdentifier(itemFieldName)}`
}

// C++ element type for a primitive-element local array, or null.
const PRIMITIVE_ARRAY_CPP: Record<'number' | 'string' | 'boolean', string> = {
  number: 'double',
  string: 'std::string',
  boolean: 'bool',
}

export function localArrayTypeRef(context: StoreMethodLowerContext, localName: string): string | null {
  const localType = context.localTypes.get(localName)
  if (localType?.kind !== 'array') return null
  if ('elementPrimitive' in localType) {
    return PRIMITIVE_ARRAY_CPP[localType.elementPrimitive]
  }
  const matchingField = context.store.fields.find((field) => field.typedStorage?.elementTypeName === localType.elementTypeName)
  if (matchingField?.typedStorage) {
    return matchingField.typedStorage.useInterfaceStruct
      ? `::${matchingField.typedStorage.itemType}`
      : matchingField.typedStorage.itemType
  }
  return `::${interfaceItemSymbol(localType.elementTypeName)}`
}

export function localArrayCppType(context: StoreMethodLowerContext, localName: string): string | null {
  const elementType = localArrayTypeRef(context, localName)
  return elementType ? `std::vector<${elementType}>` : null
}

export function nextStoreMethodTemp(context: StoreMethodLowerContext, label: string): string {
  return `__gea_${sanitizeCppIdentifier(`${label}_${context.tempIndex++}`)}`
}

function interfaceItemSymbol(name: string): string {
  return `__gea_type_${sanitizeCppIdentifier(name)}`
}
