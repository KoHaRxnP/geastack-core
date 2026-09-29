#!/usr/bin/env bash
# Each optional edge must retain its original geometry with no runtime indexing.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/packages/core/test/.build"
CXX_BIN="${CXX:-clang++}"
mkdir -p "$BUILD_DIR"
printf 'void __gea_top_level() {}\n' > "$BUILD_DIR/program.cpp"
export TMPDIR="$BUILD_DIR" GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-2}"
source "$ROOT/packages/core/test/native-test-common.sh"
for mode in full horizontal vertical pixels percentages; do
  flags=()
  for edge in TOP RIGHT BOTTOM LEFT; do
    px=1; percent=1
    case "$mode" in
      horizontal) if [[ "$edge" == TOP || "$edge" == BOTTOM ]]; then px=0; fi; if [[ "$edge" != LEFT ]]; then percent=0; fi ;;
      vertical) if [[ "$edge" == LEFT || "$edge" == RIGHT ]]; then px=0; fi; if [[ "$edge" == LEFT ]]; then percent=0; fi ;;
      pixels) percent=0 ;;
      percentages) px=0 ;;
    esac
    flags+=("-DGEA_CSS_POSITION_${edge}=$px" "-DGEA_CSS_POSITION_${edge}_PERCENT=$percent")
  done
  binary="$BUILD_DIR/position-storage-$mode"
  gea_build_native_test "$BUILD_DIR" "$binary" "$ROOT/packages/core/test/test_position_storage_main.cpp" "${flags[@]}"
  "$binary" > "$binary.csv"
done
python3 - "$BUILD_DIR" <<'PY'
import csv, pathlib, sys
root = pathlib.Path(sys.argv[1])
def read(mode):
    return {row[0]: row[1:] for row in csv.reader((root / f'position-storage-{mode}.csv').open())}
reference = read('full')
assert len(reference) == 130
covered = set()
for mode, count in [('horizontal', 50), ('vertical', 80), ('pixels', 66), ('percentages', 64)]:
    actual = read(mode)
    assert len(actual) == count, (mode, len(actual))
    for key, geometry in actual.items():
        assert geometry == reference[key], (mode, key, geometry, reference[key])
    covered.update(actual)
    print(f'PASS: {mode}: {len(actual)} position cases match full storage')
assert covered == set(reference)
PY
