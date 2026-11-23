#include "client/whisk_client.hpp"
#include "common/types.hpp"
#include "engine.hpp"
#include "errors.hpp"
#include "logger.hpp"
#include "storage/durability.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <numeric>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace database;

namespace {

struct BenchmarkMetric {
    std::string mode;
    std::string workload;
    idx_t clients = 1;
    idx_t total_ops = 0;
    double elapsed_sec = 0.0;
    double ops_per_sec = 0.0;
    double avg_lat_us = 0.0;
    double p50_lat_us = 0.0;
    double p95_lat_us = 0.0;
    double p99_lat_us = 0.0;
    double min_lat_us = 0.0;
    double max_lat_us = 0.0;
    std::string extra_info;
};

struct BenchmarkConfig {
    std::string mode_filter = "all";
    idx_t custom_ops = 0;
    idx_t threads = 4;
    idx_t clients = 1;
    idx_t simulated_rtt_us = 0;
    bool quick = false;
    bool run_durability = false;
    bool run_colocation = false;
    bool run_hotpath = false;
    bool run_all = false;
    bool markdown = false;
    std::string json_output_path;
    std::string csv_output_path;
};

std::filesystem::path CreateBenchDir(std::string_view tag) {
    auto dir = std::filesystem::temp_directory_path() /
               std::format("whisk_bench_{}_{}_{}", static_cast<int>(::getpid()), tag,
                           std::chrono::steady_clock::now().time_since_epoch().count());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void CleanupBenchDir(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

BenchmarkMetric ComputeStats(std::string_view mode, std::string_view workload,
                             std::vector<double>& latencies_us, double elapsed_sec,
                             std::string_view extra = "", idx_t clients = 1) {
    BenchmarkMetric m;
    m.mode = std::string(mode);
    m.workload = std::string(workload);
    m.clients = clients;
    m.total_ops = latencies_us.size();
    m.elapsed_sec = elapsed_sec;
    m.extra_info = std::string(extra);

    if (latencies_us.empty() || elapsed_sec <= 0.0) {
        return m;
    }

    m.ops_per_sec = static_cast<double>(m.total_ops) / elapsed_sec;
    std::sort(latencies_us.begin(), latencies_us.end());

    m.min_lat_us = latencies_us.front();
    m.max_lat_us = latencies_us.back();

    double sum = std::accumulate(latencies_us.begin(), latencies_us.end(), 0.0);
    m.avg_lat_us = sum / static_cast<double>(latencies_us.size());

    const auto p50_idx = static_cast<idx_t>(0.50 * static_cast<double>(latencies_us.size() - 1));
    const auto p95_idx = static_cast<idx_t>(0.95 * static_cast<double>(latencies_us.size() - 1));
    const auto p99_idx = static_cast<idx_t>(0.99 * static_cast<double>(latencies_us.size() - 1));

    m.p50_lat_us = latencies_us[p50_idx];
    m.p95_lat_us = latencies_us[p95_idx];
    m.p99_lat_us = latencies_us[p99_idx];

    return m;
}

std::string FindWasmClangxx() {
#ifdef WHISKDB_WASM_CLANGXX
    std::string cmake_clangxx = WHISKDB_WASM_CLANGXX;
    if (!cmake_clangxx.empty() && std::filesystem::exists(cmake_clangxx)) {
        return cmake_clangxx;
    }
#endif
    const char* home = std::getenv("HOME");
    if (home != nullptr) {
        std::filesystem::path whisk_tools_clang = std::filesystem::path(home) / ".whisk-tools" / "bin" / "clang++";
        if (std::filesystem::exists(whisk_tools_clang)) {
            return whisk_tools_clang.string();
        }
    }
    return "clang++";
}

std::filesystem::path FindRepoPath(std::string_view rel_path) {
#ifdef WHISKDB_SOURCE_DIR
    std::filesystem::path from_source = std::filesystem::path(WHISKDB_SOURCE_DIR) / rel_path;
    if (std::filesystem::exists(from_source)) {
        return from_source;
    }
#endif
    if (std::filesystem::exists(rel_path)) {
        return std::filesystem::absolute(rel_path);
    }
    std::filesystem::path parent_rel = std::filesystem::path("..") / rel_path;
    if (std::filesystem::exists(parent_rel)) {
        return std::filesystem::absolute(parent_rel);
    }
    std::filesystem::path grand_rel = std::filesystem::path("../..") / rel_path;
    if (std::filesystem::exists(grand_rel)) {
        return std::filesystem::absolute(grand_rel);
    }
    return std::filesystem::path(rel_path);
}

Result<std::vector<uint8_t>> CompileLedgerWhisk(const std::filesystem::path& temp_dir) {
    const std::filesystem::path cpp_path = FindRepoPath("examples/ledger_whisk.cpp");
    const std::filesystem::path sdk_inc = FindRepoPath("include/sdk");
    const std::filesystem::path out_wasm_path = temp_dir / "ledger_whisk.wasm";

    if (!std::filesystem::exists(cpp_path)) {
        return MAKE_ERROR(ErrorCode::NotFound, "ledger_whisk.cpp not found at {}", cpp_path.string());
    }
    if (!std::filesystem::exists(sdk_inc)) {
        return MAKE_ERROR(ErrorCode::NotFound, "SDK include directory not found at {}", sdk_inc.string());
    }

    const std::string clangxx = FindWasmClangxx();
    const std::string cmd = std::format(
        "\"{}\" --target=wasm32 -O3 -nostdlib -fno-exceptions -fno-rtti "
        "-Wl,--no-entry -Wl,--export-dynamic -Wall -Wextra -Wpedantic "
        "-I\"{}\" \"{}\" -o \"{}\"",
        clangxx, sdk_inc.string(), cpp_path.string(), out_wasm_path.string());

    const int ret = std::system(cmd.c_str());
    if (ret != 0) {
        return MAKE_ERROR(ErrorCode::Internal, "clang++ wasm compilation failed (exit {}): {}", ret, cmd);
    }

    std::ifstream file(out_wasm_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return MAKE_ERROR(ErrorCode::Internal, "Failed to open compiled WASM: {}", out_wasm_path.string());
    }
    const auto sz = file.tellg();
    if (sz < 0) {
        return MAKE_ERROR(ErrorCode::Internal, "Failed to read compiled WASM size");
    }
    std::vector<uint8_t> buffer(static_cast<size_t>(sz));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), sz)) {
        return MAKE_ERROR(ErrorCode::Internal, "Failed to read compiled WASM bytes");
    }
    return buffer;
}

