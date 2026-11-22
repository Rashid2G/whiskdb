#ifndef WHISKDB_SQL_CATALOG_HPP
#define WHISKDB_SQL_CATALOG_HPP

#include "common/types.hpp"
#include "errors.hpp"
#include "sql/sql_engine.hpp"
#include "sql/sql_types.hpp"
#include "sql/value_codec.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace database {

struct ModuleMetadata {
    std::string name;
    int64_t version = 0;
    std::string sha256;
    std::vector<uint8_t> wasm_bytes;
    std::vector<std::string> exported_funcs;
    int64_t updated_at = 0; // unix epoch microseconds
};

class SystemCatalog {
public:
    static Result<void> Initialize(SqlEngine& sql);

    static Result<int64_t> UpsertModule(SqlEngine& sql, const ModuleMetadata& mod,
                                        std::vector<DecodedStatement>* wal_log = nullptr);

    static Result<std::optional<ModuleMetadata>> GetModule(SqlEngine& sql, std::string_view name);

    static Result<std::vector<ModuleMetadata>> ListModules(SqlEngine& sql);

    static Result<void> RecordMigration(SqlEngine& sql, std::string_view module_name,
                                        int64_t version, std::string_view hook_name,
                                        std::vector<DecodedStatement>* wal_log = nullptr);

    static Result<void> RemoveModule(SqlEngine& sql, std::string_view name,
                                     std::vector<DecodedStatement>* wal_log = nullptr);
};

} // namespace database

#endif // WHISKDB_SQL_CATALOG_HPP
