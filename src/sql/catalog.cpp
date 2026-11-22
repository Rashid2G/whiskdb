#include "sql/catalog.hpp"
#include "common/sha256.hpp"

#include <cctype>
#include <chrono>

namespace database {

namespace {

std::string SerializeStringArray(const std::vector<std::string>& list) {
    std::string out = "[";
    for (size_t i = 0; i < list.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        out += "\"";
        for (char c : list[i]) {
            if (c == '"') {
                out += "\\\"";
            } else if (c == '\\') {
                out += "\\\\";
            } else if (c == '\b') {
                out += "\\b";
            } else if (c == '\f') {
                out += "\\f";
            } else if (c == '\n') {
                out += "\\n";
            } else if (c == '\r') {
                out += "\\r";
            } else if (c == '\t') {
                out += "\\t";
            } else {
                out += c;
            }
        }
        out += "\"";
    }
    out += "]";
    return out;
}

std::vector<std::string> DeserializeStringArray(std::string_view json) {
    std::vector<std::string> result;
    size_t pos = 0;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        pos++;
    }
    if (pos >= json.size() || json[pos] != '[') {
        return result;
    }
    pos++; // skip '['

    while (pos < json.size()) {
        while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
            pos++;
        }
        if (pos >= json.size() || json[pos] == ']') {
            break;
        }
        if (json[pos] == ',') {
            pos++;
            continue;
        }
        if (json[pos] == '"') {
            pos++; // skip opening quote
            std::string item;
            while (pos < json.size() && json[pos] != '"') {
                if (json[pos] == '\\' && pos + 1 < json.size()) {
                    pos++;
                    char esc = json[pos];
                    switch (esc) {
                        case '"': item += '"'; break;
                        case '\\': item += '\\'; break;
                        case '/': item += '/'; break;
                        case 'b': item += '\b'; break;
                        case 'f': item += '\f'; break;
                        case 'n': item += '\n'; break;
                        case 'r': item += '\r'; break;
                        case 't': item += '\t'; break;
                        default: item += esc; break;
                    }
                } else {
                    item += json[pos];
                }
                pos++;
            }
            if (pos < json.size() && json[pos] == '"') {
                pos++; // skip closing quote
            }
            result.push_back(std::move(item));
        } else {
            pos++;
        }
    }
    return result;
}

constexpr std::string_view kCreateModulesTableSql =
    "CREATE TABLE IF NOT EXISTS _whisk_modules ("
    "name TEXT PRIMARY KEY, "
    "version INTEGER NOT NULL, "
    "sha256 TEXT NOT NULL, "
    "wasm_bytes BLOB NOT NULL, "
    "exported_funcs TEXT NOT NULL, "
    "updated_at INTEGER NOT NULL"
    ");";

constexpr std::string_view kCreateMigrationsTableSql =
    "CREATE TABLE IF NOT EXISTS _whisk_migrations ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT, "
    "module_name TEXT NOT NULL, "
    "version INTEGER NOT NULL, "
    "hook_name TEXT NOT NULL, "
    "executed_at INTEGER NOT NULL"
    ");";

constexpr std::string_view kUpsertModuleSql =
    "INSERT INTO _whisk_modules (name, version, sha256, wasm_bytes, exported_funcs, updated_at) "
    "VALUES (?1, ?2, ?3, ?4, ?5, ?6) "
    "ON CONFLICT(name) DO UPDATE SET "
    "version = excluded.version, "
    "sha256 = excluded.sha256, "
    "wasm_bytes = excluded.wasm_bytes, "
    "exported_funcs = excluded.exported_funcs, "
    "updated_at = excluded.updated_at";

constexpr std::string_view kInsertMigrationSql =
    "INSERT INTO _whisk_migrations (module_name, version, hook_name, executed_at) "
    "VALUES (?1, ?2, ?3, ?4);";

constexpr std::string_view kDeleteModuleSql =
    "DELETE FROM _whisk_modules WHERE name = ?1;";

} // namespace

Result<void> SystemCatalog::Initialize(SqlEngine& sql) {
    auto guard = sql.AcquireCatalogWriteGuard();

    auto r1 = sql.Execute(kCreateModulesTableSql);
    if (!r1) {
        return std::unexpected(r1.error());
    }

    auto r2 = sql.Execute(kCreateMigrationsTableSql);
    if (!r2) {
        return std::unexpected(r2.error());
    }

    return {};
}