void SimulateRtt(idx_t rtt_us) {
    if (rtt_us > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(rtt_us));
    }
}

idx_t ResolveOps(DurabilityMode mode, const BenchmarkConfig& cfg) {
    if (cfg.custom_ops > 0) {
        return cfg.custom_ops;
    }
    if (cfg.quick) {
        switch (mode) {
        case DurabilityMode::Strict:
            return 200;
        case DurabilityMode::GroupCommit:
            return 500;
        case DurabilityMode::Relaxed:
            return 1000;
        case DurabilityMode::MemoryOnly:
            return 2000;
        }
    }
    switch (mode) {
    case DurabilityMode::Strict:
        return 500;
    case DurabilityMode::GroupCommit:
        return 2000;
    case DurabilityMode::Relaxed:
        return 5000;
    case DurabilityMode::MemoryOnly:
        return 10000;
    }
    return 1000;
}

idx_t ResolveIterations(const BenchmarkConfig& cfg) {
    if (cfg.custom_ops > 0) {
        return cfg.custom_ops;
    }
    return cfg.quick ? 200 : 1000;
}

DurabilityMode ResolveSingleBenchMode(const BenchmarkConfig& cfg) {
    if (cfg.mode_filter != "all") {
        auto parsed = ParseDurabilityMode(cfg.mode_filter);
        if (parsed.has_value()) {
            return *parsed;
        }
    }
    return DurabilityMode::MemoryOnly;
}

