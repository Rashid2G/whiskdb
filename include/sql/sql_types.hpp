#ifndef WHISKDB_SQL_SQL_TYPES_HPP
#define WHISKDB_SQL_SQL_TYPES_HPP

#include "common/types.hpp"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace database {

/// Raw binary value (SQLite BLOB)
using Blob = std::vector<uint8_t>;

/// A single typed SQL cell. Index order is the wire/type order:
/// 0 = NULL, 1 = INT64, 2 = DOUBLE, 3 = TEXT, 4 = BLOB
using Value = std::variant<std::monostate, int64_t, double, std::string, Blob>;

/// Stable numeric type tags used on the wire (matches Value::index())
enum class ValueType : uint8_t { Null = 0, Int64 = 1, Double = 2, Text = 3, Blob = 4 };

/// Structured result of executing one SQL statement
struct QueryResult {
    std::vector<std::string> columns;       ///< Column names (empty for non-SELECT)
    std::vector<std::vector<Value>> rows;   ///< Row-major cells
    idx_t rows_affected = 0;                ///< sqlite3_changes() for DML, 0 otherwise
    int64_t last_insert_rowid = 0;          ///< sqlite3_last_insert_rowid() after DML
    uint64_t exec_us = 0;                   ///< Server-side execution time in microseconds
    bool is_mutation = false;               ///< True if the statement modified the database (must be logged to the WAL)
};

} // namespace database

namespace Database = database;

#endif // WHISKDB_SQL_SQL_TYPES_HPP
