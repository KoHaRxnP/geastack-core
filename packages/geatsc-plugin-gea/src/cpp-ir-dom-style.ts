// `node.style` — the flat-call `gea_ir::domStyle(node)` accessor the
// `style` `nativeMemberPropertyGetters` binding in host-shims.ts calls
// (`property-access-plan-solver.ts`'s `solveHostNativeMemberPropertyRead` only
// plans a property read whose emit template is exactly `symbol({receiver})` —
// a dot-call like `({receiver}).style()`, the form this binding used before,
// has no template slot at all and stays permanently unplanned, exactly like
// `.content` still does; see that function's own doc comment). Mirrors
// `domNextSibling` in `cpp-ir.ts` right next to it: a typed, non-boxed
// result (`gea::embedded::ui::Style` is itself just a thin `{nodeId}` view,
// same shape as `NodeHandle`), plus a `gea_cpp_value` overload so a
// value-bridged node (e.g. `event.target.style`) resolves identically to a
// typed `NodeHandle` receiver.
//
// Split into its own module rather than growing `cpp-ir.ts` (already well
// past the file-size ratchet) — spliced into `canvasInteropSource`'s output
// alongside the existing `domStyleInteropSource()` call.
export function domStyleAccessorInteropSource(): string[] {
  return [
    'inline gea::embedded::ui::Style domStyle(gea::embedded::ui::NodeHandle node) {',
    '  return node.style();',
    '}',
    '',
    'inline gea::embedded::ui::Style domStyle(const gea_cpp_value &node) {',
    '  return domStyle(gea::embedded::ui::NodeHandle(nodeIdFromValue(node)));',
    '}',
    '',
    // Flat, receiver-carrying free functions for `style.setProperty(...)` /
    // `style.removeProperty(...)`. These exist ONLY because
    // `driver/host-member-calls.ts`'s `callSymbolFromReceiverTemplate` (the
    // sole live producer for a receiver-carrying host member CALL in the
    // checker-derived pipeline) plans exclusively the flat shape
    // `symbol({receiver}, {arg0}, ...)` and declines any dot-call template
    // (`({receiver}).setProperty({args})` — the shape every OTHER
    // `nativeMemberMethods` dot-call entry in host-shims.ts also uses, e.g.
    // `scrollIntoView`/`connect`/`play` — which that same planner declines
    // identically; those routed through a now-defunct text-emitter consumer
    // this driver never replaced, per that file's own doc comment). Wrapping
    // `Style::setProperty`/`removeProperty` in flat free functions is the
    // only shape this producer can actually plan today.
    'inline void domStyleSetProperty(gea::embedded::ui::Style style, const std::string &property, const std::string &value) {',
    '  style.setProperty(property, value);',
    '}',
    '',
    'inline bool domStyleRemoveProperty(gea::embedded::ui::Style style, const std::string &property) {',
    '  return style.removeProperty(property);',
    '}',
    ''
  ]
}
