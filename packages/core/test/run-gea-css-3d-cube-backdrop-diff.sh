#!/usr/bin/env bash
# Differential probe for the static-backdrop fast path on css-3d-cube: the same
# deterministic frame sequence with the backdrop cache present vs absent must
# present pixel-identical frames. See test_gea_css_3d_cube_backdrop_diff_main.cpp.
#
#   bash run-gea-css-3d-cube-backdrop-diff.sh              # fast path live
#   GEA_DIFF_NO_BACKDROP=1 bash run-...-diff.sh            # reference arm
#
# Knobs: GEA_DIFF_FRAMES, GEA_DIFF_STEP_MS, GEA_DIFF_DUMP, GEA_DIFF_DUMP_ROW.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
exec "$ROOT/packages/core/test/run-gea-native-app-pipeline.sh" \
  "css-cube-backdrop-diff" \
  "apps/css-3d-cube" \
  "index.tsx" \
  "packages/core/test/test_gea_css_3d_cube_backdrop_diff_main.cpp"