std::vector<BenchmarkMetric> RunModeBenchmarks(DurabilityMode mode, const BenchmarkConfig& cfg) {
    std::vector<BenchmarkMetric> results;
    const std::string_view mode_str = ToString(mode);
    const idx_t ops = ResolveOps(mode, cfg);
    const auto temp_dir = CreateBenchDir(mode_str);

    EngineOptions opts;
    opts.data_dir = temp_dir;
    opts.durability = mode;
    opts.checkpoint_wal_records = 0; // Manual checkpointing during benchmark
    opts.host = "127.0.0.1";
    opts.port = 0;

    auto eng_res = Engine::Open(opts);
    if (!eng_res.has_value()) {
        std::println(stderr, "Failed to open engine in mode {}: {}", mode_str, eng_res.error().Message());
        CleanupBenchDir(temp_dir);
        return results;
    }
    auto& engine = *eng_res.value();

    // Setup benchmark schema
    auto schema_res = engine.ExecuteSql(
        "CREATE TABLE bench (id INTEGER PRIMARY KEY, k INTEGER NOT NULL, v TEXT NOT NULL, score REAL NOT NULL)");
    if (!schema_res.has_value()) {
        std::println(stderr, "Failed to create bench table: {}", schema_res.error().Message());
        CleanupBenchDir(temp_dir);
        return results;
    }

    // 1. Sequential Single-Row Inserts
    {
        std::vector<double> latencies;
        latencies.reserve(ops);

        auto start = std::chrono::steady_clock::now();
        for (idx_t i = 1; i <= ops; i++) {
            std::vector<Value> params{static_cast<int64_t>(i), static_cast<int64_t>(i * 17),
                                      std::format("bench_val_{}", i), static_cast<double>(i % 100) * 1.5};
            auto op_start = std::chrono::steady_clock::now();
            auto res = engine.ExecuteSql("INSERT INTO bench VALUES (?1, ?2, ?3, ?4)", params);
            auto op_end = std::chrono::steady_clock::now();

            if (res.has_value()) {
                double us = std::chrono::duration<double, std::micro>(op_end - op_start).count();
                latencies.push_back(us);
            }
        }
        auto end = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(end - start).count();
        results.push_back(ComputeStats(mode_str, "insert_seq", latencies, elapsed));
    }

    // 2. Concurrent Multi-Threaded Inserts
    {
        auto setup_res = engine.ExecuteSql(
            "CREATE TABLE bench_concur (id INTEGER PRIMARY KEY AUTOINCREMENT, tid INTEGER NOT NULL, val INTEGER NOT NULL)");
        if (setup_res.has_value()) {
            const idx_t threads_cnt = std::max<idx_t>(1, cfg.threads);
            const idx_t ops_per_th = std::max<idx_t>(10, ops / threads_cnt);
            std::vector<std::thread> workers;
            workers.reserve(threads_cnt);

            std::vector<std::vector<double>> thread_latencies(threads_cnt);
            for (idx_t t = 0; t < threads_cnt; t++) {
                thread_latencies[t].reserve(ops_per_th);
            }

            auto start = std::chrono::steady_clock::now();
            for (idx_t t = 0; t < threads_cnt; t++) {
                workers.emplace_back([&engine, t, ops_per_th, &thread_latencies] {
                    for (idx_t i = 0; i < ops_per_th; i++) {
                        std::vector<Value> params{static_cast<int64_t>(t), static_cast<int64_t>(i)};
                        auto op_start = std::chrono::steady_clock::now();
                        auto res = engine.ExecuteSql("INSERT INTO bench_concur (tid, val) VALUES (?1, ?2)", params);
                        auto op_end = std::chrono::steady_clock::now();
                        if (res.has_value()) {
                            double us = std::chrono::duration<double, std::micro>(op_end - op_start).count();
                            thread_latencies[t].push_back(us);
                        }
                    }
                });
            }

            for (auto& w : workers) {
                w.join();
            }
            auto end = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(end - start).count();

            std::vector<double> combined_latencies;
            for (const auto& th_lat : thread_latencies) {
                combined_latencies.insert(combined_latencies.end(), th_lat.begin(), th_lat.end());
            }

            std::string extra = std::format("{} threads", threads_cnt);
            results.push_back(ComputeStats(mode_str, "insert_concur", combined_latencies, elapsed, extra, threads_cnt));
        }
    }

    // 3. Point Lookup Reads
    {
        std::vector<double> latencies;
        latencies.reserve(ops);

        auto start = std::chrono::steady_clock::now();
        for (idx_t i = 1; i <= ops; i++) {
            int64_t target_id = static_cast<int64_t>((i * 7919) % ops + 1);
            std::vector<Value> params{target_id};

            auto op_start = std::chrono::steady_clock::now();
            auto res = engine.ExecuteSql("SELECT k, v, score FROM bench WHERE id = ?1", params);
            auto op_end = std::chrono::steady_clock::now();

            if (res.has_value()) {
                double us = std::chrono::duration<double, std::micro>(op_end - op_start).count();
                latencies.push_back(us);
            }
        }
        auto end = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(end - start).count();
        results.push_back(ComputeStats(mode_str, "point_read", latencies, elapsed));
    }

    // 4. Point Updates
    {
        std::vector<double> latencies;
        latencies.reserve(ops);

        auto start = std::chrono::steady_clock::now();
        for (idx_t i = 1; i <= ops; i++) {
            int64_t target_id = static_cast<int64_t>((i * 4999) % ops + 1);
            std::vector<Value> params{static_cast<double>(i * 3.14), target_id};

            auto op_start = std::chrono::steady_clock::now();
            auto res = engine.ExecuteSql("UPDATE bench SET score = ?1 WHERE id = ?2", params);
            auto op_end = std::chrono::steady_clock::now();

            if (res.has_value()) {
                double us = std::chrono::duration<double, std::micro>(op_end - op_start).count();
                latencies.push_back(us);
            }
        }
        auto end = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(end - start).count();
        results.push_back(ComputeStats(mode_str, "update_point", latencies, elapsed));
    }

    // 5. Range Scan Analytical Reads
    {
        const idx_t scan_ops = std::max<idx_t>(50, ops / 4);
        std::vector<double> latencies;
        latencies.reserve(scan_ops);

        auto start = std::chrono::steady_clock::now();
        for (idx_t i = 1; i <= scan_ops; i++) {
            int64_t start_id = static_cast<int64_t>((i * 101) % (ops > 100 ? ops - 100 : 1) + 1);
            int64_t end_id = start_id + 50;
            std::vector<Value> params{start_id, end_id};

            auto op_start = std::chrono::steady_clock::now();
            auto res = engine.ExecuteSql(
                "SELECT COUNT(*), AVG(score), SUM(k) FROM bench WHERE id BETWEEN ?1 AND ?2", params);
            auto op_end = std::chrono::steady_clock::now();

            if (res.has_value()) {
                double us = std::chrono::duration<double, std::micro>(op_end - op_start).count();
                latencies.push_back(us);
            }
        }
        auto end = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(end - start).count();
        results.push_back(ComputeStats(mode_str, "range_scan", latencies, elapsed, "window=50"));
    }

    // 6. Checkpoint / Snapshot Performance (if durable)
    if (mode != DurabilityMode::MemoryOnly) {
        auto cp_start = std::chrono::steady_clock::now();
        auto cp_res = engine.Checkpoint();
        auto cp_end = std::chrono::steady_clock::now();

        if (cp_res.has_value()) {
            double cp_us = std::chrono::duration<double, std::micro>(cp_end - cp_start).count();
            std::vector<double> cp_lats{cp_us};
            double cp_sec = cp_us / 1000000.0;
            std::string extra = std::format("lsn={}", *cp_res);
            results.push_back(ComputeStats(mode_str, "checkpoint", cp_lats, cp_sec, extra));
        }
    }

    // 7. Recovery Replay Performance (if durable)
    if (mode != DurabilityMode::MemoryOnly) {
        // Log extra mutations before reopen to test replay
        for (idx_t i = 1; i <= 100; i++) {
            std::vector<Value> p{static_cast<int64_t>(ops + i), static_cast<int64_t>(i),
                                 std::format("recover_val_{}", i), 0.0};
            (void)engine.ExecuteSql("INSERT INTO bench VALUES (?1, ?2, ?3, ?4)", p);
        }

        // Close engine
        eng_res.value().reset();

        // Reopen and measure recovery duration
        auto rec_start = std::chrono::steady_clock::now();
        auto reopen_res = Engine::Open(opts);
        auto rec_end = std::chrono::steady_clock::now();

        if (reopen_res.has_value()) {
            double rec_us = std::chrono::duration<double, std::micro>(rec_end - rec_start).count();
            std::vector<double> rec_lats{rec_us};
            double rec_sec = rec_us / 1000000.0;
            const auto& rec_info = (*reopen_res)->Recovery();
            std::string extra = std::format("replayed={} snap_lsn={}", rec_info.wal_records_replayed,
                                            rec_info.snapshot_lsn);
            results.push_back(ComputeStats(mode_str, "recovery", rec_lats, rec_sec, extra));
            reopen_res.value().reset();
        }
    } else {
        eng_res.value().reset();
    }

    CleanupBenchDir(temp_dir);
    return results;
}

