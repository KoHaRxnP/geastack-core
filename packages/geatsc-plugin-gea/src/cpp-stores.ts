import fs from 'node:fs'
import { dirname, resolve } from 'node:path'
import ts from 'typescript'
import type { GeaIrComponent, GeaIrConstant, GeaIrStore, GeaIrStoreField, GeaIrStoreValueShape } from './types.js'
import { sanitizeCppIdentifier } from './utils.js'
import { storeDynamicFallbackEnabled, storeHubEmissionEnabled } from './cpp-store-hub.js'

// EXPERIMENTAL (ReactiveComponent): a component that holds its own reactive state
// (`component.reactiveState`) is modeled as its own store — the component class IS
// the store. Synthesizing a GeaIrStore (same className) lets the ONE mounted
// renderer resolve `this.field` reads, `this.method()` events, and reactive deps
// through the exact same store machinery as a declared store; the `selfStore`
// flag routes its field access through typed `store->field.get()` helpers and
// keeps store-method re-lowering away from its already-typed method bodies.
export function synthesizeReactiveComponentStores(components: GeaIrComponent[]): GeaIrStore[] {
  const stores: GeaIrStore[] = []
  for (const component of components) {
    const rs = component.reactiveState
    if (!rs) continue
    // `el` is the mounted-root slot the framework re-adds to a base-less
    // ReactiveComponent that reads `this.el` (see vendor/gea transform.ts). It is
    // NOT user reactive state: it never re-renders and holds a node handle, not a
    // primitive. Keep it out of the store's reactive fields so no typed
    // `store->el.get()` reader/Signal is synthesized for it — the typed mount
    // path binds it directly, and geatsc still emits the
    // plain `gea_cpp_value el` member from the class itself.
    const fields = rs.fields.filter((field) => field.name !== 'el')
    stores.push({
      id: `${component.id}#self-store`,
      module: component.module,
      className: component.exportName,
      runtimeBase: 'compiled',
      fields,
      selfStore: true,
      ...(rs.methods && rs.methods.length > 0 ? { methods: rs.methods } : {}),
      ...(rs.getters && rs.getters.length > 0 ? { getters: rs.getters } : {}),
      ...(rs.constants && rs.constants.length > 0 ? { constants: rs.constants } : {}),
      ...(component.sourceSpan ? { sourceSpan: component.sourceSpan } : {}),
    })
  }
  return stores
}

export interface StoreArrayFieldPlan {
  storeClass: string
  stateType: string
  storeGlobalName?: string
  fieldName: string
  fieldType: string
  readerName: string
  // The element type symbol WITHOUT any namespace qualifier. For the
  // synthesized case it is `<Store>_<field>_item`; for the interface-reuse
  // case it is `__gea_type_<Name>`. Used to build the per-item reader fn name
  // (`read_<itemType>`) — which must NOT contain `::`.
  itemType: string
  // The element type as referenced at C++ type-use sites inside the gea_ir
  // helper blocks. For interface reuse this is `::__gea_type_<Name>` (geatsc
  // emits it at global scope; the gea_ir blocks are nested namespaces). For
  // synthesized structs it is the bare `<Store>_<field>_item` (defined in the
  // same gea_ir block).
  itemTypeRef: string
  // When true the array reuses geatsc's `::__gea_type_<Name>` struct.
  useInterfaceStruct: boolean
  itemFields: GeaIrStoreField[]
  // When true, the array is produced by a zero-arg getter rather than a stored
  // field — the source is read via `typed_store->${fieldName}()` (a method
  // call) and reactivity binds to `reactiveDeps` (the getter's `this.<field>`
  // reads) instead of a stored field named `fieldName`.
  isGetter?: boolean
  reactiveDeps?: string[]
  // Gate for the typed-hub bind fast path (mirrors StoreFieldPlan).
  storeRuntimeBase?: 'compiled' | 'lean'
  storeIsSelfStore?: boolean
}

export interface StoreFieldReaderPlan {
  storeClass: string
  stateType: string
  fieldName: string
  fieldType: string
  readerName: string
  field: GeaIrStoreField
  shape: GeaIrStoreValueShape
}

export interface StoreFieldPlan {
  storeClass: string
  stateType: string
  fieldName: string
  fieldType: string
  readerName: string | null
  field: GeaIrStoreField
  shape: GeaIrStoreValueShape | null
  reader: StoreFieldReaderPlan | null
  // Name of the exported singleton instance for this store (e.g. `store` for
  // `export const store = new ClickerStore()`). Used to disambiguate field
  // lookups when several bundled stores declare the same field name — the
  // receiver in `store.status` picks the store whose instance is `store`.
  storeGlobalName?: string
  // Gate for the typed-hub bind fast path: only compiled-base regular stores
  // carry write-path hub notification (lean stores keep the dynamic channel).
  storeRuntimeBase?: 'compiled' | 'lean'
  storeIsSelfStore?: boolean
  // Primitive zero-argument getter exposed as a typed field-like read to
  // template lowering. The value is recomputed with `store->name()` and the
  // binding subscribes to the getter's underlying reactive field reads.
  isGetter?: boolean
  reactiveDeps?: string[]
}

export interface StoreMethodPlan {
  storeClass: string
  stateType: string
  storeGlobalName?: string
  globalAccess?: 'shared_ptr' | 'reference'
  methodName: string
  params: Array<{ name: string; valueType?: 'string' | 'number' | 'boolean' }>
}

// Forward declarations for the interface-reuse typed-item helpers, emitted at
// the TOP of the gea_ir store block (before `storeRuntimeSource`). Reason:
// `storeVectorItemPropertySet` (in storeRuntimeSource) makes a
// template-dependent call `set_typed_field(slot, ...)`. For synthesized item
// structs `slot` is a `gea_ir::X_item`, so ADL finds `gea_ir::set_typed_field`.
// For interface reuse `slot` is `::__gea_type_<Name>` (global scope), so ADL
// looks only in the global namespace and misses the gea_ir overload. A
// forward declaration here makes the name visible to ordinary lookup at the
// template's definition point (satisfying C++ two-phase lookup); the full
// definitions follow in `generateTypedItemHelpers`. The structs are already
// complete at this point (this block is emitted after geatsc's record-alias
// type definitions). Synthesized item types are unaffected (ADL already
// resolves them) so this only emits for interface-reuse fields.
export function generateInterfaceItemHelperForwardDeclarations(stores: GeaIrStore[]): string[] {
  const lines: string[] = []
  const seen = new Set<string>()
  for (const store of stores) {
    for (const arrayField of collectStoreArrayFields([store])) {
      if (!arrayField.useInterfaceStruct) continue
      if (seen.has(arrayField.itemType)) continue
      seen.add(arrayField.itemType)
      const ref = arrayField.itemTypeRef
      lines.push(
        `template <typename V> void set_typed_field(${ref} &item, const char *name, const V &value);`,
        `double read_number_field(const ${ref} &item, const char *name);`,
        `std::string read_string_field(const ${ref} &item, const char *name);`,
        `bool read_boolean_field(const ${ref} &item, const char *name);`,
      )
      if (storeDynamicFallbackEnabled()) lines.push(`gea_cpp_value gea_cpp_key(const std::vector<${ref}> &value);`)
    }
  }
  if (lines.length > 0) {
    // Declaring `gea_cpp_key(const std::vector<…>&)` inside `gea_ir` hides the
    // global `::gea_cpp_key` overload set for unqualified calls in this
    // namespace (`gea_cpp_key(field)` on a std::string/double field). Re-expose
    // the global set with a using-declaration so both resolve.
    lines.push('using ::gea_cpp_key;', '')
  }
  return lines
}

export function generateStoreDeclarations(stores: GeaIrStore[]): string[] {
  // The synthesised typed item structs (emitted via `cppTypeForShape`) carry
  // converting constructors that call `read_number_field` / `read_string_field`
  // / `read_boolean_field`. Those primitives live in
  // `generateStoreStateReaders`'s output, which is normally emitted *after*
  // the structs in cpp-ir.ts's lines list. To keep the lookup order valid
  // inside class bodies, we hoist the dynamic-record readers up here as a
  // preamble (and skip emitting them again from the readers stage via
  // `dynamicRecordReadersAlreadyEmitted`).
  const lines: string[] = stores.length > 0 && storeDynamicFallbackEnabled() ? [...dynamicRecordReaderLines()] : []
  for (const store of stores) {
    const declarations: string[] = []
    // cppTypeForField SYNTHESIZES the per-field item structs
    // (`<Store>_<field>_item`) into `declarations` as a side effect — those
    // back the typed vector storage and must still be emitted. The
    // `<Store>_state` snapshot struct that used to wrap these members (and
    // its `read_<Store>_state` readers + `for_each_*` helpers) had NO
    // remaining call sites — every live binding reads through the per-field
    // `read_<Store>_field_*` readers — so the dead mirror (whose array
    // members also degraded to boxed vectors whenever the initializer shape
    // was empty) is no longer emitted.
    for (const field of store.fields) {
      cppTypeForField(field, `${sanitizeCppIdentifier(store.className)}_${sanitizeCppIdentifier(field.name)}`, declarations)
    }
    lines.push(...declarations)
    lines.push(...generateTypedItemHelpers(store))
  }
  return lines
}

function dynamicRecordReaderLines(): string[] {
  return [
    'inline double read_number_field(const gea_cpp_value &value, const char *name) {',
    '  auto field = value.record_get_literal(name);',
    '  return field.is_nullish() ? 0.0 : static_cast<double>(field);',
    '}',
    '',
    'inline bool read_boolean_field(const gea_cpp_value &value, const char *name) {',
    '  auto field = value.record_get_literal(name);',
    '  return field.is_nullish() ? false : static_cast<bool>(field);',
    '}',
    '',
    'inline std::string read_string_field(const gea_cpp_value &value, const char *name) {',
    '  auto field = value.record_get_literal(name);',
    '  return field.is_nullish() ? std::string() : static_cast<std::string>(field);',
    '}',
    '',
  ]
}

