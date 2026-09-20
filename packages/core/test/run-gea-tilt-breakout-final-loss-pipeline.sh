#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
exec "$ROOT/packages/core/test/run-gea-native-app-pipeline.sh" \
  "gea-tilt-breakout-final-loss-pipeline" \
  "apps/tilt-breakout" \
  "index.tsx" \
  "packages/core/test/test_gea_tilt_breakout_final_loss_main.cpp" \
  --append-js "$ROOT/packages/core/test/fixtures/tilt-breakout-final-loss-driver.js"
