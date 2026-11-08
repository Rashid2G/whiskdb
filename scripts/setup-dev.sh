#!/usr/bin/env bash
# scripts/setup-dev.sh: Automated and idempotent toolchain & dependency setup
# for WhiskDB on Ubuntu 24.04+ and Debian Trixie (x86_64).
#
# Supported environments:
#   - Ubuntu 24.04+ (Noble Numbat or newer) x86_64
#   - Debian 13+ (Trixie or newer) x86_64
#
# Usage:
#   ./scripts/setup-dev.sh [options]
#
# Options:
#   --skip-build              Skip debug build verification at the end
#   --skip-tests              Skip running ctest test suite at the end
#   --prefix <dir>            Installation prefix for Wasmtime (default: /usr/local)
#   --wasmtime-version <ver>  Wasmtime release version (default: v50.0.1)
#   --help, -h                Show this help message

set -euo pipefail

# Ensure ~/.whisk-tools/bin and ~/.local/bin are in PATH if available
if [ -d "$HOME/.whisk-tools/bin" ]; then
  export PATH="$HOME/.whisk-tools/bin:$PATH"
fi
if [ -d "$HOME/.local/bin" ]; then
  export PATH="$HOME/.local/bin:$PATH"
fi

# Locate repository root
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# Configuration defaults
WASMTIME_VERSION="${WASMTIME_VERSION:-v50.0.1}"
INSTALL_PREFIX="${INSTALL_PREFIX:-/usr/local}"
SKIP_BUILD=false
SKIP_TESTS=false

# -----------------------------------------------------------------------------
# 1. Parse Command Line Arguments
# -----------------------------------------------------------------------------
parse_args() {
  while [ $# -gt 0 ]; do
    case "$1" in
      --skip-build)
        SKIP_BUILD=true
        shift
        ;;
      --skip-tests)
        SKIP_TESTS=true
        shift
        ;;
      --prefix)
        INSTALL_PREFIX="$2"
        shift 2
        ;;
      --wasmtime-version)
        WASMTIME_VERSION="$2"
        shift 2
        ;;
      -h|--help)
        cat <<'EOF'
WhiskDB Development Setup Script:
Usage: scripts/setup-dev.sh [options]

Supported OS:
  - Ubuntu 24.04+ (x86_64)
  - Debian 13 (Trixie) (x86_64)

Options:
  --skip-build              Skip the post-setup Debug build verification
  --skip-tests              Skip the post-setup CTest test suite run
  --prefix <dir>            Wasmtime C API install prefix (default: /usr/local)
  --wasmtime-version <ver>  Wasmtime C API release version (default: v50.0.1)
  -h, --help                Show this help message
EOF
        exit 0
        ;;
      *)
        echo "Unknown option: $1" >&2
        echo "Run '$0 --help' for usage." >&2
        exit 1
        ;;
    esac
  done
}

# -----------------------------------------------------------------------------
# 2. OS & Architecture Detection
# -----------------------------------------------------------------------------
detect_os() {
  local arch
  arch="$(uname -m)"
  if [ "$arch" != "x86_64" ]; then
    echo "Error: WhiskDB setup currently only supports x86_64 architecture (detected: $arch)." >&2
    exit 1
  fi

  if [ ! -f /etc/os-release ]; then
    echo "Error: Cannot identify operating system (/etc/os-release not found)." >&2
    exit 1
  fi

  # shellcheck source=/dev/null
  . /etc/os-release

  local os_id="${ID:-}"
  local os_version_id="${VERSION_ID:-}"
  local os_codename="${VERSION_CODENAME:-}"

  local supported=false
  if [ "$os_id" = "ubuntu" ]; then
    local major="${os_version_id%%.*}"
    if [ -n "$major" ] && [ "$major" -ge 24 ] 2>/dev/null; then
      supported=true
    elif [ "$os_codename" = "noble" ]; then
      supported=true
    fi
  elif [ "$os_id" = "debian" ]; then
    local major="${os_version_id%%.*}"
    if [ "$os_codename" = "trixie" ] || { [ -n "$major" ] && [ "$major" -ge 13 ] 2>/dev/null; }; then
      supported=true
    fi
  fi

  if [ "$supported" != "true" ]; then
    echo "Error: Unsupported distribution: ${PRETTY_NAME:-$os_id} (ID: $os_id, Version: $os_version_id, Codename: $os_codename)." >&2
    echo "Supported distributions are Ubuntu 24.04+ and Debian 13 (Trixie) on x86_64." >&2
    exit 1
  fi

  echo "==> Detected supported OS: ${PRETTY_NAME:-$os_id} ($arch)"
}

