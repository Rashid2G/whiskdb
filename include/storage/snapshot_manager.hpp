#ifndef WHISKDB_STORAGE_SNAPSHOT_MANAGER_HPP
#define WHISKDB_STORAGE_SNAPSHOT_MANAGER_HPP

#include "common/types.hpp"
#include "errors.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace database {

struct SnapshotData {
    lsn_t lsn = 0;
    std::vector<uint8_t> bytes;
};

/**
 * @brief Atomic, checksummed point-in-time database image.
 *
 * File layout (little-endian): "WHSK"[4] version u32, snapshot lsn u64,
 * payload size u64, payload CRC32 u32, header CRC32 u32 (over the first 28
 * bytes), then the payload.
 */
class SnapshotManager {
public:
    /// Writes `<path>.tmp` in the same directory, fdatasyncs it, renames it over
    /// `path` and fsyncs the directory. On failure the previous snapshot is untouched.
    static Result<void> Save(const std::filesystem::path& path, lsn_t lsn,
                             std::span<const uint8_t> db_bytes);

    /// NotFound if absent; ChecksumMismatch/CorruptedData if damaged. Never returns bad bytes.
    static Result<SnapshotData> Load(const std::filesystem::path& path);
};

} // namespace database

#endif // WHISKDB_STORAGE_SNAPSHOT_MANAGER_HPP
