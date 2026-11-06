#include "storage/wal_reader.hpp"

#include "common/crc32.hpp"
#include "posix_file.hpp"
#include "wal_format.hpp"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace database {

namespace {

using namespace wal_format;

enum class ParseStatus : uint8_t { Valid, Incomplete, BadCrc, UnknownType };

struct ParsedRecord {
    ParseStatus status = ParseStatus::Incomplete;
    lsn_t lsn = 0;
    uint8_t type = 0;
    std::span<const uint8_t> payload;
    idx_t size = 0;
};

bool IsKnownType(uint8_t type) {
    return type == static_cast<uint8_t>(WalRecordType::SqlStatement) ||
           type == static_cast<uint8_t>(WalRecordType::Checkpoint);
}

ParsedRecord ParseRecordAt(std::span<const uint8_t> data, idx_t offset) {
    ParsedRecord rec;
    // Callers only pass offsets inside the buffer
    D_ASSERT(offset <= data.size());
    idx_t remaining = data.size() - offset;
    if (remaining < kRecordOverhead) {
        return rec;
    }
    const uint8_t* p = data.data() + offset;
    rec.lsn = GetU64(p);
    rec.type = p[16];
    idx_t payload_len = GetU32(p + 17);
    if (payload_len > kMaxPayloadSize || payload_len > remaining - kRecordOverhead) {
        rec.status = ParseStatus::Incomplete;
        return rec;
    }
    idx_t total = kRecordOverhead + payload_len;
    uint32_t stored_crc = GetU32(p + total - kRecordTrailerSize);
    if (Crc32(std::span<const uint8_t>(p, total - kRecordTrailerSize)) != stored_crc) {
        rec.status = ParseStatus::BadCrc;
        return rec;
    }
    rec.size = total;
    rec.payload = std::span<const uint8_t>(p + kRecordHeaderSize, payload_len);
    rec.status = IsKnownType(rec.type) ? ParseStatus::Valid : ParseStatus::UnknownType;
    return rec;
}

// Error-path resync scan: is there any CRC-valid record after `offset`? If so the
// bad record at `offset` is not a crash-torn tail but damage in the middle of the log.
bool HasValidRecordAfter(std::span<const uint8_t> data, idx_t offset) {
    for (idx_t pos = offset + 1; pos + kRecordOverhead <= data.size(); pos++) {
        ParseStatus status = ParseRecordAt(data, pos).status;
        if (status == ParseStatus::Valid || status == ParseStatus::UnknownType) {
            return true;
        }
    }
    return false;
}

} // namespace

Result<WalReplayStats> ReplayWal(const std::filesystem::path& path, lsn_t after_lsn,
                                 const WalRecordCallback& cb) {
    WalReplayStats stats;
    posix::UniqueFd fd(::open(path.c_str(), O_RDWR | O_CLOEXEC));
    if (!fd.IsValid()) {
        if (errno == ENOENT) {
            return stats;
        }
        return posix::IoError("open", path, errno);
    }

    std::vector<uint8_t> file_bytes;
    if (auto read_result = posix::ReadWholeFile(fd.Get(), file_bytes, path); !read_result) {
        return std::unexpected(read_result.error());
    }
    std::span<const uint8_t> data(file_bytes);

    // Header validation; a file shorter than the header that is a prefix of it is a
    // crash during creation and holds no records
    auto expected_header = MakeFileHeader();
    if (data.size() < kFileHeaderSize) {
        if (!std::equal(data.begin(), data.end(), expected_header.begin())) {
            return MAKE_ERROR(ErrorCode::CorruptedData, "WAL '{}' has a bad header", path.string());
        }
        return stats;
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), data.begin())) {
        return MAKE_ERROR(ErrorCode::CorruptedData, "WAL '{}' has bad magic (not a WAL file)",
                          path.string());
    }
    if (GetU32(data.data() + 8) != kVersion) {
        return MAKE_ERROR(ErrorCode::CorruptedData, "WAL '{}' has unsupported version {}",
                          path.string(), GetU32(data.data() + 8));
    }

    idx_t offset = kFileHeaderSize;
    while (offset < data.size()) {
        ParsedRecord rec = ParseRecordAt(data, offset);
        if (rec.status == ParseStatus::Valid) {
            if (stats.records_seen > 0 && rec.lsn <= stats.last_lsn) {
                return MAKE_ERROR(ErrorCode::CorruptedData,
                                  "WAL '{}' non-monotonic LSN {} after {} at offset {}",
                                  path.string(), rec.lsn, stats.last_lsn, offset);
            }
            stats.records_seen++;
            stats.last_lsn = rec.lsn;
            if (rec.lsn > after_lsn) {
                auto cb_result = cb(rec.lsn, static_cast<WalRecordType>(rec.type), rec.payload);
                if (!cb_result) {
                    return std::unexpected(cb_result.error());
                }
                stats.records_applied++;
            }
            offset += rec.size;
            continue;
        }
        if (rec.status == ParseStatus::UnknownType) {
            return MAKE_ERROR(ErrorCode::CorruptedData, "WAL '{}' has unknown record type {} at offset {}",
                              path.string(), static_cast<uint32_t>(rec.type), offset);
        }
        if (HasValidRecordAfter(data, offset)) {
            return MAKE_ERROR(rec.status == ParseStatus::BadCrc ? ErrorCode::ChecksumMismatch
                                                                 : ErrorCode::CorruptedData,
                              "WAL '{}' record at offset {} is damaged but valid records follow",
                              path.string(), offset);
        }

        // Crash artifact: drop the torn tail so future appends start at a record boundary
        if (::ftruncate(fd.Get(), static_cast<off_t>(offset)) != 0) {
            return posix::IoError("ftruncate", path, errno);
        }
        if (::fsync(fd.Get()) != 0) {
            return posix::IoError("fsync", path, errno);
        }
        stats.truncated_tail = true;
        stats.truncated_bytes = data.size() - offset;
        LOG_WARN("WAL '{}': truncated {} torn tail bytes at offset {}", path.string(),
                 stats.truncated_bytes, offset);
        break;
    }
    return stats;
}

} // namespace database
