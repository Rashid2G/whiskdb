#include "engine.hpp"
#include "errors.hpp"
#include "network/protocol.hpp"
#include "network/tcp_client.hpp"
#include "network/whisk_codec.hpp"
#include "wasm_test_helper.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace database;

namespace {

std::filesystem::path MakeTempDir(std::string_view tag) {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = std::filesystem::temp_directory_path() /
               std::format("whiskdb_showcase_{}_{}_{}", static_cast<int>(::getpid()), tag, now);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::filesystem::path FindExampleCpp(std::string_view filename) {
#ifdef WHISKDB_SOURCE_DIR
    return std::filesystem::path(WHISKDB_SOURCE_DIR) / "examples" / filename;
#else
    return std::filesystem::path("examples") / filename;
#endif
}

Result<WhiskCallResponse> CallWhiskOverTcp(
    TcpClient& client,
    std::string_view module_name,
    std::string_view func_name,
    std::string_view args_json) {
    Request req;
    req.opcode = Opcode::WhiskCall;
    auto enc = EncodeWhiskCallRequest(
        req.payload,
        WhiskCallRequest{std::string(module_name), std::string(func_name), std::string(args_json)});
    if (!enc.has_value()) {
        return std::unexpected(enc.error());
    }

    auto resp_res = client.Send(req);
    if (!resp_res.has_value()) {
        return std::unexpected(resp_res.error());
    }

    const Response& resp = resp_res.value();
    if (resp.status != ErrorCode::Ok) {
        std::string msg(resp.payload.begin(), resp.payload.end());
        return std::unexpected(Error(resp.status, std::move(msg)));
    }

    return DecodeWhiskCallResponse(resp.payload);
}

int64_t QuerySingleInt(Engine& engine, std::string_view sql) {
    auto res = engine.ExecuteSql(sql);
    assert(res.has_value());
    assert(!res->rows.empty());
    assert(!res->rows[0].empty());
    return std::get<int64_t>(res->rows[0][0]);
}

void TestLedgerFunctionalAndRollbacks(const std::vector<uint8_t>& ledger_wasm) {
    std::cout << "--- Testing Ledger Functional & Rollback Scenarios ---\n";
    auto dir = MakeTempDir("ledger_func");

    {
        EngineOptions opts;
        opts.data_dir = dir;
        opts.durability = DurabilityMode::MemoryOnly;

        auto eng_res = Engine::Open(opts);
        assert(eng_res.has_value());
        auto& engine = *eng_res.value();

        auto pub = engine.PublishModule("ledger", ledger_wasm);
        assert(pub.has_value());

        auto c1 = engine.CallWhisk("ledger", "create_account", R"({"id":1,"owner":"Alice","balance":1000})");
        assert(c1.has_value());
        assert(c1->result_json == R"({"status":"created","id":1,"owner":"Alice","balance":1000})");

        auto c2 = engine.CallWhisk("ledger", "create_account", R"({"id":2,"owner":"Bob","balance":500})");
        assert(c2.has_value());
        assert(c2->result_json == R"({"status":"created","id":2,"owner":"Bob","balance":500})");

        // Valid transfer: 250 from Alice (1) to Bob (2)
        auto t1 = engine.CallWhisk("ledger", "transfer_funds", R"({"from":1,"to":2,"amount":250})");
        assert(t1.has_value());
        assert(t1->result_json == R"({"status":"committed","from":1,"to":2,"amount":250})");

        auto b1 = engine.CallWhisk("ledger", "get_balance", R"({"id":1})");
        assert(b1.has_value());
        assert(b1->result_json == R"({"id":1,"balance":750})");

        auto b2 = engine.CallWhisk("ledger", "get_balance", R"({"id":2})");
        assert(b2.has_value());
        assert(b2->result_json == R"({"id":2,"balance":750})");

        assert(QuerySingleInt(engine, "SELECT COUNT(*) FROM transfers") == 1);

        // Insufficient funds (9999) -> WasmError, balances unchanged, still 1 transfer
        auto t_insuff = engine.CallWhisk("ledger", "transfer_funds", R"({"from":1,"to":2,"amount":9999})");
        assert(!t_insuff.has_value());
        assert(t_insuff.error().Code() == ErrorCode::WasmError);
        assert(QuerySingleInt(engine, "SELECT balance FROM accounts WHERE id = 1") == 750);
        assert(QuerySingleInt(engine, "SELECT balance FROM accounts WHERE id = 2") == 750);
        assert(QuerySingleInt(engine, "SELECT COUNT(*) FROM transfers") == 1);

        // Missing recipient (to: 999) -> WasmError, sender debit rolled back
        auto t_missing = engine.CallWhisk("ledger", "transfer_funds", R"({"from":1,"to":999,"amount":100})");
        assert(!t_missing.has_value());
        assert(t_missing.error().Code() == ErrorCode::WasmError);
        assert(QuerySingleInt(engine, "SELECT balance FROM accounts WHERE id = 1") == 750);
        assert(QuerySingleInt(engine, "SELECT COUNT(*) FROM transfers") == 1);

        // Fault injection: buggy_transfer_trap -> WasmTrap, sender debit rolled back
        auto t_trap = engine.CallWhisk("ledger", "buggy_transfer_trap", R"({"from":1,"amount":100})");
        assert(!t_trap.has_value());
        assert(t_trap.error().Code() == ErrorCode::WasmTrap);
        assert(QuerySingleInt(engine, "SELECT balance FROM accounts WHERE id = 1") == 750);
    }

    // Fault injection: infinite_loop on engine with wasm_max_fuel = 1'000'000 -> WasmFuelExhausted
    {
        auto fuel_dir = MakeTempDir("ledger_fuel");
        EngineOptions opts;
        opts.data_dir = fuel_dir;
        opts.durability = DurabilityMode::MemoryOnly;
        opts.wasm_max_fuel = 1'000'000;

        auto eng_res = Engine::Open(opts);
        assert(eng_res.has_value());
        auto& engine = *eng_res.value();

        assert(engine.PublishModule("ledger", ledger_wasm).has_value());
        assert(engine.CallWhisk("ledger", "create_account", R"({"id":1,"owner":"Alice","balance":1000})").has_value());

        auto t_loop = engine.CallWhisk("ledger", "infinite_loop", R"({"from":1,"amount":100})");
        assert(!t_loop.has_value());
        assert(t_loop.error().Code() == ErrorCode::WasmFuelExhausted);
        assert(QuerySingleInt(engine, "SELECT balance FROM accounts WHERE id = 1") == 1000);

        std::error_code ec;
        std::filesystem::remove_all(fuel_dir, ec);
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void TestInventoryFunctionalAndRollbacks(const std::vector<uint8_t>& inventory_wasm) {
    std::cout << "--- Testing Inventory Functional & Rollback Scenarios ---\n";
    auto dir = MakeTempDir("inventory_func");

    EngineOptions opts;
    opts.data_dir = dir;
    opts.durability = DurabilityMode::MemoryOnly;

    auto eng_res = Engine::Open(opts);
    assert(eng_res.has_value());
    auto& engine = *eng_res.value();

    assert(engine.PublishModule("inventory", inventory_wasm).has_value());

    auto prod = engine.CallWhisk(
        "inventory",
        "create_product",
        R"({"id":1,"name":"Widget","stock":10,"price_cents":500})");
    assert(prod.has_value());
    assert(prod->result_json == R"({"status":"created","id":1,"name":"Widget","stock":10,"price_cents":500})");

    // Valid checkout qty=3 -> stock=7, total_cents=1500
    auto co1 = engine.CallWhisk("inventory", "checkout_item", R"({"user_id":42,"product_id":1,"qty":3})");
    assert(co1.has_value());
    assert(co1->result_json == R"({"status":"ordered","user_id":42,"product_id":1,"qty":3,"total_cents":1500})");
    assert(QuerySingleInt(engine, "SELECT stock FROM products WHERE id = 1") == 7);
    assert(QuerySingleInt(engine, "SELECT COUNT(*) FROM orders") == 1);
    assert(QuerySingleInt(engine, "SELECT total_cents FROM orders WHERE id = 1") == 1500);

    // Insufficient stock qty=20 -> WasmError, stock still 7, still 1 order
    auto co2 = engine.CallWhisk("inventory", "checkout_item", R"({"user_id":43,"product_id":1,"qty":20})");
    assert(!co2.has_value());
    assert(co2.error().Code() == ErrorCode::WasmError);
    assert(co2.error().Message().find("insufficient stock") != std::string::npos);
    assert(QuerySingleInt(engine, "SELECT stock FROM products WHERE id = 1") == 7);
    assert(QuerySingleInt(engine, "SELECT COUNT(*) FROM orders") == 1);

    // Restock qty=3 -> stock=10
    auto rs = engine.CallWhisk("inventory", "restock", R"({"product_id":1,"qty":3})");
    assert(rs.has_value());
    assert(rs->result_json == R"({"status":"restocked","product_id":1,"stock":10})");
    assert(QuerySingleInt(engine, "SELECT stock FROM products WHERE id = 1") == 10);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void TestTcpConcurrencyMoneyConservation(const std::vector<uint8_t>& ledger_wasm) {
    std::cout << "--- Testing 8-Client TCP Concurrency: Money Conservation ---\n";
    auto dir = MakeTempDir("concurrent_ledger");

    EngineOptions opts;
    opts.data_dir = dir;
    opts.durability = DurabilityMode::MemoryOnly;
    opts.host = "127.0.0.1";
    opts.port = 0;

    auto eng_res = Engine::Open(opts);
    assert(eng_res.has_value());
    auto& engine = *eng_res.value();

    assert(engine.PublishModule("ledger", ledger_wasm).has_value());

    constexpr int64_t kNumAccounts = 4;
    constexpr int64_t kInitialBalance = 10'000;
    for (int64_t id = 1; id <= kNumAccounts; ++id) {
        std::string args = std::format(R"({{"id":{},"owner":"User{}","balance":{}}})", id, id, kInitialBalance);
        assert(engine.CallWhisk("ledger", "create_account", args).has_value());
    }

    assert(engine.StartServer().has_value());
    const uint16_t port = engine.Port();
    assert(port > 0);

    constexpr idx_t kThreads = 8;
    constexpr idx_t kTransfersPerThread = 20;
    std::atomic<int64_t> succeeded_count{0};
    std::atomic<int64_t> unexpected_errors{0};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (idx_t t = 0; t < kThreads; ++t) {
        workers.emplace_back([port, t, &succeeded_count, &unexpected_errors]() {
            TcpClient client;
            auto conn = client.Connect("127.0.0.1", port);
            if (!conn.has_value()) {
                unexpected_errors.fetch_add(1);
                return;
            }

            std::mt19937_64 rng(1000 + t);
            std::uniform_int_distribution<int64_t> acct_dist(1, kNumAccounts);
            std::uniform_int_distribution<int64_t> amt_dist(1, 500);

            for (idx_t i = 0; i < kTransfersPerThread; ++i) {
                int64_t from_id = acct_dist(rng);
                int64_t to_id = acct_dist(rng);
                if (to_id == from_id) {
                    to_id = (from_id % kNumAccounts) + 1;
                }
                int64_t amount = amt_dist(rng);

                std::string args = std::format(R"({{"from":{},"to":{},"amount":{}}})", from_id, to_id, amount);
                auto res = CallWhiskOverTcp(client, "ledger", "transfer_funds", args);
                if (res.has_value()) {
                    succeeded_count.fetch_add(1);
                } else if (res.error().Code() != ErrorCode::WasmError) {
                    unexpected_errors.fetch_add(1);
                }
            }
            client.Close();
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    assert(unexpected_errors.load() == 0);
    assert(succeeded_count.load() > 0);

    int64_t total_balance = QuerySingleInt(engine, "SELECT SUM(balance) FROM accounts");
    int64_t min_balance = QuerySingleInt(engine, "SELECT MIN(balance) FROM accounts");
    int64_t transfer_rows = QuerySingleInt(engine, "SELECT COUNT(*) FROM transfers");

    assert(total_balance == kNumAccounts * kInitialBalance);
    assert(min_balance >= 0);
    assert(transfer_rows == succeeded_count.load());

    engine.StopServer();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void TestTcpConcurrencyZeroOverselling(const std::vector<uint8_t>& inventory_wasm) {
    std::cout << "--- Testing 8-Client TCP Concurrency: Zero Overselling ---\n";
    auto dir = MakeTempDir("concurrent_inventory");

    EngineOptions opts;
    opts.data_dir = dir;
    opts.durability = DurabilityMode::MemoryOnly;
    opts.host = "127.0.0.1";
    opts.port = 0;

    auto eng_res = Engine::Open(opts);
    assert(eng_res.has_value());
    auto& engine = *eng_res.value();

    assert(engine.PublishModule("inventory", inventory_wasm).has_value());
    assert(engine.CallWhisk(
        "inventory",
        "create_product",
        R"({"id":100,"name":"LimitedGPU","stock":10,"price_cents":99900})").has_value());

    assert(engine.StartServer().has_value());
    const uint16_t port = engine.Port();
    assert(port > 0);

    constexpr idx_t kThreads = 8;
    constexpr idx_t kAttemptsPerThread = 5;
    std::atomic<int64_t> succeeded_count{0};
    std::atomic<int64_t> rejected_count{0};
    std::atomic<int64_t> unexpected_errors{0};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (idx_t t = 0; t < kThreads; ++t) {
        workers.emplace_back([port, t, &succeeded_count, &rejected_count, &unexpected_errors]() {
            TcpClient client;
            auto conn = client.Connect("127.0.0.1", port);
            if (!conn.has_value()) {
                unexpected_errors.fetch_add(1);
                return;
            }

            for (idx_t i = 0; i < kAttemptsPerThread; ++i) {
                int64_t user_id = static_cast<int64_t>(t * 100 + i + 1);
                std::string args = std::format(R"({{"user_id":{},"product_id":100,"qty":1}})", user_id);
                auto res = CallWhiskOverTcp(client, "inventory", "checkout_item", args);
                if (res.has_value()) {
                    succeeded_count.fetch_add(1);
                } else if (res.error().Code() == ErrorCode::WasmError) {
                    rejected_count.fetch_add(1);
                } else {
                    unexpected_errors.fetch_add(1);
                }
            }
            client.Close();
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    assert(unexpected_errors.load() == 0);
    assert(succeeded_count.load() == 10);
    assert(rejected_count.load() == 30);

    int64_t remaining_stock = QuerySingleInt(engine, "SELECT stock FROM products WHERE id = 100");
    int64_t order_rows = QuerySingleInt(engine, "SELECT COUNT(*) FROM orders WHERE product_id = 100");

    assert(remaining_stock == 0);
    assert(order_rows == 10);

    engine.StopServer();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

} // namespace

int main() {
    auto compile_dir = MakeTempDir("wasm_compile");

    auto ledger_res = test_util::CompileWhiskCppToWasm(
        FindExampleCpp("ledger_whisk.cpp"),
        compile_dir / "ledger_whisk.wasm");
    if (!ledger_res.has_value()) {
        std::cerr << "Failed to compile ledger_whisk.cpp: " << ledger_res.error().ToString() << "\n";
        return 1;
    }

    auto inventory_res = test_util::CompileWhiskCppToWasm(
        FindExampleCpp("inventory_whisk.cpp"),
        compile_dir / "inventory_whisk.wasm");
    if (!inventory_res.has_value()) {
        std::cerr << "Failed to compile inventory_whisk.cpp: " << inventory_res.error().ToString() << "\n";
        return 1;
    }

    TestLedgerFunctionalAndRollbacks(ledger_res.value());
    TestInventoryFunctionalAndRollbacks(inventory_res.value());
    TestTcpConcurrencyMoneyConservation(ledger_res.value());
    TestTcpConcurrencyZeroOverselling(inventory_res.value());

    std::error_code ec;
    std::filesystem::remove_all(compile_dir, ec);

    std::cout << "All showcase module & concurrency invariant tests PASSED!\n";
    return 0;
}
