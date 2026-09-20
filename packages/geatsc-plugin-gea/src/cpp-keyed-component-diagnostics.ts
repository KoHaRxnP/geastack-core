import {
  collectSingletonMethodsFromSourceFiles,
  collectStoreArraySources,
  collectStoreFields,
  collectStoreMethods,
} from './cpp-stores.js'
import { collectIrConstants } from './cpp-ir.js'
import { mountedRendererForComponent } from './cpp-mounted.js'
import { fieldNameForSlot, keyedListPayload, mountSlotAttrs, mountSlotTag, parseMountAttr } from './cpp-mounted-lowering.js'
import type { GeaIrBundleV1, GeaIrSlot, PluginDiagnostic } from './types.js'
import { sanitizeCppIdentifier } from './utils.js'

/**
 * A typed keyed-list item must never cross a child-component boundary through
 * `gea_cpp_value`. If the native renderer cannot inline/lower the row, stop the
 * embedded build here instead of letting geatsc's generic component lifecycle
 * instantiate the child, box its props, and emit invalid render/dispose calls.
 */
export function diagnoseBoxedKeyedComponentProps(ir: GeaIrBundleV1): PluginDiagnostic[] {
  const storeFields = collectStoreFields(ir.stores)
  const storeArrayFields = collectStoreArraySources(ir.stores)
  const storeMethods = collectStoreMethods(ir.stores).concat(
    collectSingletonMethodsFromSourceFiles(ir.modules.map((module) => module.file), ir.stores),
  )
  const constants = collectIrConstants(ir)
  const diagnostics: PluginDiagnostic[] = []

  for (const component of ir.components) {
    const typedRows = component.template.slots.flatMap((slot) => typedComponentRows(slot, storeArrayFields))
    if (typedRows.length === 0) continue
    if (mountedRendererForComponent(component, ir.components, storeFields, storeArrayFields, constants, storeMethods)) continue

    for (const row of typedRows) {
      diagnostics.push({
        kind: 'unsupported',
        code: 'gea-boxed-keyed-component-props',
        file: component.module,
        severity: 'error',
        message:
          `<${component.exportName}> maps the typed list \`${row.fieldName}\` into <${row.childTag} ${row.propName}={${row.itemName}} />, ` +
          `but the parent template has no complete native renderer. The embedded compiler refuses to box the typed ` +
          `\`${row.itemType}\` item into \`gea_cpp_value\` or enter the generic component render/dispose lifecycle. ` +
          `Lower the unsupported parent slot or extend the native template renderer for that expression.`,
      })
    }
  }
  return diagnostics
}

interface TypedComponentRow {
  fieldName: string
  itemName: string
  itemType: string
  childTag: string
  propName: string
}

function typedComponentRows(
  slot: GeaIrSlot,
  fields: ReturnType<typeof collectStoreArraySources>,
): TypedComponentRow[] {
  if (slot.kind !== 'keyed-list') return []
  const payload = keyedListPayload(slot)
  if (!payload?.rowTemplate || !payload.itemParam) return []
  const fieldName = fieldNameForSlot(slot)
  if (!fieldName) return []
  const receiver = slot.exprPath && slot.exprPath.length > 1 ? sanitizeCppIdentifier(slot.exprPath[0]) : null
  const candidates = fields.filter(
    (field) => field.fieldName === fieldName && (!receiver || !field.storeGlobalName || field.storeGlobalName === receiver),
  )
  if (candidates.length !== 1) return []

  const itemName = sanitizeCppIdentifier(payload.itemParam)
  const rows: TypedComponentRow[] = []
  for (const rowSlot of payload.rowTemplate.slots) {
    if (rowSlot.kind !== 'mount') continue
    const childTag = mountSlotTag(rowSlot)
    if (!childTag || childTag === 'Image') continue
    for (const attr of mountSlotAttrs(rowSlot)) {
      const parsed = parseMountAttr(attr)
      if (!parsed || sanitizeCppIdentifier(parsed.expr) !== itemName) continue
      rows.push({
        fieldName,
        itemName,
        itemType: candidates[0].itemTypeRef,
        childTag,
        propName: parsed.name,
      })
    }
  }
  return rows
}
