#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out="$PWD/packages/geatsc-plugin-gea/dist"
test -d "$out"
export TMPDIR="$out"
source packages/core/test/native-host.sh
"${CXX:-clang++}" -std=c++20 -pthread -fsanitize=address,undefined -ffunction-sections "${GEA_HOST_LINK_GC[@]}" ${GEA_HOST_CXXFLAGS[@]+"${GEA_HOST_CXXFLAGS[@]}"} \
  -I"${GEA_COMPILER_RUNTIME:-../compiler/src/targets/cpp/runtime}" -Ipackages/host/include -Ipackages/core/include -Ipackages/engine -Ipackages/engine/ui -Ipackages/elements/ui \
  packages/core/test/test_audio_worklet.cpp packages/core/test/test_audio_worklet_bridge.cpp \
  packages/host/host/audio_worklet.cpp \
  packages/host/host/audio.cpp packages/host/host/media.cpp packages/host/host/worker.cpp \
  -o "$out/test_audio_worklet"
"$out/test_audio_worklet"
