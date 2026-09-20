#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
exec "$ROOT/packages/core/test/run-gea-native-app-pipeline.sh" \
  "gea-bouncing-balls-pipeline" \
  "apps/bouncing-balls" \
  "index.ts" \
  "packages/core/test/test_gea_bouncing_balls_main.cpp"
