import { collectSingletonMethodsFromSourceFiles, collectStoreArraySources, collectStoreFields, collectStoreMethods, enrichConstantInitializerShapes, type StoreArrayFieldPlan, type StoreFieldPlan, type StoreMethodPlan } from './cpp-stores.js'
import { isRootSetupSlot, objectArrayConstant, rootSetupLines } from './cpp-mounted-lowering.js'
import { canMountComponent, mountedRendererForComponent, mountedRendererName, selfStoreMountedRenderer } from './cpp-mounted.js'
import { collectIrConstants } from './cpp-ir.js'
import { parseTemplateRoot } from './cpp-template-renderer.js'
import type { RendererReplacement } from './cpp-source-replacements.js'
import type { GeaIrBundleV1, GeaIrComponent, GeaIrConstant, GeaIrSlot } from './types.js'
import { isRuntimeNumericConstantIdentifier, sanitizeCppIdentifier } from './utils.js'

export { replaceGeneratedRenderers } from './cpp-source-replacements.js'
export type { RendererReplacement } from './cpp-source-replacements.js'

export function rendererReplacementsForIr(ir: GeaIrBundleV1): RendererReplacement[] {
  const constants = collectIrConstants(ir)
  enrichConstantInitializerShapes(ir.stores, constants, ir.modules.map((module) => module.file))
  const storeArrayFields = collectStoreArraySources(ir.stores)
  const storeFields = collectStoreFields(ir.stores)
  const storeMethods = collectStoreMethods(ir.stores).concat(collectSingletonMethodsFromSourceFiles(ir.modules.map((module) => module.file), ir.stores))
  const replacements: RendererReplacement[] = []

  for (const component of ir.components) {
    const rendererIsGenerated = (): boolean =>
      mountedRendererForComponent(component, ir.components, storeFields, storeArrayFields, constants, storeMethods) !== null
    // EXPERIMENTAL (ReactiveComponent): a self-store component's store is
    // unambiguously itself, so we don't run the slot-by-slot store inference
    // (which requires every slot to reduce to a single `store.field`). The gate
    // is exactly "the unified typed renderer can be generated" — the SAME call
    // generateCppIrMountedSource makes, so replacement and renderer stay in
    // lockstep; the direct-mount planner mounts via `make_shared<Class>()`.
    const reactiveSelfStore = withRootRefs(
      component,
      reactiveSelfStoreReplacement(component, ir.components, storeFields, storeArrayFields, constants, storeMethods),
    )
    if (reactiveSelfStore) {
      replacements.push(reactiveSelfStore)
      continue
    }

    const staticRenderer = withRootRefs(component, staticComponentReplacement(component, storeFields))
    if (staticRenderer && rendererIsGenerated()) {
      replacements.push(staticRenderer)
      continue
    }

    const keyed = withRootRefs(component, keyedListReplacement(component, storeArrayFields, storeFields))
    if (keyed && rendererIsGenerated()) {
      replacements.push(keyed)
      continue
    }

    const style = withRootRefs(component, styleComponentReplacement(component, ir.components, storeFields, storeArrayFields, constants, storeMethods))
    if (style && rendererIsGenerated()) {
      replacements.push(style)
      continue
    }

    const text = withRootRefs(component, textComponentReplacement(component, ir.components, storeFields, storeArrayFields, constants, storeMethods))
    if (text && rendererIsGenerated()) {
      replacements.push(text)
      continue
    }

    const mount = withRootRefs(component, mountComponentReplacement(component, ir.components, storeFields, storeArrayFields, constants, storeMethods))
    if (mount && rendererIsGenerated()) {
      replacements.push(mount)
      continue
    }

    const template = withRootRefs(component, templateComponentReplacement(component, ir.components, storeFields, storeArrayFields, constants, storeMethods))
    if (template && rendererIsGenerated()) replacements.push(template)
  }

  return replacements
}

export function inlineOnlyComponentClassesForIr(ir: GeaIrBundleV1): Set<string> {
  const replacements = rendererReplacementsForIr(ir)
  const replaceableClasses = new Set(replacements.map((replacement) => replacement.className))
  const componentsByName = new Map(ir.components.map((component) => [component.exportName, component]))
  const inlineOnly = new Set<string>()
  const maybeAddInlineOnlyMount = (slot: GeaIrSlot) => {
    if (slot.kind !== 'mount' || mountSlotAttrs(slot).length === 0) return
    const tag = mountSlotTag(slot)
    const child = tag ? componentsByName.get(tag) : null
    if (!child) return
    const childClass = sanitizeCppIdentifier(child.exportName)
    if (!replaceableClasses.has(childClass)) inlineOnly.add(childClass)
  }
  for (const component of ir.components) {
    if (!replaceableClasses.has(sanitizeCppIdentifier(component.exportName))) continue
    for (const slot of component.template.slots) {
      maybeAddInlineOnlyMount(slot)
      if (slot.kind === 'keyed-list') {
        for (const rowSlot of keyedListRowTemplateSlots(slot)) maybeAddInlineOnlyMount(rowSlot)
      }
    }
  }
  return inlineOnly
}

