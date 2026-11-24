# Development

This page covers WhiskDB's architecture, dependencies, build and test commands, and repository layout.

---

## Architecture Overview

WhiskDB co-locates relational storage and sandboxed WebAssembly execution inside a single server process:

```text
                      +---------------------------------------+
                      |               Whisk CLI               |
                      |        (whisk sql, repl, call)        |
                      +-------------------+-------------------+
                                          | Binary TCP Framing (Asio)
                      +-------------------v-------------------+
                      |             WhiskDB Core              |
                      |                                       |
                      |  +---------------------------------+  |
                      |  |      In-Memory SQLite Core      |  |
                      |  |  (Relational Storage & Queries) |  |
                      |  +----------------+----------------+  |
                      |                   | In-Process ABI    |
                      |  +----------------v----------------+  |
                      |  |      WASM Guest Runtime         |  |
                      |  |   (Sandboxed Whisk Functions)   |  |
                      |  +----------------+----------------+  |
                      |                   |                   |
                      |  +----------------v----------------+  |
                      |  |       WAL & Storage Manager     |  |
                      |  |  (Strict / Batched / Ephemeral) |  |
                      |  +----------------+----------------+  |
                      +-------------------|-------------------+
                                          |
                               +----------v----------+
                               |     whiskdb.wal     |
                               |    snapshot.whisk   |
                               +---------------------+
```

### Subsystems

- **Relational Engine (`src/sql/`)**: In-memory SQLite3 C-amalgamation core with prepared statement caching, savepoint guards, and system catalog tables (`_whisk_modules`, `_whisk_migrations`).
- **WASM Runtime (`src/wasm/`)**: `libwasmtime` host with pre-compiled module caching, fuel metering, memory limits, and transactional host ABI imports (`whisk_db_*`).
- **Storage & Persistence (`src/storage/`)**: Binary WAL (`whiskdb.wal`) with CRC32 checksums, configurable durability modes, and point-in-time snapshots (`snapshot.whisk`).
- **Networking (`src/network/`)**: Standalone Asio non-blocking TCP server and client with length-prefixed binary framing.
- **CLI & SDK (`app/`, `src/cli/`, `include/sdk/`)**: The `whiskdb` server, `whisk` CLI/REPL, `whisk_bench` benchmark harness, and header-only `whisk_guest.hpp` SDK.

---

## Building & Testing

### Prerequisites (Ubuntu 24.04+ / Debian 13 `x86_64`)

Run the setup script to install CMake, Ninja, Clang/LLD (`wasm32` target), `clang-tidy`, and the Wasmtime C API:

```bash
./scripts/setup-dev.sh
```

### Build & Test Commands

Use `./scripts/macro.sh` (or standard CMake presets from `CMakePresets.json`):

| Command | Equivalent CMake Preset | Description |
|---|---|---|
| `./scripts/macro.sh build-debug` | `cmake --workflow --preset build-debug` | Configure and build Debug targets in `build/debug/`. |
| `./scripts/macro.sh build-release` | `cmake --workflow --preset build-release` | Configure and build Release targets in `build/release/`. |
| `./scripts/macro.sh run-tests` | `cmake --workflow --preset run-tests` | Build Debug and run all CTest suites. |
| `./scripts/macro.sh run-tests <regex>` | `ctest --test-dir build/debug -R <regex>` | Run test suites matching `<regex>` (e.g. `wal`, `sql`, `wasm`). |
| `./scripts/macro.sh bench` | — | Build Release and run `whisk_bench`. |
| `./scripts/macro.sh bench-compare [branch]` | — | Compare benchmark performance against a target branch. |
| `./scripts/macro.sh clean` | — | Remove `build/debug/` and `build/release/`. |

### Sanitizers (ASAN + UBSAN)

```bash
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWHISKDB_SANITIZE=ON
cmake --build build/debug
ASAN_OPTIONS=handle_segv=0:allow_user_segv_handler=1:detect_leaks=1 ctest --test-dir build/debug --output-on-failure
```

---

## External Libraries

CMake uses system-installed libraries when available and falls back to `FetchContent`:

| Library | Role |
|---|---|
| **[Asio](https://think-async.com/Asio/)** | Standalone asynchronous TCP networking (`ASIO_STANDALONE`). |
| **[SQLite3](https://www.sqlite.org/)** | Embedded in-memory relational SQL engine (C-amalgamation). |
| **[Wasmtime](https://wasmtime.dev/)** (`libwasmtime`) | Sandboxed WebAssembly runtime with fuel metering and module pre-compilation. |
| **[toml++](https://marzer.github.io/tomlplusplus/)** | Header-only TOML parser for `config.toml`. |

---

## Repository Layout

```text
app/          Executables (whiskdb server, whisk CLI, whisk_bench)
include/      Public headers (common, cli, client, network, sdk, sql, storage, wasm)
src/          Core library implementation
tests/        Unit, sandbox, and end-to-end test suites (CTest)
examples/     Sample C++23 Whisk modules
scripts/      Setup, build/test runner, demo, and benchmark scripts
docs/         MkDocs documentation source
config.toml   Default server configuration
```

For coding conventions and PR guidelines, see the [Developer Guide](developer-guide.md).
