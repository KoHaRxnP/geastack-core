#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out="$PWD/packages/geatsc-plugin-gea/dist"
test -d "$out" # use the normal package build output; never invent a scratch tree
export TMPDIR="$out"
source packages/core/test/native-host.sh
"${CXX:-clang++}" -std=c++20 -pthread -fsanitize=address,undefined \
  -ffunction-sections "${GEA_HOST_LINK_GC[@]}" ${GEA_HOST_CXXFLAGS[@]+"${GEA_HOST_CXXFLAGS[@]}"} \
  -Ipackages/host/include -Ipackages/core/include -Ipackages/engine \
  -Ipackages/engine/ui -Ipackages/elements -Ipackages/elements/ui \
  packages/core/test/test_worker_camera.cpp packages/host/host/camera.cpp \
  packages/host/host/worker.cpp -o "$out/test_worker_camera"
"$out/test_worker_camera"
