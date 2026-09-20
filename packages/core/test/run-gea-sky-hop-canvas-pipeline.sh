#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
exec "$ROOT/packages/core/test/run-gea-native-app-pipeline.sh" \
  "gea-sky-hop-canvas-pipeline" \
  "apps/sky-hop" \
  "index.tsx" \
  "packages/core/test/test_gea_sky_hop_canvas_main.cpp" \
  --pixel-panel-endian 1