// Helpers that bridge a typed item struct (synthesized by `cppTypeForShape`
// for `Array<{...primitives...}>` shapes in store fields) to and from
// `gea_cpp_value`. These let typed-storage stores expose their array fields
// across the dynamic boundary (`__gea_to_value` getter, observers reading
// `store.balls`) without paying per-property `record_get_literal` linear
// scans on every access. The per-property reads in lowered method bodies
// resolve through typed overloads of `read_number_field` / `read_string_field`
// emitted below; the geatsc emitter's fallback paths that call
// `slot.record_get_literal(...)` directly on a slot resolve through the
// duck-type member methods (also emitted on the item struct) so unmodified
// fallback code keeps compiling against typed storage.
function generateTypedItemHelpers(store: GeaIrStore): string[] {
  const lines: string[] = []
  // Two store fields can share the same interface element type (e.g.
  // `hours: Forecast[]` and `days: Forecast[]`). Each would otherwise emit a
  // duplicate set of `__gea_type_Forecast` helpers → ODR/redefinition errors.
  // Synthesized item types are unique per field so they never collide.
  const emittedItemTypes = new Set<string>()
  for (const arrayField of collectStoreArrayFields([store])) {
    if (emittedItemTypes.has(arrayField.itemType)) continue
    emittedItemTypes.add(arrayField.itemType)
    // Type-use spelling: `::__gea_type_X` for interface reuse (geatsc emits it
    // at global scope) or bare `<Store>_<field>_item` for synthesized structs
    // (defined in this same gea_ir block).
    const itemTypeRef = arrayField.itemTypeRef
    const useInterfaceStruct = arrayField.useInterfaceStruct
    const fields = arrayField.itemFields

    if (storeDynamicFallbackEnabled() && !useInterfaceStruct) {
      // gea_cpp_key(const X_item &): typed -> dynamic record value. SKIPPED for
      // the interface case — geatsc already emits a global
      // `gea_cpp_key(const __gea_type_X&)` overload; emitting another here
      // would be an ODR/ambiguity hazard.
      const setLines = fields
        .map((f) => recordSetLineForField('__gea_out', `value.${sanitizeCppIdentifier(f.name)}`, f))
        .filter((l): l is string => l !== null)
      lines.push(
        `inline gea_cpp_value gea_cpp_key(const ${itemTypeRef} &value) {`,
        '  gea_cpp_value __gea_out;',
        '  __gea_out.kind = gea_cpp_value::kind_t::record;',
        '  __gea_out.entries = std::make_shared<std::vector<std::pair<std::string, gea_cpp_value>>>();',
        ...setLines.map((l) => `  ${l}`),
        '  return __gea_out;',
        '}',
        '',
      )
    }

    if (storeDynamicFallbackEnabled()) {
      // gea_cpp_key(const std::vector<X> &): geatsc has NO vector overload for
      // its interface struct, so we always emit this in dynamic-fallback mode.
      // Its body pushes `gea_cpp_key(item)` which, in the interface case,
      // resolves to geatsc's global single-item overload.
      lines.push(
        `inline gea_cpp_value gea_cpp_key(const std::vector<${itemTypeRef}> &value) {`,
        '  gea_cpp_value __gea_out;',
        '  __gea_out.kind = gea_cpp_value::kind_t::array_value;',
        '  __gea_out.array_items = std::make_shared<std::vector<gea_cpp_value>>();',
        '  __gea_out.array_items->reserve(value.size());',
        '  for (const auto &__gea_item : value) __gea_out.array_items->push_back(gea_cpp_key(__gea_item));',
        '  __gea_out.object_id = reinterpret_cast<std::uintptr_t>(&value);',
        '  return __gea_out;',
        '}',
        '',
      )
    }

    // gea_ir_set_typed_field(X &, const char *name, const auto &value):
    // direct field write dispatched at runtime by name. Used by the
    // typed-storage `set_array_item_field` lambda's `if constexpr` branch. For
    // the interface case each branch ALSO sets `__gea_has_<field> = true` so
    // the geatsc struct's `__gea_to_value`/`gea_cpp_key` don't silently emit an
    // empty record.
    const branches = fields
      .map((f) => typedFieldSetBranch('item', f, useInterfaceStruct))
      .filter((l): l is string => l !== null)
    lines.push(
      `template <typename V>`,
      `inline void set_typed_field(${itemTypeRef} &item, const char *name, const V &value) {`,
      ...branches.map((l) => `  ${l}`),
      '}',
      '',
    )

    // typed-element overload of read_number_field/read_string_field so
    // method-body lowering can keep emitting `read_number_field(item, "x")`
    // even when `item` is a typed struct. Resolution by argument type — the
    // existing `gea_cpp_value` overloads stay in place for the dynamic branch.
    const numberBranches = fields
      .filter((f) => fieldShape(f)?.kind === 'literal' && (fieldShape(f) as { valueType?: string }).valueType === 'number')
      .map((f) => `  if (std::strcmp(name, ${JSON.stringify(f.name)}) == 0) return item.${sanitizeCppIdentifier(f.name)};`)
    if (numberBranches.length > 0) {
      lines.push(
        `inline double read_number_field(const ${itemTypeRef} &item, const char *name) {`,
        ...numberBranches,
        '  return 0.0;',
        '}',
        '',
      )
    }
    const stringBranches = fields
      .filter((f) => fieldShape(f)?.kind === 'literal' && (fieldShape(f) as { valueType?: string }).valueType === 'string')
      .map((f) => `  if (std::strcmp(name, ${JSON.stringify(f.name)}) == 0) return item.${sanitizeCppIdentifier(f.name)};`)
    if (stringBranches.length > 0) {
      lines.push(
        `inline std::string read_string_field(const ${itemTypeRef} &item, const char *name) {`,
        ...stringBranches,
        '  return std::string();',
        '}',
        '',
      )
    }
    const booleanBranches = fields
      .filter((f) => fieldShape(f)?.kind === 'literal' && (fieldShape(f) as { valueType?: string }).valueType === 'boolean')
      .map((f) => `  if (std::strcmp(name, ${JSON.stringify(f.name)}) == 0) return item.${sanitizeCppIdentifier(f.name)};`)
    if (booleanBranches.length > 0) {
      lines.push(
        `inline bool read_boolean_field(const ${itemTypeRef} &item, const char *name) {`,
        ...booleanBranches,
        '  return false;',
        '}',
        '',
      )
    }
  }
  return lines
}

function typedFieldSetBranch(target: string, field: GeaIrStoreField, useInterfaceStruct: boolean): string | null {
  // The outer `set_typed_field<V>` is one template instantiation per
  // value-type V the call site passes. Different properties on the same
  // item type may have different shapes (e.g. `x: number, color: string`);
  // the runtime `strcmp` dispatch picks the branch, but every branch's body
  // is compiled for every V. We guard each branch's assignment with
  // `if constexpr` so V-incompatible casts (`static_cast<double>` on a
  // `std::string` V) are skipped at compile time and don't fail the
  // instantiation.
  //
  // For the interface-reuse case the geatsc struct carries per-field presence
  // flags (`__gea_has_<field>`) that `__gea_to_value`/`gea_cpp_key` consult.
  // Set the flag alongside every write or the dynamic surface silently empties.
  const shape = fieldShape(field)
  if (shape?.kind !== 'literal') return null
  const name = sanitizeCppIdentifier(field.name)
  const key = JSON.stringify(field.name)
  const presence = useInterfaceStruct ? ` ${target}.__gea_has_${name} = true;` : ''
  if (shape.valueType === 'number') {
    return storeDynamicFallbackEnabled()
      ? `if (std::strcmp(name, ${key}) == 0) { if constexpr (std::is_arithmetic_v<std::decay_t<V>>) ${target}.${name} = static_cast<double>(value); else if constexpr (std::is_same_v<std::decay_t<V>, gea_cpp_value>) ${target}.${name} = gea::runtime::coerce::to_number(value);${presence} return; }`
      : `if (std::strcmp(name, ${key}) == 0) { if constexpr (std::is_arithmetic_v<std::decay_t<V>>) ${target}.${name} = static_cast<double>(value);${presence} return; }`
  }
  if (shape.valueType === 'boolean') {
    return storeDynamicFallbackEnabled()
      ? `if (std::strcmp(name, ${key}) == 0) { if constexpr (std::is_arithmetic_v<std::decay_t<V>> || std::is_same_v<std::decay_t<V>, bool>) ${target}.${name} = static_cast<bool>(value); else if constexpr (std::is_same_v<std::decay_t<V>, gea_cpp_value>) ${target}.${name} = gea::runtime::coerce::to_boolean(value);${presence} return; }`
      : `if (std::strcmp(name, ${key}) == 0) { if constexpr (std::is_arithmetic_v<std::decay_t<V>> || std::is_same_v<std::decay_t<V>, bool>) ${target}.${name} = static_cast<bool>(value);${presence} return; }`
  }
  if (shape.valueType === 'string') {
    return storeDynamicFallbackEnabled()
      ? `if (std::strcmp(name, ${key}) == 0) { if constexpr (std::is_same_v<std::decay_t<V>, std::string>) ${target}.${name} = value; else if constexpr (std::is_same_v<std::decay_t<V>, const char *> || std::is_same_v<std::decay_t<V>, char *>) ${target}.${name} = std::string(value); else if constexpr (std::is_same_v<std::decay_t<V>, gea_cpp_value>) ${target}.${name} = gea_cpp_to_string(value);${presence} return; }`
      : `if (std::strcmp(name, ${key}) == 0) { if constexpr (std::is_same_v<std::decay_t<V>, std::string>) ${target}.${name} = value; else if constexpr (std::is_same_v<std::decay_t<V>, const char *> || std::is_same_v<std::decay_t<V>, char *>) ${target}.${name} = std::string(value);${presence} return; }`
  }
  return null
}

function recordSetLineForField(target: string, sourceExpr: string, field: GeaIrStoreField): string | null {
  const shape = fieldShape(field)
  if (shape?.kind !== 'literal') return null
  if (shape.valueType === 'number') {
    return `${target}.record_set_literal(${JSON.stringify(field.name)}, gea_cpp_value(${sourceExpr}));`
  }
  if (shape.valueType === 'boolean') {
    return `${target}.record_set_literal(${JSON.stringify(field.name)}, gea_cpp_value(${sourceExpr}));`
  }
  if (shape.valueType === 'string') {
    return `${target}.record_set_literal(${JSON.stringify(field.name)}, gea_cpp_value(${sourceExpr}));`
  }
  return null
}

export function generateStoreStateReaders(stores: GeaIrStore[]): string[] {
  // The dynamic-record `read_*_field` helpers are now hoisted into
  // `generateStoreDeclarations` so the typed item structs (which invoke
  // them in their gea_cpp_value-converting ctor) see them at name lookup.
  // This stage adds the per-store array-element / state readers that
  // depend on the typed structs already being declared.
  const lines: string[] = []
  for (const store of stores) lines.push(...generateStoreStateReader(store))
  return lines
}

export function collectStoreArrayFields(stores: GeaIrStore[]): StoreArrayFieldPlan[] {
  const fields: StoreArrayFieldPlan[] = []
  for (const store of stores) {
    const storeClass = sanitizeCppIdentifier(store.className)
    const storeGlobalName = storeInstanceGlobalName(store)
    for (const field of store.fields) {
      const shape = fieldShape(field)
      if (!isArrayObjectShape(shape)) continue
      const fieldName = sanitizeCppIdentifier(field.name)
      // Prefer the metadata computed once by `attachTypedStorageMetadata`;
      // fall back to re-deriving the predicate from the shape so callers that
      // run before metadata attach (or in isolation) stay correct.
      const useInterfaceStruct = field.typedStorage
        ? field.typedStorage.useInterfaceStruct === true
        : arrayReusesInterfaceStruct(shape)
      const itemType =
        field.typedStorage?.itemType ??
        (useInterfaceStruct ? interfaceItemSymbol(shape.elementTypeName as string) : `${storeClass}_${fieldName}_item`)
      // geatsc emits `__gea_type_<Name>` at GLOBAL scope (the geatsc cpp target
      // here does not wrap its program in an anonymous namespace). The gea_ir
      // helper blocks are nested namespaces, so the interface struct is named
      // with a leading `::`. Synthesized item structs live in `namespace gea_ir`
      // and are named bare.
      const itemTypeRef = useInterfaceStruct ? `::${itemType}` : itemType
      fields.push({
        storeClass,
        stateType: `${storeClass}_state`,
        storeGlobalName,
        fieldName,
        fieldType: `std::vector<${itemTypeRef}>`,
        readerName: storeFieldReaderName(storeClass, fieldName),
        itemType,
        itemTypeRef,
        useInterfaceStruct,
        itemFields: shape.element.fields,
        storeRuntimeBase: store.runtimeBase,
        storeIsSelfStore: !!store.selfStore,
      })
    }
  }
  return fields
}

