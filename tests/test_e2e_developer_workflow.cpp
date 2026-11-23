#include "cli/dev_workflow.hpp"
#include "client/whisk_client.hpp"
#include "engine.hpp"
#include "errors.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <variant>
#include <vector>

namespace {

using database::DurabilityMode;
using database::Engine;
using database::EngineOptions;
using database::WhiskClient;

struct ScopedTempDir {
    std::filesystem::path path;

    explicit ScopedTempDir(const std::string& prefix) {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               std::format("{}_{}_{}", prefix, static_cast<uint64_t>(::getpid()), static_cast<uint64_t>(now));
        std::filesystem::create_directories(path);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

#define TEST_ASSERT(cond)                                                                      \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            std::cerr << "FAIL: " << #cond << " at " << __FILE__ << ":" << __LINE__ << "\n";   \
            return 1;                                                                          \
        }                                                                                      \
    } while (false)

bool ContainsFunc(const std::vector<std::string>& funcs, std::string_view target) {
    return std::find(funcs.begin(), funcs.end(), target) != funcs.end();
}

bool WriteTextFile(const std::filesystem::path& file_path, std::string_view content) {
    std::ofstream out(file_path, std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    out << content;
    return out.good();
}

constexpr std::string_view kModuleV1Source = R"cpp(#include "whisk_guest.hpp"

WHISK_INIT() {
    int64_t rc = whisk::Exec(
        "CREATE TABLE IF NOT EXISTS tasks ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "title TEXT NOT NULL, "
        "done INTEGER NOT NULL DEFAULT 0);");
    if (rc < 0) {
        return whisk::Error("failed to create tasks table");
    }
    whisk::LogInfo("todo_app v1 initialized");
    return whisk::Ok("{\"initialized\":true}");
}

WHISK_FUNC(add_task) {
    whisk::StrView args = WHISK_ARGS();
    whisk::StrView title_slice;
    whisk::StrView title = "untitled";
    if (whisk::JsonGetStr(args, "title", title_slice) && !title_slice.empty()) {
        title = title_slice;
    }

    whisk::Buf<256> params;
    params.append_char('[');
    params.append_json_str(title);
    params.append_char(']');

    if (whisk::Exec("INSERT INTO tasks (title, done) VALUES (?1, 0);", params.view()) < 0) {
        return whisk::Error("failed to insert task");
    }

    whisk::LogInfo("added task");
    return whisk::Ok("{\"status\":\"added\"}");
}

WHISK_FUNC(complete_task) {
    whisk::StrView args = WHISK_ARGS();
    int64_t id = whisk::JsonGetIntOr(args, "id", 0);

    whisk::Buf<64> params;
    params.append_char('[');
    params.append_int(id);
    params.append_char(']');

    if (whisk::Exec("UPDATE tasks SET done = 1 WHERE id = ?1;", params.view()) < 0) {
        return whisk::Error("failed to complete task");
    }

    whisk::LogInfo("completed task");
    return whisk::Ok("{\"status\":\"completed\"}");
}

WHISK_TEST(add_and_complete) {
    whisk::Buf<128> insert_params;
    insert_params.append_char('[');
    insert_params.append_json_str("local test task");
    insert_params.append_char(']');

    if (whisk::Exec("INSERT INTO tasks (title, done) VALUES (?1, 0);", insert_params.view()) < 0) {
        return whisk::Error("local insert failed");
    }

    whisk::StrView q_res = whisk::Query("SELECT COUNT(*) FROM tasks;");
    int64_t count = 0;
    if (q_res.empty() || !whisk::QueryHasRows(q_res) || !whisk::QueryFirstInt(q_res, count) || count != 1) {
        return whisk::Error("expected 1 task row");
    }

    return whisk::Ok("{\"ok\":true}");
}
)cpp";

constexpr std::string_view kModuleV2Source = R"cpp(#include "whisk_guest.hpp"

WHISK_INIT() {
    int64_t rc = whisk::Exec(
        "CREATE TABLE IF NOT EXISTS tasks ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "title TEXT NOT NULL, "
        "done INTEGER NOT NULL DEFAULT 0, "
        "priority INTEGER NOT NULL DEFAULT 1);");
    if (rc < 0) {
        return whisk::Error("failed to create tasks table");
    }
    whisk::LogInfo("todo_app v2 initialized");
    return whisk::Ok("{\"initialized\":true}");
}

WHISK_UPDATE() {
    int64_t rc = whisk::Exec("ALTER TABLE tasks ADD COLUMN priority INTEGER NOT NULL DEFAULT 1;");
    if (rc < 0) {
        return whisk::Error("failed to migrate tasks table");
    }
    whisk::LogInfo("todo_app v2 migrated");
    return whisk::Ok("{\"migrated\":true}");
}

WHISK_FUNC(add_task) {
    whisk::StrView args = WHISK_ARGS();
    whisk::StrView title_slice;
    whisk::StrView title = "untitled";
    if (whisk::JsonGetStr(args, "title", title_slice) && !title_slice.empty()) {
        title = title_slice;
    }

    whisk::Buf<256> params;
    params.append_char('[');
    params.append_json_str(title);
    params.append_char(']');

    if (whisk::Exec("INSERT INTO tasks (title, done) VALUES (?1, 0);", params.view()) < 0) {
        return whisk::Error("failed to insert task");
    }

    whisk::LogInfo("added task");
    return whisk::Ok("{\"status\":\"added\"}");
}

WHISK_FUNC(complete_task) {
    whisk::StrView args = WHISK_ARGS();
    int64_t id = whisk::JsonGetIntOr(args, "id", 0);

    whisk::Buf<64> params;
    params.append_char('[');
    params.append_int(id);
    params.append_char(']');

    if (whisk::Exec("UPDATE tasks SET done = 1 WHERE id = ?1;", params.view()) < 0) {
        return whisk::Error("failed to complete task");
    }

    whisk::LogInfo("completed task");
    return whisk::Ok("{\"status\":\"completed\"}");
}

WHISK_FUNC(set_priority) {
    whisk::StrView args = WHISK_ARGS();
    int64_t id = whisk::JsonGetIntOr(args, "id", 0);
    int64_t priority = whisk::JsonGetIntOr(args, "priority", 1);

    whisk::Buf<128> params;
    params.append_char('[');
    params.append_int(priority);
    params.append_char(',');
    params.append_int(id);
    params.append_char(']');

    if (whisk::Exec("UPDATE tasks SET priority = ?1 WHERE id = ?2;", params.view()) < 0) {
        return whisk::Error("failed to update priority");
    }

    whisk::LogInfo("updated priority");
    return whisk::Ok("{\"status\":\"priority_updated\"}");
}
)cpp";

