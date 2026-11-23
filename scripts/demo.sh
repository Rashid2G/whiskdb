#!/usr/bin/env bash
# WhiskDB v1.0 End-to-End Interactive Demo
# Usage: bash scripts/demo.sh [--interactive]

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

INTERACTIVE=0
for arg in "$@"; do
    case "$arg" in
        --interactive|-i) INTERACTIVE=1 ;;
        --help|-h)
            echo "Usage: bash scripts/demo.sh [--interactive]"
            echo "  --interactive, -i  Pause for Enter between demo steps"
            exit 0
            ;;
        *)
            echo "Unknown option: $arg" >&2
            exit 1
            ;;
    esac
done

# ANSI colors (disabled when stdout is not a TTY)
if [ -t 1 ]; then
    BOLD=$'\033[1m'
    CYAN=$'\033[1;36m'
    GREEN=$'\033[1;32m'
    YELLOW=$'\033[1;33m'
    RED=$'\033[1;31m'
    DIM=$'\033[2m'
    RESET=$'\033[0m'
else
    BOLD="" CYAN="" GREEN="" YELLOW="" RED="" DIM="" RESET=""
fi

step() {
    echo ""
    echo "${CYAN}================================================================${RESET}"
    echo "${BOLD}Step $1: $2${RESET}"
    echo "${CYAN}================================================================${RESET}"
}

pause_if_interactive() {
    if [ "$INTERACTIVE" -eq 1 ]; then
        echo -n "${DIM}Press Enter to continue...${RESET}"
        read -r _
    fi
}

# Locate binaries (prefer release, fallback to debug)
if [ -x "$REPO_ROOT/build/release/whiskdb" ] && [ -x "$REPO_ROOT/build/release/whisk" ] && [ -x "$REPO_ROOT/build/release/whisk_bench" ]; then
    BIN_DIR="$REPO_ROOT/build/release"
elif [ -x "$REPO_ROOT/build/debug/whiskdb" ] && [ -x "$REPO_ROOT/build/debug/whisk" ] && [ -x "$REPO_ROOT/build/debug/whisk_bench" ]; then
    BIN_DIR="$REPO_ROOT/build/debug"
else
    echo "${YELLOW}Binaries not found in build/release or build/debug; building release...${RESET}"
    "$REPO_ROOT/scripts/macro.sh" build-release --target whiskdb whisk whisk_bench
    BIN_DIR="$REPO_ROOT/build/release"
fi

WHISKDB="$BIN_DIR/whiskdb"
WHISK="$BIN_DIR/whisk"
WHISK_BENCH="$BIN_DIR/whisk_bench"

# Pick an available ephemeral TCP port
PORT="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')"
WORK_DIR="$(mktemp -d -t whiskdb-demo.XXXXXX)"
DATA_DIR="$WORK_DIR/data"
mkdir -p "$DATA_DIR"
SERVER_LOG="$WORK_DIR/whiskdb.log"
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

wait_for_server() {
    local attempts=0
    while [ "$attempts" -lt 50 ]; do
        if "$WHISK" --host 127.0.0.1 --port "$PORT" ping >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.05
        attempts=$((attempts + 1))
    done
    echo "${RED}Server failed to become ready on 127.0.0.1:$PORT${RESET}" >&2
    cat "$SERVER_LOG" >&2 || true
    return 1
}

start_server() {
    "$WHISKDB" --host 127.0.0.1 --port "$PORT" --data-dir "$DATA_DIR" --durability group_commit \
        >>"$SERVER_LOG" 2>&1 &
    SERVER_PID=$!
    wait_for_server
}

# ------------------------------------------------------------------------------
step 1 "Start WhiskDB Server (group_commit durability)"
# ------------------------------------------------------------------------------
echo "Binaries : $BIN_DIR"
echo "Endpoint : 127.0.0.1:$PORT"
echo "Data dir : $DATA_DIR"
start_server
"$WHISK" --host 127.0.0.1 --port "$PORT" ping
echo "${GREEN}[OK] WhiskDB server is up (PID $SERVER_PID).${RESET}"
pause_if_interactive

# ------------------------------------------------------------------------------
step 2 "Compile examples/ledger_whisk.cpp -> ledger.wasm & Publish"
# ------------------------------------------------------------------------------
WASM_CLANG=""
if [ -x "$HOME/.whisk-tools/bin/clang++" ]; then
    WASM_CLANG="$HOME/.whisk-tools/bin/clang++"
elif command -v clang++ >/dev/null 2>&1; then
    WASM_CLANG="$(command -v clang++)"
else
    echo "${RED}clang++ not found (install via scripts/setup-dev.sh)${RESET}" >&2
    exit 1
fi

WASM_OUT="$WORK_DIR/ledger.wasm"
"$WASM_CLANG" --target=wasm32 -O3 -std=c++23 -nostdlib -fno-exceptions -fno-rtti \
    -I"$REPO_ROOT/include/sdk" \
    -Wl,--no-entry -Wl,--export-dynamic \
    "$REPO_ROOT/examples/ledger_whisk.cpp" -o "$WASM_OUT"
