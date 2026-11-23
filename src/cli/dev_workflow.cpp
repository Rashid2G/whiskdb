#include "cli/dev_workflow.hpp"

#include "engine.hpp"
#include "storage/durability.hpp"
#include "wasm/wasm_runtime.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

namespace database {

namespace {

constexpr std::string_view kManifestFilename = "whisk.toml";
constexpr std::string_view kGuestHeaderFilename = "whisk_guest.hpp";
constexpr std::string_view kTestPrefix = "test_";

struct Manifest {
    std::string name;
    std::string entry;
    std::string output;
};

struct TempDirGuard {
    std::filesystem::path path;
    ~TempDirGuard() {
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    }
};

std::string DeriveModuleName(const std::filesystem::path& project_dir) {
    std::error_code ec;
    std::filesystem::path normalized = std::filesystem::absolute(project_dir, ec).lexically_normal();
    if (ec) {
        normalized = project_dir.lexically_normal();
    }
    std::string name = normalized.filename().string();
    if (name.empty() || name == "." || name == "..") {
        name = normalized.parent_path().filename().string();
    }
    if (name.empty() || name == "." || name == "..") {
        name = "whisk_module";
    }
    return name;
}

Result<Manifest> ParseManifest(const std::filesystem::path& manifest_path) {
    if (!std::filesystem::exists(manifest_path)) {
        return MAKE_ERROR(ErrorCode::NotFound, "Manifest not found: {}", manifest_path.string());
    }

    toml::table tbl;
    try {
        tbl = toml::parse_file(manifest_path.string());
    } catch (const toml::parse_error& err) {
        return MAKE_ERROR(ErrorCode::InvalidArgument,
                          "Failed to parse {}: {}",
                          manifest_path.string(),
                          err.description());
    }

    auto* module_tbl = tbl["module"].as_table();
    if (module_tbl == nullptr) {
        return MAKE_ERROR(ErrorCode::InvalidArgument,
                          "Missing [module] section in {}",
                          manifest_path.string());
    }

    Manifest m;
    if (auto name = (*module_tbl)["name"].value<std::string>()) {
        m.name = *name;
    }
    if (auto entry = (*module_tbl)["entry"].value<std::string>()) {
        m.entry = *entry;
    }
    if (auto output = (*module_tbl)["output"].value<std::string>()) {
        m.output = *output;
    }

    if (m.name.empty()) {
        return MAKE_ERROR(ErrorCode::InvalidArgument,
                          "Missing [module].name in {}",
                          manifest_path.string());
    }
    if (m.entry.empty()) {
        return MAKE_ERROR(ErrorCode::InvalidArgument,
                          "Missing [module].entry in {}",
                          manifest_path.string());
    }
    if (m.output.empty()) {
        return MAKE_ERROR(ErrorCode::InvalidArgument,
                          "Missing [module].output in {}",
                          manifest_path.string());
    }

    return m;
}

bool HasGuestHeader(const std::filesystem::path& dir) {
    if (dir.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::exists(dir / kGuestHeaderFilename, ec);
}

std::optional<std::filesystem::path> CurrentExecutableDir() {
    std::array<char, 4096> buf{};
    ssize_t len = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (len <= 0) {
        return std::nullopt;
    }
    buf[static_cast<idx_t>(len)] = '\0';
    return std::filesystem::path(buf.data()).parent_path();
}

Result<std::filesystem::path> DiscoverSdkIncludeDir(
    const std::optional<std::filesystem::path>& override_dir) {
    // 1. Explicit override
    if (override_dir.has_value() && !override_dir->empty()) {
        if (HasGuestHeader(*override_dir)) {
            return *override_dir;
        }
        return MAKE_ERROR(ErrorCode::NotFound,
                          "{} not found in --sdk-include directory: {}",
                          kGuestHeaderFilename,
                          override_dir->string());
    }

    // 2. Environment variable WHISKDB_SDK_INCLUDE
    if (const char* env_sdk = std::getenv("WHISKDB_SDK_INCLUDE");
        env_sdk != nullptr && env_sdk[0] != '\0') {
        std::filesystem::path env_path(env_sdk);
        if (HasGuestHeader(env_path)) {
            return env_path;
        }
    }

    // 3. Compile-time WHISKDB_SOURCE_DIR/include/sdk
#ifdef WHISKDB_SOURCE_DIR
    {
        std::filesystem::path src_sdk = std::filesystem::path(WHISKDB_SOURCE_DIR) / "include" / "sdk";
        if (HasGuestHeader(src_sdk)) {
            return src_sdk;
        }
    }
#endif

    // 4. Relative to current executable and working directory
    if (auto exe_dir = CurrentExecutableDir(); exe_dir.has_value()) {
        const std::array<std::filesystem::path, 3> exe_candidates = {
            *exe_dir / ".." / "include" / "sdk",
            *exe_dir / ".." / ".." / "include" / "sdk",
            *exe_dir / "include" / "sdk",
        };
        for (const auto& candidate : exe_candidates) {
            if (HasGuestHeader(candidate)) {
                return candidate.lexically_normal();
            }
        }
    }

    const std::array<std::filesystem::path, 3> cwd_candidates = {
        std::filesystem::path("include") / "sdk",
        std::filesystem::path("..") / "include" / "sdk",
        std::filesystem::path("..") / ".." / "include" / "sdk",
    };
    for (const auto& candidate : cwd_candidates) {
        if (HasGuestHeader(candidate)) {
            return std::filesystem::absolute(candidate).lexically_normal();
        }
    }

    // 5. System install location
    std::filesystem::path sys_sdk("/usr/local/include/whiskdb");
    if (HasGuestHeader(sys_sdk)) {
        return sys_sdk;
    }

    return MAKE_ERROR(ErrorCode::NotFound,
                      "Could not locate {}. Set WHISKDB_SDK_INCLUDE or pass --sdk-include <path>.",
                      kGuestHeaderFilename);
}

std::string LocateClangxx() {
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        std::filesystem::path tools_clang =
            std::filesystem::path(home) / ".whisk-tools" / "bin" / "clang++";
        std::error_code ec;
        if (std::filesystem::exists(tools_clang, ec)) {
            return tools_clang.string();
        }
    }
    return "clang++";
}

Result<std::vector<uint8_t>> ReadBinaryFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return MAKE_ERROR(ErrorCode::IoError, "Failed to open file: {}", path.string());
    }
    auto sz = file.tellg();
    if (sz < 0) {
        return MAKE_ERROR(ErrorCode::IoError, "Failed to determine file size: {}", path.string());
    }
    std::vector<uint8_t> bytes(static_cast<idx_t>(sz));
    file.seekg(0, std::ios::beg);
    if (sz > 0 && !file.read(reinterpret_cast<char*>(bytes.data()), sz)) {
        return MAKE_ERROR(ErrorCode::IoError, "Failed to read file bytes: {}", path.string());
    }
    return bytes;
}