int TestFullDeveloperWorkflow() {
    ScopedTempDir tmp("whisk_e2e_dev_journey");
    const std::filesystem::path proj_dir = tmp.path / "todo_app";
    const std::filesystem::path data_dir = tmp.path / "data";

    // Stage 1: Scaffold (InitProject)
    auto init_res = database::InitProject(proj_dir, false);
    if (!init_res.has_value()) {
        std::cerr << "InitProject failed: " << init_res.error().ToString() << "\n";
    }
    TEST_ASSERT(init_res.has_value());
    TEST_ASSERT(init_res->module_name == "todo_app");
    TEST_ASSERT(std::filesystem::exists(proj_dir / "whisk.toml"));
    TEST_ASSERT(std::filesystem::exists(proj_dir / "src" / "module.cpp"));

    // Stage 2: Build scaffolded template, then customize v1 and compile (BuildProject)
    auto template_build = database::BuildProject(proj_dir);
    if (!template_build.has_value()) {
        std::cerr << "Template BuildProject failed: " << template_build.error().ToString() << "\n";
    }
    TEST_ASSERT(template_build.has_value());
    TEST_ASSERT(template_build->binary_size > 0);
    TEST_ASSERT(std::filesystem::exists(template_build->output_path));

    TEST_ASSERT(WriteTextFile(proj_dir / "src" / "module.cpp", kModuleV1Source));

    auto build_v1 = database::BuildProject(proj_dir);
    if (!build_v1.has_value()) {
        std::cerr << "v1 BuildProject failed: " << build_v1.error().ToString() << "\n";
    }
    TEST_ASSERT(build_v1.has_value());
    TEST_ASSERT(build_v1->module_name == "todo_app");
    TEST_ASSERT(build_v1->binary_size > 0);
    TEST_ASSERT(std::filesystem::exists(build_v1->output_path));

    // Stage 3: Local Test (TestProject in embedded MemoryOnly mode)
    auto test_res = database::TestProject(proj_dir);
    if (!test_res.has_value()) {
        std::cerr << "TestProject failed: " << test_res.error().ToString() << "\n";
    }
    TEST_ASSERT(test_res.has_value());
    TEST_ASSERT(test_res->module_name == "todo_app");
    TEST_ASSERT(test_res->total == 1);
    TEST_ASSERT(test_res->passed == 1);
    TEST_ASSERT(test_res->failed == 0);
    TEST_ASSERT(test_res->results.size() == 1);
    TEST_ASSERT(test_res->results[0].name == "test_add_and_complete");
    TEST_ASSERT(test_res->results[0].passed);

    // Stage 4: Live Server Deploy (PublishModuleFromFile over TCP)
    EngineOptions opts;
    opts.data_dir = data_dir;
    opts.durability = DurabilityMode::GroupCommit;
    opts.host = "127.0.0.1";
    opts.port = 0;

    auto eng_res = Engine::Open(opts);
    if (!eng_res.has_value()) {
        std::cerr << "Engine::Open #1 failed: " << eng_res.error().ToString() << "\n";
    }
    TEST_ASSERT(eng_res.has_value());
    auto engine = std::move(*eng_res);
    TEST_ASSERT(engine->StartServer().has_value());
    const uint16_t port1 = engine->Port();
    TEST_ASSERT(port1 > 0);

    WhiskClient client;
    TEST_ASSERT(client.Connect("127.0.0.1", port1).has_value());
    TEST_ASSERT(client.IsConnected());

    auto pub_v1 = client.PublishModuleFromFile("todo_app", build_v1->output_path);
    if (!pub_v1.has_value()) {
        std::cerr << "PublishModuleFromFile v1 failed: " << pub_v1.error().ToString() << "\n";
    }
    TEST_ASSERT(pub_v1.has_value());
    TEST_ASSERT(pub_v1->module_name == "todo_app");
    TEST_ASSERT(pub_v1->version == 1);
    TEST_ASSERT(pub_v1->sha256.size() == 64);
    TEST_ASSERT(ContainsFunc(pub_v1->exported_funcs, "add_task"));
    TEST_ASSERT(ContainsFunc(pub_v1->exported_funcs, "complete_task"));

    // Verify __init__ created the tasks table
    auto init_check = client.Query("SELECT COUNT(*) FROM tasks");
    TEST_ASSERT(init_check.has_value());
    TEST_ASSERT(init_check->rows.size() == 1);
    TEST_ASSERT(std::get<int64_t>(init_check->rows[0][0]) == 0);

    // Stage 5: Invoke & Query (CallWhisk, Query, ListModules, GetLogs)
    auto add1 = client.CallWhisk("todo_app", "add_task", R"({"title":"write e2e test"})");
    if (!add1.has_value()) {
        std::cerr << "add_task #1 failed: " << add1.error().ToString() << "\n";
    }
    TEST_ASSERT(add1.has_value());
    TEST_ASSERT(add1->result_json.find("\"added\"") != std::string::npos);

    auto add2 = client.CallWhisk("todo_app", "add_task", R"({"title":"ship v1"})");
    TEST_ASSERT(add2.has_value());

    auto complete1 = client.CallWhisk("todo_app", "complete_task", R"({"id":1})");
    TEST_ASSERT(complete1.has_value());
    TEST_ASSERT(complete1->result_json.find("\"completed\"") != std::string::npos);

    auto rows_v1 = client.Query("SELECT id, title, done FROM tasks ORDER BY id");
    TEST_ASSERT(rows_v1.has_value());
    TEST_ASSERT(rows_v1->rows.size() == 2);
    TEST_ASSERT(std::get<int64_t>(rows_v1->rows[0][0]) == 1);
    TEST_ASSERT(std::get<std::string>(rows_v1->rows[0][1]) == "write e2e test");
    TEST_ASSERT(std::get<int64_t>(rows_v1->rows[0][2]) == 1);
    TEST_ASSERT(std::get<int64_t>(rows_v1->rows[1][0]) == 2);
    TEST_ASSERT(std::get<std::string>(rows_v1->rows[1][1]) == "ship v1");
    TEST_ASSERT(std::get<int64_t>(rows_v1->rows[1][2]) == 0);

    auto list_v1 = client.ListModules();
    TEST_ASSERT(list_v1.has_value());
    TEST_ASSERT(list_v1->modules.size() == 1);
    TEST_ASSERT(list_v1->modules[0].name == "todo_app");
    TEST_ASSERT(list_v1->modules[0].version == 1);
    TEST_ASSERT(list_v1->modules[0].sha256 == pub_v1->sha256);

    auto logs = client.GetLogs(50);
    TEST_ASSERT(logs.has_value());
    TEST_ASSERT(logs->find("[whisk:todo_app]") != std::string::npos);

    // Stage 6: Hot-Reload (__update__)
    TEST_ASSERT(WriteTextFile(proj_dir / "src" / "module.cpp", kModuleV2Source));

    auto build_v2 = database::BuildProject(proj_dir);
    if (!build_v2.has_value()) {
        std::cerr << "v2 BuildProject failed: " << build_v2.error().ToString() << "\n";
    }
    TEST_ASSERT(build_v2.has_value());
    TEST_ASSERT(build_v2->binary_size > 0);

    auto pub_v2 = client.PublishModuleFromFile("todo_app", build_v2->output_path);
    if (!pub_v2.has_value()) {
        std::cerr << "PublishModuleFromFile v2 failed: " << pub_v2.error().ToString() << "\n";
    }
    TEST_ASSERT(pub_v2.has_value());
    TEST_ASSERT(pub_v2->module_name == "todo_app");
    TEST_ASSERT(pub_v2->version == 2);
    TEST_ASSERT(pub_v2->sha256 != pub_v1->sha256);
    TEST_ASSERT(ContainsFunc(pub_v2->exported_funcs, "set_priority"));

    // Verify __update__ added the priority column and new v2 function works
    auto prio2 = client.CallWhisk("todo_app", "set_priority", R"({"id":2,"priority":5})");
    if (!prio2.has_value()) {
        std::cerr << "set_priority failed: " << prio2.error().ToString() << "\n";
    }
    TEST_ASSERT(prio2.has_value());

    auto prio_check = client.Query("SELECT priority FROM tasks WHERE id = 2");
    TEST_ASSERT(prio_check.has_value());
    TEST_ASSERT(prio_check->rows.size() == 1);
    TEST_ASSERT(std::get<int64_t>(prio_check->rows[0][0]) == 5);

    // Stage 7: Snapshot & Restart Durability
    auto snap_res = client.Snapshot();
    if (!snap_res.has_value()) {
        std::cerr << "Snapshot failed: " << snap_res.error().ToString() << "\n";
    }
    TEST_ASSERT(snap_res.has_value());

    // Execute post-snapshot mutations via Whisk
    auto add3 = client.CallWhisk("todo_app", "add_task", R"({"title":"post-snapshot task"})");
    TEST_ASSERT(add3.has_value());
    auto prio3 = client.CallWhisk("todo_app", "set_priority", R"({"id":3,"priority":9})");
    TEST_ASSERT(prio3.has_value());

    // Cleanly disconnect client and shut down engine
    client.Disconnect();
    TEST_ASSERT(!client.IsConnected());
    engine->StopServer();
    engine.reset();

    // Reopen fresh Engine on the SAME data_dir with ephemeral port 0
    auto eng_res2 = Engine::Open(opts);
    if (!eng_res2.has_value()) {
        std::cerr << "Engine::Open #2 failed: " << eng_res2.error().ToString() << "\n";
    }
    TEST_ASSERT(eng_res2.has_value());
    auto engine2 = std::move(*eng_res2);
    TEST_ASSERT(engine2->StartServer().has_value());
    const uint16_t port2 = engine2->Port();
    TEST_ASSERT(port2 > 0);

    WhiskClient client2;
    TEST_ASSERT(client2.Connect("127.0.0.1", port2).has_value());

    // Verify pre-snapshot and post-snapshot SQL rows survived
    auto rows_after = client2.Query("SELECT id, title, done, priority FROM tasks ORDER BY id");
    TEST_ASSERT(rows_after.has_value());
    TEST_ASSERT(rows_after->rows.size() == 3);
    TEST_ASSERT(std::get<int64_t>(rows_after->rows[0][0]) == 1);
    TEST_ASSERT(std::get<std::string>(rows_after->rows[0][1]) == "write e2e test");
    TEST_ASSERT(std::get<int64_t>(rows_after->rows[0][2]) == 1);
    TEST_ASSERT(std::get<int64_t>(rows_after->rows[0][3]) == 1);

    TEST_ASSERT(std::get<int64_t>(rows_after->rows[1][0]) == 2);
    TEST_ASSERT(std::get<std::string>(rows_after->rows[1][1]) == "ship v1");
    TEST_ASSERT(std::get<int64_t>(rows_after->rows[1][2]) == 0);
    TEST_ASSERT(std::get<int64_t>(rows_after->rows[1][3]) == 5);

    TEST_ASSERT(std::get<int64_t>(rows_after->rows[2][0]) == 3);
    TEST_ASSERT(std::get<std::string>(rows_after->rows[2][1]) == "post-snapshot task");
    TEST_ASSERT(std::get<int64_t>(rows_after->rows[2][2]) == 0);
    TEST_ASSERT(std::get<int64_t>(rows_after->rows[2][3]) == 9);

    // Verify v2 module was automatically re-hydrated from _whisk_modules
    auto list_after = client2.ListModules();
    TEST_ASSERT(list_after.has_value());
    TEST_ASSERT(list_after->modules.size() == 1);
    TEST_ASSERT(list_after->modules[0].name == "todo_app");
    TEST_ASSERT(list_after->modules[0].version == 2);
    TEST_ASSERT(list_after->modules[0].sha256 == pub_v2->sha256);

    // Verify v2 function succeeds immediately over TCP without republishing
    auto prio1_after = client2.CallWhisk("todo_app", "set_priority", R"({"id":1,"priority":10})");
    if (!prio1_after.has_value()) {
        std::cerr << "Post-restart set_priority failed: " << prio1_after.error().ToString() << "\n";
    }
    TEST_ASSERT(prio1_after.has_value());

    auto prio1_check = client2.Query("SELECT priority FROM tasks WHERE id = 1");
    TEST_ASSERT(prio1_check.has_value());
    TEST_ASSERT(prio1_check->rows.size() == 1);
    TEST_ASSERT(std::get<int64_t>(prio1_check->rows[0][0]) == 10);

    client2.Disconnect();
    engine2->StopServer();
    engine2.reset();

    std::cout << "[PASS] TestFullDeveloperWorkflow\n";
    return 0;
}

} // namespace

int main() {
    return TestFullDeveloperWorkflow();
}
