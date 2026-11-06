#include "storage/durability.hpp"
#include "storage/wal_manager.hpp"
#include "storage/wal_reader.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <print>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace database;
namespace fs = std::filesystem;

namespace {

constexpr idx_t kFileHeaderSize = 16;
constexpr idx_t kRecordOverhead = 25;

struct Rec {
    lsn_t lsn;
    WalRecordType type;
    std::vector<uint8_t> payload;
};

class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = fs::temp_directory_path() /
                std::format("whisk_test_{}_{}", ::getpid(), counter.fetch_add(1));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const fs::path& Path() const { return path_; }
    fs::path File(const std::string& name) const { return path_ / name; }

private:
    fs::path path_;
};

std::vector<uint8_t> ReadFileBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void WriteFileBytes(const fs::path& p, const std::vector<uint8_t>& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<uint8_t> MakePayload(idx_t seed, idx_t len) {
    std::vector<uint8_t> v(len);
    for (idx_t i = 0; i < len; i++) {
        v[i] = static_cast<uint8_t>((seed * 31 + i * 7) & 0xFF);
    }
    return v;
}

Result<WalReplayStats> Replay(const fs::path& p, lsn_t after, std::vector<Rec>& out) {
    return ReplayWal(p, after,
                     [&out](lsn_t lsn, WalRecordType type, std::span<const uint8_t> payload) -> Result<void> {
                         out.push_back({lsn, type, std::vector<uint8_t>(payload.begin(), payload.end())});
                         return {};
                     });
}

std::unique_ptr<WalManager> OpenWal(const fs::path& p, DurabilityMode mode, lsn_t next_lsn = 1) {
    WalOptions opts;
    opts.path = p;
    opts.mode = mode;
    auto wal = WalManager::Open(opts, next_lsn);
    assert(wal.has_value());
    return std::move(*wal);
}

constexpr DurabilityMode kAllModes[] = {DurabilityMode::Strict, DurabilityMode::GroupCommit,
                                        DurabilityMode::Relaxed, DurabilityMode::MemoryOnly};

// Builds a 5-record WAL and returns the byte offset where the last record starts
idx_t BuildFiveRecordWal(const fs::path& p) {
    auto wal = OpenWal(p, DurabilityMode::Relaxed);
    for (idx_t i = 0; i < 5; i++) {
        auto payload = MakePayload(i, 10 + i * 5);
        assert(wal->Append(WalRecordType::SqlStatement, payload).has_value());
    }
    wal.reset();
    idx_t last_len = kRecordOverhead + 10 + 4 * 5;
    return fs::file_size(p) - last_len;
}

void TestParseDurabilityMode() {
    struct Case {
        const char* name;
        DurabilityMode mode;
    };
    const Case cases[] = {
        {"strict", DurabilityMode::Strict},          {"sync", DurabilityMode::Strict},
        {"group_commit", DurabilityMode::GroupCommit}, {"batched", DurabilityMode::GroupCommit},
        {"relaxed", DurabilityMode::Relaxed},        {"async", DurabilityMode::Relaxed},
        {"memory_only", DurabilityMode::MemoryOnly}, {"ephemeral", DurabilityMode::MemoryOnly},
        {"STRICT", DurabilityMode::Strict},          {"Group_Commit", DurabilityMode::GroupCommit},
        {"EpHeMeRaL", DurabilityMode::MemoryOnly},
    };
    for (const auto& c : cases) {
        auto parsed = ParseDurabilityMode(c.name);
        assert(parsed.has_value() && *parsed == c.mode);
    }
    for (const char* bad : {"", "stric", "group-commit", "fast", " strict", "strict "}) {
        auto parsed = ParseDurabilityMode(bad);
        assert(!parsed.has_value() && parsed.error().Code() == ErrorCode::InvalidArgument);
    }
    for (auto mode : kAllModes) {
        auto round_trip = ParseDurabilityMode(ToString(mode));
        assert(round_trip.has_value() && *round_trip == mode);
    }
    std::println("  ParseDurabilityMode OK");
}

void TestRoundTripAllModes() {
    for (auto mode : kAllModes) {
        TempDir dir;
        fs::path path = dir.File("wal.log");
        std::vector<Rec> expected;
        {
            auto wal = OpenWal(path, mode);
            assert(wal->LastLsn() == 0);
            for (idx_t i = 0; i < 40; i++) {
                auto payload = MakePayload(i, i * 3);  // includes the empty payload
                auto type = (i % 10 == 9) ? WalRecordType::Checkpoint : WalRecordType::SqlStatement;
                auto lsn = wal->Append(type, payload);
                assert(lsn.has_value());
                assert(*lsn == i + 1);
                expected.push_back({*lsn, type, payload});
            }
            assert(wal->LastLsn() == 40);
            assert(wal->GetStats().appended == 40);
        }
        if (mode == DurabilityMode::MemoryOnly) {
            assert(fs::is_empty(dir.Path()));
            continue;
        }
        std::vector<Rec> got;
        auto stats = Replay(path, 0, got);
        assert(stats.has_value());
        assert(stats->records_seen == 40 && stats->records_applied == 40 && stats->last_lsn == 40);
        assert(!stats->truncated_tail);
        assert(got.size() == expected.size());
        for (idx_t i = 0; i < got.size(); i++) {
            assert(got[i].lsn == expected[i].lsn && got[i].type == expected[i].type &&
                   got[i].payload == expected[i].payload);
        }
    }
    std::println("  Round trip in all modes OK");
}

void TestAfterLsnAndReopen() {
    TempDir dir;
    fs::path path = dir.File("wal.log");
    {
        auto wal = OpenWal(path, DurabilityMode::Strict);
        for (idx_t i = 0; i < 10; i++) {
            assert(wal->Append(WalRecordType::SqlStatement, MakePayload(i, 8)).has_value());
        }
    }
    std::vector<Rec> got;
    auto stats = Replay(path, 6, got);
    assert(stats.has_value());
    assert(stats->records_seen == 10 && stats->records_applied == 4 && stats->last_lsn == 10);
    assert(got.size() == 4 && got.front().lsn == 7 && got.back().lsn == 10);

    got.clear();
    stats = Replay(path, 10, got);
    assert(stats.has_value() && got.empty() && stats->records_seen == 10);

    // Reopen and continue numbering
    {
        auto wal = OpenWal(path, DurabilityMode::GroupCommit, 11);
        assert(wal->LastLsn() == 10);
        auto lsn = wal->Append(WalRecordType::SqlStatement, MakePayload(99, 4));
        assert(lsn.has_value() && *lsn == 11);
    }
    got.clear();
    stats = Replay(path, 0, got);
    assert(stats.has_value() && stats->records_seen == 11 && got.back().lsn == 11);
    for (idx_t i = 1; i < got.size(); i++) {
        assert(got[i].lsn == got[i - 1].lsn + 1);
    }

    // Missing file is an empty log
    got.clear();
    stats = Replay(dir.File("missing.log"), 0, got);
    assert(stats.has_value() && stats->records_seen == 0 && !stats->truncated_tail);
    assert(!fs::exists(dir.File("missing.log")));
    std::println("  after_lsn filtering and reopen OK");
}

void TestTornTailEveryOffset() {
    TempDir dir;
    fs::path path = dir.File("wal.log");
    idx_t last_start = BuildFiveRecordWal(path);
    std::vector<uint8_t> full = ReadFileBytes(path);
    assert(last_start > kFileHeaderSize && last_start < full.size());

    fs::path copy = dir.File("copy.log");
    for (idx_t cut = last_start; cut < full.size(); cut++) {
        std::vector<uint8_t> truncated(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(cut));
        WriteFileBytes(copy, truncated);
        std::vector<Rec> got;
        auto stats = Replay(copy, 0, got);
        assert(stats.has_value());
        assert(got.size() == 4 && stats->records_applied == 4 && stats->last_lsn == 4);
        assert(stats->truncated_tail == (cut != last_start));
        assert(stats->truncated_bytes == cut - last_start);
        // The file now ends exactly at the last good record
        assert(fs::file_size(copy) == last_start);

        // Replaying again is clean, and appending after recovery works
        got.clear();
        stats = Replay(copy, 0, got);
        assert(stats.has_value() && !stats->truncated_tail && got.size() == 4);
    }

    // Recovery then append continues cleanly
    {
        WriteFileBytes(copy, std::vector<uint8_t>(full.begin(), full.end() - 3));
        std::vector<Rec> got;
        assert(Replay(copy, 0, got).has_value());
        auto wal = OpenWal(copy, DurabilityMode::Strict, 5);
        assert(wal->Append(WalRecordType::SqlStatement, MakePayload(5, 9)).has_value());
        wal.reset();
        got.clear();
        auto stats = Replay(copy, 0, got);
        assert(stats.has_value() && got.size() == 5 && got.back().lsn == 5 && !stats->truncated_tail);
    }

    // Header-only torn file (crash while creating) holds no records
    for (idx_t cut = 0; cut < kFileHeaderSize; cut++) {
        WriteFileBytes(copy, std::vector<uint8_t>(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(cut)));
        std::vector<Rec> got;
        auto stats = Replay(copy, 0, got);
        assert(stats.has_value() && got.empty());
        auto wal = WalManager::Open(WalOptions{copy, DurabilityMode::Strict}, 1);
        assert(wal.has_value());
        assert((*wal)->Append(WalRecordType::SqlStatement, MakePayload(1, 3)).has_value());
    }
    std::println("  Torn tail at every offset OK");
}

void TestBitFlips() {
    TempDir dir;
    fs::path path = dir.File("wal.log");
    idx_t last_start = BuildFiveRecordWal(path);
    std::vector<uint8_t> full = ReadFileBytes(path);
    fs::path copy = dir.File("copy.log");

    // Flip every bit position of the last record's bytes -> treated as a torn tail
    for (idx_t pos = last_start; pos < full.size(); pos++) {
        std::vector<uint8_t> damaged = full;
        damaged[pos] ^= 0x10;
        WriteFileBytes(copy, damaged);
        std::vector<Rec> got;
        auto stats = Replay(copy, 0, got);
        assert(stats.has_value());
        assert(stats->truncated_tail && got.size() == 4);
        assert(fs::file_size(copy) == last_start);
    }

    // Record 2 starts after header + record 1 (overhead 25 + payload 10)
    idx_t rec2_start = kFileHeaderSize + kRecordOverhead + 10;
    idx_t rec2_len = kRecordOverhead + 15;
    for (idx_t pos = rec2_start; pos < rec2_start + rec2_len; pos++) {
        std::vector<uint8_t> damaged = full;
        damaged[pos] ^= 0x01;
        WriteFileBytes(copy, damaged);
        std::vector<Rec> got;
        auto stats = Replay(copy, 0, got);
        assert(!stats.has_value());
        assert(stats.error().Code() == ErrorCode::ChecksumMismatch ||
               stats.error().Code() == ErrorCode::CorruptedData);
        // The damaged file must be left untouched (no silent truncation)
        assert(fs::file_size(copy) == full.size());
    }

    // Header corruption
    {
        std::vector<uint8_t> damaged = full;
        damaged[0] = 'X';
        WriteFileBytes(copy, damaged);
        std::vector<Rec> got;
        auto stats = Replay(copy, 0, got);
        assert(!stats.has_value() && stats.error().Code() == ErrorCode::CorruptedData);
        auto wal = WalManager::Open(WalOptions{copy, DurabilityMode::Strict}, 1);
        assert(!wal.has_value() && wal.error().Code() == ErrorCode::CorruptedData);
        assert(fs::file_size(copy) == full.size());

        std::vector<uint8_t> garbage(100, 0x5A);
        WriteFileBytes(copy, garbage);
        assert(!Replay(copy, 0, got).has_value());
        assert(!WalManager::Open(WalOptions{copy, DurabilityMode::Strict}, 1).has_value());
    }

    // Non-monotonic LSN (valid CRCs) is corruption: concatenate record 1 twice
    {
        idx_t rec1_len = kRecordOverhead + 10;
        std::vector<uint8_t> dup(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(kFileHeaderSize + rec1_len));
        dup.insert(dup.end(), full.begin() + static_cast<std::ptrdiff_t>(kFileHeaderSize),
                   full.begin() + static_cast<std::ptrdiff_t>(kFileHeaderSize + rec1_len));
        WriteFileBytes(copy, dup);
        std::vector<Rec> got;
        auto stats = Replay(copy, 0, got);
        assert(!stats.has_value() && stats.error().Code() == ErrorCode::CorruptedData);
    }
    std::println("  Bit flips (tail=truncate, middle=error, header, non-monotonic) OK");
}

void TestStrictFsyncPerAppend() {
    TempDir dir;
    auto wal = OpenWal(dir.File("wal.log"), DurabilityMode::Strict);
    for (idx_t i = 0; i < 25; i++) {
        assert(wal->Append(WalRecordType::SqlStatement, MakePayload(i, 20)).has_value());
        assert(wal->GetStats().fsync_calls == i + 1);
    }
    auto stats = wal->GetStats();
    assert(stats.appended == 25 && stats.bytes_written == 25 * (kRecordOverhead + 20));
    std::println("  Strict: one fdatasync per append OK");
}

void TestGroupCommit() {
    TempDir dir;
    fs::path path = dir.File("wal.log");
    constexpr idx_t kThreads = 16;
    constexpr idx_t kPerThread = 200;
    {
        auto wal = OpenWal(path, DurabilityMode::GroupCommit);

        // A lone append returns promptly
        auto start = std::chrono::steady_clock::now();
        assert(wal->Append(WalRecordType::SqlStatement, MakePayload(0, 16)).has_value());
        auto lone_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        assert(lone_ms < 50);
        uint64_t fsync_before = wal->GetStats().fsync_calls;

        std::vector<std::thread> threads;
        std::atomic<idx_t> failures{0};
        auto t0 = std::chrono::steady_clock::now();
        for (idx_t t = 0; t < kThreads; t++) {
            threads.emplace_back([&wal, &failures, t] {
                for (idx_t i = 0; i < kPerThread; i++) {
                    auto payload = MakePayload(t * 1000 + i, 32);
                    if (!wal->Append(WalRecordType::SqlStatement, payload).has_value()) {
                        failures++;
                    }
                }
            });
        }
        for (auto& th : threads) {
            th.join();
        }
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        assert(failures == 0);
        auto stats = wal->GetStats();
        uint64_t fsyncs = stats.fsync_calls - fsync_before;
        std::println("  GroupCommit: {} threads x {} appends -> {} fdatasync calls, {:.0f} appends/s",
                     kThreads, kPerThread, fsyncs, static_cast<double>(kThreads * kPerThread) / secs);
        assert(stats.appended == kThreads * kPerThread + 1);
        assert(fsyncs < kThreads * kPerThread / 4);
    }
    std::vector<Rec> got;
    auto stats = Replay(path, 0, got);
    assert(stats.has_value() && !stats->truncated_tail);
    assert(got.size() == kThreads * kPerThread + 1);
    for (idx_t i = 0; i < got.size(); i++) {
        assert(got[i].lsn == i + 1);
    }
    std::println("  GroupCommit batching + lone latency OK");
}

void TestRelaxed() {
    TempDir dir;
    fs::path path = dir.File("wal.log");
    {
        auto wal = OpenWal(path, DurabilityMode::Relaxed);
        for (idx_t i = 0; i < 2000; i++) {
            assert(wal->Append(WalRecordType::SqlStatement, MakePayload(i, 16)).has_value());
        }
        // Appends never fsync inline; the background syncer runs at most every ~100 ms
        assert(wal->GetStats().fsync_calls < 100);
        assert(wal->Flush().has_value());
        assert(wal->GetStats().fsync_calls >= 1);
    }
    std::vector<Rec> got;
    auto stats = Replay(path, 0, got);
    assert(stats.has_value() && got.size() == 2000);
    std::println("  Relaxed: no per-append fsync OK");
}

void TestResetAndDestructorFlush() {
    TempDir dir;
    fs::path path = dir.File("wal.log");
    {
        auto wal = OpenWal(path, DurabilityMode::GroupCommit);
        for (idx_t i = 0; i < 20; i++) {
            assert(wal->Append(WalRecordType::SqlStatement, MakePayload(i, 30)).has_value());
        }
        // Reset truncates the log and numbering continues at the requested LSN
        assert(wal->Reset(100).has_value());
        assert(wal->LastLsn() == 99);
        assert(fs::file_size(path) == kFileHeaderSize);
        auto lsn = wal->Append(WalRecordType::Checkpoint, MakePayload(1, 5));
        assert(lsn.has_value() && *lsn == 100);

        // Destructor must flush records that nobody waited for
        for (idx_t i = 0; i < 10; i++) {
            auto no_wait = wal->AppendNoWait(WalRecordType::SqlStatement, MakePayload(i, 7));
            assert(no_wait.has_value() && *no_wait == 101 + i);
        }
    }
    std::vector<Rec> got;
    auto stats = Replay(path, 0, got);
    assert(stats.has_value() && got.size() == 11 && got.front().lsn == 100 && got.back().lsn == 110);
    assert(got.front().type == WalRecordType::Checkpoint);

    // Reset in every file-backed mode
    for (auto mode : {DurabilityMode::Strict, DurabilityMode::Relaxed, DurabilityMode::GroupCommit}) {
        fs::path p2 = dir.File(std::format("reset_{}.log", ToString(mode)));
        auto wal = OpenWal(p2, mode);
        for (idx_t i = 0; i < 5; i++) {
            assert(wal->Append(WalRecordType::SqlStatement, MakePayload(i, 5)).has_value());
        }
        assert(wal->Reset(6).has_value());
        assert(fs::file_size(p2) == kFileHeaderSize);
        assert(*wal->Append(WalRecordType::SqlStatement, MakePayload(1, 5)) == 6);
        wal.reset();
        got.clear();
        stats = Replay(p2, 0, got);
        assert(stats.has_value() && got.size() == 1 && got[0].lsn == 6);
    }

    // Relaxed destructor flush
    fs::path p3 = dir.File("relaxed.log");
    {
        auto wal = OpenWal(p3, DurabilityMode::Relaxed);
        assert(wal->AppendNoWait(WalRecordType::SqlStatement, MakePayload(1, 5)).has_value());
    }
    got.clear();
    assert(Replay(p3, 0, got).has_value() && got.size() == 1);

    // Memory-only Reset keeps numbering
    auto mem = WalManager::Open(WalOptions{{}, DurabilityMode::MemoryOnly}, 5);
    assert(mem.has_value());
    assert(*(*mem)->Append(WalRecordType::SqlStatement, MakePayload(1, 1)) == 5);
    assert((*mem)->Reset(50).has_value());
    assert(*(*mem)->Append(WalRecordType::SqlStatement, MakePayload(1, 1)) == 50);
    assert((*mem)->Flush().has_value());
    std::println("  Reset and destructor flush OK");
}

void TestCallbackError() {
    TempDir dir;
    fs::path path = dir.File("wal.log");
    {
        auto wal = OpenWal(path, DurabilityMode::Relaxed);
        for (idx_t i = 0; i < 6; i++) {
            assert(wal->Append(WalRecordType::SqlStatement, MakePayload(i, 6)).has_value());
        }
    }
    idx_t calls = 0;
    auto stats = ReplayWal(path, 0, [&calls](lsn_t lsn, WalRecordType, std::span<const uint8_t>) -> Result<void> {
        calls++;
        if (lsn == 3) {
            return MakeError(ErrorCode::InvalidArgument, "apply failed");
        }
        return {};
    });
    assert(!stats.has_value() && stats.error().Code() == ErrorCode::InvalidArgument);
    assert(calls == 3);
    assert(stats.error().Message() == "apply failed");
    std::println("  Callback error propagates OK");
}

void TestAppendNoWaitWaitDurable() {
    for (auto mode : {DurabilityMode::Strict, DurabilityMode::GroupCommit}) {
        TempDir dir;
        fs::path path = dir.File("wal.log");
        constexpr idx_t kThreads = 8;
        constexpr idx_t kPerThread = 100;
        std::vector<std::pair<lsn_t, idx_t>> call_order;  // (lsn, tag) in caller-mutex order
        uint64_t fsyncs = 0;
        {
            auto wal = OpenWal(path, mode);
            std::mutex caller_mutex;
            std::atomic<idx_t> failures{0};
            std::vector<std::thread> threads;
            for (idx_t t = 0; t < kThreads; t++) {
                threads.emplace_back([&, t] {
                    for (idx_t i = 0; i < kPerThread; i++) {
                        idx_t tag = t * kPerThread + i;
                        std::vector<uint8_t> payload(8);
                        for (idx_t b = 0; b < 8; b++) {
                            payload[b] = static_cast<uint8_t>(tag >> (8 * b));
                        }
                        lsn_t lsn = 0;
                        {
                            std::lock_guard<std::mutex> lock(caller_mutex);
                            auto result = wal->AppendNoWait(WalRecordType::SqlStatement, payload);
                            if (!result.has_value()) {
                                failures++;
                                continue;
                            }
                            lsn = *result;
                            call_order.emplace_back(lsn, tag);
                        }
                        if (!wal->WaitDurable(lsn).has_value()) {
                            failures++;
                        }
                    }
                });
            }
            for (auto& th : threads) {
                th.join();
            }
            assert(failures == 0);
            auto stats = wal->GetStats();
            fsyncs = stats.fsync_calls;
            assert(stats.appended == kThreads * kPerThread);
            // Waiting for an already-durable LSN is free and invalid LSNs are rejected
            uint64_t before = wal->GetStats().fsync_calls;
            assert(wal->WaitDurable(1).has_value());
            assert(wal->GetStats().fsync_calls == before);
            assert(!wal->WaitDurable(wal->LastLsn() + 1).has_value());
        }
        std::println("  AppendNoWait/WaitDurable [{}]: {} appends -> {} fdatasync calls", ToString(mode),
                     kThreads * kPerThread, fsyncs);
        assert(fsyncs < kThreads * kPerThread);

        std::vector<Rec> got;
        auto stats = Replay(path, 0, got);
        assert(stats.has_value() && got.size() == kThreads * kPerThread);
        // LSN order in the file equals the order in which callers appended under their mutex
        for (idx_t i = 0; i < got.size(); i++) {
            assert(got[i].lsn == i + 1);
            assert(call_order[i].first == got[i].lsn);
            idx_t tag = 0;
            for (idx_t b = 0; b < 8; b++) {
                tag |= static_cast<idx_t>(got[i].payload[b]) << (8 * b);
            }
            assert(tag == call_order[i].second);
        }
    }

    // Relaxed and MemoryOnly: WaitDurable returns immediately
    for (auto mode : {DurabilityMode::Relaxed, DurabilityMode::MemoryOnly}) {
        TempDir dir;
        auto wal = OpenWal(dir.File("wal.log"), mode);
        auto lsn = wal->AppendNoWait(WalRecordType::SqlStatement, MakePayload(1, 4));
        assert(lsn.has_value() && wal->WaitDurable(*lsn).has_value());
        assert(wal->GetStats().fsync_calls == 0);
    }
    std::println("  AppendNoWait + WaitDurable OK");
}

void PrintThroughput() {
    struct Config {
        DurabilityMode mode;
        idx_t threads;
        idx_t per_thread;
    };
    const Config configs[] = {
        {DurabilityMode::Strict, 1, 300},
        {DurabilityMode::GroupCommit, 8, 1000},
        {DurabilityMode::Relaxed, 1, 50000},
        {DurabilityMode::MemoryOnly, 1, 200000},
    };
    for (const auto& cfg : configs) {
        TempDir dir;
        auto wal = OpenWal(dir.File("wal.log"), cfg.mode);
        std::vector<uint8_t> payload = MakePayload(1, 100);
        auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> threads;
        for (idx_t t = 0; t < cfg.threads; t++) {
            threads.emplace_back([&wal, &payload, &cfg] {
                for (idx_t i = 0; i < cfg.per_thread; i++) {
                    auto result = wal->Append(WalRecordType::SqlStatement, payload);
                    assert(result.has_value());
                }
            });
        }
        for (auto& th : threads) {
            th.join();
        }
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        idx_t total = cfg.threads * cfg.per_thread;
        std::println("  THROUGHPUT {:<12} {} thread(s): {:>10.0f} appends/s ({} fsyncs)", ToString(cfg.mode),
                     cfg.threads, static_cast<double>(total) / secs, wal->GetStats().fsync_calls);
    }
}

} // namespace

int main() {
    std::println("--- WAL tests ---");
    TestParseDurabilityMode();
    TestRoundTripAllModes();
    TestAfterLsnAndReopen();
    TestTornTailEveryOffset();
    TestBitFlips();
    TestStrictFsyncPerAppend();
    TestGroupCommit();
    TestRelaxed();
    TestResetAndDestructorFlush();
    TestCallbackError();
    TestAppendNoWaitWaitDurable();
    PrintThroughput();
    std::println("test_wal: OK");
    return 0;
}
