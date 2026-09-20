#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/packages/core/test/.build/class-recompute-local-paint"
CXX_BIN="${CXX:-clang++}"

mkdir -p "$BUILD_DIR"
source "$ROOT/packages/core/test/native-test-common.sh"

cat > "$BUILD_DIR/program.cpp" <<'CPP'
void __gea_top_level() {}
CPP

gea_build_native_test \
  "$BUILD_DIR" \
  "$BUILD_DIR/class-recompute-local-paint" \
  "$ROOT/packages/core/test/test_class_recompute_local_paint_main.cpp"

"$BUILD_DIR/class-recompute-local-paint"
