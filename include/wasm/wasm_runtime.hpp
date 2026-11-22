#ifndef WHISKDB_WASM_RUNTIME_HPP
#define WHISKDB_WASM_RUNTIME_HPP

#include "common/types.hpp"
#include "errors.hpp"

#include <wasmtime.h>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace database {

// Zero-leak RAII custom deleters
struct WasmConfigDeleter {
    void operator()(wasm_config_t* ptr) const noexcept {
        if (ptr) {
            wasm_config_delete(ptr);
        }
    }
};
using WasmConfigPtr = std::unique_ptr<wasm_config_t, WasmConfigDeleter>;

struct WasmEngineDeleter {
    void operator()(wasm_engine_t* ptr) const noexcept {
        if (ptr) {
            wasm_engine_delete(ptr);
        }
    }
};
using WasmEnginePtr = std::unique_ptr<wasm_engine_t, WasmEngineDeleter>;

struct WasmtimeStoreDeleter {
    void operator()(wasmtime_store_t* ptr) const noexcept {
        if (ptr) {
            wasmtime_store_delete(ptr);
        }
    }
};
using WasmtimeStorePtr = std::unique_ptr<wasmtime_store_t, WasmtimeStoreDeleter>;

struct WasmtimeLinkerDeleter {
    void operator()(wasmtime_linker_t* ptr) const noexcept {
        if (ptr) {
            wasmtime_linker_delete(ptr);
        }
    }
};
using WasmtimeLinkerPtr = std::unique_ptr<wasmtime_linker_t, WasmtimeLinkerDeleter>;

struct WasmtimeModuleDeleter {
    void operator()(wasmtime_module_t* ptr) const noexcept {
        if (ptr) {
            wasmtime_module_delete(ptr);
        }
    }
};
using WasmtimeModulePtr = std::unique_ptr<wasmtime_module_t, WasmtimeModuleDeleter>;

struct WasmtimeErrorDeleter {
    void operator()(wasmtime_error_t* ptr) const noexcept {
        if (ptr) {
            wasmtime_error_delete(ptr);
        }
    }
};
using WasmtimeErrorPtr = std::unique_ptr<wasmtime_error_t, WasmtimeErrorDeleter>;

struct WasmTrapDeleter {
    void operator()(wasm_trap_t* ptr) const noexcept {
        if (ptr) {
            wasm_trap_delete(ptr);
        }
    }
};
using WasmTrapPtr = std::unique_ptr<wasm_trap_t, WasmTrapDeleter>;

/**
 * @brief Configuration settings for WasmRuntime engine and store isolation.
 */
struct WasmConfig {
    idx_t max_fuel = 100'000'000;
    idx_t max_memory_mb = 64;
};

/**
 * @brief Pre-compiled WASM module artifact with extracted function exports.
 */
struct CompiledModule {
    WasmtimeModulePtr module{nullptr};
    std::vector<std::string> exported_funcs;
};

/**
 * @brief Move-only handle managing a Wasmtime store instance and its fuel budget.
 */
class StoreHandle {
public:
    StoreHandle() noexcept = default;
    StoreHandle(WasmtimeStorePtr store, uint64_t initial_fuel) noexcept
        : store_(std::move(store)), initial_fuel_(initial_fuel) {}
    ~StoreHandle() = default;

    StoreHandle(const StoreHandle&) = delete;
    StoreHandle& operator=(const StoreHandle&) = delete;

    StoreHandle(StoreHandle&&) noexcept = default;
    StoreHandle& operator=(StoreHandle&&) noexcept = default;

public:
    wasmtime_store_t* Raw() const noexcept { return store_.get(); }
    wasmtime_context_t* Context() const noexcept {
        return store_ ? wasmtime_store_context(store_.get()) : nullptr;
    }
    uint64_t InitialFuel() const noexcept { return initial_fuel_; }
    uint64_t ConsumedFuel() const noexcept;

private:
    WasmtimeStorePtr store_{nullptr};
    uint64_t initial_fuel_{0};
};

/**
 * @brief Sandboxed WebAssembly runtime engine backed by Cranelift JIT in Wasmtime.
 */
class WasmRuntime {
public:
    explicit WasmRuntime(WasmConfig config, WasmEnginePtr engine, WasmtimeLinkerPtr linker) noexcept
        : config_(config), engine_(std::move(engine)), linker_(std::move(linker)) {}
    ~WasmRuntime() = default;

    WasmRuntime(const WasmRuntime&) = delete;
    WasmRuntime& operator=(const WasmRuntime&) = delete;

    WasmRuntime(WasmRuntime&&) noexcept = default;
    WasmRuntime& operator=(WasmRuntime&&) noexcept = default;

    static Result<std::unique_ptr<WasmRuntime>> Create(WasmConfig config = {});

public:
    Result<CompiledModule> CompileModule(std::span<const uint8_t> wasm_bytes) const;

    Result<StoreHandle> CreateStore(void* user_data = nullptr,
                                    void (*finalizer)(void*) = nullptr,
                                    std::optional<uint64_t> fuel_override = std::nullopt) const;

    static Error TranslateError(wasmtime_error_t* err);
    static Error TranslateTrap(wasm_trap_t* trap);

    uint64_t GetConsumedFuel(const wasmtime_context_t* ctx, uint64_t initial_fuel) const noexcept;

    wasm_engine_t* Engine() const noexcept { return engine_.get(); }
    wasmtime_linker_t* Linker() const noexcept { return linker_.get(); }
    const WasmConfig& Config() const noexcept { return config_; }

private:
    WasmConfig config_;
    WasmEnginePtr engine_{nullptr};
    WasmtimeLinkerPtr linker_{nullptr};
};

} // namespace database

namespace Database = database;

#endif // WHISKDB_WASM_RUNTIME_HPP