// Array-returning getters as keyed-list sources. Unlike fields, a getter is
// NOT stored — it is recomputed by calling `typed_store->${name}()`. We reuse
// the element type of an existing array-of-objects field the getter derives
// from (matched by return-type interface name, else by a dependency field), so
// no new item struct needs synthesizing and the row codegen is identical to a
// field-backed list. Getters whose element type can't be resolved to an
// existing field are skipped (the non-reactive-list diagnostic still flags
// them). NOT included in `collectStoreArrayFields` — that drives typed-vector
// STORAGE generation, which a getter must never get.
export function collectStoreArrayGetters(stores: GeaIrStore[]): StoreArrayFieldPlan[] {
  const plans: StoreArrayFieldPlan[] = []
  for (const store of stores) {
    const fieldPlans = collectStoreArrayFields([store])
    if (fieldPlans.length === 0) continue
    for (const getter of store.getters ?? []) {
      if (!getter.returnsArray) continue
      const source = getterSourceFieldPlan(getter, fieldPlans)
      if (!source) continue
      plans.push({
        ...source,
        fieldName: sanitizeCppIdentifier(getter.name),
        isGetter: true,
        reactiveDeps: getter.deps.map((dep) => sanitizeCppIdentifier(dep)),
      })
    }
  }
  return plans
}

// All keyed-list array sources: stored array-of-objects fields plus
// array-returning getters. Used by the keyed-list mount + mount-decision
// passes (NOT by storage/reader generation).
export function collectStoreArraySources(stores: GeaIrStore[]): StoreArrayFieldPlan[] {
  return [...collectStoreArrayFields(stores), ...collectStoreArrayGetters(stores)]
}

function getterSourceFieldPlan(
  getter: NonNullable<GeaIrStore['getters']>[number],
  fieldPlans: StoreArrayFieldPlan[],
): StoreArrayFieldPlan | null {
  if (getter.elementTypeName) {
    const symbol = interfaceItemSymbol(getter.elementTypeName)
    const byType = fieldPlans.find((plan) => plan.useInterfaceStruct && plan.itemType === symbol)
    if (byType) return byType
  }
  for (const dep of getter.deps) {
    const byDep = fieldPlans.find((plan) => plan.fieldName === sanitizeCppIdentifier(dep))
    if (byDep) return byDep
  }
  return null
}

// Single-source-of-truth IR pre-pass: walks the bundle's stores, decides
// which fields qualify for typed-vector storage, and writes the metadata
// onto the IR's `GeaIrStoreField.typedStorage` slot. Every later pass
// (`applyTypedArrayStorage`, `replaceStoreMethodsFromIr`'s typed-field
// helpers, `generateStoreDeclarations`) reads from there directly instead
// of re-deriving the predicate. Mutates `bundle.stores[*].fields[*]` in
// place — safe because the IR is owned by the plugin for the lifetime of the
// generated-source transform.
export function attachTypedStorageMetadata(stores: GeaIrStore[]): void {
  for (const store of stores) {
    const storeClass = sanitizeCppIdentifier(store.className)
    for (const field of store.fields) {
      const shape = fieldShape(field)
      if (!isArrayObjectShape(shape)) {
        delete field.typedStorage
        continue
      }
      const fieldName = sanitizeCppIdentifier(field.name)
      if (arrayReusesInterfaceStruct(shape)) {
        const elementTypeName = shape.elementTypeName as string
        field.typedStorage = {
          itemType: interfaceItemSymbol(elementTypeName),
          readerName: storeFieldReaderName(storeClass, fieldName),
          useInterfaceStruct: true,
          elementTypeName,
        }
      } else {
        field.typedStorage = {
          itemType: `${storeClass}_${fieldName}_item`,
          readerName: storeFieldReaderName(storeClass, fieldName),
          useInterfaceStruct: false,
        }
      }
    }
  }
}

export function enrichConstantInitializerShapes(
  stores: GeaIrStore[],
  constants: ReadonlyMap<string, GeaIrConstant>,
  sourceFiles: readonly string[] = [],
): void {
  const constantShapes = sourceFiles.length > 0 ? collectConstInitializerShapes(sourceFiles, constants) : new Map<string, GeaIrStoreValueShape>()
  const typedFieldShapes = sourceFiles.length > 0 ? collectStoreFieldTypeShapes(sourceFiles, stores) : new Map<string, GeaIrStoreValueShape>()
  for (const store of stores) {
    for (const field of store.fields) {
      const typedShape = typedFieldShapes.get(`${sanitizeCppIdentifier(store.className)}.${sanitizeCppIdentifier(field.name)}`)
      if (field.shape) {
        if (shouldUseTypedFieldShape(field.shape, typedShape)) field.shape = typedShape
        continue
      }
      const shape =
        typedShape ??
        shapeFromConstantInitializer(field.initializer, constants, constantShapes)
      if (shape) field.shape = shape
    }
  }
}

function shouldUseTypedFieldShape(existing: GeaIrStoreValueShape, typed: GeaIrStoreValueShape | undefined): typed is GeaIrStoreValueShape {
  return (
    existing.kind === 'array' &&
    !existing.element &&
    !existing.elementTypeName &&
    typed?.kind === 'array' &&
    (!!typed.element || !!typed.elementTypeName)
  )
}

export function enrichNamedArrayShapesFromSource(source: string, stores: GeaIrStore[]): void {
  const fieldsByItemType = new Map<string, GeaIrStoreField[] | null>()
  for (const store of stores) {
    for (const field of store.fields) {
      const shape = fieldShape(field)
      if (shape?.kind !== 'array' || shape.element || !shape.elementTypeName) continue
      const itemType = interfaceItemSymbol(shape.elementTypeName)
      let fields = fieldsByItemType.get(itemType)
      if (fields === undefined) {
        fields = recordAliasStructFields(source, itemType)
        fieldsByItemType.set(itemType, fields.length > 0 ? fields : null)
      }
      if (!fields || fields.length === 0) continue
      field.shape = {
        ...shape,
        element: {
          kind: 'object',
          fields: fields.map((itemField) => ({
            name: itemField.name,
            ...(itemField.shape ? { shape: itemField.shape } : {}),
          })),
        },
      }
    }
  }
}

export function enrichNamedArrayShapesFromTypeSources(entryFiles: string[], stores: GeaIrStore[]): void {
  const typeNames = new Set<string>()
  for (const store of stores) {
    for (const field of store.fields) {
      const shape = fieldShape(field)
      if (shape?.kind !== 'array' || shape.element || !shape.elementTypeName) continue
      typeNames.add(shape.elementTypeName)
    }
  }
  if (typeNames.size === 0) return

  const fieldsByTypeName = typeFieldsFromReachableSources(entryFiles, typeNames)
  for (const store of stores) {
    for (const field of store.fields) {
      const shape = fieldShape(field)
      if (shape?.kind !== 'array' || shape.element || !shape.elementTypeName) continue
      const fields = fieldsByTypeName.get(shape.elementTypeName)
      if (!fields || fields.length === 0) continue
      field.shape = {
        ...shape,
        element: {
          kind: 'object',
          fields: fields.map((itemField) => ({
            name: itemField.name,
            ...(itemField.shape ? { shape: itemField.shape } : {}),
          })),
        },
      }
    }
  }
}

function typeFieldsFromReachableSources(entryFiles: string[], typeNames: ReadonlySet<string>): Map<string, GeaIrStoreField[]> {
  const out = new Map<string, GeaIrStoreField[]>()
  for (const file of reachableLocalSourceFiles(entryFiles)) {
    if (out.size === typeNames.size) break
    let sourceText: string
    try {
      sourceText = fs.readFileSync(file, 'utf8')
    } catch {
      continue
    }
    const sourceFile = ts.createSourceFile(
      file,
      sourceText,
      ts.ScriptTarget.Latest,
      true,
      file.endsWith('.tsx') ? ts.ScriptKind.TSX : ts.ScriptKind.TS,
    )
    for (const statement of sourceFile.statements) {
      if (ts.isInterfaceDeclaration(statement) && typeNames.has(statement.name.text)) {
        const fields = primitiveObjectFieldsFromMembers(statement.members)
        if (fields) out.set(statement.name.text, fields)
        continue
      }
      if (ts.isTypeAliasDeclaration(statement) && typeNames.has(statement.name.text) && ts.isTypeLiteralNode(statement.type)) {
        const fields = primitiveObjectFieldsFromMembers(statement.type.members)
        if (fields) out.set(statement.name.text, fields)
      }
    }
  }
  return out
}

function primitiveObjectFieldsFromMembers(members: ts.NodeArray<ts.TypeElement>): GeaIrStoreField[] | null {
  const fields: GeaIrStoreField[] = []
  for (const member of members) {
    if (!ts.isPropertySignature(member) || !member.type) return null
    const name = propertyNameText(member.name)
    const valueType = literalValueTypeFromTypeNode(member.type)
    if (!name || !valueType) return null
    fields.push({ name, shape: { kind: 'literal', valueType } })
  }
  return fields
}

function propertyNameText(name: ts.PropertyName): string | null {
  if (ts.isIdentifier(name) || ts.isStringLiteral(name) || ts.isNumericLiteral(name)) return name.text
  return null
}

function literalValueTypeFromTypeNode(type: ts.TypeNode): 'string' | 'number' | 'boolean' | 'null' | null {
  if (type.kind === ts.SyntaxKind.StringKeyword) return 'string'
  if (type.kind === ts.SyntaxKind.NumberKeyword) return 'number'
  if (type.kind === ts.SyntaxKind.BooleanKeyword) return 'boolean'
  if (
    ts.isLiteralTypeNode(type) &&
    type.literal.kind === ts.SyntaxKind.NullKeyword
  ) {
    return 'null'
  }
  return null
}

// Fix 3 — SINGLE SOURCE OF TRUTH reconciliation. `attachTypedStorageMetadata`
// (run at IR-load time, before any C++ exists) optimistically opts a store
// array into reusing geatsc's `::__gea_type_<Name>` struct based on the
// INITIALIZER shape. geatsc independently decides whether to EMIT that struct
// based on the DECLARED interface (collector.ts `recordAliasIsAllPrimitive`).
// These two predicates can diverge — e.g. the declared interface has a
// non-primitive field the initializer omits, so the plugin opts in but geatsc
// refuses to emit, leaving the plugin referencing a struct that does not exist
// (dangling reference → compile break).
//
// This pass closes the gap by making emission ⇔ reuse EXACT: once geatsc has
// produced the program source, we scan it for the struct geatsc actually
// emitted (`struct __gea_type_<Name>`). Any field still flagged
// `useInterfaceStruct` whose struct is absent is downgraded to the synthesized
// `<Store>_<field>_item` path — so the plugin NEVER references a struct geatsc
// did not emit. Call this on the geatsc-generated source BEFORE any store
// source generation that reads `field.typedStorage`.
export function reconcileInterfaceStructReuse(source: string, stores: GeaIrStore[]): void {
  for (const store of stores) {
    const storeClass = sanitizeCppIdentifier(store.className)
    for (const field of store.fields) {
      const typed = field.typedStorage
      if (!typed || typed.useInterfaceStruct !== true) continue
      if (geatscEmittedRecordAliasStruct(source, typed.itemType)) continue
      // geatsc did not emit `struct <itemType>` → fall back to the synthesized
      // per-field struct so storage, readers, and helpers all stay self-hosted
      // in `namespace gea_ir` and reference no undefined global type.
      const fieldName = sanitizeCppIdentifier(field.name)
      field.typedStorage = {
        itemType: `${storeClass}_${fieldName}_item`,
        readerName: storeFieldReaderName(storeClass, fieldName),
        useInterfaceStruct: false,
      }
    }
  }
}

