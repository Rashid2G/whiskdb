# WhiskDB Development & Architecture

This page outlines the architectural scope of WhiskDB, the durability and reliability model, external library dependencies, and current development milestone progress.

---

## 1. Project Scope & Objectives

WhiskDB is built around four primary engineering pillars:

1. **Sandboxed In-Engine Code Execution**: Co-locating business logic with in-memory database state using sandboxed WebAssembly execution to eliminate database network hops.
2. **Zero-Copy In-Memory Relational Access**: Leveraging SQLite3 in `:memory:` mode coupled with prepared statement caching and binary value encoders for sub-millisecond query execution.
3. **Flexible Durability & Crash Recovery**: Providing a high-performance write-ahead log (WAL) engine with configurable durability guarantees, CRC32 record integrity, and atomic checkpointing.
4. **Persistent Binary Wire Protocol**: Delivering low-latency client communication through length-prefixed binary framing, native C++ SDK bindings, and an interactive CLI shell.

---

## 2. Durability Modes & Latency Profiles

WhiskDB supports four distinct durability modes (`DurabilityMode`) configured via `durability_mode` in `config.toml` or passed via the `--durability` CLI option. This allows deployments to tailor the trade-off between write latency and crash resilience:

| Durability Mode | Config Aliases | Disk Flush Strategy | Crash Guarantee | Typical Latency | Recommended Workload |
|---|---|---|---|---|---|
| **`strict`** | `sync` | Synchronous `fdatasync` on every committed transaction | Zero data loss on sudden power loss or OS crash | ~1–5 ms (I/O bound) | Financial ledgers, high-value transactional records |
| **`group_commit`** *(Default)* | `batched` | In-memory WAL ring buffer flushed via batched `fdatasync` (e.g. periodic or batch threshold) | Bounded data loss window (up to batch interval); zero loss on process crash | ~50–150 µs | High-throughput OLTP, default production setting |
| **`relaxed`** | `async` | WAL records written to OS page cache (`write()`); background asynchronous flush | Survives process crashes; OS crash may lose unwritten page cache | ~5–15 µs | Real-time gaming, telemetry, high-ingestion streams |
| **`memory_only`** | `ephemeral` | WAL disabled entirely; pure `:memory:` execution | Volatile; data is reset on process termination | < 2 µs | Unit tests, ephemeral benchmarks, CI test runs |

### Checkpointing & Snapshotting

To keep WAL replay times fast during recovery, WhiskDB automatically checkpoints state when the number of accumulated mutations reaches `checkpoint_wal_records` (default: `100,000`):

1. **Atomic Flush**: The database engine writes a consistent snapshot (`snapshot.db`).
2. **Log Truncation**: Once the snapshot is verified on disk, the WAL file (`db.wal`) is truncated back to zero.
3. **Crash Recovery**: On startup, `Engine::Open()` inspects `./data`, loads the newest valid snapshot, and replays any trailing WAL records up to the crash point.

---

## 3. External Dependencies

WhiskDB integrates lightweight, modern C++ libraries:

| Library | Header / Target | Role in WhiskDB |
|---|---|---|
| **[Standalone Asio](https://think-async.com/Asio/)** | `asio.hpp` (`asio_headers`) | Powers the asynchronous non-blocking TCP server and client network loops |
| **[SQLite3](https://www.sqlite.org/)** | `sqlite3.h` (`sqlite3_static`) | In-memory relational storage engine, SQL execution, and transactional savepoints |
| **[toml++](https://marzer.github.io/tomlplusplus/)** | `toml++/toml.hpp` (`toml_headers`) | Parses `config.toml` for engine options and durability configuration |
| **[Wasmtime C API](https://wasmtime.dev/)** | `wasm.h`, `wasmtime.h` | Sandboxed WebAssembly host engine *(in active development)* |

### System & Toolchain Requirements

- **Operating System**: Linux `x86_64` (Ubuntu 24.04+ LTS, Debian Trixie 13)
- **Compiler**: GCC 13+ or Clang 18+ with full C++23 standard support (`-std=c++23`)
- **Build System**: CMake 3.21+ and Ninja or Make

---

## 4. Development Milestone Roadmap

```
[Phase 1: Binary Protocol]  ──>  [Phase 2: In-Memory SQL]  ──>  [Phase 3: Durability & WAL]
                                                                        │
                                                                        ▼
[Phase 6: Reactive Feeds]  <──  [Phase 5: WASM Host]     <──  [Phase 4: C++ SDK & CLI]
```

### Completed Milestones
- [x] **Phase 1: Network & Binary Framing**: Length-prefixed wire protocol, opcode dispatch, and non-blocking Asio TCP server.
- [x] **Phase 2: In-Memory SQL Engine**: Embedded SQLite3 amalgamation, prepared statement caching, and binary `ValueCodec`.
- [x] **Phase 3: Durability Engine**: Binary WAL format, CRC32 checksum validation, 4 durability modes, and snapshot recovery.
- [x] **Phase 4: Client Ecosystem**: C++ Client SDK (`WhiskClient`), result table formatting, and interactive CLI REPL (`whisk`).

### In Progress & Planned Milestones
- [ ] **Phase 5: WebAssembly Engine**: Sandboxed runtime integration via Wasmtime C API (`whisk init`, `build`, `publish`, `call`).
- [ ] **Phase 6: Real-Time Subscriptions**: Reactive client subscriptions broadcasting table mutation diffs over persistent connections.

---

## Next Steps

- Consult the **[User Guide](user-guide.md)** for installation instructions, CLI usage, and the C++ Client SDK.
- Consult the **[Developer Guide](developer-guide.md)** for coding standards, subsystem architecture, and build macros.
