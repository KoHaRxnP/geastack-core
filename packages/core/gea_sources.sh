#!/usr/bin/env bash
# Shell front end for the framework source manifest. The manifest itself now
# lives in gea_sources.mjs — CMake has to read it on Windows too, where there is
# no bash, so `execute_process(COMMAND bash -c ...)` failed and no ESP32 target
# could configure. Node is already a hard requirement of every path that gets
# here (the CLI and geatsc are Node), so reading it from Node costs no new
# dependency, and keeping these three function names means shell callers that
# `source` this file (simulator build-web.sh, core's own test) do not change.
#
# Caller must export GEA_CORE = the @geastack/core package dir, plus
# GEA_HOST_DIR / GEA_ENGINE_DIR / GEA_ELEMENTS_DIR / GEA_GEAOS_PACKAGE_DIR.
# gea_sources.mjs names any that are missing and exits non-zero.

# The manifest sits next to this file, so locate it from BASH_SOURCE rather than
# from GEA_CORE: sourcing still works when a caller points GEA_CORE at a
# workspace symlink whose spelling differs from this checkout's.
__GEA_SOURCES_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Run from that directory and name the manifest relatively: under Git Bash a
# native node.exe would otherwise receive a POSIX path, which only works while
# MSYS path conversion is enabled and turns into `C:\c\Users\...` when it is not.
__gea_sources_run() {
  ( cd "$__GEA_SOURCES_DIR" && node ./gea_sources.mjs "$1" )
}

gea_fw_include_flags() {
  __gea_sources_run include-flags
}

gea_fw_c_sources() {
  __gea_sources_run c-sources
}

gea_fw_cxx_sources() {
  __gea_sources_run cxx-sources
}