// True when the geatsc-generated source defines `struct <typeName> { … }` at
// (global) scope. Matches the exact spelling the collector emits
// (`emitRecordAliasType`: `struct __gea_type_<safeIdent(Name)> {`). If the
// plugin's `sanitizeCppIdentifier`-derived `typeName` and geatsc's
// `safeIdent`-derived struct name diverge, the struct is simply "not found"
// here and the field is conservatively downgraded — so this also guards the
// symbol-name spelling divergence between the two sanitizers.
function geatscEmittedRecordAliasStruct(source: string, typeName: string): boolean {
  return new RegExp(`\\bstruct\\s+${escapeRegExpLiteral(typeName)}\\s*\\{`).test(source)
}

function recordAliasStructFields(source: string, typeName: string): GeaIrStoreField[] {
  const body = recordAliasStructBody(source, typeName)
  if (!body) return []
  const fields: GeaIrStoreField[] = []
  const pattern = /^\s*(std::string|double|bool)\s+([A-Za-z_][A-Za-z0-9_]*)\s*;/gm
  let match: RegExpExecArray | null
  while ((match = pattern.exec(body))) {
    const [, cppType, name] = match
    if (name.startsWith('__gea_')) continue
    const valueType = cppType === 'std::string' ? 'string' : cppType === 'bool' ? 'boolean' : 'number'
    fields.push({ name, shape: { kind: 'literal', valueType } })
  }
  return fields
}

function recordAliasStructBody(source: string, typeName: string): string | null {
  const start = new RegExp(`\\bstruct\\s+${escapeRegExpLiteral(typeName)}\\s*\\{`).exec(source)
  if (!start) return null
  const bodyStart = start.index + start[0].length
  const end = source.indexOf('\n};', bodyStart)
  return end === -1 ? null : source.slice(bodyStart, end)
}

function escapeRegExpLiteral(value: string): string {
  return value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
}

export function collectStoreFields(stores: GeaIrStore[]): StoreFieldPlan[] {
  const fields: StoreFieldPlan[] = []
  for (const store of stores) {
    const storeClass = sanitizeCppIdentifier(store.className)
    const storeGlobalName = storeInstanceGlobalName(store)
    for (const field of store.fields) {
      const fieldName = sanitizeCppIdentifier(field.name)
      const reader = storeFieldReaderPlan(store, field)
      fields.push({
        storeClass,
        stateType: `${storeClass}_state`,
        fieldName,
        fieldType: cppTypeForField(field, `${storeClass}_${fieldName}`, []),
        readerName: reader?.readerName ?? null,
        field,
        shape: fieldShape(field),
        reader,
        storeGlobalName,
        storeRuntimeBase: store.runtimeBase,
        storeIsSelfStore: !!store.selfStore,
      })
    }

    // A Store getter is a computed property in TypeScript, but geatsc emits it
    // as a zero-argument C++ member function. Treat primitive getters as typed
    // field-like template inputs so `{reader.progressPercent}` becomes
    // `reader->progressPercent()` instead of a dynamic record lookup for a
    // property that is intentionally not stored in the record mirror.
    //
    // Array getters already have a dedicated typed keyed-list path. Self-store
    // component getters are inlined before this collector runs. A regular Store
    // getter needs its exported singleton so the renderer can retain the typed
    // shared_ptr all the way to the call.
    if (!store.selfStore && storeGlobalName) {
      for (const getter of store.getters ?? []) {
        const shape = primitiveGetterShape(store, getter)
        if (getter.returnsArray || shape?.kind !== 'literal') continue
        if (shape.valueType !== 'string' && shape.valueType !== 'number' && shape.valueType !== 'boolean') continue
        const fieldName = sanitizeCppIdentifier(getter.name)
        const field: GeaIrStoreField = { name: getter.name, shape }
        const fieldType = cppTypeForField(field, `${storeClass}_${fieldName}`, [])
        const fallback =
          shape.valueType === 'string' ? 'std::string()' : shape.valueType === 'boolean' ? 'false' : '0.0'
        const value =
          shape.valueType === 'string'
            ? `__gea_store->${fieldName}()`
            : shape.valueType === 'boolean'
              ? `static_cast<bool>(__gea_store->${fieldName}())`
              : `static_cast<double>(__gea_store->${fieldName}())`
        fields.push({
          storeClass,
          stateType: `${storeClass}_state`,
          fieldName,
          fieldType,
          readerName: `[](const auto &__gea_store) -> ${fieldType} { return __gea_store ? ${value} : ${fallback}; }`,
          field,
          shape,
          reader: null,
          storeGlobalName,
          storeRuntimeBase: store.runtimeBase,
          storeIsSelfStore: false,
          isGetter: true,
          reactiveDeps: expandedGetterDeps(store, getter),
        })
      }
    }
  }
  return fields
}

function expandedGetterDeps(
  store: GeaIrStore,
  getter: NonNullable<GeaIrStore['getters']>[number],
): string[] {
  const getters = new Map((store.getters ?? []).map((candidate) => [candidate.name, candidate]))
  const out: string[] = []
  const visiting = new Set<string>()
  const append = (name: string): void => {
    const nested = getters.get(name)
    if (!nested || visiting.has(name)) {
      const dep = sanitizeCppIdentifier(name)
      if (!out.includes(dep)) out.push(dep)
      return
    }
    visiting.add(name)
    for (const dep of nested.deps) append(dep)
    visiting.delete(name)
  }
  for (const dep of getter.deps) append(dep)
  return out
}

const primitiveGetterShapeCache = new Map<string, Map<string, GeaIrStoreValueShape>>()

// The current Gea IR producer records getter bodies/dependencies but may omit a
// primitive getter's shape. The compat source still carries its TypeScript
// return annotation, so recover that type here instead of discarding static
// information and falling back to a boxed property read.
function primitiveGetterShape(
  store: GeaIrStore,
  getter: NonNullable<GeaIrStore['getters']>[number],
): GeaIrStoreValueShape | undefined {
  if (getter.shape?.kind === 'literal') return getter.shape
  const cacheKey = `${store.module}#${store.className}`
  let byName = primitiveGetterShapeCache.get(cacheKey)
  if (!byName) {
    byName = new Map<string, GeaIrStoreValueShape>()
    try {
      const sourceText = fs.readFileSync(store.module, 'utf8')
      const sourceFile = ts.createSourceFile(
        store.module,
        sourceText,
        ts.ScriptTarget.Latest,
        true,
        store.module.endsWith('.tsx') ? ts.ScriptKind.TSX : ts.ScriptKind.TS,
      )
      for (const statement of sourceFile.statements) {
        if (!ts.isClassDeclaration(statement) || statement.name?.text !== store.className) continue
        for (const member of statement.members) {
          if (!ts.isGetAccessorDeclaration(member) || !ts.isIdentifier(member.name) || !member.type) continue
          const valueType =
            member.type.kind === ts.SyntaxKind.StringKeyword
              ? 'string'
              : member.type.kind === ts.SyntaxKind.NumberKeyword
                ? 'number'
                : member.type.kind === ts.SyntaxKind.BooleanKeyword
                  ? 'boolean'
                  : null
          if (valueType) byName.set(member.name.text, { kind: 'literal', valueType })
        }
      }
    } catch {
      // Missing/unreadable source keeps the existing dynamic fallback.
    }
    primitiveGetterShapeCache.set(cacheKey, byName)
  }
  return byName.get(getter.name)
}

export function collectStoreMethods(stores: GeaIrStore[]): StoreMethodPlan[] {
  const methods: StoreMethodPlan[] = []
  for (const store of stores) {
    const storeClass = sanitizeCppIdentifier(store.className)
    const storeGlobalName = storeInstanceGlobalName(store)
    for (const method of store.methods ?? []) {
      methods.push({
        storeClass,
        stateType: `${storeClass}_state`,
        storeGlobalName,
        globalAccess: store.selfStore ? 'reference' : 'shared_ptr',
        methodName: sanitizeCppIdentifier(method.name),
        params: method.params.map((param) => ({
          name: sanitizeCppIdentifier(param.name),
          ...(param.valueType ? { valueType: param.valueType } : {}),
        })),
      })
    }
  }
  return methods
}

export function collectSingletonMethodsFromSourceFiles(
  entryFiles: string[],
  stores: GeaIrStore[] = [],
): StoreMethodPlan[] {
  const storeClasses = new Set(stores.map((store) => sanitizeCppIdentifier(store.className)))
  const classMethods = new Map<string, StoreMethodPlan[]>()
  const singletons: Array<{ name: string; className: string }> = []
  for (const file of reachableLocalSourceFiles(entryFiles)) {
    let sourceText: string
    try {
      sourceText = fs.readFileSync(file, 'utf8')
    } catch {
      continue
    }
    const sourceFile = ts.createSourceFile(file, sourceText, ts.ScriptTarget.Latest, true, file.endsWith('.tsx') ? ts.ScriptKind.TSX : ts.ScriptKind.TS)
    for (const statement of sourceFile.statements) {
      if (ts.isClassDeclaration(statement) && statement.name) {
        const className = sanitizeCppIdentifier(statement.name.text)
        if (storeClasses.has(className) || classExtendsStore(statement)) continue
        const methods: StoreMethodPlan[] = []
        for (const member of statement.members) {
          if (!ts.isMethodDeclaration(member)) continue
          if (!ts.isIdentifier(member.name)) continue
          if (hasModifier(member, ts.SyntaxKind.StaticKeyword) || hasModifier(member, ts.SyntaxKind.PrivateKeyword)) continue
          methods.push({
            storeClass: className,
            stateType: `${className}_state`,
            globalAccess: 'reference',
            methodName: sanitizeCppIdentifier(member.name.text),
            params: member.parameters.map((param, index) => ({
              name: ts.isIdentifier(param.name) ? sanitizeCppIdentifier(param.name.text) : `arg${index}`,
              ...(parameterValueType(param.type) ? { valueType: parameterValueType(param.type)! } : {}),
            })),
          })
        }
        if (methods.length > 0) classMethods.set(className, methods)
        continue
      }
      if (!ts.isVariableStatement(statement)) continue
      if (!hasModifier(statement, ts.SyntaxKind.ExportKeyword)) continue
      if ((statement.declarationList.flags & ts.NodeFlags.Const) === 0) continue
      for (const declaration of statement.declarationList.declarations) {
        if (!ts.isIdentifier(declaration.name) || !declaration.initializer) continue
        const className = newExpressionClassName(declaration.initializer)
        if (!className || storeClasses.has(className)) continue
        singletons.push({ name: sanitizeCppIdentifier(declaration.name.text), className })
      }
    }
  }

  const out: StoreMethodPlan[] = []
  const seen = new Set<string>()
  for (const singleton of singletons) {
    for (const method of classMethods.get(singleton.className) ?? []) {
      const key = `${singleton.name}.${method.methodName}/${method.params.length}`
      if (seen.has(key)) continue
      seen.add(key)
      out.push({ ...method, storeGlobalName: singleton.name })
    }
  }
  return out
}

// Resolve the exported singleton instance name for a store by scanning its
// module for `export const <name> = new <ClassName>(...)`. Returns undefined
// when the binding can't be found (no source, aliased export, etc.), in which
// case ambiguous field lookups simply stay unresolved as before.
const storeGlobalNameCache = new Map<string, string | undefined>()
export function storeInstanceGlobalName(store: GeaIrStore): string | undefined {
  // A self-store has no exported singleton — its template reads it as `this`
  // (`{this.count}`). Registering `this` as the instance name lets the
  // receiver-disambiguation in storeFieldByName resolve `this.field` to the
  // component's own store when the field name collides with another store.
  // (Two ReactiveComponents sharing a field name stay ambiguous → slow path.)
  if (store.selfStore) return 'this'
  const className = sanitizeCppIdentifier(store.className)
  const cacheKey = `${store.module}#${className}`
  if (storeGlobalNameCache.has(cacheKey)) return storeGlobalNameCache.get(cacheKey)
  let resolved: string | undefined
  try {
    const source = fs.readFileSync(store.module, 'utf8')
    const escaped = store.className.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
    const match = source.match(new RegExp(`export\\s+const\\s+([A-Za-z_$][A-Za-z0-9_$]*)\\s*=\\s*new\\s+${escaped}\\b`))
    if (match) resolved = sanitizeCppIdentifier(match[1])
  } catch {
    resolved = undefined
  }
  storeGlobalNameCache.set(cacheKey, resolved)
  return resolved
}

