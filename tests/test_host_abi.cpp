#include "common/types.hpp"
#include "errors.hpp"
#include "logger.hpp"
#include "sql/catalog.hpp"
#include "sql/sql_engine.hpp"
#include "wasm/host_abi.hpp"
#include "wasm/wasm_runtime.hpp"

#include <wasmtime.h>
#include <wasmtime/wat.h>

#include <cassert>
#include <cmath>
#include <cstring>
#include <print>
#include <string>
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

constexpr const char* kHostAbiWat = R"(
(module
    (import "whisk" "whisk_db_exec" (func $whisk_db_exec (param i32 i32 i32 i32) (result i64)))
    (import "whisk" "whisk_db_query" (func $whisk_db_query (param i32 i32 i32 i32 i32 i32) (result i32)))
    (import "whisk" "whisk_log" (func $whisk_log (param i32 i32 i32)))
    (memory (export "memory") 1)

    (func (export "call_exec") (param i32 i32 i32 i32) (result i64)
        (call $whisk_db_exec (local.get 0) (local.get 1) (local.get 2) (local.get 3))
    )
    (func (export "call_query") (param i32 i32 i32 i32 i32 i32) (result i32)
        (call $whisk_db_query (local.get 0) (local.get 1) (local.get 2) (local.get 3) (local.get 4) (local.get 5))
    )
    (func (export "call_log") (param i32 i32 i32)
        (call $whisk_log (local.get 0) (local.get 1) (local.get 2))
    )
)
)";

struct TestWasmHarness {
    std::unique_ptr<SqlEngine> sql;
    std::unique_ptr<WasmRuntime> runtime;
    CompiledModule compiled;
    WhiskCallContext ctx;
    StoreHandle store;
    wasmtime_instance_t instance{};
    wasmtime_func_t fn_exec{};
    wasmtime_func_t fn_query{};
    wasmtime_func_t fn_log{};
    uint8_t* mem_ptr = nullptr;
    size_t mem_size = 0;

    static TestWasmHarness Create(std::string mod_name = "test_mod") {
        TestWasmHarness h;
        auto sql_res = SqlEngine::Open();
        assert(sql_res.has_value());
        h.sql = std::move(sql_res.value());

        auto runtime_res = WasmRuntime::Create();
        assert(runtime_res.has_value());
        h.runtime = std::move(runtime_res.value());

        auto reg_res = RegisterHostAbi(h.runtime->Linker());
        assert(reg_res.has_value());

        auto wasm_bytes = WatToWasm(kHostAbiWat);
        auto compile_res = h.runtime->CompileModule(wasm_bytes);
        assert(compile_res.has_value());
        h.compiled = std::move(compile_res.value());

        h.ctx.sql = h.sql.get();
        h.ctx.module_name = std::move(mod_name);
        h.ctx.mutating_statements.clear();
        h.ctx.last_error.reset();

        auto store_res = h.runtime->CreateStore(&h.ctx);
        assert(store_res.has_value());
        h.store = std::move(store_res.value());

        wasm_trap_t* trap = nullptr;
        wasmtime_error_t* err = wasmtime_linker_instantiate(
            h.runtime->Linker(), h.store.Context(), h.compiled.module.get(), &h.instance, &trap);
        assert(err == nullptr);
        assert(trap == nullptr);

        wasmtime_extern_t item{};
        bool found = wasmtime_instance_export_get(h.store.Context(), &h.instance, "call_exec", 9, &item);
        assert(found && item.kind == WASMTIME_EXTERN_FUNC);
        h.fn_exec = item.of.func;

        found = wasmtime_instance_export_get(h.store.Context(), &h.instance, "call_query", 10, &item);
        assert(found && item.kind == WASMTIME_EXTERN_FUNC);
        h.fn_query = item.of.func;

        found = wasmtime_instance_export_get(h.store.Context(), &h.instance, "call_log", 8, &item);
        assert(found && item.kind == WASMTIME_EXTERN_FUNC);
        h.fn_log = item.of.func;

        found = wasmtime_instance_export_get(h.store.Context(), &h.instance, "memory", 6, &item);
        assert(found && item.kind == WASMTIME_EXTERN_MEMORY);
        h.mem_ptr = wasmtime_memory_data(h.store.Context(), &item.of.memory);
        h.mem_size = wasmtime_memory_data_size(h.store.Context(), &item.of.memory);

        return h;
    }

