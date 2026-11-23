#include "client/whisk_client.hpp"
#include "engine.hpp"
#include "errors.hpp"
#include "logger.hpp"
#include "network/protocol.hpp"
#include "network/tcp_client.hpp"
#include "network/whisk_codec.hpp"
#include "wasm_test_helper.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

using namespace database;

namespace {

std::filesystem::path MakeTempDir(std::string_view tag) {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = std::filesystem::temp_directory_path() /
               std::format("whiskdb_sandbox_safety_{}_{}_{}", static_cast<int>(::getpid()), tag, now);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::filesystem::path FindAdversaryFixtureCpp() {
#ifdef WHISKDB_SOURCE_DIR
    return std::filesystem::path(WHISKDB_SOURCE_DIR) / "tests" / "fixtures" / "sandbox_adversary_whisk.cpp";
#else
    return std::filesystem::path("tests/fixtures/sandbox_adversary_whisk.cpp");
#endif
}

int64_t CountRows(Engine& engine, std::string_view table_name) {
    auto res = engine.ExecuteSql(std::format("SELECT COUNT(*) FROM {}", table_name));
    assert(res.has_value());
    assert(res->rows.size() == 1);
    return std::get<int64_t>(res->rows[0][0]);
}

void VerifyPostFaultLiveness(Engine& engine, uint16_t port, int64_t& next_id) {
    const int64_t inproc_id = next_id++;
    const int64_t tcp_id = next_id++;

    // 1. In-process Engine CallWhisk + ExecuteSql
    auto call_res = engine.CallWhisk(
        "adversary",
        "valid_insert",
        std::format(R"({{"id":{},"label":"inproc_{}"}})", inproc_id, inproc_id));
    assert(call_res.has_value());
    assert(call_res->result_json == R"({"status":"ok"})");

    auto sql_res = engine.ExecuteSql(
        std::format("SELECT label FROM safety_items WHERE id = {}", inproc_id));
    assert(sql_res.has_value());
    assert(sql_res->rows.size() == 1);
    assert(std::get<std::string>(sql_res->rows[0][0]) == std::format("inproc_{}", inproc_id));

    // 2. Live TCP WhiskCall via TcpClient
    TcpClient raw_client;
    assert(raw_client.Connect("127.0.0.1", port).has_value());

    Request call_req;
    call_req.opcode = Opcode::WhiskCall;
    auto enc = EncodeWhiskCallRequest(
        call_req.payload,
        WhiskCallRequest{
            "adversary",
            "valid_insert",
            std::format(R"({{"id":{},"label":"tcp_{}"}})", tcp_id, tcp_id)});
    assert(enc.has_value());

    auto resp_res = raw_client.Send(call_req);
    assert(resp_res.has_value());
    assert(resp_res->status == ErrorCode::Ok);
    auto dec = DecodeWhiskCallResponse(resp_res->payload);
    assert(dec.has_value());
    assert(dec->result_json == R"({"status":"ok"})");
    raw_client.Close();

    // 3. Live TCP SQL via WhiskClient
    WhiskClient sql_client;
    assert(sql_client.Connect("127.0.0.1", port).has_value());
    auto tcp_sel = sql_client.Query(
        std::format("SELECT label FROM safety_items WHERE id = {}", tcp_id));
    assert(tcp_sel.has_value());
    assert(tcp_sel->rows.size() == 1);
    assert(std::get<std::string>(tcp_sel->rows[0][0]) == std::format("tcp_{}", tcp_id));
    sql_client.Close();

    // Clean up liveness probe rows so subsequent attack assertions start from 0 rows
    auto del_res = engine.ExecuteSql(
        std::format("DELETE FROM safety_items WHERE id IN ({}, {})", inproc_id, tcp_id));
    assert(del_res.has_value());
    assert(CountRows(engine, "safety_items") == 0);
}

void RunSandboxSafetySuite() {
    auto temp_dir = MakeTempDir("suite");
    auto wasm_path = temp_dir / "sandbox_adversary_whisk.wasm";

    auto wasm_res = test_util::CompileWhiskCppToWasm(FindAdversaryFixtureCpp(), wasm_path);
    assert(wasm_res.has_value());
    const auto& wasm_bytes = wasm_res.value();

    auto data_dir = temp_dir / "data";
    EngineOptions opts;
    opts.data_dir = data_dir;
    opts.durability = DurabilityMode::Strict;
    opts.host = "127.0.0.1";
    opts.port = 0;
    opts.wasm_max_fuel = 1'000'000;
    opts.wasm_max_memory_mb = 4;

    int64_t next_liveness_id = 1000;

    // =========================================================================
    // Scenario 1: trap_after_insert + WAL durability verification across restart
    // =========================================================================
    {
        std::println("--- [1/6] Testing trap_after_insert & WAL Rollback Across Restart ---");
        auto eng_res = Engine::Open(opts);
        assert(eng_res.has_value());
        auto& engine = *eng_res.value();
        assert(engine.StartServer().has_value());

        auto pub_res = engine.PublishModule("adversary", wasm_bytes);
        if (!pub_res.has_value()) {
            std::println("PublishModule failed: {}", pub_res.error().ToString());
        }
        assert(pub_res.has_value());
        assert(CountRows(engine, "safety_items") == 0);

        auto trap_res = engine.CallWhisk("adversary", "trap_after_insert", "{}");
        assert(!trap_res.has_value());
        assert(trap_res.error().Code() == ErrorCode::WasmTrap);
        assert(CountRows(engine, "safety_items") == 0);

        VerifyPostFaultLiveness(engine, engine.Port(), next_liveness_id);
        engine.StopServer();
    }

    // Reopen engine from the same data_dir to verify WAL has no dirty committed row
    auto eng_res = Engine::Open(opts);
    assert(eng_res.has_value());
    auto& engine = *eng_res.value();
    assert(engine.StartServer().has_value());
    const uint16_t port = engine.Port();
    assert(port > 0);
    assert(CountRows(engine, "safety_items") == 0);

    // =========================================================================
    // Scenario 2: infinite_loop_after_insert (fuel exhaustion < 500 ms)
    // =========================================================================
    {
        std::println("--- [2/6] Testing infinite_loop_after_insert (Fuel Exhaustion) ---");
        auto start = std::chrono::steady_clock::now();
        auto loop_res = engine.CallWhisk("adversary", "infinite_loop_after_insert", "{}");
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count();

        assert(!loop_res.has_value());
        assert(loop_res.error().Code() == ErrorCode::WasmFuelExhausted);
        assert(elapsed_ms < 500);
        assert(CountRows(engine, "safety_items") == 0);
        std::println("    Fuel exhaustion trapped in {} ms", elapsed_ms);

        VerifyPostFaultLiveness(engine, port, next_liveness_id);
    }

    // =========================================================================
    // Scenario 3: oob_pointer_attack
    // =========================================================================
    {
        std::println("--- [3/6] Testing oob_pointer_attack ---");
        auto oob_res = engine.CallWhisk("adversary", "oob_pointer_attack", "{}");
        assert(oob_res.has_value());

        const int64_t expected_oob = -static_cast<int64_t>(ErrorCode::WasmMemoryOutOfBounds);
        const std::string expected_json =
            std::format(R"({{"exec_rc":{},"query_rc":{}}})", expected_oob, expected_oob);
        assert(oob_res->result_json == expected_json);

        VerifyPostFaultLiveness(engine, port, next_liveness_id);
    }

    // =========================================================================
    // Scenario 4: stack_overflow_recursion
    // =========================================================================
    {
        std::println("--- [4/6] Testing stack_overflow_recursion ---");
        auto stack_res = engine.CallWhisk("adversary", "stack_overflow_recursion", "{}");
        assert(!stack_res.has_value());
        assert(stack_res.error().Code() == ErrorCode::WasmTrap);
        assert(CountRows(engine, "safety_items") == 0);

        VerifyPostFaultLiveness(engine, port, next_liveness_id);
    }

    // =========================================================================
    // Scenario 5: memory_bomb
    // =========================================================================
    {
        std::println("--- [5/6] Testing memory_bomb (4 MiB Store Limiter) ---");
        auto mem_res = engine.CallWhisk("adversary", "memory_bomb", "{}");
        assert(mem_res.has_value());
        assert(mem_res->result_json == R"({"grow_rc":-1})");

        VerifyPostFaultLiveness(engine, port, next_liveness_id);
    }

    // =========================================================================
    // Scenario 6: escape_transaction_attack
    // =========================================================================
    {
        std::println("--- [6/6] Testing escape_transaction_attack ---");
        auto esc_res = engine.CallWhisk("adversary", "escape_transaction_attack", "{}");
        assert(!esc_res.has_value());

        const int64_t expected_invalid = -static_cast<int64_t>(ErrorCode::InvalidArgument);
        const int64_t expected_denied = -static_cast<int64_t>(ErrorCode::AccessDenied);
        const std::string expected_summary = std::format(
            R"({{"commit_rc":{},"rollback_rc":{},"drop_rc":{}}})",
            expected_invalid,
            expected_invalid,
            expected_denied);
        assert(esc_res.error().Message().find(expected_summary) != std::string::npos);

        // Verify _whisk_modules is still intact and safety_items rolled back to 0
        assert(CountRows(engine, "_whisk_modules") >= 1);
        assert(CountRows(engine, "safety_items") == 0);

        VerifyPostFaultLiveness(engine, port, next_liveness_id);
    }

    engine.StopServer();
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
}

} // namespace

int main() {
    Logger::SetLevel(LogLevel::Info);
    std::println("=========================================");
    std::println("Running WASM Sandbox Safety Test Suite");
    std::println("=========================================");

    RunSandboxSafetySuite();

    std::println("=========================================");
    std::println("ALL WASM SANDBOX SAFETY TESTS PASSED!");
    std::println("=========================================");
    return 0;
}