export function storeFieldReaderName(storeClass: string, fieldName: string): string {
  return `read_${sanitizeCppIdentifier(storeClass)}_field_${sanitizeCppIdentifier(fieldName)}`
}

// Typed reader OVERLOADS for self-stores. The renderer's read call shape is
// identical in both modes — `read_App_field_count(store)` — so a self-store just
// needs a `shared_ptr<App>` overload whose body reads the typed Signal field
// directly. Emitted in the LATE gea_ir block (after `class App` is complete);
// the boxed overload from the early block remains for any dynamic consumer.
// Non-primitive fields (arrays, records) get no typed reader yet — an expression
// over them keeps resolving to the boxed overload, which fails to compile only
// if a typed renderer actually uses it (loud, not silent).
export function generateTypedSelfStoreReaders(stores: GeaIrStore[]): string[] {
  const lines: string[] = []
  for (const store of stores) {
    if (!store.selfStore) continue
    const storeClass = sanitizeCppIdentifier(store.className)
    for (const field of store.fields) {
      const shape = fieldShape(field)
      if (shape?.kind !== 'literal') continue
      const cppType = shape.valueType === 'string' ? 'std::string' : shape.valueType === 'boolean' ? 'bool' : 'double'
      const fieldName = sanitizeCppIdentifier(field.name)
      lines.push(
        `inline ${cppType} ${storeFieldReaderName(storeClass, fieldName)}(const std::shared_ptr<${storeClass}> &store) { return store->${fieldName}.get(); }`,
      )
    }
  }
  if (lines.length > 0) lines.push('')
  return lines
}

export function storeFieldReaderPlan(store: GeaIrStore, field: GeaIrStoreField): StoreFieldReaderPlan | null {
  const shape = fieldShape(field)
  if (!isFieldReadableShape(shape)) return null

  const storeClass = sanitizeCppIdentifier(store.className)
  const fieldName = sanitizeCppIdentifier(field.name)
  return {
    storeClass,
    stateType: `${storeClass}_state`,
    fieldName,
    fieldType: cppTypeForField(field, `${storeClass}_${fieldName}`, []),
    readerName: storeFieldReaderName(storeClass, fieldName),
    field,
    shape,
  }
}

function generateStoreStateReader(store: GeaIrStore): string[] {
  if (!storeDynamicFallbackEnabled()) return []
  const lines: string[] = []
  // Dedup per item-type: two fields sharing an interface element type
  // (`hours`/`days` both `Forecast[]`) produce the same `read___gea_type_X`
  // reader fn. Synthesized item types are field-unique so never collide.
  const emittedItemReaders = new Set<string>()
  for (const field of store.fields) {
    const shape = fieldShape(field)
    if (!isArrayObjectShape(shape)) continue
    const itemType = arrayItemTypeInfo(sanitizeCppIdentifier(store.className), field).itemType
    if (emittedItemReaders.has(itemType)) continue
    emittedItemReaders.add(itemType)
    lines.push(...generateArrayItemStateReader(store, field, shape.element.fields))
  }
  for (const field of store.fields) lines.push(...generateStoreFieldReader(store, field))
  // The whole-state `read_<Store>_state` readers are gone with the dead
  // `<Store>_state` mirror struct — bindings read per-field.
  return lines
}

function generateArrayItemStateReader(store: GeaIrStore, field: GeaIrStoreField, itemFields: GeaIrStoreField[]): string[] {
  const info = arrayItemTypeInfo(sanitizeCppIdentifier(store.className), field)
  // Reader fn name keyed off the UNQUALIFIED item symbol so it never contains
  // `::` (`read___gea_type_X`, not `read_::__gea_type_X`). cpp-mounted and
  // generateArrayStoreFieldReader both reconstruct this same name.
  const readerFn = `read_${info.itemType}`
  if (info.useInterfaceStruct) {
    // Reuse geatsc's `__gea_from_value` factory, which SETS the per-field
    // presence flags. A field-by-field read here would leave them false and
    // empty the dynamic surface.
    return [
      `inline ${info.itemTypeRef} ${readerFn}(const gea_cpp_value &value) {`,
      `  return ${info.itemTypeRef}::__gea_from_value(value);`,
      '}',
      '',
    ]
  }
  const lines = [
    `inline ${info.itemTypeRef} ${readerFn}(const gea_cpp_value &value) {`,
    `  ${info.itemTypeRef} item{};`,
  ]
  for (const itemField of itemFields) {
    const line = readLiteralFieldLine('item', 'value', itemField)
    if (line) lines.push(`  ${line}`)
  }
  lines.push('  return item;', '}', '')
  return lines
}

function generateStoreFieldReader(store: GeaIrStore, field: GeaIrStoreField): string[] {
  const plan = storeFieldReaderPlan(store, field)
  if (!plan) return []
  if (!boxedStoreFieldReaderEmitted(store, plan)) return []
  // Typed-carrier stores additionally get `shared_ptr<Store>` reader
  // overloads, but those are declared-and-defined ONLY in the late block
  // (generateTypedStoreReaderDefinitions): this early block can precede the
  // store class's forward declaration entirely (module emission order
  // varies), and a `class X;` here would declare gea_ir::X, shadowing the
  // global store class. Every typed call site lives in the late mounted
  // renderers, which follow the definitions.
  return plan.shape.kind === 'literal'
    ? generateScalarStoreFieldReader(store, plan)
    : isArrayObjectShape(plan.shape)
      ? generateArrayStoreFieldReader(store, plan)
      : []
}

// Typed reader overloads exist for native IR stores whose global cells geatsc
// core types as `std::shared_ptr<Class>`. A lean runtime base changes the
// method implementation strategy, not the store's native carrier.
function typedReaderOverloadEligible(store: GeaIrStore): boolean {
  return !store.selfStore
}

// Whether this field's BOXED reader is emitted at all. For a typed-carrier
// store (typed cell + typed mounts, hub on) a typed-direct field has NO
// remaining boxed call site — the shared_ptr overload reads the member
// directly and every crossing on the store path is typed — so the boxed
// reader (and its as_shared_ptr fast path) would be dead weight in the
// program. Fields whose class member ISN'T the typed value keep the boxed
// reader: the typed overload routes through it via gea_cpp_key.
function boxedStoreFieldReaderEmitted(store: GeaIrStore, plan: StoreFieldReaderPlan): boolean {
  if (!typedReaderOverloadEligible(store) || !storeHubEmissionEnabled()) return true
  const typedDirect =
    (plan.shape.kind === 'literal' && scalarReaderHasTypedDefinition(store, plan)) ||
    (isArrayObjectShape(plan.shape) && arrayReaderHasTypedDefinition(store, plan))
  return !typedDirect
}

// Mirrors the early-block emission predicate: which fields actually got a
// reader. Must match generateScalarStoreFieldReader/generateArrayStoreFieldReader
// or the typed overload would be declared without a boxed sibling (harmless)
// or defined for a reader that doesn't exist (compile error).
function storeFieldReaderEmitted(plan: StoreFieldReaderPlan): boolean {
  if (plan.shape.kind === 'literal') return !!literalFieldReadExpression('store', plan.field)
  return isArrayObjectShape(plan.shape)
}

// A scalar reader gets a typed fast-path DEFINITION in the late gea_ir block
// (after the store class is complete — the typed member read needs it) iff the
// class member is a typed primitive. Early-block emission is then a declaration
// only. The predicate must match EXACTLY between the early declaration and the
// late definition, or we'd declare without defining (or define twice).
function scalarReaderHasTypedDefinition(store: GeaIrStore, plan: StoreFieldReaderPlan): boolean {
  if (store.selfStore) return false // self-stores get a shared_ptr<T> overload instead
  return plan.fieldType === 'double' || plan.fieldType === 'std::string' || plan.fieldType === 'bool'
}

function generateScalarStoreFieldReader(store: GeaIrStore, plan: StoreFieldReaderPlan): string[] {
  const expr = literalFieldReadExpression('store', plan.field)
  if (!expr) return []
  if (scalarReaderHasTypedDefinition(store, plan)) {
    return [`inline ${plan.fieldType} ${plan.readerName}(const gea_cpp_value &store);`, '']
  }
  return [`inline ${plan.fieldType} ${plan.readerName}(const gea_cpp_value &store) {`, `  return ${expr};`, '}', '']
}

// Late-block scalar reader definitions with the typed fast path: resolve the
// store's typed instance from the boxed global's object_owner (one pointer cast)
// and read the member directly — no string-keyed record lookup, no proxy
// roundtrip, no gea_cpp_value materialization. Falls back to the dynamic read
// for a value that doesn't carry the typed instance (defensive; the globals
// always do).
export function generateTypedStoreReaderDefinitions(stores: GeaIrStore[]): string[] {
  const lines: string[] = []
  for (const store of stores) {
    const storeClass = sanitizeCppIdentifier(store.className)
    for (const field of store.fields) {
      const plan = storeFieldReaderPlan(store, field)
      if (!plan) continue
      // Typed-carrier stores don't emit boxed readers for typed-direct
      // fields at all — no boxed call site survives the debox. They still
      // need the shared_ptr overload emitted below because generated module
      // code can call the reader through the typed global cell.
      const emitBoxedReader = boxedStoreFieldReaderEmitted(store, plan)
      const fieldIdent = sanitizeCppIdentifier(field.name)
      if (emitBoxedReader && plan.shape.kind === 'literal' && scalarReaderHasTypedDefinition(store, plan)) {
        const expr = literalFieldReadExpression('store', plan.field)
        if (!expr) continue
        lines.push(
          `inline ${plan.fieldType} ${plan.readerName}(const gea_cpp_value &store) {`,
          `  if (auto __gea_typed = gea_cpp_value_as_shared_ptr<${storeClass}>(store)) return __gea_typed->${fieldIdent};`,
          `  return ${expr};`,
          '}',
          '',
        )
        continue
      }
      if (emitBoxedReader && isArrayObjectShape(plan.shape) && arrayReaderHasTypedDefinition(store, plan)) {
        lines.push(
          `inline ${plan.fieldType} ${plan.readerName}(const gea_cpp_value &store) {`,
          `  if (auto __gea_typed = gea_cpp_value_as_shared_ptr<${storeClass}>(store)) return __gea_typed->${fieldIdent};`,
          ...boxedArrayReaderBody(plan),
          '}',
          '',
        )
      }
    }
    // (The late `read_<Store>_state` definitions were removed with the dead
    // `<Store>_state` mirror struct — see generateStoreDeclarations.)
    if (typedReaderOverloadEligible(store)) {
      // shared_ptr overloads for the typed carrier: typed members read
      // directly; anything else boxes through gea_cpp_key (the retained
      // tracked proxy) into the boxed reader.
      for (const field of store.fields) {
        const plan = storeFieldReaderPlan(store, field)
        if (!plan || !storeFieldReaderEmitted(plan)) continue
        const fieldIdent = sanitizeCppIdentifier(field.name)
        const typedDirect =
          (plan.shape.kind === 'literal' && scalarReaderHasTypedDefinition(store, plan)) ||
          (isArrayObjectShape(plan.shape) && arrayReaderHasTypedDefinition(store, plan))
        lines.push(
          `inline ${plan.fieldType} ${plan.readerName}(const std::shared_ptr<${storeClass}> &store) {`,
          (plan.shape.kind === 'literal' && plan.fieldType === 'std::nullptr_t') ? '  return nullptr;' : typedDirect ? `  return store->${fieldIdent};` : `  return ${plan.readerName}(gea_cpp_key(store));`,
          '}',
          '',
        )
      }
    }
  }
  return lines
}

