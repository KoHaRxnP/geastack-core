import assert from 'node:assert/strict'
import { execFileSync } from 'node:child_process'
import { existsSync, readFileSync } from 'node:fs'
import { join, resolve } from 'node:path'

// The repo split scattered this test's inputs across sibling repos, and
// it also moved the framework source list out of the build script entirely:
//
//   targets/web/build-web.sh        -> simulator/targets/web/build-web.sh
//   lib/@geastack/core/host/*.cpp   -> core/packages/host/host/*.cpp
//
// build-web.sh no longer hardcodes the always-linked framework sources; it
// resolves @geastack/core and sources its gea_sources.sh, whose own header
// calls itself the single source of truth for that set. Only target shims
// (web_*) still live in the build script. So the framework half of this check
// asserts against the manifest -- by EXECUTING it, which measures the real
// linked set rather than grepping for paths no longer written anywhere -- and
// the target half still asserts against build-web.sh.
const coreRoot = new URL('../../..', import.meta.url).pathname
const workspaceRoot = join(coreRoot, '..')
const simulatorRoot = join(workspaceRoot, 'simulator')

const buildScript = readFileSync(join(simulatorRoot, 'targets', 'web', 'build-web.sh'), 'utf8')
const fetchHost = readFileSync(join(coreRoot, 'packages', 'host', 'host', 'fetch.cpp'), 'utf8')

const manifestSources = execFileSync(
  'bash',
  ['-c', '. "$1"; gea_fw_cxx_sources', 'bash', join(coreRoot, 'packages', 'core', 'gea_sources.sh')],
  {
    encoding: 'utf8',
    env: {
      ...process.env,
      GEA_CORE: join(coreRoot, 'packages', 'core'),
      GEA_HOST_DIR: join(coreRoot, 'packages', 'host'),
      GEA_ENGINE_DIR: join(coreRoot, 'packages', 'engine'),
      GEA_ELEMENTS_DIR: join(coreRoot, 'packages', 'elements'),
      GEA_GEAOS_PACKAGE_DIR: join(coreRoot, 'packages', 'geaos')
    }
  }
)
  .split('\n')
  .filter(Boolean)
  .map((path) => resolve(path))

// Always-linked framework sources backing application services.
const requiredFrameworkSources = [
  'packages/host/input.cpp',
  'packages/host/host/fetch.cpp',
  'packages/host/host/input.cpp',
  'packages/host/host/media.cpp'
]

for (const source of requiredFrameworkSources) {
  const absolute = resolve(join(coreRoot, source))
  assert.ok(
    manifestSources.includes(absolute),
    `gea_sources.sh should link ${source}`
  )
  assert.ok(
    existsSync(absolute),
    `${source} should exist`
  )
}

// Web-only target shims: the manifest deliberately excludes these, so they are
// still spelled out in the build script.
const requiredTargetSources = [
  'targets/web/main/web_power.cpp',
  'targets/web/main/web_storage_service.cpp'
]

for (const source of requiredTargetSources) {
  assert.ok(
    buildScript.includes(source),
    `targets/web/build-web.sh should link ${source}`
  )
  assert.ok(
    existsSync(join(simulatorRoot, source)),
    `${source} should exist`
  )
}

assert.ok(
  buildScript.includes('ASYNCIFY=1'),
  'web builds should enable Asyncify so host fetch can yield to real browser fetch'
)
assert.ok(
  fetchHost.includes('EM_ASYNC_JS'),
  'web host fetch should use an async Emscripten bridge'
)
assert.ok(
  !fetchHost.includes('xhr.open(method, url, false)'),
  'web host fetch must not block the browser main thread with synchronous XHR'
)
