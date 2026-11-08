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

set -euo pipefail

# Ensure ~/.whisk-tools/bin is in PATH if available
if [ -d "$HOME/.whisk-tools/bin" ]; then
  export PATH="$HOME/.whisk-tools/bin:$PATH"
fi
if [ -d "$HOME/.local/bin" ]; then
  export PATH="$HOME/.local/bin:$PATH"
fi

# Locate repository root
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# -----------------------------------------------------------------------------
# 1. OS & Architecture Detection
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
# 2. Privileged Command Helper
# -----------------------------------------------------------------------------
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
# 3. Package Verification & Installation
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

main() {
  detect_os
  install_packages
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
  main "$@"
fi