// A typedStorage array field's CLASS member is already the typed vector
// (`std::vector<__gea_type_Forecast>` — applyTypedArrayStorage rewrote it), so
// the typed read is a direct vector copy: no facade materialization of a
// gea_cpp_value array, no per-element record conversion. Same decl-early /
// def-late split as scalars.
function arrayReaderHasTypedDefinition(store: GeaIrStore, plan: StoreFieldReaderPlan): boolean {
  return !store.selfStore && !!plan.field.typedStorage
}

function boxedArrayReaderBody(plan: StoreFieldReaderPlan): string[] {
  // The per-item reader fn name is `read_<itemType>` — `read_<Store>_<field>_item`
  // for synthesized structs, `read___gea_type_<Name>` for interface reuse.
  const itemReader = `read_${arrayItemTypeInfo(plan.storeClass, plan.field).itemType}`
  return [
    `  ${plan.fieldType} items;`,
    `  auto __gea_array_value = store.record_get_literal(${JSON.stringify(plan.field.name)});`,
    `  const auto &__gea_values = (__gea_array_value.kind == gea_cpp_value::kind_t::proxy && __gea_array_value.proxy_target) ? __gea_array_value.proxy_target->array_ref() : __gea_array_value.array_ref();`,
    `  items.reserve(__gea_values.size());`,
    `  for (const auto &value : __gea_values) {`,
    `    items.push_back(${itemReader}(value));`,
    '  }',
    '  return items;',
  ]
}

function generateArrayStoreFieldReader(store: GeaIrStore, plan: StoreFieldReaderPlan): string[] {
  if (!isArrayObjectShape(plan.shape)) return []
  if (arrayReaderHasTypedDefinition(store, plan)) {
    return [`inline ${plan.fieldType} ${plan.readerName}(const gea_cpp_value &store);`, '']
  }
  return [
    `inline ${plan.fieldType} ${plan.readerName}(const gea_cpp_value &store) {`,
    ...boxedArrayReaderBody(plan),
    '}',
    '',
  ]
}

function readLiteralFieldLine(target: string, source: string, field: GeaIrStoreField): string | null {
  const name = sanitizeCppIdentifier(field.name)
  const expr = literalFieldReadExpression(source, field)
  return expr ? `${target}.${name} = ${expr};` : null
}

function literalFieldReadExpression(source: string, field: GeaIrStoreField): string | null {
  const shape = fieldShape(field)
  if (shape?.kind !== 'literal') return null
  if (shape.valueType === 'number') return `read_number_field(${source}, ${JSON.stringify(field.name)})`
  if (shape.valueType === 'boolean') return `read_boolean_field(${source}, ${JSON.stringify(field.name)})`
  if (shape.valueType === 'string') return `read_string_field(${source}, ${JSON.stringify(field.name)})`
  if (shape.valueType === 'null') return 'nullptr'
  return null
}

interface ArrayItemTypeInfo {
  itemType: string
  itemTypeRef: string
  useInterfaceStruct: boolean
}

// Resolve the element type info for a store array field, consistent with
// `collectStoreArrayFields`. `itemType` is the unqualified symbol (used to
// build `read_<itemType>` reader names — never contains `::`); `itemTypeRef`
// is the qualified type-use spelling (`::__gea_type_X` for interface reuse).
function arrayItemTypeInfo(storeClass: string, field: GeaIrStoreField): ArrayItemTypeInfo {
  const shape = fieldShape(field)
  const fieldName = sanitizeCppIdentifier(field.name)
  if (isArrayObjectShape(shape)) {
    const useInterfaceStruct = field.typedStorage
      ? field.typedStorage.useInterfaceStruct === true
      : arrayReusesInterfaceStruct(shape)
    if (useInterfaceStruct) {
      const itemType = field.typedStorage?.itemType ?? interfaceItemSymbol(shape.elementTypeName as string)
      // `::`-qualified (global scope) — see `collectStoreArrayFields`.
      return { itemType, itemTypeRef: `::${itemType}`, useInterfaceStruct: true }
    }
  }
  const itemType = field.typedStorage?.itemType ?? `${storeClass}_${fieldName}_item`
  return { itemType, itemTypeRef: itemType, useInterfaceStruct: false }
}

function cppTypeForField(field: GeaIrStoreField, nameHint: string, declarations: string[]): string {
  const shape = fieldShape(field)
  if (shape && isArrayObjectShape(shape) && field.typedStorage) {
    if (field.typedStorage.useInterfaceStruct) {
      return `std::vector<::${field.typedStorage.itemType}>`
    }
    const elementType = cppTypeForShape(shape.element, field.typedStorage.itemType, declarations)
    return `std::vector<${elementType}>`
  }
  return shape ? cppTypeForShape(shape, nameHint, declarations) : 'gea_cpp_value'
}

function cppTypeForShape(shape: GeaIrStoreValueShape, nameHint: string, declarations: string[]): string {
  if (shape.kind === 'literal') {
    if (shape.valueType === 'number') {
      // Keyed-list item position/velocity fields (x/y/dx/dy) lower to the
      // single-precision `gea_f32` type (hardware FPU) on boards that opt in via
      // GEA_NUMBER_FLOAT; plain `double` otherwise (byte-identical, test262-safe
      // default). The board gate lives in the `GEA_ITEM_NUMBER` macro emitted
      // below, so the field type is one `#define` rather than baked per-field.
      // gea_f32 is the unified single-precision type (see runtime/gea_f32.h);
      // this is the implicit, board-gated adoption path. Scoped to animated item
      // fields, where float's 2^24 integer-exactness limit is never a concern
      // (screen coords).
      if (/_item_(x|y|dx|dy)$/.test(nameHint)) return 'GEA_ITEM_NUMBER'
      return 'double'
    }
    if (shape.valueType === 'boolean') return 'bool'
    if (shape.valueType === 'string') return 'std::string'
    return 'std::nullptr_t'
  }
  if (shape.kind === 'array') {
    // Interface-reuse array: storage element is the geatsc-declared global
    // struct `::__gea_type_<Name>`. Do NOT push a synthesized struct — geatsc
    // already emits it (and the gea plugin force-emits it; see collector.ts).
    if (isArrayObjectShape(shape) && arrayReusesInterfaceStruct(shape)) {
      return `std::vector<::${interfaceItemSymbol(shape.elementTypeName as string)}>`
    }
    const elementType = shape.element ? cppTypeForShape(shape.element, `${nameHint}_item`, declarations) : 'gea_cpp_value'
    return `std::vector<${elementType}>`
  }
  const structName = sanitizeCppIdentifier(nameHint)
  const nested: string[] = []
  const fields = shape.fields.map((field) => {
    const fieldType = cppTypeForField(field, `${structName}_${sanitizeCppIdentifier(field.name)}`, nested)
    return `  ${fieldType} ${sanitizeCppIdentifier(field.name)};`
  })
  declarations.push(...nested)
  // If any field is stored as `gea_number`, define the typedef right here (in
  // namespace gea_ir, before the struct that uses it). gea_number is `double` by
  // default (test262-safe, byte-identical); a board that defines GEA_NUMBER_FLOAT
  // lowers it to single-precision float for the hardware FPU. We emit it inline
  // rather than relying on runtime/gea_number.h because the per-app build dir
  // carries a stale snapshot of the runtime umbrella that need not include it. An
  // identical `using` re-declaration across structs is legal C++.
  if (fields.some((f) => f.includes('GEA_ITEM_NUMBER '))) {
    // The item-field number type is board-gated: on boards that define
    // GEA_NUMBER_FLOAT it is the single-precision `gea_f32` wrapper (hardware
    // FPU); otherwise plain `double` (byte-identical, test262-safe default).
    // gea_f32 itself is defined once in the runtime umbrella (runtime/gea_f32.h),
    // always float — so the board gate here only picks WHICH type the field
    // uses, not the wrapper's precision. Macro-guarded so re-definition across
    // structs is a no-op.
    declarations.push(
      [
        '#ifndef GEA_ITEM_NUMBER_DEFINED',
        '#define GEA_ITEM_NUMBER_DEFINED',
        '#if defined(GEA_NUMBER_FLOAT)',
        '#define GEA_ITEM_NUMBER gea_f32',
        '#else',
        '#define GEA_ITEM_NUMBER double',
        '#endif',
        '#endif',
      ].join('\n'),
    )
  }
  declarations.push(`struct ${structName} {`)
  declarations.push(...fields)
  // Duck-type members so the typed struct can stand in for a `gea_cpp_value`
  // record slot in geatsc-emitter fallback code paths that call
  // `slot.record_get_literal(...)` / `slot.record_set_literal(...)` /
  // `slot.is_nullish()` / `slot.record_set(symbol, ...)` directly. The
  // gea plugin's lowered method bodies route through `set_typed_field`
  // / `read_number_field` overloads instead and don't need these, but
  // unmodified fallback emissions for the same store still link.
  const memberLines = generateTypedSlotMemberMethods(shape.fields, structName)
  if (memberLines.length > 0) {
    declarations.push('')
    declarations.push(...memberLines.map((l) => `  ${l}`))
  }
  declarations.push('};')
  declarations.push('')
  return structName
}

