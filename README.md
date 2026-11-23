# WhiskDB

> Database with Superpowers — In-Engine WebAssembly Execution with Real-Time Persistence.

WhiskDB is a high-performance, in-memory relational database and WebAssembly application host written in **C++23** for Linux `x86_64`. Inspired by **SpacetimeDB**, **Convex**, and **DuckDB**, WhiskDB co-locates relational data (in-memory SQLite3) and sandboxed server-side functions (**Whisks** via `libwasmtime`) in the same memory space.

---

## Architecture Overview

- **Relational Query Engine**: In-memory SQLite3 C-amalgamation core with prepared statement caching and ANSI SQL support.
- **Sandboxed WASM Runtime**: Pre-compiled `libwasmtime` modules with per-invocation `SAVEPOINT` ACID rollback, memory isolation, and fuel metering.
- **Zero-Copy TCP Networking**: Standalone **Asio** with length-prefixed binary TCP framing (WebSockets planned as future work).
- **Configurable Durability**: Four modes (`strict`, `group_commit`, `relaxed`, `memory_only`) backed by CRC32-verified Write-Ahead Logging (`whiskdb.wal`) and binary snapshots (`snapshot.whisk`).

---

## Quickstart

### 1. Build WhiskDB
```bash
./scripts/setup-dev.sh          # Install toolchains (Clang, Wasmtime, Ninja)
cmake build-release             # Compile whiskdb, whisk CLI, and whisk_bench
```

### 2. Run the End-to-End Demo
```bash
bash scripts/demo.sh            # Non-interactive walkthrough (pass --interactive to pause)
```

### 3. Manual Server & CLI
```bash
# Start the server
./build/release/whiskdb --host 127.0.0.1 --port 8080 --data-dir ./data --durability group_commit

# Interact via the whisk CLI
./build/release/whisk ping
./build/release/whisk sql "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);"
./build/release/whisk init my_service && cd my_service && ../build/release/whisk build
```

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

Full documentation is built with MkDocs (`docs/`):
- [User Guide](docs/user-guide.md) — `whisk` CLI, REPL dot-commands, and `whisk_guest.hpp` quickstart.
- [Project Development](docs/development.md) — Architecture, durability modes, and build macros.
- [Developer Guide](docs/developer-guide.md) — Coding standards, external libraries, and Git workflow.

```bash
pip install mkdocs
mkdocs serve
```

---

## Team

Senior Capstone Project (YZL 400) at **OSTİM Technical University**:
- **Abdullah Al Tamsha**
- **Ibrahim Jamil Karaja**
- **Rashid Alkatheri**
