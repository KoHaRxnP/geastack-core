#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
exec "$ROOT/packages/core/test/run-gea-native-app-pipeline.sh" \
  "gea-watch-analog-pipeline" \
  "apps/watch-analog" \
  "index.tsx" \
  "packages/core/test/test_gea_watch_analog_main.cpp"
