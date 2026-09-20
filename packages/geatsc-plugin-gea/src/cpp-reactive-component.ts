// EXPERIMENTAL (component-as-store): class-level source transforms for the
// LEAN, FULLY-TYPED ReactiveComponent path.
//
// A `class App extends ReactiveComponent` compiles (via the vite-plugin) to a
// base-less, typed C++ class: `mutable double count{}`, `void bump()`,
// `std::string label()`. RENDERING goes through the ONE shared mounted renderer
// (cpp-template-renderer) with `TypedSelfFieldAccess` — see cpp-mounted.ts
// `selfStoreMountedRenderer`. This module only contains the class/source surgery
// that makes the typed class reactive:
//   1. `rewriteReactiveComponentFields` — each primitive reactive field becomes a
//      typed `gea::embedded::ui::Signal<T>` (assignment notifies subscribers).
//   2. `inlineReactiveComponentGetters` — substitutes simple single-return getter
//      reads in template slots (`{this.label}` → `{'n=' + this.count}`) so they
//      lower through the ordinary store-field path and re-render on the fields
//      the getter actually reads.
//   3. `neutralizeReactiveComponentToValue` — blanks the dead auto-emitted
//      `__gea_to_value()` so it never boxes a Signal field.
//   4. `insertReactiveSignalSupport` — global `gea_cpp_to_string(Signal<T>)`
//      overload so getter bodies stringify the value, not "[object Object]".
//   5. `insertReactiveRendererForwardDeclarations` — declares the typed
//      `mount_<Name>(shared_ptr<Name>&, …)` after `class <Name>;` (the mount call
//      site precedes the renderer definition).

import { sanitizeCppIdentifier } from './utils.js'
import type { GeaIrComponent } from './types.js'

export function isReactiveComponentIr(component: GeaIrComponent): boolean {
  return !!component.reactiveState
}

interface FieldInfo {
  cppType: string
}

function cppTypeForValueType(valueType: string | undefined): FieldInfo {
  if (valueType === 'string') return { cppType: 'std::string' }
  if (valueType === 'boolean') return { cppType: 'bool' }
  return { cppType: 'double' }
}

// Primitive reactive fields → Signal<T>. Arrays/records are left as plain typed
// members for now (no Signal, no typed reader) — templates reading them fall off
// the fast path loudly rather than emitting broken code.
function reactiveFieldInfo(component: GeaIrComponent): Map<string, FieldInfo> {
  const map = new Map<string, FieldInfo>()
  for (const field of component.reactiveState?.fields ?? []) {
    if (field.shape && field.shape.kind !== 'literal') continue
    map.set(field.name, cppTypeForValueType(field.shape?.kind === 'literal' ? field.shape.valueType : undefined))
  }
  return map
}

export interface ReactiveArrayFieldInfo {
  name: string
  elementCppType: string
  elementValueType: 'string' | 'number' | 'boolean'
}

// Reactive ARRAY fields with primitive elements (`cells: string[]`). The field
// stays a plain `std::vector<T>` (so all geatsc-compiled method bodies — element
// reads, vector_set writes, the record_get_property fallback — keep compiling
// untouched); reactivity comes from a companion `Signal<double> <name>__rev`
// that mutation helpers tick and the keyed-list renderer subscribes to.
export function reactiveArrayFieldInfo(component: GeaIrComponent): ReactiveArrayFieldInfo[] {
  const out: ReactiveArrayFieldInfo[] = []
  for (const field of component.reactiveState?.fields ?? []) {
    if (field.shape?.kind !== 'array') continue
    const element = field.shape.element
    if (!element || element.kind !== 'literal') continue
    const valueType = element.valueType === 'string' || element.valueType === 'boolean' ? element.valueType : 'number'
    out.push({
      name: field.name,
      elementCppType: cppTypeForValueType(valueType).cppType,
      elementValueType: valueType,
    })
  }
  return out
}

export function arrayRevFieldName(fieldName: string): string {
  return `${sanitizeCppIdentifier(fieldName)}__rev`
}

// ── getter inlining (template-slot rewrite, applied once at IR-load) ──────────

