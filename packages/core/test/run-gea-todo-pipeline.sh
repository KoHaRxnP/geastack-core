#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
exec "$ROOT/packages/core/test/run-gea-native-app-pipeline.sh" \
  "gea-todo-pipeline" \
  "apps/todo-jsx" \
  "index.tsx" \
  "packages/core/test/test_gea_todo_main.cpp" \
  --font-device-pixel-ratio 2
