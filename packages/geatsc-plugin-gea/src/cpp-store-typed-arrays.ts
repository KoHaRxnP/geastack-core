// Source-level rewrite that turns store array fields whose IR shape is
// `Array<{...primitives...}>` from `std::vector<gea_cpp_value>` storage with
// dynamic record slots into `std::vector<X_item>` storage with real C++
// fields. The geatsc emitter (under the gea plugin's allow-any mode) emits
// the generic `gea_cpp_value`-shaped class skeleton; this pass localises the
// switch to typed storage to the spots that actually need it (field
// declaration, constructor seed, `__gea_to_value` getter/setter cast) and
// leaves the dynamic surface intact for everything else.
//
// The `set_array_item_field` lambda inside lowered store methods stays
// generic and dispatches via `if constexpr (std::is_same_v<Element,
// gea_cpp_value>)`. The typed branch calls `gea_ir::set_typed_field(slot,
// property, value)` (emitted per-item-type by `cpp-stores.ts`). Per-property
// reads via `gea_ir::read_number_field(item, "x")` resolve through typed
// overloads of the same helper that `cpp-stores.ts` also emits.
//
// This is purely a source-substitution pass over the existing geatsc-emitted
// classes — no app-specific logic. A store/field is eligible when its IR
// shape is `array<object<all-primitive-fields>>`.

import type { GeaIrBundleV1 } from './types.js'
import { sanitizeCppIdentifier } from './utils.js'

interface TypedFieldEdit {
  storeClass: string
  fieldName: string
  itemType: string
  readerName: string
  // The C++ element-type spelling at type-use sites. For interface reuse this
  // is `::${itemType}` (geatsc's global struct); for synthesized structs it is
  // `gea_ir::${itemType}` (the struct lives in `namespace gea_ir`).
  qualifiedItemType: string
  useInterfaceStruct: boolean
  // The item's primitive fields, in declaration order — used to construct the
  // ctor seed directly (`gea_ir::X_item(v0, v1, ...)`) instead of boxing each
  // record through a temporary `gea_cpp_value`. Empty when the item has any
  // non-primitive field (then the seed keeps the converting-ctor path).
  itemFields: Array<{ name: string; valueType: 'number' | 'string' | 'boolean' }>
}

// Extract an array field's primitive item fields, in order. Returns [] for any
// shape that isn't `array<object<all-primitive>>` (the only shape eligible for
// direct field-wise seed construction).
function primitiveItemFields(field: { shape?: unknown }): TypedFieldEdit['itemFields'] {
  const shape = field.shape as
    | { kind: string; element?: { kind: string; fields?: Array<{ name: string; shape?: { kind: string; valueType?: string } }> } }
    | undefined
  if (!shape || shape.kind !== 'array' || shape.element?.kind !== 'object' || !shape.element.fields) return []
  const out: TypedFieldEdit['itemFields'] = []
  for (const f of shape.element.fields) {
    const vt = f.shape?.kind === 'literal' ? f.shape.valueType : undefined
    if (vt !== 'number' && vt !== 'string' && vt !== 'boolean') return []
    out.push({ name: f.name, valueType: vt })
  }
  return out
}

export function applyTypedArrayStorage(source: string, ir: GeaIrBundleV1): string {
  const grouped = new Map<string, TypedFieldEdit[]>()
  for (const store of ir.stores) {
    const storeClass = sanitizeCppIdentifier(store.className)
    for (const field of store.fields) {
      if (!field.typedStorage) continue
      const list = grouped.get(storeClass) ?? []
      const useInterfaceStruct = field.typedStorage.useInterfaceStruct === true
      list.push({
        storeClass,
        fieldName: sanitizeCppIdentifier(field.name),
        itemType: field.typedStorage.itemType,
        readerName: field.typedStorage.readerName,
        // Interface reuse: geatsc emits `__gea_type_<Name>` at global scope, so
        // the store class field decl names it with a leading `::`. Synthesized:
        // the struct lives in `namespace gea_ir`, so qualify with `gea_ir::`.
        qualifiedItemType: useInterfaceStruct ? `::${field.typedStorage.itemType}` : `gea_ir::${field.typedStorage.itemType}`,
        useInterfaceStruct,
        itemFields: primitiveItemFields(field),
      })
      if (!grouped.has(storeClass)) grouped.set(storeClass, list)
    }
  }
  if (grouped.size === 0) return source

  let next = source
  for (const [storeClass, fields] of grouped) {
    next = rewriteClassBody(next, storeClass, (body) => rewriteFieldsInClassBody(body, fields))
    next = rewriteOutOfLineMemberBodies(next, storeClass, (body) => rewriteFieldsInMemberBody(body, fields))
    next = rewriteToValueSetterCast(next, storeClass, fields)
  }
  next = pruneUnusedRecordHelpers(next)
  return next
}