function keyedListRowTemplateSlots(slot: GeaIrSlot): GeaIrSlot[] {
  const payload = slot.payload
  if (!payload || typeof payload !== 'object') return []
  const rowTemplate = (payload as { rowTemplate?: unknown }).rowTemplate
  if (!rowTemplate || typeof rowTemplate !== 'object') return []
  const slots = (rowTemplate as { slots?: unknown }).slots
  return Array.isArray(slots) ? (slots as GeaIrSlot[]) : []
}

function withRootRefs(component: GeaIrComponent, replacement: RendererReplacement | null): RendererReplacement | null {
  if (!replacement) return null
  const refFields = component.template.slots
    .filter((slot) => slot.kind === 'ref' && slot.walk.length === 0 && slot.exprPath?.length === 2 && slot.exprPath[0] === 'this')
    .map((slot) => sanitizeCppIdentifier(slot.exprPath![1]))
  if (refFields.length > 0) replacement.refFields = [...new Set(refFields)]
  return replacement
}

function reactiveSelfStoreReplacement(
  component: GeaIrComponent,
  components: GeaIrComponent[],
  storeFields: StoreFieldPlan[],
  storeArrayFields: StoreArrayFieldPlan[],
  constants: ReadonlyMap<string, GeaIrConstant>,
  storeMethods: StoreMethodPlan[],
): RendererReplacement | null {
  if (!component.reactiveState) return null
  // Mountable iff the unified typed renderer generates for this template.
  if (!selfStoreMountedRenderer(component, components, storeFields, storeArrayFields, new Map(constants), storeMethods)) return null
  return {
    className: sanitizeCppIdentifier(component.exportName),
    rendererName: `render_${sanitizeCppIdentifier(component.exportName)}`,
    mountedRendererName: mountedRendererName(component),
    // Placeholder: the direct-mount planner sees `reactiveState` and mounts the
    // live `make_shared<Class>()` instance, so this value is never read.
    storeExpression: 'this',
  }
}

function staticComponentReplacement(component: GeaIrComponent, storeFields: StoreFieldPlan[]): RendererReplacement | null {
  if (component.template.slots.length !== 0) {
    if (!component.template.slots.every((slot) => isRootSetupSlot(slot))) return null
    if (!rootSetupLines('root', component.template.slots, storeFields)) return null
  }
  const eventStoreExpression = eventStoreExpressionForComponent(component)
  if (eventStoreExpression === undefined) return null
  return {
    className: sanitizeCppIdentifier(component.exportName),
    rendererName: `render_${sanitizeCppIdentifier(component.exportName)}`,
    mountedRendererName: mountedRendererName(component),
    storeExpression: eventStoreExpression ?? 'gea_cpp_value::missing()',
  }
}

function keyedListReplacement(
  component: GeaIrComponent,
  storeArrayFields: StoreArrayFieldPlan[],
  storeFields: StoreFieldPlan[],
): RendererReplacement | null {
  const slots = component.template.slots.filter((slot) => slot.kind === 'keyed-list')
  if (slots.length !== 1) return null
  if (!component.template.slots.every((slot) => slot === slots[0] || isRootSetupSlot(slot))) return null
  if (!rootSetupLines('root', component.template.slots, storeFields)) return null

  const slot = slots[0]
  const storeExpression = storeExpressionForSlot(slot)
  const fieldName = slotFieldName(slot)
  if (!storeExpression || !fieldName) return null

  const matches = storeArrayFields.filter((field) => field.fieldName === fieldName)
  if (matches.length !== 1) return null

  return {
    className: sanitizeCppIdentifier(component.exportName),
    rendererName: `render_${sanitizeCppIdentifier(component.exportName)}_${matches[0].fieldName}`,
    mountedRendererName: mountedRendererName(component),
    stateReaderName: `read_${matches[0].stateType}`,
    storeExpression: sanitizeCppIdentifier(storeExpression),
  }
}

