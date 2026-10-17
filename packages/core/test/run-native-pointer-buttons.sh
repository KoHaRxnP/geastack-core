#!/usr/bin/env bash
set -euo pipefail
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-2}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/packages/core/test/.build/native-pointer-buttons"
CXX_BIN="${CXX:-clang++}"
mkdir -p "$BUILD_DIR"
source "$ROOT/packages/core/test/native-test-common.sh"
printf 'void __gea_top_level() {}\n' > "$BUILD_DIR/program.cpp"
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/native-pointer-buttons" "$ROOT/packages/core/test/test_native_pointer_buttons.cpp"
"$BUILD_DIR/native-pointer-buttons"