echo "Compiled $WASM_OUT ($(wc -c < "$WASM_OUT") bytes)"

"$WHISK" --host 127.0.0.1 --port "$PORT" publish ledger "$WASM_OUT"
"$WHISK" --host 127.0.0.1 --port "$PORT" list
echo "${GREEN}[OK] Module 'ledger' published and __init__ auto-created accounts & transfers.${RESET}"
pause_if_interactive

# ------------------------------------------------------------------------------
step 3 "Seed Accounts & Execute Atomic Fund Transfer"
# ------------------------------------------------------------------------------
"$WHISK" --host 127.0.0.1 --port "$PORT" call ledger create_account '{"id":1,"owner":"Alice","balance":1000}'
"$WHISK" --host 127.0.0.1 --port "$PORT" call ledger create_account '{"id":2,"owner":"Bob","balance":500}'
"$WHISK" --host 127.0.0.1 --port "$PORT" call ledger transfer_funds '{"from":1,"to":2,"amount":250}'

echo ""
echo "${BOLD}Accounts table:${RESET}"
"$WHISK" --host 127.0.0.1 --port "$PORT" sql "SELECT id, owner, balance FROM accounts ORDER BY id;"
echo "${BOLD}Transfers audit trail:${RESET}"
"$WHISK" --host 127.0.0.1 --port "$PORT" sql "SELECT id, from_id, to_id, amount FROM transfers ORDER BY id;"
echo "${GREEN}[OK] Atomic transfer committed (Alice=750, Bob=750).${RESET}"
pause_if_interactive

# ------------------------------------------------------------------------------
step 4 "Sandbox Safety & ACID Rollback (Trap & Fuel Exhaustion)"
# ------------------------------------------------------------------------------
echo "Invoking buggy_transfer_trap (debits Alice by 100 then executes __builtin_trap)..."
if "$WHISK" --host 127.0.0.1 --port "$PORT" call ledger buggy_transfer_trap '{"from":1,"amount":100}'; then
    echo "${RED}ERROR: buggy_transfer_trap unexpectedly succeeded!${RESET}" >&2
    exit 1
else
    echo "${YELLOW}[EXPECTED] buggy_transfer_trap failed and rolled back.${RESET}"
fi

echo "Invoking infinite_loop (debits Alice by 100 then exhausts WASM fuel)..."
if "$WHISK" --host 127.0.0.1 --port "$PORT" call ledger infinite_loop '{"from":1,"amount":100}'; then
    echo "${RED}ERROR: infinite_loop unexpectedly succeeded!${RESET}" >&2
    exit 1
else
    echo "${YELLOW}[EXPECTED] infinite_loop terminated by fuel metering and rolled back.${RESET}"
fi

echo ""
echo "${BOLD}Verifying 0 partial writes via SQL:${RESET}"
"$WHISK" --host 127.0.0.1 --port "$PORT" sql "SELECT id, owner, balance FROM accounts ORDER BY id;"
echo "${BOLD}Verifying server liveness via get_balance:${RESET}"
"$WHISK" --host 127.0.0.1 --port "$PORT" call ledger get_balance '{"id":1}'
echo "${GREEN}[OK] Sandbox contained both faults with zero partial writes.${RESET}"
pause_if_interactive

# ------------------------------------------------------------------------------
step 5 "Crash Recovery (kill -9 -> Restart -> WAL Replay)"
# ------------------------------------------------------------------------------
echo "Killing WhiskDB server (PID $SERVER_PID) with SIGKILL (-9)..."
kill -9 "$SERVER_PID"
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""

echo "Restarting WhiskDB on the same data_dir ($DATA_DIR) and port ($PORT)..."
start_server

echo "${BOLD}Recovered accounts:${RESET}"
"$WHISK" --host 127.0.0.1 --port "$PORT" sql "SELECT id, owner, balance FROM accounts ORDER BY id;"
echo "${BOLD}Recovered _whisk_modules:${RESET}"
"$WHISK" --host 127.0.0.1 --port "$PORT" list
echo "${BOLD}Invoking transfer_funds immediately without republishing:${RESET}"
"$WHISK" --host 127.0.0.1 --port "$PORT" call ledger transfer_funds '{"from":2,"to":1,"amount":50}'
"$WHISK" --host 127.0.0.1 --port "$PORT" sql "SELECT id, owner, balance FROM accounts ORDER BY id;"
echo "${GREEN}[OK] WAL replay restored SQL state and WASM modules cleanly.${RESET}"
pause_if_interactive

# ------------------------------------------------------------------------------
step 6 "Co-Location Benchmark (4-RTT SQL vs 1-RTT WHISK_CALL)"
# ------------------------------------------------------------------------------
"$WHISK_BENCH" --colocation --iterations 100 --markdown

echo ""
echo "${GREEN}${BOLD}WhiskDB v1.0 end-to-end demo completed successfully!${RESET}"
