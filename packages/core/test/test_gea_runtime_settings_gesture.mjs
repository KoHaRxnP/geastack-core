import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'

// The settings gesture is handled once, in TypeScript. It used to be detected
// in the C++ runtime as well, which meant a top-edge swipe could open the
// control centre twice or fight the TS handler for the same touch. This guards
// the C++ half: the runtime must carry no settings-gesture logic at all.
//
// The TypeScript half -- that no app or shared component calls back into the
// removed Settings.handleSwipe -- is guarded in geastack/examples, where those
// sources live.
const runtimeSource = readFileSync(new URL('../runtime.cpp', import.meta.url), 'utf8')

assert.doesNotMatch(runtimeSource, /detect_settings_swipe/)
assert.doesNotMatch(runtimeSource, /top-edge swipe-down opens the control-center/)
assert.doesNotMatch(runtimeSource, /queueSettingsToggle\(\);\n\s*break;/)