// Rewrite a self-store component's template slots so each `this.<getter>`
// reference is replaced by the getter's return expression (over the component's
// own fields). The renderer's expression lowering resolves store FIELDS, not
// getters; inlining a simple single-return getter (`get label() { return 'n=' +
// this.count }`) turns `{this.label}` into `{'n=' + this.count}`, which lowers
// through the existing field path and re-renders on the same fields the getter
// reads (`count`). Getters with non-trivial bodies are left as-is — the
// component then falls off the fast path exactly as before. Idempotent (after
// the first pass no `this.<getter>` reference remains).
export function inlineReactiveComponentGetters(component: GeaIrComponent): GeaIrComponent {
  const getters = component.reactiveState?.getters
  if (!getters || getters.length === 0) return component
  const exprByName = new Map<string, string>()
  for (const getter of getters) {
    const expr = getterReturnExpression(getter.body)
    if (expr !== null) exprByName.set(getter.name, expr)
  }
  if (exprByName.size === 0) return component
  // Expand getter-in-getter references to a fixed point (bounded; real getters
  // never chain anywhere near this deep, and the bound stops degenerate cycles).
  for (let pass = 0; pass < 8; pass += 1) {
    let changed = false
    for (const [name, expr] of exprByName) {
      const expanded = substituteGetterReferences(expr, exprByName, name)
      if (expanded !== expr) {
        exprByName.set(name, expanded)
        changed = true
      }
    }
    if (!changed) break
  }
  let touched = false
  const slots = component.template.slots.map((slot) => {
    const nextExpr = slot.expr ? substituteGetterReferences(slot.expr, exprByName) : slot.expr
    const nextFields = slot.exprObjectFields?.map((field) => ({
      ...field,
      expr: substituteGetterReferences(field.expr, exprByName),
    }))
    const exprChanged = nextExpr !== slot.expr
    const fieldsChanged = !!nextFields && nextFields.some((field, index) => field.expr !== slot.exprObjectFields![index].expr)
    if (!exprChanged && !fieldsChanged) return slot
    touched = true
    const next: GeaIrComponent['template']['slots'][number] = { ...slot }
    if (exprChanged) {
      next.expr = nextExpr
      // No longer a bare `this.<field>` path; drop the stale path so lowering
      // uses the substituted `expr` string.
      delete next.exprPath
    }
    if (fieldsChanged) next.exprObjectFields = nextFields
    return next
  })
  if (!touched) return component
  return { ...component, template: { ...component.template, slots } }
}

// Extract the single returned expression from a getter body (`{ return <expr> }`).
// Returns null for anything that isn't exactly one return of an expression.
function getterReturnExpression(body: string | undefined): string | null {
  if (!body) return null
  let text = body.trim()
  if (text.startsWith('{') && text.endsWith('}')) text = text.slice(1, -1).trim()
  const match = text.match(/^return\b([\s\S]*?);?$/)
  if (!match) return null
  const expr = match[1].trim()
  if (!expr || expr.includes(';')) return null
  return expr
}

function substituteGetterReferences(expr: string, exprByName: Map<string, string>, excludeName?: string): string {
  let out = expr
  for (const [name, replacement] of exprByName) {
    if (name === excludeName) continue
    out = out.replace(new RegExp(`\\bthis\\.${name}\\b`, 'g'), `(${replacement})`)
  }
  return out
}

// ── source transforms applied to the generated program ─────────────────────────

