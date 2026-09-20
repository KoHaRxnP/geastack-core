#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/packages/core/test/.build/gea-canvas-3d-pipeline"
CXX_BIN="${CXX:-clang++}"

source "$ROOT/packages/core/test/native-test-common.sh"
gea_require_app_project

mkdir -p "$BUILD_DIR"

node "$ROOT/packages/core/scripts/build-gea-vite-geatsc.mjs" \
  --app-dir "$GEA_APP_PROJECT/apps/canvas-3d" \
  --entry "index.tsx" \
  --out-dir "$BUILD_DIR" \
  --gea-ir-backend \
  --pixel-panel-endian 1 \
  --allow-any

GEA_NATIVE_TEST_INCLUDE_HOST=0
gea_build_native_test \
  "$BUILD_DIR" \
  "$BUILD_DIR/gea-canvas-3d-test" \
  "$ROOT/packages/core/test/test_gea_canvas_3d_main.cpp" \
  "-DGEA_CANVAS_3D_TEST_PANEL_PPM=\"$BUILD_DIR/panel.ppm\""

"$BUILD_DIR/gea-canvas-3d-test"
