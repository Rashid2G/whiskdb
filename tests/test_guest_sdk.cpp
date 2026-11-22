#include "common/types.hpp"
#include "errors.hpp"
#include "logger.hpp"
#include "sql/sql_engine.hpp"
#include "wasm/host_abi.hpp"
#include "wasm/wasm_runtime.hpp"
#include "wasm_test_helper.hpp"

#include <wasmtime.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <print>
#include <string>
#include <string_view>
#include <vector>

using namespace database;
using namespace database::test_util;

namespace {

struct GuestInstance {
    StoreHandle store;
    wasmtime_instance_t instance{};
    wasmtime_memory_t memory{};
    wasmtime_func_t fn_alloc{};
    wasmtime_func_t fn_reset_heap{};
    wasmtime_func_t fn_result_ptr{};
    wasmtime_func_t fn_result_len{};

    static GuestInstance Create(
        WasmRuntime& runtime,
        const CompiledModule& compiled,
        WhiskCallContext* ctx
    ) {
        GuestInstance gi;
        auto store_res = runtime.CreateStore(ctx);
        assert(store_res.has_value());
        gi.store = std::move(store_res.value());

        wasm_trap_t* trap = nullptr;
        wasmtime_error_t* err = wasmtime_linker_instantiate(
            runtime.Linker(), gi.store.Context(), compiled.module.get(), &gi.instance, &trap);
        assert(err == nullptr);
        assert(trap == nullptr);

        wasmtime_extern_t item{};
        bool found = wasmtime_instance_export_get(gi.store.Context(), &gi.instance, "memory", 6, &item);
        assert(found && item.kind == WASMTIME_EXTERN_MEMORY);
        gi.memory = item.of.memory;

        found = wasmtime_instance_export_get(gi.store.Context(), &gi.instance, "whisk_alloc", 11, &item);
        assert(found && item.kind == WASMTIME_EXTERN_FUNC);
        gi.fn_alloc = item.of.func;

        found = wasmtime_instance_export_get(gi.store.Context(), &gi.instance, "whisk_reset_heap", 16, &item);
        assert(found && item.kind == WASMTIME_EXTERN_FUNC);
        gi.fn_reset_heap = item.of.func;

        found = wasmtime_instance_export_get(gi.store.Context(), &gi.instance, "whisk_result_ptr", 16, &item);
        assert(found && item.kind == WASMTIME_EXTERN_FUNC);
        gi.fn_result_ptr = item.of.func;

        found = wasmtime_instance_export_get(gi.store.Context(), &gi.instance, "whisk_result_len", 16, &item);
        assert(found && item.kind == WASMTIME_EXTERN_FUNC);
        gi.fn_result_len = item.of.func;

        return gi;
    }

    int32_t Alloc(int32_t size) {
        wasmtime_val_t args[1];
        args[0].kind = WASMTIME_I32;
        args[0].of.i32 = size;
        wasmtime_val_t res[1];
        wasm_trap_t* trap = nullptr;
        wasmtime_error_t* err = wasmtime_func_call(store.Context(), &fn_alloc, args, 1, res, 1, &trap);
        assert(err == nullptr && trap == nullptr);
        assert(res[0].kind == WASMTIME_I32);
        return res[0].of.i32;
    }

    void ResetHeap() {
        wasm_trap_t* trap = nullptr;
        wasmtime_error_t* err = wasmtime_func_call(store.Context(), &fn_reset_heap, nullptr, 0, nullptr, 0, &trap);
        assert(err == nullptr && trap == nullptr);
    }

    int32_t AllocAndWriteString(std::string_view str) {
        int32_t ptr = Alloc(static_cast<int32_t>(str.size() + 1));
        assert(ptr > 0);
        uint8_t* mem = wasmtime_memory_data(store.Context(), &memory);
        size_t mem_sz = wasmtime_memory_data_size(store.Context(), &memory);
        assert(static_cast<uint64_t>(ptr) + str.size() <= mem_sz);
        if (!str.empty()) {
            std::memcpy(mem + ptr, str.data(), str.size());
        }
        mem[ptr + str.size()] = '\0';
        return ptr;
    }

