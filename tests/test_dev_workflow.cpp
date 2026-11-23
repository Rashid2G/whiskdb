#include "cli/dev_workflow.hpp"
#include "errors.hpp"

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

using namespace database;

namespace {

struct ScopedTempDir {
    std::filesystem::path path;

    explicit ScopedTempDir(const std::string& prefix) {
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
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

int TestInitBuildTestEndToEnd() {
    ScopedTempDir tmp("whisk_e2e");
    std::filesystem::path proj = tmp.path / "demo_app";

    auto init_res = InitProject(proj, false);
    TEST_ASSERT(init_res.has_value());
    TEST_ASSERT(init_res->module_name == "demo_app");
    TEST_ASSERT(std::filesystem::exists(proj / "whisk.toml"));
    TEST_ASSERT(std::filesystem::exists(proj / "src" / "module.cpp"));

    auto build_res = BuildProject(proj);
    TEST_ASSERT(build_res.has_value());
    TEST_ASSERT(build_res->module_name == "demo_app");
    TEST_ASSERT(build_res->binary_size > 0);
    TEST_ASSERT(std::filesystem::exists(build_res->output_path));

    auto test_res = TestProject(proj);
    TEST_ASSERT(test_res.has_value());
    TEST_ASSERT(test_res->module_name == "demo_app");
    TEST_ASSERT(test_res->total == 1);
    TEST_ASSERT(test_res->passed == 1);
    TEST_ASSERT(test_res->failed == 0);
    TEST_ASSERT(test_res->results.size() == 1);
    TEST_ASSERT(test_res->results[0].name == "test_smoke");
    TEST_ASSERT(test_res->results[0].passed);

    std::cout << "[PASS] TestInitBuildTestEndToEnd\n";
    return 0;
}

int TestInitAlreadyExistsAndForce() {
    ScopedTempDir tmp("whisk_init_force");
    std::filesystem::path proj = tmp.path / "dup_app";

    auto first = InitProject(proj, false);
    TEST_ASSERT(first.has_value());

    auto second = InitProject(proj, false);
    TEST_ASSERT(!second.has_value());
    TEST_ASSERT(second.error().Code() == ErrorCode::AlreadyExists);

    auto forced = InitProject(proj, true);
    TEST_ASSERT(forced.has_value());

    std::cout << "[PASS] TestInitAlreadyExistsAndForce\n";
    return 0;
}

int TestFailingAndTrappingTests() {
    ScopedTempDir tmp("whisk_fail_test");
    std::filesystem::path proj = tmp.path / "fail_app";

    auto init_res = InitProject(proj, false);
    TEST_ASSERT(init_res.has_value());

    // Overwrite src/module.cpp with one passing test, one error-returning test, and one trapping test
    std::ofstream out(proj / "src" / "module.cpp", std::ios::trunc);
    TEST_ASSERT(out.is_open());
    out << R"cpp(#include "whisk_guest.hpp"

WHISK_INIT() {
    return whisk::Ok("{}");
}

WHISK_TEST(passes) {
    return whisk::Ok("{\"ok\":true}");
}

WHISK_TEST(returns_error) {
    return whisk::Error("intentional failure");
}

WHISK_TEST(traps) {
    __builtin_trap();
}
)cpp";
    out.close();

    auto test_res = TestProject(proj);
    TEST_ASSERT(test_res.has_value());
    TEST_ASSERT(test_res->total == 3);
    TEST_ASSERT(test_res->passed == 1);
    TEST_ASSERT(test_res->failed == 2);
    TEST_ASSERT(test_res->passed < test_res->total);

    std::cout << "[PASS] TestFailingAndTrappingTests\n";
    return 0;
}

int TestMissingManifestReturnsNotFound() {
    ScopedTempDir tmp("whisk_missing_manifest");
    std::filesystem::path empty_dir = tmp.path / "no_manifest";
    std::filesystem::create_directories(empty_dir);

    auto build_res = BuildProject(empty_dir);
    TEST_ASSERT(!build_res.has_value());
    TEST_ASSERT(build_res.error().Code() == ErrorCode::NotFound);

    auto test_res = TestProject(empty_dir);
    TEST_ASSERT(!test_res.has_value());
    TEST_ASSERT(test_res.error().Code() == ErrorCode::NotFound);

    std::cout << "[PASS] TestMissingManifestReturnsNotFound\n";
    return 0;
}

} // namespace

int main() {
    if (TestInitBuildTestEndToEnd() != 0) {
        return 1;
    }
    if (TestInitAlreadyExistsAndForce() != 0) {
        return 1;
    }
    if (TestFailingAndTrappingTests() != 0) {
        return 1;
    }
    if (TestMissingManifestReturnsNotFound() != 0) {
        return 1;
    }
    return 0;
}
