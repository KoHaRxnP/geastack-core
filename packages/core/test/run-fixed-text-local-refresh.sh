#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/packages/core/test/.build/fixed-text-local-refresh"
CXX_BIN="${CXX:-clang++}"

mkdir -p "$BUILD_DIR"
source "$ROOT/packages/core/test/native-test-common.sh"

cat > "$BUILD_DIR/program.cpp" <<'CPP'
void __gea_top_level() {}
CPP

gea_build_native_test \
  "$BUILD_DIR" \
  "$BUILD_DIR/fixed-text-local-refresh" \
  "$ROOT/packages/core/test/test_fixed_text_local_refresh_main.cpp"

"$BUILD_DIR/fixed-text-local-refresh"
