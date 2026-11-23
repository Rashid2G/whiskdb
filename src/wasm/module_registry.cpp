#include "wasm/module_registry.hpp"

#include "common/sha256.hpp"
#include "sql/transaction_guard.hpp"
#include "wasm/host_abi.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>

namespace database {

namespace {

bool IsValidModuleName(std::string_view name) noexcept {
    if (name.empty() || name.size() > 128) {
        return false;
    }
    for (char c : name) {
        bool ok = (c >= 'A' && c <= 'Z') ||
                  (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') ||
                  (c == '_') || (c == '-');
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool IsInternalAbiFunction(std::string_view fn) noexcept {
    if (fn == "whisk_alloc" ||
        fn == "whisk_reset_heap" ||
        fn == "whisk_result_ptr" ||
        fn == "whisk_result_len") {
        return true;
    }
    if (fn.starts_with("__")) {
        return true;
    }
    return false;
}

} // namespace

ModuleRegistry::ModuleRegistry(const WasmRuntime& runtime) noexcept
    : runtime_(runtime) {}

Result<PreparedPublish> ModuleRegistry::PreparePublish(
    SqlEngine& sql, std::string_view name, std::span<const uint8_t> wasm_bytes) {
    // Step 1: Validate name and wasm_bytes
    if (!IsValidModuleName(name)) {
        return MAKE_ERROR(ErrorCode::InvalidArgument, "Invalid module name '{}'", name);
    }
    if (wasm_bytes.empty()) {
        return MAKE_ERROR(ErrorCode::InvalidArgument, "WASM binary cannot be empty");
    }

    // Step 2: Compile via runtime_.CompileModule
    auto compile_res = runtime_.CompileModule(wasm_bytes);
    if (!compile_res) {
        return std::unexpected(compile_res.error());
    }
    auto compiled_mod = std::move(compile_res.value());

    // Step 3: Validate required Guest ABI exports BEFORE touching DB
    // Module exports must include a WASM_EXTERN_MEMORY named "memory"
    // and function exports must include whisk_alloc, whisk_result_ptr, whisk_result_len.
    wasm_exporttype_vec_t exports{};
    wasmtime_module_exports(compiled_mod.module.get(), &exports);

    bool has_memory = false;
    bool has_whisk_alloc = false;
    bool has_whisk_result_ptr = false;
    bool has_whisk_result_len = false;
    std::vector<std::string> raw_func_exports;

    for (idx_t i = 0; i < exports.size; ++i) {
        const wasm_exporttype_t* exp = exports.data[i];
        const wasm_externtype_t* ext_type = wasm_exporttype_type(exp);
        wasm_externkind_t kind = wasm_externtype_kind(ext_type);
        const wasm_name_t* exp_name = wasm_exporttype_name(exp);
        if (!exp_name || !exp_name->data) {
            continue;
        }
        std::string_view sym_name(exp_name->data, exp_name->size);

        if (kind == WASM_EXTERN_MEMORY && sym_name == "memory") {
            has_memory = true;
        } else if (kind == WASM_EXTERN_FUNC) {
            raw_func_exports.emplace_back(sym_name);
            if (sym_name == "whisk_alloc") {
                has_whisk_alloc = true;
            } else if (sym_name == "whisk_result_ptr") {
                has_whisk_result_ptr = true;
            } else if (sym_name == "whisk_result_len") {
                has_whisk_result_len = true;
            }
        }
    }
    wasm_exporttype_vec_delete(&exports);

    if (!has_memory) {
        return MAKE_ERROR(ErrorCode::WasmError, "Missing required Guest ABI export: 'memory'");
    }
    if (!has_whisk_alloc) {
        return MAKE_ERROR(ErrorCode::WasmError, "Missing required Guest ABI export: 'whisk_alloc'");
    }
    if (!has_whisk_result_ptr) {
        return MAKE_ERROR(ErrorCode::WasmError, "Missing required Guest ABI export: 'whisk_result_ptr'");
    }
    if (!has_whisk_result_len) {
        return MAKE_ERROR(ErrorCode::WasmError, "Missing required Guest ABI export: 'whisk_result_len'");
    }

    // Step 4: Filter exported_funcs into user-callable functions
    std::vector<std::string> user_funcs;
    for (const auto& fn : raw_func_exports) {
        if (!IsInternalAbiFunction(fn)) {
            user_funcs.push_back(fn);
        }
    }
    std::sort(user_funcs.begin(), user_funcs.end());

    // Wrap raw module into SharedWasmModule
    wasmtime_module_t* raw_mod = compiled_mod.module.release();
    SharedWasmModule shared_mod(raw_mod, [](wasmtime_module_t* m) {
        if (m) {
            wasmtime_module_delete(m);
        }
    });

    // Step 5: Compute sha256 and updated_at
    std::string sha256 = Sha256Hex(wasm_bytes);
    int64_t updated_at = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Step 6: Check SystemCatalog::GetModule
    auto existing_res = SystemCatalog::GetModule(sql, name);
    if (!existing_res) {
        return std::unexpected(existing_res.error());
    }

    int64_t version = 1;
    std::string_view hook_name = "__init__";
    if (existing_res.value().has_value()) {
        version = existing_res.value()->version + 1;
        hook_name = "__update__";
    }

    // Step 7: Create WhiskCallContext
    WhiskCallContext ctx{&sql, std::string(name), {}, std::nullopt};

    // Check if hook_name exists in raw_func_exports
    bool hook_exists = false;
    for (const auto& fn : raw_func_exports) {
        if (fn == hook_name) {
            hook_exists = true;
            break;
        }
    }

    if (hook_exists) {
        auto store_res = runtime_.CreateStore(&ctx);
        if (!store_res) {
            return std::unexpected(store_res.error());
        }
        StoreHandle store = std::move(store_res.value());

        wasmtime_instance_t instance{};
        wasm_trap_t* trap = nullptr;
        wasmtime_error_t* err = wasmtime_linker_instantiate(
            runtime_.Linker(), store.Context(), shared_mod.get(), &instance, &trap);
        if (err) {
            return std::unexpected(WasmRuntime::TranslateError(err));
        }
        if (trap) {
            return std::unexpected(WasmRuntime::TranslateTrap(trap));
        }

        wasmtime_extern_t hook_ext{};
        bool found = wasmtime_instance_export_get(
            store.Context(), &instance, hook_name.data(), hook_name.size(), &hook_ext);
        if (!found || hook_ext.kind != WASMTIME_EXTERN_FUNC) {
            return MAKE_ERROR(ErrorCode::WasmError, "Exported hook '{}' is not a function", hook_name);
        }

        wasmtime_val_t res_vals[1];
        err = wasmtime_func_call(store.Context(), &hook_ext.of.func, nullptr, 0, res_vals, 1, &trap);
        if (err) {
            return std::unexpected(WasmRuntime::TranslateError(err));
        }
        if (trap) {
            return std::unexpected(WasmRuntime::TranslateTrap(trap));
        }

        // Return code must be i32 == 0
        int32_t rc = 0;
        if (res_vals[0].kind == WASMTIME_I32) {
            rc = res_vals[0].of.i32;
        }
        if (rc != 0) {
            std::string err_msg;
            wasmtime_extern_t mem_ext{};
            wasmtime_extern_t ptr_ext{};
            wasmtime_extern_t len_ext{};

            bool has_mem = wasmtime_instance_export_get(store.Context(), &instance, "memory", 6, &mem_ext);
            bool has_ptr = wasmtime_instance_export_get(store.Context(), &instance, "whisk_result_ptr", 16, &ptr_ext);
            bool has_len = wasmtime_instance_export_get(store.Context(), &instance, "whisk_result_len", 16, &len_ext);

            if (has_mem && mem_ext.kind == WASMTIME_EXTERN_MEMORY &&
                has_ptr && ptr_ext.kind == WASMTIME_EXTERN_FUNC &&
                has_len && len_ext.kind == WASMTIME_EXTERN_FUNC) {
                wasmtime_val_t val_ptr[1];
                wasmtime_val_t val_len[1];
                wasm_trap_t* sub_trap = nullptr;
                wasmtime_error_t* sub_err = wasmtime_func_call(store.Context(), &ptr_ext.of.func, nullptr, 0, val_ptr, 1, &sub_trap);
                if (!sub_err && !sub_trap && val_ptr[0].kind == WASMTIME_I32) {
                    sub_err = wasmtime_func_call(store.Context(), &len_ext.of.func, nullptr, 0, val_len, 1, &sub_trap);
                    if (!sub_err && !sub_trap && val_len[0].kind == WASMTIME_I32) {
                        int32_t ptr = val_ptr[0].of.i32;
                        int32_t len = val_len[0].of.i32;
                        size_t mem_sz = wasmtime_memory_data_size(store.Context(), &mem_ext.of.memory);
                        uint8_t* mem_bytes = wasmtime_memory_data(store.Context(), &mem_ext.of.memory);
                        if (ptr >= 0 && len > 0 && static_cast<uint64_t>(ptr) + static_cast<uint64_t>(len) <= mem_sz) {
                            err_msg.assign(reinterpret_cast<const char*>(mem_bytes + ptr), static_cast<size_t>(len));
                        }
                    }
                }
                if (sub_err) {
                    wasmtime_error_delete(sub_err);
                }
                if (sub_trap) {
                    wasm_trap_delete(sub_trap);
                }
            }

            return MAKE_ERROR(ErrorCode::WasmError, "Lifecycle hook '{}' failed (rc={}): {}", hook_name, rc, err_msg);
        }

        // Record migration
        auto mig_res = SystemCatalog::RecordMigration(sql, name, version, hook_name, &ctx.mutating_statements);
        if (!mig_res) {
            return std::unexpected(mig_res.error());
        }
    }

    // Step 8: Persist module via SystemCatalog::UpsertModule
    ModuleMetadata meta{
        .name = std::string(name),
        .version = version,
        .sha256 = sha256,
        .wasm_bytes = std::vector<uint8_t>(wasm_bytes.begin(), wasm_bytes.end()),
        .exported_funcs = user_funcs,
        .updated_at = updated_at
    };

    auto upsert_res = SystemCatalog::UpsertModule(sql, meta, &ctx.mutating_statements);
    if (!upsert_res) {
        return std::unexpected(upsert_res.error());
    }

    // Step 9: Return PreparedPublish
    CachedModule cached{
        .name = std::string(name),
        .version = version,
        .sha256 = std::move(sha256),
        .exported_funcs = std::move(user_funcs),
        .updated_at = updated_at,
        .compiled = std::move(shared_mod)
    };

    return PreparedPublish{
        .module = std::move(cached),
        .mutating_statements = std::move(ctx.mutating_statements)
    };
}

void ModuleRegistry::CommitCachedModule(CachedModule module) {
    std::lock_guard<std::mutex> lock(mutex_);
    modules_[module.name] = std::move(module);
}

Result<PreparedPublish> ModuleRegistry::Publish(
    SqlEngine& sql, std::string_view name, std::span<const uint8_t> wasm_bytes) {
    auto tg_res = TransactionGuard::Begin(sql);
    if (!tg_res) {
        return std::unexpected(tg_res.error());
    }
    TransactionGuard guard = std::move(tg_res.value());

    auto prep_res = PreparePublish(sql, name, wasm_bytes);
    if (!prep_res) {
        // Rollback is triggered automatically by guard destructor
        return std::unexpected(prep_res.error());
    }

    auto commit_res = guard.Commit();
    if (!commit_res) {
        return std::unexpected(commit_res.error());
    }

    CommitCachedModule(prep_res.value().module);
    return prep_res;
}

Result<idx_t> ModuleRegistry::LoadFromCatalog(SqlEngine& sql) {
    auto list_res = SystemCatalog::ListModules(sql);
    if (!list_res) {
        return std::unexpected(list_res.error());
    }

    idx_t count = 0;
    std::unordered_map<std::string, CachedModule> loaded;
    loaded.reserve(list_res.value().size());

    for (const auto& meta : list_res.value()) {
        auto compile_res = runtime_.CompileModule(meta.wasm_bytes);
        if (!compile_res) {
            return std::unexpected(compile_res.error());
        }

        wasmtime_module_t* raw = compile_res.value().module.release();
        SharedWasmModule shared(raw, [](wasmtime_module_t* m) {
            if (m) {
                wasmtime_module_delete(m);
            }
        });

        CachedModule mod{
            .name = meta.name,
            .version = meta.version,
            .sha256 = meta.sha256,
            .exported_funcs = meta.exported_funcs,
            .updated_at = meta.updated_at,
            .compiled = std::move(shared)
        };
        loaded[meta.name] = std::move(mod);
        ++count;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [name, mod] : loaded) {
            modules_[name] = std::move(mod);
        }
    }

    return count;
}

std::optional<CachedModule> ModuleRegistry::Get(std::string_view name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = modules_.find(std::string(name));
    if (it == modules_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<CachedModule> ModuleRegistry::List() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CachedModule> result;
    result.reserve(modules_.size());
    for (const auto& [_, mod] : modules_) {
        result.push_back(mod);
    }
    std::sort(result.begin(), result.end(), [](const CachedModule& a, const CachedModule& b) {
        return a.name < b.name;
    });
    return result;
}

bool ModuleRegistry::Remove(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = modules_.find(std::string(name));
    if (it != modules_.end()) {
        modules_.erase(it);
        return true;
    }
    return false;
}

idx_t ModuleRegistry::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return modules_.size();
}

} // namespace database
