import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import test from 'node:test'
import { analyzeSourceHostBindings } from '../dist/analyze.js'

test('RTC and microphone globals retain networking and audio capabilities', (t) => {
  // Resolved, and compared resolved: on Windows TypeScript asks for `C:/virtual-rtc/index.js`.
  const file = path.resolve('/virtual-rtc/index.js')
  t.mock.method(fs, 'existsSync', p => path.resolve(String(p)) === file)
  t.mock.method(fs, 'statSync', () => ({ isFile: () => true }))
  t.mock.method(fs, 'readFileSync', () => `
    const peer = new RTCPeerConnection({});
    const mic = navigator.mediaDevices.getUserMedia({ audio: true });
  `)
  assert.deepEqual(analyzeSourceHostBindings(file).bindings, ['audio', 'rtc'])
})

test('installed LiveKit source contributes the app audio and network requirements', () => {
  // The fixture imports the actual npm SDK, a devDependency of this package.
  const entry = path.resolve(import.meta.dirname, 'fixtures/livekit-agent/conversation.js')
  assert.ok(fs.existsSync(path.resolve(import.meta.dirname, '../node_modules/livekit-client/package.json')), 'livekit-client is not installed: run npm ci in packages/geatsc-plugin-gea')
  const result = analyzeSourceHostBindings(entry)
  assert.ok(result.bindings.includes('audio'), JSON.stringify(result.bindings))
  assert.ok(result.bindings.includes('rtc'), JSON.stringify(result.bindings))
  assert.ok(result.bindings.includes('websocket'), JSON.stringify(result.bindings))
})