function styleComponentReplacement(
  component: GeaIrComponent,
  components: GeaIrComponent[],
  storeFields: StoreFieldPlan[],
  storeArrayFields: StoreArrayFieldPlan[],
  constants: ReadonlyMap<string, GeaIrConstant>,
  storeMethods: StoreMethodPlan[],
): RendererReplacement | null {
  // The shell this replacement produces calls `gea_ir::mount_<Name>` — it must
  // only exist when the mounted renderer will actually be generated, or the
  // reference dangles at link time. `canMountComponent` is the same gate the
  // renderer generator applies (a reactive self-store component reaching this
  // point has already failed its typed renderer, so it never gets a mount).
  if (component.reactiveState) return null
  if (!canMountComponent(component, components, storeFields, storeArrayFields, constants, storeMethods)) return null
  if (component.template.slots.some((slot) => slot.kind === 'keyed-list')) return null
  if (!component.template.slots.every((slot) => slot.kind === 'style' || isRootSetupSlot(slot))) return null
  if (!rootSetupLines('root', component.template.slots, storeFields)) return null

  const fields = component.template.slots.flatMap((slot) => (slot.kind === 'style' ? (slot.exprObjectFields ?? []) : []))
  if (fields.length === 0) return null

  const refs: StoreReference[] = []
  for (const field of fields) {
    const fieldRefs = expressionReferences(field.expr, new Map(), constants)
    if (!fieldRefs) return null
    refs.push(...fieldRefs)
  }

  const replacement = replacementFromReferences(component, refs, storeFields)
  return replacementMatchesEventStore(component, replacement)
}

function textComponentReplacement(
  component: GeaIrComponent,
  components: GeaIrComponent[],
  storeFields: StoreFieldPlan[],
  storeArrayFields: StoreArrayFieldPlan[],
  constants: ReadonlyMap<string, GeaIrConstant>,
  storeMethods: StoreMethodPlan[],
): RendererReplacement | null {
  // Same lockstep rule as styleComponentReplacement: no shell without a
  // generated `mount_<Name>` renderer behind it.
  if (component.reactiveState) return null
  if (!canMountComponent(component, components, storeFields, storeArrayFields, constants, storeMethods)) return null
  if (component.template.slots.some((slot) => slot.kind === 'keyed-list')) return null
  if (!component.template.slots.every((slot) => slot.kind === 'text' || slot.kind === 'style' || isRootSetupSlot(slot))) return null
  if (!rootSetupLines('root', component.template.slots, storeFields)) return null

  const slots = component.template.slots.filter((slot) => slot.kind === 'text')
  if (slots.length === 0 || slots.some((slot) => !slot.directText)) return null

  const refs: StoreReference[] = []
  for (const slot of slots) {
    const slotRefs = textSlotReferences(slot, constants)
    if (!slotRefs) return null
    refs.push(...slotRefs)
  }
  if (refs.length === 0) return null

  const replacement = replacementFromReferences(component, refs, storeFields)
  return replacementMatchesEventStore(component, replacement)
}

function mountComponentReplacement(
  component: GeaIrComponent,
  components: GeaIrComponent[],
  storeFields: StoreFieldPlan[],
  storeArrayFields: StoreArrayFieldPlan[],
  constants: ReadonlyMap<string, GeaIrConstant>,
  storeMethods: StoreMethodPlan[],
): RendererReplacement | null {
  const slots = component.template.slots.filter((slot) => slot.kind === 'mount')
  if (slots.length === 0) return null
  if (!component.template.slots.every((slot) => slot.kind === 'mount' || isRootSetupSlot(slot))) return null
  if (!rootSetupLines('root', component.template.slots, storeFields)) return null
  if (!canMountComponent(component, components, storeFields, storeArrayFields, constants, storeMethods)) return null

  const refs: StoreReference[] = []
  for (const slot of slots) {
    const tag = mountSlotTag(slot)
    const child = tag ? components.find((candidate) => candidate.exportName === tag) : null
    if (!child) return null
    const childRefs = componentStoreReferences(child, components, storeFields, storeArrayFields, constants, storeMethods)
    if (!childRefs) return null
    refs.push(...childRefs)
  }
  if (refs.length === 0) return null

  // Single-store case: every child slot references the same store. Thread it
  // through as the parent's `storeExpression` so `mount_<child>(store, …)`
  // bindings (`bindReactiveApply(store, …)`, `read_<Store>_field_X(store)`)
  // see the actual store, not `missing()`. This is the path analog-clock and
  // every other single-store composition takes — passing `missing()` here
  // makes every reactive field read return 0 and freezes the UI.
  const singleStoreReplacement = replacementFromReferences(component, refs, storeFields)
  if (singleStoreReplacement) return replacementMatchesEventStore(component, singleStoreReplacement)

  // Multi-store composition (e.g. launcher's <App> whose children touch both
  // `launcher` and `Settings`). No single store fits the parent, but every
  // child's own `mount_<child>` renderer fetches its store via
  // `__gea_global_*()`, so passing `missing()` here is harmless — the param
  // store is never read inside any analyzable single-store child renderer.
  // (`replacementFromReferences` returned null only when the ref set spans
  // multiple distinct store expressions; that's the case the comment about
  // "children may collectively use any number of stores" covers, and the only
  // case where this fallback is reached.)
  return replacementMatchesEventStore(component, {
    className: sanitizeCppIdentifier(component.exportName),
    rendererName: `render_${sanitizeCppIdentifier(component.exportName)}`,
    mountedRendererName: mountedRendererName(component),
    storeExpression: 'gea_cpp_value::missing()',
  })
}

