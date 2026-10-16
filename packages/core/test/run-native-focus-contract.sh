#!/usr/bin/env bash
set -euo pipefail
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-2}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/packages/core/test/.build/native-focus-contract"
CXX_BIN="${CXX:-clang++}"
source "$ROOT/packages/core/test/native-test-common.sh"
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/native-focus-contract" "$ROOT/packages/core/test/test_native_focus_contract.cpp"
"$BUILD_DIR/native-focus-contract"
