#ifndef WHISKDB_SQL_VALUE_CODEC_HPP
#define WHISKDB_SQL_VALUE_CODEC_HPP

#include "common/types.hpp"
#include "errors.hpp"
#include "sql/sql_types.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace database {

/**
 * @brief Encodes a single Value into out buffer (1B ValueType tag + payload).
 */
void EncodeValue(std::vector<uint8_t>& out, const Value& v);

/**
 * @brief Decodes a Value from the span and advances `in`.
 * Bounds-checked and hostile-input safe; returns ProtocolError on truncation/bad tag.
 */
Result<Value> DecodeValue(std::span<const uint8_t>& in);

/**
 * @brief Encodes a SQL statement with parameters (also used as WAL payload).
 */
void EncodeStatement(std::vector<uint8_t>& out, std::string_view sql, std::span<const Value> params);

struct DecodedStatement {
    std::string sql;
    std::vector<Value> params;
};

/**
 * @brief Decodes a statement; bounds-checked and rejects trailing garbage.
 */
Result<DecodedStatement> DecodeStatement(std::span<const uint8_t> in);

/**
 * @brief Encodes a QueryResult into out buffer.
 */
void EncodeQueryResult(std::vector<uint8_t>& out, const QueryResult& r);

/**
 * @brief Decodes a QueryResult; bounds-checked and rejects trailing garbage.
 */
Result<QueryResult> DecodeQueryResult(std::span<const uint8_t> in);

} // namespace database

#endif // WHISKDB_SQL_VALUE_CODEC_HPP