function rewriteFieldsInClassBody(body: string, fields: TypedFieldEdit[]): string {
  let next = body
  for (const field of fields) {
    next = rewriteFieldDeclaration(next, field)
    next = rewriteCtorSeed(next, field)
    // The __gea_to_value getter for each field uses `gea_cpp_key(self->X)`
    // which already picks up the new typed `gea_cpp_key(const X_item&)`
    // overload via std::vector<T> templating, so no rewrite needed here.
  }
  // Rebuild store-field pushes as typed item constructors when the array item
  // shape is known. This keeps static object-array data on native C++ fields
  // instead of flowing through `__gea_record_N`/`gea_cpp_key` boxers.
  for (const field of fields) {
    next = rewritePushBackItemsForField(next, field)
  }
  return next
}

// The module-split (typed-snapshot) emitter defines store members OUT-OF-LINE
// — the class definition with its field declarations goes to the layout
// header while `NotesStore::NotesStore() : Store() { ... }` and the methods
// land in the module .cpp. The class-body pass above never sees those bodies,
// so without this pass a typed field keeps its boxed
// `std::vector<gea_cpp_value>` constructor seed and the module fails to
// compile (`no viable overloaded '='`). Apply the same seed/push_back
// rewrites to every out-of-line member definition of the store class.
function rewriteFieldsInMemberBody(body: string, fields: TypedFieldEdit[]): string {
  let next = body
  for (const field of fields) {
    next = rewriteCtorSeed(next, field)
  }
  for (const field of fields) {
    next = rewritePushBackItemsForField(next, field)
  }
  return next
}

function rewriteOutOfLineMemberBodies(
  source: string,
  className: string,
  transform: (body: string) => string,
): string {
  const memberPattern = new RegExp(`\\b${escapeRegExp(className)}::[A-Za-z_~][A-Za-z0-9_]*\\s*\\(`, 'g')
  let next = source
  let searchFrom = 0
  for (;;) {
    memberPattern.lastIndex = searchFrom
    const match = memberPattern.exec(next)
    if (!match) break
    const paramsOpen = match.index + match[0].length - 1
    const paramsClose = findMatchingParen(next, paramsOpen)
    if (paramsClose < 0) break
    // Skip the member-init list (`: Store()`) to the body brace; hitting a `;`
    // first means this was a declaration or a call expression, not a
    // definition. (The store emitter never brace-initializes in init lists,
    // so the first `{` after the parameter list is always the body.)
    let cursor = paramsClose + 1
    while (cursor < next.length && next[cursor] !== '{' && next[cursor] !== ';') cursor += 1
    if (next[cursor] !== '{') {
      searchFrom = paramsClose + 1
      continue
    }
    const close = findMatchingBrace(next, cursor)
    if (close < 0) break
    const transformed = transform(next.slice(cursor + 1, close))
    next = next.slice(0, cursor + 1) + transformed + next.slice(close)
    searchFrom = cursor + 1 + transformed.length + 1
  }
  return next
}

// Rewrite `.push_back(__gea_record_N{...})` or
// `.push_back(gea_cpp_key(__gea_record_N{...}))` only on call sites whose
// receiver expression names the typed store field. geatsc has emitted both an
// older IIFE receiver pattern and a newer direct `(*this).field.push_back(...)`
// pattern, so handle both.
function rewritePushBackItemsForField(body: string, field: TypedFieldEdit): string {
  let next = rewritePushBackItemsAfterNeedle(
    body,
    field,
    `return ((*this).${field.fieldName});`,
    ` })().push_back(`,
  )
  next = rewritePushBackItemsAfterNeedle(
    next,
    field,
    `(*this).${field.fieldName}`,
    `.push_back(`,
  )
  return next
}

