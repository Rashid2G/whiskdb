# WhiskDB

WhiskDB is an in-memory relational database and WebAssembly application host written in **C++23** for Linux `x86_64`. It eliminates traditional network boundaries and object-relational impedance by co-locating relational state and sandboxed server-side functions (**Whisks**) in the same physical memory space.

Inspired by **DuckDB**, **SpacetimeDB**, and **Convex**, WhiskDB combines sub-millisecond query execution with configurable write-ahead log (WAL) persistence, zero-copy binary wire framing, and a native C++ client SDK and CLI.

---

## Architecture at a Glance

WhiskDB unites client communication, transaction execution, in-memory relational state, and durable persistence into a unified engine:

```
┌───────────────────────────────────────────────────────────────┐
│               Whisk CLI  &  C++ Client SDK                    │
│        whisk repl  │  whisk sql  │  whisk ping  │  whisk logs │
└───────────────────────────────┬───────────────────────────────┘
                                │  TCP Binary Wire Protocol
                                │  (4-byte framing, Opcode, CRC32)
┌───────────────────────────────▼───────────────────────────────┐
│                       WhiskDB Server                          │
│                                                               │
│   ┌───────────────────────────────────────────────────────┐   │
│   │           Asynchronous Network Engine                 │   │
│   │           (Standalone Asio Event Loop)                │   │
│   └───────────────────────────┬───────────────────────────┘   │
│                               │ Request Dispatch              │
│   ┌───────────────────────────▼───────────────────────────┐   │
│   │                 In-Memory SQL Engine                  │   │
│   │         SQLite3 Amalgamation + Statement Cache        │   │
│   │         Value Codec (Int64, Double, Text, Blob)       │   │
│   └───────────────────────────┬───────────────────────────┘   │
│                               │ Transaction Savepoint / WAL   │
│   ┌───────────────────────────▼───────────────────────────┐   │
│   │              Storage & Durability Engine              │   │
│   │  WAL Manager (Strict, Group Commit, Relaxed, Ephemeral)│   │
│   │  Snapshot Manager (Atomic Zstandard Checkpoints)      │   │
│   └───────────────────────────────────────────────────────┘   │
└───────────────────────────────────────────────────────────────┘
```

---

## Core Capabilities

| Capability | Specification | Key Benefit |
|---|---|---|
| **Language & Standard** | Modern C++23 | Zero-overhead abstractions, `std::string_view`, `std::span`, and monadic `Result<T>` |
| **In-Memory SQL** | SQLite3 Amalgamation | Full ACID transactions with sub-millisecond in-memory query execution |
| **Statement Caching** | Hash-keyed Prepared Statements | Bypasses query parsing on hot paths for repeated execution |
| **Configurable Durability** | 4 `DurabilityMode` Profiles | Select between `strict` (`fdatasync`), `group_commit` (batched), `relaxed`, and `memory_only` |
| **Binary Wire Protocol** | Length-Prefixed Framing | Compact 4-byte big-endian framing with opcodes and CRC32 payload verification |
| **Client Ecosystem** | `whisk` CLI & `WhiskClient` SDK | Interactive REPL, formatted ASCII tables, and native C++ client library |

---

## Quick Start Preview

### Start the Server

Launch the database daemon with default options (`group_commit` durability on `0.0.0.0:8080`):

```sh
./build/debug/app/whiskdb
```

### Connect via the CLI

Start the interactive REPL to query and inspect the server:

```sh
./build/debug/app/whisk repl
```

```sql
whisk> CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, balance REAL);
Query OK, 0 rows affected (round trip 0.42 ms)

whisk> INSERT INTO users (name, balance) VALUES ('Alice', 150.50), ('Bob', 82.00);
Query OK, 2 rows affected, last_insert_rowid = 2 (round trip 0.38 ms)

whisk> SELECT * FROM users;
+----+-------+---------+
| id | name  | balance |
+----+-------+---------+
|  1 | Alice |  150.50 |
|  2 | Bob   |   82.00 |
+----+-------+---------+
2 rows in set (0.35 ms)
```

---

## Academic Information

WhiskDB is developed as a Senior Capstone Graduation Project at **[OSTİM Technical University](https://www.ostimteknik.edu.tr/)** (Faculty of Engineering, Department of Software Engineering).

### Project Team

| Student ID | Full Name | Program |
|---|---|---|
| `230208916` | **Abdullah Al Tamsha** | Software Engineering |
| `230208879` | **Ibrahim Jamil Karaja** | Software Engineering |
| `230208843` | **Rashid Alkatheri** | Software Engineering |

---

## Documentation Navigation

- **[User Guide](user-guide.md)**: Server setup, `config.toml` options, `whisk` CLI reference, interactive REPL, and complete C++ Client SDK (`WhiskClient`) usage.
- **[Developer Guide](developer-guide.md)**: Engineering standards, DuckDB-inspired C++ conventions, internal engine architecture, `D_ASSERT` error handling, and unified build macros.
- **[Development Status](development.md)**: Project scope, durability trade-offs, library ecosystem, and milestone progress.