function templateComponentReplacement(
  component: GeaIrComponent,
  components: GeaIrComponent[],
  storeFields: StoreFieldPlan[],
  storeArrayFields: StoreArrayFieldPlan[],
  constants: ReadonlyMap<string, GeaIrConstant>,
  storeMethods: StoreMethodPlan[],
): RendererReplacement | null {
  const refs = componentStoreReferences(component, components, storeFields, storeArrayFields, constants, storeMethods)
  if (!refs) return null
  if (refs.length === 0) {
    const eventStoreExpression = eventStoreExpressionForComponent(component)
    if (eventStoreExpression === undefined) return null
    return {
      className: sanitizeCppIdentifier(component.exportName),
      rendererName: `render_${sanitizeCppIdentifier(component.exportName)}`,
      mountedRendererName: mountedRendererName(component),
      storeExpression: eventStoreExpression ?? 'gea_cpp_value::missing()',
    }
  }
  const replacement = replacementFromReferences(component, refs, storeFields)
  if (replacement) return replacementMatchesEventStore(component, replacement)

  // Multi-store templates (for example an app shell with status, routed
  // screens, and an action bar) still have a single native `mount_<Name>`
  // renderer. That renderer reads each referenced global directly, so the
  // generated class shell does not need a single threaded store parameter.
  return replacementMatchesEventStore(component, {
    className: sanitizeCppIdentifier(component.exportName),
    rendererName: `render_${sanitizeCppIdentifier(component.exportName)}`,
    mountedRendererName: mountedRendererName(component),
    storeExpression: 'gea_cpp_value::missing()',
  })
}

// Exported for cpp-mounted's typed child store-arg resolution (the typed
// parent threads the same single global store the boxed entry would).
export function componentStoreReferences(
  component: GeaIrComponent,
  components: GeaIrComponent[],
  storeFields: StoreFieldPlan[],
  storeArrayFields: StoreArrayFieldPlan[],
  constants: ReadonlyMap<string, GeaIrConstant>,
  storeMethods: StoreMethodPlan[] = [],
  propBindings: Map<string, string> = new Map(),
  seenComponents = new Set<string>(),
): StoreReference[] | null {
  if (seenComponents.has(component.id)) return []
  const nextSeen = new Set(seenComponents)
  nextSeen.add(component.id)
  const refs: StoreReference[] = []
  const eventStoreExpression = eventStoreExpressionForComponent(component)
  if (eventStoreExpression === undefined) return null
  if (eventStoreExpression) refs.push({ storeExpression: eventStoreExpression })
  for (const slot of component.template.slots) {
    if (slot.kind === 'keyed-list') {
      if (isStaticKeyedListSlot(slot)) continue
      if (objectArrayConstant(slot.expr, constants)) continue
      const storeExpression = storeExpressionForSlot(slot)
      const fieldName = slotFieldName(slot)
      if (!storeExpression || !fieldName) return null
      refs.push({ storeExpression, fieldName })
      continue
    }
    if (slot.kind === 'style') {
      for (const field of slot.exprObjectFields ?? []) {
        const fieldRefs = expressionReferences(field.expr, propBindings, constants)
        if (!fieldRefs) return null
        refs.push(...fieldRefs)
      }
      continue
    }
    if (slot.kind === 'class') {
      const classRefs = expressionReferences(slot.expr, propBindings, constants)
      if (!classRefs) return null
      refs.push(...classRefs)
      continue
    }
    if (slot.kind === 'text') {
      const textRefs = expressionReferences(slot.expr, propBindings, constants)
      if (!textRefs) return null
      refs.push(...textRefs)
      continue
    }
    if (slot.kind === 'mount') {
      const tag = mountSlotTag(slot)
      if (tag === 'Image') {
        const imageRefs = imageMountReferences(slot, propBindings, constants)
        if (!imageRefs) return null
        refs.push(...imageRefs)
        continue
      }
      const child = childForMountName(tag, components)
      if (!child) return null
      const childProps = mountPropBindings(slot, propBindings)
      // A ReactiveComponent child consumes none of the threaded store — it
      // mounts a fresh typed instance of its own class. It contributes no
      // references; an unmountable one (or one with props, which the renderer
      // can't inline) makes the whole template unanalyzable, matching the
      // emission gates.
      if (child.reactiveState) {
        if (childProps.size > 0 || !canMountComponent(child, components, storeFields, storeArrayFields, constants, storeMethods)) return null
        continue
      }
      for (const expr of childProps.values()) {
        const propRefs = expressionReferences(expr, propBindings, constants)
        if (!propRefs) return null
        refs.push(...propRefs)
      }
      if (
        !canMountComponent(child, components, storeFields, storeArrayFields, constants, storeMethods) &&
        canMountGenericZeroPropChild(child, childProps)
      ) {
        continue
      }
      const childRefs = componentStoreReferences(child, components, storeFields, storeArrayFields, constants, storeMethods, childProps, nextSeen)
      if (!childRefs) return null
      refs.push(...childRefs)
      continue
    }
    if (slot.kind === 'event') {
      continue
    }
    if (slot.kind === 'attr') {
      continue
    }
  }
  return refs
}

