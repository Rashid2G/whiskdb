#include "common/types.hpp"
#include "engine.hpp"
#include "errors.hpp"
#include "logger.hpp"
#include "storage/durability.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
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
    bool quick = false;
    std::string json_output_path;
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
                             std::string_view extra = "") {
    BenchmarkMetric m;
    m.mode = std::string(mode);
    m.workload = std::string(workload);
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
            results.push_back(ComputeStats(mode_str, "insert_concur", combined_latencies, elapsed, extra));
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

void PrintTable(const std::vector<BenchmarkMetric>& metrics) {
    std::println("\n{:=^110}", " WHISKDB PERSISTENCE BENCHMARK RESULTS ");
    std::println("{:<13} | {:<15} | {:>8} | {:>12} | {:>10} | {:>10} | {:>10} | {:>10} | {:<16}",
                 "Mode", "Workload", "Ops", "Ops/sec", "Avg (us)", "p50 (us)", "p95 (us)", "p99 (us)", "Notes");
    std::println("{:-^110}", "");

    for (const auto& m : metrics) {
        std::println("{:<13} | {:<15} | {:>8} | {:>12.1f} | {:>10.1f} | {:>10.1f} | {:>10.1f} | {:>10.1f} | {:<16}",
                     m.mode, m.workload, m.total_ops, m.ops_per_sec, m.avg_lat_us, m.p50_lat_us,
                     m.p95_lat_us, m.p99_lat_us, m.extra_info);
    }
    std::println("{:=^110}\n", "");
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

void PrintHelp() {
    std::println("WhiskDB Standard Persistence Benchmark Suite");
    std::println("Usage: whisk_bench [options]");
    std::println("");
    std::println("Options:");
    std::println("  --mode <name>      strict | group_commit | relaxed | memory_only | all (default: all)");
    std::println("  --ops <N>          Override total operations per workload");
    std::println("  --threads <N>      Concurrency worker threads for concurrent workloads (default: 4)");
    std::println("  --quick            Run fast benchmark profile suitable for quick CI checks");
    std::println("  --json <path>      Output benchmark metrics to a JSON file");
    std::println("  -h, --help         Show this help message");
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
        } else if (arg == "--mode" && i + 1 < argc) {
            cfg.mode_filter = argv[++i];
        } else if (arg == "--ops" && i + 1 < argc) {
            cfg.custom_ops = static_cast<idx_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--threads" && i + 1 < argc) {
            cfg.threads = static_cast<idx_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--json" && i + 1 < argc) {
            cfg.json_output_path = argv[++i];
        }
    }

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

    std::println(">>> Starting WhiskDB Standard Benchmark across {} persistence mode(s)...", target_modes.size());
    std::println(">>> Config: quick={}, threads={}, mode={}", cfg.quick, cfg.threads, cfg.mode_filter);

    std::vector<BenchmarkMetric> all_metrics;
    for (auto mode : target_modes) {
        std::println(">>> Benchmarking mode: {} ...", ToString(mode));
        auto mode_metrics = RunModeBenchmarks(mode, cfg);
        all_metrics.insert(all_metrics.end(), mode_metrics.begin(), mode_metrics.end());
    }

    PrintTable(all_metrics);

    if (!cfg.json_output_path.empty()) {
        WriteJson(all_metrics, cfg.json_output_path);
    }

    return 0;
}