    void WriteString(int32_t offset, std::string_view str) {
        assert(offset >= 0);
        assert(static_cast<uint64_t>(offset) + str.size() <= mem_size);
        if (!str.empty()) {
            std::memcpy(mem_ptr + offset, str.data(), str.size());
        }
    }

    std::string ReadString(int32_t offset, int32_t len) {
        assert(offset >= 0 && len >= 0);
        assert(static_cast<uint64_t>(offset) + static_cast<uint64_t>(len) <= mem_size);
        return std::string(reinterpret_cast<const char*>(mem_ptr + offset), static_cast<size_t>(len));
    }

    int64_t Exec(int32_t sql_ptr, int32_t sql_len, int32_t p_ptr, int32_t p_len) {
        wasmtime_val_t args[4];
        args[0].kind = WASMTIME_I32; args[0].of.i32 = sql_ptr;
        args[1].kind = WASMTIME_I32; args[1].of.i32 = sql_len;
        args[2].kind = WASMTIME_I32; args[2].of.i32 = p_ptr;
        args[3].kind = WASMTIME_I32; args[3].of.i32 = p_len;
        wasmtime_val_t results[1];
        wasm_trap_t* trap = nullptr;
        wasmtime_error_t* err = wasmtime_func_call(store.Context(), &fn_exec, args, 4, results, 1, &trap);
        assert(err == nullptr);
        assert(trap == nullptr);
        assert(results[0].kind == WASMTIME_I64);
        return results[0].of.i64;
    }

    int32_t Query(int32_t sql_ptr, int32_t sql_len, int32_t p_ptr, int32_t p_len, int32_t out_ptr, int32_t out_cap) {
        wasmtime_val_t args[6];
        args[0].kind = WASMTIME_I32; args[0].of.i32 = sql_ptr;
        args[1].kind = WASMTIME_I32; args[1].of.i32 = sql_len;
        args[2].kind = WASMTIME_I32; args[2].of.i32 = p_ptr;
        args[3].kind = WASMTIME_I32; args[3].of.i32 = p_len;
        args[4].kind = WASMTIME_I32; args[4].of.i32 = out_ptr;
        args[5].kind = WASMTIME_I32; args[5].of.i32 = out_cap;
        wasmtime_val_t results[1];
        wasm_trap_t* trap = nullptr;
        wasmtime_error_t* err = wasmtime_func_call(store.Context(), &fn_query, args, 6, results, 1, &trap);
        assert(err == nullptr);
        assert(trap == nullptr);
        assert(results[0].kind == WASMTIME_I32);
        return results[0].of.i32;
    }

    void Log(int32_t level, int32_t msg_ptr, int32_t msg_len) {
        wasmtime_val_t args[3];
        args[0].kind = WASMTIME_I32; args[0].of.i32 = level;
        args[1].kind = WASMTIME_I32; args[1].of.i32 = msg_ptr;
        args[2].kind = WASMTIME_I32; args[2].of.i32 = msg_len;
        wasm_trap_t* trap = nullptr;
        wasmtime_error_t* err = wasmtime_func_call(store.Context(), &fn_log, args, 3, nullptr, 0, &trap);
        assert(err == nullptr);
        assert(trap == nullptr);
    }
};

