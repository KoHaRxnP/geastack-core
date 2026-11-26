import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import { fileURLToPath } from 'node:url'
import vm from 'node:vm'
import test from 'node:test'
import ts from 'typescript'

const componentPath = fileURLToPath(new URL('../../elements/components/Checkbox.tsx', import.meta.url))
const source = await readFile(componentPath, 'utf8')
const compiled = ts.transpileModule(source, {
  compilerOptions: {
    target: ts.ScriptTarget.ES2022,
    module: ts.ModuleKind.CommonJS,
    jsx: ts.JsxEmit.React,
    jsxFactory: 'jsx',
  },
}).outputText

const moduleExports = {}
vm.runInNewContext(compiled, {
  exports: moduleExports,
  jsx: (type, props) => ({ type, props }),
})
const { Checkbox } = moduleExports

function render(props = {}) {
  return Checkbox({ label: 'Example option', ...props })
}

test('unchecked Checkbox requests checked=true and invokes each callback once', () => {
  const changes = []
  const callbacks = []
  const event = { type: 'click' }
  const view = render({
    onChange: (value) => changes.push(value),
    onPress: (value) => callbacks.push(['press', value]),
    onClick: (value) => callbacks.push(['click', value]),
  })

  view.props.onClick(event)

  assert.deepEqual(changes, [true])
  assert.deepEqual(callbacks, [['press', event], ['click', event]])
})

test('checked Checkbox requests checked=false', () => {
  const changes = []
  render({ checked: true, onChange: (value) => changes.push(value) }).props.onClick({})
  assert.deepEqual(changes, [false])
})

test('indeterminate Checkbox requests checked=true', () => {
  const changes = []
  render({ indeterminate: true, onChange: (value) => changes.push(value) }).props.onClick({})
  assert.deepEqual(changes, [true])
})

test('disabled Checkbox suppresses state and interaction callbacks', () => {
  const calls = []
  const view = render({
    disabled: true,
    onChange: (value) => calls.push(['change', value]),
    onPress: () => calls.push(['press']),
    onClick: () => calls.push(['click']),
  })

  view.props.onClick({ type: 'click' })
  assert.deepEqual(calls, [])
})

test('uses a single native click binding for touch-generated click activation', () => {
  const view = render({ onPress() {}, onClick() {} })
  assert.equal(typeof view.props.onClick, 'function')
  assert.equal('onPress' in view.props, false)
})

test('merges caller class maps with state classes and default styles', () => {
  const view = render({ checked: true, class: { 'custom-active': true, 'custom-off': false } })
  assert.equal(view.props.class['gea-checkbox'], true)
  assert.equal(view.props.class['gea-checkbox-checked'], true)
  assert.equal(view.props.class['custom-active'], true)
  assert.equal('custom-off' in view.props.class, false)
  assert.equal(view.props.style.display, 'flex')
  assert.equal(view.props.children[0].props.style.width, 18)
})

test('requires a non-empty visible label for every Checkbox', () => {
  assert.throws(() => Checkbox({ label: '' }), /non-empty label/)
  assert.throws(() => Checkbox({}), /non-empty label/)
})
