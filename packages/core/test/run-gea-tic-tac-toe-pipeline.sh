#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
exec "$ROOT/packages/core/test/run-gea-native-app-pipeline.sh" \
  "gea-tic-tac-toe-pipeline" \
  "apps/tic-tac-toe" \
  "index.tsx" \
  "packages/core/test/test_gea_tic_tac_toe_main.cpp"