function canMountGenericZeroPropChild(component: GeaIrComponent, propBindings: Map<string, string>): boolean {
  return propBindings.size === 0 && parseTemplateRoot(component.template.html) !== null
}

function imageMountReferences(
  slot: GeaIrSlot,
  propBindings: Map<string, string>,
  constants: ReadonlyMap<string, GeaIrConstant>,
): StoreReference[] | null {
  const refs: StoreReference[] = []
  for (const attr of mountSlotAttrs(slot)) {
    const parsed = parseMountAttr(attr)
    if (!parsed) continue
    if (parsed.name === 'src' && isBareIdentifier(parsed.expr)) continue
    const attrRefs = expressionReferences(parsed.expr, propBindings, constants)
    if (!attrRefs) return null
    refs.push(...attrRefs)
  }
  return refs
}

export function isStaticKeyedListSlot(slot: GeaIrSlot): boolean {
  const values = staticStringArrayValues(slot.expr)
  if (!values) return false
  const payload = slot.payload
  if (!payload || typeof payload !== 'object') return false
  const record = payload as { itemParam?: unknown; rowTemplate?: unknown }
  const itemParam = typeof record.itemParam === 'string' ? record.itemParam : null
  const rowTemplate =
    record.rowTemplate && typeof record.rowTemplate === 'object'
      ? (record.rowTemplate as { slots?: unknown })
      : null
  if (!itemParam || !rowTemplate || !Array.isArray(rowTemplate.slots)) return false
  return rowTemplate.slots.every((rowSlot) => isStaticKeyedListRowSlot(rowSlot as GeaIrSlot, itemParam))
}

function isStaticKeyedListRowSlot(slot: GeaIrSlot, itemParam: string): boolean {
  if (slot.walk.length !== 0) return false
  if (slot.kind !== 'class' && slot.kind !== 'text') return false
  if (slot.exprPath && slot.exprPath.length === 1 && slot.exprPath[0] === itemParam) return true
  return slot.expr?.trim() === itemParam
}

function staticStringArrayValues(expr: string | undefined): string[] | null {
  if (!expr) return null
  const text = expr.trim()
  if (!text.startsWith('[') || !text.endsWith(']')) return null
  const inner = text.slice(1, -1).trim()
  if (!inner) return []
  const values: string[] = []
  for (const part of splitTopLevelComma(inner)) {
    if (!isStringLiteral(part)) return null
    values.push(part)
  }
  return values
}

function replacementMatchesEventStore(component: GeaIrComponent, replacement: RendererReplacement | null): RendererReplacement | null {
  if (!replacement) return null
  const eventStoreExpression = eventStoreExpressionForComponent(component)
  // `undefined` means the receiver couldn't be parsed (anonymous handler,
  // unsupported shape) — we still bail in that case because the event
  // lowering has nothing to bind to.
  if (eventStoreExpression === undefined) return null
  return replacement
}

