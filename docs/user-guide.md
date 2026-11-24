# User Guide

This guide covers running the `whiskdb` server, configuring durability, using the `whisk` CLI and REPL, and writing WebAssembly Whisk modules.

---

## Running the Server (`whiskdb`)

Start the database server with defaults from `config.toml` or override settings via CLI flags:

```bash
whiskdb [--config <file>] [--data-dir <dir>] [--host <ip>] [--port <n>] [--durability <mode>]
```

| Flag | Default (`config.toml`) | Description |
|---|---|---|
| `--config <file>` | `config.toml` | Path to the TOML configuration file. |
| `--data-dir <dir>` | `./data` | Directory for `whiskdb.wal` and `snapshot.whisk`. |
| `--host <ip>` | `0.0.0.0` | Listen address. |
| `--port <n>` | `8080` | TCP listen port. |
| `--durability <mode>` | `group_commit` | Durability mode (`strict`, `group_commit`, `relaxed`, `memory_only`). |

### Configuration (`config.toml`)

```toml
[config]
host = "0.0.0.0"
port = 8080
data_dir = "./data"
page_size = 4096

# Durability mode: strict | group_commit | relaxed | memory_only
durability_mode = "group_commit"

# Group commit batching interval in milliseconds
wal_group_commit_ms = 10

# Snapshot + reset WAL after this many WAL records (0 = never)
checkpoint_wal_records = 100000

# WebAssembly (WASM) execution limits
wasm_max_fuel = 100000000
wasm_max_memory_mb = 64
```

### Durability Modes

| Mode | Aliases | Disk Sync Strategy | Use Case |
|---|---|---|---|
| `strict` | `sync` | Synchronous `fdatasync` on every committed write. | Financial ledgers and zero-data-loss workloads. |
| `group_commit` *(default)* | `batched` | Buffers WAL records and flushes periodically via batched `fdatasync`. | Balanced production throughput and durability. |
| `relaxed` | `async` | Writes WAL records to the OS page cache without blocking on `fdatasync`. | Low-latency writes that survive process crashes. |
| `memory_only` | `ephemeral` | Purely in-memory; WAL disabled. | Local tests (`whisk test`) and compute benchmarks. |

---

## Command-Line Interface (`whisk`)

The `whisk` CLI connects to a running `whiskdb` server over TCP and also provides offline commands (`init`, `build`, `test`) for local module development.

### Global Options

| Option | Default | Description |
|---|---|---|
| `--host <H>` | `127.0.0.1` | Server hostname or IPv4 address. |
| `--port <P>` | `8080` | Server TCP port. |
| `--json` | `false` | Emit machine-readable JSON output. |
| `-h`, `--help` | — | Display usage information. |

### Commands

| Command | Usage Example | Description |
|---|---|---|
| `init` | `whisk init my_service` | Scaffold a new C++ Whisk module project (`whisk_guest.hpp` + build script). |
| `build` | `whisk build [dir]` | Compile a local Whisk project into `.wasm` bytecode. |
| `test` | `whisk test [dir]` | Run Whisk tests locally against an ephemeral in-memory engine. |
| `publish` | `whisk publish auth build/auth.wasm` | Deploy or hot-reload a `.wasm` module (runs `__init__` / `__update__` atomically). |
| `call` | `whisk call auth login '{"user":"alice"}'` | Invoke an exported Whisk function transactionally with optional JSON args. |
| `list` | `whisk list` | List published modules and their exported functions. |
| `sql` | `whisk sql "SELECT id, name FROM users;"` | Execute a SQL statement and print the result table. |
| `logs` | `whisk logs [-n N]` | Print the latest `N` server execution logs (default: 200). |
| `snapshot` | `whisk snapshot` | Trigger a manual snapshot checkpoint (`snapshot.whisk`). |
| `ping` | `whisk ping` | Measure server round-trip latency (`PONG in <us>`). |
| `repl` | `whisk` or `whisk repl` | Open the interactive SQL and admin shell (default when no command is given). |

#### Examples

```bash
# Execute an ad-hoc SQL query
whisk --host 127.0.0.1 --port 8080 sql "SELECT id, username FROM accounts WHERE active = 1;"

# Check server connectivity and latency
whisk ping

# Fetch recent server logs
whisk logs -n 50
```

---

## Interactive REPL

Running `whisk` with no command opens an interactive shell:

```text
whisk> CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, price REAL);
[OK] (round trip 0.85 ms)
whisk> INSERT INTO items VALUES (1, 'Widget', 19.99), (2, 'Gadget', 49.50);
[OK] 2 rows affected (round trip 1.20 ms)
whisk> SELECT * FROM items;
+----+--------+-------+
| id | name   | price |
+----+--------+-------+
|  1 | Widget | 19.99 |
|  2 | Gadget | 49.50 |
+----+--------+-------+
(round trip 0.94 ms)
whisk> .ping
PONG in 280 us
whisk> .quit
```

Terminate SQL statements with `;` (multi-line statements continue with `...> `). Administrative dot-commands run immediately:

| Dot-Command | Description |
|---|---|
| `.publish <name> <file.wasm>` | Upload and hot-reload a `.wasm` Whisk module. |
| `.call <mod> <fn> [json]` | Invoke a published Whisk function transactionally. |
| `.list` | List registered Whisk modules and exported functions. |
| `.tables` | List user-defined tables in the database. |
| `.logs [N]` | Print the latest `N` server logs. |
| `.snapshot` | Trigger a manual WAL checkpoint and snapshot. |
| `.ping` | Measure server round-trip latency. |
| `.help` | Show available dot-commands. |
| `.quit`, `.exit` | Disconnect and exit the REPL. |

---

## Writing a Whisk Module (`whisk_guest.hpp`)

Whisks are freestanding C++23 functions compiled to `wasm32` using the header-only `include/sdk/whisk_guest.hpp` SDK:

```cpp
#include "whisk_guest.hpp"

// Runs atomically when the module is first published
WHISK_INIT() {
    whisk::Exec("CREATE TABLE IF NOT EXISTS counters (id INTEGER PRIMARY KEY, val INTEGER NOT NULL);");
    return whisk::Ok("{\"status\":\"initialized\"}");
}

// Exported transactional Whisk function
WHISK_FUNC(increment) {
    int64_t id = whisk::JsonGetIntOr(WHISK_ARGS(), "id", 1);
    whisk::Buf<64> params;
    params.append_char('[');
    params.append_int(id);
    params.append_char(']');
    whisk::Exec("INSERT INTO counters (id, val) VALUES (?1, 1) "
                "ON CONFLICT(id) DO UPDATE SET val = val + 1;", params.view());
    return whisk::Ok("{\"status\":\"incremented\"}");
}
```

Build, test, publish, and invoke:

```bash
whisk init counter && cd counter
whisk build
whisk test
whisk publish counter build/counter.wasm
whisk call counter increment '{"id":1}'
```

Complete reference modules are available in `examples/` (`counter_whisk.cpp`, `inventory_whisk.cpp`, and `ledger_whisk.cpp`).