function rewritePushBackItemsAfterNeedle(body: string, field: TypedFieldEdit, needle: string, tailFragment: string): string {
  let cursor = 0
  let out = ''
  while (true) {
    const needleIdx = body.indexOf(needle, cursor)
    if (needleIdx < 0) {
      out += body.slice(cursor)
      break
    }
    const tailStart = needleIdx + needle.length
    if (body.startsWith(tailFragment, tailStart)) {
      const argStart = tailStart + tailFragment.length
      const pushClose = findMatchingParen(body, argStart - 1)
      if (pushClose < 0) {
        out += body.slice(cursor, tailStart) + body.slice(tailStart)
        cursor = body.length
        break
      }
      const originalArg = body.slice(argStart, pushClose).trim()
      const unwrapped = unwrapOuterCall(originalArg, 'gea_cpp_key') ?? originalArg
      const nativeItem = buildNativeTypedItem(unwrapped, field)
      if (nativeItem || unwrapped !== originalArg) {
        out += body.slice(cursor, argStart)
        out += nativeItem ?? unwrapped
        cursor = pushClose
        continue
      }
    }
    out += body.slice(cursor, tailStart)
    cursor = tailStart
  }
  return out
}

function unwrapOuterCall(expr: string, callName: string): string | null {
  const trimmed = expr.trim()
  const prefix = `${callName}(`
  if (!trimmed.startsWith(prefix)) return null
  const close = findMatchingParen(trimmed, prefix.length - 1)
  if (close !== trimmed.length - 1) return null
  return trimmed.slice(prefix.length, close).trim()
}

function buildNativeTypedItem(element: string, field: TypedFieldEdit): string | null {
  if (field.useInterfaceStruct || field.itemFields.length === 0) return null
  const pairs = parseAggregateRecordElement(element)
  if (!pairs) return null
  for (const key of pairs.keys()) {
    if (!field.itemFields.some((f) => f.name === key)) return null
  }
  const args = field.itemFields.map((f) => {
    const value = pairs.get(f.name)
    const raw = value ?? (f.valueType === 'string' ? 'std::string()' : f.valueType === 'boolean' ? 'false' : '0')
    return castNativeTypedCtorArg(raw, f.valueType)
  })
  return `${field.qualifiedItemType}(${args.join(', ')})`
}

function parseAggregateRecordElement(element: string): Map<string, string> | null {
  const recordIdx = element.indexOf('__gea_record_')
  if (recordIdx < 0) return null
  const braceOpen = element.indexOf('{', recordIdx)
  if (braceOpen < 0) return null
  const braceClose = findMatchingBrace(element, braceOpen)
  if (braceClose < 0) return null
  const body = element.slice(braceOpen + 1, braceClose)
  const pairs = new Map<string, string>()
  for (const part of splitTopLevelArgs(body)) {
    const match = /^\s*\.([A-Za-z_]\w*)\s*=\s*([\s\S]+?)\s*$/.exec(part)
    if (!match) return null
    const name = match[1]
    if (name.startsWith('__gea_has_')) continue
    pairs.set(name, match[2])
  }
  return pairs.size > 0 ? pairs : null
}

// Turn the comma-separated boxed record builders of a ctor seed into a typed
// vector initializer that constructs each item through its field-wise ctor.
// Returns the `{...}` initializer (braces included) or null to signal the
// caller should keep the boxed converting-ctor seed.
function buildNativeTypedSeed(seedInner: string, field: TypedFieldEdit): string | null {
  // Interface-struct items carry presence flags the reader sets, so keep those
  // on their existing reader/conversion path. Synthesized primitive items can
  // be built directly, including one-field shapes such as collision indexes.
  if (field.useInterfaceStruct || field.itemFields.length === 0) return null
  const elements = splitTopLevelArgs(seedInner)
  if (elements.length === 0) return null
  const built: string[] = []
  for (const element of elements) {
    const pairs = parseSeedRecordElement(element)
    if (!pairs) return null
    // A seed key with no matching struct field means we'd silently drop data —
    // bail to the boxed path rather than emit a wrong constructor call.
    for (const key of pairs.keys()) {
      if (!field.itemFields.some((f) => f.name === key)) return null
    }
    const args = field.itemFields.map((f) => {
      const value = pairs.get(f.name)
      if (value !== undefined) return castNativeTypedCtorArg(value, f.valueType)
      // Omitted key → the record literal didn't set it; use the zero value.
      return f.valueType === 'string' ? 'std::string()' : f.valueType === 'boolean' ? 'false' : 'static_cast<double>(0)'
    })
    built.push(`${field.qualifiedItemType}(${args.join(', ')})`)
  }
  return `{${built.join(', ')}}`
}