std::vector<BenchmarkMetric> RunColocationBenchmark(const BenchmarkConfig& cfg) {
    std::vector<BenchmarkMetric> results;
    const DurabilityMode mode = ResolveSingleBenchMode(cfg);
    const std::string_view mode_str = ToString(mode);
    const idx_t clients_cnt = std::max<idx_t>(1, cfg.clients);
    const idx_t total_iters = ResolveIterations(cfg);
    const idx_t iters_per_client = std::max<idx_t>(1, total_iters / clients_cnt);
    const auto temp_dir = CreateBenchDir("colocation");

    auto wasm_res = CompileLedgerWhisk(temp_dir);
    if (!wasm_res.has_value()) {
        std::println(stderr, "Co-location benchmark failed to compile ledger_whisk.cpp: {}", wasm_res.error().Message());
        CleanupBenchDir(temp_dir);
        return results;
    }

    EngineOptions opts;
    opts.data_dir = temp_dir;
    opts.durability = mode;
    opts.checkpoint_wal_records = 0;
    opts.host = "127.0.0.1";
    opts.port = 0;

    auto eng_res = Engine::Open(opts);
    if (!eng_res.has_value()) {
        std::println(stderr, "Failed to open engine for co-location benchmark: {}", eng_res.error().Message());
        CleanupBenchDir(temp_dir);
        return results;
    }
    auto& engine = *eng_res.value();

    auto pub_res = engine.PublishModule("ledger", *wasm_res);
    if (!pub_res.has_value()) {
        std::println(stderr, "Failed to publish ledger module: {}", pub_res.error().Message());
        CleanupBenchDir(temp_dir);
        return results;
    }

    for (idx_t c = 0; c < clients_cnt; c++) {
        const int64_t acct_a = static_cast<int64_t>(c * 2 + 1);
        const int64_t acct_b = static_cast<int64_t>(c * 2 + 2);
        auto r1 = engine.CallWhisk("ledger", "create_account",
                                   std::format(R"({{"id":{},"owner":"alice_{}","balance":1000000000}})", acct_a, c));
        auto r2 = engine.CallWhisk("ledger", "create_account",
                                   std::format(R"({{"id":{},"owner":"bob_{}","balance":1000000000}})", acct_b, c));
        if (!r1.has_value() || !r2.has_value()) {
            std::println(stderr, "Failed to seed accounts for client {}", c);
            CleanupBenchDir(temp_dir);
            return results;
        }
    }

    auto srv_res = engine.StartServer();
    if (!srv_res.has_value()) {
        std::println(stderr, "Failed to start TCP server: {}", srv_res.error().Message());
        CleanupBenchDir(temp_dir);
        return results;
    }
    const uint16_t port = engine.Port();

    // Baseline: 4 sequential TCP SQL_QUERY round-trips per transfer
    BenchmarkMetric baseline_metric;
    {
        std::vector<std::unique_ptr<WhiskClient>> clients;
        clients.reserve(clients_cnt);
        for (idx_t c = 0; c < clients_cnt; c++) {
            auto client = std::make_unique<WhiskClient>();
            auto conn_res = client->Connect("127.0.0.1", port);
            if (!conn_res.has_value()) {
                std::println(stderr, "Baseline client {} failed to connect: {}", c, conn_res.error().Message());
                engine.StopServer();
                CleanupBenchDir(temp_dir);
                return results;
            }
            clients.push_back(std::move(client));
        }

        std::vector<std::vector<double>> client_lats(clients_cnt);
        for (idx_t c = 0; c < clients_cnt; c++) {
            client_lats[c].reserve(iters_per_client);
        }

        std::vector<std::thread> workers;
        workers.reserve(clients_cnt);
        auto start = std::chrono::steady_clock::now();
        for (idx_t c = 0; c < clients_cnt; c++) {
            workers.emplace_back([&, c] {
                const int64_t acct_a = static_cast<int64_t>(c * 2 + 1);
                const int64_t acct_b = static_cast<int64_t>(c * 2 + 2);
                auto& cli = *clients[c];

                for (idx_t i = 0; i < iters_per_client; i++) {
                    const int64_t from_id = (i % 2 == 0) ? acct_a : acct_b;
                    const int64_t to_id = (i % 2 == 0) ? acct_b : acct_a;
                    constexpr int64_t amount = 10;

                    auto op_start = std::chrono::steady_clock::now();

                    // 1. SELECT balance
                    SimulateRtt(cfg.simulated_rtt_us);
                    std::vector<Value> sel_params{from_id};
                    auto q1 = cli.Query("SELECT balance FROM accounts WHERE id = ?1;", sel_params);

                    // 2. UPDATE debit
                    SimulateRtt(cfg.simulated_rtt_us);
                    std::vector<Value> debit_params{amount, from_id};
                    auto q2 = cli.Query("UPDATE accounts SET balance = balance - ?1 WHERE id = ?2;", debit_params);

                    // 3. UPDATE credit
                    SimulateRtt(cfg.simulated_rtt_us);
                    std::vector<Value> credit_params{amount, to_id};
                    auto q3 = cli.Query("UPDATE accounts SET balance = balance + ?1 WHERE id = ?2;", credit_params);

                    // 4. INSERT transfer audit
                    SimulateRtt(cfg.simulated_rtt_us);
                    std::vector<Value> log_params{from_id, to_id, amount};
                    auto q4 = cli.Query("INSERT INTO transfers (from_id, to_id, amount) VALUES (?1, ?2, ?3);",
                                        log_params);

                    auto op_end = std::chrono::steady_clock::now();
                    if (q1.has_value() && q2.has_value() && q3.has_value() && q4.has_value()) {
                        double us = std::chrono::duration<double, std::micro>(op_end - op_start).count();
                        client_lats[c].push_back(us);
                    }
                }
            });
        }
        for (auto& w : workers) {
            w.join();
        }
        auto end = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(end - start).count();

        std::vector<double> combined;
        for (const auto& l : client_lats) {
            combined.insert(combined.end(), l.begin(), l.end());
        }
        std::string extra = std::format("4x TCP SQL, rtt={}us", cfg.simulated_rtt_us);
        baseline_metric = ComputeStats(mode_str, "colocation_sql", combined, elapsed, extra, clients_cnt);
        results.push_back(baseline_metric);
    }

    // Co-Located: 1 single TCP WHISK_CALL invoking ledger::transfer_funds
    {
        std::vector<std::unique_ptr<WhiskClient>> clients;
        clients.reserve(clients_cnt);
        for (idx_t c = 0; c < clients_cnt; c++) {
            auto client = std::make_unique<WhiskClient>();
            auto conn_res = client->Connect("127.0.0.1", port);
            if (!conn_res.has_value()) {
                std::println(stderr, "Whisk client {} failed to connect: {}", c, conn_res.error().Message());
                engine.StopServer();
                CleanupBenchDir(temp_dir);
                return results;
            }
            clients.push_back(std::move(client));
        }

        std::vector<std::vector<double>> client_lats(clients_cnt);
        for (idx_t c = 0; c < clients_cnt; c++) {
            client_lats[c].reserve(iters_per_client);
        }

        std::vector<std::thread> workers;
        workers.reserve(clients_cnt);
        auto start = std::chrono::steady_clock::now();
        for (idx_t c = 0; c < clients_cnt; c++) {
            workers.emplace_back([&, c] {
                const int64_t acct_a = static_cast<int64_t>(c * 2 + 1);
                const int64_t acct_b = static_cast<int64_t>(c * 2 + 2);
                const std::string args_ab = std::format(R"({{"from":{},"to":{},"amount":10}})", acct_a, acct_b);
                const std::string args_ba = std::format(R"({{"from":{},"to":{},"amount":10}})", acct_b, acct_a);
                auto& cli = *clients[c];

                for (idx_t i = 0; i < iters_per_client; i++) {
                    const std::string& args = (i % 2 == 0) ? args_ab : args_ba;
                    auto op_start = std::chrono::steady_clock::now();
                    SimulateRtt(cfg.simulated_rtt_us);
                    auto res = cli.CallWhisk("ledger", "transfer_funds", args);
                    auto op_end = std::chrono::steady_clock::now();

                    if (res.has_value()) {
                        double us = std::chrono::duration<double, std::micro>(op_end - op_start).count();
                        client_lats[c].push_back(us);
                    }
                }
            });
        }
        for (auto& w : workers) {
            w.join();
        }
        auto end = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(end - start).count();

        std::vector<double> combined;
        for (const auto& l : client_lats) {
            combined.insert(combined.end(), l.begin(), l.end());
        }

        double speedup = 0.0;
        if (baseline_metric.ops_per_sec > 0.0 && elapsed > 0.0) {
            const double whisk_tps = static_cast<double>(combined.size()) / elapsed;
            speedup = whisk_tps / baseline_metric.ops_per_sec;
        }
        std::string extra = std::format("{:.2f}x faster (rtt={}us)", speedup, cfg.simulated_rtt_us);
        results.push_back(ComputeStats(mode_str, "colocation_whisk", combined, elapsed, extra, clients_cnt));
    }

    engine.StopServer();
    eng_res.value().reset();
    CleanupBenchDir(temp_dir);
    return results;
}

