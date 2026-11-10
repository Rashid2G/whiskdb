# Welcome to WhiskDB Docs

This is the official project documentation for **WhiskDB**, an in-memory relational database and WebAssembly application host designed to execute application logic directly within the engine.

---

## Project Overview

Traditional database architectures separate the database engine from the application tier with rigid network boundaries, incurring serialization, IPC, and round-trip network latency. **WhiskDB** eliminates this boundary by co-locating transactional business logic (sandboxed WebAssembly **Whisks**) and relational state in the same physical memory space.

Inspired by modern data systems such as **[SpacetimeDB](https://spacetimedb.com)**, **[DuckDB](https://duckdb.org/)**, and **[Convex](https://convex.dev)**, WhiskDB delivers sub-millisecond in-memory execution combined with configurable Write-Ahead Log (WAL) durability, point-in-time snapshots, and real-time client reactivity.

---

## Academic Information

This project is conducted as part of the Senior Capstone Graduation Project at **[OSTİM Technical University](https://www.ostimteknik.edu.tr/)** (Department of Software Engineering).

### Project Team

| Student Number | Full Name | Program |
|---|---|---|
| `230208916` | **Abdullah Al Tamsha** | Software Engineering |
| `230208879` | **Ibrahim Jamil Karaja** | Software Engineering |
| `230208843` | **Rashid Alkatheri** | Software Engineering |

---

## Explore the Docs

- **[User Guide](user-guide.md)**: CLI command reference, REPL dot-commands, and interactive shell usage.
- **[Development](development.md)**: Engine architecture, durability modes (`MemoryOnly`, `AsyncWal`, `SyncWalPerBatch`, `SyncWalPerTx`), and development macros.
- **[Developer Guide](developer-guide.md)**: C++23 engineering standards, external library integrations, testing guidelines, and PR workflow.
