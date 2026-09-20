import type { GeaIrBundleV1, GeaIrComponent, GeaIrSlot, GeaIrStore, PluginDiagnostic } from './types.js'
import { collectStoreArraySources } from './cpp-stores.js'
import { collectIrConstants } from './cpp-ir.js'
import { objectArrayConstant } from './cpp-mounted-lowering.js'
import { isStaticKeyedListSlot, slotFieldName } from './cpp-replacements.js'

/**
 * Detect `{x.map(...)}` lists whose source is not a reactive array-of-objects
 * store field. On the embedded target such a list silently renders nothing (the
 * keyed-list mount bails to a placeholder anchor — see `keyedListMountedRenderer`
 * in cpp-mounted.ts), which is a hard-to-diagnose trap. We surface it as a
 * warning so the build tells the author exactly why the list is empty.
 *
 * The predicate mirrors the emitter exactly: a keyed-list renders reactively
 * only when it is a compile-time static string-literal list
 * (`isStaticKeyedListSlot`) or its name resolves to an array source collected by
 * `collectStoreArraySources` — an array-of-objects field OR an array-returning
 * getter. Anything else — a local/derived array, a primitive array, a getter
 * whose element type can't be resolved, or a typo'd field — produces no row
 * renderer and is flagged.
 */
export function diagnoseNonReactiveLists(ir: GeaIrBundleV1): PluginDiagnostic[] {
  const diagnostics: PluginDiagnostic[] = []
  // Sources include array-of-objects fields AND array-returning getters, so a
  // getter-backed list does not get flagged as non-reactive.
  const reactiveFieldNames = new Set(collectStoreArraySources(ir.stores).map((field) => field.fieldName))
  const constants = collectIrConstants(ir)
  for (const component of ir.components) {
    for (const slot of component.template.slots) {
      if (slot.kind !== 'keyed-list') continue
      if (isStaticKeyedListSlot(slot)) continue
      if (objectArrayConstant(slot.expr, constants)) continue
      const fieldName = slotFieldName(slot)
      if (fieldName && reactiveFieldNames.has(fieldName)) continue
      if (fieldName && isNamedTypedArrayField(fieldName, ir.stores)) continue
      diagnostics.push(buildDiagnostic(component, slot, fieldName, ir.stores))
    }
  }
  return diagnostics
}

function buildDiagnostic(
  component: GeaIrComponent,
  slot: GeaIrSlot,
  fieldName: string | null,
  stores: GeaIrStore[],
): PluginDiagnostic {
  const source = slot.expr ?? fieldName ?? '<list>'
  const reason = describeReason(fieldName, stores)
  const message =
    `<${component.exportName}> renders the list \`{${source}.map(...)}\`, but ${reason}. ` +
    `On the embedded target a list only renders when its source is a typed array-of-objects ` +
    `field on a Store (e.g. \`${fieldName ?? 'items'}: Tile[]\` where \`Tile\` is an object), mutated/reassigned ` +
    `through the store. Getters, local/derived arrays, and primitive arrays are not reactive ` +
    `lists, so this list renders nothing.`
  return {
    kind: 'unsupported',
    code: 'gea-nonreactive-list',
    message,
    file: component.module,
    severity: 'warning',
  }
}

function describeReason(fieldName: string | null, stores: GeaIrStore[]): string {
  if (!fieldName) return 'its source is not a reactive store field'
  for (const store of stores) {
    const field = store.fields.find((candidate) => candidate.name === fieldName)
    if (field) {
      const shape = field.shape
      if (shape?.kind === 'array') return `\`${fieldName}\` is an array of primitives, not of objects`
      return `\`${fieldName}\` is not an array field`
    }
    if (store.methods?.some((method) => method.name === fieldName)) {
      return `\`${fieldName}\` is a method (getters/methods never reach the embedded IR)`
    }
  }
  return `\`${fieldName}\` is not a reactive store field (it looks like a getter or local array)`
}

function isNamedTypedArrayField(fieldName: string, stores: GeaIrStore[]): boolean {
  for (const store of stores) {
    const field = store.fields.find((candidate) => candidate.name === fieldName)
    if (field?.shape?.kind === 'array' && !!field.shape.elementTypeName) return true
  }
  return false
}
