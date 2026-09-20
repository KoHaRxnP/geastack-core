#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
exec "$ROOT/packages/core/test/run-gea-native-app-pipeline.sh" \
  "gea-stopwatch-pipeline" \
  "apps/stopwatch-jsx" \
  "index.tsx" \
  "packages/core/test/test_gea_stopwatch_main.cpp"