    std::string ReadResult() {
        wasmtime_val_t res_ptr_val[1];
        wasm_trap_t* trap = nullptr;
        wasmtime_error_t* err = wasmtime_func_call(store.Context(), &fn_result_ptr, nullptr, 0, res_ptr_val, 1, &trap);
        assert(err == nullptr && trap == nullptr);
        assert(res_ptr_val[0].kind == WASMTIME_I32);

        wasmtime_val_t res_len_val[1];
        err = wasmtime_func_call(store.Context(), &fn_result_len, nullptr, 0, res_len_val, 1, &trap);
        assert(err == nullptr && trap == nullptr);
        assert(res_len_val[0].kind == WASMTIME_I32);

        int32_t ptr = res_ptr_val[0].of.i32;
        int32_t len = res_len_val[0].of.i32;
        assert(ptr >= 0 && len >= 0);

        uint8_t* mem = wasmtime_memory_data(store.Context(), &memory);
        size_t mem_sz = wasmtime_memory_data_size(store.Context(), &memory);
        assert(static_cast<uint64_t>(ptr) + static_cast<uint64_t>(len) <= mem_sz);

        return std::string(reinterpret_cast<const char*>(mem + ptr), static_cast<size_t>(len));
    }

    int32_t CallFunc(std::string_view func_name, int32_t args_ptr = 0, int32_t args_len = 0) {
        wasmtime_extern_t item{};
        bool found = wasmtime_instance_export_get(
            store.Context(), &instance, func_name.data(), func_name.size(), &item);
        assert(found && item.kind == WASMTIME_EXTERN_FUNC);

        wasmtime_val_t res[1];
        wasm_trap_t* trap = nullptr;
        if (args_ptr == 0 && args_len == 0) {
            // Check parameter count
            wasm_functype_t* ty = wasmtime_func_type(store.Context(), &item.of.func);
            const wasm_valtype_vec_t* params = wasm_functype_params(ty);
            size_t nparams = params->size;
            wasm_functype_delete(ty);

            if (nparams == 0) {
                wasmtime_error_t* err = wasmtime_func_call(store.Context(), &item.of.func, nullptr, 0, res, 1, &trap);
                assert(err == nullptr && trap == nullptr);
                assert(res[0].kind == WASMTIME_I32);
                return res[0].of.i32;
            }
        }

        wasmtime_val_t args[2];
        args[0].kind = WASMTIME_I32; args[0].of.i32 = args_ptr;
        args[1].kind = WASMTIME_I32; args[1].of.i32 = args_len;
        wasmtime_error_t* err = wasmtime_func_call(store.Context(), &item.of.func, args, 2, res, 1, &trap);
        assert(err == nullptr && trap == nullptr);
        assert(res[0].kind == WASMTIME_I32);
        return res[0].of.i32;
    }
};

void TestCounterWhiskE2E() {
    std::filesystem::path src_dir = FindSdkIncludeDir().parent_path().parent_path();
    std::filesystem::path counter_cpp = src_dir / "examples" / "counter_whisk.cpp";
    std::filesystem::path out_wasm = std::filesystem::temp_directory_path() / "test_counter_whisk.wasm";

    // 1. Compile examples/counter_whisk.cpp to WASM
    auto compile_wasm_res = CompileWhiskCppToWasm(counter_cpp, out_wasm);
    assert(compile_wasm_res.has_value());
    const auto& wasm_bytes = *compile_wasm_res;
    assert(!wasm_bytes.empty());

    // 2. Load and verify exported function symbols
    auto runtime_res = WasmRuntime::Create();
    assert(runtime_res.has_value());
    auto runtime = std::move(*runtime_res);

    auto reg_res = RegisterHostAbi(runtime->Linker());
    assert(reg_res.has_value());

    auto mod_res = runtime->CompileModule(wasm_bytes);
    assert(mod_res.has_value());
    auto compiled = std::move(*mod_res);

    const auto& funcs = compiled.exported_funcs;
    auto has_func = [&funcs](std::string_view name) {
        return std::find(funcs.begin(), funcs.end(), name) != funcs.end();
    };
    assert(has_func("whisk_alloc"));
    assert(has_func("whisk_reset_heap"));
    assert(has_func("whisk_result_ptr"));
    assert(has_func("whisk_result_len"));
    assert(has_func("__init__"));
    assert(has_func("increment"));
    assert(has_func("get"));

    // 3. Setup host SqlEngine & WhiskCallContext
    auto sql_res = SqlEngine::Open();
    assert(sql_res.has_value());
    auto sql = std::move(*sql_res);

    WhiskCallContext ctx;
    ctx.sql = sql.get();
    ctx.module_name = "counter_whisk";

    auto guest = GuestInstance::Create(*runtime, compiled, &ctx);

    // 4. Test allocator bump and reset
    int32_t p1 = guest.Alloc(64);
    assert(p1 > 0);
    int32_t p2 = guest.Alloc(64);
    assert(p2 > p1);
    guest.ResetHeap();
    int32_t p3 = guest.Alloc(64);
    assert(p3 == p1); // Successfully reset to initial heap offset!

    // 5. Invoke __init__()
    int32_t rc_init = guest.CallFunc("__init__");
    assert(rc_init == 0);

    // Verify counters table exists in sqlite
    auto chk_table = sql->Execute("SELECT count(*) FROM counters;");
    assert(chk_table.has_value());
    assert(std::get<int64_t>(chk_table->rows[0][0]) == 0);

    // 6. Invoke increment(ptr, len) with {"name":"hits","delta":5}
    std::string arg1 = "{\"name\":\"hits\",\"delta\":5}";
    int32_t arg1_ptr = guest.AllocAndWriteString(arg1);
    int32_t rc_inc1 = guest.CallFunc("increment", arg1_ptr, static_cast<int32_t>(arg1.size()));
    assert(rc_inc1 == 0);

    std::string res1 = guest.ReadResult();
    assert(res1.find("\"name\":\"hits\"") != std::string::npos);
    assert(res1.find("\"val\":5") != std::string::npos);
    assert(!ctx.mutating_statements.empty());

    // 7. Invoke increment(ptr, len) second time with delta 3
    std::string arg2 = "{\"name\":\"hits\",\"delta\":3}";
    int32_t arg2_ptr = guest.AllocAndWriteString(arg2);
    int32_t rc_inc2 = guest.CallFunc("increment", arg2_ptr, static_cast<int32_t>(arg2.size()));
    assert(rc_inc2 == 0);

    std::string res2 = guest.ReadResult();
    assert(res2.find("\"name\":\"hits\"") != std::string::npos);
    assert(res2.find("\"val\":8") != std::string::npos);
    assert(ctx.mutating_statements.size() >= 2);

    // 8. Invoke get for "hits"
    std::string arg_get_hits = "{\"name\":\"hits\"}";
    int32_t get_hits_ptr = guest.AllocAndWriteString(arg_get_hits);
    int32_t rc_get1 = guest.CallFunc("get", get_hits_ptr, static_cast<int32_t>(arg_get_hits.size()));
    assert(rc_get1 == 0);
    std::string res_get1 = guest.ReadResult();
    assert(res_get1.find("\"name\":\"hits\"") != std::string::npos);
    assert(res_get1.find("\"val\":8") != std::string::npos);

    // 9. Invoke get for "missing"
    std::string arg_get_missing = "{\"name\":\"missing\"}";
    int32_t get_missing_ptr = guest.AllocAndWriteString(arg_get_missing);
    int32_t rc_get2 = guest.CallFunc("get", get_missing_ptr, static_cast<int32_t>(arg_get_missing.size()));
    assert(rc_get2 == 1); // Returns 1 on error
    std::string res_get2 = guest.ReadResult();
    assert(res_get2.find("counter not found") != std::string::npos);
}

void TestSdkErgonomicsInWasm() {
    constexpr std::string_view kErgonomicsTestCpp = R"(
#include "whisk_guest.hpp"

WHISK_FUNC(test_buf_formatting) {
    whisk::Buf<256> buf;
    buf.append("int_min:");
    buf.append_int(-9223372036854775807LL - 1LL); // INT64_MIN
    buf.append(",zero:");
    buf.append_int(0);
    buf.append(",pos:");
    buf.append_int(123456789);
    buf.append(",dbl:");
    buf.append_double(-123.45);
    buf.append(",json:");
    buf.append_json_str("line\n\"quoted\"\\");

    return whisk::Ok(buf.view());
}

WHISK_FUNC(test_json_parsing) {
    whisk::StrView args = WHISK_ARGS();
    int64_t val = 0;
    bool found_neg = whisk::JsonGetInt(args, "negative", val);
    if (!found_neg || val != -42) {
        return whisk::Error("failed negative int");
    }

    int64_t def_val = whisk::JsonGetIntOr(args, "missing_key", 999);
    if (def_val != 999) {
        return whisk::Error("failed default int");
    }

    whisk::StrView str_val;
    bool found_str = whisk::JsonGetStr(args, "greeting", str_val);
    if (!found_str || !str_val.equals("hello world")) {
        return whisk::Error("failed get str");
    }

    return whisk::Ok("{\"status\":\"ok\"}");
}

WHISK_FUNC(test_query_helpers) {
    // Test QueryHasRows and QueryRowInt on synthetic host query output format
    whisk::StrView mock_empty = "{\"columns\":[\"a\"],\"rows\":[]}";
    if (whisk::QueryHasRows(mock_empty)) {
        return whisk::Error("expected empty rows");
    }

    whisk::StrView mock_rows = "{\"columns\":[\"id\",\"name\",\"val\"],\"rows\":[[10,\"Alice\",-500],[20,\"Bob\",300]]}";
    if (!whisk::QueryHasRows(mock_rows)) {
        return whisk::Error("expected rows");
    }

    int64_t col0 = 0;
    if (!whisk::QueryRowInt(mock_rows, 0, col0) || col0 != 10) {
        return whisk::Error("failed col0");
    }

    int64_t col2 = 0;
    if (!whisk::QueryRowInt(mock_rows, 2, col2) || col2 != -500) {
        return whisk::Error("failed col2");
    }

    int64_t first_int = 0;
    if (!whisk::QueryFirstInt(mock_rows, first_int) || first_int != 10) {
        return whisk::Error("failed first int");
    }

    return whisk::Ok("{\"all_helpers_passed\":true}");
}
)";

    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "whisk_sdk_tests";
    auto wasm_bytes_res = CompileWhiskSourceToWasm(kErgonomicsTestCpp, temp_dir, "test_ergonomics");
    assert(wasm_bytes_res.has_value());

