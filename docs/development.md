# Project Development

This page outlines the architectural foundation, durability strategies, and development macro workflows of WhiskDB.

---

## Architecture Overview

WhiskDB eliminates traditional client-database network boundaries by co-locating server-side application logic and relational state within the same memory space.

```
                      +---------------------------------------+
                      |               Whisk CLI               |
                      |        (whisk sql, repl, call)        |
                      +-------------------+-------------------+
                                          | TCP Framing (Asio)
                      +-------------------v-------------------+
                      |             WhiskDB Core              |
                      |                                       |
                      |  +---------------------------------+  |
                      |  |      In-Memory SQLite Core      |  |
                      |  |  (Relational Storage & Queries) |  |
                      |  +----------------+----------------+  |
                      |                   | Shared Memory     |
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

### Key Subsystems

1. **Relational Query Engine**: In-memory SQLite C-amalgamation core supporting ANSI SQL, indexed scans, and cached prepared statements.
2. **WASM Runtime Host**: Embeds `libwasmtime` to execute user-defined procedural Whisks with transactional savepoint isolation, gas/fuel metering, and memory protection.
3. **Storage & Persistence**: Configurable Write-Ahead Logging (`whiskdb.wal`) with binary framing, CRC32 checksums, and atomic crash recovery paired with binary snapshots (`snapshot.whisk`).
4. **Asynchronous Network Layer**: High-performance, non-blocking TCP communication powered by standalone **Asio** and **Boost.Beast**.

---

## Durability Modes

WhiskDB provides four durability levels to balance latency against persistence requirements. The mode can be configured in `config.toml` (`durability_mode`) or per-session:

| Durability Mode | Config Aliases | Disk Sync Strategy | Crash Recovery Guarantee | Primary Use Case |
|---|---|---|---|---|
| **`MemoryOnly`** | `memory_only`, `ephemeral` | No disk I/O; purely in-memory execution. | None. State is lost upon process termination. | Unit testing (`whisk test`), staging sandboxes, and raw compute benchmarks. |
| **`AsyncWal`** | `relaxed`, `async` | Appends WAL records to OS page cache without blocking on `fdatasync`. | Survives process crashes. Risk of small data loss window on power failure or OS crash. | High-frequency logging, telemetry, and sub-10µs write overhead workloads. |
| **`SyncWalPerBatch`** *(Default)* | `group_commit`, `batched` | Appends to WAL ring buffer and periodically flushes via batched `fdatasync`. | Zero data loss for all acknowledged batch flushes. | Production workloads requiring optimal balance of SpacetimeDB-grade throughput and durability. |
| **`SyncWalPerTx`** | `strict`, `sync` | Issues synchronous `fdatasync` before acknowledging each transaction. | Complete durability against power failure, OS kernel panic, or hardware failure. | Financial ledgers, mission-critical mutations, and strict compliance systems. |

---

## Development Macros

WhiskDB provides unified macro commands for standard development, compilation, and testing workflows. These commands can be executed directly using `cmake <macro>` or `scripts/macro.sh <macro>`:

| Macro Command | Description | Artifact Output |
|---|---|---|
| `cmake build-debug` | Configures and compiles the engine, apps, and test suites in Debug mode. | `build/debug/` |
| `cmake build-release` | Configures and compiles with maximum compiler optimizations in Release mode. | `build/release/` |
| `cmake run-tests` | Compiles Debug and executes all CTest suites with verbose diagnostic reporting on failure. | `build/debug/` |
| `cmake run-tests <regex>` | Runs only test suites matching the specified regex filter (e.g. `cmake run-tests wal`). | `build/debug/` |
| `cmake bench` | Builds Release and executes the persistence and throughput benchmark suite (`whisk_bench`). | `build/release/` |
| `cmake bench-compare [branch]` | Runs the automated benchmark harness comparing current metrics against a target branch (defaults to `master`). | Benchmark summary |
| `cmake clean` | Removes all temporary build trees and cached dependency artifacts. | `build/debug`, `build/release` |

### Adding Custom Macros

All macros are orchestrated through `scripts/macro.sh`. To implement a new development macro:

1. Open `scripts/macro.sh`.
2. Define a bash handler function:
   ```bash
   cmd_coverage() {
     cmd_build_debug
     echo "==> Generating code coverage report..."
     # Execute coverage tooling
   }
   ```
3. Register the command pattern in the `case "$SUBCOMMAND" in` dispatcher block.

---

## External Libraries Summary

WhiskDB integrates focused external libraries for networking, data storage, serialization, and sandboxed bytecode execution:

- **Asio** & **Boost.Beast**: Asynchronous networking, TCP session multiplexing, and WebSocket transport.
- **SQLite3**: In-process SQL query compilation and relational table storage.
- **libwasmtime**: Fast, sandboxed WebAssembly execution environment.
- **toml++**: Configuration parsing (`config.toml`).

For deep architectural requirements, component links, and coding standards, refer to the [Developer Guide](developer-guide.md).