void TestDirectParamParsingAndSerialization() {
    // 1. Empty / whitespace params
    auto res_empty = ParseHostAbiParamsJson("");
    assert(res_empty.has_value() && res_empty->empty());
    auto res_ws = ParseHostAbiParamsJson("   \t\n  ");
    assert(res_ws.has_value() && res_ws->empty());
    auto res_arr_empty = ParseHostAbiParamsJson("[]");
    assert(res_arr_empty.has_value() && res_arr_empty->empty());
    auto res_arr_ws = ParseHostAbiParamsJson("[   ]");
    assert(res_arr_ws.has_value() && res_arr_ws->empty());

    // 2. Full typed array with escapes and surrogate pairs
    std::string json = R"([1, "Alice", 250.5, null, true, false, "line\nbreak \"quotes\"", "\u0041"])";
    auto res = ParseHostAbiParamsJson(json);
    assert(res.has_value());
    const auto& vals = *res;
    assert(vals.size() == 8);
    assert(std::get<int64_t>(vals[0]) == 1);
    assert(std::get<std::string>(vals[1]) == "Alice");
    assert(std::abs(std::get<double>(vals[2]) - 250.5) < 1e-6);
    assert(std::holds_alternative<std::monostate>(vals[3]));
    assert(std::get<int64_t>(vals[4]) == 1);
    assert(std::get<int64_t>(vals[5]) == 0);
    assert(std::get<std::string>(vals[6]) == "line\nbreak \"quotes\"");
    assert(std::get<std::string>(vals[7]) == "A");

    // 3. Error cases for invalid JSON
    assert(!ParseHostAbiParamsJson("[1, 2").has_value());
    assert(!ParseHostAbiParamsJson("1, 2]").has_value());
    assert(!ParseHostAbiParamsJson("[1,]").has_value());
    assert(!ParseHostAbiParamsJson("[1, 2] extra").has_value());
    assert(!ParseHostAbiParamsJson(R"({"key": 1})").has_value());
    assert(!ParseHostAbiParamsJson("[undefined]").has_value());

    // 4. QueryResult serialization
    QueryResult qr;
    qr.columns = {"id", "name", "active", "extra", "data"};
    Blob b = {0xde, 0xad, 0xbe, 0xef};
    qr.rows.push_back({int64_t{10}, std::string("Bob\n\"Jr\""), int64_t{1}, std::monostate{}, b});
    std::string serialized = SerializeQueryResultToJson(qr);
    std::string expected = "{\"columns\":[\"id\",\"name\",\"active\",\"extra\",\"data\"],\"rows\":[[10,\"Bob\\n\\\"Jr\\\"\",1,null,\"deadbeef\"]]}";
    assert(serialized == expected);
}

void TestValidWhiskDbExec() {
    auto h = TestWasmHarness::Create();

    // DDL: CREATE TABLE
    std::string ddl = "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, balance REAL, extra TEXT, is_active INTEGER);";
    h.WriteString(100, ddl);
    int64_t exec_res = h.Exec(100, static_cast<int32_t>(ddl.size()), 0, 0);
    assert(exec_res == 0);
    assert(h.ctx.mutating_statements.size() == 1);
    assert(h.ctx.mutating_statements[0].sql == ddl);

    // DML: INSERT with params [1, "Alice", 250.5, null, true]
    std::string insert_sql = "INSERT INTO users VALUES (?, ?, ?, ?, ?);";
    std::string params_json = R"([1, "Alice", 250.5, null, true])";
    h.WriteString(100, insert_sql);
    h.WriteString(300, params_json);
    exec_res = h.Exec(100, static_cast<int32_t>(insert_sql.size()), 300, static_cast<int32_t>(params_json.size()));
    assert(exec_res == 1); // 1 row affected
    assert(h.ctx.mutating_statements.size() == 2);

    const auto& stmt = h.ctx.mutating_statements[1];
    assert(stmt.sql == insert_sql);
    assert(stmt.params.size() == 5);
    assert(std::get<int64_t>(stmt.params[0]) == 1);
    assert(std::get<std::string>(stmt.params[1]) == "Alice");
    assert(std::abs(std::get<double>(stmt.params[2]) - 250.5) < 1e-6);
    assert(std::holds_alternative<std::monostate>(stmt.params[3]));
    assert(std::get<int64_t>(stmt.params[4]) == 1);
}

