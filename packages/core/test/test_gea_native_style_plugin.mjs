import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { build } from 'vite'
import { geaPlugin } from '@geajs/vite-plugin'
import { geaNativeStylePlugin } from '../scripts/gea-native-style-plugin.mjs'

const root = fileURLToPath(new URL('..', import.meta.url))
const pkg = path.join(root, 'node_modules/@geajs/core')
const runtimeEntry = path.join(pkg, 'src/index.ts')
assert.equal(JSON.parse(fs.readFileSync(path.join(pkg, 'package.json'), 'utf8')).version, '1.5.1')
const result = await build({
  configFile: false, root, logLevel: 'error',
  plugins: [geaNativeStylePlugin({ runtimeEntry }), {
    name: 'style-conformance-entry',
    resolveId(id) { if (id.endsWith('virtual:style-conformance')) return '\0virtual:style-conformance' },
    load(id) {
      if (id !== '\0virtual:style-conformance') return null
      return `export { jsx } from '@geajs/core/jsx-runtime';
export { styleValue } from ${JSON.stringify(path.join(pkg, 'src/runtime/style-value.ts'))};`
    },
  }],
  build: { write: false, minify: false, lib: { entry: 'virtual:style-conformance', formats: ['es'] } },
})
const chunk = (Array.isArray(result) ? result[0] : result).output.find(item => item.type === 'chunk' && item.isEntry)
const native = await import('data:text/javascript;base64,' + Buffer.from(chunk.code).toString('base64'))
assert.equal(native.styleValue('width', 150), '150')
assert.equal(native.styleValue('width', '150px'), '150px')
assert.equal(native.styleValue('line-height', 1.5), '1.5')
assert.equal(native.jsx('div', { style: { width: 150, opacity: 0.5 } }), '<div style="width:150;opacity:0.5"></div>')
const browser = await import(new URL('../node_modules/@geajs/core/dist/jsx-runtime.mjs', import.meta.url))
assert.equal(browser.jsx('div', { style: { width: 150, opacity: 0.5 } }), '<div style="width:150px;opacity:0.5"></div>')

// Exercise the installed JSX compiler and native adapter together. The native
// compiler consumes this IR, so browser unit defaults must not rewrite it.
const entry = path.join(root, 'test/unit-conformance.tsx')
const irBuild = await build({
  configFile: false, root: path.join(root, 'test'), logLevel: 'error',
  plugins: [geaNativeStylePlugin({ runtimeEntry }), {
    name: 'unit-conformance-entry',
    resolveId(id) { if (id === entry) return id },
    load(id) {
      if (id !== entry) return null
      return "import { Component } from '@geajs/core'; export class UnitConformance extends Component { width = 150; template() { return <div style={{ width: this.width, height: 150, left: `${this.width}px` }} /> } }"
    },
  }, geaPlugin({ ir: { enabled: true, outFile: 'units.json' } })],
  build: { write: false, minify: false, lib: { entry, formats: ['es'] } },
})
const irAsset = (Array.isArray(irBuild) ? irBuild[0] : irBuild).output.find(item => item.type === 'asset' && item.fileName === 'units.json')
const ir = JSON.parse(irAsset.source)
assert.deepEqual(ir.components[0].template.slots[0].exprObjectFields.map(({ name, expr }) => [name, expr]), [
  ['width', 'this.width'], ['height', '150'], ['left', '`${this.width}px`'],
])
console.log('Gea 1.5.1: native serializer and JSX IR preserve raw values; standalone browser serializer adds px')
