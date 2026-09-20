import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'

const entry = readFileSync(new URL('../gea_app_entry.cpp', import.meta.url), 'utf8')

assert.doesNotMatch(entry, /weak_import/, 'embedded GCC does not support the Mach-O weak_import attribute')
assert.match(entry, /gea_cycle_collection_defer_begin\(\) __attribute__\(\(weak\)\)/)
assert.match(entry, /gea_cycle_collection_defer_end\(\) __attribute__\(\(weak\)\)/)

const initBody = entry.slice(entry.indexOf('void Application::init'), entry.indexOf('namespace {', entry.indexOf('void Application::init')))
const frameBody = entry.slice(entry.indexOf('void Application::frame'), entry.indexOf('void Application::toggleSettings'))
assert.match(initBody, /FrameCycleCollectionDeferral deferCycleCollection;/)
assert.match(frameBody, /FrameCycleCollectionDeferral deferCycleCollection;/)