void TestValidWhiskDbQuery() {
    auto h = TestWasmHarness::Create();

    // Prepare table and row with all types (NULL, INT64, DOUBLE, TEXT with quotes/newlines, BLOB)
    std::string setup_ddl = "CREATE TABLE items (id INTEGER, val_null TEXT, val_int INTEGER, val_double REAL, val_text TEXT, val_blob BLOB);";
    h.WriteString(100, setup_ddl);
    h.Exec(100, static_cast<int32_t>(setup_ddl.size()), 0, 0);

    std::string insert_sql = "INSERT INTO items VALUES (1, NULL, 42, 3.1415, 'Line 1\nLine 2 with \"quotes\"', X'0102030a');";
    h.WriteString(100, insert_sql);
    h.Exec(100, static_cast<int32_t>(insert_sql.size()), 0, 0);

    // Run query
    std::string query_sql = "SELECT * FROM items;";
    h.WriteString(100, query_sql);
    int32_t q_res = h.Query(100, static_cast<int32_t>(query_sql.size()), 0, 0, 1000, 2048);
    assert(q_res > 0);

    std::string json_result = h.ReadString(1000, q_res);
    std::string expected = "{\"columns\":[\"id\",\"val_null\",\"val_int\",\"val_double\",\"val_text\",\"val_blob\"],\"rows\":[[1,null,42,3.1415,\"Line 1\\nLine 2 with \\\"quotes\\\"\",\"0102030a\"]]}";
    assert(json_result == expected);
}

void TestWhiskDbQueryBufferTooSmall() {
    auto h = TestWasmHarness::Create();

    std::string setup_ddl = "CREATE TABLE t (x INTEGER); INSERT INTO t VALUES (12345);";
    auto script_res = h.sql->ExecuteScript(setup_ddl);
    assert(script_res.has_value());

    std::string query_sql = "SELECT * FROM t;";
    h.WriteString(100, query_sql);

    // Provide buffer of capacity 5 (too small for JSON result)
    int32_t q_res = h.Query(100, static_cast<int32_t>(query_sql.size()), 0, 0, 1000, 5);
    assert(q_res == -static_cast<int32_t>(ErrorCode::OutOfMemory));
    assert(h.ctx.last_error.has_value());
    assert(h.ctx.last_error->Code() == ErrorCode::OutOfMemory);
}

void TestWhiskDbQueryRejectMutations() {
    auto h = TestWasmHarness::Create();

    std::string setup_ddl = "CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT); INSERT INTO t VALUES (1, 'initial');";
    auto script_res = h.sql->ExecuteScript(setup_ddl);
    assert(script_res.has_value());

    // 1. Attempt INSERT via query
    std::string bad_insert = "INSERT INTO t VALUES (2, 'injected');";
    h.WriteString(100, bad_insert);
    int32_t res1 = h.Query(100, static_cast<int32_t>(bad_insert.size()), 0, 0, 1000, 1024);
    assert(res1 == -static_cast<int32_t>(ErrorCode::InvalidArgument));
    assert(h.ctx.last_error.has_value() && h.ctx.last_error->Code() == ErrorCode::InvalidArgument);

    // Verify row 2 was NOT inserted (zero side effects, rollback succeeded)
    auto verify1 = h.sql->Execute("SELECT count(*) FROM t WHERE id = 2;");
    assert(verify1.has_value());
    assert(std::get<int64_t>(verify1->rows[0][0]) == 0);

    // 2. Attempt DELETE via query
    std::string bad_delete = "DELETE FROM t WHERE id = 1;";
    h.WriteString(100, bad_delete);
    int32_t res2 = h.Query(100, static_cast<int32_t>(bad_delete.size()), 0, 0, 1000, 1024);
    assert(res2 == -static_cast<int32_t>(ErrorCode::InvalidArgument));

    // Verify row 1 still exists
    auto verify2 = h.sql->Execute("SELECT count(*) FROM t WHERE id = 1;");
    assert(verify2.has_value());
    assert(std::get<int64_t>(verify2->rows[0][0]) == 1);
}

