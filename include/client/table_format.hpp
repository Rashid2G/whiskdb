#ifndef WHISKDB_CLIENT_TABLE_FORMAT_HPP
#define WHISKDB_CLIENT_TABLE_FORMAT_HPP

#include "common/types.hpp"
#include "sql/sql_types.hpp"

#include <string>
#include <vector>

namespace database {

/// Formats a QueryResult as an ASCII box-drawing table with header row, cell content,
/// and timing footer, or as a DML status summary when no columns are present.
std::string FormatResultTable(const QueryResult& qr);

/// Splits buffered input into complete ';'-terminated statements, respecting single-quoted
/// strings with '' escapes and ignoring ';' inside them. Complete statements are returned,
/// leaving the incomplete remainder in buffer.
std::vector<std::string> SplitStatements(std::string& buffer);

} // namespace database

#endif // WHISKDB_CLIENT_TABLE_FORMAT_HPP
