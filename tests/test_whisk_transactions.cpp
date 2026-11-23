#include "engine.hpp"
#include "logger.hpp"
#include "network/whisk_codec.hpp"
#include "wasm_test_helper.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace database;

namespace {

std::filesystem::path MakeTempDir(std::string_view tag) {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = std::filesystem::temp_directory_path() /
               std::format("whiskdb_tx_test_{}_{}", tag, now);
    std::filesystem::create_directories(dir);
    return dir;
}

std::filesystem::path FindCounterWhiskCpp() {
#ifdef WHISKDB_SOURCE_DIR
    return std::filesystem::path(WHISKDB_SOURCE_DIR) / "examples" / "counter_whisk.cpp";
#else
    return std::filesystem::path("examples/counter_whisk.cpp");
#endif
}

void TestPublishCallAndWireProtocol(const std::vector<uint8_t>& counter_wasm) {
    auto dir = MakeTempDir("wire");
    EngineOptions opts;
    opts.data_dir = dir;
    opts.durability = DurabilityMode::MemoryOnly;

    auto eng_res = Engine::Open(opts);
    assert(eng_res.has_value());
    auto& engine = *eng_res.value();

    // 1. Direct PublishModule + CallWhisk
    auto pub_res = engine.PublishModule("counter", counter_wasm);
    assert(pub_res.has_value());
    assert(pub_res->module_name == "counter");
    assert(pub_res->version == 1);
    assert(!pub_res->sha256.empty());

    auto inc1 = engine.CallWhisk("counter", "increment", R"({"name":"hits","delta":5})");
    assert(inc1.has_value());
    assert(inc1->result_json == R"({"name":"hits","val":5})");
    assert(inc1->fuel_used > 0);

    auto get1 = engine.CallWhisk("counter", "get", R"({"name":"hits"})");
    assert(get1.has_value());
    assert(get1->result_json == R"({"name":"hits","val":5})");

    auto stats = engine.GetSqlCacheStats();
    assert(stats.hits + stats.misses > 0);

    // 2. Wire protocol via Engine::Handle
    // 2a. WhiskPublish (re-publish v2)
    Request pub_req;
    pub_req.opcode = Opcode::WhiskPublish;
    auto enc_pub = EncodeWhiskPublishRequest(pub_req.payload, WhiskPublishRequest{"counter", counter_wasm});
    assert(enc_pub.has_value());
    Response pub_wire = engine.Handle(pub_req);
    assert(pub_wire.status == ErrorCode::Ok);
    auto dec_pub = DecodeWhiskPublishResponse(pub_wire.payload);
    assert(dec_pub.has_value());
    assert(dec_pub->module_name == "counter");
    assert(dec_pub->version == 2);

    // 2b. WhiskCall
    Request call_req;
    call_req.opcode = Opcode::WhiskCall;
    auto enc_call = EncodeWhiskCallRequest(call_req.payload,
                                           WhiskCallRequest{"counter", "increment", R"({"name":"hits","delta":3})"});
    assert(enc_call.has_value());
    Response call_wire = engine.Handle(call_req);
    assert(call_wire.status == ErrorCode::Ok);
    auto dec_call = DecodeWhiskCallResponse(call_wire.payload);
    assert(dec_call.has_value());
    assert(dec_call->result_json == R"({"name":"hits","val":8})");
    assert(dec_call->fuel_used > 0);

    // 2c. WhiskList
    Request list_req;
    list_req.opcode = Opcode::WhiskList;
    Response list_wire = engine.Handle(list_req);
    assert(list_wire.status == ErrorCode::Ok);
    auto dec_list = DecodeWhiskListResponse(list_wire.payload);
    assert(dec_list.has_value());
    assert(dec_list->modules.size() == 1);
    assert(dec_list->modules[0].name == "counter");
    assert(dec_list->modules[0].version == 2);

    // 2d. WhiskList with non-empty payload -> ProtocolError
    Request bad_list_req;
    bad_list_req.opcode = Opcode::WhiskList;
    bad_list_req.payload = {0x01};
    Response bad_list_wire = engine.Handle(bad_list_req);
    assert(bad_list_wire.status == ErrorCode::ProtocolError);

    // 2e. Logs with custom limit
    LOG_INFO("whisk_tx_test_log_1");
    LOG_INFO("whisk_tx_test_log_2");
    LOG_INFO("whisk_tx_test_log_3");
    Request logs_req;
    logs_req.opcode = Opcode::Logs;
    EncodeLogsRequest(logs_req.payload, LogsRequest{2});
    Response logs_wire = engine.Handle(logs_req);
    assert(logs_wire.status == ErrorCode::Ok);
    std::string logs_text(logs_wire.payload.begin(), logs_wire.payload.end());
    size_t line_count = 0;
    for (char c : logs_text) {
        if (c == '\n') {
            line_count++;
        }
    }
    assert(line_count <= 2);

    std::filesystem::remove_all(dir);
    std::cout << "[PASS] TestPublishCallAndWireProtocol\n";
}

void TestAtomicCommitAndRollback(const std::filesystem::path& work_dir) {
    constexpr std::string_view kTxModuleSrc = R"cpp(
#include "whisk_guest.hpp"

WHISK_INIT() {
    if (whisk::Exec("CREATE TABLE IF NOT EXISTS items (id INTEGER PRIMARY KEY, label TEXT NOT NULL);") < 0) {
        return whisk::Error("init failed");
    }
    return whisk::Ok("{}");
}

WHISK_FUNC(commit_two) {
    if (whisk::Exec("INSERT INTO items (id, label) VALUES (1, 'a');") < 0) {
        return whisk::Error("insert 1 failed");
    }
    if (whisk::Exec("INSERT INTO items (id, label) VALUES (2, 'b');") < 0) {
        return whisk::Error("insert 2 failed");
    }
    return whisk::Ok("{\"inserted\":2}");
}

WHISK_FUNC(abort_midway) {
    if (whisk::Exec("INSERT INTO items (id, label) VALUES (10, 'temp1');") < 0) {
        return whisk::Error("insert 10 failed");
    }
    if (whisk::Exec("INSERT INTO items (id, label) VALUES (11, 'temp2');") < 0) {
        return whisk::Error("insert 11 failed");
    }
    return whisk::Error("abort");
}

WHISK_FUNC(trap_midway) {
    (void)whisk::Exec("INSERT INTO items (id, label) VALUES (20, 'trapped');");
    __builtin_trap();
}
)cpp";

