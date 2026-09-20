#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
# camera-studio boots into the full-screen viewfinder: the control panel
# ("gea camera" / CAP / REC chrome) is display:none until the viewfinder is
# tapped, so no text is visible at mount — assert on node count only. The
# native test host renders the <camera> leaf against the null platform camera
# stub (native_test_host.cpp); the app mounts 18 nodes today.
export GEA_SMOKE_MIN_NODES=15
unset GEA_SMOKE_EXPECT
exec "$ROOT/packages/core/test/run-gea-native-app-pipeline.sh" \
  "gea-camera-studio-pipeline" \
  "apps/camera-studio" \
  "index.tsx" \
  "packages/core/test/test_gea_app_smoke_main.cpp"
