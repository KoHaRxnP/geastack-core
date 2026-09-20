import type { HostShimDefinitions } from './types.js'

// CSSStyleDeclaration / Element.style native-handle mapping. The embedded
// backend already returns a concrete `gea::embedded::ui::Style` handle from
// `NodeHandle::style()` (see the `style` getter in host-shims.ts's
// `nativeMemberPropertyGetters`), but a CAPTURED `const style = el.style`
// local — exactly the shape gea's own `reactiveStyle`/`reactiveStyleProp`
// runtime helpers use (packages/gea/src/runtime/reactive-style.ts) — carries
// the DOM-declared `CSSStyleDeclaration` type for ITS OWN storage, and that
// type had no entry in `nativeTypes`. Without one, the captured variable
// resolved to a boxed `gea_cpp_value` record instead of the concrete `Style`
// handle the getter already hands back, so `style.setProperty(...)` /
// `style.removeProperty(...)` on the STORED variable (as opposed to a
// one-shot `el.style.setProperty(...)`) found no native call target and
// declined to a dynamic ABI bridge. See
// docs/campaign/findings/consolidated-census-and-tails-20260818.md, "(B)
// Missing DOM/host-shim native type — 2 of 60."
export const CSS_STYLE_DECLARATION_NATIVE_TYPES: NonNullable<HostShimDefinitions['nativeTypes']> = {
  CSSStyleDeclaration: 'gea::embedded::ui::Style',
}

// A `Style` receiver needs BOTH names covered: geatsc probes a member-call
// binding with either the resolved TS type display name or the C++ storage
// type (the same dual-listing pattern `NODE_HANDLE_RECEIVER_TYPES` documents
// in host-shims.ts for node receivers).
const STYLE_RECEIVER_TYPES = ['gea::embedded::ui::Style', 'CSSStyleDeclaration']

// `packages/engine/ui/style.h`'s `Style` class exposes fixed named setters
// (width/height/backgroundColor/…) for the compiler's own static style
// authoring, plus a generic string-keyed `setProperty`/`removeProperty` pair
// added alongside this shim (see style.h/style.cpp) that delegates to
// `StyleSheet::instance().applyProperty`/`removeProperty` — the same
// inline-style path the CSS parser itself already feeds. This is exactly
// the shape gea's `reactiveStyle`/`reactiveStyleProp` runtime helpers use: a
// kebab-cased CSS property name plus a stringified value.
//
// The emit templates below call the FLAT `gea_ir::domStyleSetProperty`/
// `domStyleRemoveProperty` free functions (`cpp-ir-dom-style.ts`), not
// `Style::setProperty`/`removeProperty` directly as a dot-call: this
// package's receiver-carrying host member CALL producer
// (`driver/host-member-calls.ts`'s `callSymbolFromReceiverTemplate`) only
// plans the flat shape `symbol({receiver}, {arg0}, ...)` and declines a
// dot-call template outright — see that function's own doc comment.
export const CSS_STYLE_DECLARATION_NATIVE_MEMBER_METHODS: NonNullable<HostShimDefinitions['nativeMemberMethods']> = {
  setProperty: [
    { receiverTypes: STYLE_RECEIVER_TYPES, emit: 'gea_ir::domStyleSetProperty({receiver}, {arg0}, {arg1})', returnType: 'void' },
  ],
  removeProperty: [
    { receiverTypes: STYLE_RECEIVER_TYPES, emit: 'gea_ir::domStyleRemoveProperty({receiver}, {arg0})', returnType: 'bool' },
  ],
}
