#!/usr/bin/env bash
set -euo pipefail
[[ $# == 1 && "$1" == --build-only ]] || { echo 'Exactly --build-only required; execution belongs to Root' >&2;exit 2; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/dist/native-input-contract"
CXX_BIN="${CXX:-clang++}"
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-2}"
[[ -s "$BUILD_DIR/geatsc-sources.txt" ]] || { echo 'Actual normal CLI source manifest required' >&2;exit 2; }
if [[ -f "$BUILD_DIR/program.cpp" && "$BUILD_DIR/program.cpp" -nt "$BUILD_DIR/geatsc-sources.txt" ]]; then
  echo 'Refuse newer fallback program.cpp' >&2;exit 2
fi
source "$ROOT/packages/core/test/native-test-common.sh"
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/native-input-contract-test" "$ROOT/packages/core/test/test_native_input_contract.cpp"
printf '%s\n' "${GEA_NATIVE_OBJECTS[@]}" > "$BUILD_DIR/native-input-contract-test.objects.txt"
printf '%s\n' "${GEA_NATIVE_CXXFLAGS[@]}" > "$BUILD_DIR/native-input-contract-test.cxx-flags.txt"
printf '%s\n' "${GEA_NATIVE_LINK_FLAGS[@]}" > "$BUILD_DIR/native-input-contract-test.link-flags.txt"