// Rewrite each reactive field's declaration in its class body to a typed Signal.
// `mutable double count{};` → `mutable gea::embedded::ui::Signal<double> count{};`
export function rewriteReactiveComponentFields(source: string, components: GeaIrComponent[]): string {
  let next = source
  for (const component of components) {
    if (!component.reactiveState) continue
    const className = sanitizeCppIdentifier(component.exportName)
    const fields = reactiveFieldInfo(component)
    const arrayFields = reactiveArrayFieldInfo(component)
    next = transformClassBody(next, className, (bodyText) => {
      let body = bodyText
      for (const [name, info] of fields) {
        const ident = sanitizeCppIdentifier(name)
        const pattern = new RegExp(`(\\bmutable\\s+)${escapeRegExp(info.cppType)}(\\s+${escapeRegExp(ident)}\\s*\\{)`, 'g')
        body = body.replace(pattern, `$1gea::embedded::ui::Signal<${info.cppType}>$2`)
        // geatsc's generic cast lambdas (`([](const auto &v){ if constexpr
        // (is_same_v<decay_t, std::string>) return v; else return
        // gea_cpp_to_string(gea_cpp_value(v)); })((*this).turn)`) dispatch on
        // the DECLARED type — a Signal field takes the boxing branch and
        // stringifies as "[object Undefined]". Unwrap the read so the lambda
        // sees the plain T value.
        body = body.replaceAll(`})((*this).${ident})`, `})((*this).${ident}.get())`)
      }
      for (const arrayField of arrayFields) {
        const ident = sanitizeCppIdentifier(arrayField.name)
        const rev = arrayRevFieldName(arrayField.name)
        // Companion revision Signal, declared right after the vector field. The
        // vector itself stays plain so geatsc's array codegen compiles untouched.
        const declPattern = new RegExp(
          `(\\bmutable\\s+std::vector<${escapeRegExp(arrayField.elementCppType)}>\\s+${escapeRegExp(ident)}\\s*\\{\\s*\\};)`,
        )
        body = body.replace(declPattern, `$1\n  mutable gea::embedded::ui::Signal<double> ${rev}{};`)
        // Route element writes through the notify wrapper so the keyed-list
        // renderer re-renders. Only vector_set for now (the only mutation geatsc
        // emits for `arr[i] = v`); other mutators (push/splice) get wrapped when
        // a component actually uses them.
        body = body.replaceAll(
          `gea::runtime::array::vector_set((*this).${ident}, `,
          `gea_rc_vector_set_notify((*this).${ident}, (*this).${rev}, `,
        )
      }
      return body
    })
  }
  return next
}

// geatsc compiles a getter (`get label() { return 'n=' + this.count }`) to
// `gea_cpp_to_string((*this).count)`. With `count` rewritten to `Signal<double>`,
// that bare read otherwise resolves to the generic `gea_cpp_value` overload and
// stringifies the Signal as "[object Object]". Inject an exact-match
// `gea_cpp_to_string(const Signal<T>&)` (found by ordinary lookup) right before
// the first reactive class DEFINITION — after the runtime declares
// `gea_cpp_to_string`, before any reactive read uses it. (Not in signal.h: that
// header is included before the runtime's `gea_cpp_to_string` is declared.)
export function insertReactiveSignalSupport(source: string, components: GeaIrComponent[]): string {
  const reactive = components.filter((component) => component.reactiveState)
  if (reactive.length === 0) return source
  let earliest = -1
  for (const component of reactive) {
    const at = findClassDefinition(source, sanitizeCppIdentifier(component.exportName))
    if (at >= 0 && (earliest < 0 || at < earliest)) earliest = at
  }
  if (earliest < 0) return source
  // Global scope (NOT inside gea::embedded::ui): an overload inside that
  // namespace would hide the global `gea_cpp_to_string(double)` from its own
  // body (unqualified lookup stops at the first scope that declares the name).
  const overload =
    '// @geastack/geatsc-plugin-gea ReactiveComponent typed-Signal stringify support\n' +
    'template <typename T> inline std::string gea_cpp_to_string(const gea::embedded::ui::Signal<T> &__gea_signal) { return gea_cpp_to_string(__gea_signal.get()); }\n' +
    // Element write + revision tick for reactive array fields. Same contract as
    // gea::runtime::array::vector_set, plus notifying the companion Signal so
    // keyed-list subscribers re-render. (The runtime's array helpers are
    // declared by runtime_pch.h, included at the very top of the program.)
    'template <typename T, typename I, typename U, typename S> inline T gea_rc_vector_set_notify(std::vector<T> &__gea_vec, gea::embedded::ui::Signal<S> &__gea_rev, I __gea_index, U &&__gea_value) { T __gea_out = gea::runtime::array::vector_set(__gea_vec, __gea_index, std::forward<U>(__gea_value)); __gea_rev.notify(); return __gea_out; }\n'
  return `${source.slice(0, earliest)}${overload}${source.slice(earliest)}`
}

// Locate a class DEFINITION `class <Name> {` (not the `class <Name>;` forward
// declaration). Returns the index of the `class` keyword, or -1.
function findClassDefinition(source: string, className: string): number {
  const pattern = new RegExp(`\\bclass\\s+${escapeRegExp(className)}\\b`, 'g')
  let match: RegExpExecArray | null
  while ((match = pattern.exec(source)) !== null) {
    const brace = source.indexOf('{', match.index)
    const semicolon = source.indexOf(';', match.index)
    if (brace >= 0 && (semicolon < 0 || brace < semicolon)) return match.index
  }
  return -1
}