std::vector<BenchmarkMetric> RunHotPathBenchmark(const BenchmarkConfig& cfg) {
    std::vector<BenchmarkMetric> results;
    const DurabilityMode mode = ResolveSingleBenchMode(cfg);
    const std::string_view mode_str = ToString(mode);
    const idx_t clients_cnt = std::max<idx_t>(1, cfg.clients);
    const idx_t total_iters = ResolveIterations(cfg);
    const idx_t iters_per_client = std::max<idx_t>(1, total_iters / clients_cnt);
    const auto temp_dir = CreateBenchDir("hotpath");

    auto wasm_res = CompileLedgerWhisk(temp_dir);
    if (!wasm_res.has_value()) {
        std::println(stderr, "Hot-path benchmark failed to compile ledger_whisk.cpp: {}", wasm_res.error().Message());
        CleanupBenchDir(temp_dir);
        return results;
    }

    EngineOptions opts;
    opts.data_dir = temp_dir;
    opts.durability = mode;
    opts.checkpoint_wal_records = 0;
    opts.host = "127.0.0.1";
    opts.port = 0;

    auto eng_res = Engine::Open(opts);
    if (!eng_res.has_value()) {
        std::println(stderr, "Failed to open engine for hot-path benchmark: {}", eng_res.error().Message());
        CleanupBenchDir(temp_dir);
        return results;
    }
    auto& engine = *eng_res.value();

    auto pub_res = engine.PublishModule("ledger", *wasm_res);
    if (!pub_res.has_value()) {
        std::println(stderr, "Failed to publish ledger module: {}", pub_res.error().Message());
        CleanupBenchDir(temp_dir);
        return results;
    }

    for (idx_t c = 0; c < clients_cnt; c++) {
        const int64_t acct_a = static_cast<int64_t>(c * 2 + 1);
        const int64_t acct_b = static_cast<int64_t>(c * 2 + 2);
        (void)engine.CallWhisk("ledger", "create_account",
                               std::format(R"({{"id":{},"owner":"hot_a_{}","balance":1000000000}})", acct_a, c));
        (void)engine.CallWhisk("ledger", "create_account",
                               std::format(R"({{"id":{},"owner":"hot_b_{}","balance":1000000000}})", acct_b, c));
    }

    // Warm up JIT module and SQL prepared statement cache
    for (idx_t i = 0; i < 10; i++) {
        (void)engine.CallWhisk("ledger", "transfer_funds", R"({"from":1,"to":2,"amount":1})");
    }

    auto FormatCacheRatio = [](const SqlEngine::CacheStats& before, const SqlEngine::CacheStats& after) {
        const idx_t dhits = after.hits >= before.hits ? (after.hits - before.hits) : 0;
        const idx_t dmisses = after.misses >= before.misses ? (after.misses - before.misses) : 0;
        const idx_t total = dhits + dmisses;
        const double ratio = total > 0 ? (100.0 * static_cast<double>(dhits) / static_cast<double>(total)) : 0.0;
        return std::format("sql_cache_hit={:.2f}% (cached={})", ratio, after.size);
    };

    // 1. In-Process Hot-Path CallWhisk
    {
        const auto cache_before = engine.GetSqlCacheStats();
        std::vector<std::vector<double>> client_lats(clients_cnt);
        for (idx_t c = 0; c < clients_cnt; c++) {
            client_lats[c].reserve(iters_per_client);
        }

        std::vector<std::thread> workers;
        workers.reserve(clients_cnt);
        auto start = std::chrono::steady_clock::now();
        for (idx_t c = 0; c < clients_cnt; c++) {
            workers.emplace_back([&, c] {
                const int64_t acct_a = static_cast<int64_t>(c * 2 + 1);
                const int64_t acct_b = static_cast<int64_t>(c * 2 + 2);
                const std::string args_ab = std::format(R"({{"from":{},"to":{},"amount":10}})", acct_a, acct_b);
                const std::string args_ba = std::format(R"({{"from":{},"to":{},"amount":10}})", acct_b, acct_a);

                for (idx_t i = 0; i < iters_per_client; i++) {
                    const std::string& args = (i % 2 == 0) ? args_ab : args_ba;
                    auto op_start = std::chrono::steady_clock::now();
                    auto res = engine.CallWhisk("ledger", "transfer_funds", args);
                    auto op_end = std::chrono::steady_clock::now();
                    if (res.has_value()) {
                        double us = std::chrono::duration<double, std::micro>(op_end - op_start).count();
                        client_lats[c].push_back(us);
                    }
                }
            });
        }
        for (auto& w : workers) {
            w.join();
        }
        auto end = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(end - start).count();
        const auto cache_after = engine.GetSqlCacheStats();

        std::vector<double> combined;
        for (const auto& l : client_lats) {
            combined.insert(combined.end(), l.begin(), l.end());
        }
        results.push_back(ComputeStats(mode_str, "hotpath_inproc", combined, elapsed,
                                       FormatCacheRatio(cache_before, cache_after), clients_cnt));
    }

    // 2. Over-TCP Hot-Path CallWhisk
    auto srv_res = engine.StartServer();
    if (srv_res.has_value()) {
        const uint16_t port = engine.Port();
        std::vector<std::unique_ptr<WhiskClient>> clients;
        clients.reserve(clients_cnt);
        bool all_connected = true;
        for (idx_t c = 0; c < clients_cnt; c++) {
            auto client = std::make_unique<WhiskClient>();
            auto conn_res = client->Connect("127.0.0.1", port);
            if (!conn_res.has_value()) {
                std::println(stderr, "Hot-path TCP client {} failed to connect: {}", c, conn_res.error().Message());
                all_connected = false;
                break;
            }
            clients.push_back(std::move(client));
        }

        if (all_connected) {
            const auto cache_before = engine.GetSqlCacheStats();
            std::vector<std::vector<double>> client_lats(clients_cnt);
            for (idx_t c = 0; c < clients_cnt; c++) {
                client_lats[c].reserve(iters_per_client);
            }

            std::vector<std::thread> workers;
            workers.reserve(clients_cnt);
            auto start = std::chrono::steady_clock::now();
            for (idx_t c = 0; c < clients_cnt; c++) {
                workers.emplace_back([&, c] {
                    const int64_t acct_a = static_cast<int64_t>(c * 2 + 1);
                    const int64_t acct_b = static_cast<int64_t>(c * 2 + 2);
                    const std::string args_ab = std::format(R"({{"from":{},"to":{},"amount":10}})", acct_a, acct_b);
                    const std::string args_ba = std::format(R"({{"from":{},"to":{},"amount":10}})", acct_b, acct_a);
                    auto& cli = *clients[c];

                    for (idx_t i = 0; i < iters_per_client; i++) {
                        const std::string& args = (i % 2 == 0) ? args_ab : args_ba;
                        auto op_start = std::chrono::steady_clock::now();
                        SimulateRtt(cfg.simulated_rtt_us);
                        auto res = cli.CallWhisk("ledger", "transfer_funds", args);
                        auto op_end = std::chrono::steady_clock::now();
                        if (res.has_value()) {
                            double us = std::chrono::duration<double, std::micro>(op_end - op_start).count();
                            client_lats[c].push_back(us);
                        }
                    }
                });
            }
            for (auto& w : workers) {
                w.join();
            }
            auto end = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(end - start).count();
            const auto cache_after = engine.GetSqlCacheStats();

            std::vector<double> combined;
            for (const auto& l : client_lats) {
                combined.insert(combined.end(), l.begin(), l.end());
            }
            results.push_back(ComputeStats(mode_str, "hotpath_tcp", combined, elapsed,
                                           FormatCacheRatio(cache_before, cache_after), clients_cnt));
        }
        engine.StopServer();
    }

    eng_res.value().reset();
    CleanupBenchDir(temp_dir);
    return results;
}

