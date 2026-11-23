#include "common/sha256.hpp"
#include "common/types.hpp"
#include "errors.hpp"
#include "sql/catalog.hpp"
#include "sql/sql_engine.hpp"
#include "wasm/host_abi.hpp"
#include "wasm/module_registry.hpp"
#include "wasm/wasm_runtime.hpp"
#include "wasm_test_helper.hpp"

#include <wasmtime.h>
#include <wasmtime/wat.h>

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

const std::filesystem::path kWorkDir = "/tmp/whisk_test_lifecycle";

// -----------------------------------------------------------------------------
// Test 1: First deploy (__init__)
// -----------------------------------------------------------------------------
void TestFirstDeployInit() {
    std::println("--> Running TestFirstDeployInit...");

    auto sql_res = SqlEngine::Open();
    assert(sql_res.has_value());
    auto sql = std::move(sql_res.value());

    auto cat_init = SystemCatalog::Initialize(*sql);
    assert(cat_init.has_value());

    auto runtime_res = WasmRuntime::Create();
    assert(runtime_res.has_value());
    auto runtime = std::move(runtime_res.value());

    auto reg_res = RegisterHostAbi(runtime->Linker());
    assert(reg_res.has_value());

    ModuleRegistry registry(*runtime);

    const char* v1_code = R"(
#include "whisk_guest.hpp"

WHISK_INIT() {
    whisk::Exec("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);");
    whisk::Exec("INSERT INTO users (id, name) VALUES (1, 'Alice');");
    return 0;
}

WHISK_FUNC(ping) {
    return whisk::Ok("pong");
}
)";

    auto wasm_res = CompileWhiskSourceToWasm(v1_code, kWorkDir, "deploy_v1");
    assert(wasm_res.has_value());
    auto wasm_bytes = std::move(wasm_res.value());

    auto pub_res = registry.Publish(*sql, "users_mod", wasm_bytes);
    assert(pub_res.has_value());

    const auto& prep = pub_res.value();
    assert(prep.module.version == 1);
    assert(prep.module.name == "users_mod");
    assert(prep.module.sha256 == Sha256Hex(wasm_bytes));
    assert(prep.module.exported_funcs == std::vector<std::string>{"ping"});
    assert(!prep.mutating_statements.empty());

    // Verify table & seed row exist
    auto q_res = sql->Execute("SELECT id, name FROM users;");
    assert(q_res.has_value());
    assert(q_res->rows.size() == 1);
    assert(std::get<int64_t>(q_res->rows[0][0]) == 1);
    assert(std::get<std::string>(q_res->rows[0][1]) == "Alice");

    // Verify _whisk_modules has v1
    auto mod_cat = SystemCatalog::GetModule(*sql, "users_mod");
    assert(mod_cat.has_value() && mod_cat->has_value());
    assert((*mod_cat)->version == 1);
    assert((*mod_cat)->exported_funcs == std::vector<std::string>{"ping"});

    // Verify _whisk_migrations has __init__ v1
    auto mig_q = sql->Execute("SELECT module_name, version, hook_name FROM _whisk_migrations;");
    assert(mig_q.has_value());
    assert(mig_q->rows.size() == 1);
    assert(std::get<std::string>(mig_q->rows[0][0]) == "users_mod");
    assert(std::get<int64_t>(mig_q->rows[0][1]) == 1);
    assert(std::get<std::string>(mig_q->rows[0][2]) == "__init__");

    // Verify in registry
    assert(registry.Size() == 1);
    auto cached = registry.Get("users_mod");
    assert(cached.has_value());
    assert(cached->version == 1);
    assert(cached->exported_funcs == std::vector<std::string>{"ping"});

    std::println("--> TestFirstDeployInit PASSED.");
}

// -----------------------------------------------------------------------------
// Test 2: Hot-reload (__update__)
// -----------------------------------------------------------------------------
void TestHotReloadUpdate() {
    std::println("--> Running TestHotReloadUpdate...");

    auto sql_res = SqlEngine::Open();
    assert(sql_res.has_value());
    auto sql = std::move(sql_res.value());

    auto cat_init = SystemCatalog::Initialize(*sql);
    assert(cat_init.has_value());

    auto runtime_res = WasmRuntime::Create();
    assert(runtime_res.has_value());
    auto runtime = std::move(runtime_res.value());

    auto reg_res = RegisterHostAbi(runtime->Linker());
    assert(reg_res.has_value());

    ModuleRegistry registry(*runtime);

    // v1
    const char* v1_code = R"(
#include "whisk_guest.hpp"

WHISK_INIT() {
    whisk::Exec("CREATE TABLE items (id INTEGER PRIMARY KEY, title TEXT);");
    whisk::Exec("INSERT INTO items (id, title) VALUES (10, 'Sword');");
    return 0;
}

WHISK_FUNC(ping) {
    return whisk::Ok("pong");
}
)";

    auto wasm1_res = CompileWhiskSourceToWasm(v1_code, kWorkDir, "reload_v1");
    assert(wasm1_res.has_value());
    auto pub1 = registry.Publish(*sql, "items_mod", wasm1_res.value());
    assert(pub1.has_value());
    assert(pub1->module.version == 1);

    // v2: adds column and inserts new row + exports ping & pong
    const char* v2_code = R"(
