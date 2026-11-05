#ifndef WHISKDB_COMMON_TYPES_HPP
#define WHISKDB_COMMON_TYPES_HPP

#include <cstddef>
#include <cstdint>

namespace database {

/// DuckDB-style standard index, count, and offset type used across WhiskDB
using idx_t = uint64_t;

/// Log sequence number type for WAL and snapshots
using lsn_t = uint64_t;

} // namespace database

// Alias for backward compatibility with existing PascalCase namespace references
namespace Database = database;

#endif // WHISKDB_COMMON_TYPES_HPP
