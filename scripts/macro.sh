#!/usr/bin/env bash
# WhiskDB Script & Macro Runner
# Usage: scripts/macro.sh <command> [args...]
# Supported macros:
#   build-debug   - Configure & build Debug target (build/debug)
#   build-release - Configure & build Release target (build/release)
#   run-tests     - Build and run all CTest suites with failure diagnostics
#   clean         - Clean build artifacts
#
# To add a new macro, add a new handler function below and register it in the case statement.

set -euo pipefail

# Locate repository root
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# Ensure ~/.whisk-tools toolchain is active if available
if [ -d "$HOME/.whisk-tools" ]; then
  export PATH="$HOME/.whisk-tools/bin:$PATH"
  export CC="${CC:-$HOME/.whisk-tools/wrap/zcc}"
  export CXX="${CXX:-$HOME/.whisk-tools/wrap/zcxx}"
  TOOL_ARGS="-DCMAKE_AR=$HOME/.whisk-tools/wrap/zar -DCMAKE_RANLIB=$HOME/.whisk-tools/wrap/zranlib"
else
  TOOL_ARGS=""
fi

# Detect CMake binary
CMAKE_BIN="$(command -v cmake || echo "$HOME/.whisk-tools/bin/cmake")"
CTEST_BIN="$(command -v ctest || echo "$HOME/.whisk-tools/bin/ctest")"

configure_if_needed() {
  local build_type="$1"
  local build_dir="build/${build_type,,}"

  if [ ! -f "$build_dir/build.ninja" ] && [ ! -f "$build_dir/Makefile" ]; then
    echo "==> Configuring WhiskDB ($build_type) in $build_dir..."
    mkdir -p "$build_dir"
    "$CMAKE_BIN" -S . -B "$build_dir" -G Ninja \
      -DCMAKE_BUILD_TYPE="$build_type" \
      -DFETCHCONTENT_BASE_DIR="$REPO_ROOT/build/_deps" \
      ${TOOL_ARGS} \
      ${WHISKDB_CMAKE_ARGS:-}
  fi
}

cmd_build_debug() {
  configure_if_needed "Debug"
  echo "==> Building WhiskDB (Debug)..."
  "$CMAKE_BIN" --build "build/debug" "$@"
}

cmd_build_release() {
  configure_if_needed "Release"
  echo "==> Building WhiskDB (Release)..."
  "$CMAKE_BIN" --build "build/release" "$@"
}

cmd_run_tests() {
  cmd_build_debug
  echo "==> Running WhiskDB Test Suite..."
  local filter="${1:-.}"
  shift || true
  "$CTEST_BIN" --test-dir "build/debug" --output-on-failure -R "$filter" "$@"
}

cmd_bench() {
  cmd_build_release
  echo "==> Running WhiskDB Persistence Benchmark..."
  "build/release/whisk_bench" "$@"
}

cmd_bench_compare() {
  local target="${1:-master}"
  shift || true
  python3 "$REPO_ROOT/scripts/benchmark_branch.py" --target "$target" "$@"
}

cmd_clean() {
  echo "==> Cleaning build artifacts..."
  rm -rf build/debug build/release
  echo "Clean complete."
}

cmd_setup_dev() {
  echo "==> Running WhiskDB developer environment setup..."
  "$REPO_ROOT/scripts/setup-dev.sh" "$@"
}

cmd_help() {
  cat <<'EOF'
WhiskDB Macro Commands:
  build-debug    Configure & build in Debug mode (build/debug)
  build-release  Configure & build in Release mode (build/release)
  run-tests      Build Debug & execute all ctest suites with diagnostics
  bench          Build Release & run WhiskDB persistence benchmark
  bench-compare  Run benchmark comparison against specified branch (default: master)
  setup-dev      Set up developer environment dependencies and toolchains
  clean          Remove build directories

Adding New Macros:
  Define a `cmd_<name>()` function in `scripts/macro.sh` and add the
  subcommand pattern to the entry-point case switch below.
EOF
}

SUBCOMMAND="${1:-help}"
shift || true

case "$SUBCOMMAND" in
  build-debug|debug)
    cmd_build_debug "$@"
    ;;
  build-release|release)
    cmd_build_release "$@"
    ;;
  run-tests|test|tests)
    cmd_run_tests "$@"
    ;;
  bench|benchmark)
    cmd_bench "$@"
    ;;
  bench-compare|benchmark-compare)
    cmd_bench_compare "$@"
    ;;
  setup-dev|setup)
    cmd_setup_dev "$@"
    ;;
  clean)
    cmd_clean "$@"
    ;;
  help|--help|-h)
    cmd_help
    ;;
  *)
    echo "Unknown macro command: '$SUBCOMMAND'"
    echo "Run 'cmake help' or 'scripts/macro.sh help' for available commands."
    exit 1
    ;;
esac
