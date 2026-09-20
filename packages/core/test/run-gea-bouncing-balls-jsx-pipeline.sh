#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
GEA_NATIVE_TEST_INCLUDE_HOST=0 exec "$ROOT/packages/core/test/run-gea-native-app-pipeline.sh" \
  "gea-bouncing-balls-jsx-pipeline" \
  "apps/bouncing-balls-jsx" \
  "index.tsx" \
  "packages/core/test/test_gea_bouncing_balls_jsx_main.cpp"
