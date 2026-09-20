#!/usr/bin/env bash
set -euo pipefail

TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "==> run-gea-counter-pipeline.sh"
"$TEST_DIR/run-gea-counter-pipeline.sh"

echo "==> run-gea-app-launcher-pipeline.sh"
"$TEST_DIR/run-gea-app-launcher-pipeline.sh"
