# AGENTS.md — WhiskDB Engineering & Workflow Guide

WhiskDB is a high-performance, in-memory relational database and WebAssembly application host written in **C++23** for Linux `x86_64` (Ubuntu 24.04+ / Debian Trixie). Inspired by **SpacetimeDB**, **Convex**, and **DuckDB**, WhiskDB co-locates relational data and sandboxed server-side functions (**Whisks**) in the same memory space.

---

## 1. Core Architectural Principles

1. **SpacetimeDB-Style Developer Workflow:**
   Developers interact with WhiskDB through a complete local-to-production module lifecycle:
   - `whisk init <project>`: Scaffold a C++ Whisk module project (`whisk_guest.hpp` + build script).
   - `whisk build`: Compile C++ Whisks into deterministic `.wasm` bytecode.
   - `whisk test`: Run Whisks locally against an ephemeral `:memory:` WhiskDB instance before publishing.
   - `whisk publish <module> <file.wasm>`: Push/hot-reload `.wasm` modules into the host DB (running optional `__init__` / `__update__` schema lifecycle hooks atomically).
   - `whisk call <module> <func> [json_args]`: Invoke a server-side Whisk transactionally.
   - `whisk sql "<query>"`: Execute standard SQL queries over the persistent TCP connection.
   - `whisk logs`: Stream or inspect structured execution logs from Whisks and the host engine.

2. **Flexible Reliability & Durability Modes (`DurabilityMode`):**
   Configured in `config.toml` (`durability_mode`) or per-session to balance latency and durability:
   - `strict` (`SYNC`): `fdatasync` on every committed write/Whisk. Zero data loss on power/OS crash.
   - `group_commit` (`BATCHED` — Default): Appends to an in-memory WAL ring buffer and flushes via batched `fdatasync` (e.g., every 2 ms or 64 commits) for SpacetimeDB-grade throughput.
   - `relaxed` (`ASYNC`): Writes WAL records to the OS page cache without blocking on `fdatasync` (survives process crashes with sub-10µs write overhead).
   - `memory_only` (`EPHEMERAL`): Pure `:memory:` execution with WAL disabled (used by `whisk test` and raw compute benchmarks).

3. **DuckDB-Inspired C++ Engineering:**
   When making design or implementation choices, follow **DuckDB** conventions and [docs/developer-guide.md](docs/developer-guide.md).

---

## 2. C++23 Coding Standards (Strictly Enforced)

- **Namespace:** All core engine code under `src/` and `include/` MUST reside in `namespace database` (never use `using namespace std;`).
- **Naming Conventions:**
  - **Files:** `snake_case` (`wal_manager.cpp`, `wasm_runtime.hpp`)
  - **Types (Classes, Structs, Enums, Aliases):** `PascalCase` (`WalManager`, `TcpSession`, `DurabilityMode`)
  - **Functions & Methods:** `PascalCase` (`ExecuteQuery()`, `InvokeWhisk()`, `AppendRecord()`)
  - **Variables & Members:** `snake_case` (`chunk_size`, `payload_len`, `is_active_`)
  - **Loop Counters:** Meaningful names in nested loops (`row_idx`, `col_idx`); `i` only in single flat loops.
- **Types & Memory:**
  - Use `idx_t` (`uint64_t`) for counts, indices, and offsets instead of `size_t`.
  - Use fixed-width integers (`int8_t`..`int64_t`, `uint8_t`..`uint64_t`) instead of raw `int` or `long`.
  - Use `enum class` (specify `: uint8_t` when serialized on disk or wire).
  - Never use raw `new`, `delete`, or `malloc`. Prefer `std::unique_ptr` by default; use `std::shared_ptr` only when shared ownership is strictly required.
  - Replace magic numbers with `constexpr` constants.
- **Class Layout:** Order sections explicitly as:
  1. `public:` constructors and public member variables
  2. `public:` member functions
  3. `private:` helper functions
  4. `private:` member variables
