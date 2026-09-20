import assert from 'node:assert/strict'
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import test from 'node:test'

import { geatscPlugin } from '../dist/index.js'

function transformModuleFirstFixture({
  entrySource,
  hostCapabilities,
  moduleDefinesDrainMicrotasks = false,
  pluginRuntimeSource,
}) {
  const work = mkdtempSync(join(tmpdir(), 'gea-module-runtime-'))
  const entry = join(work, 'entry.ts')
  const irPath = join(work, 'gea-ir.json')
  writeFileSync(entry, entrySource)
  writeFileSync(irPath, JSON.stringify({
    schema: 'gea-ir',
    version: 1,
    entry,
    modules: [],
    components: [],
    stores: [],
    hostCapabilities,
  }))

  try {
    const options = { 'gea.ir': './gea-ir.json' }
    const plugin = geatscPlugin()
    plugin.configure?.({ entry, options })
    assert.deepEqual(plugin.validate?.(), [])

    const moduleSource = moduleDefinesDrainMicrotasks
      ? [
          '#include "./0000_entry.hpp"',
          '',
          'namespace gea::framework::app::generated {',
          'void drainMicrotasks() {',
          '  gea_cpp_drain_microtasks();',
          '}',
          '}  // namespace gea::framework::app::generated',
          '',
        ].join('\n')
      : '#include "./0000_entry.hpp"\n'
    const sources = [
      { fileName: 'entry.cpp', source: '#include "generated_support.hpp"\n' },
      { fileName: 'generated_support.hpp', source: '#pragma once\n' },
      {
        fileName: 'modules/0000_entry.types.hpp',
        source: '#pragma once\n#include "../generated_support.hpp"\n',
      },
      {
        fileName: 'modules/0000_entry.hpp',
        source: '#pragma once\n#include "../generated_support.hpp"\n#include "./0000_entry.types.hpp"\n',
      },
      { fileName: 'modules/0000_entry.cpp', source: moduleSource },
      ...(pluginRuntimeSource === undefined
        ? []
        : [{ fileName: 'modules/gea_plugin_runtime.cpp', source: pluginRuntimeSource }]),
    ]
    const transform = plugin.createCppBackend?.().transformGeneratedSources
    assert.ok(transform, 'expected the plugin C++ source transform')
    return transform(sources, {
      options,
      modules: [{ sourceFile: { fileName: entry, text: entrySource }, relativePath: 'entry.ts' }],
    })
  } finally {
    rmSync(work, { recursive: true, force: true })
  }
}

function sourceNamed(sources, fileName) {
  const source = sources.find((candidate) => candidate.fileName === fileName)
  assert.ok(source, `missing generated source ${fileName}`)
  return source.source
}

function occurrenceCount(source, needle) {
  return source.split(needle).length - 1
}

test('module-first DOM shim is compiled once even when microtasks already have a definition', () => {
  const sources = transformModuleFirstFixture({
    entrySource: 'export const body = document.body\n',
    hostCapabilities: ['dom'],
    moduleDefinesDrainMicrotasks: true,
  })
  const geaIr = sourceNamed(sources, 'modules/gea_ir.hpp')
  const pluginRuntime = sourceNamed(sources, 'modules/gea_plugin_runtime.cpp')
  const combined = sources.map((source) => source.source).join('\n')

  assert.doesNotMatch(geaIr, /host_document_gea\.cpp/)
  assert.match(geaIr, /void drainMicrotasks\(\);/)
  assert.match(geaIr, /gea_cpp_value hostDocumentNodeValue\(gea::embedded::ui::NodeHandle node, double nodeType = 1\.0\);/)
  assert.match(pluginRuntime, /#include "\.\.\/host_document_gea\.cpp"/)
  assert.match(pluginRuntime, /gea_cpp_value hostDocumentNodeValue\(gea::embedded::ui::NodeHandle node, double nodeType\)/)
  assert.match(pluginRuntime, /return gea::runtime::host::domNodeValue\(node, nodeType\);/)
  assert.equal(occurrenceCount(combined, 'host_document_gea.cpp'), 1)
  assert.doesNotMatch(pluginRuntime, /void drainMicrotasks\s*\(/)
  for (const source of sources) {
    if (source.fileName === 'modules/gea_plugin_runtime.cpp') continue
    assert.doesNotMatch(source.source, /host_document_gea\.cpp/, source.fileName)
  }
})

test('module-first DOM shim is appended to an existing plugin runtime source exactly once', () => {
  const sources = transformModuleFirstFixture({
    entrySource: 'export const body = document.body\n',
    hostCapabilities: ['dom'],
    pluginRuntimeSource: '#include "../generated_support.hpp"\n\nint gea_plugin_runtime_marker = 1;\n',
  })
  const pluginRuntime = sourceNamed(sources, 'modules/gea_plugin_runtime.cpp')
  const combined = sources.map((source) => source.source).join('\n')

  assert.match(pluginRuntime, /#include "\.\.\/host_document_gea\.cpp"/)
  assert.match(pluginRuntime, /int gea_plugin_runtime_marker = 1;/)
  assert.match(pluginRuntime, /void drainMicrotasks\(\)/)
  assert.equal(occurrenceCount(pluginRuntime, 'host_document_gea.cpp'), 1)
  assert.equal(occurrenceCount(combined, 'host_document_gea.cpp'), 1)
})

test('module-first non-DOM output keeps the microtasks runtime without the DOM shim', () => {
  const sources = transformModuleFirstFixture({
    entrySource: 'export const answer = 42\n',
    hostCapabilities: [],
  })
  const geaIr = sourceNamed(sources, 'modules/gea_ir.hpp')
  const pluginRuntime = sourceNamed(sources, 'modules/gea_plugin_runtime.cpp')
  const combined = sources.map((source) => source.source).join('\n')

  assert.doesNotMatch(geaIr, /host_document_gea\.cpp/)
  assert.doesNotMatch(geaIr, /hostDocumentNodeValue/)
  assert.doesNotMatch(pluginRuntime, /host_document_gea\.cpp/)
  assert.doesNotMatch(pluginRuntime, /hostDocumentNodeValue/)
  assert.match(pluginRuntime, /void drainMicrotasks\(\)/)
  assert.equal(occurrenceCount(combined, 'host_document_gea.cpp'), 0)
})
