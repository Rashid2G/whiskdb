#!/usr/bin/env bash
# Usage: scripts/dev.sh <build-dir-name> [ctest-regex]
# Configures + builds + runs ctest in build/<build-dir-name> (Debug).
# Uses a local zig-based toolchain from ~/.whisk-tools when no system compiler exists.
set -euo pipefail
cd "$(dirname "$0")/.."
name="${1:-dev}"
filter="${2:-.}"
if [ -d "$HOME/.whisk-tools" ]; then
  export PATH="$HOME/.whisk-tools/bin:$PATH"
  export CC="${CC:-$HOME/.whisk-tools/wrap/zcc}" CXX="${CXX:-$HOME/.whisk-tools/wrap/zcxx}"
  TOOL_ARGS="-DCMAKE_AR=$HOME/.whisk-tools/wrap/zar -DCMAKE_RANLIB=$HOME/.whisk-tools/wrap/zranlib"
fi
cmake -S . -B "build/$name" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DFETCHCONTENT_BASE_DIR=$PWD/build/_deps ${TOOL_ARGS:-} ${WHISKDB_CMAKE_ARGS:-} >/dev/null
cmake --build "build/$name" 2>&1 | tail -n "${TAIL:-60}"
ctest --test-dir "build/$name" --output-on-failure -R "$filter" 2>&1 | tail -n "${TAIL:-60}"