    auto wasm_res = test_util::CompileWhiskSourceToWasm(kTxModuleSrc, work_dir, "tx_mod");
    assert(wasm_res.has_value());

    auto dir = MakeTempDir("rollback");
    {
        EngineOptions opts;
        opts.data_dir = dir;
        opts.durability = DurabilityMode::Strict;
        auto eng_res = Engine::Open(opts);
        assert(eng_res.has_value());
        auto& engine = *eng_res.value();

        auto pub = engine.PublishModule("tx_mod", *wasm_res);
        assert(pub.has_value());

        // Abort midway -> error, 0 rows
        auto abort_res = engine.CallWhisk("tx_mod", "abort_midway", "{}");
        assert(!abort_res.has_value());
        assert(abort_res.error().Code() == ErrorCode::WasmError);

        auto q1 = engine.ExecuteSql("SELECT COUNT(*) FROM items;");
        assert(q1.has_value());
        assert(std::get<int64_t>(q1->rows[0][0]) == 0);

        // Trap midway -> trap error, 0 rows
        auto trap_res = engine.CallWhisk("tx_mod", "trap_midway", "{}");
        assert(!trap_res.has_value());

        auto q2 = engine.ExecuteSql("SELECT COUNT(*) FROM items;");
        assert(q2.has_value());
        assert(std::get<int64_t>(q2->rows[0][0]) == 0);

        // Commit two -> 2 rows
        auto ok_res = engine.CallWhisk("tx_mod", "commit_two", "{}");
        assert(ok_res.has_value());
        assert(ok_res->result_json == R"({"inserted":2})");

        auto q3 = engine.ExecuteSql("SELECT COUNT(*) FROM items;");
        assert(q3.has_value());
        assert(std::get<int64_t>(q3->rows[0][0]) == 2);
    }

    // Reopen from WAL: verify aborted/trapped writes were NOT logged to WAL, only the 2 committed rows exist
    {
        EngineOptions opts;
        opts.data_dir = dir;
        opts.durability = DurabilityMode::Strict;
        auto eng_res = Engine::Open(opts);
        assert(eng_res.has_value());
        auto& engine = *eng_res.value();

        auto q = engine.ExecuteSql("SELECT COUNT(*) FROM items;");
        assert(q.has_value());
        assert(std::get<int64_t>(q->rows[0][0]) == 2);
    }

    std::filesystem::remove_all(dir);
    std::cout << "[PASS] TestAtomicCommitAndRollback\n";
}