void PrintTable(const std::vector<BenchmarkMetric>& metrics) {
    std::println("\n{:=^120}", " WHISKDB BENCHMARK RESULTS ");
    std::println("{:<13} | {:<18} | {:>8} | {:>12} | {:>10} | {:>10} | {:>10} | {:>10} | {:<24}",
                 "Mode", "Workload", "Ops", "Ops/sec", "Avg (us)", "p50 (us)", "p95 (us)", "p99 (us)", "Notes");
    std::println("{:-^120}", "");

    for (const auto& m : metrics) {
        std::println("{:<13} | {:<18} | {:>8} | {:>12.1f} | {:>10.1f} | {:>10.1f} | {:>10.1f} | {:>10.1f} | {:<24}",
                     m.mode, m.workload, m.total_ops, m.ops_per_sec, m.avg_lat_us, m.p50_lat_us,
                     m.p95_lat_us, m.p99_lat_us, m.extra_info);
    }
    std::println("{:=^120}\n", "");
}

void PrintMarkdown(const std::vector<BenchmarkMetric>& metrics) {
    std::println("\n| Mode | Benchmark | Clients | Iterations | Ops/sec | p50 (us) | p95 (us) | p99 (us) | Notes |");
    std::println("| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |");
    for (const auto& m : metrics) {
        std::println("| {} | {} | {} | {} | {:.1f} | {:.1f} | {:.1f} | {:.1f} | {} |",
                     m.mode, m.workload, m.clients, m.total_ops, m.ops_per_sec,
                     m.p50_lat_us, m.p95_lat_us, m.p99_lat_us, m.extra_info);
    }
    std::println("");
}

