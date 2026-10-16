import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'
import { runInNewContext } from 'node:vm'
import test from 'node:test'
import ts from 'typescript'
import { createGeaHostShims } from '../dist/host-shims.js'

const core = resolve(import.meta.dirname, '../../core')
const source = ts.createSourceFile('host.ts', readFileSync(resolve(core, 'runtime/host.ts'), 'utf8'), ts.ScriptTarget.Latest, true)
const declaration = source.statements.find(statement => ts.isVariableStatement(statement) && statement.declarationList.declarations.some(d => d.name.getText(source) === 'EspNow'))
const code = ts.transpileModule(declaration.getText(source).replace('export const EspNow', 'globalThis.EspNow'), { compilerOptions: { target: ts.ScriptTarget.ES2022 } }).outputText

test('ESP-NOW preserves typed subviews and defaults to 54 Mbps', () => {
  const calls = []
  const context = { __gea_EspNow: {
    open(...args) { calls.push(args); return true },
    isOpen() { return true }, canSend() { return true }, close() {},
    setMaxTxPower(power) { calls.push(power); return power === 8 },
    getMaxTxPower() { return 8 },
    addPeer(mac, key) { calls.push([mac.length, key.length]); return true },
    send(mac, bytes) { assert.equal(mac[0], 28); return bytes.length },
    receive(mac, bytes) { mac[0] = 28; bytes[0] = 83; return 1 },
  } }
  runInNewContext(code, context)
  assert.equal(context.EspNow.open(6), true)
  assert.deepEqual(calls[0], [6, 54])
  assert.equal(context.EspNow.canSend(), true)
  assert.equal(context.EspNow.setMaxTxPower(8), true)
  assert.equal(calls.at(-1), 8)
  assert.equal(context.EspNow.getMaxTxPower(), 8)
  const mac = new Uint8Array(6)
  const backing = new Uint8Array(1500)
  const view = backing.subarray(12, 1482)
  assert.equal(context.EspNow.receive(mac, view), 1)
  assert.equal(backing[12], 83)
  assert.equal(context.EspNow.send(mac, view), 1470)
  assert.equal(context.EspNow.addPeer(mac), true)
  assert.deepEqual(calls.at(-1), [6, 0])
  assert.equal(context.EspNow.addPeer(mac, new Uint8Array(16)), true)
  assert.deepEqual(calls.at(-1), [6, 16])
})
test('unsupported targets report unavailable, and typed buffers may throw for detachment', () => {
  const context = {}
  runInNewContext(code, context)
  assert.equal(context.EspNow.open(6), false)
  assert.equal(context.EspNow.canSend(), false)
  assert.equal(context.EspNow.setMaxTxPower(8), false)
  assert.equal(context.EspNow.getMaxTxPower(), -1)
  assert.equal(context.EspNow.send(new Uint8Array(6), new Uint8Array(1)), -1)
  assert.equal(context.EspNow.receive(new Uint8Array(6), new Uint8Array(1470)), -1)
  const bindings = createGeaHostShims()
  assert.equal(bindings.hostNamespaces.__gea_EspNow, 'gea::host::EspNow')
  for (const name of ['send', 'receive', 'addPeer']) {
    assert.equal(bindings.hostNamespaceMethods.__gea_EspNow[name], `gea::host::EspNow.${name}`)
    assert.ok(!bindings.hostNamespaceNoThrowMethods.__gea_EspNow.includes(name))
  }
})

test('power methods have typed native bindings and documented units', () => {
  const bindings = createGeaHostShims()
  for (const name of ['setMaxTxPower', 'getMaxTxPower']) {
    assert.equal(bindings.hostNamespaceMethods.__gea_EspNow[name], `gea::host::EspNow.${name}`)
    assert.ok(bindings.hostNamespaceNoThrowMethods.__gea_EspNow.includes(name))
  }
  const declarations = readFileSync(resolve(core, 'index.d.ts'), 'utf8')
  assert.match(declarations, /setMaxTxPower\(quarterDbm: number\): boolean/)
  assert.match(declarations, /getMaxTxPower\(\): number/)
})

test('HE selector reaches the existing typed open binding and preserves rejection', () => {
  const calls = []
  const context = { __gea_EspNow: {
    open(channel, rate) { calls.push([channel, rate]); return rate !== 115 },
  } }
  runInNewContext(code, context)
  for (const rate of [24, 36, 54, 65, 115]) {
    assert.equal(context.EspNow.open(6, rate), rate !== 115)
    assert.deepEqual(calls.at(-1), [6, rate])
  }
  context.__gea_EspNow.open = (channel, rate) => channel === 6 && rate === 115
  assert.equal(context.EspNow.open(6, 115), true)
  const bindings = createGeaHostShims()
  assert.equal(bindings.hostNamespaceMethods.__gea_EspNow.open, 'gea::host::EspNow.open')
  assert.ok(bindings.hostNamespaceNoThrowMethods.__gea_EspNow.includes('open'))
  const declarations = readFileSync(resolve(core, 'index.d.ts'), 'utf8')
  assert.match(declarations, /open\(channel: number, rateMbps\?: number\): boolean/)
  assert.match(declarations, /115 for HE20 MCS9 short GI \(nominal 114\.7 Mbps\)/)
})
