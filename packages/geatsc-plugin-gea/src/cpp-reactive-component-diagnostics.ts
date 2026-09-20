// A ReactiveComponent whose template falls off the typed fast path has NO
// renderer at all: the direct-mount planner finds no replacement, the boxed
// fallback mounts a class whose `__gea_to_value()` is neutralized (so its
// `render` lookup misses), and the app draws NOTHING — the frame loop runs at
// full rate over a black screen with zero log output. That silent-empty mode
// cost a real on-device debugging cycle; fail the build loudly instead.

import { collectStoreArraySources, collectStoreFields, collectStoreMethods } from './cpp-stores.js'
import type { StoreArrayFieldPlan, StoreFieldPlan, StoreMethodPlan } from './cpp-stores.js'
import { collectIrConstants } from './cpp-ir.js'
import {
  childSubtreeGlobalStoreName,
  mountedRendererForComponent,
  rendererReadsStoreParam,
  selfStoreMountedRenderer,
} from './cpp-mounted.js'
import { childForMount, mountSlotAttrs, mountSlotTag, type ConstantMap } from './cpp-mounted-lowering.js'
import type { GeaIrBundleV1, GeaIrComponent, PluginDiagnostic } from './types.js'

export function diagnoseUnmountableReactiveComponents(ir: GeaIrBundleV1): PluginDiagnostic[] {
  const reactive = ir.components.filter((component) => component.reactiveState)
  if (reactive.length === 0) return []
  const storeFields = collectStoreFields(ir.stores)
  const storeArrayFields = collectStoreArraySources(ir.stores)
  const storeMethods = collectStoreMethods(ir.stores)
  const constants: ConstantMap = new Map(collectIrConstants(ir))
  const diagnostics: PluginDiagnostic[] = []
  for (const component of reactive) {
    if (selfStoreMountedRenderer(component, ir.components, storeFields, storeArrayFields, constants, storeMethods)) continue
    const blockers = mountBlockers(component, ir.components, storeFields, storeArrayFields, constants, storeMethods)
    const detail =
      blockers.length > 0
        ? blockers.join('; ')
        : 'the template uses a construct the typed renderer does not support yet (e.g. reactive array/record fields in slots, or conditionals over self fields)'
    diagnostics.push({
      kind: 'unsupported',
      code: 'gea-unmountable-reactive-component',
      message:
        `ReactiveComponent <${component.exportName}> has no typed mounted renderer: ${detail}. ` +
        `Without a renderer the component renders NOTHING at runtime (black screen, no diagnostics) — ` +
        `inline the blocking child markup or drop back to \`Component\` until the construct is supported.`,
      file: component.module,
      severity: 'error',
    })
  }
  return diagnostics
}

// Best-effort explanation of WHY the typed renderer bailed: re-run the child
// gate from selfStoreMountedRenderer per mount slot and name each blocker.
// Other bail causes (unsupported slot kinds, unresolved readers) fall through
// to the generic message above.
function mountBlockers(
  component: GeaIrComponent,
  components: GeaIrComponent[],
  storeFields: StoreFieldPlan[],
  storeArrayFields: StoreArrayFieldPlan[],
  constants: ConstantMap,
  storeMethods: StoreMethodPlan[],
): string[] {
  const blockers: string[] = []
  for (const slot of component.template.slots) {
    if (slot.kind !== 'mount') continue
    const tag = mountSlotTag(slot)
    if (tag === 'Image') continue
    const child = childForMount(slot, components)
    if (!child) {
      blockers.push(`child <${tag ?? '?'} /> is not in the compiled IR (its module may have been tree-shaken)`)
      continue
    }
    if (child.reactiveState) {
      if (mountSlotAttrs(slot).length > 0) {
        blockers.push(
          `child <${tag} /> is a ReactiveComponent mounted with props — props mean inlining, which a reactive child can't do (lift the props into its own state or a shared store)`,
        )
      } else if (!selfStoreMountedRenderer(child, components, storeFields, storeArrayFields, constants, storeMethods)) {
        blockers.push(
          `child <${tag} /> is a ReactiveComponent whose own template has no typed renderer (see its own diagnostic; a reactive mount cycle also lands here)`,
        )
      }
      continue
    }
    const renderer = mountedRendererForComponent(child, components, storeFields, storeArrayFields, constants, storeMethods)
    if (!renderer) {
      blockers.push(`child <${tag} /> has no compilable mounted renderer`)
      continue
    }
    if (
      rendererReadsStoreParam(renderer) &&
      !childSubtreeGlobalStoreName(child, components, storeFields, storeArrayFields, constants)
    ) {
      blockers.push(
        `child <${tag} />'s renderer reads its boxed store parameter, and its template doesn't resolve to exactly one bundled global store (subtrees spanning multiple stores or reading an outer \`this\` can't be threaded from a typed parent)`,
      )
    }
  }
  return blockers
}
