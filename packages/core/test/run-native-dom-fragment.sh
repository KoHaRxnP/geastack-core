#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/dist/native-dom-fragment"
mkdir -p "$BUILD_DIR"
printf 'void __gea_top_level() {}\n' > "$BUILD_DIR/program.cpp"
CXX_BIN="${CXX:-clang++}"
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-2}"
source "$ROOT/packages/core/test/native-test-common.sh"
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/native-dom-fragment-test" "$ROOT/packages/core/test/test_native_dom_fragment.cpp"
status=0
for mode in basic block absolute structural-root relative fixed; do
  if ! "$BUILD_DIR/native-dom-fragment-test" "$mode"; then status=1; fi
done
exit "$status"