void TestNotFoundAndForbiddenAbiCalls(const std::vector<uint8_t>& counter_wasm) {
    auto dir = MakeTempDir("notfound");
    EngineOptions opts;
    opts.data_dir = dir;
    opts.durability = DurabilityMode::MemoryOnly;

    auto eng_res = Engine::Open(opts);
    assert(eng_res.has_value());
    auto& engine = *eng_res.value();

    auto pub = engine.PublishModule("counter", counter_wasm);
    assert(pub.has_value());

    // Missing module
    auto r1 = engine.CallWhisk("nonexistent", "increment", "{}");
    assert(!r1.has_value());
    assert(r1.error().Code() == ErrorCode::NotFound);

    // Missing function
    auto r2 = engine.CallWhisk("counter", "does_not_exist", "{}");
    assert(!r2.has_value());
    assert(r2.error().Code() == ErrorCode::NotFound);

    // Forbidden internal ABI calls
    auto r3 = engine.CallWhisk("counter", "whisk_alloc", "{}");
    assert(!r3.has_value());
    assert(r3.error().Code() == ErrorCode::NotFound);

    auto r4 = engine.CallWhisk("counter", "__init__", "{}");
    assert(!r4.has_value());
    assert(r4.error().Code() == ErrorCode::NotFound);

    std::filesystem::remove_all(dir);
    std::cout << "[PASS] TestNotFoundAndForbiddenAbiCalls\n";
}

void TestRestartDurability(const std::vector<uint8_t>& counter_wasm, DurabilityMode mode, std::string_view tag) {
    auto dir = MakeTempDir(tag);

    // Phase 1: Publish + mutate + close
    {
        EngineOptions opts;
        opts.data_dir = dir;
        opts.durability = mode;
        auto eng_res = Engine::Open(opts);
        assert(eng_res.has_value());
        auto& engine = *eng_res.value();

        auto pub = engine.PublishModule("counter", counter_wasm);
        assert(pub.has_value());

        auto inc = engine.CallWhisk("counter", "increment", R"({"name":"persisted","delta":10})");
        assert(inc.has_value());
        assert(inc->result_json == R"({"name":"persisted","val":10})");
    }

    // Phase 2: Reopen -> verify module auto-rehydrated and state intact, then Checkpoint + post-snapshot mutation
    {
        EngineOptions opts;
        opts.data_dir = dir;
        opts.durability = mode;
        auto eng_res = Engine::Open(opts);
        assert(eng_res.has_value());
        auto& engine = *eng_res.value();

        auto list = engine.ListModules();
        assert(list.has_value());
        assert(list->modules.size() == 1);
        assert(list->modules[0].name == "counter");

        auto get = engine.CallWhisk("counter", "get", R"({"name":"persisted"})");
        assert(get.has_value());
        assert(get->result_json == R"({"name":"persisted","val":10})");

        auto ckpt = engine.Checkpoint();
        assert(ckpt.has_value());

        auto inc2 = engine.CallWhisk("counter", "increment", R"({"name":"persisted","delta":7})");
        assert(inc2.has_value());
        assert(inc2->result_json == R"({"name":"persisted","val":17})");
    }

    // Phase 3: Reopen after snapshot + post-snapshot WAL -> verify both survive
    {
        EngineOptions opts;
        opts.data_dir = dir;
        opts.durability = mode;
        auto eng_res = Engine::Open(opts);
        assert(eng_res.has_value());
        auto& engine = *eng_res.value();

        assert(engine.Recovery().snapshot_loaded);
        assert(engine.Recovery().wal_records_replayed >= 1);

        auto list = engine.ListModules();
        assert(list.has_value());
        assert(list->modules.size() == 1);
        assert(list->modules[0].name == "counter");

        auto get = engine.CallWhisk("counter", "get", R"({"name":"persisted"})");
        assert(get.has_value());
        assert(get->result_json == R"({"name":"persisted","val":17})");
    }

    std::filesystem::remove_all(dir);
    std::cout << "[PASS] TestRestartDurability (" << tag << ")\n";
}

} // namespace

int main() {
    auto work_dir = MakeTempDir("wasm_build");
    auto counter_wasm_res = test_util::CompileWhiskCppToWasm(
        FindCounterWhiskCpp(), work_dir / "counter_whisk.wasm");
    assert(counter_wasm_res.has_value());
    const auto& counter_wasm = *counter_wasm_res;

    TestPublishCallAndWireProtocol(counter_wasm);
    TestAtomicCommitAndRollback(work_dir);
    TestNotFoundAndForbiddenAbiCalls(counter_wasm);
    TestRestartDurability(counter_wasm, DurabilityMode::Strict, "strict");
    TestRestartDurability(counter_wasm, DurabilityMode::GroupCommit, "group_commit");

    std::filesystem::remove_all(work_dir);
    std::cout << "All test_whisk_transactions tests passed!\n";
    return 0;
}
