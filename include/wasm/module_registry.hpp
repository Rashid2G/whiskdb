#ifndef WHISKDB_WASM_MODULE_REGISTRY_HPP
#define WHISKDB_WASM_MODULE_REGISTRY_HPP

#include "common/types.hpp"
#include "errors.hpp"
#include "sql/catalog.hpp"
#include "sql/sql_engine.hpp"
#include "wasm/wasm_runtime.hpp"

#include <wasmtime.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace database {

using SharedWasmModule = std::shared_ptr<wasmtime_module_t>;

struct CachedModule {
    std::string name;
    int64_t version = 0;
    std::string sha256;
    std::vector<std::string> exported_funcs; // user-callable exports (excludes internal ABI & __* symbols; includes test_*)
    int64_t updated_at = 0;                  // unix epoch microseconds
    SharedWasmModule compiled;               // native Cranelift-compiled module (custom deleter wasmtime_module_delete)
};

struct PreparedPublish {
    CachedModule module;
    std::vector<DecodedStatement> mutating_statements; // hook mutations + _whisk_migrations + _whisk_modules upsert
};

/**
 * @brief Thread-safe in-memory cache of pre-compiled WASM modules with lifecycle execution.
 */
class ModuleRegistry {
public:
    explicit ModuleRegistry(const WasmRuntime& runtime) noexcept;
    ~ModuleRegistry() = default;

    ModuleRegistry(const ModuleRegistry&) = delete;
    ModuleRegistry& operator=(const ModuleRegistry&) = delete;

    ModuleRegistry(ModuleRegistry&&) = delete;
    ModuleRegistry& operator=(ModuleRegistry&&) = delete;

public:
    Result<PreparedPublish> PreparePublish(SqlEngine& sql, std::string_view name, std::span<const uint8_t> wasm_bytes);

    void CommitCachedModule(CachedModule module);

    Result<PreparedPublish> Publish(SqlEngine& sql, std::string_view name, std::span<const uint8_t> wasm_bytes);

    Result<idx_t> LoadFromCatalog(SqlEngine& sql);

    std::optional<CachedModule> Get(std::string_view name) const;

    std::vector<CachedModule> List() const;

    bool Remove(std::string_view name);

    idx_t Size() const;

private:
    const WasmRuntime& runtime_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, CachedModule> modules_;
};

} // namespace database

namespace Database = database;

#endif // WHISKDB_WASM_MODULE_REGISTRY_HPP
