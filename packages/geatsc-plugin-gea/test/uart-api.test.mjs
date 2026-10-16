import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'
import { runInNewContext } from 'node:vm'
import test from 'node:test'
import ts from 'typescript'
import { createGeaHostShims } from '../dist/host-shims.js'

const core = resolve(import.meta.dirname, '../../core')
const hostFile = resolve(core, 'runtime/host.ts')
const source = ts.createSourceFile(hostFile, readFileSync(hostFile, 'utf8'), ts.ScriptTarget.Latest, true)
const uart = source.statements.find(statement => ts.isVariableStatement(statement) &&
  statement.declarationList.declarations.some(declaration => declaration.name.getText(source) === 'Uart'))
const code = ts.transpileModule(uart.getText(source).replace('export const Uart', 'globalThis.Uart'), {
  compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS },
}).outputText

test('UART facade preserves byte buffers and applies configuration defaults', () => {
  const calls = []
  const context = { __gea_Uart: {
    open(...args) { calls.push(args); return true },
    isOpen(port) { return port === 1 },
    close(port) { calls.push(['close', port]) },
    read(port, bytes) { assert.equal(port, 1); bytes[0] = 42; return 1 },
    write(port, bytes) { assert.equal(port, 1); assert.equal(bytes[0], 42); return bytes.length },
    available() { return 5 }, writable() { return 100 },
  } }
  runInNewContext(code, context)
  assert.equal(context.Uart.open(1, { tx: 1, rx: 0, baudRate: 5000000 }), true)
  assert.deepEqual(calls[0], [1, 1, 0, 5000000, 4096, 4096])
  context.Uart.open(2, { tx: 46, rx: 47, baudRate: 5000000, rxBufferSize: 8192, txBufferSize: 2048 })
  assert.deepEqual(calls[1], [2, 46, 47, 5000000, 8192, 2048])
  const storage = new Uint8Array(8)
  const view = storage.subarray(2, 5)
  assert.equal(context.Uart.read(1, view), 1)
  assert.equal(storage[2], 42)
  assert.equal(context.Uart.write(1, view), 3)
  assert.equal(context.Uart.available(1), 5)
  assert.equal(context.Uart.writable(1), 100)
  context.Uart.close(1)
  assert.deepEqual(calls.at(-1), ['close', 1])
})

test('unsupported browser UART reports unavailable without touching hardware', () => {
  const context = {}
  runInNewContext(code, context)
  assert.equal(context.Uart.open(1, { tx: 1, rx: 0, baudRate: 5000000 }), false)
  assert.equal(context.Uart.isOpen(1), false)
  assert.equal(context.Uart.read(1, new Uint8Array(1)), -1)
  assert.equal(context.Uart.write(1, new Uint8Array(1)), -1)
  assert.equal(context.Uart.available(1), -1)
  assert.equal(context.Uart.writable(1), -1)
  context.Uart.close(1)
})

test('native UART bindings leave the typed configuration wrapper in the program', () => {
  const bindings = createGeaHostShims()
  assert.equal(bindings.hostNamespaces.__gea_Uart, 'gea::host::Uart')
  assert.equal(bindings.hostNamespaces.Uart, undefined)
  for (const name of ['open', 'close', 'isOpen', 'read', 'write', 'available', 'writable'])
    assert.equal(bindings.hostNamespaceMethods.__gea_Uart[name], `gea::host::Uart.${name}`)
  assert.ok(!bindings.hostNamespaceNoThrowMethods.__gea_Uart.includes('read'))
  assert.ok(!bindings.hostNamespaceNoThrowMethods.__gea_Uart.includes('write'))
})
