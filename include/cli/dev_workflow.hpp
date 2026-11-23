#ifndef WHISKDB_CLI_DEV_WORKFLOW_HPP
#define WHISKDB_CLI_DEV_WORKFLOW_HPP

#include "common/types.hpp"
#include "errors.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace database {

/// Result of scaffolding a new Whisk module project via `whisk init`.
struct InitResult {
    std::filesystem::path project_dir;
    std::string module_name;
    std::filesystem::path manifest_path;
    std::filesystem::path entry_path;
};

/// Result of compiling and validating a Whisk module project via `whisk build`.
struct BuildResult {
    std::string module_name;
    std::filesystem::path entry_path;
    std::filesystem::path output_path;
    idx_t binary_size = 0;
    std::vector<std::string> exported_funcs;
};

/// Execution outcome of a single `WHISK_TEST(...)` function.
struct TestCaseResult {
    std::string name;
    bool passed = false;
    uint64_t exec_us = 0;
    uint64_t fuel_used = 0;
    std::string output;
    std::string error;
};

/// Aggregated summary of `whisk test` execution.
struct TestSummary {
    std::string module_name;
    idx_t total = 0;
    idx_t passed = 0;
    idx_t failed = 0;
    uint64_t total_exec_us = 0;
    std::vector<TestCaseResult> results;
};

/**
 * @brief Scaffold a new local Whisk module project (`whisk.toml` + `src/module.cpp`).
 *
 * Fails with `ErrorCode::AlreadyExists` if `whisk.toml` already exists and `force` is false.
 */
Result<InitResult> InitProject(const std::filesystem::path& project_dir, bool force = false);

/**
 * @brief Compile a local Whisk module project to WebAssembly and validate its exports.
 */
Result<BuildResult> BuildProject(
    const std::filesystem::path& project_dir = ".",
    const std::optional<std::filesystem::path>& sdk_include_override = std::nullopt);

/**
 * @brief Build and run all `test_*` functions in an ephemeral in-memory WhiskDB engine.
 */
Result<TestSummary> TestProject(
    const std::filesystem::path& project_dir = ".",
    const std::optional<std::filesystem::path>& sdk_include_override = std::nullopt);

} // namespace database

namespace Database = database;

#endif // WHISKDB_CLI_DEV_WORKFLOW_HPP