Result<int64_t> SystemCatalog::UpsertModule(SqlEngine& sql, const ModuleMetadata& mod,
                                            std::vector<DecodedStatement>* wal_log) {
    if (mod.name.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "Module name cannot be empty");
    }

    auto guard = sql.AcquireCatalogWriteGuard();

    // Look up existing version
    int64_t version = 1;
    auto sel = sql.Execute("SELECT version FROM _whisk_modules WHERE name = ?1;",
                           std::vector<Value>{Value(mod.name)});
    if (!sel) {
        return std::unexpected(sel.error());
    }
    if (!sel->rows.empty()) {
        version = std::get<int64_t>(sel->rows[0][0]) + 1;
    }

    std::string sha = mod.sha256;
    if (sha.empty()) {
        sha = Sha256Hex(mod.wasm_bytes);
    }

    int64_t updated_at = mod.updated_at;
    if (updated_at <= 0) {
        updated_at = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::string exported_funcs_json = SerializeStringArray(mod.exported_funcs);

    std::vector<Value> params;
    params.reserve(6);
    params.emplace_back(mod.name);
    params.emplace_back(version);
    params.emplace_back(std::move(sha));
    params.emplace_back(mod.wasm_bytes);
    params.emplace_back(std::move(exported_funcs_json));
    params.emplace_back(updated_at);

    auto exec = sql.Execute(kUpsertModuleSql, params);
    if (!exec) {
        return std::unexpected(exec.error());
    }

    if (wal_log != nullptr) {
        wal_log->push_back(DecodedStatement{
            .sql = std::string(kUpsertModuleSql),
            .params = std::move(params)
        });
    }

    return version;
}

Result<std::optional<ModuleMetadata>> SystemCatalog::GetModule(SqlEngine& sql, std::string_view name) {
    auto sel = sql.Execute("SELECT name, version, sha256, wasm_bytes, exported_funcs, updated_at "
                           "FROM _whisk_modules WHERE name = ?1;",
                           std::vector<Value>{Value(std::string(name))});
    if (!sel) {
        return std::unexpected(sel.error());
    }
    if (sel->rows.empty()) {
        return std::nullopt;
    }

    const auto& row = sel->rows[0];
    ModuleMetadata meta;
    meta.name = std::get<std::string>(row[0]);
    meta.version = std::get<int64_t>(row[1]);
    meta.sha256 = std::get<std::string>(row[2]);
    meta.wasm_bytes = std::get<Blob>(row[3]);
    meta.exported_funcs = DeserializeStringArray(std::get<std::string>(row[4]));
    meta.updated_at = std::get<int64_t>(row[5]);

    return meta;
}

Result<std::vector<ModuleMetadata>> SystemCatalog::ListModules(SqlEngine& sql) {
    auto sel = sql.Execute("SELECT name, version, sha256, wasm_bytes, exported_funcs, updated_at "
                           "FROM _whisk_modules ORDER BY name ASC;");
    if (!sel) {
        return std::unexpected(sel.error());
    }

    std::vector<ModuleMetadata> modules;
    modules.reserve(sel->rows.size());
    for (const auto& row : sel->rows) {
        ModuleMetadata meta;
        meta.name = std::get<std::string>(row[0]);
        meta.version = std::get<int64_t>(row[1]);
        meta.sha256 = std::get<std::string>(row[2]);
        meta.wasm_bytes = std::get<Blob>(row[3]);
        meta.exported_funcs = DeserializeStringArray(std::get<std::string>(row[4]));
        meta.updated_at = std::get<int64_t>(row[5]);
        modules.push_back(std::move(meta));
    }

    return modules;
}

Result<void> SystemCatalog::RecordMigration(SqlEngine& sql, std::string_view module_name,
                                            int64_t version, std::string_view hook_name,
                                            std::vector<DecodedStatement>* wal_log) {
    if (module_name.empty() || hook_name.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "module_name and hook_name cannot be empty");
    }

    auto guard = sql.AcquireCatalogWriteGuard();

    int64_t executed_at = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::vector<Value> params;
    params.reserve(4);
    params.emplace_back(std::string(module_name));
    params.emplace_back(version);
    params.emplace_back(std::string(hook_name));
    params.emplace_back(executed_at);

    auto exec = sql.Execute(kInsertMigrationSql, params);
    if (!exec) {
        return std::unexpected(exec.error());
    }

    if (wal_log != nullptr) {
        wal_log->push_back(DecodedStatement{
            .sql = std::string(kInsertMigrationSql),
            .params = std::move(params)
        });
    }

    return {};
}

Result<void> SystemCatalog::RemoveModule(SqlEngine& sql, std::string_view name,
                                         std::vector<DecodedStatement>* wal_log) {
    if (name.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "Module name cannot be empty");
    }

    auto guard = sql.AcquireCatalogWriteGuard();

    std::vector<Value> params;
    params.emplace_back(std::string(name));

    auto exec = sql.Execute(kDeleteModuleSql, params);
    if (!exec) {
        return std::unexpected(exec.error());
    }

    if (wal_log != nullptr) {
        wal_log->push_back(DecodedStatement{
            .sql = std::string(kDeleteModuleSql),
            .params = std::move(params)
        });
    }

    return {};
}

} // namespace database
