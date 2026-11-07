# WhiskDB User Guide

This guide covers running the WhiskDB server daemon, using the interactive `whisk` command-line interface (CLI), and integrating WhiskDB into applications using the native C++ Client SDK.

---

## 1. WhiskDB Server (`whiskdb`)

The `whiskdb` binary starts the persistent database server daemon. It initializes the storage engine, opens or recovers the database from disk snapshots and WAL logs, and accepts client TCP connections.

### Command-Line Usage

```sh
whiskdb [--config <path>] [--data-dir <dir>] [--host <ip>] [--port <n>] [--durability <mode>]
```

| Option | Type | Default | Description |
|---|---|---|---|
| `--config <path>` | string | `./config.toml` | Path to the TOML configuration file |
| `--data-dir <dir>` | string | `./data` | Directory where WAL files and snapshot checkpoints are stored |
| `--host <ip>` | string | `0.0.0.0` | Network interface to bind for incoming client TCP connections |
| `--port <n>` | integer | `8080` | TCP port number to listen on (`1`–`65535`) |
| `--durability <mode>` | string | `group_commit` | Durability mode: `strict`, `group_commit`, `relaxed`, or `memory_only` |
| `-h`, `--help` | flag | — | Display usage instructions and exit |

Command-line flags override the corresponding values in `config.toml`.

### Configuration File (`config.toml`)

WhiskDB reads server settings from a TOML configuration file on startup:

```toml
[config]
data_directory = "./data"
page_size = 4096
ipv4 = "0.0.0.0"
port = 8080

# Durability profile: strict | group_commit | relaxed | memory_only
durability_mode = "group_commit"

# Automatic snapshot threshold: snapshot + reset WAL after N records (0 = disable)
checkpoint_wal_records = 100000
```

| Key | Type | Default | Description |
|---|---|---|---|
| `data_directory` | string | `./data` | Target directory for persistence artifacts (`db.wal`, `snapshot.db`) |
| `page_size` | integer | `4096` | Allocation block size in bytes |
| `ipv4` / `host` | string | `0.0.0.0` | Listening IPv4 interface |
| `port` | integer | `8080` | Listening TCP port |
| `durability_mode` | string | `group_commit` | Durability mode balancing latency and crash safety |
| `checkpoint_wal_records` | integer | `100000` | Number of WAL mutations before triggering an automatic checkpoint |

---

## 2. Whisk CLI Reference (`whisk`)

The `whisk` CLI provides an interactive database shell (REPL), health inspection, log streaming, and command-driven SQL execution over the persistent TCP protocol.

### Synopsis

```sh
whisk [--host <ip>] [--port <n>] <command> [args]
```

### Global Options

| Option | Default | Description |
|---|---|---|
| `--host <ip>`, `--host=<ip>` | `127.0.0.1` | WhiskDB server hostname or IP address |
| `--port <n>`, `--port=<n>` | `8080` | WhiskDB server TCP port |
| `-h`, `--help` | — | Display CLI usage information and exit |

### Commands

| Command | Arguments | Description |
|---|---|---|
| `repl` | *(none)* | Start the interactive SQL shell with multi-line input and dot commands (default) |
| `ping` | *(none)* | Ping the server and display measured round-trip latency in microseconds |
| `sql` | `"<query>"` | Execute one or more SQL statements and render formatted result tables |
| `logs` | *(none)* | Retrieve and display recent structured server-side engine logs |
| `snapshot` | *(none)* | Trigger an immediate manual database checkpoint and WAL reset |

### Interactive REPL (`whisk repl`)

Launching `whisk` without arguments (or with `whisk repl`) enters the interactive REPL:

```sh
./build/debug/app/whisk
```

#### REPL Prompt & Multi-Line Editing
- Single statements ending in a semicolon `;` execute immediately.
- Unfinished statements show a continuation prompt (`   ...> `):

```sql
whisk> CREATE TABLE inventory (
   ...>   item_id INTEGER PRIMARY KEY,
   ...>   name TEXT NOT NULL,
   ...>   stock INTEGER DEFAULT 0
   ...> );
Query OK, 0 rows affected (round trip 0.39 ms)
```

#### Dot Commands

