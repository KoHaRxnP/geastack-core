import type { StoreArrayFieldPlan, StoreFieldPlan, StoreMethodPlan } from './cpp-stores.js'
import { existsSync, readFileSync } from 'node:fs'
import { canTemplateMountComponent, parseTemplateRoot, templateMountedRenderer, type TemplateChild, type TemplateElement } from './cpp-template-renderer.js'
import {
  attributeLines,
  type ConstantMap,
  fieldNameForSlot,
  isRootSetupSlot,
  keyedListKeyExpression,
  keyedListPayload,
  lowerRowSlots,
  lowerSlots,
  mountSlotAttrs,
  mountSlotImageSourceLines,
  parseMountAttr,
  mountSlotStyleSlot,
  mountSlotTag,
  parseHtmlRoot,
  rootSetupLines,
  rootSetupSlotsSupported,
  slotAttrName,
  storeFieldLocalName,
  storeGlobalIsTyped,
  stringLiteralValue,
  unique,
  type HtmlRoot,
  type LoweredRowSlots,
  type LoweredSlots,
} from './cpp-mounted-lowering.js'
import type { GeaIrComponent, GeaIrSlot, GeaIrStoreField, GeaIrStoreValueShape, GeaIrTemplate } from './types.js'
import { cppString, sanitizeCppIdentifier } from './utils.js'
import { BoxedStoreFieldAccess, reactiveChildInstanceExpression, TypedSelfFieldAccess } from './cpp-field-access.js'
import { storeHubEmissionEnabled } from './cpp-store-hub.js'
import ts from 'typescript'
// Circular at module level (cpp-replacements imports this file back), but both
// sides only call each other inside function bodies, never during evaluation —
// the same reason the canMount callbacks below are injected instead of imported.
import { componentStoreReferences } from './cpp-replacements.js'

// EXPERIMENTAL (ReactiveComponent): a self-store component renders through the
// SAME template renderer as every other component, but with typed field access —
// `read_App_field_x(store)` resolves to the `shared_ptr<App>` overload reading
// the Signal field, bindings are per-field `store->dep.subscribe(...)`, and
// events call `store->method(...)` directly. Child-component mounts compose
// with the regular BOXED renderer, with the store argument resolved per child
// (see typedChildMountStoreArg). Templates that still can't mount a child
// surface the gea-unmountable-reactive-component diagnostic instead of a
// silent black screen.

// Re-entry guard for nested reactive mounts: generating A's renderer gates
// child <B/> by generating B's renderer, which may gate <A/> right back. A
// genuine A→B→A mount cycle must fail (null → diagnostic), not recurse forever.
const selfStoreRenderersInProgress = new Set<string>()

export function selfStoreMountedRenderer(
  component: GeaIrComponent,
  components: GeaIrComponent[],
  storeFields: StoreFieldPlan[],
  storeArrayFields: StoreArrayFieldPlan[],
  constants: ConstantMap = new Map(),
  storeMethods: StoreMethodPlan[] = [],
): string[] | null {
  if (!component.reactiveState) return null
  if (selfStoreRenderersInProgress.has(component.id)) return null
  selfStoreRenderersInProgress.add(component.id)
  try {
    const childStoreArgs = new Map<string, string | null>()
    const childStoreArg = (child: GeaIrComponent): string | null => {
      if (child === component) return null
      const cached = childStoreArgs.get(child.id)
      if (cached !== undefined) return cached
      childStoreArgs.set(child.id, null) // cycle guard while the child renders
      const resolved = typedChildMountStoreArg(child, components, storeFields, storeArrayFields, constants, storeMethods)
      childStoreArgs.set(child.id, resolved)
      return resolved
    }
    const canMountTypedChild = (child: GeaIrComponent): boolean => childStoreArg(child) !== null
    return templateMountedRenderer(
      component,
      components,
      storeFields,
      constants,
      canMountTypedChild,
      storeMethods,
      new TypedSelfFieldAccess(component.exportName, childStoreArg),
      storeArrayFields,
    )
  } finally {
    selfStoreRenderersInProgress.delete(component.id)
  }
}

// What a typed self-store parent passes as the `store` argument when mounting
// `child`, or null when it can't supply one:
// - a ReactiveComponent child mounts a fresh typed instance of its own class
//   (iff the child's own typed renderer generates — cycles land on the
//   in-progress guard above and resolve to null);
// - a child whose generated boxed renderer never reads its `store` param gets
//   `missing()` — all its reads resolve through `__gea_global_*()` accessors;
// - a child that DOES read the param (keyed-list renderers bind through it)
//   gets the single global store its template subtree references — the same
//   value the boxed entry threads down — when that store is unambiguous.
export function typedChildMountStoreArg(
  child: GeaIrComponent,
  components: GeaIrComponent[],
  storeFields: StoreFieldPlan[],
  storeArrayFields: StoreArrayFieldPlan[],
  constants: ConstantMap,
  storeMethods: StoreMethodPlan[],
): string | null {
  if (child.reactiveState) {
    return selfStoreMountedRenderer(child, components, storeFields, storeArrayFields, constants, storeMethods)
      ? reactiveChildInstanceExpression(child)
      : null
  }
  const renderer = mountedRendererForComponent(child, components, storeFields, storeArrayFields, constants, storeMethods)
  if (!renderer) return null
  // Match the child's actual param flavor. A typed-carrier child ALWAYS
  // takes its subtree's shared_ptr global (even when its body never reads
  // the param — missing() can't bind a shared_ptr). A boxed child that
  // never reads its param gets missing(); one fed a TYPED global
  // (recognized store but hub off) crosses through gea_cpp_key — the
  // retained tracked proxy, not a snapshot.
  const childSignatureTyped = renderer.some((line) =>
    line.startsWith(`inline gea::embedded::ui::NodeHandle ${mountedRendererName(child)}(const std::shared_ptr<`),
  )
  if (!childSignatureTyped && !rendererReadsStoreParam(renderer)) return 'gea_cpp_value::missing()'
  const globalName = childSubtreeGlobalStoreName(child, components, storeFields, storeArrayFields, constants, storeMethods)
  if (!globalName) return null
  const expr = `__gea_global_${sanitizeCppIdentifier(globalName)}()`
  if (childSignatureTyped) return expr
  return storeGlobalIsTyped(globalName, storeFields) ? `gea_cpp_key(${expr})` : expr
}

// What a template parent passes as a child's mount store argument. A child
// whose mount takes the typed shared_ptr gets its subtree's typed global
// accessor directly. A boxed child under a TYPED-param parent crosses through
// gea_cpp_key (retained tracked proxy); under a boxed parent it keeps the
// legacy `store` forward. Reactive children mount a fresh typed instance.
function boxedChildMountStoreArg(
  child: GeaIrComponent,
  components: GeaIrComponent[],
  storeFields: StoreFieldPlan[],
  storeArrayFields: StoreArrayFieldPlan[],
  constants: ConstantMap,
  storeMethods: StoreMethodPlan[],
  parentParamTyped: boolean,
): string | null {
  if (child.reactiveState) return reactiveChildInstanceExpression(child)
  const renderer = mountedRendererForComponent(child, components, storeFields, storeArrayFields, constants, storeMethods)
  if (!renderer) return null
  const signatureTyped = renderer.some((line) =>
    line.startsWith(`inline gea::embedded::ui::NodeHandle ${mountedRendererName(child)}(const std::shared_ptr<`),
  )
  if (!signatureTyped) return parentParamTyped ? 'gea_cpp_key(store)' : 'store'
  const globalName = childSubtreeGlobalStoreName(child, components, storeFields, storeArrayFields, constants, storeMethods)
  if (globalName) return `__gea_global_${sanitizeCppIdentifier(globalName)}()`
  // The child takes a typed store but its subtree global is unresolvable from
  // here — when the parent's own param is the same typed carrier, forward it.
  return parentParamTyped ? 'store' : null
}

// The one bundled global store instance `child`'s template (and its mounted
// subtree) references, or null when there is none, more than one, or an
// outer-`this` receiver — the cases a typed parent cannot thread a store for.
export function childSubtreeGlobalStoreName(
  child: GeaIrComponent,
  components: GeaIrComponent[],
  storeFields: StoreFieldPlan[],
  storeArrayFields: StoreArrayFieldPlan[],
  constants: ConstantMap,
  storeMethods: StoreMethodPlan[] = [],
): string | null {
  const refs = componentStoreReferences(child, components, storeFields, storeArrayFields, constants, storeMethods)
  if (!refs) return null
  const names = new Set(refs.map((ref) => sanitizeCppIdentifier(ref.storeExpression)))
  if (names.size !== 1) return null
  const name = [...names][0]
  if (name === 'this') return null
  // Must be a real bundled store instance — receivers the reference analysis
  // can't tie to a store global (locals, unresolved identifiers) don't qualify.
  return storeFields.some((field) => field.storeGlobalName === name && !field.storeIsSelfStore) ? name : null
}

// Whether a generated boxed renderer reads its `store` parameter anywhere in
// its body (field reads, bindReactiveApply roots, lambda captures, forwarding
// to grandchild mounts). Strip the parameter declarations (`&store`) first so
// only actual uses remain.
export function rendererReadsStoreParam(lines: string[]): boolean {
  return /\bstore\b/.test(lines.join('\n').replaceAll('&store', ''))
}

// The class name when `plan`'s store travels as the TYPED carrier
// (`std::shared_ptr<Class>` end to end — typed global cell, typed mount
// param, typed Context): compiled-base, non-self stores with the typed
// SignalHub active. Everything else stays on the boxed gea_cpp_value channel.
export function typedStoreCarrier(plan: {
  storeClass: string
  storeRuntimeBase?: 'compiled' | 'lean'
  storeIsSelfStore?: boolean
}): string | null {
  return storeHubEmissionEnabled() && plan.storeRuntimeBase === 'compiled' && !plan.storeIsSelfStore ? plan.storeClass : null
}

