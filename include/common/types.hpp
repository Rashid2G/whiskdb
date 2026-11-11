#ifndef WHISKDB_COMMON_TYPES_HPP
#define WHISKDB_COMMON_TYPES_HPP

#include <cstddef>
#include <cstdint>

namespace database {

/// DuckDB-style standard index, count, and offset type used across WhiskDB
using idx_t = uint64_t;

/// Log sequence number type for WAL and snapshots
using lsn_t = uint64_t;

/// Fixed-width integer types
using int8_t = std::int8_t;
using int16_t = std::int16_t;
using int32_t = std::int32_t;
using int64_t = std::int64_t;
using uint8_t = std::uint8_t;
using uint16_t = std::uint16_t;
using uint32_t = std::uint32_t;
using uint64_t = std::uint64_t;

/// Pointer and buffer types
using data_t = uint8_t;
using data_ptr_t = uint8_t*;
using const_data_ptr_t = const uint8_t*;

} // namespace database

// Alias for backward compatibility with existing PascalCase namespace references
namespace Database = database;

#endif // WHISKDB_COMMON_TYPES_HPP
