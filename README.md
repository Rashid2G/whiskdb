# WhiskDB

> In-memory relational database with co-located WebAssembly execution and configurable WAL persistence.

WhiskDB is an in-memory relational database and WebAssembly application host written in **C++23** for Linux `x86_64`. It pairs an in-memory SQLite3 engine with sandboxed server-side WebAssembly functions (**Whisks**, powered by `libwasmtime`) in the same memory space.

---

## Key Features

- **In-Memory SQL Engine**: Embedded SQLite3 C-amalgamation core with ANSI SQL support and prepared statement caching.
- **Sandboxed WASM Functions (Whisks)**: Pre-compiled `libwasmtime` modules with memory isolation, fuel metering, and automatic per-call `SAVEPOINT` rollback on error or trap.
- **Binary TCP Protocol**: Standalone **Asio** server and `whisk` CLI/REPL using length-prefixed binary framing.
- **Configurable Durability**: Four modes (`strict`, `group_commit`, `relaxed`, `memory_only`) backed by CRC32-verified Write-Ahead Logging (`whiskdb.wal`) and binary snapshots (`snapshot.whisk`).

---

## Quickstart

### 1. Setup & Build

```bash
./scripts/setup-dev.sh            # Install toolchain (Clang, Ninja, Wasmtime) on Ubuntu 24.04+ / Debian 13
./scripts/macro.sh build-release  # Build whiskdb, whisk, and whisk_bench (or: cmake --workflow --preset build-release)
```

### 2. Run the End-to-End Demo

```bash
bash scripts/demo.sh              # Non-interactive walkthrough (pass --interactive to pause between steps)
```

### 3. Run Server & CLI Manually

```bash
# Start the server (configured via config.toml or CLI flags)
./build/release/whiskdb --host 127.0.0.1 --port 8080 --data-dir ./data --durability group_commit

# Query and manage modules with the whisk CLI
./build/release/whisk ping
./build/release/whisk sql "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);"

# Scaffold, build, and test a local Whisk module
./build/release/whisk init my_service
./build/release/whisk build my_service
./build/release/whisk test my_service
```

See [`examples/`](examples/) for complete Whisk modules (`counter_whisk.cpp`, `inventory_whisk.cpp`, `ledger_whisk.cpp`).

---

## Measured Benchmarks

Measured on Linux `x86_64` using Release builds (`./build/release/whisk_bench`):

### Co-Location Advantage (`--colocation --iterations 200 --markdown`)
| Mode | Benchmark | Clients | Iterations | Ops/sec | p50 (us) | p95 (us) | p99 (us) | Notes |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| memory_only | colocation_sql | 1 | 200 | 2990.2 | 329.5 | 372.8 | 397.0 | 4x TCP SQL, rtt=0us |
| memory_only | colocation_whisk | 1 | 200 | 3528.3 | 269.4 | 315.8 | 483.9 | 1.18x faster (rtt=0us) |

### In-Memory Engine Throughput (`--quick --mode memory_only`)
| Workload | Ops | Ops/sec | Avg (us) | p50 (us) | p95 (us) | p99 (us) | Notes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `insert_seq` | 2000 | 110965.2 | 8.6 | 8.5 | 8.6 | 15.3 | |
| `insert_concur` | 2000 | 89939.1 | 36.8 | 10.1 | 14.7 | 26.4 | 4 threads |
| `point_read` | 2000 | 485402.7 | 1.9 | 1.9 | 1.9 | 2.2 | |
| `update_point` | 2000 | 106595.1 | 9.3 | 9.0 | 10.3 | 16.2 | |
| `range_scan` | 500 | 107266.8 | 9.2 | 9.0 | 9.2 | 9.5 | window=50 |

---

## Documentation

Documentation lives in [`docs/`](docs/) and can be served locally with MkDocs:

- [User Guide](docs/user-guide.md) — Server configuration, durability modes, `whisk` CLI, REPL, and writing Whisks.
- [Development](docs/development.md) — Architecture, building from source, testing, and repository layout.
- [Developer Guide](docs/developer-guide.md) — C++23 coding standards, error handling, and contributing.

```bash
pip install mkdocs
mkdocs serve
```

---

## Authors

- **Abdullah Al Tamsha**
- **Ibrahim Jamil Karaja**
- **Rashid Alkatheri**
