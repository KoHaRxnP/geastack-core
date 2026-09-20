#!/usr/bin/env bash
# Loop-boundary perf probe for css-3d-cube: pumps deterministic frames across two
# 10s cube-spin iterations and prints every frame that falls off the fast paths,
# plus the static-backdrop bake/drop lifecycle ([bdrop] lines).
#
# Knobs (env):
#   GEA_PROBE_STEP_MS=21       frame step (16 default; 21 ≈ device cadence)
#   GEA_PROBE_JITTER=1         deterministic ±4ms frame jitter (device-like fps text churn)
#   GEA_PROBE_QUIET=1          print [quiet] for frames with no display-list pass
#   GEA_PROBE_FORCE_TICK_MS=N  force a badge setText at t>=N ms (models a badge tick
#                              landing inside an eased keyframe pause — the trigger
#                              for the pre-fix backdrop-cache drop at loop wraps)
#   GEA_DEBUG_BACKDROP=1       per-frame gate/settle debug lines
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
exec "$ROOT/packages/core/test/run-gea-native-app-pipeline.sh" \
  "css-cube-loop-probe" \
  "apps/css-3d-cube" \
  "index.tsx" \
  "packages/core/test/test_gea_css_3d_cube_loop_probe_main.cpp"
