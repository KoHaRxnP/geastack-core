#!/usr/bin/env bash
set -euo pipefail
[[ $# == 1 && "$1" == --build-only ]] || { echo 'Exactly --build-only required; CPU mode execution is explicit' >&2;exit 2; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/dist/native-dom-mutations"
mkdir -p "$BUILD_DIR"
CXX_BIN="${CXX:-clang++}"
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-2}"
source "$ROOT/packages/core/test/native-test-common.sh"
# Engine API regression only; no compiler emission is claimed.
printf 'void __gea_top_level() {}\n' > "$BUILD_DIR/program.cpp"
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/native-dom-mutations-test" "$ROOT/packages/core/test/test_native_dom_mutations.cpp"
printf '%s\n' "${GEA_NATIVE_OBJECTS[@]}" > "$BUILD_DIR/native-dom-mutations-test.objects.txt"
printf '%s\n' "${GEA_NATIVE_CXXFLAGS[@]}" > "$BUILD_DIR/native-dom-mutations-test.cxx-flags.txt"
printf '%s\n' "${GEA_NATIVE_LINK_FLAGS[@]}" > "$BUILD_DIR/native-dom-mutations-test.link-flags.txt"
