#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
exec "$ROOT/packages/core/test/run-gea-native-app-pipeline.sh" \
  "gea-watch-face-pipeline" \
  "apps/watch-face" \
  "index.tsx" \
  "packages/core/test/test_gea_watch_face_main.cpp"