function replacementFromReferences(
  component: GeaIrComponent,
  refs: StoreReference[],
  storeFields: StoreFieldPlan[],
): RendererReplacement | null {
  const storeExpressions = new Set(refs.map((ref) => sanitizeCppIdentifier(ref.storeExpression)))
  if (storeExpressions.size !== 1) return null
  const storeExpression = [...storeExpressions][0]

  const fieldRefs = refs.filter((ref): ref is StoreReference & { fieldName: string } => !!ref.fieldName)
  if (fieldRefs.length === 0) {
    return {
      className: sanitizeCppIdentifier(component.exportName),
      rendererName: `render_${sanitizeCppIdentifier(component.exportName)}`,
      mountedRendererName: mountedRendererName(component),
      storeExpression,
    }
  }

  const stores = fieldRefs.map((ref) => storeFieldByName(ref.fieldName, storeFields, ref.storeExpression))
  if (stores.some((store) => store === null)) return null

  const storeClasses = new Set(stores.map((store) => store!.storeClass))
  if (storeClasses.size !== 1) return null

  const store = stores[0]!
  return {
    className: sanitizeCppIdentifier(component.exportName),
    rendererName: `render_${sanitizeCppIdentifier(component.exportName)}`,
    mountedRendererName: mountedRendererName(component),
    stateReaderName: `read_${store.stateType}`,
    storeExpression,
  }
}

interface StoreReference {
  storeExpression: string
  fieldName?: string
}

function storeFieldByName(fieldName: string, storeFields: StoreFieldPlan[], receiver?: string): StoreFieldPlan | null {
  const matches = storeFields.filter((candidate) => candidate.fieldName === sanitizeCppIdentifier(fieldName))
  if (matches.length === 1) return matches[0]
  // Disambiguate colliding field names across bundled stores by the receiver
  // (e.g. `store.status` vs the SettingsStore's `status`). See the matching
  // logic in cpp-template-renderer.ts's storeFieldByName.
  if (matches.length > 1 && receiver) {
    const recv = sanitizeCppIdentifier(receiver)
    const byReceiver = matches.filter((candidate) => candidate.storeGlobalName === recv)
    if (byReceiver.length === 1) return byReceiver[0]
  }
  return null
}

function textSlotReferences(slot: GeaIrSlot, constants: ReadonlyMap<string, GeaIrConstant>): StoreReference[] | null {
  if (slot.exprPath && slot.exprPath.length === 2) return [{ storeExpression: slot.exprPath[0], fieldName: slot.exprPath[1] }]
  const refs: StoreReference[] = []
  for (const part of splitTopLevelPlus(slot.expr ?? '')) {
    if (isStringLiteral(part)) continue
    if (isKnownConstantExpression(part, constants)) continue
    const match = part.match(/^([A-Za-z_$][A-Za-z0-9_$]*)\.([A-Za-z_$][A-Za-z0-9_$]*)$/)
    if (!match) return null
    refs.push({ storeExpression: match[1], fieldName: match[2] })
  }
  return refs
}

function mountSlotTag(slot: GeaIrSlot): string | null {
  const payload = slot.payload
  if (!payload || typeof payload !== 'object' || !('tag' in payload)) return null
  const tag = (payload as { tag?: unknown }).tag
  return typeof tag === 'string' ? tag : null
}

function childForMountName(tag: string | null, components: GeaIrComponent[]): GeaIrComponent | null {
  return tag ? (components.find((component) => component.exportName === tag) ?? null) : null
}

function mountSlotAttrs(slot: GeaIrSlot): string[] {
  const payload = slot.payload
  if (!payload || typeof payload !== 'object' || !('attrs' in payload)) return []
  const attrs = (payload as { attrs?: unknown }).attrs
  if (!Array.isArray(attrs)) return []
  return attrs
    .map((attr) => {
      if (!attr || typeof attr !== 'object' || !('code' in attr)) return null
      const code = (attr as { code?: unknown }).code
      return typeof code === 'string' ? code : null
    })
    .filter((code): code is string => !!code)
}

function mountPropBindings(slot: GeaIrSlot, parentProps: Map<string, string>): Map<string, string> {
  const out = new Map<string, string>()
  for (const attr of mountSlotAttrs(slot)) {
    const parsed = parseMountAttr(attr)
    if (!parsed) continue
    out.set(parsed.name, resolvePropExpression(parsed.expr, parentProps) ?? parsed.expr)
  }
  return out
}