#include "whisk_guest.hpp"

WHISK_UPDATE() {
    whisk::Exec("ALTER TABLE items ADD COLUMN price INTEGER DEFAULT 0;");
    whisk::Exec("INSERT INTO items (id, title, price) VALUES (20, 'Shield', 50);");
    return 0;
}

WHISK_FUNC(ping) {
    return whisk::Ok("pong");
}

WHISK_FUNC(pong) {
    return whisk::Ok("ping");
}
)";

    auto wasm2_res = CompileWhiskSourceToWasm(v2_code, kWorkDir, "reload_v2");
    assert(wasm2_res.has_value());
    auto pub2 = registry.Publish(*sql, "items_mod", wasm2_res.value());
    assert(pub2.has_value());

    assert(pub2->module.version == 2);
    std::vector<std::string> expected_funcs = {"ping", "pong"};
    assert(pub2->module.exported_funcs == expected_funcs);

    // Check table & columns
    auto q_res = sql->Execute("SELECT id, title, price FROM items ORDER BY id ASC;");
    assert(q_res.has_value());
    assert(q_res->rows.size() == 2);
    assert(std::get<int64_t>(q_res->rows[0][0]) == 10);
    assert(std::get<std::string>(q_res->rows[0][1]) == "Sword");
    assert(std::get<int64_t>(q_res->rows[0][2]) == 0);

    assert(std::get<int64_t>(q_res->rows[1][0]) == 20);
    assert(std::get<std::string>(q_res->rows[1][1]) == "Shield");
    assert(std::get<int64_t>(q_res->rows[1][2]) == 50);

    // Check migrations table has __init__ v1 and __update__ v2
    auto mig_q = sql->Execute("SELECT version, hook_name FROM _whisk_migrations ORDER BY id ASC;");
    assert(mig_q.has_value());
    assert(mig_q->rows.size() == 2);
    assert(std::get<int64_t>(mig_q->rows[0][0]) == 1);
    assert(std::get<std::string>(mig_q->rows[0][1]) == "__init__");
    assert(std::get<int64_t>(mig_q->rows[1][0]) == 2);
    assert(std::get<std::string>(mig_q->rows[1][1]) == "__update__");

    // Check registry.Get returns v2
    auto cached = registry.Get("items_mod");
    assert(cached.has_value());
    assert(cached->version == 2);
    assert(cached->exported_funcs == expected_funcs);

    std::println("--> TestHotReloadUpdate PASSED.");
}

// -----------------------------------------------------------------------------
// Test 3: Rollback on failing __init__ (non-zero return)
// -----------------------------------------------------------------------------
void TestRollbackOnFailingInit() {
    std::println("--> Running TestRollbackOnFailingInit...");

    auto sql_res = SqlEngine::Open();
    assert(sql_res.has_value());
    auto sql = std::move(sql_res.value());

    auto cat_init = SystemCatalog::Initialize(*sql);
    assert(cat_init.has_value());

    auto runtime_res = WasmRuntime::Create();
    assert(runtime_res.has_value());
    auto runtime = std::move(runtime_res.value());

    auto reg_res = RegisterHostAbi(runtime->Linker());
    assert(reg_res.has_value());

    ModuleRegistry registry(*runtime);

    const char* fail_code = R"(
#include "whisk_guest.hpp"

WHISK_INIT() {
    whisk::Exec("CREATE TABLE doomed (id INTEGER PRIMARY KEY);");
    whisk::Exec("INSERT INTO doomed (id) VALUES (42);");
    return whisk::Error("init failed");
}

WHISK_FUNC(ping) {
    return whisk::Ok("pong");
}
)";

    auto wasm_res = CompileWhiskSourceToWasm(fail_code, kWorkDir, "fail_init");
    assert(wasm_res.has_value());

    auto pub_res = registry.Publish(*sql, "fail_mod", wasm_res.value());
    assert(!pub_res.has_value());
    assert(pub_res.error().Code() == ErrorCode::WasmError);
    assert(pub_res.error().Message().find("init failed") != std::string::npos);

    // Verify table/row rolled back
    auto q_res = sql->Execute("SELECT * FROM doomed;");
    assert(!q_res.has_value()); // table should not exist

    // Verify not in catalog
    auto cat_res = SystemCatalog::GetModule(*sql, "fail_mod");
    assert(cat_res.has_value() && !cat_res->has_value());

    // Verify not in registry
    assert(!registry.Get("fail_mod").has_value());
    assert(registry.Size() == 0);

    std::println("--> TestRollbackOnFailingInit PASSED.");
}

