#ifndef WHISKDB_STORAGE_WAL_READER_HPP
#define WHISKDB_STORAGE_WAL_READER_HPP

#include "common/types.hpp"
#include "errors.hpp"
#include "storage/durability.hpp"

#include <filesystem>
#include <functional>
#include <span>

namespace database {

struct WalReplayStats {
    idx_t records_seen = 0;     // valid records found in the file
    idx_t records_applied = 0;  // records passed to the callback (lsn > after_lsn)
    lsn_t last_lsn = 0;         // LSN of the last valid record (0 if none)
    bool truncated_tail = false;
    idx_t truncated_bytes = 0;
};

using WalRecordCallback =
    std::function<Result<void>(lsn_t, WalRecordType, std::span<const uint8_t>)>;

/**
 * @brief Replays a WAL file.
 *
 * Calls `cb` for each valid record with lsn > after_lsn in file order. A torn or
 * CRC-bad record that is the last thing in the file is a crash artifact: the file
 * is truncated at the last good record (ftruncate + fsync) and truncated_tail is
 * set. A bad record followed by further valid records, a bad header, an unknown
 * record type or a non-monotonic LSN is real corruption and yields
 * ChecksumMismatch / CorruptedData. A missing file is ok with zero stats. Errors
 * returned by the callback abort replay and are propagated. The payload span is
 * only valid during the callback.
 */
Result<WalReplayStats> ReplayWal(const std::filesystem::path& path, lsn_t after_lsn,
                                 const WalRecordCallback& cb);

} // namespace database

#endif // WHISKDB_STORAGE_WAL_READER_HPP