function parseMountAttr(code: string): { name: string; expr: string } | null {
  const trimmed = code.trim()
  const stringAttr = trimmed.match(/^([A-Za-z_$][A-Za-z0-9_$]*)=(["'])(.*)\2$/s)
  if (stringAttr) return { name: stringAttr[1], expr: `${stringAttr[2]}${stringAttr[3]}${stringAttr[2]}` }
  const expressionAttr = trimmed.match(/^([A-Za-z_$][A-Za-z0-9_$]*)=\{([\s\S]*)\}$/)
  if (expressionAttr) return { name: expressionAttr[1], expr: expressionAttr[2].trim() }
  const bareAttr = trimmed.match(/^([A-Za-z_$][A-Za-z0-9_$]*)$/)
  return bareAttr ? { name: bareAttr[1], expr: 'true' } : null
}

function expressionReferences(
  expr: string | undefined,
  propBindings: Map<string, string>,
  constants: ReadonlyMap<string, GeaIrConstant>,
): StoreReference[] | null {
  if (!expr) return []
  const resolved = resolvePropExpression(expr, propBindings) ?? expr
  if (/\bthis\.props\./.test(resolved)) return null
  const refs: StoreReference[] = []
  const dependencySource = resolved.replace(
    /\b([A-Za-z_$][A-Za-z0-9_$]*\.[A-Za-z_$][A-Za-z0-9_$]*)\.length\b/g,
    '$1',
  )
  const pattern = /\b([A-Za-z_$][A-Za-z0-9_$]*)\.([A-Za-z_$][A-Za-z0-9_$]*)\b/g
  let match: RegExpExecArray | null
  while ((match = pattern.exec(dependencySource)) !== null) {
    if (match[1] === 'this') return null
    // `props.X` references contribute no store dependency at top-level
    // analysis. Wrapper components like `<View>` reference `props.class`,
    // `props.style`, etc. — when nothing is bound (top-level mount), those
    // slots are no-ops (see `slotReferencesUnboundProp` in the renderer).
    // When the wrapper is inlined, the parent passes literal/store refs and
    // those flow through the slot's own analysis path.
    if (match[1] === 'props') continue
    if (match[1] === 'Math') continue
    refs.push({ storeExpression: match[1], fieldName: match[2] })
  }
  if (refs.length === 0 && /^[A-Za-z_$][A-Za-z0-9_$]*$/.test(resolved.trim()) && !isKnownConstantExpression(resolved, constants)) {
    // A bare identifier that isn't a known compile-time constant is normally a
    // bail signal (could be an unresolvable local). But SCREAMING_SNAKE_CASE
    // module constants computed at runtime (e.g. `ITEM_HEIGHT = 126 * CSS_PX`)
    // never make it into the IR constant map, yet they carry no store
    // dependency. Treat them as zero-reference so the component still qualifies
    // for the optimized template mount path.
    if (isRuntimeNumericConstantIdentifier(resolved.trim())) return refs
    return null
  }
  return refs
}

function isKnownConstantExpression(expr: string, constants: ReadonlyMap<string, GeaIrConstant>): boolean {
  return constants.has(expr.trim())
}

function isBareIdentifier(expr: string): boolean {
  return /^[A-Za-z_$][A-Za-z0-9_$]*$/.test(expr.trim())
}

function resolvePropExpression(expr: string, propBindings: Map<string, string>): string | null {
  if (propBindings.size === 0) return null
  let out = expr
  let changed = false
  for (const [name, value] of propBindings) {
    const escaped = escapeRegExp(name)
    const pattern = new RegExp(`\\bthis\\.props\\.${escaped}\\b|\\bprops\\.${escaped}\\b|(?<!\\.)\\b${escaped}\\b`, 'g')
    out = out.replace(pattern, () => {
      changed = true
      return `(${value})`
    })
  }
  return changed ? out : null
}

function storeExpressionForSlot(slot: GeaIrSlot): string | null {
  if (slot.exprPath && slot.exprPath.length > 1) return slot.exprPath[0]
  return firstObjectIdentifier(slot.expr)
}

export function slotFieldName(slot: GeaIrSlot): string | null {
  if (slot.exprPath && slot.exprPath.length > 1) return sanitizeCppIdentifier(slot.exprPath[slot.exprPath.length - 1])
  const match = slot.expr?.match(/\.([A-Za-z_$][A-Za-z0-9_$]*)$/)
  return match ? sanitizeCppIdentifier(match[1]) : null
}

function firstObjectIdentifier(expr: string | undefined): string | null {
  const match = expr?.match(/^([A-Za-z_$][A-Za-z0-9_$]*)\./)
  return match ? match[1] : null
}

function eventStoreExpressionForComponent(component: GeaIrComponent): string | null | undefined {
  const receivers = component.template.slots
    .filter((slot) => slot.kind === 'event')
    .map((slot) => eventReceiver(slot.expr))
  if (receivers.some((receiver) => receiver === undefined)) return undefined
  const uniqueReceivers = [...new Set(receivers.filter((receiver): receiver is string => typeof receiver === 'string'))]
  if (uniqueReceivers.length === 0) return null
  return uniqueReceivers.length === 1 ? sanitizeCppIdentifier(uniqueReceivers[0]) : undefined
}

function eventReceiver(expr: string | undefined): string | null | undefined {
  if (!expr) return undefined
  // Prop-forwarding events (`onClick={props.onClick}`) — common in the
  // gea-embedded wrappers — don't constrain the event store. The parent
  // decides what handler runs; we return null (no contribution) instead of
  // undefined (failure) so `componentStoreReferences` doesn't bail when
  // analyzing a wrapper component that's only meant to be inlined.
  const trimmed = expr.trim()
  if (/^props\.[A-Za-z_$][A-Za-z0-9_$]*$/.test(trimmed)) return null
  const arrow = expr.indexOf('=>')
  if (arrow < 0) return undefined
  const rawParams = expr.slice(0, arrow).trim()
  const params = rawParams.startsWith('(')
    ? rawParams.slice(1, -1).split(',').map((part) => part.trim()).filter(Boolean)
    : rawParams.length === 0
      ? []
      : [rawParams]
  let body = expr.slice(arrow + 2).trim()
  let statements: string[]
  if (body.startsWith('{') && body.endsWith('}')) {
    const inner = body.slice(1, -1).trim()
    const returnMatch = inner.match(/^return\s+(.+?);?$/s)
    statements = returnMatch
      ? [returnMatch[1].trim()]
      : splitTopLevelSemicolon(inner).map((statement) => statement.trim()).filter(Boolean)
  } else {
    statements = [body.replace(/;$/, '').trim()].filter(Boolean)
  }
  const receivers: string[] = []
  for (const statement of statements) {
    const guarded = statement.match(/^if\s*\(([\s\S]+?)\)\s*([\s\S]+)$/)
    const eventStatement = guarded ? guarded[2].trim().replace(/;$/, '') : statement
    if (/^[A-Za-z_$][A-Za-z0-9_$]*\(/.test(eventStatement)) continue
    const call = eventStatement.match(/^([A-Za-z_$][A-Za-z0-9_$]*)\.([A-Za-z_$][A-Za-z0-9_$]*)\((.*)\)$/s)
    if (!call) return undefined
    const [, receiver, method, rawArgs] = call
    if (params.includes(receiver) && (method === 'preventDefault' || method === 'stopPropagation')) {
      if (splitTopLevelComma(rawArgs).filter((arg) => arg !== '').length > 0) return undefined
      continue
    }
    if (params.includes(receiver)) return undefined
    receivers.push(receiver)
  }
  const uniqueReceivers = [...new Set(receivers)]
  if (uniqueReceivers.length === 0) return null
  return uniqueReceivers.length === 1 ? uniqueReceivers[0] : undefined
}

function splitTopLevelComma(expr: string): string[] {
  return splitTopLevelDelimiter(expr, ',')
}

function splitTopLevelSemicolon(expr: string): string[] {
  return splitTopLevelDelimiter(expr, ';')
}

function splitTopLevelDelimiter(expr: string, delimiter: string): string[] {
  const parts: string[] = []
  let start = 0
  let quote: string | null = null
  let escaped = false
  let depth = 0
  for (let index = 0; index < expr.length; index += 1) {
    const char = expr[index]
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
    if (char === '(' || char === '[' || char === '{') depth += 1
    else if (char === ')' || char === ']' || char === '}') depth -= 1
    else if (char === delimiter && depth === 0) {
      parts.push(expr.slice(start, index).trim())
      start = index + 1
    }
  }
  parts.push(expr.slice(start).trim())
  return parts
}

function splitTopLevelPlus(expr: string): string[] {
  const parts: string[] = []
  let start = 0
  let quote: string | null = null
  let escaped = false
  for (let index = 0; index < expr.length; index += 1) {
    const char = expr[index]
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
    if (char !== '+') continue
    parts.push(expr.slice(start, index).trim())
    start = index + 1
  }
  parts.push(expr.slice(start).trim())
  return parts.filter(Boolean)
}

function isStringLiteral(expr: string): boolean {
  const quote = expr[0]
  return (quote === '"' || quote === "'") && expr[expr.length - 1] === quote
}

function escapeRegExp(value: string): string {
  return value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
}