Result<std::string> RunCommandCapture(const std::string& cmd) {
    std::string full_cmd = cmd + " 2>&1";
    std::FILE* pipe = ::popen(full_cmd.c_str(), "r");
    if (pipe == nullptr) {
        return MAKE_ERROR(ErrorCode::Internal, "Failed to spawn compiler command");
    }

    std::string output;
    std::array<char, 512> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        output += buf.data();
    }

    int status = ::pclose(pipe);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (exit_code != 0) {
        return MAKE_ERROR(ErrorCode::Internal,
                          "clang++ failed (exit code {}):\n{}",
                          exit_code,
                          output);
    }
    return output;
}

std::filesystem::path MakeUniqueTempDir() {
    auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
    auto pid = static_cast<uint64_t>(::getpid());
    return std::filesystem::temp_directory_path() /
           std::format("whisk_test_{}_{}", pid, static_cast<uint64_t>(now_ns));
}

} // namespace

Result<InitResult> InitProject(const std::filesystem::path& project_dir, bool force) {
    if (project_dir.empty()) {
        return MAKE_ERROR(ErrorCode::InvalidArgument, "Project directory must not be empty");
    }

    std::filesystem::path manifest_path = project_dir / kManifestFilename;
    std::error_code ec;
    if (std::filesystem::exists(manifest_path, ec) && !force) {
        return MAKE_ERROR(ErrorCode::AlreadyExists,
                          "{} already exists in {} (pass --force to overwrite)",
                          kManifestFilename,
                          project_dir.string());
    }

    std::filesystem::path src_dir = project_dir / "src";
    std::filesystem::create_directories(src_dir, ec);
    if (ec) {
        return MAKE_ERROR(ErrorCode::IoError,
                          "Failed to create project directories at {}: {}",
                          src_dir.string(),
                          ec.message());
    }

    std::string module_name = DeriveModuleName(project_dir);
    std::filesystem::path entry_path = src_dir / "module.cpp";

    std::string manifest_content = std::format(
        "[module]\n"
        "name = \"{}\"\n"
        "entry = \"src/module.cpp\"\n"
        "output = \"build/{}.wasm\"\n",
        module_name,
        module_name);

    {
        std::ofstream manifest_out(manifest_path, std::ios::trunc);
        if (!manifest_out.is_open()) {
            return MAKE_ERROR(ErrorCode::IoError,
                              "Failed to write {}",
                              manifest_path.string());
        }
        manifest_out << manifest_content;
        if (!manifest_out.good()) {
            return MAKE_ERROR(ErrorCode::IoError,
                              "Failed to flush {}",
                              manifest_path.string());
        }
    }

    constexpr std::string_view kTemplateModule = R"cpp(#include "whisk_guest.hpp"

WHISK_INIT() {
    int64_t rc = whisk::Exec(
        "CREATE TABLE IF NOT EXISTS greetings ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "message TEXT NOT NULL)");
    if (rc < 0) {
        return whisk::Error("failed to initialize greetings table");
    }
    return whisk::Ok("{\"status\":\"initialized\"}");
}

WHISK_FUNC(hello) {
    whisk::StrView args = WHISK_ARGS();
    whisk::StrView name("world");
    whisk::StrView parsed_name;
    if (whisk::JsonGetStr(args, "name", parsed_name) && !parsed_name.empty()) {
        name = parsed_name;
    }

    whisk::Buf<256> out;
    out.append("{\"greeting\":\"hello, ");
    out.append(name);
    out.append("\"}");
    return whisk::Ok(out.view());
}

WHISK_TEST(smoke) {
    int64_t rc = whisk::Exec("INSERT INTO greetings (message) VALUES ('hello whisk')");
    if (rc < 0) {
        return whisk::Error("failed to insert greeting row");
    }

    whisk::StrView rows = whisk::Query("SELECT COUNT(*) FROM greetings");
    int64_t count = 0;
    if (!whisk::QueryFirstInt(rows, count) || count < 1) {
        return whisk::Error("expected at least one greeting row");
    }

    return whisk::Ok("{\"smoke\":\"passed\"}");
}
)cpp";

    {
        std::ofstream entry_out(entry_path, std::ios::trunc);
        if (!entry_out.is_open()) {
            return MAKE_ERROR(ErrorCode::IoError,
                              "Failed to write {}",
                              entry_path.string());
        }
        entry_out << kTemplateModule;
        if (!entry_out.good()) {
            return MAKE_ERROR(ErrorCode::IoError,
                              "Failed to flush {}",
                              entry_path.string());
        }
    }

    InitResult result;
    result.project_dir = project_dir;
    result.module_name = std::move(module_name);
    result.manifest_path = std::move(manifest_path);
    result.entry_path = std::move(entry_path);
    return result;
}

