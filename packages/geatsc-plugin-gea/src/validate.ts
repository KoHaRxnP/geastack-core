import fs from 'node:fs'
import type {
  GeaIrBundleV1,
  GeaIrComponent,
  GeaIrModule,
  GeaIrSlot,
  GeaIrSourceSpan,
  GeaIrStore,
  GeaIrStoreValueShape,
  PluginDiagnostic,
} from './types.js'
import { isRecord } from './utils.js'

const supportedSlotKinds = new Set([
  'text',
  'attr',
  'bool',
  'class',
  'style',
  'value',
  'event',
  'ref',
  'mount',
  'direct-fn',
  'conditional',
  'keyed-list',
  'html',
])

const supportedRuntimeBases = new Set(['static', 'static-element', 'compiled', 'tiny-reactive', 'lean-reactive', 'reactive'])

export function validateIr(ir: GeaIrBundleV1, file: string): PluginDiagnostic[] {
  const diagnostics: PluginDiagnostic[] = []
  if (typeof ir.entry !== 'string') diagnostics.push(baseDiagnostic(file, 'entry must be a string'))
  for (const module of ir.modules) validateModule(module, file, diagnostics)
  for (const component of ir.components) validateComponent(component, file, diagnostics)
  for (const store of ir.stores) validateStore(store, file, diagnostics)
  for (const capability of ir.hostCapabilities) {
    if (typeof capability !== 'string') diagnostics.push(baseDiagnostic(file, 'hostCapabilities entries must be strings'))
  }
  return diagnostics
}

function validateModule(module: GeaIrModule, file: string, diagnostics: PluginDiagnostic[]): void {
  if (!isRecord(module)) {
    diagnostics.push(baseDiagnostic(file, 'module entries must be objects'))
    return
  }
  if (typeof module.id !== 'string') diagnostics.push(baseDiagnostic(file, 'module.id must be a string'))
  if (typeof module.file !== 'string') diagnostics.push(baseDiagnostic(file, 'module.file must be a string'))
  if (!Array.isArray(module.components)) diagnostics.push(baseDiagnostic(file, 'module.components must be an array'))
  if (!Array.isArray(module.stores)) diagnostics.push(baseDiagnostic(file, 'module.stores must be an array'))
}

function validateComponent(component: GeaIrComponent, file: string, diagnostics: PluginDiagnostic[]): void {
  if (!isRecord(component)) {
    diagnostics.push(baseDiagnostic(file, 'component entries must be objects'))
    return
  }
  const componentFile = typeof component.module === 'string' ? component.module : file
  if (typeof component.id !== 'string') diagnostics.push(componentDiagnostic(componentFile, component, 'component.id must be a string'))
  if (typeof component.module !== 'string') diagnostics.push(componentDiagnostic(componentFile, component, 'component.module must be a string'))
  if (typeof component.exportName !== 'string') {
    diagnostics.push(componentDiagnostic(componentFile, component, 'component.exportName must be a string'))
  }
  if (typeof component.runtimeBase !== 'string' || !supportedRuntimeBases.has(component.runtimeBase)) {
    diagnostics.push(componentDiagnostic(componentFile, component, `unsupported component runtime base: ${String(component.runtimeBase)}`))
  }
  if (!isRecord(component.template)) {
    diagnostics.push(componentDiagnostic(componentFile, component, 'component.template must be an object'))
    return
  }
  if (typeof component.template.html !== 'string') diagnostics.push(componentDiagnostic(componentFile, component, 'template.html must be a string'))
  if (!Array.isArray(component.template.slots)) {
    diagnostics.push(componentDiagnostic(componentFile, component, 'template.slots must be an array'))
    return
  }
  for (const slot of component.template.slots) validateSlot(slot, componentFile, component, diagnostics)
}

function validateSlot(slot: GeaIrSlot, file: string, component: GeaIrComponent, diagnostics: PluginDiagnostic[]): void {
  if (!isRecord(slot)) {
    diagnostics.push(componentDiagnostic(file, component, 'slot entries must be objects'))
    return
  }
  if (typeof slot.index !== 'number') diagnostics.push(componentDiagnostic(file, component, 'slot.index must be a number'))
  if (typeof slot.kind !== 'string' || !supportedSlotKinds.has(slot.kind)) {
    diagnostics.push(componentDiagnostic(file, component, `unsupported slot kind: ${String(slot.kind)}`))
  }
  if (!Array.isArray(slot.walk) || !slot.walk.every((part) => typeof part === 'number')) {
    diagnostics.push(componentDiagnostic(file, component, 'slot.walk must be a number array'))
  }
  if (slot.exprPath !== undefined && (!Array.isArray(slot.exprPath) || !slot.exprPath.every((part) => typeof part === 'string'))) {
    diagnostics.push(componentDiagnostic(file, component, 'slot.exprPath must be a string array'))
  }
  if (slot.exprObjectFields !== undefined) validateExpressionObjectFields(slot.exprObjectFields, file, component, diagnostics)
  if (slot.kind === 'keyed-list' && isRecord(slot.payload) && slot.payload.rowTemplate !== undefined) {
    validateKeyedListRowTemplate(slot.payload.rowTemplate, file, component, diagnostics)
  }
}

