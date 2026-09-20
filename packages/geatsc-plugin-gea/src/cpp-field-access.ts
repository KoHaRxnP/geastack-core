// The seam that lets the ONE mounted renderer emit either boxed or fully-typed
// reads. The renderer's tree/keyed-list/conditional/child logic is type-agnostic;
// only the leaves differ — how a store field is read, how a reactive binding is
// installed, how a method is called, and the renderer's store/self parameter.
//
//   BoxedStore  — today's behavior, byte-for-byte: `read_X_field(store)`,
//                 `bindReactiveApply(store, …)`, `const gea_cpp_value &store`.
//   TypedSelf   — a self-store ReactiveComponent: `self->field.get()`, per-field
//                 `self->field.subscribe(apply)`, `const std::shared_ptr<App> &self`.
//                 No gea_cpp_value anywhere in the read/bind/call path.
//
// A field whose type isn't statically known still falls back to a boxed read even
// under TypedSelf (the renderer marks such reads), so "emit types as much as
// possible" degrades gracefully rather than failing.

import { cppString, sanitizeCppIdentifier } from './utils.js'
import { storeFieldLocalName } from './cpp-mounted-lowering.js'
import type { GeaIrComponent } from './types.js'

// A ReactiveComponent child mounts a fresh typed instance of its own class in
// EVERY parent mode — its renderer takes `shared_ptr<Child>`, never the
// parent's store.
export function reactiveChildInstanceExpression(child: GeaIrComponent): string {
  return `std::make_shared<${sanitizeCppIdentifier(child.exportName)}>()`
}

export interface ReactiveFieldRef {
  fieldName: string
  // Mirrors StoreFieldPlan.readerName. Non-null at every binding site (the
  // renderer bails earlier on an unresolved reader), but typed nullable to match.
  readerName: string | null
}

export interface BindReactiveRequest {
  // Distinct dependency field names this binding re-runs on.
  deps: string[]
  // The store fields whose current value the apply body reads (declared as
  // `const auto <local> = <read>;` at the top of the apply).
  fields: ReactiveFieldRef[]
  // Capture list members besides the store/self root (e.g. the target node var).
  captures: string[]
  // The apply body (DOM mutations) — emitted inside the binding, indented.
  body: string[]
  disposerVar?: string
}

export interface FieldAccess {
  // Parameter declaration for `mount_<Name>(<paramDecl>, const gea_cpp_value &disposer)`.
  readonly paramDecl: string
  // Expression naming the component's own store/instance: `store` or `self`.
  readonly rootExpr: string
  // True for the typed self-store path.
  readonly typed: boolean
  // Store argument for a child component's `mount_<Child>(…)` call, or null
  // when this parent mode cannot supply what the child's renderer reads. Boxed
  // forwards its own boxed `store` param (today's exact behavior). Typed has no
  // boxed mirror of the self-store to forward, so it resolves per child:
  // missing() for children that never read their store param, the child
  // subtree's single global store accessor when its renderer binds one, and a
  // fresh typed instance for ReactiveComponent children (both modes).
  childMountStoreArg(child: GeaIrComponent): string | null
  // Read one of the component's own store fields → a C++ expression.
  readOwnField(field: ReactiveFieldRef): string
  // The `const auto <local> = <read>;` line declaring a field's value as a local.
  readOwnFieldLocal(field: ReactiveFieldRef): string
  // Emit a reactive binding over the component's own fields. Boxed → one
  // `bindReactiveApply`; typed → run once + per-field `Signal::subscribe`.
  bindReactiveOwn(request: BindReactiveRequest): string[]
  // Call a zero-or-more-arg method on the component's own store → C++ statement.
  callOwnMethod(method: string, args: string[]): string
}

// ── BoxedStore: today's exact emit (keep byte-identical) ───────────────────────

export class BoxedStoreFieldAccess implements FieldAccess {
  readonly paramDecl: string
  readonly rootExpr = 'store'
  readonly typed = false
  // Optional child-aware resolver: a child whose mount takes the TYPED
  // shared_ptr store can't accept the parent's boxed `store` forward — the
  // resolver (injected by cpp-mounted, which has the component/store context)
  // passes the child's typed global accessor instead. Default keeps the
  // legacy forward.
  private readonly resolveChildStoreArg: (child: GeaIrComponent) => string | null
  // Set when the component's single subtree store is a typed-carrier global:
  // the store param becomes the `std::shared_ptr<Class>` itself. Field reads
  // keep the same `read_X(store)` shape (typed overloads resolve), binds go
  // through the typed hub, and a genuinely dynamic own-method call boxes via
  // gea_cpp_key at that boundary.
  private readonly typedParamClass: string | null

  constructor(childStoreArg?: (child: GeaIrComponent) => string | null, typedParamClass: string | null = null) {
    this.resolveChildStoreArg =
      childStoreArg ?? ((child) => (child.reactiveState ? reactiveChildInstanceExpression(child) : 'store'))
    this.typedParamClass = typedParamClass
    this.paramDecl = typedParamClass ? `const std::shared_ptr<${typedParamClass}> &store` : 'const gea_cpp_value &store'
  }

