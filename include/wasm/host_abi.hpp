#ifndef WHISKDB_WASM_HOST_ABI_HPP
#define WHISKDB_WASM_HOST_ABI_HPP

#include "errors.hpp"
#include "sql/sql_engine.hpp"
#include "sql/sql_types.hpp"
#include "sql/value_codec.hpp"

#include <wasmtime.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace database {

/**
 * @brief Per-invocation context stored in wasmtime_store_t data.
 */
struct WhiskCallContext {
    SqlEngine* sql = nullptr;
    std::string module_name;
    std::vector<DecodedStatement> mutating_statements;
    std::optional<Error> last_error;
};

/// Registers `"whisk"` host imports (`whisk_db_exec`, `whisk_db_query`, `whisk_log`) on the linker.
Result<void> RegisterHostAbi(wasmtime_linker_t* linker);

/// Parses a JSON array of SQL parameters into typed Values (empty or "[]" -> empty vector).
Result<std::vector<Value>> ParseHostAbiParamsJson(std::string_view params_json);

/// Serializes a QueryResult into compact JSON: {"columns":[...],"rows":[[...],...]}
std::string SerializeQueryResultToJson(const QueryResult& result);

} // namespace database

namespace Database = database;

#endif // WHISKDB_WASM_HOST_ABI_HPP