Result<BuildResult> BuildProject(
    const std::filesystem::path& project_dir,
    const std::optional<std::filesystem::path>& sdk_include_override) {
    std::filesystem::path manifest_path = project_dir / kManifestFilename;
    auto manifest_res = ParseManifest(manifest_path);
    if (!manifest_res) {
        return std::unexpected(manifest_res.error());
    }
    const Manifest& manifest = *manifest_res;

    std::filesystem::path entry_path = project_dir / manifest.entry;
    if (!std::filesystem::exists(entry_path)) {
        return MAKE_ERROR(ErrorCode::NotFound,
                          "Entry source file not found: {}",
                          entry_path.string());
    }

    auto sdk_res = DiscoverSdkIncludeDir(sdk_include_override);
    if (!sdk_res) {
        return std::unexpected(sdk_res.error());
    }

    std::filesystem::path output_path = project_dir / manifest.output;
    if (output_path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(output_path.parent_path(), ec);
        if (ec) {
            return MAKE_ERROR(ErrorCode::IoError,
                              "Failed to create output directory {}: {}",
                              output_path.parent_path().string(),
                              ec.message());
        }
    }

    std::string clangxx = LocateClangxx();
    std::string cmd = std::format(
        "\"{}\" --target=wasm32 -O3 -nostdlib -fno-exceptions -fno-rtti "
        "-Wl,--no-entry -Wl,--export-dynamic -I\"{}\" \"{}\" -o \"{}\"",
        clangxx,
        sdk_res->string(),
        entry_path.string(),
        output_path.string());

    auto compile_res = RunCommandCapture(cmd);
    if (!compile_res) {
        return std::unexpected(compile_res.error());
    }

    auto wasm_bytes_res = ReadBinaryFile(output_path);
    if (!wasm_bytes_res) {
        return std::unexpected(wasm_bytes_res.error());
    }
    const std::vector<uint8_t>& wasm_bytes = *wasm_bytes_res;

    auto runtime_res = WasmRuntime::Create();
    if (!runtime_res) {
        return std::unexpected(runtime_res.error());
    }

    auto compiled_res = (*runtime_res)->CompileModule(wasm_bytes);
    if (!compiled_res) {
        return MAKE_ERROR(ErrorCode::WasmError,
                          "Produced WASM binary failed validation: {}",
                          compiled_res.error().Message());
    }

    const auto& exports = compiled_res->exported_funcs;
    const std::array<std::string_view, 3> required_exports = {
        "whisk_alloc",
        "whisk_result_ptr",
        "whisk_result_len",
    };
    for (std::string_view req : required_exports) {
        bool found = std::find(exports.begin(), exports.end(), req) != exports.end();
        if (!found) {
            return MAKE_ERROR(ErrorCode::WasmError,
                              "Compiled module is missing required Whisk export '{}'",
                              req);
        }
    }

    BuildResult result;
    result.module_name = manifest.name;
    result.entry_path = std::move(entry_path);
    result.output_path = std::move(output_path);
    result.binary_size = static_cast<idx_t>(wasm_bytes.size());
    result.exported_funcs = compiled_res->exported_funcs;
    return result;
}

