#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/packages/core/test/.build/packed-copy-overlap"
CXX_BIN="${CXX:-clang++}"

mkdir -p "$BUILD_DIR"

# Header-only: PackedPixels lives entirely in pixel.h, and the test instantiates
# both depths (4bpp GRAY4, 2bpp GRAY2) directly, so no target format needs to be
# selected and no engine sources are linked.
"$CXX_BIN" -std=c++20 -O2 \
  -fsanitize=undefined \
  -Wno-deprecated \
  -ferror-limit=40 \
  -I "$ROOT/packages/core" \
  -I "$ROOT/packages/core/include" \
  "$ROOT/packages/core/test/test_packed_copy_overlap_main.cpp" \
  -o "$BUILD_DIR/packed-copy-overlap-test"

"$BUILD_DIR/packed-copy-overlap-test"
