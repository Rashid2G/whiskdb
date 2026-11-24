# Developer Guide

This guide defines the C++23 coding standards, error-handling conventions, and contribution workflow for WhiskDB. For architecture and build commands, see [Development](development.md).

---

## Coding Standards

### Memory & Types
- **Smart pointers only**: Do not use raw `new`, `delete`, or `malloc`. Prefer `std::unique_ptr`; use `std::shared_ptr` only when ownership is genuinely shared.
- **Fixed-width integers**: Use explicit fixed-width types (`int32_t`, `uint64_t`, etc.) rather than raw `int` or `long`.
- **Indices and counts**: Use `idx_t` (`uint64_t`) for sizes, counts, and offsets.
- **Scoped enums**: Use `enum class`, specifying `: uint8_t` when serialized on disk or over the wire.
- **Named constants**: Replace magic numbers with `constexpr` constants.

### Style & Structure
- **Namespace**: Core engine code under `include/` and `src/` resides in `namespace database`. Never write `using namespace std;`.
- **Const & references**: Mark variables and methods `const` by default; pass non-trivial types by `const &`, `std::string_view`, or `std::span<const uint8_t>`.
- **Control flow**: Always use `{}` braces on `if`, `for`, and `while`. Return early to keep nesting shallow. Omit `virtual` on overrides and mark with `override` or `final`.
- **Class layout**: Order sections as (1) `public:` constructors and public members, (2) `public:` methods, (3) `private:` helper methods, (4) `private:` member variables.

### Naming Conventions

| Target | Convention | Example |
|---|---|---|
| Files | `snake_case` | `wal_manager.cpp` |
| Types (classes, structs, enums) | `PascalCase` | `WalManager`, `DurabilityMode` |
| Functions & methods | `PascalCase` | `ExecuteQuery()`, `AppendRecord()` |
| Variables & members | `snake_case` | `chunk_size`, `payload_len` |
| Loop counters | Descriptive in nested loops (`i` in flat loops) | `row_idx`, `col_idx` |

Naming and static checks are enforced via `.clang-tidy`.

---

## Error Handling & Assertions

- **Expected errors (`Result<T>` / `Error`)**: Return `Result<T>` or error codes for expected failures such as invalid SQL, missing Whisk functions, network disconnects, or guest WASM traps. Do not throw exceptions for expected control flow.
- **Internal invariants (`D_ASSERT`)**: Assert internal preconditions with `D_ASSERT(condition)` and a short comment explaining the invariant. `D_ASSERT` must never be triggered by external user input:
  ```cpp
  // Validate that payload length fits within the maximum frame limit
  D_ASSERT(payload_len <= Framing::MAX_PAYLOAD_SIZE);
  ```
- **Fatal failures**: Reserve exceptions for unrecoverable engine errors (such as corrupted snapshot checksums or unrecoverable I/O failures).

---

## Contributing

1. **Branch from `dev`**:
   ```bash
   git checkout dev && git pull origin dev
   git checkout -b feat/<short-slug>
   ```
2. **Build and run tests**:
   ```bash
   ./scripts/macro.sh build-debug
   ./scripts/macro.sh run-tests
   ```
   Ensure zero compiler warnings (`-Wall -Wextra -Wpedantic`) and passing tests.
3. **Commit and open a Pull Request**:
   - Use conventional commit messages (`feat(<scope>): ...`, `fix(<scope>): ...`, `docs(<scope>): ...`).
   - Target the `dev` branch (`main`/`master` is reserved for releases).
   - Reference linked issues (e.g. `Closes #<N>`) and include verification output in the PR description.
