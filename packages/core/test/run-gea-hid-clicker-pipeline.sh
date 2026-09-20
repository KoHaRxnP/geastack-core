#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
exec "$ROOT/packages/core/test/run-gea-native-app-pipeline.sh" \
  "gea-hid-clicker-pipeline" \
  "apps/hid-clicker" \
  "src/index.tsx" \
  "packages/core/test/test_gea_hid_clicker_main.cpp"
