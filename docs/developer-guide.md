# WhiskDB Developer Guide

The WhiskDB Developer Guide specifies the normative engineering standards, internal subsystem architecture, coding conventions, and collaboration workflow for all team members and automated agents contributing to WhiskDB.

---

## 1. External Libraries & Ecosystem

WhiskDB relies on focused, lightweight dependencies managed via **vcpkg** or CMake's **FetchContent**:

| Library | Header / Component | Purpose | Integration Method |
|---|---|---|---|
| **[Standalone Asio](https://think-async.com/Asio/)** | `asio.hpp` (`asio_headers`) | Asynchronous non-blocking network I/O, event loops, TCP server and client socket handling | vcpkg / CMake FetchContent |
| **[SQLite3 Amalgamation](https://www.sqlite.org/)** | `sqlite3.h` (`sqlite3_static`) | In-memory relational storage engine, SQL parsing, B-tree indexes, and ACID savepoint management | Bundled C static library |
| **[toml++](https://marzer.github.io/tomlplusplus/)** | `toml++/toml.hpp` (`toml_headers`) | Header-only C++20/23 TOML configuration file parser for `config.toml` | vcpkg / CMake FetchContent |
| **[Wasmtime C API](https://wasmtime.dev/)** | `wasm.h`, `wasmtime.h` | Sandboxed WebAssembly runtime for server-side Whisk module execution *(in progress)* | System / Vendor archive |

!!! note "Zero Heavy Frameworks"
    WhiskDB avoids heavy all-in-one frameworks. All network protocols use length-prefixed binary frames built on top of standalone Asio rather than HTTP or heavy WebSocket stacks.

---

## 2. Coding Standards & Conventions (C++23)

### Namespace Rule
All engine code under `src/` and `include/` must reside inside `namespace database`. Never write `using namespace std;` in any header or translation unit.

### Naming Conventions

WhiskDB follows DuckDB-inspired naming conventions, partially verified via `clang-tidy`:

| Target | Convention | Example | Notes |
|---|---|---|---|
| **Files** | `snake_case` | `wal_manager.cpp`, `sql_engine.hpp` | Match class concept name |
| **Types** (Classes, Structs, Enums, Aliases) | `PascalCase` | `WhiskClient`, `WalManager`, `DurabilityMode` | Concrete and unambiguous |
| **Functions & Methods** | `PascalCase` | `ExecuteQuery()`, `AppendRecord()`, `Recover()` | Verb-noun phrases |
| **Variables & Members** | `snake_case` | `page_size`, `write_mutex_`, `is_active_` | Private members use trailing underscore |
| **Loop Indices** | Meaningful names in nested loops | `row_idx`, `col_idx`, `frame_idx` | `i` is allowed only in single, flat loops |

### Types & Memory Management

- **No Raw Allocation**: Never invoke raw `new`, `delete`, or `malloc`. Use smart pointers for dynamic allocation.
- **Ownership Semantics**: Default to `std::unique_ptr` for exclusive resource ownership. Use `std::shared_ptr` only when multiple distinct subsystems require shared lifetime.
- **Indices and Offsets**: Use `idx_t` (`uint64_t`) instead of raw `size_t` for counts, offsets, and collection indices.
- **Fixed-Width Integers**: Use explicit types (`int8_t`..`int64_t`, `uint8_t`..`uint64_t`) instead of ambiguous standard types like `int` or `long`.
- **Scoped Enums**: Always declare `enum class`. Specify the underlying storage type (e.g., `: uint8_t`) when the enum is serialized over the wire or to disk:
  ```cpp
  enum class Opcode : uint8_t {
      Ping = 0x01,
      SqlQuery = 0x02,
      WhiskPublish = 0x03,
      WhiskCall = 0x04
  };
  ```
- **Constants**: Replace magic numbers with named `constexpr` constants.

### Control Flow & Class Layout

- **Explicit Braces**: Always use `{}` braces for `if`, `else`, `for`, and `while` statements, even for single-line bodies.
- **Return Early**: Guard clauses and early returns keep indentation shallow.
- **Virtual Overrides**: Omit the `virtual` keyword on overriding member functions; use `override` or `final` exclusively.
- **Class Section Layout**: Order class definitions consistently:
  ```cpp
  class ExampleManager {
  public:
      // 1. Constructors & public member variables
      ExampleManager();
      ~ExampleManager();

  public:
      // 2. Public member functions
      Result<void> Initialize();

  private:
      // 3. Private helper functions
      Result<void> FlushBuffer();

  private:
      // 4. Private member variables
      std::string path_;
      idx_t buffer_size_ = 0;
  };
  ```

---

## 3. Error Handling & Invariant Policy

WhiskDB enforces a strict distinction between operational errors and developer bugs:

### Operational Errors (`Result<T>` & `ErrorCode`)
Expected runtime failures—such as network disconnects, invalid SQL syntax, table constraint violations, or missing files—must return `Result<T>` or status codes. Do not throw exceptions for expected operational flow.

```cpp
Result<QueryResult> result = client.Query("SELECT * FROM invalid_table;");
if (!result) {
    LOG_WARN("Query failed: {}", result.error().Message());
    return result.error();
}
```

### Invariant Assertions (`D_ASSERT`)
Use `D_ASSERT(condition)` generously to verify internal assumptions and developer invariants. Every `D_ASSERT` must include an explanatory comment on the preceding line:

```cpp
// Ensure the payload does not exceed the maximum allowable wire frame size
D_ASSERT(payload_len <= Framing::MAX_PAYLOAD_SIZE);
```

`D_ASSERT` checks are active in Debug builds and must **never** be triggered by external user input.

### Fatal Failures
Exceptions are reserved strictly for unrecoverable system failures, such as memory allocation exhaustion or unrecoverable disk corruption.

---

## 4. Internal Subsystem Architecture

WhiskDB is composed of four modular internal subsystems:

```
┌─────────────────────────────────────────────────────────────┐
│                       whiskdb core                          │
├─────────────────┬─────────────────┬─────────────────────────┤
│ Network Layer   │ SQL Engine      │ Storage & Durability    │
├─────────────────┼─────────────────┼─────────────────────────┤
│ - Framing       │ - SQLite3 :mem: │ - WalManager            │
│ - Protocol      │ - Cache         │ - PosixFile             │
│ - Asio Server   │ - ValueCodec    │ - SnapshotManager       │
│ - TcpClient     │ - Savepoints    │ - CRC32 Verifier        │
└─────────────────┴─────────────────┴─────────────────────────┘
```

### 1. Network Layer (`include/network/`, `src/network/`)
- **Framing (`Framing`)**: All packets use a 4-byte big-endian length prefix. Frames larger than `MAX_PAYLOAD_SIZE` (64 MB) are rejected.
- **Protocol (`Protocol`, `Request`, `Response`)**: Requests include an `Opcode` (1 byte) followed by payload bytes. Responses include a status code (`ErrorCode`, 2 bytes), execution duration in microseconds (`exec_us`, 8 bytes), and response payload.
- **Asio Server (`Server`, `TcpSession`)**: Scalable, non-blocking asynchronous TCP server running on standalone Asio. Sessions maintain dedicated receive buffers to minimize allocations.

### 2. SQL Engine Layer (`include/sql/`, `src/sql/`)
- **Engine Core (`SqlEngine`)**: Wraps an in-memory SQLite3 handle (`:memory:`).
- **Statement Cache (`StatementCache`)**: Prepared statement cache keyed by SQL text hash, eliminating parsing and compilation overhead on repetitive queries.
- **Value Codec (`ValueCodec`)**: High-speed binary serializer and deserializer for SQL data types (`ValueType`: `Null`, `Int64`, `Double`, `Text`, `Blob`), facilitating zero-copy decoding across wire packets.
- **Transaction Guard (`TransactionGuard`)**: Manages nested SQLite `SAVEPOINT` transactions with automatic rollback on error.

### 3. Storage & Durability Layer (`include/storage/`, `src/storage/`)
- **Write-Ahead Log (`WalManager`)**: Appends binary transaction records (`WalRecordType::SqlStatement`) protected by CRC32 checksums to `./data/db.wal`.
- **Durability Modes (`DurabilityMode`)**: Configurable flush behavior (`Strict`, `GroupCommit`, `Relaxed`, `MemoryOnly`).
- **File I/O (`PosixFile`)**: RAII POSIX file descriptor wrapper managing `open`, `pwrite`, `pread`, and `fdatasync`.
- **Snapshot Manager (`SnapshotManager`)**: Serializes full database state to disk checkpoints (`snapshot.db`) and safely resets the WAL log upon reaching record limits.

### 4. Client SDK & CLI Layer (`include/client/`, `src/client/`, `app/`)
- **WhiskClient (`WhiskClient`)**: Native C++ client handling socket connection, handshake, query execution, and error unwrapping.
- **Table Formatter (`FormatResultTable`)**: ASCII box-drawing table renderer with automatic column width calculation and execution metrics.
- **Interactive REPL (`app/whisk.cpp`)**: Interactive terminal client with multi-line statement continuation (`SplitStatements`), dot command execution (`.help`, `.ping`, `.logs`, `.snapshot`), and ANSI color error highlighting.

---

## 5. Build & Test Macros

WhiskDB provides unified development macros orchestrated by `scripts/macro.sh`:

| Macro | Description | Output Artifacts |
|---|---|---|
| `cmake build-debug` | Configures and builds all targets with Debug symbols and `-Wall -Wextra -Wpedantic` | `build/debug/` |
| `cmake build-release` | Configures and builds all targets with full `-O3` compiler optimizations | `build/release/` |
| `cmake run-tests` | Compiles Debug target and executes all test suites via CTest with verbose failure logging | Full CTest report |
| `cmake run-tests <regex>` | Runs only test suites matching the specified regex pattern (e.g., `cmake run-tests wal`) | Filtered test results |
| `cmake bench` | Builds Release and runs standard persistence and throughput benchmarks | Benchmark metrics |
| `cmake clean` | Removes all build directories (`build/debug/`, `build/release/`) | Clean workspace |

---

## 6. Git & Collaboration Workflow

Every contributor and agent must follow WhiskDB's mandatory 5-step development lifecycle:

```
[GitHub Issue] ──> [Branch off dev] ──> [Code & Test] ──> [Commit & Push] ──> [Open Pull Request]
```

### 1. Check & Update GitHub Issues
- Inspect the target issue before writing code: `gh issue view <issue-number>`.
- Post an acknowledgement comment stating that work has begun in the feature branch.

### 2. Dedicated Feature Branch off `dev`
- Always branch from the latest `dev` branch:
  ```sh
  git checkout dev
  git pull origin dev
  git checkout -b feat/issue-<issue-number>-<slug>
  ```

### 3. Build & Test Verification
- Ensure zero compiler warnings and 100% test pass rate:
  ```sh
  cmake build-debug
  cmake run-tests
  ```

### 4. Conventional Commits
Use conventional commit prefixes:
- `feat(scope): add feature description (Closes #N)`
- `fix(scope): fix bug description (Closes #N)`
- `docs(scope): update documentation`
- `test(scope): add test coverage`

### 5. Pull Request Submission
- Open the PR targeting `dev`:
  ```sh
  gh pr create --base dev --head feat/issue-<issue-number>-<slug> --title "feat(scope): ..." --body "..."
  ```
- Reference the resolved issue (`Closes #N`) in the PR description. Work is complete only after the PR is submitted.