// -----------------------------------------------------------------------------
// Test 4: Rollback on trapping __update__ (__builtin_trap())
// -----------------------------------------------------------------------------
void TestRollbackOnTrappingUpdate() {
    std::println("--> Running TestRollbackOnTrappingUpdate...");

    auto sql_res = SqlEngine::Open();
    assert(sql_res.has_value());
    auto sql = std::move(sql_res.value());

    auto cat_init = SystemCatalog::Initialize(*sql);
    assert(cat_init.has_value());

    auto runtime_res = WasmRuntime::Create();
    assert(runtime_res.has_value());
    auto runtime = std::move(runtime_res.value());

    auto reg_res = RegisterHostAbi(runtime->Linker());
    assert(reg_res.has_value());

    ModuleRegistry registry(*runtime);

    // Deploy v1 successfully
    const char* v1_code = R"(
#include "whisk_guest.hpp"

WHISK_INIT() {
    whisk::Exec("CREATE TABLE robust (val INTEGER);");
    whisk::Exec("INSERT INTO robust (val) VALUES (100);");
    return 0;
}

WHISK_FUNC(ping) {
    return whisk::Ok("pong");
}
)";
    auto wasm1_res = CompileWhiskSourceToWasm(v1_code, kWorkDir, "robust_v1");
    assert(wasm1_res.has_value());
    auto pub1 = registry.Publish(*sql, "robust_mod", wasm1_res.value());
    assert(pub1.has_value());
    assert(pub1->module.version == 1);

    // Upgrade v2 with trapping __update__
    const char* v2_trap_code = R"(
#include "whisk_guest.hpp"

WHISK_UPDATE() {
    whisk::Exec("INSERT INTO robust (val) VALUES (999);");
    __builtin_trap();
    return 0;
}

WHISK_FUNC(ping) {
    return whisk::Ok("pong");
}
)";
    auto wasm2_res = CompileWhiskSourceToWasm(v2_trap_code, kWorkDir, "robust_v2");
    assert(wasm2_res.has_value());

    auto pub2 = registry.Publish(*sql, "robust_mod", wasm2_res.value());
    assert(!pub2.has_value());
    assert(pub2.error().Code() == ErrorCode::WasmTrap);

    // Verify row 999 is rolled back; row 100 remains
    auto q_res = sql->Execute("SELECT val FROM robust;");
    assert(q_res.has_value());
    assert(q_res->rows.size() == 1);
    assert(std::get<int64_t>(q_res->rows[0][0]) == 100);

    // Verify catalog and registry still have v1 intact
    auto cat_res = SystemCatalog::GetModule(*sql, "robust_mod");
    assert(cat_res.has_value() && cat_res->has_value());
    assert((*cat_res)->version == 1);

    auto cached = registry.Get("robust_mod");
    assert(cached.has_value());
    assert(cached->version == 1);

    std::println("--> TestRollbackOnTrappingUpdate PASSED.");
}

// -----------------------------------------------------------------------------
// Test 5: Invalid WASM / missing ABI / invalid module name
// -----------------------------------------------------------------------------
void TestInvalidInputsAndAbi() {
    std::println("--> Running TestInvalidInputsAndAbi...");

    auto sql_res = SqlEngine::Open();
    assert(sql_res.has_value());
    auto sql = std::move(sql_res.value());

    auto cat_init = SystemCatalog::Initialize(*sql);
    assert(cat_init.has_value());

    auto runtime_res = WasmRuntime::Create();
    assert(runtime_res.has_value());
    auto runtime = std::move(runtime_res.value());

    ModuleRegistry registry(*runtime);

    const uint8_t dummy_wasm[] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};

    // 1. Empty name
    auto r1 = registry.Publish(*sql, "", dummy_wasm);
    assert(!r1.has_value() && r1.error().Code() == ErrorCode::InvalidArgument);

    // 2. Bad characters
    auto r2 = registry.Publish(*sql, "bad/name", dummy_wasm);
    assert(!r2.has_value() && r2.error().Code() == ErrorCode::InvalidArgument);

    auto r2b = registry.Publish(*sql, "bad name", dummy_wasm);
    assert(!r2b.has_value() && r2b.error().Code() == ErrorCode::InvalidArgument);

    // 3. Garbage bytes
    const uint8_t garbage[] = {0xde, 0xad, 0xbe, 0xef};
    auto r3 = registry.Publish(*sql, "valid_name", garbage);
    assert(!r3.has_value() && r3.error().Code() == ErrorCode::WasmError);

    // 4. Missing required ABI export (whisk_alloc)
    const char* wat_no_alloc = R"(
