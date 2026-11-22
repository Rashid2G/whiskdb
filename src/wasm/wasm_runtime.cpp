#include "wasm/wasm_runtime.hpp"

#include <cstring>

namespace database {

uint64_t StoreHandle::ConsumedFuel() const noexcept {
    if (!store_) {
        return 0;
    }
    wasmtime_context_t* ctx = Context();
    if (!ctx) {
        return 0;
    }
    uint64_t remaining = 0;
    wasmtime_error_t* err = wasmtime_context_get_fuel(ctx, &remaining);
    if (err) {
        wasmtime_error_delete(err);
        return 0;
    }
    return initial_fuel_ >= remaining ? (initial_fuel_ - remaining) : 0;
}

Result<std::unique_ptr<WasmRuntime>> WasmRuntime::Create(WasmConfig config) {
    WasmConfigPtr wasm_cfg(wasm_config_new());
    if (!wasm_cfg) {
        return MAKE_ERROR(ErrorCode::OutOfMemory, "Failed to allocate wasm_config_t");
    }
    wasmtime_config_strategy_set(wasm_cfg.get(), WASMTIME_STRATEGY_CRANELIFT);
    wasmtime_config_consume_fuel_set(wasm_cfg.get(), true);

    // wasm_engine_new_with_config takes ownership of wasm_config_t*
    wasm_engine_t* engine = wasm_engine_new_with_config(wasm_cfg.release());
    if (!engine) {
        return MAKE_ERROR(ErrorCode::Internal, "Failed to create wasm_engine_t");
    }
    WasmEnginePtr engine_ptr(engine);

    wasmtime_linker_t* linker = wasmtime_linker_new(engine_ptr.get());
    if (!linker) {
        return MAKE_ERROR(ErrorCode::Internal, "Failed to create wasmtime_linker_t");
    }
    WasmtimeLinkerPtr linker_ptr(linker);

    return std::make_unique<WasmRuntime>(config, std::move(engine_ptr), std::move(linker_ptr));
}

Result<CompiledModule> WasmRuntime::CompileModule(std::span<const uint8_t> wasm_bytes) const {
    // Engine must be initialized
    D_ASSERT(engine_ != nullptr);

    // Check non-empty and at least 4 bytes for WASM binary magic header
    if (wasm_bytes.size() < 4) {
        return MAKE_ERROR(ErrorCode::WasmError, "WASM binary is too small or empty");
    }
    constexpr uint8_t WASM_MAGIC[4] = {0x00, 0x61, 0x73, 0x6d};
    if (wasm_bytes[0] != WASM_MAGIC[0] || wasm_bytes[1] != WASM_MAGIC[1] ||
        wasm_bytes[2] != WASM_MAGIC[2] || wasm_bytes[3] != WASM_MAGIC[3]) {
        return MAKE_ERROR(ErrorCode::WasmError, "Invalid WASM magic header");
    }

    wasmtime_module_t* raw_module = nullptr;
    wasmtime_error_t* err = wasmtime_module_new(
        engine_.get(), wasm_bytes.data(), wasm_bytes.size(), &raw_module);
    if (err) {
        return std::unexpected(TranslateError(err));
    }
    WasmtimeModulePtr module(raw_module);

    wasm_exporttype_vec_t exports{};
    wasmtime_module_exports(module.get(), &exports);

    std::vector<std::string> func_names;
    for (idx_t i = 0; i < exports.size; ++i) {
        const wasm_exporttype_t* exp = exports.data[i];
        const wasm_externtype_t* ext_type = wasm_exporttype_type(exp);
        if (wasm_externtype_kind(ext_type) == WASM_EXTERN_FUNC) {
            const wasm_name_t* name = wasm_exporttype_name(exp);
            if (name && name->data) {
                func_names.emplace_back(name->data, name->size);
            }
        }
    }
    wasm_exporttype_vec_delete(&exports);

    return CompiledModule{
        .module = std::move(module),
        .exported_funcs = std::move(func_names)
    };
}

Result<StoreHandle> WasmRuntime::CreateStore(void* user_data,
                                             void (*finalizer)(void*),
                                             std::optional<uint64_t> fuel_override) const {
    // Engine must be initialized
    D_ASSERT(engine_ != nullptr);

    wasmtime_store_t* raw_store = wasmtime_store_new(engine_.get(), user_data, finalizer);
    if (!raw_store) {
        return MAKE_ERROR(ErrorCode::OutOfMemory, "Failed to create wasmtime_store_t");
    }
    WasmtimeStorePtr store(raw_store);

    int64_t max_memory_bytes = static_cast<int64_t>(config_.max_memory_mb) * 1024 * 1024;
    wasmtime_store_limiter(store.get(), max_memory_bytes, -1, -1, -1, -1);

    uint64_t fuel = fuel_override.value_or(config_.max_fuel);
    wasmtime_context_t* ctx = wasmtime_store_context(store.get());
    wasmtime_error_t* fuel_err = wasmtime_context_set_fuel(ctx, fuel);
    if (fuel_err) {
        return std::unexpected(TranslateError(fuel_err));
    }

    return StoreHandle(std::move(store), fuel);
}

Error WasmRuntime::TranslateError(wasmtime_error_t* err) {
    if (!err) {
        return Error(ErrorCode::Ok);
    }
    wasm_name_t msg{};
    wasmtime_error_message(err, &msg);
    std::string message;
    if (msg.data && msg.size > 0) {
        size_t len = msg.size;
        if (msg.data[len - 1] == '\0') {
            --len;
        }
        message = std::string(msg.data, len);
    }
    wasm_name_delete(&msg);
    wasmtime_error_delete(err);
    return Error(ErrorCode::WasmError, std::move(message));
}

Error WasmRuntime::TranslateTrap(wasm_trap_t* trap) {
    if (!trap) {
        return Error(ErrorCode::Ok);
    }
    wasmtime_trap_code_t code = 0;
    bool has_code = wasmtime_trap_code(trap, &code);

    wasm_message_t msg{};
    wasm_trap_message(trap, &msg);
    std::string message;
    if (msg.data && msg.size > 0) {
        size_t len = msg.size;
        if (msg.data[len - 1] == '\0') {
            --len;
        }
        message = std::string(msg.data, len);
    }
    wasm_name_delete(&msg);
    wasm_trap_delete(trap);

    ErrorCode err_code = ErrorCode::WasmTrap;
    if (has_code) {
        switch (code) {
            case WASMTIME_TRAP_CODE_OUT_OF_FUEL:
                err_code = ErrorCode::WasmFuelExhausted;
                break;
            case WASMTIME_TRAP_CODE_MEMORY_OUT_OF_BOUNDS:
                err_code = ErrorCode::WasmMemoryOutOfBounds;
                break;
            default:
                err_code = ErrorCode::WasmTrap;
                break;
        }
    }
    return Error(err_code, std::move(message));
}

uint64_t WasmRuntime::GetConsumedFuel(const wasmtime_context_t* ctx, uint64_t initial_fuel) const noexcept {
    if (!ctx) {
        return 0;
    }
    uint64_t remaining = 0;
    wasmtime_error_t* err = wasmtime_context_get_fuel(ctx, &remaining);
    if (err) {
        wasmtime_error_delete(err);
        return 0;
    }
    return initial_fuel >= remaining ? (initial_fuel - remaining) : 0;
}

} // namespace database