  childMountStoreArg(child: GeaIrComponent): string | null {
    return this.resolveChildStoreArg(child)
  }

  readOwnField(field: ReactiveFieldRef): string {
    return `${field.readerName}(store)`
  }

  readOwnFieldLocal(field: ReactiveFieldRef): string {
    return `const auto ${storeFieldLocalName(field.fieldName)} = ${field.readerName}(store);`
  }

  bindReactiveOwn(request: BindReactiveRequest): string[] {
    const deps = [...new Set(request.deps)].map((dep) => cppString(dep)).join(', ')
    const captures = [...request.captures, 'store'].join(', ')
    const disposerVar = request.disposerVar ?? 'disposer'
    if (this.typedParamClass) {
      const lines: string[] = [`  gea_rc_bind_hub_apply(store, ${disposerVar}, {${deps}}, std::function<void()>([${captures}]() mutable -> void {`]
      for (const field of request.fields) lines.push(`    ${this.readOwnFieldLocal(field)}`)
      for (const line of request.body) lines.push(`    ${line}`)
      lines.push('  }));')
      return lines
    }
    const lines: string[] = [`  bindReactiveApply(store, ${disposerVar}, {${deps}}, [${captures}]() mutable -> void {`]
    for (const field of request.fields) lines.push(`    ${this.readOwnFieldLocal(field)}`)
    for (const line of request.body) lines.push(`    ${line}`)
    lines.push('  });')
    return lines
  }

  callOwnMethod(method: string, args: string[]): string {
    // Existing event lowering resolves `this.method()` against `store` via the
    // shared_ptr facade; preserved verbatim by the caller in the boxed path.
    // A typed param boxes through gea_cpp_key (the retained tracked proxy)
    // for this genuinely dynamic lookup.
    const receiver = this.typedParamClass ? `gea_cpp_key(${this.rootExpr})` : this.rootExpr
    return `${receiver}.record_get_literal(${cppString(method)})(${args.join(', ')})`
  }
}

// ── TypedSelf: fully-typed self-store ReactiveComponent ────────────────────────

export class TypedSelfFieldAccess implements FieldAccess {
  readonly typed = true
  // Crucially, the renderer's store parameter keeps the name `store` in BOTH
  // modes — only its TYPE differs (gea_cpp_value vs shared_ptr<Class>). That way
  // the ~100 `store` references and every `read_X(store)` call in the shared
  // renderer stay byte-identical; the difference lives entirely in (a) the
  // generated read-helper BODIES (typed: `store->field.get()`), (b) this param
  // type, (c) the reactive binding, and (d) method calls.
  readonly rootExpr = 'store'
  readonly paramDecl: string
  private readonly className: string
  // Resolves the store argument for a non-reactive child's mount call (see the
  // interface comment). Injected by selfStoreMountedRenderer, which has the
  // full component/store context this class deliberately doesn't depend on.
  private readonly resolveChildStoreArg: (child: GeaIrComponent) => string | null

  constructor(className: string, childStoreArg: (child: GeaIrComponent) => string | null = () => 'gea_cpp_value::missing()') {
    this.className = sanitizeCppIdentifier(className)
    this.paramDecl = `const std::shared_ptr<${this.className}> &store`
    this.resolveChildStoreArg = childStoreArg
  }

  childMountStoreArg(child: GeaIrComponent): string | null {
    if (child.reactiveState) return reactiveChildInstanceExpression(child)
    return this.resolveChildStoreArg(child)
  }

  // Same call shape as boxed — `read_X(store)` — but the emitted helper for a
  // self-store is generated typed (returns `store->field.get()`).
  readOwnField(field: ReactiveFieldRef): string {
    return `${field.readerName}(store)`
  }

  readOwnFieldLocal(field: ReactiveFieldRef): string {
    return `const auto ${storeFieldLocalName(field.fieldName)} = ${this.readOwnField(field)};`
  }

  // Run the apply once for the initial render, then subscribe it to each
  // dependency's typed Signal. No gea_cpp_value, no string-keyed observe.
  bindReactiveOwn(request: BindReactiveRequest): string[] {
    const captures = [...request.captures, 'store'].join(', ')
    const applyBody: string[] = []
    for (const field of request.fields) applyBody.push(`    ${this.readOwnFieldLocal(field)}`)
    for (const line of request.body) applyBody.push(`    ${line}`)
    const lines: string[] = [
      `  {`,
      `    auto __gea_apply = [${captures}]() mutable -> void {`,
      ...applyBody.map((line) => `  ${line}`),
      `    };`,
      `    __gea_apply();`,
    ]
    for (const dep of [...new Set(request.deps)]) {
      lines.push(`    store->${sanitizeCppIdentifier(dep)}.subscribe(__gea_apply);`)
    }
    lines.push('  }')
    return lines
  }

  callOwnMethod(method: string, args: string[]): string {
    return `store->${sanitizeCppIdentifier(method)}(${args.join(', ')})`
  }
}
