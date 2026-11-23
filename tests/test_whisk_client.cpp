#include "client/whisk_client.hpp"
#include "engine.hpp"
#include "errors.hpp"
#include "wasm_test_helper.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <format>
#include <print>
#include <string>
#include <unistd.h>
#include <vector>

using namespace database;

namespace {

std::filesystem::path MakeTempDir(std::string_view tag) {
    auto dir = std::filesystem::temp_directory_path() /
               std::format("whiskdb_client_test_{}_{}", static_cast<int>(::getpid()), tag);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::filesystem::path FindCounterExamplePath() {
#ifdef WHISKDB_SOURCE_DIR
    std::filesystem::path p = std::filesystem::path(WHISKDB_SOURCE_DIR) / "examples" / "counter_whisk.cpp";
    if (std::filesystem::exists(p)) {
        return p;
    }
#endif
    if (std::filesystem::exists("examples/counter_whisk.cpp")) {
        return std::filesystem::absolute("examples/counter_whisk.cpp");
    }
    if (std::filesystem::exists("../examples/counter_whisk.cpp")) {
        return std::filesystem::absolute("../examples/counter_whisk.cpp");
    }
    return "examples/counter_whisk.cpp";
}

bool ContainsFunc(const std::vector<std::string>& funcs, std::string_view target) {
    return std::find(funcs.begin(), funcs.end(), target) != funcs.end();
}

void TestWhiskClientEndToEnd() {
    std::println("--- Testing WhiskClient End-to-End over Live TCP ---");
    auto temp_dir = MakeTempDir("e2e");
    auto data_dir = temp_dir / "data";
    auto wasm_path = temp_dir / "counter_whisk.wasm";

    // Compile examples/counter_whisk.cpp to WASM
    auto compile_res = test_util::CompileWhiskCppToWasm(FindCounterExamplePath(), wasm_path);
    if (!compile_res) {
        std::println("Compile failed: {}", compile_res.error().ToString());
    }
    assert(compile_res.has_value());
    const std::vector<uint8_t>& wasm_bytes = *compile_res;
    assert(!wasm_bytes.empty());

    // Start in-process Engine with live TCP server
    EngineOptions opts;
    opts.data_dir = data_dir;
    opts.durability = DurabilityMode::GroupCommit;
    opts.host = "127.0.0.1";
    opts.port = 0;

    auto eng_res = Engine::Open(opts);
    assert(eng_res.has_value());
    auto& engine = *eng_res.value();
    assert(engine.StartServer().has_value());
    const uint16_t port = engine.Port();
    assert(port > 0);

    // 1. Connect, Ping, ListModules (initially empty)
    WhiskClient client;
    assert(!client.IsConnected());
    assert(client.Connect("127.0.0.1", port).has_value());
    assert(client.IsConnected());

    auto ping_res = client.Ping();
    assert(ping_res.has_value());

    auto list_empty = client.ListModules();
    assert(list_empty.has_value());
    assert(list_empty->modules.empty());

    // 2. PublishModuleFromFile("counter", wasm_path)
    auto pub1 = client.PublishModuleFromFile("counter", wasm_path);
    if (!pub1) {
        std::println("PublishModuleFromFile failed: {}", pub1.error().ToString());
    }
    assert(pub1.has_value());
    assert(pub1->module_name == "counter");
    assert(pub1->version == 1);
    assert(pub1->sha256.size() == 64);
    assert(ContainsFunc(pub1->exported_funcs, "increment"));
    assert(ContainsFunc(pub1->exported_funcs, "get"));

    // 3. CallWhisk("counter", "increment", ...) and verify via get + SQL Query
    auto call1 = client.CallWhisk("counter", "increment", R"({"name":"clicks","delta":7})");
    if (!call1) {
        std::println("CallWhisk increment #1 failed: {}", call1.error().ToString());
    }
    assert(call1.has_value());
    assert(call1->result_json.find("\"val\":7") != std::string::npos);
    assert(call1->fuel_used > 0);

    auto call2 = client.CallWhisk("counter", "increment", R"({"name":"clicks","delta":7})");
    assert(call2.has_value());
    assert(call2->result_json.find("\"val\":14") != std::string::npos);

    auto get_res = client.CallWhisk("counter", "get", R"({"name":"clicks"})");
    assert(get_res.has_value());
    assert(get_res->result_json.find("\"val\":14") != std::string::npos);

    auto sql_res = client.Query("SELECT val FROM counters WHERE name = 'clicks'");
    assert(sql_res.has_value());
    assert(sql_res->rows.size() == 1);
    assert(std::get<int64_t>(sql_res->rows[0][0]) == 14);

    // 4. ListModules() -> 1 module "counter" v1 with updated_at > 0
    auto list_one = client.ListModules();
    assert(list_one.has_value());
    assert(list_one->modules.size() == 1);
    assert(list_one->modules[0].name == "counter");
    assert(list_one->modules[0].version == 1);
    assert(list_one->modules[0].sha256 == pub1->sha256);
    assert(list_one->modules[0].updated_at > 0);

    // 5. PublishModule("counter", wasm_bytes) (re-publish) -> version == 2
    auto pub2 = client.PublishModule("counter", wasm_bytes);
    assert(pub2.has_value());
    assert(pub2->module_name == "counter");
    assert(pub2->version == 2);
    assert(pub2->sha256 == pub1->sha256);

    // 6. GetLogs(5) and GetLogs() -> verify log lines contain [whisk:counter]
    auto logs_limited = client.GetLogs(5);
    assert(logs_limited.has_value());
    assert(logs_limited->find("[whisk:counter]") != std::string::npos);

    auto logs_default = client.GetLogs();
    assert(logs_default.has_value());
    assert(logs_default->find("[whisk:counter]") != std::string::npos);

    // 7. Error paths over TCP
    // 7a. PublishModuleFromFile on missing file -> ErrorCode::NotFound
    auto err_missing = client.PublishModuleFromFile("missing", temp_dir / "nonexistent.wasm");
    assert(!err_missing.has_value());
    assert(err_missing.error().Code() == ErrorCode::NotFound);

    // 7b. PublishModule with invalid WASM bytes -> ErrorCode::WasmError
    const std::vector<uint8_t> bad_wasm = {1, 2, 3, 4};
    auto err_bad_wasm = client.PublishModule("badmod", bad_wasm);
    assert(!err_bad_wasm.has_value());
    assert(err_bad_wasm.error().Code() == ErrorCode::WasmError);

    // 7c. CallWhisk on unknown module -> ErrorCode::NotFound
    auto err_unknown_mod = client.CallWhisk("no_such_module", "increment", "{}");
    assert(!err_unknown_mod.has_value());
    assert(err_unknown_mod.error().Code() == ErrorCode::NotFound);

    // 7d. CallWhisk on unknown function -> ErrorCode::NotFound
    auto err_unknown_fn = client.CallWhisk("counter", "no_such_func", "{}");
    assert(!err_unknown_fn.has_value());
    assert(err_unknown_fn.error().Code() == ErrorCode::NotFound);

    // 7e. CallWhisk("counter", "get", R"({"name":"does_not_exist"})") -> ErrorCode::WasmError with "counter not found"
    auto err_guest = client.CallWhisk("counter", "get", R"({"name":"does_not_exist"})");
    assert(!err_guest.has_value());
    assert(err_guest.error().Code() == ErrorCode::WasmError);
    assert(err_guest.error().Message().find("counter not found") != std::string::npos);

    // 7f. Disconnect() / IsConnected() == false
    client.Disconnect();
    assert(!client.IsConnected());

    engine.StopServer();

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
}

} // namespace

int main() {
    TestWhiskClientEndToEnd();
    std::println("All WhiskClient tests passed!");
    return 0;
}