function generateTypedSlotMemberMethods(fields: GeaIrStoreField[], structName: string): string[] {
  const getBranches: string[] = []
  const setBranches: string[] = []
  const fromValueAssigns: string[] = []
  const fromRecordValueAssigns: string[] = []
  const fromShapeAssigns: string[] = []
  // Field-wise constructor params/inits, in declaration order. Lets a fully
  // typed seed construct the item directly — `gea_ir::X_item(0, 0, "#000")` —
  // instead of round-tripping each element through a boxed `gea_cpp_value`
  // record. See `rewriteCtorSeed` in cpp-store-typed-arrays.ts.
  const ctorParams: string[] = []
  const ctorInits: string[] = []
  for (const field of fields) {
    const shape = fieldShape(field)
    if (shape?.kind !== 'literal') continue
    const name = sanitizeCppIdentifier(field.name)
    const key = JSON.stringify(field.name)
    if (shape.valueType === 'number') {
      getBranches.push(`if (std::strcmp(name, ${key}) == 0) return gea_cpp_value(this->${name});`)
      setBranches.push(`if (std::strcmp(name, ${key}) == 0) { this->${name} = gea::runtime::coerce::to_number(value); return; }`)
      fromValueAssigns.push(`this->${name} = read_number_field(value, ${key});`)
      fromRecordValueAssigns.push(`{ auto __gea_field = value.record_get_literal(${key}); if (!__gea_field.is_nullish()) this->${name} = gea::runtime::coerce::to_number(__gea_field); }`)
      fromShapeAssigns.push(`if constexpr (requires { other.${name}; }) this->${name} = static_cast<double>(other.${name});`)
      ctorParams.push(`double ${name}`)
      ctorInits.push(`${name}(${name})`)
    } else if (shape.valueType === 'boolean') {
      getBranches.push(`if (std::strcmp(name, ${key}) == 0) return gea_cpp_value(this->${name});`)
      setBranches.push(`if (std::strcmp(name, ${key}) == 0) { this->${name} = gea::runtime::coerce::to_boolean(value); return; }`)
      fromValueAssigns.push(`this->${name} = read_boolean_field(value, ${key});`)
      fromRecordValueAssigns.push(`{ auto __gea_field = value.record_get_literal(${key}); if (!__gea_field.is_nullish()) this->${name} = gea::runtime::coerce::to_boolean(__gea_field); }`)
      fromShapeAssigns.push(`if constexpr (requires { other.${name}; }) this->${name} = static_cast<bool>(other.${name});`)
      ctorParams.push(`bool ${name}`)
      ctorInits.push(`${name}(${name})`)
    } else if (shape.valueType === 'string') {
      getBranches.push(`if (std::strcmp(name, ${key}) == 0) return gea_cpp_value(this->${name});`)
      setBranches.push(`if (std::strcmp(name, ${key}) == 0) { this->${name} = gea_cpp_to_string(value); return; }`)
      fromValueAssigns.push(`this->${name} = read_string_field(value, ${key});`)
      fromRecordValueAssigns.push(`{ auto __gea_field = value.record_get_literal(${key}); if (!__gea_field.is_nullish()) this->${name} = gea_cpp_to_string(__gea_field); }`)
      fromShapeAssigns.push(`if constexpr (requires { other.${name}; }) this->${name} = static_cast<std::string>(other.${name});`)
      ctorParams.push(`std::string ${name}`)
      ctorInits.push(`${name}(std::move(${name}))`)
    }
  }
  if (getBranches.length === 0 && setBranches.length === 0) return []
  // A direct field-wise constructor for primitive object shapes. The one-field
  // case is important for static collision/tile data: those should construct
  // typed items directly instead of taking the generic shape-converting path.
  const allFieldsArePrimitive = fields.every((field) => fieldShape(field)?.kind === 'literal')
  const fieldwiseCtor =
    allFieldsArePrimitive && ctorParams.length >= 1 ? [`${structName}(${ctorParams.join(', ')}) : ${ctorInits.join(', ')} {}`] : []
  if (!storeDynamicFallbackEnabled()) {
    return [
      `${structName}() = default;`,
      ...fieldwiseCtor,
      'template <typename _GeaShape>',
      `${structName}(const _GeaShape &other) {`,
      ...fromShapeAssigns.map((l) => `  ${l}`),
      `}`,
      `explicit ${structName}(const gea_cpp_value &value) {`,
      ...fromRecordValueAssigns.map((l) => `  ${l}`),
      `}`,
      `static ${structName} __gea_from_value(const gea_cpp_value &value) { return ${structName}(value); }`,
    ]
  }
  // The typed→dynamic body for `__gea_to_value()` mirrors the free
  // `gea_cpp_key(const X_item&)` overload but kept inline so any code that
  // does `pack_value(typed_item)` or invokes `gea_cpp_value(const T&)` (the
  // catch-all templated ctor that probes for `__gea_to_value()`) gets a
  // properly-populated record gea_cpp_value instead of an empty
  // kind=object placeholder.
  const toValueLines: string[] = []
  for (const field of fields) {
    const shape = fieldShape(field)
    if (shape?.kind !== 'literal') continue
    const name = sanitizeCppIdentifier(field.name)
    const key = JSON.stringify(field.name)
    toValueLines.push(`__gea_out.record_set_literal(${key}, gea_cpp_value(this->${name}));`)
  }
  return [
    // Default ctor — a user-declared default keeps brace-init `T{}` working
    // and pairs with the converting ctors below so the struct is no longer
    // an aggregate but still cheaply zero-initialisable.
    `${structName}() = default;`,
    ...fieldwiseCtor,
    // Converting ctor from `gea_cpp_value`. Marked `explicit` so it doesn't
    // make typed and dynamic interchangeable in implicit-conversion
    // contexts (e.g. ternaries that mix a typed slot and a gea_cpp_value
    // sentinel); without `explicit` the compiler reports the conditional
    // as ambiguous because `gea_cpp_value` itself has a catch-all
    // `gea_cpp_value(const T&)` template that would convert the typed slot
    // back. Call sites that need the conversion (notably `push_array_items`
    // and the geatsc emitter's `vec.push_back(gea_cpp_key(<record>))`
    // fallback) go through `static_cast<X_item>(...)`, which is wired in
    // both `cpp-store-method-replacements.ts`'s `push_array_items` and the
    // typed-array push-rewrite in `cpp-store-typed-arrays.ts`.
    `explicit ${structName}(const gea_cpp_value &value) {`,
    ...fromValueAssigns.map((l) => `  ${l}`),
    `}`,
    `static ${structName} __gea_from_value(const gea_cpp_value &value) { return ${structName}(value); }`,
    // `__gea_to_value()` matches the convention the geatsc runtime probes
    // for in `pack_value` and the templated `gea_cpp_value(const T&)`
    // catch-all ctor. Without this, those paths produce an empty
    // kind=object gea_cpp_value for typed items — which silently breaks
    // any reactive consumer that reads the array via the dynamic surface
    // (`store.record_get_literal("balls")` snapshots, change-record
    // previousValue, etc.).
    `gea_cpp_value __gea_to_value() const {`,
    `  gea_cpp_value __gea_out;`,
    `  __gea_out.kind = gea_cpp_value::kind_t::record;`,
    `  __gea_out.entries = std::make_shared<std::vector<std::pair<std::string, gea_cpp_value>>>();`,
    ...toValueLines.map((l) => `  ${l}`),
    `  return __gea_out;`,
    `}`,
    // Converting ctor from any shape-equivalent struct (e.g. the
    // `__gea_record_N{...}` anonymous record temporaries the geatsc emitter
    // produces for typed object literals at each source location). Field
    // names are checked at compile time via `requires`. Implicit so the
    // emitter's `vec.push_back(<record temp>)` pattern flows directly.
    'template <typename _GeaShape, std::enable_if_t<!std::is_same_v<std::decay_t<_GeaShape>, gea_cpp_value> && !std::is_same_v<std::decay_t<_GeaShape>, ' + structName + '>, int> = 0>',
    `${structName}(const _GeaShape &other) {`,
    ...fromShapeAssigns.map((l) => `  ${l}`),
    `}`,
    'gea_cpp_value record_get_literal(const char *name) const {',
    ...getBranches.map((l) => `  ${l}`),
    '  return gea_cpp_value::missing();',
    '}',
    'void record_set_literal(const char *name, const gea_cpp_value &value) {',
    ...setBranches.map((l) => `  ${l}`),
    '}',
    // record_set(const gea_cpp_value &symbol_key, ...) — the legacy per-slot
    // dirty marker. Typed slots don't need it because dirty tracking is
    // signalled at the array level by `mark_dirty_field(array_key)`.
    'template <typename Key, typename Value>',
    'void record_set(const Key &, const Value &) {}',
    'bool is_nullish() const { return false; }',
  ]
}

function fieldShape(field: GeaIrStoreField): GeaIrStoreValueShape | null {
  return field.shape ?? shapeFromInitializer(field.initializer)
}

function shapeFromInitializer(initializer: string | undefined): GeaIrStoreValueShape | null {
  if (!initializer) return null
  const text = initializer.trim()
  if (/^-?(?:\d+|\d+\.\d+|\.\d+)$/.test(text)) return { kind: 'literal', valueType: 'number' }
  if (text === 'true' || text === 'false') return { kind: 'literal', valueType: 'boolean' }
  if (text === 'null') return { kind: 'literal', valueType: 'null' }
  if ((text.startsWith("'") && text.endsWith("'")) || (text.startsWith('"') && text.endsWith('"'))) {
    return { kind: 'literal', valueType: 'string' }
  }
  return null
}

function shapeFromConstantInitializer(
  initializer: string | undefined,
  constants: ReadonlyMap<string, GeaIrConstant>,
  constantShapes: ReadonlyMap<string, GeaIrStoreValueShape> = new Map(),
): GeaIrStoreValueShape | null {
  if (!initializer) return null
  const text = initializer.trim()
  const name = text.startsWith('-') ? text.slice(1).trim() : text
  if (!/^[A-Za-z_$][A-Za-z0-9_$]*$/.test(name)) return null
  const constant = constants.get(name)
  if (constant && constant.valueType !== 'object-array') return { kind: 'literal', valueType: constant.valueType }
  return constantShapes.get(name) ?? null
}

function collectConstInitializerShapes(
  sourceFiles: readonly string[],
  constants: ReadonlyMap<string, GeaIrConstant>,
): Map<string, GeaIrStoreValueShape> {
  const shapes = new Map<string, GeaIrStoreValueShape>()
  for (const [name, constant] of constants) {
    if (constant.valueType === 'object-array') continue
    shapes.set(name, { kind: 'literal', valueType: constant.valueType })
  }

  const pending = new Map<string, string>()
  for (const file of reachableLocalSourceFiles([...sourceFiles])) {
    let sourceText: string
    try {
      sourceText = fs.readFileSync(file, 'utf8')
    } catch {
      continue
    }
    const sourceFile = ts.createSourceFile(
      file,
      sourceText,
      ts.ScriptTarget.Latest,
      true,
      file.endsWith('.tsx') ? ts.ScriptKind.TSX : ts.ScriptKind.TS,
    )
    for (const statement of sourceFile.statements) {
      if (!ts.isVariableStatement(statement) || (statement.declarationList.flags & ts.NodeFlags.Const) === 0) continue
      for (const declaration of statement.declarationList.declarations) {
        if (!ts.isIdentifier(declaration.name) || !declaration.initializer) continue
        pending.set(declaration.name.text, declaration.initializer.getText(sourceFile))
      }
    }
  }

  let changed = true
  while (changed) {
    changed = false
    for (const [name, expr] of pending) {
      if (shapes.has(name)) continue
      const shape = shapeFromExpression(expr, shapes)
      if (!shape) continue
      shapes.set(name, shape)
      changed = true
    }
  }
  return shapes
}

function collectStoreFieldTypeShapes(sourceFiles: readonly string[], stores: readonly GeaIrStore[]): Map<string, GeaIrStoreValueShape> {
  const storeNames = new Set(stores.map((store) => sanitizeCppIdentifier(store.className)))
  const shapes = new Map<string, GeaIrStoreValueShape>()
  for (const file of reachableLocalSourceFiles([...sourceFiles])) {
    let sourceText: string
    try {
      sourceText = fs.readFileSync(file, 'utf8')
    } catch {
      continue
    }
    const sourceFile = ts.createSourceFile(
      file,
      sourceText,
      ts.ScriptTarget.Latest,
      true,
      file.endsWith('.tsx') ? ts.ScriptKind.TSX : ts.ScriptKind.TS,
    )
    for (const statement of sourceFile.statements) {
      if (!ts.isClassDeclaration(statement) || !statement.name) continue
      const className = sanitizeCppIdentifier(statement.name.text)
      if (!storeNames.has(className)) continue
      for (const member of statement.members) {
        if (!ts.isPropertyDeclaration(member) || !member.type || !ts.isIdentifier(member.name)) continue
        const shape = shapeFromTypeNode(member.type)
        if (shape) shapes.set(`${className}.${sanitizeCppIdentifier(member.name.text)}`, shape)
      }
    }
  }
  return shapes
}

