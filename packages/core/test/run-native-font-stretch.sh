#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/packages/core/test/.build/native-font-stretch"
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-2}"
CXX_BIN="${CXX:-clang++}"
mkdir -p "$BUILD_DIR"
source "$ROOT/packages/core/test/native-test-common.sh"
printf 'void __gea_top_level() {}\n' > "$BUILD_DIR/program.cpp"
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/native-font-stretch" "$ROOT/packages/core/test/test_native_font_stretch.cpp" -DGEA_CSS_FONT_STRETCH=1
"$BUILD_DIR/native-font-stretch"