function castNativeTypedCtorArg(value: string, valueType: 'number' | 'string' | 'boolean'): string {
  if (valueType === 'number') return `static_cast<double>(${value})`
  if (valueType === 'boolean') return `static_cast<bool>(${value})`
  return `std::string(${value})`
}

// Split a brace/paren/bracket- and string-aware comma list at depth 0.
function splitTopLevelArgs(source: string): string[] {
  const parts: string[] = []
  let depth = 0
  let quote: string | null = null
  let escaped = false
  let start = 0
  for (let i = 0; i < source.length; i += 1) {
    const ch = source[i]
    if (quote) {
      if (escaped) escaped = false
      else if (ch === '\\') escaped = true
      else if (ch === quote) quote = null
      continue
    }
    if (ch === '"' || ch === "'") quote = ch
    else if (ch === '(' || ch === '{' || ch === '[') depth += 1
    else if (ch === ')' || ch === '}' || ch === ']') depth -= 1
    else if (ch === ',' && depth === 0) {
      parts.push(source.slice(start, i))
      start = i + 1
    }
  }
  parts.push(source.slice(start))
  return parts.map((p) => p.trim()).filter((p) => p.length > 0)
}

// Parse one boxed record-builder IIFE (`([&]() { gea_cpp_value r; ...
// r.record_set_literal("k", V); ...; return r; })()`) into a key→value-expr
// map. Returns null for any element that isn't exactly that shape or whose
// values are themselves boxed (nested records can't assign to a typed field).
function parseSeedRecordElement(element: string): Map<string, string> | null {
  if (!element.includes('gea_cpp_value::kind_t::record')) return null
  const pairs = new Map<string, string>()
  const needle = '.record_set_literal('
  let cursor = 0
  while (true) {
    const at = element.indexOf(needle, cursor)
    if (at < 0) break
    const open = at + needle.length - 1
    const close = findMatchingParen(element, open)
    if (close < 0) return null
    const args = splitTopLevelArgs(element.slice(open + 1, close))
    if (args.length !== 2) return null
    const keyMatch = /^"((?:[^"\\]|\\.)*)"$/.exec(args[0])
    if (!keyMatch) return null
    const key = keyMatch[1].replace(/\\(.)/g, '$1')
    if (!/^[A-Za-z_]\w*$/.test(key)) return null
    const value = args[1]
    if (!value || /\bgea_cpp_value\b/.test(value) || value.includes('record_set_literal')) return null
    pairs.set(key, value)
    cursor = close + 1
  }
  return pairs.size > 0 ? pairs : null
}

function findMatchingParen(source: string, openParen: number): number {
  let depth = 0
  let quote: string | null = null
  let escaped = false
  for (let i = openParen; i < source.length; i += 1) {
    const ch = source[i]
    if (quote) {
      if (escaped) escaped = false
      else if (ch === '\\') escaped = true
      else if (ch === quote) quote = null
      continue
    }
    if (ch === '"' || ch === "'") {
      quote = ch
      continue
    }
    if (ch === '(') depth += 1
    else if (ch === ')') {
      depth -= 1
      if (depth === 0) return i
    }
  }
  return -1
}

