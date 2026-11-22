#include "common/types.hpp"
#include "errors.hpp"
#include "wasm/wasm_runtime.hpp"

#include <wasmtime/wat.h>

#include <cassert>
#include <cstring>
#include <print>
#include <vector>

using namespace database;

namespace {

std::vector<uint8_t> WatToWasm(const char* wat) {
    wasm_byte_vec_t wasm{};
    wasmtime_error_t* err = wasmtime_wat2wasm(wat, std::strlen(wat), &wasm);
    assert(err == nullptr);
    std::vector<uint8_t> bytes(
        reinterpret_cast<const uint8_t*>(wasm.data),
        reinterpret_cast<const uint8_t*>(wasm.data + wasm.size));
    wasm_byte_vec_delete(&wasm);
    return bytes;
}

void TestValidModuleExecution() {
    auto runtime_res = WasmRuntime::Create();
    assert(runtime_res.has_value());
    auto runtime = std::move(runtime_res.value());
    assert(runtime->Engine() != nullptr);
    assert(runtime->Linker() != nullptr);
    assert(runtime->Config().max_fuel == 100'000'000);
    assert(runtime->Config().max_memory_mb == 64);

    constexpr const char* wat = R"(
        (module
            (memory (export "memory") 1)
            (global (export "g") i32 (i32.const 42))
            (func (export "add") (param i32 i32) (result i32)
                (i32.add (local.get 0) (local.get 1))
            )
        )
    )";

    auto wasm_bytes = WatToWasm(wat);
    auto compile_res = runtime->CompileModule(wasm_bytes);
    assert(compile_res.has_value());

    auto& compiled = compile_res.value();
    assert(compiled.module != nullptr);
    // Export enumeration must only include functions
    assert(compiled.exported_funcs.size() == 1);
    assert(compiled.exported_funcs[0] == "add");

    auto store_res = runtime->CreateStore();
    assert(store_res.has_value());
    auto store = std::move(store_res.value());
    assert(store.Raw() != nullptr);
    assert(store.Context() != nullptr);
    assert(store.InitialFuel() == 100'000'000);
    assert(store.ConsumedFuel() == 0);

    wasmtime_instance_t instance{};
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err = wasmtime_linker_instantiate(
        runtime->Linker(), store.Context(), compiled.module.get(), &instance, &trap);
    assert(err == nullptr);
    assert(trap == nullptr);

    wasmtime_extern_t item{};
    bool found = wasmtime_instance_export_get(store.Context(), &instance, "add", 3, &item);
    assert(found);
    assert(item.kind == WASMTIME_EXTERN_FUNC);

    wasmtime_val_t args[2];
    args[0].kind = WASMTIME_I32;
    args[0].of.i32 = 17;
    args[1].kind = WASMTIME_I32;
    args[1].of.i32 = 25;

    wasmtime_val_t results[1];
    err = wasmtime_func_call(store.Context(), &item.of.func, args, 2, results, 1, &trap);
    assert(err == nullptr);
    assert(trap == nullptr);
    assert(results[0].kind == WASMTIME_I32);
    assert(results[0].of.i32 == 42);

    assert(store.ConsumedFuel() > 0);
    assert(runtime->GetConsumedFuel(store.Context(), store.InitialFuel()) == store.ConsumedFuel());
}

void TestInvalidBytecode() {
    auto runtime_res = WasmRuntime::Create();
    assert(runtime_res.has_value());
    auto runtime = std::move(runtime_res.value());

    // 1. Empty span
    auto res1 = runtime->CompileModule({});
    assert(!res1.has_value());
    assert(res1.error().Code() == ErrorCode::WasmError);

    // 2. Bad magic bytes
    const uint8_t bad_magic[] = {1, 2, 3, 4};
    auto res2 = runtime->CompileModule(bad_magic);
    assert(!res2.has_value());
    assert(res2.error().Code() == ErrorCode::WasmError);

    // 3. Truncated \0asm magic header without version
    const uint8_t truncated[] = {0x00, 0x61, 0x73, 0x6d};
    auto res3 = runtime->CompileModule(truncated);
    assert(!res3.has_value());
    assert(res3.error().Code() == ErrorCode::WasmError);
}

void TestFuelExhaustion() {
    auto runtime_res = WasmRuntime::Create();
    assert(runtime_res.has_value());
    auto runtime = std::move(runtime_res.value());

    constexpr const char* wat = R"(
        (module
            (func (export "run_loop")
                (loop $forever
                    (br $forever)
                )
            )
        )
    )";

    auto wasm_bytes = WatToWasm(wat);
    auto compile_res = runtime->CompileModule(wasm_bytes);
    assert(compile_res.has_value());

    // Small fuel limit
    auto store_res = runtime->CreateStore(nullptr, nullptr, 1000);
    assert(store_res.has_value());
    auto store = std::move(store_res.value());
    assert(store.InitialFuel() == 1000);

    wasmtime_instance_t instance{};
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err = wasmtime_linker_instantiate(
        runtime->Linker(), store.Context(), compile_res->module.get(), &instance, &trap);
    assert(err == nullptr);
    assert(trap == nullptr);

    wasmtime_extern_t item{};
    bool found = wasmtime_instance_export_get(store.Context(), &instance, "run_loop", 8, &item);
    assert(found);
    assert(item.kind == WASMTIME_EXTERN_FUNC);

    err = wasmtime_func_call(store.Context(), &item.of.func, nullptr, 0, nullptr, 0, &trap);
    assert(err == nullptr);
    assert(trap != nullptr);

    Error translated = WasmRuntime::TranslateTrap(trap);
    assert(translated.Code() == ErrorCode::WasmFuelExhausted);
    assert(store.ConsumedFuel() > 0);
}