Result<TestSummary> TestProject(
    const std::filesystem::path& project_dir,
    const std::optional<std::filesystem::path>& sdk_include_override) {
    auto build_res = BuildProject(project_dir, sdk_include_override);
    if (!build_res) {
        return std::unexpected(build_res.error());
    }

    auto wasm_bytes_res = ReadBinaryFile(build_res->output_path);
    if (!wasm_bytes_res) {
        return std::unexpected(wasm_bytes_res.error());
    }

    TempDirGuard temp_guard{MakeUniqueTempDir()};
    EngineOptions opts;
    opts.data_dir = temp_guard.path;
    opts.durability = DurabilityMode::MemoryOnly;

    auto engine_res = Engine::Open(std::move(opts));
    if (!engine_res) {
        return std::unexpected(engine_res.error());
    }
    auto& engine = *engine_res;

    auto pub_res = engine->PublishModule(build_res->module_name, *wasm_bytes_res);
    if (!pub_res) {
        return std::unexpected(pub_res.error());
    }

    TestSummary summary;
    summary.module_name = build_res->module_name;

    std::vector<std::string> test_funcs;
    for (const auto& fn : build_res->exported_funcs) {
        if (fn.starts_with(kTestPrefix)) {
            test_funcs.push_back(fn);
        }
    }

    summary.total = static_cast<idx_t>(test_funcs.size());
    summary.results.reserve(test_funcs.size());

    for (const auto& fn : test_funcs) {
        TestCaseResult tc;
        tc.name = fn;

        auto t0 = std::chrono::steady_clock::now();
        auto call_res = engine->CallWhisk(build_res->module_name, fn, "{}");
        auto t1 = std::chrono::steady_clock::now();
        tc.exec_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        summary.total_exec_us += tc.exec_us;

        if (call_res) {
            tc.passed = true;
            tc.fuel_used = call_res->fuel_used;
            tc.output = call_res->result_json;
            ++summary.passed;
        } else {
            tc.passed = false;
            tc.error = call_res.error().Message().empty()
                           ? std::string(ToString(call_res.error().Code()))
                           : call_res.error().Message();
            ++summary.failed;
        }
        summary.results.push_back(std::move(tc));
    }

    return summary;
}

} // namespace database
