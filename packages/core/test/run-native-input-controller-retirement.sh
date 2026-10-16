#!/usr/bin/env bash
set -euo pipefail
[[ $# == 1 && "$1" == --build-only ]] || { echo 'Exactly --build-only required' >&2;exit 2; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/dist/native-input-detach"
CXX_BIN="${CXX:-clang++}"
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-2}"
source "$ROOT/packages/core/test/native-test-common.sh"
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/native-input-controller-retirement-test" "$ROOT/packages/core/test/test_native_input_controller_retirement.cpp"
printf '%s\n' "${GEA_NATIVE_OBJECTS[@]}" > "$BUILD_DIR/native-input-controller-retirement-test.objects.txt"
