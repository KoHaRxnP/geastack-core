#!/usr/bin/env bash
set -euo pipefail
[[ $# == 0 || ( $# == 1 && "$1" == --build-only ) ]] || { echo 'Use --build-only or no arguments' >&2;exit 2; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/dist/native-input-destructive"
CXX_BIN="${CXX:-clang++}"
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-2}"
source "$ROOT/packages/core/test/native-test-common.sh"
mkdir -p "$BUILD_DIR"
printf 'void __gea_top_level() {}\n' > "$BUILD_DIR/program.cpp"
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/native-input-destructive-test" "$ROOT/packages/core/test/test_native_input_destructive.cpp"
printf '%s\n' "${GEA_NATIVE_OBJECTS[@]}" > "$BUILD_DIR/native-input-destructive-test.objects.txt"
if [[ $# == 0 ]]; then
  for mode in reentrant-down cancel-coordinates primary-move primary-up secondary-move secondary-up; do
    "$BUILD_DIR/native-input-destructive-test" "$mode"
  done
fi