void WriteJson(const std::vector<BenchmarkMetric>& metrics, const std::string& path) {
    std::ofstream out(path);
    if (!out.is_open()) {
        std::println(stderr, "Failed to write JSON output to {}", path);
        return;
    }

    out << "{\n";
    out << "  \"version\": \"1.0.0\",\n";
    out << "  \"metrics\": [\n";
    for (idx_t i = 0; i < metrics.size(); i++) {
        const auto& m = metrics[i];
        out << "    {\n";
        out << std::format("      \"mode\": \"{}\",\n", m.mode);
        out << std::format("      \"workload\": \"{}\",\n", m.workload);
        out << std::format("      \"clients\": {},\n", m.clients);
        out << std::format("      \"total_ops\": {},\n", m.total_ops);
        out << std::format("      \"elapsed_sec\": {:.6f},\n", m.elapsed_sec);
        out << std::format("      \"ops_per_sec\": {:.2f},\n", m.ops_per_sec);
        out << std::format("      \"avg_lat_us\": {:.2f},\n", m.avg_lat_us);
        out << std::format("      \"p50_lat_us\": {:.2f},\n", m.p50_lat_us);
        out << std::format("      \"p95_lat_us\": {:.2f},\n", m.p95_lat_us);
        out << std::format("      \"p99_lat_us\": {:.2f},\n", m.p99_lat_us);
        out << std::format("      \"min_lat_us\": {:.2f},\n", m.min_lat_us);
        out << std::format("      \"max_lat_us\": {:.2f},\n", m.max_lat_us);
        out << std::format("      \"extra_info\": \"{}\"\n", m.extra_info);
        out << "    }" << (i + 1 < metrics.size() ? "," : "") << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    std::println("Saved benchmark JSON results to {}", path);
}

void WriteCsv(const std::vector<BenchmarkMetric>& metrics, const std::string& path) {
    std::ofstream out(path);
    if (!out.is_open()) {
        std::println(stderr, "Failed to write CSV output to {}", path);
        return;
    }

    out << "benchmark,mode,clients,iterations,ops_per_sec,p50_us,p95_us,p99_us\n";
    for (const auto& m : metrics) {
        out << std::format("{},{},{},{},{:.2f},{:.2f},{:.2f},{:.2f}\n",
                           m.workload, m.mode, m.clients, m.total_ops,
                           m.ops_per_sec, m.p50_lat_us, m.p95_lat_us, m.p99_lat_us);
    }
    std::println("Saved benchmark CSV results to {}", path);
}

void PrintHelp() {
    std::println("WhiskDB Co-Location & Persistence Benchmark Suite");
    std::println("Usage: whisk_bench [options]");
    std::println("");
    std::println("Options:");
    std::println("  --mode <name>           strict | group_commit | relaxed | memory_only | all (default: all)");
    std::println("  --ops <N>               Override total operations per workload");
    std::println("  --iterations <N>        Alias for --ops (total iterations for colocation/hotpath)");
    std::println("  --threads <N>           Concurrency worker threads for durability workloads (default: 4)");
    std::println("  --clients <N>           Concurrent TCP clients for colocation/hotpath (default: 1)");
    std::println("  --simulated-rtt-us <N>  Simulated client-side network RTT in microseconds (default: 0)");
    std::println("  --colocation            Run Benchmark 1: Co-Location Advantage (4x SQL vs 1x Whisk)");
    std::println("  --hotpath               Run Benchmark 3: Prepared Statement & Pre-Compiled JIT Hot Path");
    std::println("  --all                   Run all benchmark suites (durability + colocation + hotpath)");
    std::println("  --quick                 Run fast benchmark profile suitable for quick CI checks");
    std::println("  --markdown              Print results as a GitHub-flavored Markdown table");
    std::println("  --json <path>           Output benchmark metrics to a JSON file");
    std::println("  --csv <path>            Output benchmark metrics to a CSV file");
    std::println("  -h, --help              Show this help message");
}

} // namespace