# -----------------------------------------------------------------------------
# 3. Privileged Command Helper
# -----------------------------------------------------------------------------
can_run_privileged() {
  if [ "$(id -u)" -eq 0 ]; then
    return 0
  fi
  if command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
    return 0
  fi
  return 1
}

run_privileged() {
  if [ "$(id -u)" -eq 0 ]; then
    "$@"
  elif command -v sudo >/dev/null 2>&1; then
    sudo "$@"
  else
    echo "Error: Sudo or root privileges required to run: $*" >&2
    return 1
  fi
}

# -----------------------------------------------------------------------------
# 4. Package Verification & Installation
# -----------------------------------------------------------------------------
install_packages() {
  echo "==> Checking required system packages..."

  local required_pkgs=(
    cmake
    ninja-build
    clang
    lld
    clang-tidy
    libsqlite3-dev
    libasio-dev
    curl
    tar
    xz-utils
    ca-certificates
  )

  local missing_pkgs=()
  for pkg in "${required_pkgs[@]}"; do
    if dpkg -s "$pkg" 2>/dev/null | grep -q "Status: install ok installed"; then
      continue
    fi
    missing_pkgs+=("$pkg")
  done

  if [ ${#missing_pkgs[@]} -eq 0 ]; then
    echo "==> All required system packages are already installed."
    return 0
  fi

  echo "==> Missing packages to install: ${missing_pkgs[*]}"
  if [ "$(id -u)" -ne 0 ] && ! command -v sudo >/dev/null 2>&1; then
    echo "Warning: Sudo is not available; checking whether toolchain binaries already exist..."
    if command -v cmake >/dev/null 2>&1 && command -v ninja >/dev/null 2>&1 && command -v clang >/dev/null 2>&1; then
      echo "==> Required binaries found in PATH, skipping system package installation."
      return 0
    else
      echo "Error: Cannot install packages without root or sudo." >&2
      exit 1
    fi
  fi

  echo "==> Updating apt repositories..."
  if ! run_privileged apt-get update -y; then
    echo "Warning: apt-get update failed or was not permitted."
  fi

  echo "==> Installing missing packages..."
  if ! run_privileged apt-get install -y --no-install-recommends "${missing_pkgs[@]}"; then
    echo "Warning: apt-get install encountered an error; checking if fallback toolchain is functional..."
    if command -v cmake >/dev/null 2>&1 && command -v ninja >/dev/null 2>&1; then
      echo "==> Basic build tools are available. Continuing setup."
    else
      echo "Error: Failed to install required packages and no fallback tools found." >&2
      exit 1
    fi
  fi

  echo "==> Package verification completed."
}

# -----------------------------------------------------------------------------
# 5. Wasmtime C API Download & Installation
# -----------------------------------------------------------------------------
find_installed_wasmtime() {
  local search_prefixes=(
    "$INSTALL_PREFIX"
    "/usr/local"
    "/usr"
    "$HOME/.local"
    "$HOME/.whisk-tools"
  )

  for prefix in "${search_prefixes[@]}"; do
    if [ -f "$prefix/include/wasmtime.h" ] && [ -f "$prefix/include/wasm.h" ]; then
      if [ -f "$prefix/lib/libwasmtime.a" ] || [ -f "$prefix/lib/libwasmtime.so" ] || \
         [ -f "$prefix/lib64/libwasmtime.a" ] || [ -f "$prefix/lib64/libwasmtime.so" ]; then
        echo "$prefix"
        return 0
      fi
    fi
  done
  return 1
}

install_wasmtime() {
  echo "==> Checking Wasmtime C API installation..."

  local existing_prefix
  if existing_prefix="$(find_installed_wasmtime)"; then
    echo "==> Wasmtime C API is already installed at: $existing_prefix"
    return 0
  fi

  # Determine writable target directory
  local target_prefix="$INSTALL_PREFIX"
  local use_privileged=false
  if [ -w "$target_prefix" ]; then
    use_privileged=false
  elif can_run_privileged; then
    use_privileged=true
  else
    echo "Notice: $target_prefix is not writable and sudo is unavailable."
    echo "Falling back to user-local directory: $HOME/.local"
    target_prefix="$HOME/.local"
    use_privileged=false
  fi

  local asset_name="wasmtime-${WASMTIME_VERSION}-x86_64-linux-c-api.tar.xz"
  local tmp_dir
  tmp_dir="$(mktemp -d -t whiskdb-wasmtime-XXXXXX)"
  trap 'rm -rf "$tmp_dir"' EXIT

  local tar_path="$tmp_dir/$asset_name"

  echo "==> Downloading Wasmtime C API ($WASMTIME_VERSION)..."
  # Check if pre-cached in /tmp
  if [ -f "/tmp/$asset_name" ] && tar -tf "/tmp/$asset_name" >/dev/null 2>&1; then
    echo "==> Using cached archive from /tmp/$asset_name"
    tar_path="/tmp/$asset_name"
  elif command -v gh >/dev/null 2>&1 && gh release download "$WASMTIME_VERSION" -R bytecodealliance/wasmtime -p "wasmtime-*-x86_64-linux-c-api.tar.xz" --dir "$tmp_dir" 2>/dev/null; then
    tar_path="$tmp_dir/$asset_name"
  elif command -v curl >/dev/null 2>&1; then
    local download_url="https://github.com/bytecodealliance/wasmtime/releases/download/${WASMTIME_VERSION}/${asset_name}"
    if ! curl -fsSL --retry 3 --connect-timeout 15 "$download_url" -o "$tar_path" 2>/dev/null; then
      echo "Notice: Direct release download timed out or was blocked. Trying GitHub API..."
      local asset_id
      asset_id="$(curl -s "https://api.github.com/repos/bytecodealliance/wasmtime/releases/tags/${WASMTIME_VERSION}" | grep -B 2 -A 3 "${asset_name}" | grep '"id":' | head -n 1 | tr -dc '0-9')"
      if [ -n "$asset_id" ]; then
        curl -sL -H "Accept: application/octet-stream" "https://api.github.com/repos/bytecodealliance/wasmtime/releases/assets/${asset_id}" -o "$tar_path"
      fi
    fi
  fi

  if [ ! -f "$tar_path" ] || ! tar -tf "$tar_path" >/dev/null 2>&1; then
    echo "Error: Failed to download a valid Wasmtime C API archive." >&2
    exit 1
  fi

  echo "==> Extracting Wasmtime C API archive..."
  tar -xf "$tar_path" -C "$tmp_dir"

  local extracted_dir="$tmp_dir/wasmtime-${WASMTIME_VERSION}-x86_64-linux-c-api"
  if [ ! -d "$extracted_dir" ]; then
    echo "Error: Expected extracted directory not found: $extracted_dir" >&2
    exit 1
  fi

  echo "==> Installing Wasmtime C API to $target_prefix..."
  if [ "$use_privileged" = "true" ]; then
    run_privileged mkdir -p "$target_prefix/include" "$target_prefix/lib"
    run_privileged cp -r "$extracted_dir"/include/* "$target_prefix/include/"
    run_privileged cp -P "$extracted_dir"/lib/* "$target_prefix/lib/"
    if command -v ldconfig >/dev/null 2>&1 && [ "$target_prefix" = "/usr/local" ]; then
      run_privileged ldconfig 2>/dev/null || true
    fi
  else
    mkdir -p "$target_prefix/include" "$target_prefix/lib"
    cp -r "$extracted_dir"/include/* "$target_prefix/include/"
    cp -P "$extracted_dir"/lib/* "$target_prefix/lib/"
  fi

  # Verify installation
  if [ -f "$target_prefix/include/wasmtime.h" ] && { [ -f "$target_prefix/lib/libwasmtime.a" ] || [ -f "$target_prefix/lib/libwasmtime.so" ]; }; then
    echo "==> Wasmtime C API ($WASMTIME_VERSION) successfully installed at $target_prefix."
  else
    echo "Error: Wasmtime C API installation verification failed at $target_prefix." >&2
    exit 1
  fi

  rm -rf "$tmp_dir"
  trap - EXIT
}

# -----------------------------------------------------------------------------
# 6. Verify clang++ --target=wasm32 Compilation
# -----------------------------------------------------------------------------
verify_wasm_compiler() {
  echo "==> Verifying clang++ --target=wasm32 compilation capability..."

  if ! command -v clang++ >/dev/null 2>&1; then
    echo "Error: clang++ compiler executable not found in PATH." >&2
    exit 1
  fi

  local tmp_dir
  tmp_dir="$(mktemp -d -t whiskdb-wasm-verify-XXXXXX)"
  trap 'rm -rf "$tmp_dir"' EXIT

  local src_file="$tmp_dir/test_wasm.cpp"
  local out_file="$tmp_dir/test_wasm.o"

  cat << 'EOF' > "$src_file"
extern "C" int whiskdb_wasm_verify(int a, int b) {
    return a + b;
}
EOF

  if ! clang++ --target=wasm32 -c "$src_file" -o "$out_file" >/dev/null 2>&1; then
    echo "Error: clang++ failed to compile C++ source targeting wasm32." >&2
    rm -rf "$tmp_dir"
    exit 1
  fi

  if [ ! -f "$out_file" ]; then
    echo "Error: Expected wasm object file was not generated: $out_file" >&2
    rm -rf "$tmp_dir"
    exit 1
  fi

  # Verify WebAssembly binary magic bytes (0x00 0x61 0x73 0x6d / '\0asm')
  local magic
  magic="$(od -An -N 4 -t x1 "$out_file" | tr -d ' \n')"
  if [ "$magic" != "0061736d" ]; then
    echo "Error: Compiled object does not contain valid WebAssembly magic bytes (got: $magic)." >&2
    rm -rf "$tmp_dir"
    exit 1
  fi

  rm -rf "$tmp_dir"
  trap - EXIT

  echo "==> Verified clang++ --target=wasm32 compilation capability successfully."
}

# -----------------------------------------------------------------------------
# 7. Verification Build & Test Execution
# -----------------------------------------------------------------------------
verify_build_and_tests() {
  if [ "$SKIP_BUILD" = "true" ]; then
    echo "==> Skipping build verification (--skip-build requested)."
  else
    echo "==> Running verification build (cmake build-debug)..."
    cmake build-debug
  fi

  if [ "$SKIP_TESTS" = "true" ]; then
    echo "==> Skipping test suite verification (--skip-tests requested)."
  else
    echo "==> Running verification test suite (cmake run-tests)..."
    cmake run-tests
  fi
}

main() {
  parse_args "$@"
  detect_os
  install_packages
  install_wasmtime
  verify_wasm_compiler
  verify_build_and_tests
  echo "==> WhiskDB developer environment setup completed successfully!"
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
  main "$@"
fi