// `mutable std::vector<gea_cpp_value> ${name}{};` → `mutable std::vector<gea_ir::${itemType}> ${name}{};`.
// The typed item struct is emitted inside `namespace gea_ir { ... }` by
// `cpp-stores.ts`, but store classes live outside that namespace, so we
// fully qualify here.
function rewriteFieldDeclaration(body: string, field: TypedFieldEdit): string {
  const pattern = new RegExp(
    `\\bmutable\\s+std::vector<\\s*gea_cpp_value\\s*>\\s+${escapeRegExp(field.fieldName)}(\\s*\\{\\s*\\}\\s*;)`,
  )
  return body.replace(pattern, `mutable std::vector<${field.qualifiedItemType}> ${field.fieldName}$1`)
}

// `this->${name} = std::vector<gea_cpp_value>{...records...};` → typed seeds.
//
// The seed records carry the store's initial array data (e.g. a notes app's
// pre-populated `notes = [{...}, ...]`). The typed field is now a
// `std::vector<X_item>`, which can't hold `gea_cpp_value` directly, so we
// keep the original record literals in a temporary `std::vector<gea_cpp_value>`
// and convert each element through `X_item`'s `gea_cpp_value` constructor.
// This preserves seed data instead of dropping it — important for stores
// whose array IS the data source (not a placeholder an `init()` rebuilds).
// Stores that DO clear+rebuild in `init()` (e.g. button-tetris's single
// placeholder) are unaffected: the seed is materialised then immediately
// overwritten.
function rewriteCtorSeed(body: string, field: TypedFieldEdit): string {
  // The emitter produces the store array-literal initializer in one of two shapes:
  //   direct:       this->X = std::vector<gea_cpp_value>{ ELEMENTS };
  //   IIFE-wrapped: this->X = ([&]() { std::vector<gea_cpp_value> __gea_array =
  //                   std::vector<gea_cpp_value>{ ELEMENTS };
  //                   ...native-property-attribute bookkeeping...; return __gea_array; })();
  // (the IIFE form is what the typed-runtime-coverage codegen emits so the array
  // carries native property attributes). Either way the typed field is now a
  // `std::vector<X_item>`, which can't hold `gea_cpp_value`, so we must rewrite the
  // seed to typed items. Locate the ELEMENTS brace and the end of the whole
  // assignment statement for both shapes.
  const assignPattern = new RegExp(`this->${escapeRegExp(field.fieldName)}\\s*=\\s*`)
  const match = assignPattern.exec(body)
  if (!match) return body
  const prefix = match[0]
  const afterEq = match.index + prefix.length

  let seedOpen = -1
  let stmtEnd = -1
  if (/^std::vector<\s*gea_cpp_value\s*>\s*\{/.test(body.slice(afterEq))) {
    seedOpen = body.indexOf('{', afterEq)
    const close = findMatchingBrace(body, seedOpen)
    if (close < 0) return body
    let e = close + 1
    while (e < body.length && /\s/.test(body[e])) e += 1
    if (body[e] === ';') e += 1
    stmtEnd = e
  } else if (body[afterEq] === '(') {
    // `([&]() { ... })()` — find the lambda group's close, then the `()` call.
    const groupClose = findMatchingParen(body, afterEq)
    if (groupClose < 0) return body
    let e = groupClose + 1
    while (e < body.length && /\s/.test(body[e])) e += 1
    if (body[e] === '(') {
      const invokeClose = findMatchingParen(body, e)
      if (invokeClose < 0) return body
      e = invokeClose + 1
    }
    while (e < body.length && /\s/.test(body[e])) e += 1
    if (body[e] === ';') e += 1
    stmtEnd = e
    // The seed vector is the `= std::vector<gea_cpp_value>{` inside the IIFE (the
    // second `std::vector<gea_cpp_value>` — the first is the local's declared type).
    const innerPattern = /=\s*std::vector<\s*gea_cpp_value\s*>\s*\{/g
    innerPattern.lastIndex = afterEq
    const inner = innerPattern.exec(body)
    if (!inner || inner.index > groupClose) return body
    seedOpen = inner.index + inner[0].length - 1
  } else {
    return body
  }

  const seedClose = findMatchingBrace(body, seedOpen)
  if (seedClose < 0) return body
  // `{ ...record literals... }` — the brace-delimited initializer contents,
  // braces included.
  const seedBraces = body.slice(seedOpen, seedClose + 1)
  const seedInner = body.slice(seedOpen + 1, seedClose).trim()
  const fieldRef = `this->${field.fieldName}`
  // No seed elements → just the empty typed vector.
  if (seedInner.length === 0) {
    const empty = `${prefix}std::vector<${field.qualifiedItemType}>{};`
    return body.slice(0, match.index) + empty + body.slice(stmtEnd)
  }
  // Preferred path: construct each item directly through its field-wise ctor —
  // `std::vector<gea_ir::X_item>{ gea_ir::X_item(0, 0, "#000"), ... }` — with no
  // boxed `gea_cpp_value` anywhere. Falls back to the converting-ctor seed below
  // for anything that doesn't cleanly parse (nested objects, dynamic values,
  // interface-struct items with presence flags, single-field items).
  const nativeSeed = buildNativeTypedSeed(seedInner, field)
  if (nativeSeed) {
    const seeded = `${prefix}std::vector<${field.qualifiedItemType}>${nativeSeed};`
    return body.slice(0, match.index) + seeded + body.slice(stmtEnd)
  }
  const seedVar = `__gea_seed_${field.fieldName}`
  // Materialise each seed record. The synthesized item struct has an explicit
  // `gea_cpp_value`-converting ctor (`gea_ir::X_item(v)`); geatsc's interface
  // struct does NOT, so route those through the reader fn
  // (`gea_ir::read___gea_type_X(v)` → `__gea_from_value`, which sets presence
  // flags) instead.
  const elementFromSeed = field.useInterfaceStruct
    ? `gea_ir::read_${field.itemType}(__gea_seed_value)`
    : `gea_ir::${field.itemType}(__gea_seed_value)`
  const replacement =
    `${prefix}std::vector<${field.qualifiedItemType}>{}; ` +
    `{ std::vector<gea_cpp_value> ${seedVar} = std::vector<gea_cpp_value>${seedBraces}; ` +
    `${fieldRef}.reserve(${seedVar}.size()); ` +
    `for (auto &__gea_seed_value : ${seedVar}) ${fieldRef}.push_back(${elementFromSeed}); }`
  return body.slice(0, match.index) + replacement + body.slice(stmtEnd)
}

// In `__gea_to_value`'s setter lambda, the cast for typed-array fields:
//   __gea_self->X = ([](auto &&v) -> std::vector<gea_cpp_value> { ... })(value);
// becomes:
//   __gea_self->X = gea_ir::read_${storeClass}_${fieldName}(gea_cpp_value(value));
function rewriteToValueSetterCast(source: string, storeClass: string, fields: TypedFieldEdit[]): string {
  let next = source
  for (const field of fields) {
    // The whole setter cast IIFE for this field starts with `__gea_self->X = ([...`
    // and ends at the `)(__gea_value);` matching the lambda's invocation.
    const startPattern = new RegExp(
      `__gea_self->${escapeRegExp(field.fieldName)}\\s*=\\s*\\(\\s*\\[`,
    )
    const startMatch = startPattern.exec(next)
    if (!startMatch) continue
    const start = startMatch.index
    // Find the IIFE's outer parenthesised expression: ( [...](...) -> ... { ... } )(__gea_value)
    // Easiest: scan from `start` to find the `)(__gea_value);` close. The
    // call-arg `__gea_value` is a literal in the geatsc-emitted code.
    const tailPattern = /\)\(__gea_value\);/g
    tailPattern.lastIndex = start
    const tailMatch = tailPattern.exec(next)
    if (!tailMatch) continue
    const end = tailMatch.index + tailMatch[0].length
    const replacement = `__gea_self->${field.fieldName} = gea_ir::${field.readerName}(gea_cpp_value(__gea_value));`
    // Restrict to the `BallStore::__gea_to_value` (or member equivalent) so
    // we don't accidentally clobber unrelated `__gea_self->X = (...)(value)`
    // patterns elsewhere. The setter lambda always lives inside the
    // `__gea_to_value()` body for this class.
    const guardPattern = new RegExp(
      `\\b${escapeRegExp(storeClass)}::__gea_to_value\\b|\\bvirtual\\s+gea_cpp_value\\s+__gea_to_value\\b`,
    )
    const before = next.slice(0, start)
    if (!guardPattern.test(before)) continue
    next = next.slice(0, start) + replacement + next.slice(end)
  }
  return next
}

function rewriteClassBody(source: string, className: string, transform: (body: string) => string): string {
  const open = findClassOpenBrace(source, className)
  if (open < 0) return source
  const close = findMatchingBrace(source, open)
  if (close < 0) return source
  const transformed = transform(source.slice(open + 1, close))
  return source.slice(0, open + 1) + transformed + source.slice(close)
}

function findClassOpenBrace(source: string, className: string): number {
  const pattern = new RegExp(`\\bclass\\s+${escapeRegExp(className)}\\b`, 'g')
  let match: RegExpExecArray | null
  while ((match = pattern.exec(source)) !== null) {
    const brace = source.indexOf('{', match.index)
    if (brace < 0) return -1
    const semicolon = source.indexOf(';', match.index)
    if (semicolon < 0 || brace < semicolon) return brace
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
      continue
    }
    if (char === '"' || char === "'") {
      quote = char
      continue
    }
    if (char === '{') depth += 1
    else if (char === '}') {
      depth -= 1
      if (depth === 0) return index
    }
  }
  return -1
}

