// Typed notification channel for regular Store-based AOT stores.
//
// Renderer bindings subscribe directly to a native SignalHub on the typed Store
// base. Store method replacement and normal typed field assignment notify that
// hub without routing through the old CompiledStore Proxy side tables.

import type { GeaIrBundleV1 } from './types.js'

// The typed Store base in hub mode gets the SignalHub plus a native `observe`
// method. The AOT keyed-list runtime calls `store.observe(prop, handler)` to
// re-reconcile a list field on change; in hub mode there is no dynamic observer
// channel, so route it through the hub. The handler is invoked with no payload
// (missing, missing) — the keyed-list handler re-resolves the current array from
// the typed field, so the value need not travel through the notification. Kept
// fully typed (no gea_cpp_value receiver) so the `store->observe(...)` call site
// binds to a real member instead of a boxed record dispatch.
// `observe` is a typed `std::function` member (NOT a method) so the keyed-list's
// `typeof store.observe === 'function'` probe — which reads `store->observe` as a
// value (a method can't be named as a value, a member can) — sees it and the
// subscription actually wires up at runtime. The stored type is a plain typed
// std::function (no gea_cpp_value receiver / no boxed storage); only the dynamic
// `typeof` query transiently views it as a callable value.
const HUB_SIGNAL =
  'public:\n' +
  '  mutable gea::embedded::ui::SignalHub __gea_hub;\n'

const HUB_OBSERVE_FIELD =
  '  std::function<std::function<void()>(std::string, std::function<void(gea_cpp_value, gea_cpp_value)>)> observe =\n' +
  '    [this](std::string __gea_prop, std::function<void(gea_cpp_value, gea_cpp_value)> __gea_handler) -> std::function<void()> {\n' +
  '      std::size_t __gea_sub = __gea_hub.subscribe(__gea_prop.c_str(), [__gea_handler]() mutable { __gea_handler(gea_cpp_value::missing(), gea_cpp_value::missing()); });\n' +
  '      return [this, __gea_sub]() { __gea_hub.unsubscribe(__gea_sub); };\n' +
  '    };\n'

const HUB_MEMBER = HUB_SIGNAL + HUB_OBSERVE_FIELD

let hubEmissionEnabled = false
let storeDynamicFallback = false

export function storeHubEmissionEnabled(): boolean {
  return hubEmissionEnabled
}

export function configureStoreDynamicFallback(enabled: boolean): void {
  void enabled
  storeDynamicFallback = false
}

export function storeDynamicFallbackEnabled(): boolean {
  return storeDynamicFallback
}

export function decideStoreHubEmission(source: string, ir: GeaIrBundleV1 | null): boolean {
  hubEmissionEnabled = false
  if (!ir) return false
  // Both compiled and lean IR stores are native typed store classes. Lean
  // changes the generated method surface, not the notification transport:
  // native field writes and mounted bindings still meet at the inherited hub.
  if (!ir.stores.some((store) => !store.selfStore)) return false
  const hasBase = ['Store', 'CompiledStore', 'CompiledLeanStore'].some((base) => findClassBodyOpen(source, base) >= 0)
  if (!hasBase) return false
  hubEmissionEnabled = true
  return true
}

export function injectStoreSignalHub(source: string, ir: GeaIrBundleV1): string {
  void ir
  if (!hubEmissionEnabled) return source
  let next = source
  for (const base of ['Store', 'CompiledStore', 'CompiledLeanStore']) {
    const open = findClassBodyOpen(next, base)
    if (open < 0) continue
    // The reactive runtime's CompiledLeanStore hand-writes its own `observe`
    // method (routing through leanObserve); injecting the hub's `observe` field
    // on top of it is a redeclaration. Emit only the SignalHub for such a class
    // — field writes and renderer binds still route through `__gea_hub`, and the
    // class's own `observe` is the observe API.
    const body = classBodySlice(next, open)
    const member = classBodyDeclaresObserveMember(body) ? HUB_SIGNAL : HUB_MEMBER
    next = `${next.slice(0, open + 1)}\n${member}${next.slice(open + 1)}`
  }
  return next
}

// A member declaration named `observe` (method `observe(` or field `observe =`),
// as opposed to a member-access call `x.observe(` / `store->observe(`.
function classBodyDeclaresObserveMember(classBody: string): boolean {
  return /(?<![\w.>:])observe\s*[(=]/.test(classBody)
}

// The `{ ... }` span of a class body starting at the opening brace, skipping
// string/char literals and comments so braces inside them do not unbalance the
// match. Used to scope the `observe` member scan to a single class.
function classBodySlice(source: string, openBraceIndex: number): string {
  let depth = 0
  for (let i = openBraceIndex; i < source.length; i++) {
    const ch = source[i]
    const nextCh = source[i + 1]
    if (ch === '/' && nextCh === '/') {
      const nl = source.indexOf('\n', i)
      if (nl < 0) return source.slice(openBraceIndex)
      i = nl
      continue
    }
    if (ch === '/' && nextCh === '*') {
      const end = source.indexOf('*/', i + 2)
      if (end < 0) return source.slice(openBraceIndex)
      i = end + 1
      continue
    }
    if (ch === '"' || ch === "'") {
      i++
      while (i < source.length) {
        if (source[i] === '\\') {
          i += 2
          continue
        }
        if (source[i] === ch) break
        i++
      }
      continue
    }
    if (ch === '{') depth++
    else if (ch === '}') {
      depth--
      if (depth === 0) return source.slice(openBraceIndex, i + 1)
    }
  }
  return source.slice(openBraceIndex)
}

// The typed bind helper, emitted once into the late gea_ir block. Run-once
// apply + microtask scheduling with per-binding dedup and disposer-registered
// teardown, registering through the typed hub (store->__gea_hub) rather than a
// dynamic observe channel.
export function storeHubBindHelperLines(ir: GeaIrBundleV1): string[] {
  if (!hubEmissionEnabled) return []
  if (!ir.stores.some((store) => !store.selfStore)) return []
  return [
    'template <typename Store>',
    'inline void gea_rc_bind_hub_apply(const std::shared_ptr<Store> &store, const std::shared_ptr<NativeDisposer> &disposer, std::initializer_list<const char *> deps, std::function<void()> apply) {',
    '  auto apply_fn = std::make_shared<std::function<void()>>(std::move(apply));',
    '  auto pending = std::make_shared<bool>(false);',
    '  gea_cpp_run_reactive_apply(apply_fn);',
    '  for (const char *dep : deps) {',
    '    auto __gea_sub_id = store->__gea_hub.subscribe(dep, [apply_fn, pending]() { gea_cpp_schedule_reactive_apply(apply_fn, pending); });',
    '    if (disposer) disposer->add([store, __gea_sub_id]() { store->__gea_hub.unsubscribe(__gea_sub_id); });',
    '  }',
    '  // Unsubscribing only stops FUTURE notifications — an apply already queued on',
    '  // the microtask queue still runs after disposal, against node ids the unmount',
    '  // freed (and the next mount may already have reused: stale keyed-list rebuilds',
    '  // then remove freshly-mounted nodes). Null the shared fn on dispose so the',
    '  // queued run is a no-op (gea_cpp_run_reactive_apply skips empty functions).',
    '  if (disposer) disposer->add([apply_fn]() { *apply_fn = nullptr; });',
    '}',
    '',
  ]
}

function findClassBodyOpen(source: string, className: string): number {
  const pattern = new RegExp(`\\bclass\\s+${className}\\b[^;{]*\\{`, 'g')
  const match = pattern.exec(source)
  if (!match) return -1
  return match.index + match[0].length - 1
}