int main(int argc, char** argv) {
    Logger::SetLevel(LogLevel::Warn);

    BenchmarkConfig cfg;
    for (int i = 1; i < argc; i++) {
        std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            PrintHelp();
            return 0;
        } else if (arg == "--quick") {
            cfg.quick = true;
        } else if (arg == "--colocation") {
            cfg.run_colocation = true;
        } else if (arg == "--hotpath") {
            cfg.run_hotpath = true;
        } else if (arg == "--all") {
            cfg.run_all = true;
        } else if (arg == "--markdown") {
            cfg.markdown = true;
        } else if (arg == "--mode" && i + 1 < argc) {
            cfg.mode_filter = argv[++i];
        } else if ((arg == "--ops" || arg == "--iterations") && i + 1 < argc) {
            cfg.custom_ops = static_cast<idx_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--threads" && i + 1 < argc) {
            cfg.threads = static_cast<idx_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--clients" && i + 1 < argc) {
            cfg.clients = static_cast<idx_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--simulated-rtt-us" && i + 1 < argc) {
            cfg.simulated_rtt_us = static_cast<idx_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--json" && i + 1 < argc) {
            cfg.json_output_path = argv[++i];
        } else if (arg == "--csv" && i + 1 < argc) {
            cfg.csv_output_path = argv[++i];
        }
    }

    if (cfg.run_all) {
        cfg.run_durability = true;
        cfg.run_colocation = true;
        cfg.run_hotpath = true;
    } else if (!cfg.run_colocation && !cfg.run_hotpath) {
        cfg.run_durability = true;
    }

    std::vector<BenchmarkMetric> all_metrics;

    if (cfg.run_durability) {
        std::vector<DurabilityMode> target_modes;
        if (cfg.mode_filter == "all") {
            target_modes = {DurabilityMode::Strict, DurabilityMode::GroupCommit,
                            DurabilityMode::Relaxed, DurabilityMode::MemoryOnly};
        } else {
            auto parsed = ParseDurabilityMode(cfg.mode_filter);
            if (!parsed.has_value()) {
                std::println(stderr, "Unknown durability mode '{}'. Use strict|group_commit|relaxed|memory_only|all",
                             cfg.mode_filter);
                return 1;
            }
            target_modes.push_back(*parsed);
        }

        std::println(">>> Starting WhiskDB Durability Benchmark across {} persistence mode(s)...", target_modes.size());
        std::println(">>> Config: quick={}, threads={}, mode={}", cfg.quick, cfg.threads, cfg.mode_filter);

        for (auto mode : target_modes) {
            std::println(">>> Benchmarking mode: {} ...", ToString(mode));
            auto mode_metrics = RunModeBenchmarks(mode, cfg);
            all_metrics.insert(all_metrics.end(), mode_metrics.begin(), mode_metrics.end());
        }
    }

    if (cfg.run_colocation) {
        std::println(">>> Starting WhiskDB Co-Location Advantage Benchmark (clients={}, iterations={}, rtt={}us)...",
                     cfg.clients, ResolveIterations(cfg), cfg.simulated_rtt_us);
        auto coloc_metrics = RunColocationBenchmark(cfg);
        all_metrics.insert(all_metrics.end(), coloc_metrics.begin(), coloc_metrics.end());
    }

    if (cfg.run_hotpath) {
        std::println(">>> Starting WhiskDB Prepared Statement & JIT Hot-Path Benchmark (clients={}, iterations={})...",
                     cfg.clients, ResolveIterations(cfg));
        auto hot_metrics = RunHotPathBenchmark(cfg);
        all_metrics.insert(all_metrics.end(), hot_metrics.begin(), hot_metrics.end());
    }

    if (cfg.markdown) {
        PrintMarkdown(all_metrics);
    } else {
        PrintTable(all_metrics);
    }

    if (!cfg.json_output_path.empty()) {
        WriteJson(all_metrics, cfg.json_output_path);
    }

    if (!cfg.csv_output_path.empty()) {
        WriteCsv(all_metrics, cfg.csv_output_path);
    }

    return 0;
}
