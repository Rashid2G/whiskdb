# User Guide

WhiskDB provides a unified command-line tool, `whisk`, for interacting with the database engine, managing WebAssembly Whisk modules, executing SQL queries, and administering the database.

---

## Command-Line Interface (CLI)

The `whisk` binary connects to a running WhiskDB server instance via TCP.

### Global Options

These options apply across CLI commands and REPL sessions:

| Option | Flag Format | Default | Description |
|---|---|---|---|
| **Host** | `--host <H>` or `--host=<H>` | `127.0.0.1` | WhiskDB server hostname or IPv4 address. |
| **Port** | `--port <P>` or `--port=<P>` | `8080` | WhiskDB server listening port. |
| **JSON Output** | `--json` | `false` | Emit machine-readable JSON output for scripting and automation. |
| **Help** | `-h`, `--help` | — | Display CLI usage information and available commands. |

---

### Command Reference

The table below outlines all primary `whisk` commands:

| Command | Usage Example | Description |
|---|---|---|
| `whisk init` | `whisk init my_service` | Scaffolds a new C++ Whisk module project with template source code (`whisk_guest.hpp`) and build scripts. |
| `whisk build` | `whisk build` | Compiles C++ Whisk module sources into deterministic WebAssembly (`.wasm`) bytecode. |
| `whisk test` | `whisk test` | Executes Whisks locally against an ephemeral in-memory (`:memory:`) WhiskDB instance before publishing. |
| `whisk publish` | `whisk publish auth build/auth.wasm` | Deploys and hot-reloads `.wasm` bytecode modules onto the host database, running `__init__` / `__update__` hooks atomically. |
| `whisk call` | `whisk call auth login '{"user":"alice"}'` | Invokes a registered Whisk function transactionally with optional JSON parameters. |
| `whisk list` | `whisk list` | Lists all published Whisk modules and their exported functions. |
| `whisk sql` | `whisk sql "SELECT id, name FROM users;"` | Executes a SQL statement over the connection and displays the result table. |
| `whisk logs` | `whisk logs [-n N]` | Retrieves the latest `N` structured in-memory execution and diagnostic logs from the server. |
| `whisk snapshot` | `whisk snapshot` | Triggers a manual snapshot checkpoint, flushing state to `snapshot.whisk`. |
| `whisk ping` | `whisk ping` | Measures connection round-trip latency (`PONG in <us>`). |
| `whisk repl` | `whisk` or `whisk repl` | Launches an interactive command and SQL REPL shell (default command when invoked without arguments). |

#### CLI Usage Examples

**Execute an ad-hoc SQL query:**
```bash
whisk --host 127.0.0.1 --port 8080 sql "SELECT id, username, email FROM accounts WHERE active = 1;"
```

**Check server connectivity and latency:**
```bash
whisk ping
# Output: PONG in 312 us
```

**Fetch recent server diagnostic logs:**
```bash
whisk logs
```

---

## Interactive REPL Shell

Starting `whisk` without arguments (or running `whisk repl`) opens the interactive database shell:

```text
whisk> 
```

### REPL Features
- **Interactive SQL**: Enter SQL queries terminated by `;`. Multi-line input is supported automatically (`...> ` prompt).
- **Execution Timing**: Each query displays execution duration in milliseconds along with formatted tabular output.
- **Dot-Commands**: Administrative and diagnostic commands prefixed with `.` execute immediately.

### Dot-Commands Reference

| Dot-Command | Purpose | Description |
|---|---|---|
| `.publish <name> <file.wasm>` | Publish Module | Uploads and hot-reloads a `.wasm` Whisk module on the server. |
| `.call <mod> <fn> [json]` | Invoke Whisk | Executes a published Whisk function transactionally with optional JSON arguments. |
| `.list` | List Modules | Displays all registered Whisk modules and their exported functions. |
| `.tables` | List Tables | Queries and displays all user-defined relational tables in the active schema. |
| `.logs [N]` | View Logs | Prints the latest `N` server diagnostic logs directly into the terminal. |
| `.snapshot` | Trigger Snapshot | Triggers a manual WAL checkpoint and persists an in-memory snapshot to disk. |
| `.ping` | Ping Server | Tests connection health and displays round-trip network time in microseconds. |
| `.help` | Show Help | Displays the list of available REPL dot-commands. |
| `.quit`, `.exit` | Exit REPL | Closes the client connection and exits the shell session. |

#### REPL Session Example

```text
$ whisk
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
whisk> .snapshot
Snapshot created: 2 tables, 1420 bytes
whisk> .quit
```

---

## Writing a Whisk Module (`whisk_guest.hpp`)

Whisks are freestanding C++23 functions compiled to WebAssembly (`wasm32`) using the header-only `sdk/whisk_guest.hpp` SDK:

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
    params.append_char('['); params.append_int(id); params.append_char(']');
    whisk::Exec("INSERT INTO counters (id, val) VALUES (?1, 1) "
                "ON CONFLICT(id) DO UPDATE SET val = val + 1;", params.view());
    return whisk::Ok("{\"status\":\"incremented\"}");
}
```

Build, publish, and invoke:
```bash
whisk init counter && cd counter
whisk build
whisk publish counter build/counter.wasm
whisk call counter increment '{"id":1}'
```

---

## Further Reading

- [Project Development Overview](development.md)
- [Developer Guide & Architecture](developer-guide.md)