function pruneUnusedRecordHelpers(source: string): string {
  let next = source
  let changed = true
  while (changed) {
    changed = false
    const pattern = /\nstruct\s+(__gea_record_\d+)\s*\{/g
    let match: RegExpExecArray | null
    while ((match = pattern.exec(next)) !== null) {
      const name = match[1]
      const structOpen = next.indexOf('{', match.index)
      if (structOpen < 0) continue
      const structClose = findMatchingBrace(next, structOpen)
      if (structClose < 0) continue
      let removeEnd = structClose + 1
      while (removeEnd < next.length && /\s/.test(next[removeEnd])) removeEnd += 1
      if (next[removeEnd] !== ';') continue
      removeEnd += 1

      const helperStart = skipWhitespace(next, removeEnd)
      const helperPrefix = `inline gea_cpp_value gea_cpp_key(const ${name} &__gea_v)`
      if (next.startsWith(helperPrefix, helperStart)) {
        const helperOpen = next.indexOf('{', helperStart + helperPrefix.length)
        if (helperOpen < 0) continue
        const helperClose = findMatchingBrace(next, helperOpen)
        if (helperClose < 0) continue
        removeEnd = helperClose + 1
      }
      while (removeEnd < next.length && (next[removeEnd] === '\n' || next[removeEnd] === '\r')) removeEnd += 1

      const candidate = next.slice(0, match.index) + '\n' + next.slice(removeEnd)
      if (!new RegExp(`\\b${escapeRegExp(name)}\\b`).test(candidate)) {
        next = candidate
        changed = true
        break
      }
    }
    if (changed) continue
    const helperPattern = /\ninline\s+gea_cpp_value\s+gea_cpp_key\(const\s+(__gea_record_\d+)\s+&__gea_v\)\s*\{/g
    let helperMatch: RegExpExecArray | null
    while ((helperMatch = helperPattern.exec(next)) !== null) {
      const name = helperMatch[1]
      const helperOpen = next.indexOf('{', helperMatch.index)
      if (helperOpen < 0) continue
      const helperClose = findMatchingBrace(next, helperOpen)
      if (helperClose < 0) continue
      let removeEnd = helperClose + 1
      while (removeEnd < next.length && (next[removeEnd] === '\n' || next[removeEnd] === '\r')) removeEnd += 1
      const candidate = next.slice(0, helperMatch.index) + '\n' + next.slice(removeEnd)
      const structStillExists = new RegExp(`\\nstruct\\s+${escapeRegExp(name)}\\s*\\{`).test(candidate)
      if (!structStillExists || !new RegExp(`\\b${escapeRegExp(name)}\\b`).test(candidate)) {
        next = candidate
        changed = true
        break
      }
    }
  }
  return next
}

function skipWhitespace(source: string, index: number): number {
  let cursor = index
  while (cursor < source.length && /\s/.test(source[cursor])) cursor += 1
  return cursor
}

function escapeRegExp(text: string): string {
  return text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
}
