#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
exec "$ROOT/packages/core/test/run-gea-native-app-pipeline.sh" \
  "gea-typography-pipeline" \
  "apps/typography" \
  "index.tsx" \
  "packages/core/test/test_gea_typography_main.cpp"
