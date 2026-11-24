# WhiskDB Documentation

**WhiskDB** is an in-memory relational database and WebAssembly application host written in C++23 for Linux `x86_64`.

---

## Overview

Traditional architectures separate the database from application code over a network boundary, adding serialization and round-trip latency to every multi-step transaction. WhiskDB co-locates relational tables (in-memory SQLite3) and sandboxed WebAssembly functions (**Whisks**, via `libwasmtime`) in the same process.

- **Single-round-trip transactions**: Multi-query business logic runs directly inside the engine against in-memory tables.
- **Transactional sandboxing**: Every Whisk invocation executes inside a SQL `SAVEPOINT` with memory isolation and fuel limits, rolling back cleanly on errors or traps.
- **Configurable durability**: Choose between `strict`, `group_commit`, `relaxed`, and `memory_only` modes backed by a CRC32-verified WAL and binary snapshots.
- **Integrated CLI & SDK**: Scaffold, compile, test, publish, and invoke C++23 Whisk modules using the `whisk` CLI and header-only `whisk_guest.hpp` SDK.

---

## Quickstart

```bash
# 1. Install dependencies (Ubuntu 24.04+ / Debian 13 x86_64)
./scripts/setup-dev.sh

# 2. Build release binaries
./scripts/macro.sh build-release

# 3. Run the end-to-end demo
bash scripts/demo.sh
```

---

## Guides

- **[User Guide](user-guide.md)** — Starting `whiskdb`, `config.toml`, durability modes, `whisk` CLI & REPL, and writing Whisk modules.
- **[Development](development.md)** — Architecture overview, build & test commands, benchmarks, and repository layout.
- **[Developer Guide](developer-guide.md)** — C++23 coding standards, error handling, and contributing workflow.
