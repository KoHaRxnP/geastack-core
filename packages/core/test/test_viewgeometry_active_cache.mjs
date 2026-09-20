import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'

// The renderer's `ui/` tree moved from `packages/core/ui/` to
// `packages/engine/ui/` in the repo split. `repoRoot` (three levels up) is
// still correct -- only the package name changed -- but the stale path made
// every run die in `readFileSync` with ENOENT before a single assertion ran.
const repoRoot = resolve(import.meta.dirname, '../../..')
const source = readFileSync(resolve(repoRoot, 'packages/engine/ui/view.cpp'), 'utf8')

const forbidden = [
  /NodeTransformCache\s*\*\s*cache\s*=\s*new\s*\([^)]*\)\s*NodeTransformCache\s*\[\s*kMaxNodes\s*\]/,
  /AverageDepthCacheEntry\s*\*\s*cache\s*=\s*new\s*\([^)]*\)\s*AverageDepthCacheEntry\s*\[\s*kMaxNodes\s*\]/,
  /CornerTransformCacheEntry\s*\*\s*cache\s*=\s*new\s*\([^)]*\)\s*CornerTransformCacheEntry\s*\[\s*kMaxNodes\s*\]/,
]

for (const pattern of forbidden) {
  if (pattern.test(source)) {
    throw new Error(`ViewGeometry caches must track active transform nodes, not allocate kMaxNodes entries: ${pattern}`)
  }
}

// The three slot counts became build-tunable macros after this test was
// written -- each `#define`d to a default just above the constants, so a
// target can override them -- and `kActive*CacheSlots = <digits>` no longer
// appears literally. Follow the indirection instead of matching digits: the
// point of the check is that each cache is capped by an explicit small bound
// rather than an open-ended one like kMaxNodes, and that survives the macro.
const caches = [
  ['Transform', 'GEA_EMBEDDED_UI_TRANSFORM_CACHE_SLOTS'],
  ['Depth', 'GEA_EMBEDDED_UI_DEPTH_CACHE_SLOTS'],
  ['Corner', 'GEA_EMBEDDED_UI_CORNER_CACHE_SLOTS'],
]

for (const [name, macro] of caches) {
  if (!new RegExp(`kActive${name}CacheSlots\\s*=\\s*${macro}\\s*;`).test(source)) {
    throw new Error(`kActive${name}CacheSlots must be bound to ${macro}, not to an open-ended bound such as kMaxNodes`)
  }

  const match = source.match(new RegExp(`#define\\s+${macro}\\s+(\\d+)`))
  if (!match) throw new Error(`expected an explicit default slot limit for ${macro}`)

  const count = Number(match[1])
  if (!Number.isInteger(count) || count <= 0 || count > 64) {
    throw new Error(`active ViewGeometry cache slot count must be in 1..64, got ${count}`)
  }
}