void TestOobPointers() {
    auto h = TestWasmHarness::Create();

    // 1. Negative pointer
    int64_t res1 = h.Exec(-1, 10, 0, 0);
    assert(res1 == -static_cast<int64_t>(ErrorCode::WasmMemoryOutOfBounds));
    assert(h.ctx.last_error.has_value() && h.ctx.last_error->Code() == ErrorCode::WasmMemoryOutOfBounds);

    // 2. Negative length
    int64_t res2 = h.Exec(100, -5, 0, 0);
    assert(res2 == -static_cast<int64_t>(ErrorCode::WasmMemoryOutOfBounds));

    // 3. Overflow: ptr = 0x7FFFFFFF, len = 100
    int64_t res3 = h.Exec(0x7FFFFFFF, 100, 0, 0);
    assert(res3 == -static_cast<int64_t>(ErrorCode::WasmMemoryOutOfBounds));

    // 4. Past-end output buffer in Query
    std::string q = "SELECT 1;";
    h.WriteString(100, q);
    int32_t res4 = h.Query(100, static_cast<int32_t>(q.size()), 0, 0, 65530, 100);
    assert(res4 == -static_cast<int32_t>(ErrorCode::WasmMemoryOutOfBounds));
}

void TestForbiddenTransactionControl() {
    auto h = TestWasmHarness::Create();

    const char* forbidden_queries[] = {
        "BEGIN",
        "BEGIN TRANSACTION",
        "COMMIT",
        "END",
        "ROLLBACK",
        "SAVEPOINT sp1",
        "RELEASE sp1",
        "-- leading comment\nCOMMIT",
        "/* block comment */ ROLLBACK"
    };

    for (const char* sql_str : forbidden_queries) {
        std::string sql(sql_str);
        h.WriteString(100, sql);
        int64_t res = h.Exec(100, static_cast<int32_t>(sql.size()), 0, 0);
        assert(res == -static_cast<int64_t>(ErrorCode::InvalidArgument));
        assert(h.ctx.last_error.has_value() && h.ctx.last_error->Code() == ErrorCode::InvalidArgument);

        int32_t q_res = h.Query(100, static_cast<int32_t>(sql.size()), 0, 0, 1000, 1024);
        assert(q_res == -static_cast<int32_t>(ErrorCode::InvalidArgument));
    }
}

void TestCatalogTampering() {
    auto h = TestWasmHarness::Create();
    assert(SystemCatalog::Initialize(*h.sql).has_value());

    std::string drop_sql = "DROP TABLE _whisk_modules;";
    h.WriteString(100, drop_sql);
    int64_t res = h.Exec(100, static_cast<int32_t>(drop_sql.size()), 0, 0);
    assert(res == -static_cast<int64_t>(ErrorCode::AccessDenied));
    assert(h.ctx.last_error.has_value() && h.ctx.last_error->Code() == ErrorCode::AccessDenied);
}

void TestWhiskLog() {
    auto h = TestWasmHarness::Create();

    Logger::SetLevel(LogLevel::Trace);
    Logger::ClearRecentLogs();

    std::string msg = "hello host abi log";
    h.WriteString(500, msg);
    h.Log(2, 500, static_cast<int32_t>(msg.size())); // Level 2 = Info

    auto logs = Logger::GetRecentLogs(50);
    bool found = false;
    for (const auto& line : logs) {
        if (line.find("[whisk:test_mod] hello host abi log") != std::string::npos) {
            found = true;
            break;
        }
    }
    assert(found);

    // OOB log call: should not crash, should set ctx.last_error
    h.ctx.last_error.reset();
    h.Log(1, -1, 10);
    assert(h.ctx.last_error.has_value());
    assert(h.ctx.last_error->Code() == ErrorCode::WasmMemoryOutOfBounds);
}

} // namespace

int main() {
    TestDirectParamParsingAndSerialization();
    TestValidWhiskDbExec();
    TestValidWhiskDbQuery();
    TestWhiskDbQueryBufferTooSmall();
    TestWhiskDbQueryRejectMutations();
    TestOobPointers();
    TestForbiddenTransactionControl();
    TestCatalogTampering();
    TestWhiskLog();

    std::println("test_host_abi: ALL TESTS PASSED");
    return 0;
}
