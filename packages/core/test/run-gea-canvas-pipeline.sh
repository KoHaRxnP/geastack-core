#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
exec "$ROOT/packages/core/test/run-gea-native-app-pipeline.sh" \
  "gea-canvas-pipeline" \
  "$ROOT/packages/core/test/fixtures/gea-canvas-basic" \
  "index.tsx" \
  "packages/core/test/test_gea_canvas_main.cpp"
