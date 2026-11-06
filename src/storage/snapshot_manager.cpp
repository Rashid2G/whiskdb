#include "storage/snapshot_manager.hpp"

#include "common/crc32.hpp"
#include "posix_file.hpp"
#include "wal_format.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace database {

namespace {

using wal_format::GetU32;
using wal_format::GetU64;
using wal_format::PutU32;
using wal_format::PutU64;

constexpr std::array<uint8_t, 4> kSnapshotMagic = {'W', 'H', 'S', 'K'};
constexpr uint32_t kSnapshotVersion = 1;
constexpr idx_t kSnapshotHeaderSize = 32;
constexpr idx_t kHeaderCrcOffset = 28;

std::filesystem::path TempPathFor(const std::filesystem::path& path) {
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    return tmp;
}

// Writes header + payload to `tmp`, fdatasyncs it
Result<void> WriteTempFile(const std::filesystem::path& tmp,
                           std::span<const uint8_t> header,
                           std::span<const uint8_t> payload) {
    posix::UniqueFd fd(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
    if (!fd.IsValid()) {
        return posix::IoError("open", tmp, errno);
    }
    if (auto result = posix::WriteAll(fd.Get(), header, tmp); !result) {
        return result;
    }
    if (auto result = posix::WriteAll(fd.Get(), payload, tmp); !result) {
        return result;
    }
    return posix::DataSync(fd.Get(), tmp);
}

} // namespace

Result<void> SnapshotManager::Save(const std::filesystem::path& path, lsn_t lsn,
                                   std::span<const uint8_t> db_bytes) {
    std::array<uint8_t, kSnapshotHeaderSize> header{};
    std::copy(kSnapshotMagic.begin(), kSnapshotMagic.end(), header.begin());
    PutU32(header.data() + 4, kSnapshotVersion);
    PutU64(header.data() + 8, lsn);
    PutU64(header.data() + 16, db_bytes.size());
    PutU32(header.data() + 24, Crc32(db_bytes));
    PutU32(header.data() + kHeaderCrcOffset,
           Crc32(std::span<const uint8_t>(header.data(), kHeaderCrcOffset)));

    std::filesystem::path tmp = TempPathFor(path);
    if (auto result = WriteTempFile(tmp, header, db_bytes); !result) {
        ::unlink(tmp.c_str());
        return result;
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        int err = errno;
        ::unlink(tmp.c_str());
        return posix::IoError("rename", path, err);
    }
    return posix::SyncDirectory(path.parent_path());
}

Result<SnapshotData> SnapshotManager::Load(const std::filesystem::path& path) {
    posix::UniqueFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (!fd.IsValid()) {
        if (errno == ENOENT) {
            return MAKE_ERROR(ErrorCode::NotFound, "snapshot '{}' does not exist", path.string());
        }
        return posix::IoError("open", path, errno);
    }
    struct stat st {};
    if (::fstat(fd.Get(), &st) != 0) {
        return posix::IoError("fstat", path, errno);
    }
    auto file_size = static_cast<idx_t>(st.st_size);
    if (file_size < kSnapshotHeaderSize) {
        return MAKE_ERROR(ErrorCode::CorruptedData, "snapshot '{}' is truncated ({} bytes)",
                          path.string(), file_size);
    }

    std::array<uint8_t, kSnapshotHeaderSize> header{};
    auto header_read = posix::ReadUpTo(fd.Get(), header, path);
    if (!header_read) {
        return std::unexpected(header_read.error());
    }
    if (*header_read != kSnapshotHeaderSize ||
        !std::equal(kSnapshotMagic.begin(), kSnapshotMagic.end(), header.begin())) {
        return MAKE_ERROR(ErrorCode::CorruptedData, "snapshot '{}' has bad magic", path.string());
    }
    if (Crc32(std::span<const uint8_t>(header.data(), kHeaderCrcOffset)) !=
        GetU32(header.data() + kHeaderCrcOffset)) {
        return MAKE_ERROR(ErrorCode::ChecksumMismatch, "snapshot '{}' header checksum mismatch",
                          path.string());
    }
    if (GetU32(header.data() + 4) != kSnapshotVersion) {
        return MAKE_ERROR(ErrorCode::CorruptedData, "snapshot '{}' has unsupported version {}",
                          path.string(), GetU32(header.data() + 4));
    }
    uint64_t payload_size = GetU64(header.data() + 16);
    // Validate before allocating so a damaged size field cannot trigger a huge allocation
    if (payload_size != file_size - kSnapshotHeaderSize) {
        return MAKE_ERROR(ErrorCode::CorruptedData,
                          "snapshot '{}' size mismatch (header says {} payload bytes, file has {})",
                          path.string(), payload_size, file_size - kSnapshotHeaderSize);
    }

    SnapshotData data;
    data.lsn = GetU64(header.data() + 8);
    data.bytes.resize(payload_size);
    auto payload_read = posix::ReadUpTo(fd.Get(), data.bytes, path);
    if (!payload_read) {
        return std::unexpected(payload_read.error());
    }
    if (*payload_read != payload_size) {
        return MAKE_ERROR(ErrorCode::CorruptedData, "snapshot '{}' payload is truncated", path.string());
    }
    if (Crc32(data.bytes) != GetU32(header.data() + 24)) {
        return MAKE_ERROR(ErrorCode::ChecksumMismatch, "snapshot '{}' payload checksum mismatch",
                          path.string());
    }
    return data;
}

} // namespace database
