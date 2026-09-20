#!/usr/bin/env node
import assert from "node:assert/strict"
import { readFileSync } from "node:fs"
import { resolve } from "node:path"

// The renderer's `ui/` tree moved from `packages/core/ui/` to
// `packages/engine/ui/` in the repo split. `repoRoot` (three levels up) is
// still correct -- only the package name changed -- but the stale paths made
// every run die in `readFileSync` with ENOENT before a single assertion ran.
const repoRoot = resolve(new URL("../../..", import.meta.url).pathname)
const renderSource = readFileSync(resolve(repoRoot, "packages/engine/ui/render.cpp"), "utf8")
const internalHeader = readFileSync(resolve(repoRoot, "packages/engine/ui/internal.h"), "utf8")
const treeNodesSource = readFileSync(resolve(repoRoot, "packages/engine/ui/tree_nodes.cpp"), "utf8")

assert.doesNotMatch(
  renderSource,
  /replayClipPushed\s*\[\s*kMaxCommands\s*\]/,
  "DisplayList replay clip stack must grow by actual clip depth, not reserve kMaxCommands bytes"
)

assert.doesNotMatch(
  renderSource,
  /FilterBlurCacheEntry\s+filterBlurCaches\s*\[\s*kMaxNodes\s*\]/,
  "filter blur cache metadata must be allocated lazily instead of reserving one entry for every possible node"
)

assert.match(
  renderSource,
  /ensureNodeScratchCapacity\s*\(/,
  "DisplayListState should expose an active-node-capacity growth path"
)

assert.match(
  renderSource,
  /filterBlurCacheForNode\s*\(/,
  "DisplayListState should allocate filter blur cache entries only for nodes that use blur"
)

assert.match(
  renderSource,
  /resetStorage\s*\(/,
  "DisplayListState should be able to release growable storage on app/tree reset"
)

assert.match(
  internalHeader,
  /void\s+resetStorage\s*\(\s*\)\s*;/,
  "DisplayList should expose a resetStorage hook for app switches"
)

assert.match(
  treeNodesSource,
  /DisplayList::instance\(\)\.resetStorage\(\)/,
  "Tree::clear should release DisplayList dynamic render-state storage between apps"
)
