#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 4 ]]; then
  echo "usage: $0 <build-name> <app-dir under the app project> <entry> <test-main> [build args...]" >&2
  exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_NAME="$1"
APP_DIR="$2"
ENTRY="$3"
TEST_MAIN="$4"
shift 4

BUILD_DIR="$ROOT/packages/core/test/.build/$BUILD_NAME"
CXX_BIN="${CXX:-clang++}"

source "$ROOT/packages/core/test/native-test-common.sh"
gea_require_app_project

mkdir -p "$BUILD_DIR"

node "$ROOT/packages/core/scripts/build-gea-vite-geatsc.mjs" \
  --app-dir "$GEA_APP_PROJECT/$APP_DIR" \
  --entry "$ENTRY" \
  --out-dir "$BUILD_DIR" \
  --gea-embedded-compat \
  --gea-ir-backend \
  --allow-any \
  "$@"

gea_build_native_test \
  "$BUILD_DIR" \
  "$BUILD_DIR/$BUILD_NAME-test" \
  "$ROOT/$TEST_MAIN"

"$BUILD_DIR/$BUILD_NAME-test"
