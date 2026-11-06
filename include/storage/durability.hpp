#ifndef WHISKDB_STORAGE_DURABILITY_HPP
#define WHISKDB_STORAGE_DURABILITY_HPP

#include "errors.hpp"

#include <cstdint>
#include <string_view>

namespace database {

/**
 * @brief Durability / latency trade-off for the write-ahead log.
 *
 * Strict       (sync):      fdatasync before every append returns.
 * GroupCommit  (batched):   appends are batched and share one fdatasync (default).
 * Relaxed      (async):     write() to the OS page cache; periodic background fdatasync.
 * MemoryOnly   (ephemeral): no WAL file at all.
 */
enum class DurabilityMode : uint8_t { Strict, GroupCommit, Relaxed, MemoryOnly };

/// Parses strict|group_commit|relaxed|memory_only (case-insensitive) plus the
/// aliases sync|batched|async|ephemeral.
Result<DurabilityMode> ParseDurabilityMode(std::string_view s);

/// Canonical (non-alias) lowercase name.
std::string_view ToString(DurabilityMode m);

/// Type tag of a WAL record, serialized as one byte on disk.
enum class WalRecordType : uint8_t { SqlStatement = 1, Checkpoint = 2 };

} // namespace database

#endif // WHISKDB_STORAGE_DURABILITY_HPP
