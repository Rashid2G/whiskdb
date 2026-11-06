#include "storage/snapshot_manager.hpp"

#include <atomic>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <print>
#include <string>
#include <unistd.h>
#include <vector>

using namespace database;
namespace fs = std::filesystem;

namespace {

constexpr idx_t kHeaderSize = 32;

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
    fs::path File(const std::string& name) const { return path_ / name; }
    const fs::path& Path() const { return path_; }

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

std::vector<uint8_t> MakeBytes(idx_t len, idx_t seed) {
    std::vector<uint8_t> v(len);
    uint64_t state = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    for (idx_t i = 0; i < len; i++) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        v[i] = static_cast<uint8_t>(state >> 56);
    }
    return v;
}

void TestRoundTrip() {
    TempDir dir;
    fs::path path = dir.File("db.snap");
    auto bytes = MakeBytes(1000, 1);
    assert(SnapshotManager::Save(path, 42, bytes).has_value());
    auto loaded = SnapshotManager::Load(path);
    assert(loaded.has_value() && loaded->lsn == 42 && loaded->bytes == bytes);
    assert(!fs::exists(path.string() + ".tmp"));

    // Overwrite with a newer snapshot
    auto bytes2 = MakeBytes(77, 2);
    assert(SnapshotManager::Save(path, 43, bytes2).has_value());
    loaded = SnapshotManager::Load(path);
    assert(loaded.has_value() && loaded->lsn == 43 && loaded->bytes == bytes2);

    // Empty payload is valid
    assert(SnapshotManager::Save(path, 7, {}).has_value());
    loaded = SnapshotManager::Load(path);
    assert(loaded.has_value() && loaded->lsn == 7 && loaded->bytes.empty());
    std::println("  Snapshot round trip OK");
}

void TestLargePayload() {
    TempDir dir;
    fs::path path = dir.File("big.snap");
    auto bytes = MakeBytes(8 * 1024 * 1024, 3);
    assert(SnapshotManager::Save(path, 123456789012ULL, bytes).has_value());
    assert(fs::file_size(path) == kHeaderSize + bytes.size());
    auto loaded = SnapshotManager::Load(path);
    assert(loaded.has_value() && loaded->lsn == 123456789012ULL && loaded->bytes == bytes);
    std::println("  8 MiB snapshot OK");
}

void TestAbsentAndDamaged() {
    TempDir dir;
    auto missing = SnapshotManager::Load(dir.File("nope.snap"));
    assert(!missing.has_value() && missing.error().Code() == ErrorCode::NotFound);

    fs::path path = dir.File("db.snap");
    auto bytes = MakeBytes(500, 4);
    assert(SnapshotManager::Save(path, 9, bytes).has_value());
    auto good = ReadFileBytes(path);

    // Flip a bit at every byte position (header and payload): always an error
    fs::path bad = dir.File("bad.snap");
    for (idx_t pos = 0; pos < good.size(); pos++) {
        auto damaged = good;
        damaged[pos] ^= 0x04;
        WriteFileBytes(bad, damaged);
        auto result = SnapshotManager::Load(bad);
        assert(!result.has_value());
        assert(result.error().Code() == ErrorCode::ChecksumMismatch ||
               result.error().Code() == ErrorCode::CorruptedData);
    }

    // Truncation at every length is an error; extra trailing bytes too
    for (idx_t len = 0; len < good.size(); len += 7) {
        WriteFileBytes(bad, std::vector<uint8_t>(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(len)));
        assert(!SnapshotManager::Load(bad).has_value());
    }
    auto extended = good;
    extended.push_back(0);
    WriteFileBytes(bad, extended);
    assert(!SnapshotManager::Load(bad).has_value());

    // Payload damage is reported as a checksum mismatch
    auto damaged = good;
    damaged[kHeaderSize + 10] ^= 0xFF;
    WriteFileBytes(bad, damaged);
    auto result = SnapshotManager::Load(bad);
    assert(!result.has_value() && result.error().Code() == ErrorCode::ChecksumMismatch);
    std::println("  Absent / damaged snapshots OK");
}

void TestCrashedSaveAndFailedSave() {
    TempDir dir;
    fs::path path = dir.File("db.snap");
    fs::path tmp = dir.File("db.snap.tmp");
    auto good_bytes = MakeBytes(300, 5);
    assert(SnapshotManager::Save(path, 5, good_bytes).has_value());

    // A leftover (half-written) temp file from a crashed save is ignored by Load...
    WriteFileBytes(tmp, MakeBytes(77, 6));
    auto loaded = SnapshotManager::Load(path);
    assert(loaded.has_value() && loaded->lsn == 5 && loaded->bytes == good_bytes);

    // ...and is simply replaced by the next successful save
    auto next_bytes = MakeBytes(400, 7);
    assert(SnapshotManager::Save(path, 6, next_bytes).has_value());
    assert(!fs::exists(tmp));
    loaded = SnapshotManager::Load(path);
    assert(loaded.has_value() && loaded->lsn == 6 && loaded->bytes == next_bytes);

    // A failing save (temp path is a directory) never clobbers the previous snapshot
    fs::create_directory(tmp);
    auto failed = SnapshotManager::Save(path, 7, MakeBytes(10, 8));
    assert(!failed.has_value() && failed.error().Code() == ErrorCode::IoError);
    loaded = SnapshotManager::Load(path);
    assert(loaded.has_value() && loaded->lsn == 6 && loaded->bytes == next_bytes);
    fs::remove(tmp);

    // Saving into a missing directory fails cleanly
    auto no_dir = SnapshotManager::Save(dir.File("missing_dir/db.snap"), 1, good_bytes);
    assert(!no_dir.has_value() && no_dir.error().Code() == ErrorCode::IoError);
    std::println("  Crashed / failed save safety OK");
}

} // namespace

int main() {
    std::println("--- Snapshot tests ---");
    TestRoundTrip();
    TestLargePayload();
    TestAbsentAndDamaged();
    TestCrashedSaveAndFailedSave();
    std::println("test_snapshot: OK");
    return 0;
}
