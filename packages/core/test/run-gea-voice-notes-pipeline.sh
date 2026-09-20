#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
# Idle screen content: status meter ("Ready"), note summary ("… pending"),
# and the action list ("Record" / "Menu").
export GEA_SMOKE_EXPECT="Ready,pending,Record,Menu"
exec "$ROOT/packages/core/test/run-gea-native-app-pipeline.sh" \
  "gea-voice-notes-pipeline" \
  "apps/voice-notes" \
  "index.tsx" \
  "packages/core/test/test_gea_app_smoke_main.cpp"
