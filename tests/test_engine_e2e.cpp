#include "client/table_format.hpp"
#include "client/whisk_client.hpp"
#include "config.hpp"
#include "engine.hpp"
#include "errors.hpp"
#include "logger.hpp"
#include "network/protocol.hpp"
#include "network/server.hpp"
#include "network/tcp_client.hpp"
#include "sql/value_codec.hpp"
#include "storage/wal_manager.hpp"

#include <cassert>
#include <filesystem>
#include <format>
#include <print>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace database;

namespace {

std::filesystem::path MakeTempDir(std::string_view tag) {
    auto dir = std::filesystem::temp_directory_path() /
               std::format("whiskdb_e2e_{}_{}", static_cast<int>(::getpid()), tag);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void TestMemoryOnlyAndSql() {
    std::println("--- Testing Engine MemoryOnly & SQL Execution ---");
    auto temp_dir = MakeTempDir("mem");

    EngineOptions opts;
    opts.data_dir = temp_dir;
    opts.durability = DurabilityMode::MemoryOnly;
    opts.host = "127.0.0.1";
    opts.port = 0;

    auto eng_res = Engine::Open(opts);
    assert(eng_res.has_value());
    auto& engine = *eng_res.value();

    assert(!engine.Recovery().snapshot_loaded);
    assert(engine.Recovery().wal_records_replayed == 0);

    // Reject explicit transaction control
    auto begin_res = engine.ExecuteSql("BEGIN TRANSACTION");
    assert(!begin_res.has_value());
    assert(begin_res.error().Code() == ErrorCode::InvalidArgument);

    // Create table + insert + query
    auto create_res = engine.ExecuteSql(
        "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT NOT NULL, price REAL)");
    assert(create_res.has_value());
    assert(create_res->is_mutation);

    std::vector<Value> p1{int64_t{1}, std::string{"Keyboard"}, double{99.5}};
    auto ins1 = engine.ExecuteSql("INSERT INTO items VALUES (?1, ?2, ?3)", p1);
    assert(ins1.has_value());
    assert(ins1->rows_affected == 1);
    assert(ins1->last_insert_rowid == 1);

    std::vector<Value> p2{int64_t{2}, std::string{"Mouse"}, double{49.25}};
    auto ins2 = engine.ExecuteSql("INSERT INTO items VALUES (?1, ?2, ?3)", p2);
    assert(ins2.has_value());

    auto sel = engine.ExecuteSql("SELECT id, name, price FROM items ORDER BY id");
    assert(sel.has_value());
    assert(!sel->is_mutation);
    assert(sel->columns.size() == 3);
    assert(sel->rows.size() == 2);
    assert(std::get<int64_t>(sel->rows[0][0]) == 1);
    assert(std::get<std::string>(sel->rows[0][1]) == "Keyboard");
    assert(std::get<double>(sel->rows[0][2]) == 99.5);

    std::string formatted = FormatResultTable(*sel);
    assert(formatted.find("Keyboard") != std::string::npos);
    assert(formatted.find("Mouse") != std::string::npos);

    // Checkpoints are disabled in MemoryOnly mode
    auto cp = engine.Checkpoint();
    assert(!cp.has_value());

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
}

void TestWalRecoveryAndSnapshotOverTcp() {
    std::println("--- Testing Engine TCP Server, WAL Recovery & Snapshot Checkpoint ---");
    auto temp_dir = MakeTempDir("durable");

    // Phase 1: Start server in GroupCommit mode, insert rows over WhiskClient, take snapshot, insert more
    {
        EngineOptions opts;
        opts.data_dir = temp_dir;
        opts.durability = DurabilityMode::GroupCommit;
        opts.host = "127.0.0.1";
        opts.port = 0;

        auto eng_res = Engine::Open(opts);
        assert(eng_res.has_value());
        auto& engine = *eng_res.value();
        assert(engine.StartServer().has_value());
        const uint16_t port = engine.Port();
        assert(port > 0);

        WhiskClient client;
        assert(client.Connect("127.0.0.1", port).has_value());

        auto ping_res = client.Ping();
        assert(ping_res.has_value());

        assert(client.Query("CREATE TABLE accounts (id INTEGER PRIMARY KEY, balance INTEGER NOT NULL)").has_value());
        assert(client.Query("INSERT INTO accounts VALUES (1, 1000)").has_value());
        assert(client.Query("INSERT INTO accounts VALUES (2, 2000)").has_value());

        // Trigger snapshot over TCP
        auto snap_msg = client.Snapshot();
        assert(snap_msg.has_value());
        assert(snap_msg->find("LSN") != std::string::npos);

        // Mutations after the snapshot (will live in WAL tail)
        assert(client.Query("UPDATE accounts SET balance = 1500 WHERE id = 1").has_value());
        assert(client.Query("INSERT INTO accounts VALUES (3, 3000)").has_value());

        // Fetch server logs over TCP
        auto logs = client.GetLogs();
        assert(logs.has_value());
        assert(!logs->empty());

        client.Close();
        engine.StopServer();
    }

    // Phase 2: Reopen Engine from the same directory; verify snapshot + WAL replay
    {
        EngineOptions opts;
        opts.data_dir = temp_dir;
        opts.durability = DurabilityMode::Strict;
        opts.host = "127.0.0.1";
        opts.port = 0;

        auto eng_res = Engine::Open(opts);
        assert(eng_res.has_value());
        auto& engine = *eng_res.value();

        assert(engine.Recovery().snapshot_loaded);
        assert(engine.Recovery().snapshot_lsn >= 3);
        assert(engine.Recovery().wal_records_replayed == 2);

        auto sel = engine.ExecuteSql("SELECT id, balance FROM accounts ORDER BY id");
        assert(sel.has_value());
        assert(sel->rows.size() == 3);
        assert(std::get<int64_t>(sel->rows[0][0]) == 1);
        assert(std::get<int64_t>(sel->rows[0][1]) == 1500);
        assert(std::get<int64_t>(sel->rows[1][0]) == 2);
        assert(std::get<int64_t>(sel->rows[1][1]) == 2000);
        assert(std::get<int64_t>(sel->rows[2][0]) == 3);
        assert(std::get<int64_t>(sel->rows[2][1]) == 3000);
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
}

void TestConcurrentClientsDurable() {
    std::println("--- Testing Concurrent TCP Clients with GroupCommit + Reopen ---");
    auto temp_dir = MakeTempDir("concurrent");

    constexpr idx_t kThreads = 8;
    constexpr idx_t kInsertsPerThread = 50;

    {
        EngineOptions opts;
        opts.data_dir = temp_dir;
        opts.durability = DurabilityMode::GroupCommit;
        opts.host = "127.0.0.1";
        opts.port = 0;
        opts.io_threads = 4;

        auto eng_res = Engine::Open(opts);
        assert(eng_res.has_value());
        auto& engine = *eng_res.value();
        assert(engine.ExecuteSql("CREATE TABLE events (id INTEGER PRIMARY KEY AUTOINCREMENT, worker INTEGER, seq INTEGER)").has_value());
        assert(engine.StartServer().has_value());
        const uint16_t port = engine.Port();

        std::vector<std::thread> workers;
        for (idx_t t = 0; t < kThreads; t++) {
            workers.emplace_back([port, t]() {
                WhiskClient client;
                assert(client.Connect("127.0.0.1", port).has_value());
                for (idx_t i = 0; i < kInsertsPerThread; i++) {
                    std::vector<Value> params{static_cast<int64_t>(t), static_cast<int64_t>(i)};
                    auto r = client.Query("INSERT INTO events (worker, seq) VALUES (?1, ?2)", params);
                    assert(r.has_value());
                }
                client.Close();
            });
        }
        for (auto& w : workers) {
            w.join();
        }
        engine.StopServer();
    }

    // Reopen and verify all kThreads * kInsertsPerThread rows were durably logged and replayed
    {
        EngineOptions opts;
        opts.data_dir = temp_dir;
        opts.durability = DurabilityMode::MemoryOnly; // just to inspect
        opts.durability = DurabilityMode::Relaxed;
        auto eng_res = Engine::Open(opts);
        assert(eng_res.has_value());
        auto sel = (*eng_res)->ExecuteSql("SELECT COUNT(*) FROM events");
        assert(sel.has_value());
        assert(std::get<int64_t>(sel->rows[0][0]) == static_cast<int64_t>(kThreads * kInsertsPerThread));
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
}

void TestCliBinaryEndToEnd() {
    std::println("--- Testing `whisk` CLI Binary End-to-End Against Live Server ---");
    if (!std::filesystem::exists("./whisk")) {
        return;
    }
    auto temp_dir = MakeTempDir("cli_bin");

    EngineOptions opts;
    opts.data_dir = temp_dir;
    opts.durability = DurabilityMode::GroupCommit;
    opts.host = "127.0.0.1";
    opts.port = 0;

    auto eng_res = Engine::Open(opts);
    assert(eng_res.has_value());
    auto& engine = *eng_res.value();
    assert(engine.StartServer().has_value());
    const uint16_t port = engine.Port();

    const auto run_cmd = [port](std::string_view args) -> int {
        std::string cmd = std::format("./whisk --host 127.0.0.1 --port {} {} >/dev/null", port, args);
        return std::system(cmd.c_str());
    };

    assert(run_cmd("ping") == 0);
    assert(run_cmd("sql \"CREATE TABLE cli_t (id INTEGER PRIMARY KEY, v TEXT)\"") == 0);
    assert(run_cmd("sql \"INSERT INTO cli_t VALUES (1, 'from_cli')\"") == 0);
    assert(run_cmd("sql \"SELECT * FROM cli_t\"") == 0);
    assert(run_cmd("snapshot") == 0);
    assert(run_cmd("logs") == 0);

    auto sel = engine.ExecuteSql("SELECT v FROM cli_t WHERE id = 1");
    assert(sel.has_value());
    assert(sel->rows.size() == 1);
    assert(std::get<std::string>(sel->rows[0][0]) == "from_cli");

    engine.StopServer();
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
}

void TestEngineOptionsFromConfig() {
    std::println("--- Testing EngineOptions::FromConfig Wiring ---");
    // Default config
    {
        Config cfg;
        auto opts = EngineOptions::FromConfig(cfg);
        assert(opts.has_value());
        assert(opts->wal_group_commit_ms == 10);
        assert(opts->wasm_max_fuel == 100'000'000);
        assert(opts->wasm_max_memory_mb == 64);
    }
    // Custom config
    {
        std::string toml = R"(
            data_dir = "/tmp/whisk_custom"
            host = "0.0.0.0"
            port = 9090
            durability_mode = "relaxed"
            checkpoint_wal_records = 50000
            wal_group_commit_ms = 25
            wasm_max_fuel = 50000000
            wasm_max_memory_mb = 128
        )";
        Config cfg = Config::FromString(toml);
        assert(cfg.IsValid());
        auto opts = EngineOptions::FromConfig(cfg);
        assert(opts.has_value());
        assert(opts->data_dir == "/tmp/whisk_custom");
        assert(opts->host == "0.0.0.0");
        assert(opts->port == 9090);
        assert(opts->durability == DurabilityMode::Relaxed);
        assert(opts->checkpoint_wal_records == 50000);
        assert(opts->wal_group_commit_ms == 25);
        assert(opts->wasm_max_fuel == 50000000);
        assert(opts->wasm_max_memory_mb == 128);
    }
}

void TestServerForwardsWasmOpcodes() {
    std::println("--- Testing Server Forwards WASM Opcodes (WhiskPublish, WhiskCall, WhiskList) ---");
    std::vector<Opcode> received_opcodes;
    Server server("127.0.0.1", 0, [&received_opcodes](const Request& req) -> Response {
        received_opcodes.push_back(req.opcode);
        Response resp;
        resp.status = ErrorCode::Ok;
        std::string msg = "handler_invoked";
        resp.payload.assign(msg.begin(), msg.end());
        return resp;
    });
    assert(server.Start().has_value());
    const uint16_t port = server.Port();

    TcpClient client;
    assert(client.Connect("127.0.0.1", port).has_value());

    for (auto op : {Opcode::WhiskPublish, Opcode::WhiskCall, Opcode::WhiskList}) {
        Request req;
        req.opcode = op;
        req.payload = {1, 2, 3};
        auto resp_res = client.Send(req);
        assert(resp_res.has_value());
        assert(resp_res->status == ErrorCode::Ok);
        std::string payload_str(resp_res->payload.begin(), resp_res->payload.end());
        assert(payload_str == "handler_invoked");
    }

    assert(received_opcodes.size() == 3);
    assert(received_opcodes[0] == Opcode::WhiskPublish);
    assert(received_opcodes[1] == Opcode::WhiskCall);
    assert(received_opcodes[2] == Opcode::WhiskList);

    client.Close();
    server.Stop();
}

void TestEngineRecoverSqlBatchAtomic() {
    std::println("--- Testing Engine::Recover() SqlBatch Atomic Replay & Rollback ---");
    auto temp_dir = MakeTempDir("batch_atomic");

    // Phase 1: Append a valid batch to WAL manually, then Open Engine
    {
        WalOptions wal_opts;
        wal_opts.path = temp_dir / "whiskdb.wal";
        wal_opts.mode = DurabilityMode::Strict;
        auto wal_res = WalManager::Open(wal_opts, 1);
        assert(wal_res.has_value());
        auto& wal = *wal_res.value();

        std::vector<DecodedStatement> batch = {
            {"CREATE TABLE batch_tab (id INTEGER PRIMARY KEY, v TEXT)", {}},
            {"INSERT INTO batch_tab VALUES (?1, ?2)", {int64_t(1), std::string("first")}},
            {"INSERT INTO batch_tab VALUES (?1, ?2)", {int64_t(2), std::string("second")}}
        };
        std::vector<uint8_t> payload;
        EncodeStatementBatch(payload, batch);

        auto app = wal.Append(WalRecordType::SqlBatch, payload);
        assert(app.has_value() && *app == 1);
    }

    // Recover via Engine::Open
    {
        EngineOptions opts;
        opts.data_dir = temp_dir;
        opts.durability = DurabilityMode::Strict;
        auto eng_res = Engine::Open(opts);
        assert(eng_res.has_value());
        auto& engine = *eng_res.value();

        assert(engine.Recovery().wal_records_replayed == 1);
        auto sel = engine.ExecuteSql("SELECT id, v FROM batch_tab ORDER BY id");
        assert(sel.has_value());
        assert(sel->rows.size() == 2);
        assert(std::get<int64_t>(sel->rows[0][0]) == 1);
        assert(std::get<std::string>(sel->rows[0][1]) == "first");
        assert(std::get<int64_t>(sel->rows[1][0]) == 2);
        assert(std::get<std::string>(sel->rows[1][1]) == "second");
    }

    // Phase 2: Append a failing mid-batch record to the WAL
    {
        WalOptions wal_opts;
        wal_opts.path = temp_dir / "whiskdb.wal";
        wal_opts.mode = DurabilityMode::Strict;
        auto wal_res = WalManager::Open(wal_opts, 2);
        assert(wal_res.has_value());
        auto& wal = *wal_res.value();

        std::vector<DecodedStatement> failing_batch = {
            {"INSERT INTO batch_tab VALUES (?1, ?2)", {int64_t(3), std::string("third")}},
            {"INSERT INTO batch_tab VALUES (?1, ?2)", {int64_t(1), std::string("duplicate_key")}}, // will fail!
            {"INSERT INTO batch_tab VALUES (?1, ?2)", {int64_t(4), std::string("fourth")}}
        };
        std::vector<uint8_t> payload;
        EncodeStatementBatch(payload, failing_batch);

        auto app = wal.Append(WalRecordType::SqlBatch, payload);
        assert(app.has_value() && *app == 2);
    }

    // Engine::Open should fail with CorruptedData
    {
        EngineOptions opts;
        opts.data_dir = temp_dir;
        opts.durability = DurabilityMode::Strict;
        auto eng_res = Engine::Open(opts);
        assert(!eng_res.has_value());
        assert(eng_res.error().Code() == ErrorCode::CorruptedData);
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
}

} // namespace

int main() {
    Logger::SetLevel(LogLevel::Info);
    std::println("=========================================");
    std::println("Running WhiskDB End-to-End MVP Test Suite");
    std::println("=========================================");

    TestMemoryOnlyAndSql();
    TestWalRecoveryAndSnapshotOverTcp();
    TestConcurrentClientsDurable();
    TestCliBinaryEndToEnd();
    TestEngineOptionsFromConfig();
    TestServerForwardsWasmOpcodes();
    TestEngineRecoverSqlBatchAtomic();

    std::println("=========================================");
    std::println("ALL E2E MVP TESTS PASSED!");
    std::println("=========================================");
    return 0;
}