    auto runtime_res = WasmRuntime::Create();
    assert(runtime_res.has_value());
    auto runtime = std::move(*runtime_res);
    assert(RegisterHostAbi(runtime->Linker()).has_value());

    auto mod_res = runtime->CompileModule(*wasm_bytes_res);
    assert(mod_res.has_value());
    auto compiled = std::move(*mod_res);

    auto sql_res = SqlEngine::Open();
    assert(sql_res.has_value());
    auto sql = std::move(*sql_res);

    WhiskCallContext ctx;
    ctx.sql = sql.get();
    ctx.module_name = "test_ergonomics";

    auto guest = GuestInstance::Create(*runtime, compiled, &ctx);

    // 1. Test Buf formatting
    int32_t rc1 = guest.CallFunc("test_buf_formatting");
    assert(rc1 == 0);
    std::string res1 = guest.ReadResult();
    assert(res1.find("int_min:-9223372036854775808") != std::string::npos);
    assert(res1.find("zero:0") != std::string::npos);
    assert(res1.find("pos:123456789") != std::string::npos);
    assert(res1.find("dbl:-123.45") != std::string::npos);
    assert(res1.find("json:\"line\\n\\\"quoted\\\"\\\\\"") != std::string::npos);

    // 2. Test JSON parsing
    std::string json_input = "{\"negative\":-42,\"greeting\":\"hello world\"}";
    int32_t json_ptr = guest.AllocAndWriteString(json_input);
    int32_t rc2 = guest.CallFunc("test_json_parsing", json_ptr, static_cast<int32_t>(json_input.size()));
    assert(rc2 == 0);
    std::string res2 = guest.ReadResult();
    assert(res2.find("\"status\":\"ok\"") != std::string::npos);

    // 3. Test Query JSON helpers
    int32_t rc3 = guest.CallFunc("test_query_helpers");
    assert(rc3 == 0);
    std::string res3 = guest.ReadResult();
    assert(res3.find("\"all_helpers_passed\":true") != std::string::npos);
}

} // namespace

int main() {
    std::println("==> Running TestCounterWhiskE2E...");
    TestCounterWhiskE2E();

    std::println("==> Running TestSdkErgonomicsInWasm...");
    TestSdkErgonomicsInWasm();

    std::println("test_guest_sdk: ALL TESTS PASSED");
    return 0;
}
