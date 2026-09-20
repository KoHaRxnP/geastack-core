#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"

exec "$ROOT/packages/core/test/run-gea-native-app-pipeline.sh" \
  "gea-reactive-counter-fonts" \
  "apps/reactive-counter" \
  "index.tsx" \
  "packages/core/test/test_gea_reactive_counter_fonts_main.cpp" \
  --font-viewport-width 200 \
  --font-viewport-width 720 \
  --font-viewport-height 200 \
  --font-viewport-height 1440 \
  --font-device-pixel-ratio 2.0