// Derive the forward declaration from the generated definition's signature
// line so the param flavor (typed shared_ptr store vs boxed gea_cpp_value)
// can never skew between declaration and definition.
export function mountedRendererDeclarationFromLines(renderer: string[]): string | null {
  const signature = renderer.find((line) => line.startsWith('inline gea::embedded::ui::NodeHandle mount_'))
  if (!signature) return null
  return signature.replace(/^inline\s+/, '').replace(/\s*\{\s*$/, ';')
}

export function generateMountedRendererDeclarations(
  components: GeaIrComponent[],
  storeFields: StoreFieldPlan[],
  storeArrayFields: StoreArrayFieldPlan[],
  constants: ConstantMap = new Map(),
  storeMethods: StoreMethodPlan[] = [],
): string[] {
  return components
    .map((component) => mountedRendererForComponent(component, components, storeFields, storeArrayFields, constants, storeMethods))
    .filter((renderer): renderer is string[] => renderer !== null)
    .map((renderer) => mountedRendererDeclarationFromLines(renderer))
    .filter((declaration): declaration is string => declaration !== null)
}

export function generateMountedRenderers(
  components: GeaIrComponent[],
  storeFields: StoreFieldPlan[],
  storeArrayFields: StoreArrayFieldPlan[],
  constants: ConstantMap = new Map(),
  storeMethods: StoreMethodPlan[] = [],
): string[] {
  const renderers = generateMountedRendererSections(components, storeFields, storeArrayFields, constants, storeMethods).map((section) => section.lines)

  return renderers.flat()
}

export interface MountedRendererSection {
  componentId: string
  exportName: string
  functionName: string
  lines: string[]
}

export function generateMountedRendererSections(
  components: GeaIrComponent[],
  storeFields: StoreFieldPlan[],
  storeArrayFields: StoreArrayFieldPlan[],
  constants: ConstantMap = new Map(),
  storeMethods: StoreMethodPlan[] = [],
): MountedRendererSection[] {
  return components
    .map((component) => {
      const renderer = mountedRendererForComponent(component, components, storeFields, storeArrayFields, constants, storeMethods)
      if (!renderer) return null
      return {
        componentId: component.id,
        exportName: component.exportName,
        functionName: mountedRendererName(component),
        lines: withComponentLocalConstAccessors(component, renderer)
      }
    })
    .filter((section): section is MountedRendererSection => section !== null)
}

interface ComponentLocalConst {
  name: string
  initializer: ts.Expression
}

function withComponentLocalConstAccessors(component: GeaIrComponent, renderer: string[]): string[] {
  const locals = componentLocalConsts(component)
  if (locals.length === 0) return renderer
  const localByName = new Map(locals.map((local) => [local.name, local]))
  const rendererText = renderer.join('\n')
  const needed = componentLocalConstClosure(
    [...identifiersInText(rendererText)].filter((name) => localByName.has(name)),
    localByName,
  )
  if (needed.size === 0) return renderer

  const accessorByName = new Map<string, string>()
  for (const name of needed) {
    accessorByName.set(name, `__gea_local_${sanitizeCppIdentifier(component.exportName)}_${sanitizeCppIdentifier(name)}`)
  }

  const accessors: string[] = []
  for (const local of locals) {
    if (!needed.has(local.name)) continue
    const expr = lowerComponentLocalConstExpression(local.initializer, accessorByName)
    if (!expr) continue
    accessors.push(`inline auto ${accessorByName.get(local.name)}() { return ${expr}; }`)
  }
  if (accessors.length === 0) return renderer

  const emitted = new Set([...accessorByName.entries()].filter(([name]) => needed.has(name)).map(([, accessor]) => accessor))
  const rewritten = renderer.map((line) => {
    let next = line
    for (const [name, accessor] of accessorByName) {
      if (!emitted.has(accessor)) continue
      next = replaceIdentifierToken(next, name, `${accessor}()`)
    }
    return next
  })
  return [...accessors, '', ...rewritten]
}

function componentLocalConsts(component: GeaIrComponent): ComponentLocalConst[] {
  const fileName = component.id.split('#')[0]
  if (!fileName || !existsSync(fileName)) return []
  const text = readFileSync(fileName, 'utf8')
  const sourceFile = ts.createSourceFile(fileName, text, ts.ScriptTarget.Latest, true, ts.ScriptKind.TSX)
  const locals: ComponentLocalConst[] = []
  for (const statement of sourceFile.statements) {
    if (!ts.isVariableStatement(statement)) continue
    if (!(statement.declarationList.flags & ts.NodeFlags.Const)) continue
    for (const declaration of statement.declarationList.declarations) {
      if (!ts.isIdentifier(declaration.name) || !declaration.initializer) continue
      locals.push({ name: declaration.name.text, initializer: declaration.initializer })
    }
  }
  return locals
}

function componentLocalConstClosure(seed: string[], localByName: ReadonlyMap<string, ComponentLocalConst>): Set<string> {
  const needed = new Set<string>()
  const visit = (name: string) => {
    if (needed.has(name)) return
    const local = localByName.get(name)
    if (!local) return
    needed.add(name)
    for (const dependency of identifiersInExpression(local.initializer)) {
      if (localByName.has(dependency)) visit(dependency)
    }
  }
  for (const name of seed) visit(name)
  return needed
}

function lowerComponentLocalConstExpression(node: ts.Expression, accessorByName: ReadonlyMap<string, string>): string | null {
  if (ts.isParenthesizedExpression(node) || ts.isAsExpression(node) || ts.isNonNullExpression(node)) {
    return lowerComponentLocalConstExpression(node.expression, accessorByName)
  }
  if (ts.isNumericLiteral(node)) return node.getText()
  if (ts.isStringLiteralLike(node)) return `std::string(${cppString(node.text)})`
  if (node.kind === ts.SyntaxKind.TrueKeyword) return 'true'
  if (node.kind === ts.SyntaxKind.FalseKeyword) return 'false'
  if (ts.isIdentifier(node)) return accessorByName.has(node.text) ? `${accessorByName.get(node.text)}()` : sanitizeCppIdentifier(node.text)
  if (ts.isPrefixUnaryExpression(node)) {
    const operand = lowerComponentLocalConstExpression(node.operand, accessorByName)
    if (!operand) return null
    if (node.operator === ts.SyntaxKind.MinusToken) return `(-${operand})`
    if (node.operator === ts.SyntaxKind.PlusToken) return `(+${operand})`
    if (node.operator === ts.SyntaxKind.ExclamationToken) return `(!${operand})`
    return null
  }
  if (ts.isConditionalExpression(node)) {
    const condition = lowerComponentLocalConstExpression(node.condition, accessorByName)
    const whenTrue = lowerComponentLocalConstExpression(node.whenTrue, accessorByName)
    const whenFalse = lowerComponentLocalConstExpression(node.whenFalse, accessorByName)
    if (!condition || !whenTrue || !whenFalse) return null
    return `(gea::runtime::coerce::to_boolean(${condition}) ? ${whenTrue} : ${whenFalse})`
  }
  if (ts.isBinaryExpression(node)) {
    const left = lowerComponentLocalConstExpression(node.left, accessorByName)
    const right = lowerComponentLocalConstExpression(node.right, accessorByName)
    const op = cppLocalConstBinaryOperator(node.operatorToken.kind)
    if (!left || !right || !op) return null
    return `(${left} ${op} ${right})`
  }
  if (ts.isCallExpression(node) && ts.isPropertyAccessExpression(node.expression) && ts.isIdentifier(node.expression.expression)) {
    const receiver = node.expression.expression.text
    const method = node.expression.name.text
    const args = node.arguments.map((arg) => lowerComponentLocalConstExpression(arg, accessorByName))
    if (args.some((arg) => !arg)) return null
    if (receiver === 'Math' && method === 'floor' && args.length === 1) return `std::floor(${args[0]})`
    if (receiver === 'Math' && method === 'ceil' && args.length === 1) return `std::ceil(${args[0]})`
    if (receiver === 'Math' && method === 'round' && args.length === 1) return `std::round(${args[0]})`
    if (receiver === 'Math' && method === 'abs' && args.length === 1) return `std::abs(${args[0]})`
    if (receiver === 'Math' && method === 'max' && args.length > 0) return `gea::runtime::math::max({${args.join(', ')}})`
    if (receiver === 'Math' && method === 'min' && args.length > 0) return `gea::runtime::math::min({${args.join(', ')}})`
  }
  return null
}

function cppLocalConstBinaryOperator(kind: ts.SyntaxKind): string | null {
  if (kind === ts.SyntaxKind.PlusToken) return '+'
  if (kind === ts.SyntaxKind.MinusToken) return '-'
  if (kind === ts.SyntaxKind.AsteriskToken) return '*'
  if (kind === ts.SyntaxKind.SlashToken) return '/'
  if (kind === ts.SyntaxKind.PercentToken) return '%'
  if (kind === ts.SyntaxKind.LessThanToken) return '<'
  if (kind === ts.SyntaxKind.LessThanEqualsToken) return '<='
  if (kind === ts.SyntaxKind.GreaterThanToken) return '>'
  if (kind === ts.SyntaxKind.GreaterThanEqualsToken) return '>='
  if (kind === ts.SyntaxKind.EqualsEqualsToken || kind === ts.SyntaxKind.EqualsEqualsEqualsToken) return '=='
  if (kind === ts.SyntaxKind.ExclamationEqualsToken || kind === ts.SyntaxKind.ExclamationEqualsEqualsToken) return '!='
  if (kind === ts.SyntaxKind.AmpersandAmpersandToken) return '&&'
  if (kind === ts.SyntaxKind.BarBarToken) return '||'
  return null
}

function identifiersInExpression(node: ts.Node): Set<string> {
  const identifiers = new Set<string>()
  const visit = (next: ts.Node) => {
    if (ts.isIdentifier(next)) identifiers.add(next.text)
    ts.forEachChild(next, visit)
  }
  visit(node)
  return identifiers
}

function identifiersInText(text: string): Set<string> {
  const identifiers = new Set<string>()
  const pattern = /\b[A-Za-z_$][A-Za-z0-9_$]*\b/g
  let match: RegExpExecArray | null
  while ((match = pattern.exec(text))) identifiers.add(match[0])
  return identifiers
}

function replaceIdentifierToken(text: string, name: string, replacement: string): string {
  return text.replace(new RegExp(`\\b${escapeRegExp(name)}\\b`, 'g'), replacement)
}

export function mountedRendererName(component: GeaIrComponent): string {
  return `mount_${sanitizeCppIdentifier(component.exportName)}`
}

function canUseDedicatedKeyedListRenderer(component: GeaIrComponent, constants: ConstantMap): boolean {
  const keyed = component.template.slots.filter((slot) => slot.kind === 'keyed-list')
  if (keyed.length === 0) return false
  return component.template.slots.every((slot) => slot.kind === 'keyed-list' || isRootSetupSlot(slot, constants))
}

export function mountedRendererForComponent(
  component: GeaIrComponent,
  components: GeaIrComponent[],
  storeFields: StoreFieldPlan[],
  storeArrayFields: StoreArrayFieldPlan[],
  constants: ConstantMap,
  storeMethods: StoreMethodPlan[],
): string[] | null {
  // Self-store ReactiveComponent: typed template renderer only (no boxed paths).
  if (component.reactiveState) {
    return selfStoreMountedRenderer(component, components, storeFields, storeArrayFields, constants, storeMethods)
  }

  const dedicatedKeyed = canUseDedicatedKeyedListRenderer(component, constants)
  const multiKeyed = dedicatedKeyed ? multiKeyedListMountedRenderer(component, storeArrayFields, constants) : null
  if (multiKeyed) return multiKeyed

  const keyed = dedicatedKeyed ? keyedListMountedRenderer(component, components, storeFields, storeArrayFields, constants, storeMethods) : null
  if (keyed) return keyed

  // A template component whose whole subtree reads ONE typed-carrier global
  // store takes the typed shared_ptr param itself — the boxed gea_cpp_value
  // channel disappears from the entire mount chain. Multi-store or
  // non-typed-store components keep the boxed param.
  const ownGlobal = childSubtreeGlobalStoreName(component, components, storeFields, storeArrayFields, constants, storeMethods)
  const hasKeyedListSlot = component.template.slots.some((slot) => slot.kind === 'keyed-list')
  const ownTypedClass =
    (ownGlobal &&
      !hasKeyedListSlot &&
      storeFields.find(
        (field) => field.storeGlobalName === ownGlobal && field.storeRuntimeBase === 'compiled' && !field.storeIsSelfStore,
      )?.storeClass) ||
    null
  const canNativeChildMount = (child: GeaIrComponent): boolean =>
    child !== component && mountedRendererForComponent(child, components, storeFields, storeArrayFields, constants, storeMethods) !== null
  const template = templateMountedRenderer(
    component,
    components,
    storeFields,
    constants,
    canNativeChildMount,
    storeMethods,
    new BoxedStoreFieldAccess(
      (child) => boxedChildMountStoreArg(child, components, storeFields, storeArrayFields, constants, storeMethods, !!ownTypedClass),
      storeHubEmissionEnabled() ? ownTypedClass : null,
    ),
    storeArrayFields,
  )
  if (template) return template

  const root = parseHtmlRoot(component.template.html)
  if (!root) return null

  const lowered = lowerSlots(component.template.slots, storeFields, constants)
  const setup = rootSetupLines('root', component.template.slots, storeFields, storeMethods, undefined, root.tag, constants, 'disposer')
  if (!setup) return null
  if (!lowered) {
    if (component.template.slots.some((slot) => !isRootSetupSlot(slot, constants))) return null
    return staticMountedRenderer(component, root, setup)
  }
  if (!canUseStyleTextMountedSlots(component.template.slots)) return null
  if (!canUseMountedFieldReaders(lowered)) return null
  return styleTextMountedRenderer(component, root, lowered, setup)
}

function staticMountedRenderer(component: GeaIrComponent, root: HtmlRoot, setup: string[] = []): string[] | null {
  const createRoot = uiCreateNodeExpression(root)
  if (!createRoot) return null
  return [
    `inline gea::embedded::ui::NodeHandle ${mountedRendererName(component)}(const gea_cpp_value &store, const std::shared_ptr<NativeDisposer> &disposer) {`,
    `  auto root = ${createRoot};`,
    ...attributeLines('root', root, component.template.slots),
    ...setup,
    '  return root;',
    '}',
    '',
  ]
}

function styleTextMountedRenderer(component: GeaIrComponent, root: HtmlRoot, lowered: LoweredSlots, setup: string[]): string[] | null {
  const createRoot = uiCreateNodeExpression(root)
  if (!createRoot) return null
  const deps = [...new Set(lowered.deps)]
  const depsList = deps.map((dep) => cppString(dep)).join(', ')
  const applyLambda = [
    `[root, store]() mutable -> void {`,
    ...lowered.fields.map((field) => `    const auto ${storeFieldLocalName(field.fieldName)} = ${mountedFieldReaderName(field)}(store);`),
    ...lowered.lines.map((line) => `    ${line}`),
    '  }',
  ]
  // Compiled-base single-store deps take the FULLY TYPED carrier: the store
  // param is the `std::shared_ptr<Class>` itself (matching the typed global
  // cell callers pass) and the bind registers straight into the SignalHub —
  // no downcast, no boxed fallback. Anything else keeps the boxed param and
  // dynamic registration.
  const hubClasses = new Set(lowered.fields.map((field) => field.storeClass))
  const typedCarrier =
    storeHubEmissionEnabled() &&
    hubClasses.size === 1 &&
    lowered.fields.every((field) => field.storeRuntimeBase === 'compiled' && !field.storeIsSelfStore)
      ? [...hubClasses][0]
      : null
  const storeParam = typedCarrier ? `const std::shared_ptr<${typedCarrier}> &store` : 'const gea_cpp_value &store'
  const bindLines = typedCarrier
    ? [
        `  gea_rc_bind_hub_apply(store, disposer, {${depsList}}, std::function<void()>(${applyLambda[0]}`,
        ...applyLambda.slice(1, -1),
        `  ${applyLambda[applyLambda.length - 1]}));`,
      ]
    : [`  bindReactiveApply(store, disposer, {${depsList}}, ${applyLambda[0]}`, ...applyLambda.slice(1, -1), `  ${applyLambda[applyLambda.length - 1]});`]
  return [
    `inline gea::embedded::ui::NodeHandle ${mountedRendererName(component)}(${storeParam}, const std::shared_ptr<NativeDisposer> &disposer) {`,
    `  auto root = ${createRoot};`,
    ...attributeLines('root', root, component.template.slots),
    ...setup,
    ...lowered.staticLines.map((line) => `  ${line}`),
    ...bindLines,
    '  return root;',
    '}',
    '',
  ]
}

function multiKeyedListMountedRenderer(component: GeaIrComponent, storeArrayFields: StoreArrayFieldPlan[], constants: ConstantMap): string[] | null {
  const slots = component.template.slots.filter((slot) => slot.kind === 'keyed-list')
  if (slots.length <= 1) return null

  const root = parseHtmlRoot(component.template.html)
  const setup = root ? rootSetupLines('root', component.template.slots, [], [], undefined, root.tag, constants, 'disposer') : null
  const createRoot = root ? uiCreateNodeExpression(root) : null
  if (!root || !setup || !createRoot) return null

  const plans = slots.map((slot, slotIndex) => {
    const fieldName = fieldNameForSlot(slot)
    const field = fieldName ? unique(storeArrayFields.filter((candidate) => candidate.fieldName === fieldName)) : null
    const payload = keyedListPayload(slot)
    const rowSlot = payload?.rowTemplate ? intrinsicImageRowSlot(payload.rowTemplate.html, payload.rowTemplate.slots) : null
    const source = rowSlot ? mountSlotImageSourceLines('row', rowSlot) : null
    const styleSlot = rowSlot ? mountSlotStyleSlot(rowSlot) : null
    const itemName = payload?.itemParam ? sanitizeCppIdentifier(payload.itemParam) : 'item'
    const rowSlots = field && styleSlot ? lowerRowSlots([styleSlot], itemName, field, constants) : null
    return field && payload?.rowTemplate && rowSlot && source && rowSlots && rowSlots.lines.length > 0
      ? { slotIndex, field, itemName, source, rowSlots }
      : null
  })
  if (plans.some((plan) => plan === null)) return null
  const concretePlans = plans as Array<NonNullable<(typeof plans)[number]>>
  const storeClass = uniqueEqual(concretePlans.map((plan) => plan.field.storeClass))
  if (!storeClass) return null

  const typedCarrier = typedStoreCarrier(concretePlans[0].field)
  const helperNs = `gea_mkl_${sanitizeCppIdentifier(component.exportName)}`
  const helperLines: string[] = [
    `namespace ${helperNs} {`,
    `struct Context {`,
    `  gea::embedded::ui::NodeHandle root;`,
    `  std::shared_ptr<${storeClass}> typed_store;`,
    ...(typedCarrier ? [] : [`  gea_cpp_value store;`]),
    `  std::vector<gea::embedded::ui::NodeHandle> rows;`,
    `};`,
  ]
  for (const plan of concretePlans) {
    const fn = `make_row_${plan.slotIndex}`
    helperLines.push(
      `inline gea::embedded::ui::NodeHandle ${fn}(const ${plan.field.itemTypeRef} &${plan.itemName}) {`,
      `  auto row = gea::embedded::ui::Document::instance().createImage();`,
      ...attributeLines('row', { tag: 'img' }).map((line) => `  ${line}`),
      ...plan.source.map((line) => `  ${line}`),
      ...plan.rowSlots.lines.map((line) => `  ${line}`),
      `  return row;`,
      `}`,
    )
  }
  helperLines.push(
    `inline void append_row(Context &ctx, gea::embedded::ui::NodeHandle row) {`,
    `  ctx.root.appendChild(row);`,
    `  ctx.rows.push_back(row);`,
    `}`,
    `inline void clear_rows(Context &ctx) {`,
    `  for (const auto &row : ctx.rows) row.remove();`,
    `  ctx.rows.clear();`,
    `}`,
    `inline void apply(Context &ctx) {`,
    `  clear_rows(ctx);`,
  )
  if (typedCarrier) {
    for (const plan of concretePlans) {
      helperLines.push(
        `  for (const auto &${plan.itemName} : ctx.typed_store->${plan.field.fieldName}) append_row(ctx, make_row_${plan.slotIndex}(${plan.itemName}));`,
      )
    }
  } else {
    helperLines.push(`  if (ctx.typed_store) {`)
    for (const plan of concretePlans) {
      helperLines.push(
        `    for (const auto &${plan.itemName} : ctx.typed_store->${plan.field.fieldName}) append_row(ctx, make_row_${plan.slotIndex}(${plan.itemName}));`,
      )
    }
    helperLines.push(`    return;`, `  }`)
    for (const plan of concretePlans) {
      helperLines.push(
        `  {`,
        `    auto &__gea_store_unwrapped = (ctx.store.kind == gea_cpp_value::kind_t::proxy && ctx.store.proxy_target) ? *ctx.store.proxy_target : ctx.store;`,
        `    auto __gea_array_value = __gea_store_unwrapped.record_get_literal(${cppString(plan.field.fieldName)});`,
        `    const auto &__gea_values = (__gea_array_value.kind == gea_cpp_value::kind_t::proxy && __gea_array_value.proxy_target) ? __gea_array_value.proxy_target->array_ref() : __gea_array_value.array_ref();`,
        `    for (const auto &__gea_item_value : __gea_values) {`,
        `      const auto ${plan.itemName} = read_${plan.field.itemType}(__gea_item_value);`,
        `      append_row(ctx, make_row_${plan.slotIndex}(${plan.itemName}));`,
        `    }`,
        `  }`,
      )
    }
  }
  helperLines.push(`}`, `}  // namespace ${helperNs}`, ``)

  const deps = concretePlans.map((plan) => plan.field.fieldName)
  return [
    ...helperLines,
    `inline gea::embedded::ui::NodeHandle ${mountedRendererName(component)}(${typedCarrier ? `const std::shared_ptr<${storeClass}> &store` : 'const gea_cpp_value &store'}, const std::shared_ptr<NativeDisposer> &disposer) {`,
    `  auto root = ${createRoot};`,
    ...attributeLines('root', root, component.template.slots),
    ...setup,
    ...(typedCarrier
      ? [`  auto __gea_ctx = std::make_shared<${helperNs}::Context>(${helperNs}::Context{root, store, {}});`]
      : [
          `  auto __gea_typed_store = gea_cpp_value_as_shared_ptr<${storeClass}>(store);`,
          `  auto __gea_ctx = std::make_shared<${helperNs}::Context>(${helperNs}::Context{root, std::move(__gea_typed_store), store, {}});`,
        ]),
    ...hubAwareCtxBindLines(
      concretePlans[0].field,
      deps.map((dep) => cppString(dep)).join(', '),
      `[__gea_ctx]() mutable -> void { ${helperNs}::apply(*__gea_ctx); }`,
    ),
    `  return root;`,
    `}`,
    ``,
  ]
}

// Emit the reactive registration for a keyed-list context. A typed-carrier
// store (compiled base, not a self-store, hub active) holds the
// `std::shared_ptr` in the context and registers straight into the typed
// SignalHub — no runtime fallback; the store param itself is the typed
// pointer. Everything else keeps the dynamic observe channel.
function hubAwareCtxBindLines(plan: StoreArrayFieldPlan, depsList: string, applyLambda: string): string[] {
  if (!typedStoreCarrier(plan)) return [`  bindReactiveApply(store, disposer, {${depsList}}, ${applyLambda});`]
  return [`  gea_rc_bind_hub_apply(__gea_ctx->typed_store, disposer, {${depsList}}, std::function<void()>(${applyLambda}));`]
}

function keyedListEffectiveRowTemplate(
  rowTemplate: GeaIrTemplate,
  components: GeaIrComponent[],
  itemName: string,
): GeaIrTemplate | null {
  if (parseHtmlRoot(rowTemplate.html)) return rowTemplate
  if (intrinsicImageRowSlot(rowTemplate.html, rowTemplate.slots)) return rowTemplate
  const mountSlots = rowTemplate.slots.filter((slot) => slot.kind === 'mount')
  if (mountSlots.length !== 1) return null
  const childTag = mountSlotTag(mountSlots[0])
  if (!childTag) return null
  const child = components.find((component) => component.exportName === childTag)
  if (!child || !parseHtmlRoot(child.template.html)) return null
  const bindings = rowComponentPropBindings(mountSlots[0], itemName)
  if (!bindings) return null
  return {
    html: child.template.html,
    slots: child.template.slots.map((slot) => rewriteRowComponentSlot(slot, bindings)),
  }
}

function rowComponentPropBindings(slot: GeaIrSlot, itemName: string): ReadonlyMap<string, string> | null {
  const attrs = mountSlotAttrs(slot)
  if (attrs.length === 0) return null
  const bindings = new Map<string, string>()
  let bindsItem = false
  for (const attr of attrs) {
    const parsed = parseMountAttr(attr)
    if (!parsed) return null
    // `key` belongs to the list reconciler, not to the child component.
    if (parsed.name === 'key') continue
    bindings.set(parsed.name, parsed.expr)
    if (/^[A-Za-z_$][A-Za-z0-9_$]*$/.test(parsed.expr) && sanitizeCppIdentifier(parsed.expr) === itemName) {
      bindsItem = true
    }
  }
  return bindsItem ? bindings : null
}

function rewriteRowComponentSlot(slot: GeaIrSlot, bindings: ReadonlyMap<string, string>): GeaIrSlot {
  return {
    ...slot,
    ...(slot.expr ? { expr: rewriteRowComponentExpression(slot.expr, bindings) } : {}),
    ...(slot.exprObjectFields
      ? {
          exprObjectFields: slot.exprObjectFields.map((field) => ({
            ...field,
            expr: rewriteRowComponentExpression(field.expr, bindings),
          })),
        }
      : {}),
  }
}

function rewriteRowComponentExpression(expr: string, bindings: ReadonlyMap<string, string>): string {
  let rewritten = expr
  // Longest names first keeps `props.bookTitle` independent from `props.book`.
  const entries = [...bindings.entries()].sort(([a], [b]) => b.length - a.length)
  for (const [propName, value] of entries) {
    const prop = escapeRegExp(propName)
    const replacement = /^[A-Za-z_$][A-Za-z0-9_$]*$/.test(value) ? value : `(${value})`
    rewritten = rewritten
      .replace(new RegExp(`\\bthis\\.props\\.${prop}\\b`, 'g'), replacement)
      .replace(new RegExp(`\\bprops\\.${prop}\\b`, 'g'), replacement)
  }
  return rewritten
}

function keyedListMountedRenderer(
  component: GeaIrComponent,
  components: GeaIrComponent[],
  storeFields: StoreFieldPlan[],
  storeArrayFields: StoreArrayFieldPlan[],
  constants: ConstantMap,
  storeMethods: StoreMethodPlan[],
): string[] | null {
  const slots = component.template.slots.filter((slot) => slot.kind === 'keyed-list')
  if (slots.length !== 1) return null

  const slot = slots[0]
  const fieldName = fieldNameForSlot(slot)
  const field = fieldName ? unique(storeArrayFields.filter((candidate) => candidate.fieldName === fieldName)) : null
  const payload = keyedListPayload(slot)
  if (!payload) return null
  const itemName = payload.itemParam ? sanitizeCppIdentifier(payload.itemParam) : 'item'
  const rowTemplate = payload.rowTemplate ? keyedListEffectiveRowTemplate(payload.rowTemplate, components, itemName) : null
  const root = parseHtmlRoot(component.template.html)
  const intrinsicRowSlot = rowTemplate ? intrinsicImageRowSlot(rowTemplate.html, rowTemplate.slots) : null
  const row = rowTemplate ? (parseHtmlRoot(rowTemplate.html) ?? (intrinsicRowSlot ? { tag: 'img' } : null)) : null
  if (!field || !rowTemplate || !root || !row) return null
  const rowRootVar = '__gea_row_root'
  // Thread the store plans: without them a root-level event handler on a
  // keyed-list component (`onTouchMove={() => scrollProbe.track()}`) loses
  // the typed direct-call lowering and re-boxes the whole store through the
  // retained proxy + record_get_literal dispatch ON EVERY EVENT.
  const setup = rootSetupLines('root', component.template.slots, storeFields, storeMethods, undefined, root.tag, constants, 'disposer')
  // The row's text / class / event / style slots are all handled by
  // lowerRowSlots (which threads item references + walk-path targeting). Only
  // pass the remaining attr slots through rootSetupLines so it doesn't choke
  // on item-referencing event handlers like `() => store.select(item.id)`.
  const rowSetupSlots = rowTemplate.slots.filter(
    (s) =>
      s.kind !== 'event' &&
      s.kind !== 'class' &&
      s.kind !== 'text' &&
      s.kind !== 'style' &&
      // JSX `key` is reconciler metadata, not a DOM attribute. Passing an
      // item-derived key through rootSetupLines rejects the native renderer.
      !(s.kind === 'attr' && slotAttrName(s) === 'key') &&
      // A dynamic `<img src={item.field}>` is handled by lowerRowSlots via the
      // runtime asset lookup; routing it through rootSetupLines (literal/global
      // only) would reject the whole keyed list.
      !(s.kind === 'attr' && slotAttrName(s) === 'src' && !!s.expr && stringLiteralValue(s.expr) === null),
  )
  const rowSetup = intrinsicRowSlot ? mountSlotImageSourceLines(rowRootVar, intrinsicRowSlot) : rootSetupLines(rowRootVar, rowSetupSlots, [], [], undefined, undefined, constants)
  if (!setup || !rowSetup) return null

  const rowStyleSlot = intrinsicRowSlot ? mountSlotStyleSlot(intrinsicRowSlot) : null

  // Build the row's nested element tree. Without this, `make_row` only
  // creates the row's root element and every child binding falls through to
  // 0×0 placeholders (visible as static "0" text on screen). The tree
  // walker maps each child element to a named local (`__gea_row_0`, etc.) so
  // the slot lowering below can target reactive bindings to the correct
  // nested element via the slot's IR walk path.
  const rowTreeParsed = intrinsicRowSlot ? null : parseTemplateRoot(rowTemplate.html)
  let rowVarCounter = 0
  const nextRowVar = () => `__gea_row_${rowVarCounter++}`
  let rowChildTree: { ok: boolean; treeLines: string[]; pathVars: Map<string, string>; elementPaths: Map<string, number[]> } = {
    ok: true,
    treeLines: [],
    pathVars: new Map([['', rowRootVar]]),
    elementPaths: new Map(),
  }
  if (rowTreeParsed) {
    rowChildTree = emitRowChildTree(rowTreeParsed, rowRootVar, nextRowVar)
    if (!rowChildTree.ok) return null
  }

  const rowSlots = lowerRowSlots(
    rowStyleSlot ? [rowStyleSlot] : rowTemplate.slots,
    itemName,
    field,
    constants,
    rowChildTree.pathVars,
    storeFields,
    storeMethods,
  )
  if (rowSlots.lines.length === 0 && rowChildTree.treeLines.length === 0) return null
  const keyExpr = keyedListKeyExpression(payload, itemName, field)
  const keyExprNeedsItem = keyExpr !== 'std::to_string(__gea_index)'
  const createRoot = uiCreateNodeExpression(root)
  const createRow = uiCreateNodeExpression(row)
  if (!createRoot || !createRow) return null

  // Compile-time keyed-list binding: hoist the whole diff body and the
  // three stateless helpers (same_rendered_item, patch_row, make_row)
  // out of the mount function into per-component free functions in a
  // `gea_kl_<Component>` namespace. The mount keeps just the per-instance
  // context (root + rows) on the heap; the bindReactiveApply lambda's
  // capture shrinks to a single shared_ptr, which fits in std::function's
  // small-buffer optimization and skips the per-binding heap alloc that
  // the previous closure-with-everything-captured form forced. Deps are
  // emitted as a `static constexpr std::array<const char*, N>` so the
  // dep list is .rodata, not a per-call temporary.
  const helperNs = `gea_kl_${sanitizeCppIdentifier(component.exportName)}`
  // Type-use spelling of the row item: `::__gea_type_X` for interface reuse,
  // bare `<Store>_<field>_item` for synthesized structs. The reader fn name
  // keys off the UNQUALIFIED `field.itemType` (`read_<itemType>`) so it never
  // becomes `read_::__gea_type_X`.
  const itemType = field.itemTypeRef
  const itemReaderFn = `read_${field.itemType}`
  const typedCarrier = typedStoreCarrier(field)
  const thresholdPatch = detectThresholdClassPatch(rowSlots, itemName, rowRootVar) ?? detectThresholdStylePatch(rowSlots, itemName, rowRootVar)
  const thresholdStoreFields = new Set(thresholdPatch ? [thresholdPatch.storeField.fieldName] : [])
  const fullApplyStoreFields = rowSlots.storeFields.filter((dep) => !thresholdStoreFields.has(dep.fieldName))
  const shouldPatchOnStoreDeps = fullApplyStoreFields.length > 0
  const afterFullApplyLines = thresholdPatch ? ['rebuild_threshold_order(ctx);', 'sync_threshold_snapshot(ctx);'] : []
  const thresholdReaderCall = thresholdPatch?.storeField.readerName ? `${thresholdPatch.storeField.readerName}(store)` : null
  const rewriteThresholdReader = (line: string): string =>
    thresholdReaderCall ? line.replaceAll(thresholdReaderCall, '__gea_threshold') : line
  // The Context expression that carries the store into patch_row/make_row:
  // typed carrier → the shared_ptr member; boxed → the gea_cpp_value member.
  const ctxStoreExpr = typedCarrier ? 'ctx.typed_store' : 'ctx.store'
  // Body for the typed fast path (or the gea_cpp_value fallback). The two
  // share the same diff/patch logic; they differ only in how they iterate
  // the item collection. Build the body lines once and reuse.
  const diffBodyLines = (itemAccessor: string, itemReadLine: string | null, thresholdExpr: string | null = null): string[] => {
    const patchRowArgs =
      thresholdPatch && thresholdExpr
        ? `entry.row, ${thresholdExpr}, entry.item, ${itemName}, false`
        : `entry.row, ${ctxStoreExpr}, entry.item, ${itemName}, false`
    const makeRowArgs =
      thresholdPatch && thresholdExpr
        ? `${thresholdExpr}, ctx.typed_store, ${itemName}`
        : typedCarrier
          ? `ctx.typed_store, ${itemName}`
          : `ctx.store, ctx.typed_store, ${itemName}`
    const reusedPatchRowArgs =
      thresholdPatch && thresholdExpr
        ? `row_node, ${thresholdExpr}, previous_item, ${itemName}, !reused_row`
        : `row_node, ${ctxStoreExpr}, previous_item, ${itemName}, !reused_row`
    return [
    `if (ctx.rows.size() == ${itemAccessor}.size()) {`,
    `  bool keys_stable = true;`,
    `  std::size_t __gea_index = 0;`,
    ...(keyExprNeedsItem
      ? [
          ...(itemReadLine
            ? [`  for (const auto &__gea_item_value : ${itemAccessor}) {`, `    ${itemReadLine}`]
            : [`  for (const auto &${itemName} : ${itemAccessor}) {`]),
          `    const std::string key = ${keyExpr};`,
          `    if (ctx.rows[__gea_index].key != key) {`,
          `      keys_stable = false;`,
          `      break;`,
          `    }`,
          `    ++__gea_index;`,
          `  }`,
        ]
      : []),
    `  if (keys_stable) {`,
    `    __gea_index = 0;`,
    ...(itemReadLine
      ? [`    for (const auto &__gea_item_value : ${itemAccessor}) {`, `      ${itemReadLine}`]
      : [`    for (const auto &${itemName} : ${itemAccessor}) {`]),
    `      auto &entry = ctx.rows[__gea_index];`,
    `      if (${shouldPatchOnStoreDeps ? 'true' : `!same_rendered_item(entry.item, ${itemName})`}) patch_row(${patchRowArgs});`,
    `      entry.item = ${itemName};`,
    `      ++__gea_index;`,
    `    }`,
    ...afterFullApplyLines.map((line) => `    ${line}`),
    `    return;`,
    `  }`,
    `}`,
    `auto previous_rows = std::move(ctx.rows);`,
    `ctx.rows.clear();`,
    `ctx.rows.reserve(${itemAccessor}.size());`,
    `std::size_t __gea_index = 0;`,
    ...(itemReadLine
      ? [`for (const auto &__gea_item_value : ${itemAccessor}) {`, `  ${itemReadLine}`]
      : [`for (const auto &${itemName} : ${itemAccessor}) {`]),
    `  const std::string key = ${keyExpr};`,
    `  auto row_node = gea::embedded::ui::NodeHandle();`,
    `  auto previous_item = ${itemType}{};`,
    `  bool reused_row = false;`,
    `  for (auto it = previous_rows.begin(); it != previous_rows.end(); ++it) {`,
    `    if (it->key != key) continue;`,
    `    row_node = it->row;`,
    `    previous_item = it->item;`,
    `    reused_row = true;`,
    `    previous_rows.erase(it);`,
    `    break;`,
    `  }`,
    `  if (!row_node) row_node = make_row(${makeRowArgs});`,
    `  if (!reused_row || ${shouldPatchOnStoreDeps ? 'true' : `!same_rendered_item(previous_item, ${itemName})`}) patch_row(${reusedPatchRowArgs});`,
    `  ctx.root.appendChild(row_node);`,
    `  ctx.rows.push_back({key, row_node, ${itemName}});`,
    `  ++__gea_index;`,
    `}`,
    `for (const auto &entry : previous_rows) entry.row.remove();`,
    ...afterFullApplyLines,
    ]
  }
  // patch_row only receives the row root handle, but the per-item patch
  // lines target nested elements by their make_row local (`__gea_row_N`).
  // Re-resolve each referenced nested handle from `row` by walking child
  // indices (the tree shape is fixed across patches). Only emit resolution
  // for vars that patch lines actually reference.
  const patchedVars = new Set<string>()
  for (const patch of rowSlots.patches) {
    for (const line of patch.lines) {
      for (const match of line.matchAll(/\b(__gea_row_\d+)\b/g)) patchedVars.add(match[1])
    }
  }
  for (const line of rowSlots.unconditionalPatches) {
    for (const match of line.matchAll(/\b(__gea_row_\d+)\b/g)) patchedVars.add(match[1])
  }
  const patchResolveLines: string[] = []
  for (const varName of patchedVars) {
    const path = rowChildTree.elementPaths.get(varName)
    if (!path) continue
    const initList = path.join(', ')
    patchResolveLines.push(`auto ${varName} = ${helperNs}::resolve_child(${rowRootVar}, {${initList}});`)
  }

  const helperLines: string[] = [
    `namespace ${helperNs} {`,
    `inline gea::embedded::ui::NodeHandle resolve_child(gea::embedded::ui::NodeHandle parent, std::initializer_list<int> path) {`,
    `  auto &tree = gea::embedded::ui::Tree::instance();`,
    `  int id = parent.id();`,
    `  for (int idx : path) {`,
    `    if (id < 0 || id >= tree.nodeCount()) return gea::embedded::ui::NodeHandle();`,
    `    int child = tree.node(id).first_child;`,
    `    for (int i = 0; i < idx && child >= 0; ++i) child = tree.node(child).next_sibling;`,
    `    id = child;`,
    `  }`,
    `  return gea::embedded::ui::NodeHandle(id);`,
    `}`,
    `inline bool same_rendered_item(const ${itemType} &a, const ${itemType} &b) {`,
    ...sameItemLines(field, rowSlots.itemFieldNames).map((line) => `  ${line}`),
    `}`,
    thresholdPatch
      ? `inline void patch_row(gea::embedded::ui::NodeHandle ${rowRootVar}, double __gea_threshold, const ${itemType} &previous, const ${itemType} &${itemName}, bool initial) {`
      : typedCarrier
        ? `inline void patch_row(gea::embedded::ui::NodeHandle ${rowRootVar}, const std::shared_ptr<${field.storeClass}> &store, const ${itemType} &previous, const ${itemType} &${itemName}, bool initial) {`
        : `inline void patch_row(gea::embedded::ui::NodeHandle ${rowRootVar}, const gea_cpp_value &store, const ${itemType} &previous, const ${itemType} &${itemName}, bool initial) {`,
    thresholdPatch ? `  (void)__gea_threshold;` : `  (void)store;`,
    ...patchResolveLines.map((line) => `  ${line}`),
    ...rowPatchLines(itemName, rowSlots.patches, rowSlots.unconditionalPatches).map(rewriteThresholdReader).map((line) => `  ${line}`),
    `}`,
    thresholdPatch
      ? `inline gea::embedded::ui::NodeHandle make_row(double __gea_threshold, const std::shared_ptr<${field.storeClass}> &typed_store, const ${itemType} &${itemName}) {`
      : typedCarrier
        ? `inline gea::embedded::ui::NodeHandle make_row(const std::shared_ptr<${field.storeClass}> &store, const ${itemType} &${itemName}) {`
        : `inline gea::embedded::ui::NodeHandle make_row(const gea_cpp_value &store, const std::shared_ptr<${field.storeClass}> &typed_store, const ${itemType} &${itemName}) {`,
    // Row event handlers reference `typed_store`; in the typed flavor the
    // store param IS the typed pointer, so alias it.
    ...(thresholdPatch ? [] : typedCarrier ? [`  const auto &typed_store = store;`] : []),
    thresholdPatch ? `  (void)__gea_threshold; (void)typed_store; (void)${itemName};` : `  (void)store; (void)typed_store; (void)${itemName};`,
    `  auto ${rowRootVar} = ${createRow};`,
    ...attributeLines(rowRootVar, row, rowTemplate.slots).map((line) => `  ${line}`),
    ...rowSetup.map((line) => `  ${line}`),
    ...rowChildTree.treeLines,
    ...rowSlots.lines.map(rewriteThresholdReader).map((line) => `  ${line}`),
    ...rowSlots.eventLines.map((line) => `  ${line}`),
    `  return ${rowRootVar};`,
    `}`,
    // The Context now holds a typed `std::shared_ptr<${storeClass}>` to the
    // backing store. The mount fn extracts it once via
    // `gea_cpp_value_as_shared_ptr<${storeClass}>(store)` (an existing
    // runtime helper that walks proxy_target → object_owner). The apply
    // iterates `ctx.typed_store->${fieldName}` directly — a plain
    // `std::vector<${itemType}>` reference. The previous form went through
    // `store.record_get_literal("${fieldName}")` every fire, which calls
    // `gea_cpp_key(const std::vector<${itemType}>&)` and **heap-allocates a
    // gea_cpp_value record per element** to box them into an array_value
    // snapshot, then back-converts each element to typed
    // (`read_${itemType}`) every frame. Direct access skips the entire
    // snapshot dance.
    //
    // Falls back to the gea_cpp_value path when typed extraction returns
    // null — covers the case where the store wasn't a typed-storage
    // candidate, or the proxy/object_owner relationship isn't intact.
    ...(thresholdPatch ? [`struct ThresholdSnapshot { double ${thresholdPatch.storeField.fieldName} = 0.0; bool valid = false; };`] : []),
    `struct Context {`,
    `  gea::embedded::ui::NodeHandle root;`,
    `  std::shared_ptr<${field.storeClass}> typed_store;`,
    ...(typedCarrier ? [] : [`  gea_cpp_value store;`]),
    ...(thresholdPatch ? [`  ThresholdSnapshot threshold;`] : []),
    ...(thresholdPatch ? [`  std::vector<std::size_t> threshold_order;`] : []),
    `  struct RowEntry { std::string key; gea::embedded::ui::NodeHandle row; ${itemType} item; };`,
    `  std::vector<RowEntry> rows;`,
    `};`,
    ...(thresholdPatch ? thresholdPatchHelperLines(thresholdPatch, !!typedCarrier) : []),
    // Typed fast path. The mounted renderer definitions are emitted after
    // geatsc's store classes and typed-array rewrites, so this body can
    // dereference the concrete store class directly in both single-app and
    // application translation units.
    `inline void apply_typed_path(Context &ctx, ${field.storeClass} *typed_store) {`,
    ...(thresholdPatch ? [`  const double __gea_threshold = typed_store->${sanitizeCppIdentifier(thresholdPatch.storeField.fieldName)};`] : []),
    // A getter is recomputed by CALLING the zero-arg getter (`name()`); a field
    // reads the stored vector member. Both are `std::vector<${itemType}>` of
    // typed structs (a getter's derived-array element type stays typed — the
    // mutation analyzer ignores Symbol-keyed dirty-bit writes), so the diff body
    // iterates them identically with no per-element conversion.
    `  const auto &__gea_typed_items = typed_store->${field.fieldName}${field.isGetter ? '()' : ''};`,
    ...diffBodyLines('__gea_typed_items', null, thresholdPatch ? '__gea_threshold' : null).map((line) => `  ${line}`),
    `}`,
    // Outer apply: instantiates the typed-fast-path template at the call
    // site if the typed pointer is available, else falls through to the
    // gea_cpp_value snapshot path. The fast path skips
    // `gea_cpp_key(std::vector<${itemType}>&)` — a per-frame allocation of
    // ${itemType.length}-many record entries to box items as gea_cpp_value
    // — and the matching `read_${field.itemType}` round-trip.
    `inline void apply(Context &ctx) {`,
    ...(typedCarrier
      ? [`  apply_typed_path(ctx, ctx.typed_store.get());`]
      : [
          `  if (ctx.typed_store) {`,
          `    apply_typed_path(ctx, ctx.typed_store.get());`,
          `    return;`,
          `  }`,
          // A getter is computed, not a stored record field, so there is no
          // `record_get_literal` snapshot to read — a getter-backed list requires the
          // typed store. (In practice a store carrying a getter is always typed.)
          ...(field.isGetter
            ? [`  return;`]
            : [
                // Fallback: gea_cpp_value snapshot path. Reads the field via the
                // dynamic record interface and converts each item back to typed via
                // read_${field.itemType}.
                `  auto &__gea_store_unwrapped = (ctx.store.kind == gea_cpp_value::kind_t::proxy && ctx.store.proxy_target) ? *ctx.store.proxy_target : ctx.store;`,
                `  auto __gea_array_value = __gea_store_unwrapped.record_get_literal(${cppString(field.fieldName)});`,
                `  const auto &__gea_values = (__gea_array_value.kind == gea_cpp_value::kind_t::proxy && __gea_array_value.proxy_target) ? __gea_array_value.proxy_target->array_ref() : __gea_array_value.array_ref();`,
                ...(thresholdPatch && thresholdPatch.storeField.readerName
                  ? [`  const double __gea_threshold = ${thresholdPatch.storeField.readerName}(ctx.store);`]
                  : []),
                ...diffBodyLines(
                  '__gea_values',
                  `const auto ${itemName} = ${itemReaderFn}(__gea_item_value);`,
                  thresholdPatch ? '__gea_threshold' : null,
                ).map((line) => `  ${line}`),
              ]),
        ]),
    `}`,
    `}  // namespace ${helperNs}`,
    ``,
  ]

  return [
    ...helperLines,
    `inline gea::embedded::ui::NodeHandle ${mountedRendererName(component)}(${typedCarrier ? `const std::shared_ptr<${field.storeClass}> &store` : 'const gea_cpp_value &store'}, const std::shared_ptr<NativeDisposer> &disposer) {`,
    `  auto root = ${createRoot};`,
    ...attributeLines('root', root, component.template.slots),
    ...setup,
    ...(typedCarrier
      ? [`  auto __gea_ctx = std::make_shared<${helperNs}::Context>(${helperNs}::Context{root, store, {}});`]
      : [
          `  auto __gea_typed_store = gea_cpp_value_as_shared_ptr<${field.storeClass}>(store);`,
          `  auto __gea_ctx = std::make_shared<${helperNs}::Context>(${helperNs}::Context{root, std::move(__gea_typed_store), store, {}});`,
        ]),
    // A getter binds to its dependency fields (its `this.<field>` reads); a
    // field binds to itself. Both also bind any extra store fields read by row
    // slots (`fullApplyStoreFields`).
    ...hubAwareCtxBindLines(
      field,
      uniqueItemFieldNames([...(field.isGetter ? field.reactiveDeps ?? [] : [field.fieldName]), ...fullApplyStoreFields.map((dep) => dep.fieldName)]).map((dep) => cppString(dep)).join(', '),
      `[__gea_ctx]() mutable -> void { ${helperNs}::apply(*__gea_ctx); }`,
    ),
    ...(thresholdPatch
      ? hubAwareCtxBindLines(
          field,
          cppString(thresholdPatch.storeField.fieldName),
          `[__gea_ctx]() mutable -> void { ${helperNs}::patch_threshold_from_store(*__gea_ctx); }`,
        )
      : []),
    `  return root;`,
    `}`,
    ``,
  ]
}

function sameItemLines(field: StoreArrayFieldPlan, itemFieldNames = field.itemFields.map((itemField) => sanitizeCppIdentifier(itemField.name))): string[] {
  const comparisons = uniqueItemFieldNames(itemFieldNames)
    .map((name) => field.itemFields.find((itemField) => sanitizeCppIdentifier(itemField.name) === name))
    .filter((itemField): itemField is GeaIrStoreField => itemField !== undefined)
    .map((itemField) => sameItemComparison(itemField))
  if (comparisons.some((comparison) => comparison === null) || comparisons.length === 0) {
    return [
      '// TODO(gea-ir): add item comparators for non-literal keyed-list fields so mounted rows can patch only changed items.',
      'return false;',
    ]
  }
  return [`return ${comparisons.join(' && ')};`]
}

// Emits the row's nested element tree. Returns:
//   treeLines    — element create + classList + appendChild lines for every
//                  child of the row root, run once inside make_row before
//                  apply lines. An empty array means the row has no children.
//   pathVars     — walk-path → local-var-name map. Slot lowering uses this to
//                  target a specific nested element by its IR walk path. The
//                  root walk ([]) maps to the row var name supplied by the
//                  caller.
//   ok           — false if any element couldn't be created (returns early).
function emitRowChildTree(parsed: TemplateElement, rowVarName: string, nextVar: () => string): {
  ok: boolean
  treeLines: string[]
  pathVars: Map<string, string>
  elementPaths: Map<string, number[]>
} {
  const treeLines: string[] = []
  const pathVars = new Map<string, string>()
  const elementPaths = new Map<string, number[]>()
  pathVars.set('', rowVarName)
  // parseTemplateRoot wraps the row in a synthetic `#fragment`. The row's
  // own root element is already created by the caller (as `rowVarName` via
  // `createRow`), so descend into that element's children rather than
  // re-creating it. If the parse didn't wrap (already the root element), use
  // it directly.
  const rowRoot =
    parsed.tag === '#fragment' && parsed.children.length === 1 && parsed.children[0].kind === 'element'
      ? (parsed.children[0] as TemplateElement)
      : parsed
  if (!emitRowChildren(rowRoot, rowVarName, [], treeLines, nextVar, pathVars, elementPaths)) {
    return { ok: false, treeLines: [], pathVars, elementPaths }
  }
  return { ok: true, treeLines, pathVars, elementPaths }
}

function emitRowChildren(
  parent: TemplateElement,
  parentVar: string,
  parentPath: number[],
  lines: string[],
  nextVar: () => string,
  pathVars: Map<string, string>,
  elementPaths: Map<string, number[]>,
): boolean {
  for (let index = 0; index < parent.children.length; index += 1) {
    const child = parent.children[index]
    const childPath = [...parentPath, index]
    if (child.kind === 'element') {
      const createChild = uiCreateNodeExpression(child)
      if (!createChild) return false
      const childVar = nextVar()
      lines.push(`  auto ${childVar} = ${createChild};`)
      lines.push(`  ${childVar}.setTagName(${cppString(child.tag.toLowerCase())});`)
      if (child.className) {
        lines.push(`  ${childVar}.classList().set(${cppString(child.className)});`)
      }
      if (child.attrs) {
        for (const [name, value] of Object.entries(child.attrs)) {
          const lower = name.toLowerCase()
          if (lower === 'class' || lower === 'style') continue
          lines.push(`  ${childVar}.setAttribute(${cppString(name)}, ${cppString(value)});`)
        }
      }
      lines.push(`  ${parentVar}.appendChild(${childVar});`)
      pathVars.set(childPath.join(','), childVar)
      elementPaths.set(childVar, childPath)
      if (!emitRowChildren(child, childVar, childPath, lines, nextVar, pathVars, elementPaths)) return false
      continue
    }
    if (child.kind === 'text') {
      const text = renderableTemplateText(child.text)
      if (!text) continue
      // The IR uses '0' as the placeholder for `directText` text slots; the
      // matching slot lowering will set the real reactive content. Skip
      // emitting a literal "0" — otherwise the user sees a bare zero on
      // screen until the first reactive bind fires.
      if (text === '0') {
        pathVars.set(childPath.join(','), parentVar)
        continue
      }
      // Static text that isn't a placeholder: bake it into the parent if that
      // parent collapses to a single Text node (one run), otherwise create a
      // sibling Text node. A multi-run text-tag parent (e.g. `<span>{a} / {b}</span>`)
      // stays a View, so baking via setText would overlap the runs. Same heuristic
      // emitTemplateChildren in cpp-template-renderer.ts uses.
      const parentTag = parent.tag.toLowerCase()
      const parentIsTextNode =
        (parentTag === 'span' || parentTag === 'p' || /^h[1-6]$/.test(parentTag)) &&
        spanCollapsesToTextNode(parent.children)
      if (parentIsTextNode) {
        lines.push(`  ${parentVar}.setText(${cppString(text)});`)
        pathVars.set(childPath.join(','), parentVar)
      } else {
        const textVar = nextVar()
        lines.push(`  auto ${textVar} = gea::embedded::ui::Document::instance().createText();`)
        lines.push(`  ${textVar}.setText(${cppString(text)});`)
        lines.push(`  ${parentVar}.appendChild(${textVar});`)
        pathVars.set(childPath.join(','), textVar)
        elementPaths.set(textVar, childPath)
      }
      continue
    }
    // Slot placeholder. The matching slot's walk path will be `childPath`;
    // map it to the parent so reactive bindings can target the right
    // element. We do NOT create a separate node here — the slot apply takes
    // care of inserting / setting content on the parent.
    pathVars.set(childPath.join(','), parentVar)
  }
  return true
}

function rowPatchLines(itemName: string, patches: Array<{ itemFieldName: string; lines: string[] }>, unconditionalPatches: string[] = []): string[] {
  const lines: string[] = []
  lines.push(...unconditionalPatches)
  for (const patch of patches) {
    const fieldName = sanitizeCppIdentifier(patch.itemFieldName)
    lines.push(`if (initial || previous.${fieldName} != ${itemName}.${fieldName}) {`)
    lines.push(...patch.lines.map((line) => `  ${line}`))
    lines.push('}')
  }
  return lines
}

type ThresholdPatch =
  | {
      kind: 'class'
      valueVar: string
      falseValue: string
      itemFieldName: string
      storeField: StoreFieldPlan
      targetVar: string
      trueValue: string
    }
  | {
      kind: 'style'
      valueVar: string
      falseValue: string
      itemFieldName: string
      propertyName: string
      storeField: StoreFieldPlan
      targetVar: string
      trueValue: string
    }

function detectThresholdClassPatch(rowSlots: LoweredRowSlots, itemName: string, rowRootVar: string): ThresholdPatch | null {
  const escapedItemName = escapeRegExp(itemName)
  for (let index = 0; index < rowSlots.unconditionalPatches.length - 1; index += 1) {
    const declaration = rowSlots.unconditionalPatches[index]
    const update = rowSlots.unconditionalPatches[index + 1]
    const match = declaration.match(
      new RegExp(
        `^const std::string ([A-Za-z_][A-Za-z0-9_]*) = \\(\\((${escapedItemName})\\.([A-Za-z_][A-Za-z0-9_]*) < ([A-Za-z_][A-Za-z0-9_]*\\(store\\))\\) \\? std::string\\("([^"]*)"\\) : std::string\\("([^"]*)"\\)\\);$`,
      ),
    )
    if (!match) continue
    const [, valueVar, , itemFieldName, readerCall, trueValue, falseValue] = match
    const updateMatch = update.match(
      new RegExp(
        `^if \\(([A-Za-z_][A-Za-z0-9_]*)\\.classList\\(\\)\\.value\\(\\) != ${escapeRegExp(valueVar)}\\) \\1\\.classList\\(\\)\\.set\\(${escapeRegExp(valueVar)}\\);$`,
      ),
    )
    if (!updateMatch) continue
    const targetVar = updateMatch[1]
    if (targetVar !== rowRootVar) continue
    const storeField = rowSlots.storeFields.find((dep) => dep.readerName !== null && `${dep.readerName}(store)` === readerCall)
    if (!storeField || storeField.fieldType !== 'double') continue
    return {
      kind: 'class',
      valueVar,
      falseValue,
      itemFieldName,
      storeField,
      targetVar,
      trueValue,
    }
  }
  return null
}

function detectThresholdStylePatch(rowSlots: LoweredRowSlots, itemName: string, rowRootVar: string): ThresholdPatch | null {
  const escapedItemName = escapeRegExp(itemName)
  for (const line of rowSlots.unconditionalPatches) {
    const match = line.match(
      new RegExp(
        `^gea::embedded::ui::StyleSheet::instance\\(\\)\\.applyProperty\\(([A-Za-z_][A-Za-z0-9_]*), std::string\\("([^"]+)"\\), \\(\\((${escapedItemName})\\.([A-Za-z_][A-Za-z0-9_]*) < ([A-Za-z_][A-Za-z0-9_]*\\(store\\))\\) \\? std::string\\("([^"]*)"\\) : std::string\\("([^"]*)"\\)\\)\\);$`,
      ),
    )
    if (!match) continue
    const [, targetVar, propertyName, , itemFieldName, readerCall, trueValue, falseValue] = match
    if (targetVar !== rowRootVar) continue
    const storeField = rowSlots.storeFields.find((dep) => dep.readerName !== null && `${dep.readerName}(store)` === readerCall)
    if (!storeField || storeField.fieldType !== 'double') continue
    return {
      kind: 'style',
      valueVar: '__gea_next_threshold_style',
      falseValue,
      itemFieldName,
      propertyName,
      storeField,
      targetVar,
      trueValue,
    }
  }
  return null
}

function thresholdPatchApplyLines(patch: ThresholdPatch): string[] {
  if (patch.kind === 'class') {
    return [`    if (entry.row.classList().value() != ${patch.valueVar}) entry.row.classList().set(${patch.valueVar});`]
  }
  return [
    `    gea::embedded::ui::StyleSheet::instance().applyProperty(entry.row, ${cppString(patch.propertyName)}, ${patch.valueVar});`,
  ]
}

function thresholdPatchHelperLines(patch: ThresholdPatch, typedCarrier: boolean): string[] {
  const fieldName = sanitizeCppIdentifier(patch.storeField.fieldName)
  const readerName = patch.storeField.readerName
  if (!readerName) return []
  return [
    `inline double read_threshold(Context &ctx) {`,
    ...(typedCarrier
      ? [`  return ctx.typed_store->${fieldName};`]
      : [`  if (ctx.typed_store) return ctx.typed_store->${fieldName};`, `  return ${readerName}(ctx.store);`]),
    `}`,
    `inline void sync_threshold_snapshot(Context &ctx) {`,
    `  ctx.threshold.${fieldName} = read_threshold(ctx);`,
    `  ctx.threshold.valid = true;`,
    `}`,
    `inline double threshold_item_value(const Context::RowEntry &entry) {`,
    `  return static_cast<double>(entry.item.${patch.itemFieldName});`,
    `}`,
    `inline void rebuild_threshold_order(Context &ctx) {`,
    `  ctx.threshold_order.clear();`,
    `  ctx.threshold_order.reserve(ctx.rows.size());`,
    `  for (std::size_t __gea_index = 0; __gea_index < ctx.rows.size(); ++__gea_index) ctx.threshold_order.push_back(__gea_index);`,
    `  std::sort(ctx.threshold_order.begin(), ctx.threshold_order.end(), [&ctx](std::size_t a, std::size_t b) {`,
    `    return threshold_item_value(ctx.rows[a]) < threshold_item_value(ctx.rows[b]);`,
    `  });`,
    `}`,
    `inline std::size_t threshold_lower_bound(Context &ctx, double value) {`,
    `  std::size_t low = 0;`,
    `  std::size_t high = ctx.threshold_order.size();`,
    `  while (low < high) {`,
    `    const std::size_t mid = low + ((high - low) / 2);`,
    `    if (threshold_item_value(ctx.rows[ctx.threshold_order[mid]]) < value) low = mid + 1;`,
    `    else high = mid;`,
    `  }`,
    `  return low;`,
    `}`,
    `inline void patch_threshold_range(Context &ctx, double previousThreshold, double nextThreshold) {`,
    `  if (previousThreshold == nextThreshold) {`,
    `    ctx.threshold.${fieldName} = nextThreshold;`,
    `    ctx.threshold.valid = true;`,
    `    return;`,
    `  }`,
    `  double low = previousThreshold < nextThreshold ? previousThreshold : nextThreshold;`,
    `  double high = previousThreshold < nextThreshold ? nextThreshold : previousThreshold;`,
    `  if (ctx.threshold_order.size() != ctx.rows.size()) rebuild_threshold_order(ctx);`,
    `  if (ctx.threshold_order.empty()) {`,
    `    ctx.threshold.${fieldName} = nextThreshold;`,
    `    ctx.threshold.valid = true;`,
    `    return;`,
    `  }`,
    `  std::size_t start = threshold_lower_bound(ctx, low);`,
    `  std::size_t end = threshold_lower_bound(ctx, high);`,
    `  const double threshold = nextThreshold;`,
    `  for (std::size_t __gea_index = start; __gea_index < end; ++__gea_index) {`,
    `    auto &entry = ctx.rows[ctx.threshold_order[__gea_index]];`,
    `    const std::string ${patch.valueVar} = ((entry.item.${patch.itemFieldName} < threshold) ? std::string(${cppString(patch.trueValue)}) : std::string(${cppString(patch.falseValue)}));`,
    ...thresholdPatchApplyLines(patch),
    `  }`,
    `  ctx.threshold.${fieldName} = nextThreshold;`,
    `  ctx.threshold.valid = true;`,
    `}`,
    `inline void patch_threshold_from_store(Context &ctx) {`,
    `  const double nextThreshold = read_threshold(ctx);`,
    `  if (!ctx.threshold.valid) {`,
    `    ctx.threshold.${fieldName} = nextThreshold;`,
    `    ctx.threshold.valid = true;`,
    `    return;`,
    `  }`,
    `  patch_threshold_range(ctx, ctx.threshold.${fieldName}, nextThreshold);`,
    `}`,
  ]
}

function escapeRegExp(value: string): string {
  return value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
}

function uniqueItemFieldNames(fields: string[]): string[] {
  return [...new Set(fields)]
}

function uniqueEqual(values: string[]): string | null {
  if (values.length === 0) return null
  const [first] = values
  return values.every((value) => value === first) ? first : null
}

function intrinsicImageRowSlot(html: string, slots: GeaIrComponent['template']['slots']): GeaIrComponent['template']['slots'][number] | null {
  if (slots.length !== 1) return null
  const slot = slots[0]
  if (slot.kind !== 'mount' || mountSlotTag(slot) !== 'Image') return null
  if (html.trim() === '<!--0-->') return slot
  const root = parseTemplateRoot(html)
  if (!root) return null
  const attrs = root.attrs ?? {}
  const keys = Object.keys(attrs)
  if (keys.length !== 1 || keys[0] !== 'style' || !/\bdisplay\s*:\s*contents\b/i.test(attrs.style)) return null
  if (root.children.length !== 1) return null
  const child = root.children[0]
  return child.kind === 'slot' && child.index === slot.index ? slot : null
}

// A <span>/<p>/<h*> collapses into a single native Text node only when it holds
// exactly ONE text run. With multiple runs (e.g. `<span>{a} / {b}</span>`) it
// must stay a View so each run is a separate child text node — otherwise one run
// is baked via `setText` on the parent and the rest overlap it at the parent
// origin. Mirrors canUseTextNodeForElement() in cpp-template-renderer.ts.
function spanCollapsesToTextNode(children: TemplateChild[]): boolean {
  if (children.some((c) => c.kind === 'element')) return false
  let runs = 0
  for (const c of children) {
    if (c.kind === 'text') {
      if (renderableTemplateText(c.text)) runs += 1
    } else {
      runs += 1
    }
    if (runs > 1) return false
  }
  return true
}

function uiCreateNodeExpression(root: HtmlRoot | TemplateElement): string | null {
  const normalized = root.tag.toLowerCase()
  if (normalized === 'canvas') return 'gea::embedded::ui::Document::instance().createCanvas()'
  if (normalized === 'camera') return 'gea::embedded::ui::Document::instance().createCamera()'
  if (normalized === 'audio') return 'gea::embedded::ui::Document::instance().createAudio()'
  if (normalized === 'img') return 'gea::embedded::ui::Document::instance().createImage()'
  if (normalized === 'virtual-list') return 'gea::embedded::ui::Document::instance().createVirtualList()'
  if (normalized === 'input' && root.attrs?.type === 'button') return 'gea::embedded::ui::Document::instance().createButton()'
  if (normalized === 'button') return 'gea::embedded::ui::Document::instance().createButton()'
  if (normalized === 'span' || normalized === 'p' || /^h[1-6]$/.test(normalized)) {
    // children is present for TemplateElement (child path); absent for a bare
    // HtmlRoot (row root) — default to text node there to preserve prior shape.
    const children = (root as TemplateElement).children
    if (!children || spanCollapsesToTextNode(children)) {
      return 'gea::embedded::ui::Document::instance().createText()'
    }
    return 'gea::embedded::ui::Document::instance().createView()'
  }
  return 'gea::embedded::ui::Document::instance().createView()'
}

function renderableTemplateText(text: string): string {
  const collapsed = text.replace(/\s+/g, ' ')
  return collapsed.trim() ? collapsed : ''
}

function canUseMountedFieldReaders(lowered: LoweredSlots): boolean {
  return lowered.fields.every((field) => field.readerName !== null)
}

function mountedFieldReaderName(field: StoreFieldPlan): string {
  if (field.readerName) return field.readerName
  throw new Error(`missing mounted field reader for ${field.storeClass}.${field.fieldName}`)
}

function sameItemComparison(field: GeaIrStoreField): string | null {
  const shape = fieldShape(field)
  if (shape?.kind !== 'literal') return null
  const name = sanitizeCppIdentifier(field.name)
  return `a.${name} == b.${name}`
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

export function canMountComponent(
  component: GeaIrComponent,
  components: GeaIrComponent[],
  storeFields: StoreFieldPlan[],
  storeArrayFields: StoreArrayFieldPlan[],
  constants: ConstantMap = new Map(),
  storeMethods: StoreMethodPlan[] = [],
): boolean {
  // Reactive self-store components mount ONLY through their typed renderer —
  // a parent in any mode mounts them as `mount_X(std::make_shared<X>(), …)`.
  // The boxed template gates below would otherwise approve a mount call whose
  // `store` argument can't convert to the typed renderer's shared_ptr param.
  if (component.reactiveState) {
    return selfStoreMountedRenderer(component, components, storeFields, storeArrayFields, constants, storeMethods) !== null
  }
  const root = parseHtmlRoot(component.template.html)
  if (!root || !uiCreateNodeExpression(root)) return false

  const keyed = component.template.slots.filter((slot) => slot.kind === 'keyed-list')
  const template = canTemplateMountComponent(component, components, storeFields, constants, (child) =>
    child !== component && canMountComponent(child, components, storeFields, storeArrayFields, constants, storeMethods),
    storeMethods,
    storeArrayFields,
  )
  if (template) return true

  if (keyed.length === 1 && canUseDedicatedKeyedListRenderer(component, constants)) {
    return keyedListMountedRenderer(component, components, storeFields, storeArrayFields, constants, storeMethods) !== null
  }
  if (keyed.length > 1 && canUseDedicatedKeyedListRenderer(component, constants)) {
    return multiKeyedListMountedRenderer(component, storeArrayFields, constants) !== null
  }

  const lowered = lowerSlots(component.template.slots, storeFields, constants)
  if (!rootSetupSlotsSupported(component.template.slots, storeFields, storeMethods, constants)) return false
  return (
    component.template.slots.length === 0 ||
    component.template.slots.every((slot) => isRootSetupSlot(slot, constants)) ||
    (canUseStyleTextMountedSlots(component.template.slots) && lowered !== null && canUseMountedFieldReaders(lowered))
  )
}

function canUseStyleTextMountedSlots(slots: GeaIrComponent['template']['slots']): boolean {
  return slots.every((slot) => slot.kind === 'style' || slot.kind === 'text' || isRootSetupSlot(slot))
}