function validateStore(store: GeaIrStore, file: string, diagnostics: PluginDiagnostic[]): void {
  if (!isRecord(store)) {
    diagnostics.push(baseDiagnostic(file, 'store entries must be objects'))
    return
  }
  const storeFile = typeof store.module === 'string' ? store.module : file
  if (typeof store.id !== 'string') diagnostics.push(storeDiagnostic(storeFile, store, 'store.id must be a string'))
  if (typeof store.module !== 'string') diagnostics.push(storeDiagnostic(storeFile, store, 'store.module must be a string'))
  if (typeof store.className !== 'string') diagnostics.push(storeDiagnostic(storeFile, store, 'store.className must be a string'))
  if (store.runtimeBase !== 'compiled' && store.runtimeBase !== 'lean') {
    diagnostics.push(storeDiagnostic(storeFile, store, `unsupported store runtime base: ${String(store.runtimeBase)}`))
  }
  if (!Array.isArray(store.fields)) {
    diagnostics.push(storeDiagnostic(storeFile, store, 'store.fields must be an array'))
    return
  }
  const names = new Set<string>()
  for (const field of store.fields) {
    if (!isRecord(field) || typeof field.name !== 'string') {
      diagnostics.push(storeDiagnostic(storeFile, store, 'store fields must have string names'))
      continue
    }
    if (names.has(field.name)) diagnostics.push(storeDiagnostic(storeFile, store, `duplicate store field: ${field.name}`))
    names.add(field.name)
    if (field.shape) validateStoreShape(field.shape, storeFile, store, diagnostics)
  }
}

function validateStoreShape(shape: GeaIrStoreValueShape, file: string, store: GeaIrStore, diagnostics: PluginDiagnostic[]): void {
  if (!isRecord(shape) || typeof shape.kind !== 'string') {
    diagnostics.push(storeDiagnostic(file, store, 'store field shape must be an object with a kind'))
    return
  }
  const shapeKind = shape.kind
  if (shapeKind === 'array') {
    if (shape.element) validateStoreShape(shape.element, file, store, diagnostics)
    return
  }
  if (shapeKind === 'object') {
    if (!Array.isArray(shape.fields)) {
      diagnostics.push(storeDiagnostic(file, store, 'object store shape fields must be an array'))
      return
    }
    for (const field of shape.fields) {
      if (!isRecord(field) || typeof field.name !== 'string') {
        diagnostics.push(storeDiagnostic(file, store, 'object store shape fields must have string names'))
        continue
      }
      if (field.shape) validateStoreShape(field.shape, file, store, diagnostics)
    }
    return
  }
  if (shapeKind === 'literal') {
    if (!['string', 'number', 'boolean', 'null'].includes(String(shape.valueType))) {
      diagnostics.push(storeDiagnostic(file, store, `unsupported literal store shape: ${String(shape.valueType)}`))
    }
    return
  }
  diagnostics.push(storeDiagnostic(file, store, `unsupported store shape kind: ${shapeKind}`))
}

function validateExpressionObjectFields(
  fields: unknown,
  file: string,
  component: GeaIrComponent,
  diagnostics: PluginDiagnostic[],
): void {
  if (!Array.isArray(fields)) {
    diagnostics.push(componentDiagnostic(file, component, 'slot.exprObjectFields must be an array'))
    return
  }
  for (const field of fields) {
    if (!isRecord(field) || typeof field.name !== 'string' || typeof field.expr !== 'string') {
      diagnostics.push(componentDiagnostic(file, component, 'slot.exprObjectFields entries must have string name and expr'))
      continue
    }
    if (field.exprPath !== undefined && (!Array.isArray(field.exprPath) || !field.exprPath.every((part) => typeof part === 'string'))) {
      diagnostics.push(componentDiagnostic(file, component, 'slot.exprObjectFields.exprPath must be a string array'))
    }
  }
}

function validateKeyedListRowTemplate(
  rowTemplate: unknown,
  file: string,
  component: GeaIrComponent,
  diagnostics: PluginDiagnostic[],
): void {
  if (!isRecord(rowTemplate)) {
    diagnostics.push(componentDiagnostic(file, component, 'keyed-list payload.rowTemplate must be an object'))
    return
  }
  if (typeof rowTemplate.html !== 'string') diagnostics.push(componentDiagnostic(file, component, 'keyed-list rowTemplate.html must be a string'))
  if (!Array.isArray(rowTemplate.slots)) {
    diagnostics.push(componentDiagnostic(file, component, 'keyed-list rowTemplate.slots must be an array'))
    return
  }
  for (const rowSlot of rowTemplate.slots) validateSlot(rowSlot as GeaIrSlot, file, component, diagnostics)
}

function baseDiagnostic(file: string, message: string): PluginDiagnostic {
  return { kind: 'unsupported', code: 'gea-ir', file, message }
}

function componentDiagnostic(file: string, component: GeaIrComponent, message: string): PluginDiagnostic {
  return sourceDiagnostic(file, component.sourceSpan, message)
}

function storeDiagnostic(file: string, store: GeaIrStore, message: string): PluginDiagnostic {
  return sourceDiagnostic(file, store.sourceSpan, message)
}

export function sourceDiagnostic(file: string, span: GeaIrSourceSpan | undefined, message: string): PluginDiagnostic {
  const diagnostic: PluginDiagnostic = { kind: 'unsupported', code: 'gea-ir', file, message }
  if (span?.start === undefined || !fs.existsSync(file)) return diagnostic
  const location = offsetLocation(fs.readFileSync(file, 'utf8'), span.start)
  diagnostic.line = location.line
  diagnostic.column = location.column
  return diagnostic
}

function offsetLocation(text: string, offset: number): { line: number; column: number } {
  let line = 1
  let column = 1
  for (let i = 0; i < Math.min(offset, text.length); i++) {
    if (text.charCodeAt(i) === 10) {
      line++
      column = 1
    } else {
      column++
    }
  }
  return { line, column }
}
