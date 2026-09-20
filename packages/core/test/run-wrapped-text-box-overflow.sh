#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/packages/core/test/.build/wrapped-text-box-overflow"
CXX_BIN="${CXX:-clang++}"

mkdir -p "$BUILD_DIR"
source "$ROOT/packages/core/test/native-test-common.sh"

# No app is compiled for this test: the tree is built through the C++ Document
# API directly. Stand in for the geatsc-generated translation units with a
# single stub TU and the source list gea_build_native_test reads.
cat > "$BUILD_DIR/program.cpp" <<'CPP'
void __gea_top_level() {}
CPP
printf '%s\n' "$BUILD_DIR/program.cpp" > "$BUILD_DIR/geatsc-sources.txt"

gea_build_native_test \
  "$BUILD_DIR" \
  "$BUILD_DIR/wrapped-text-box-overflow" \
  "$ROOT/packages/core/test/test_wrapped_text_box_overflow_main.cpp"

"$BUILD_DIR/wrapped-text-box-overflow"
