// build-gea-vite-geatsc.mjs turns an app's Display.setMemoryConfig call into
// compile-time overrides. A board test can only see the macro names; this is
// the generator that emits them.
//
// Each repo guards its own files: the boards that switch these features ON
// assert that in their own target tests.
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'

const embeddedBuildScript = readFileSync(new URL('../scripts/build-gea-vite-geatsc.mjs', import.meta.url), 'utf8')

assert.match(embeddedBuildScript, /memberPath\[1\] === 'setMemoryConfig'/, 'embedded app config generation should read Display.setMemoryConfig calls')
assert.match(embeddedBuildScript, /GEA_EMBEDDED_DISPLAY_COMMAND_BUFFER_COMMANDS/, 'embedded app config generation should emit command-buffer override macros')
assert.match(embeddedBuildScript, /GEA_EMBEDDED_DISPLAY_BACKGROUND_CACHE_IN_SRAM/, 'embedded app config generation should emit background-cache override macros')

// A board states its panel size with GEA_EMBEDDED_DISPLAY_WIDTH/HEIGHT compile
// definitions (see any target's main/CMakeLists.txt). This header must take
// them rather than hardcode a size, or every board but the default renders
// at the wrong geometry.
const displayHeader = readFileSync(new URL('../include/display.h', import.meta.url), 'utf8')
for (const macro of ['GEA_EMBEDDED_DISPLAY_WIDTH', 'GEA_EMBEDDED_DISPLAY_HEIGHT']) {
  assert.match(displayHeader, new RegExp(`#ifndef ${macro}\\n#define ${macro} `),
    `${macro} must be overridable by the board, not hardcoded`)
}
