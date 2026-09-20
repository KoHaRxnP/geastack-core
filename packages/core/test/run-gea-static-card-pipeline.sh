#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
exec "$ROOT/packages/core/test/run-gea-native-app-pipeline.sh" \
  "gea-static-card-pipeline" \
  "apps/static-card" \
  "index.tsx" \
  "packages/core/test/test_gea_static_card_main.cpp"