// Inject the typed mounted-renderer forward declaration right after the class's
// own forward declaration (`class App;`). It names `App`, so it can't sit in the
// early `gea_ir` namespace block that precedes the class forward declaration; it
// must come after `class App;` but before `__gea_top_level` (the mount site).
// An unused declaration (template unmountable → no renderer emitted) is harmless.
export function insertReactiveRendererForwardDeclarations(source: string, components: GeaIrComponent[]): string {
  let next = source
  for (const component of components) {
    if (!component.reactiveState) continue
    const className = sanitizeCppIdentifier(component.exportName)
    const decl = `class ${className};`
    const at = next.indexOf(decl)
    if (at < 0) continue
    const insertAt = at + decl.length
    // Disposer type MUST match the renderer definition emitted by
    // cpp-template-renderer.ts (`const std::shared_ptr<NativeDisposer> &`). A
    // stale `gea_cpp_value` here declares an inline overload that's never
    // defined, which ESP's `-Werror=undefined-inline` rejects.
    // This forward declaration sits right after `class App;`, which is BEFORE
    // the `namespace gea_ir { struct NativeDisposer; … }` runtime block. A
    // `shared_ptr<NativeDisposer>` parameter only needs an incomplete type, so
    // forward-declare `NativeDisposer` (in the same `gea_ir` namespace it is
    // later defined in) here too — otherwise GCC rejects the use-before-decl
    // (`'NativeDisposer' was not declared in this scope`), which then cascades
    // into the param decaying to `const int&`. clang was lenient; GCC is not.
    const forward = `\nnamespace gea_ir { struct NativeDisposer; gea::embedded::ui::NodeHandle mount_${className}(const std::shared_ptr<${className}> &store, const std::shared_ptr<NativeDisposer> &disposer); }`
    next = `${next.slice(0, insertAt)}${forward}${next.slice(insertAt)}`
  }
  return next
}

// Blank the auto-emitted `__gea_to_value()` for a reactive component so it never
// boxes a Signal field (it is dead — the typed mount/renderer never call it).
export function neutralizeReactiveComponentToValue(source: string, components: GeaIrComponent[]): string {
  let next = source
  for (const component of components) {
    if (!component.reactiveState) continue
    const className = sanitizeCppIdentifier(component.exportName)
    const head = `gea_cpp_value ${className}::__gea_to_value() const {`
    const start = next.indexOf(head)
    if (start < 0) continue
    const open = start + head.length - 1
    const close = matchBrace(next, open)
    if (close < 0) continue
    next = `${next.slice(0, open + 1)} (void)this; return gea_cpp_value::missing(); ${next.slice(close)}`
  }
  return next
}

// ── small brace utilities (self-contained) ─────────────────────────────────────

function escapeRegExp(text: string): string {
  return text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
}

function transformClassBody(source: string, className: string, transform: (body: string) => string): string {
  const pattern = new RegExp(`\\bclass\\s+${escapeRegExp(className)}\\b`, 'g')
  let match: RegExpExecArray | null
  while ((match = pattern.exec(source)) !== null) {
    const brace = source.indexOf('{', match.index)
    if (brace < 0) continue
    const semicolon = source.indexOf(';', match.index)
    if (semicolon >= 0 && semicolon < brace) continue // forward declaration
    const close = matchBrace(source, brace)
    if (close < 0) continue
    const transformed = transform(source.slice(brace + 1, close))
    return `${source.slice(0, brace + 1)}${transformed}${source.slice(close)}`
  }
  return source
}

function matchBrace(source: string, open: number): number {
  let depth = 0
  let quote: string | null = null
  let escaped = false
  for (let i = open; i < source.length; i += 1) {
    const ch = source[i]
    if (quote) {
      if (escaped) escaped = false
      else if (ch === '\\') escaped = true
      else if (ch === quote) quote = null
      continue
    }
    if (ch === '"' || ch === "'") { quote = ch; continue }
    if (ch === '{') depth += 1
    else if (ch === '}') {
      depth -= 1
      if (depth === 0) return i
    }
  }
  return -1
}
