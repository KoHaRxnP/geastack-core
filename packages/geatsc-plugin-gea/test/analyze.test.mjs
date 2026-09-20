import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import test from 'node:test'

import { analyzeSourceHostBindings } from '../dist/analyze.js'

function app(t, files) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'gea-plugin-analyze-'))
  t.after(() => fs.rmSync(root, { recursive: true, force: true }))
  for (const [name, text] of Object.entries(files)) {
    fs.mkdirSync(path.dirname(path.join(root, name)), { recursive: true })
    fs.writeFileSync(path.join(root, name), text)
  }
  return path.join(root, 'index.tsx')
}

test('an https literal reached through a relative import reports the https feature', (t) => {
  const entry = app(t, {
    'index.tsx': "import { Display, mount } from '@geastack/core'\nimport { weather } from './stores/WeatherStore'\n// see https://example.com for the API\nmount(weather)\n",
    'stores/WeatherStore.ts': "import { Store, WiFi } from '@geastack/core'\nexport class WeatherStore extends Store {\n  fetchInFlight = 0\n  async load(id: number) {\n    const response = await fetch('https://api.open-meteo.com/v1/forecast?id=' + id)\n    return response.json()\n  }\n}\nexport const weather = new WeatherStore()\n",
  })
  assert.deepEqual(analyzeSourceHostBindings(entry), { bindings: ['display', 'fetch', 'wifi'], features: ['https'] })
})

test('a comment citing an https URL does not link TLS, and a member fetch is not the host global', (t) => {
  const entry = app(t, {
    'index.tsx': "import { Display } from '@geastack/core'\n// Source: https://github.com/example/example\nclass Loader { fetch(url: string) { return url } }\nnew Loader().fetch('http://192.168.1.2/tile')\n",
  })
  assert.deepEqual(analyzeSourceHostBindings(entry), { bindings: ['display'], features: [] })
})

test('a WebSocket constructor and a wss literal both bring the network stack', (t) => {
  const entry = app(t, {
    'index.tsx': "const socket = new WebSocket(`wss://${host}/stream`)\n",
  })
  assert.deepEqual(analyzeSourceHostBindings(entry), { bindings: ['websocket'], features: ['https'] })
})