(module
    (memory (export "memory") 1)
    (func (export "whisk_result_ptr") (result i32) (i32.const 0))
    (func (export "whisk_result_len") (result i32) (i32.const 0))
)
)";
    auto wasm_no_alloc = WatToWasm(wat_no_alloc);
    auto r4 = registry.Publish(*sql, "mod_no_alloc", wasm_no_alloc);
    assert(!r4.has_value() && r4.error().Code() == ErrorCode::WasmError);
    assert(r4.error().Message().find("whisk_alloc") != std::string::npos);

    // 5. Missing required ABI export (memory)
    const char* wat_no_mem = R"(
(module
    (func (export "whisk_alloc") (param i32) (result i32) (i32.const 0))
    (func (export "whisk_result_ptr") (result i32) (i32.const 0))
    (func (export "whisk_result_len") (result i32) (i32.const 0))
)
)";
    auto wasm_no_mem = WatToWasm(wat_no_mem);
    auto r5 = registry.Publish(*sql, "mod_no_mem", wasm_no_mem);
    assert(!r5.has_value() && r5.error().Code() == ErrorCode::WasmError);
    assert(r5.error().Message().find("memory") != std::string::npos);

    std::println("--> TestInvalidInputsAndAbi PASSED.");
}

// -----------------------------------------------------------------------------
// Test 6: Catalog re-hydration (LoadFromCatalog)
// -----------------------------------------------------------------------------
void TestLoadFromCatalog() {
    std::println("--> Running TestLoadFromCatalog...");

    auto sql_res = SqlEngine::Open();
    assert(sql_res.has_value());
    auto sql = std::move(sql_res.value());

    auto cat_init = SystemCatalog::Initialize(*sql);
    assert(cat_init.has_value());

    auto runtime_res = WasmRuntime::Create();
    assert(runtime_res.has_value());
    auto runtime = std::move(runtime_res.value());

    auto reg_res = RegisterHostAbi(runtime->Linker());
    assert(reg_res.has_value());

    ModuleRegistry registry1(*runtime);

    const char* mod_code = R"(
#include "whisk_guest.hpp"

WHISK_INIT() {
    return 0;
}

WHISK_FUNC(hello) {
    return whisk::Ok("world");
}
)";
    auto wasm_res = CompileWhiskSourceToWasm(mod_code, kWorkDir, "load_catalog");
    assert(wasm_res.has_value());

    auto pub_res = registry1.Publish(*sql, "mod_hello", wasm_res.value());
    assert(pub_res.has_value());

    // Create fresh ModuleRegistry against the same SqlEngine
    ModuleRegistry registry2(*runtime);
    assert(registry2.Size() == 0);

    auto load_res = registry2.LoadFromCatalog(*sql);
    assert(load_res.has_value());
    assert(load_res.value() == 1);
    assert(registry2.Size() == 1);

    auto cached = registry2.Get("mod_hello");
    assert(cached.has_value());
    assert(cached->name == "mod_hello");
    assert(cached->version == 1);
    assert(cached->exported_funcs == std::vector<std::string>{"hello"});
    assert(cached->compiled != nullptr);

    // Verify it can be instantiated immediately
    StoreHandle store;
    auto store_res = runtime->CreateStore();
    assert(store_res.has_value());
    store = std::move(store_res.value());

    wasmtime_instance_t inst{};
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err = wasmtime_linker_instantiate(
        runtime->Linker(), store.Context(), cached->compiled.get(), &inst, &trap);
    assert(err == nullptr);
    assert(trap == nullptr);

    // List() sorted check
    auto list = registry2.List();
    assert(list.size() == 1);
    assert(list[0].name == "mod_hello");

    // Remove check
    assert(registry2.Remove("mod_hello"));
    assert(!registry2.Get("mod_hello").has_value());
    assert(registry2.Size() == 0);

    std::println("--> TestLoadFromCatalog PASSED.");
}

} // namespace

int main() {
    std::println("=== Running test_wasm_lifecycle ===");
    TestFirstDeployInit();
    TestHotReloadUpdate();
    TestRollbackOnFailingInit();
    TestRollbackOnTrappingUpdate();
    TestInvalidInputsAndAbi();
    TestLoadFromCatalog();
    std::println("=== All test_wasm_lifecycle tests PASSED! ===");
    return 0;
}
