#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/packages/core/test/.build/canvas-rounded-rect-alpha"
CXX_BIN="${CXX:-clang++}"

mkdir -p "$BUILD_DIR"

"$CXX_BIN" -std=c++20 \
  -DGEA_EMBEDDED_PIXEL_PANEL_ENDIAN=1 \
  -Wno-deprecated \
  -ffunction-sections \
  -fdata-sections \
  -ferror-limit=40 \
  -I "$ROOT/packages/core" \
  -I "$ROOT/packages/core/include" \
  "$ROOT/packages/engine/canvas.cpp" \
  "$ROOT/packages/core/test/test_canvas_rounded_rect_alpha_main.cpp" \
  -Wl,-dead_strip \
  -o "$BUILD_DIR/canvas-rounded-rect-alpha-test"

"$BUILD_DIR/canvas-rounded-rect-alpha-test"