Inside the REPL, commands starting with a dot `.` control shell session operations:

| Dot Command | Description |
|---|---|
| `.help` | Display the list of available REPL commands |
| `.ping` | Send a ping request and display round-trip latency |
| `.logs` | Print the recent server execution log buffer |
| `.snapshot` | Trigger a manual server checkpoint |
| `.quit`, `.exit` | Disconnect and terminate the REPL session |

#### Table Formatting

`SELECT` queries are automatically rendered into aligned ASCII tables:

```sql
whisk> INSERT INTO inventory (name, stock) VALUES ('Espresso Beans', 250), ('Oat Milk', 40);
Query OK, 2 rows affected, last_insert_rowid = 2 (round trip 0.41 ms)

whisk> SELECT item_id, name, stock FROM inventory WHERE stock < 50;
+---------+----------+-------+
| item_id | name     | stock |
+---------+----------+-------+
|       2 | Oat Milk |    40 |
+---------+----------+-------+
1 row in set (0.32 ms)
```

### Non-Interactive CLI Commands

#### Query Execution (`sql`)
Run SQL queries directly from automation scripts or shell pipelines:

```sh
./build/debug/app/whisk sql "SELECT COUNT(*) AS total_items FROM inventory;"
```

Output:
```
+-------------+
| total_items |
+-------------+
|           2 |
+-------------+
1 row in set (0.28 ms)
```

#### Server Health & Latency (`ping`)
Check server availability and network round-trip time:

```sh
./build/debug/app/whisk ping
```

Output:
```
PONG in 184 us
```

#### Server Logs Inspection (`logs`)
Stream diagnostic server logs:

```sh
./build/debug/app/whisk logs
```

#### Manual Checkpoint (`snapshot`)
Force an atomic snapshot and WAL truncation:

```sh
./build/debug/app/whisk snapshot
```

!!! info "Planned Whisk Module Commands"
    Commands for managing server-side WebAssembly modules (`whisk init`, `whisk build`, `whisk test`, `whisk publish`, and `whisk call`) are part of the upcoming WASM engine release. See [Development Status](development.md) for milestone tracking.

---

## 3. C++ Client SDK (`WhiskClient`)

The C++ Client SDK provides a type-safe, zero-copy client interface for integrating WhiskDB directly into C++23 applications.

### Including the Header

Add `include/` to your compiler search path and include the SDK header:

```cpp
#include "client/whisk_client.hpp"
#include "client/table_format.hpp"
```

Link your executable against `whiskdb_core`.

### API Reference: `WhiskClient`

```cpp
namespace database {

class WhiskClient {
public:
    WhiskClient();
    ~WhiskClient();

    // Prevent copying and moving (manages an active TCP connection)
    WhiskClient(const WhiskClient&) = delete;
    WhiskClient& operator=(const WhiskClient&) = delete;

    // Connection lifecycle
    Result<void> Connect(const std::string& host, uint16_t port);
    void Close();
    bool IsConnected() const;

    // Server operations
    Result<uint64_t> Ping();
    Result<QueryResult> Query(std::string_view sql, std::span<const Value> params = {});
    Result<std::string> Snapshot();
    Result<std::string> GetLogs();
};

} // namespace database
```

#### Method Details

| Method | Parameters | Return Value | Description |
|---|---|---|---|
| `Connect()` | `const std::string& host`, `uint16_t port` | `Result<void>` | Establishes a persistent TCP connection to the WhiskDB server |
| `Close()` | *(none)* | `void` | Closes the underlying socket connection |
| `IsConnected()` | *(none)* | `bool` | Returns `true` if the socket is currently connected |
| `Ping()` | *(none)* | `Result<uint64_t>` | Sends `Opcode::Ping` and returns round-trip duration in microseconds |
| `Query()` | `std::string_view sql`, `std::span<const Value> params` | `Result<QueryResult>` | Executes SQL statement(s), returning column headers and typed rows |
| `Snapshot()` | *(none)* | `Result<std::string>` | Requests the server to flush an atomic snapshot checkpoint |
| `GetLogs()` | *(none)* | `Result<std::string>` | Retrieves the engine execution logs from the server |

### Data Model & Results

#### SQL Values (`Value`)

