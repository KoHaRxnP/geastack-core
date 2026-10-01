#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/packages/core/test/.build/rotated-projected-text"
CXX_BIN="${CXX:-clang++}"
mkdir -p "$BUILD_DIR"
export TMPDIR="$BUILD_DIR"
source "$ROOT/packages/core/test/native-test-common.sh"
printf 'void __gea_top_level() {}\n' > "$BUILD_DIR/program.cpp"
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/rotated-projected-text" \
  "$ROOT/packages/core/test/test_rotated_projected_text_main.cpp" \
  -DGEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE=1
"$BUILD_DIR/rotated-projected-text"