function shapeFromTypeNode(type: ts.TypeNode): GeaIrStoreValueShape | null {
  if (type.kind === ts.SyntaxKind.NumberKeyword) return { kind: 'literal', valueType: 'number' }
  if (type.kind === ts.SyntaxKind.StringKeyword) return { kind: 'literal', valueType: 'string' }
  if (type.kind === ts.SyntaxKind.BooleanKeyword) return { kind: 'literal', valueType: 'boolean' }
  if (ts.isArrayTypeNode(type)) return arrayShapeFromElementTypeNode(type.elementType)
  if (ts.isTypeReferenceNode(type) && ts.isIdentifier(type.typeName)) {
    const typeName = type.typeName.text
    if ((typeName === 'Array' || typeName === 'ReadonlyArray') && type.typeArguments?.length === 1) {
      return arrayShapeFromElementTypeNode(type.typeArguments[0])
    }
  }
  return null
}

function arrayShapeFromElementTypeNode(type: ts.TypeNode): GeaIrStoreValueShape | null {
  if (ts.isParenthesizedTypeNode(type)) return arrayShapeFromElementTypeNode(type.type)
  if (ts.isTypeLiteralNode(type)) {
    const fields = primitiveObjectFieldsFromMembers(type.members)
    return fields ? { kind: 'array', element: { kind: 'object', fields } } : null
  }
  if (ts.isTypeReferenceNode(type) && ts.isIdentifier(type.typeName) && !type.typeArguments?.length) {
    return { kind: 'array', elementTypeName: type.typeName.text }
  }
  return null
}

function shapeFromExpression(expr: string, constants: ReadonlyMap<string, GeaIrStoreValueShape>): GeaIrStoreValueShape | null {
  const text = expr.trim()
  const literal = shapeFromInitializer(text)
  if (literal) return literal
  const identifier = text.startsWith('-') ? text.slice(1).trim() : text
  if (/^[A-Za-z_$][A-Za-z0-9_$]*$/.test(identifier)) {
    const shape = constants.get(identifier)
    return shape?.kind === 'literal' && shape.valueType === 'number' ? shape : null
  }
  if (isNumericExpression(text, constants)) return { kind: 'literal', valueType: 'number' }
  return null
}

function isNumericExpression(expr: string, constants: ReadonlyMap<string, GeaIrStoreValueShape>): boolean {
  const source = ts.createSourceFile('expr.ts', `const __x = ${expr};`, ts.ScriptTarget.Latest, true, ts.ScriptKind.TS)
  const statement = source.statements[0]
  if (!statement || !ts.isVariableStatement(statement)) return false
  const init = statement.declarationList.declarations[0]?.initializer
  if (!init) return false
  return expressionIsNumeric(init, constants)
}

function expressionIsNumeric(node: ts.Expression, constants: ReadonlyMap<string, GeaIrStoreValueShape>): boolean {
  if (ts.isNumericLiteral(node)) return true
  if (node.kind === ts.SyntaxKind.TrueKeyword || node.kind === ts.SyntaxKind.FalseKeyword || node.kind === ts.SyntaxKind.NullKeyword) return false
  if (ts.isStringLiteral(node) || ts.isNoSubstitutionTemplateLiteral(node)) return false
  if (ts.isIdentifier(node)) {
    const shape = constants.get(node.text)
    return shape?.kind === 'literal' && shape.valueType === 'number'
  }
  if (ts.isParenthesizedExpression(node)) return expressionIsNumeric(node.expression, constants)
  if (ts.isPrefixUnaryExpression(node)) {
    return (
      (node.operator === ts.SyntaxKind.MinusToken || node.operator === ts.SyntaxKind.PlusToken) &&
      expressionIsNumeric(node.operand, constants)
    )
  }
  if (ts.isBinaryExpression(node)) {
    if (
      node.operatorToken.kind === ts.SyntaxKind.PlusToken ||
      node.operatorToken.kind === ts.SyntaxKind.MinusToken ||
      node.operatorToken.kind === ts.SyntaxKind.AsteriskToken ||
      node.operatorToken.kind === ts.SyntaxKind.SlashToken ||
      node.operatorToken.kind === ts.SyntaxKind.PercentToken
    ) {
      return expressionIsNumeric(node.left, constants) && expressionIsNumeric(node.right, constants)
    }
    return false
  }
  if (ts.isCallExpression(node)) {
    const callee = node.expression.getText()
    if (!/^Math\.(?:abs|ceil|floor|max|min|pow|round|sqrt|sin|cos|tan)$/.test(callee)) return false
    return node.arguments.every((arg) => expressionIsNumeric(arg, constants))
  }
  if (ts.isPropertyAccessExpression(node)) {
    const text = node.getText()
    return text === 'window.innerWidth' || text === 'window.innerHeight'
  }
  return false
}

function reachableLocalSourceFiles(entryFiles: string[]): string[] {
  const out: string[] = []
  const pending = [...entryFiles]
  const seen = new Set<string>()
  for (let index = 0; index < pending.length; index += 1) {
    const file = pending[index]
    if (seen.has(file) || !fs.existsSync(file)) continue
    seen.add(file)
    out.push(file)
    let source: string
    try {
      source = fs.readFileSync(file, 'utf8')
    } catch {
      continue
    }
    for (const imported of localImportFiles(file, source)) {
      if (!seen.has(imported)) pending.push(imported)
    }
  }
  return out
}

function localImportFiles(file: string, source: string): string[] {
  const out: string[] = []
  const pattern = /\bimport\b[\s\S]*?\bfrom\s+['"](\.{1,2}\/[^'"]+)['"]/g
  let match: RegExpExecArray | null
  while ((match = pattern.exec(source))) {
    const resolved = resolveLocalSource(file, match[1])
    if (resolved) out.push(resolved)
  }
  return out
}

function resolveLocalSource(fromFile: string, specifier: string): string | null {
  const base = resolve(dirname(fromFile), specifier)
  const candidates = [
    base,
    `${base}.ts`,
    `${base}.tsx`,
    `${base}.js`,
    `${base}.jsx`,
    resolve(base, 'index.ts'),
    resolve(base, 'index.tsx'),
    resolve(base, 'index.js'),
    resolve(base, 'index.jsx'),
  ]
  return candidates.find((candidate) => fs.existsSync(candidate)) ?? null
}

function hasModifier(node: ts.Node, kind: ts.SyntaxKind): boolean {
  return ts.canHaveModifiers(node) && (ts.getModifiers(node) ?? []).some((modifier) => modifier.kind === kind)
}

function classExtendsStore(node: ts.ClassDeclaration): boolean {
  return (node.heritageClauses ?? []).some((clause) =>
    clause.token === ts.SyntaxKind.ExtendsKeyword &&
    clause.types.some((type) => {
      const expr = type.expression
      return ts.isIdentifier(expr) && expr.text === 'Store'
    }),
  )
}

function parameterValueType(type: ts.TypeNode | undefined): 'string' | 'number' | 'boolean' | undefined {
  if (!type) return undefined
  if (type.kind === ts.SyntaxKind.StringKeyword) return 'string'
  if (type.kind === ts.SyntaxKind.NumberKeyword) return 'number'
  if (type.kind === ts.SyntaxKind.BooleanKeyword) return 'boolean'
  return undefined
}

function newExpressionClassName(expr: ts.Expression): string | null {
  const unwrapped = stripExpressionWrappers(expr)
  if (!ts.isNewExpression(unwrapped) || !ts.isIdentifier(unwrapped.expression)) return null
  return sanitizeCppIdentifier(unwrapped.expression.text)
}

function stripExpressionWrappers(expr: ts.Expression): ts.Expression {
  let current = expr
  while (
    ts.isParenthesizedExpression(current) ||
    ts.isAsExpression(current) ||
    ts.isTypeAssertionExpression(current) ||
    ts.isNonNullExpression(current) ||
    ts.isSatisfiesExpression(current)
  ) {
    current = current.expression
  }
  return current
}

function isArrayObjectShape(
  shape: GeaIrStoreValueShape | null,
): shape is { kind: 'array'; element: { kind: 'object'; fields: GeaIrStoreField[] }; elementTypeName?: string } {
  return shape?.kind === 'array' && shape.element?.kind === 'object'
}

// The geatsc-declared interface struct symbol. Must match the symbol the
// geatsc collector emits (`__gea_type_<safeIdent(name)>` — see
// declarations/collector.ts). `sanitizeCppIdentifier` agrees with `safeIdent`
// for ordinary alphanumeric interface names, which is the only shape that
// reaches this path (named interface with all-primitive fields).
export function interfaceItemSymbol(name: string): string {
  return `__gea_type_${sanitizeCppIdentifier(name)}`
}

// SINGLE SOURCE OF TRUTH for the interface-reuse opt-in (see also Fix 3:
// `reconcileInterfaceStructReuse` below). The criterion here is the plugin's
// FIRST, optimistic gate; it is later RECONCILED against geatsc's actual
// emitted output so the plugin never references a `__gea_type_<Name>` struct
// geatsc did not emit.
//
// An array shape is eligible to reuse geatsc's interface struct when ALL hold:
//   1. it names a concrete interface element type (`shape.elementTypeName`);
//   2. every element field present in the initializer is primitive (a
//      `literal` shape) — non-all-primitive keeps the synthesized path so
//      geatsc's file-scope field widening (which demotes nested record/array
//      fields to gea_cpp_value) can't desync from our field-by-field access;
//   3. the interface name and every field name are PLAIN C++ identifiers whose
//      plugin spelling (`sanitizeCppIdentifier`) is verbatim. geatsc spells the
//      same names with `safeIdent`; the two agree only for plain identifiers
//      that neither sanitizer mangles. This guarantees the presence-flag
//      spelling (`__gea_has_<field>`) matches on both sides — a guard the
//      source-level struct-name reconciliation cannot see (it checks the struct
//      name, not per-field members).
//
// Note: criterion (2) is over INITIALIZER fields only (the plugin cannot see
// the declared interface), whereas geatsc decides over the DECLARED interface.
// They can diverge (interface has a non-primitive field the initializer omits).
// `reconcileInterfaceStructReuse` resolves that by downgrading to the
// synthesized path whenever geatsc did not actually emit the struct.
function arrayReusesInterfaceStruct(
  shape: { kind: 'array'; element: { kind: 'object'; fields: GeaIrStoreField[] }; elementTypeName?: string },
): boolean {
  if (!shape.elementTypeName) return false
  if (!isPlainCppIdentifier(shape.elementTypeName)) return false
  return shape.element.fields.every(
    (field) => fieldShape(field)?.kind === 'literal' && isPlainCppIdentifier(field.name),
  )
}

// True when `name` is a PLAIN C++ identifier (`[A-Za-z_][A-Za-z0-9_]*`). For
// such names the plugin's `sanitizeCppIdentifier` and geatsc's `safeIdent`
// provably produce the SAME spelling — including reserved/keyword/stdlib names,
// which BOTH mangle identically to `__gea_<name>` (e.g. `index` →
// `__gea_index`, `y0` → `__gea_y0`). This is the criterion the reviewer asked
// for ("field names are all plain identifiers"); a stricter verbatim-identity
// check would WRONGLY exclude reserved-but-agreeing names like `index` and
// desync the plugin from geatsc (which has no such guard and would still emit
// and type the property with the struct). Non-identifier names (numeric or
// symbol keys, `foo-bar`) are mangled with hex escapes (`_x2d_`) and are the
// only place the two could disagree, so they are excluded here.
function isPlainCppIdentifier(name: string): boolean {
  return /^[A-Za-z_][A-Za-z0-9_]*$/.test(name)
}

function isFieldReadableShape(shape: GeaIrStoreValueShape | null): shape is GeaIrStoreValueShape {
  return shape?.kind === 'literal' || isArrayObjectShape(shape)
}