WhiskDB represents dynamically typed SQL values via `std::variant`:

```cpp
namespace database {

using Blob = std::vector<uint8_t>;

// 0: Null, 1: int64_t, 2: double, 3: std::string, 4: Blob
using Value = std::variant<std::monostate, int64_t, double, std::string, Blob>;

enum class ValueType : uint8_t {
    Null = 0,
    Int64 = 1,
    Double = 2,
    Text = 3,
    Blob = 4
};

} // namespace database
```

#### Query Results (`QueryResult`)

```cpp
namespace database {

struct QueryResult {
    std::vector<std::string> columns;       // Column names (empty for non-SELECT DML)
    std::vector<std::vector<Value>> rows;   // Row-major cell matrix
    idx_t rows_affected = 0;                // Number of rows modified (for INSERT/UPDATE/DELETE)
    int64_t last_insert_rowid = 0;          // Auto-generated primary key from last insert
    uint64_t exec_us = 0;                   // Server-side execution time in microseconds
    bool is_mutation = false;               // True if statement modified database state
};

} // namespace database
```

#### Result Formatting Utilities

- `std::string FormatResultTable(const QueryResult& qr)`: Renders an ASCII box-drawing table with column alignment, row counts, and execution metrics.
- `std::vector<std::string> SplitStatements(std::string& buffer)`: Splits multi-line SQL input into complete, semicolon-terminated statements while respecting quotes and escapes.

### Complete C++23 Integration Example

```cpp
#include "client/whisk_client.hpp"
#include "client/table_format.hpp"

#include <iostream>
#include <print>

int main() {
    using namespace database;

    WhiskClient client;

    // 1. Connect to WhiskDB
    auto conn_res = client.Connect("127.0.0.1", 8080);
    if (!conn_res) {
        std::println(stderr, "Connection failed: {}", conn_res.error().Message());
        return 1;
    }
    std::println("Connected to WhiskDB server.");

    // 2. Measure latency with Ping
    auto ping_res = client.Ping();
    if (ping_res) {
        std::println("Ping latency: {} us", *ping_res);
    }

    // 3. Execute DDL schema creation
    auto ddl_res = client.Query("CREATE TABLE IF NOT EXISTS accounts ("
                                "  id INTEGER PRIMARY KEY, "
                                "  owner TEXT NOT NULL, "
                                "  balance REAL NOT NULL"
                                ");");
    if (!ddl_res) {
        std::println(stderr, "DDL error: {}", ddl_res.error().Message());
        return 1;
    }

    // 4. Insert data
    auto insert_res = client.Query("INSERT INTO accounts (owner, balance) VALUES ('Alice', 1250.75), ('Bob', 300.00);");
    if (insert_res) {
        std::println("Inserted rows: {}, last rowid: {}", 
                     insert_res->rows_affected, insert_res->last_insert_rowid);
    }

    // 5. Query data and inspect typed rows
    auto query_res = client.Query("SELECT id, owner, balance FROM accounts ORDER BY id ASC;");
    if (!query_res) {
        std::println(stderr, "Query error: {}", query_res.error().Message());
        return 1;
    }

    // Print formatted table using SDK utility
    std::print("{}", FormatResultTable(*query_res));

    // Access individual cells programmatically
    for (idx_t row_idx = 0; row_idx < query_res->rows.size(); ++row_idx) {
        const auto& row = query_res->rows[row_idx];
        int64_t id = std::get<int64_t>(row[0]);
        const std::string& owner = std::get<std::string>(row[1]);
        double balance = std::get<double>(row[2]);

        std::println("Account #{}: {} (Balance: ${:.2f})", id, owner, balance);
    }

    // 6. Graceful disconnect
    client.Close();
    return 0;
}
```

### Error Handling Policy

The SDK uses the monadic `Result<T>` template rather than throwing C++ exceptions:
- **Success**: Check `if (result)` or evaluate `result.has_value()`. Retrieve the value via `*result` or `result.value()`.
- **Failure**: Extract the error details via `result.error().Code()` and `result.error().Message()`.

Operational errors (network disconnections, SQL syntax errors, constraint violations) are always returned through `Result<T>`. Exceptions are reserved for unrecoverable host failures.