void TestUnreachableTrap() {
    auto runtime_res = WasmRuntime::Create();
    assert(runtime_res.has_value());
    auto runtime = std::move(runtime_res.value());

    constexpr const char* wat = R"(
        (module
            (func (export "trap_func")
                (unreachable)
            )
        )
    )";

    auto wasm_bytes = WatToWasm(wat);
    auto compile_res = runtime->CompileModule(wasm_bytes);
    assert(compile_res.has_value());

    auto store_res = runtime->CreateStore();
    assert(store_res.has_value());
    auto store = std::move(store_res.value());

    wasmtime_instance_t instance{};
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err = wasmtime_linker_instantiate(
        runtime->Linker(), store.Context(), compile_res->module.get(), &instance, &trap);
    assert(err == nullptr);
    assert(trap == nullptr);

    wasmtime_extern_t item{};
    bool found = wasmtime_instance_export_get(store.Context(), &instance, "trap_func", 9, &item);
    assert(found);
    assert(item.kind == WASMTIME_EXTERN_FUNC);

    err = wasmtime_func_call(store.Context(), &item.of.func, nullptr, 0, nullptr, 0, &trap);
    assert(err == nullptr);
    assert(trap != nullptr);

    Error translated = WasmRuntime::TranslateTrap(trap);
    assert(translated.Code() == ErrorCode::WasmTrap);
}

void TestMemoryOutOfBounds() {
    auto runtime_res = WasmRuntime::Create();
    assert(runtime_res.has_value());
    auto runtime = std::move(runtime_res.value());

    // 1-page memory = 64KB. Loading at 1048576 (1MB) is out of bounds.
    constexpr const char* wat = R"(
        (module
            (memory (export "memory") 1)
            (func (export "oob_read") (result i32)
                (i32.load (i32.const 1048576))
            )
        )
    )";

    auto wasm_bytes = WatToWasm(wat);
    auto compile_res = runtime->CompileModule(wasm_bytes);
    assert(compile_res.has_value());

    auto store_res = runtime->CreateStore();
    assert(store_res.has_value());
    auto store = std::move(store_res.value());

    wasmtime_instance_t instance{};
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err = wasmtime_linker_instantiate(
        runtime->Linker(), store.Context(), compile_res->module.get(), &instance, &trap);
    assert(err == nullptr);
    assert(trap == nullptr);

    wasmtime_extern_t item{};
    bool found = wasmtime_instance_export_get(store.Context(), &instance, "oob_read", 8, &item);
    assert(found);
    assert(item.kind == WASMTIME_EXTERN_FUNC);

    wasmtime_val_t results[1];
    err = wasmtime_func_call(store.Context(), &item.of.func, nullptr, 0, results, 1, &trap);
    assert(err == nullptr);
    assert(trap != nullptr);

    Error translated = WasmRuntime::TranslateTrap(trap);
    assert(translated.Code() == ErrorCode::WasmMemoryOutOfBounds);
}

void TestMemoryLimiter() {
    // 1 MB limit = 16 pages of 64KB
    WasmConfig config{.max_fuel = 100'000'000, .max_memory_mb = 1};
    auto runtime_res = WasmRuntime::Create(config);
    assert(runtime_res.has_value());
    auto runtime = std::move(runtime_res.value());

    constexpr const char* wat = R"(
        (module
            (memory (export "memory") 1)
            (func (export "grow") (param i32) (result i32)
                (local.get 0)
                (memory.grow)
            )
        )
    )";

    auto wasm_bytes = WatToWasm(wat);
    auto compile_res = runtime->CompileModule(wasm_bytes);
    assert(compile_res.has_value());

    auto store_res = runtime->CreateStore();
    assert(store_res.has_value());
    auto store = std::move(store_res.value());

    wasmtime_instance_t instance{};
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err = wasmtime_linker_instantiate(
        runtime->Linker(), store.Context(), compile_res->module.get(), &instance, &trap);
    assert(err == nullptr);
    assert(trap == nullptr);

    wasmtime_extern_t item{};
    bool found = wasmtime_instance_export_get(store.Context(), &instance, "grow", 4, &item);
    assert(found);
    assert(item.kind == WASMTIME_EXTERN_FUNC);

    // Growing by 1 page (from 1 page to 2 pages = 128KB <= 1MB) succeeds, returning old size (1)
    wasmtime_val_t args[1];
    args[0].kind = WASMTIME_I32;
    args[0].of.i32 = 1;
    wasmtime_val_t results[1];
    err = wasmtime_func_call(store.Context(), &item.of.func, args, 1, results, 1, &trap);
    assert(err == nullptr);
    assert(trap == nullptr);
    assert(results[0].kind == WASMTIME_I32);
    assert(results[0].of.i32 == 1);

    // Growing by 32 pages (2 + 32 = 34 pages = 2176KB > 1MB) fails, returning -1
    args[0].of.i32 = 32;
    err = wasmtime_func_call(store.Context(), &item.of.func, args, 1, results, 1, &trap);
    assert(err == nullptr);
    assert(trap == nullptr);
    assert(results[0].kind == WASMTIME_I32);
    assert(results[0].of.i32 == -1);
}

void TestNullErrorAndTrapTranslation() {
    Error err = WasmRuntime::TranslateError(nullptr);
    assert(err.Code() == ErrorCode::Ok);

    Error trap = WasmRuntime::TranslateTrap(nullptr);
    assert(trap.Code() == ErrorCode::Ok);
}

} // namespace

int main() {
    TestValidModuleExecution();
    TestInvalidBytecode();
    TestFuelExhaustion();
    TestUnreachableTrap();
    TestMemoryOutOfBounds();
    TestMemoryLimiter();
    TestNullErrorAndTrapTranslation();

    std::println("test_wasm_runtime: OK");
    return 0;
}