- **Control Flow:** Always use `{}` braces on `if`/`for`/`while`. Return early to keep nesting shallow. Omit `virtual` on overrides and mark with `override` or `final`.

---

## 3. Error Handling & `D_ASSERT` Policy

1. **Expected Operational Errors (`Result<T>` / `Error`):** Invalid user SQL, missing Whisk function, network disconnect, or guest WASM failure MUST return `Result<T>` / status codes—do not throw exceptions for expected control flow.
2. **Developer Invariants (`D_ASSERT`):** Assert internal preconditions and invariants generously using `D_ASSERT(condition)` with an explanatory comment right above it:
   ```cpp
   // Validate that payload length fits within the maximum frame limit
   D_ASSERT(payload_len <= Framing::MAX_PAYLOAD_SIZE);
   ```
   `D_ASSERT` must never be triggered by external user input.
3. **Fatal Unrecoverable Failures (`FatalError`):** Use exceptions only for hard query/engine termination (e.g., corrupted snapshot checksum, out-of-memory, unrecoverable disk I/O failure).

---

## 4. High-Performance Rules

- **Zero-Copy Hot Paths:** Pass `std::string_view` and `std::span<const uint8_t>` across framing, protocol, WAL, and WASM host ABI boundaries. Avoid unnecessary `std::string` or `std::vector` copies.
- **Reusable Buffers:** Reuse per-session TCP frame buffers and WAL serialization buffers instead of reallocating on every request.
- **Prepared Statement & Module Caching:**
  - Cache compiled `sqlite3_stmt*` handles keyed by SQL hash so Whisks calling `whisk_db_exec` / `whisk_db_query` skip SQL re-parsing.
  - Pre-compile and cache `wasmtime_module_t` artifacts on registration so `WHISK_INVOKE` only instantiates and runs with sub-millisecond overhead.
- **Transactional Savepoints:** Every `WHISK_INVOKE` runs inside an automatic `SAVEPOINT` and rolls back on non-zero return, WASM trap, or fuel exhaustion.

---

## 5. Strict Testing & Quality Gates

Nothing merges into `dev` or `main` without passing all checks:
1. **Unit & Exception Path Tests:** Every PR must add unit tests in `tests/` covering normal execution, boundary cases, and expected error/exception paths.
2. **Deterministic End-to-End Tests:** SQL queries, Whisk invocations, trap rollbacks, and WAL crash-recovery scenarios must have automated reproducible tests run via `ctest --output-on-failure`.
3. **Compiler & Sanitizer Cleanliness:**
   - Must compile cleanly on Ubuntu 24.04+ and Debian Trixie (`x86_64`) with `-Wall -Wextra -Wpedantic`.
   - Must pass AddressSanitizer (`ASAN`) and UndefinedBehaviorSanitizer (`UBSAN`) with zero leaks or UB.
   - Must adhere to `clang-tidy` naming and style checks.

---

## 6. Documentation (`docs/` & MkDocs)

- Keep `docs/index.md`, `docs/development.md`, `docs/developer-guide.md`, and `docs/user-guide.md` **up to date, short, and to the point**.
- **Never overwrite or bloat existing documentation sections.** Update only what changed in the corresponding PR using concise tables and minimal prose.

---

## 7. Git, Issue & Pull Request Workflow

1. **1 Issue = 1 Branch = 1 PR:**
   - Every task starts as a tracked GitHub Issue with clear Acceptance Criteria and Test Requirements.
   - Branch off `dev`: `feat/issue-<N>-<short-slug>`, `fix/issue-<N>-<short-slug>`, or `docs/issue-<N>-<short-slug>`.
2. **Commits:** Conventional commits (`feat: ...`, `fix: ...`, `refactor: ...`, `test: ...`, `docs: ...`).
3. **Pull Requests:**
   - Target branch: `dev` (`main`/`master` is reserved for stable milestone releases).
   - Reference `Closes #<N>` in the PR body, include test output, and verify the PR checklist in [docs/developer-guide.md](docs/developer-guide.md).
