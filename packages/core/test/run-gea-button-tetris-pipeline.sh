#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
exec "$ROOT/packages/core/test/run-gea-native-app-pipeline.sh" \
  "gea-button-tetris-pipeline" \
  "apps/button-tetris" \
  "index.tsx" \
  "packages/core/test/test_gea_button_tetris_main.cpp" \
  --append-js "$ROOT/packages/core/test/fixtures/button-tetris-drop-driver.js" \
  --font-device-pixel-ratio 2
