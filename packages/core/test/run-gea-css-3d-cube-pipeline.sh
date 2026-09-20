#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
exec "$ROOT/packages/core/test/run-gea-native-app-pipeline.sh" \
  "gea-css-3d-cube-pipeline" \
  "apps/css-3d-cube" \
  "index.tsx" \
  "packages/core/test/test_gea_css_3d_cube_main.cpp"
