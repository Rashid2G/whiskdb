#ifndef WHISKDB_TESTS_WASM_TEST_HELPER_HPP
#define WHISKDB_TESTS_WASM_TEST_HELPER_HPP

#include "errors.hpp"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace database::test_util {

inline std::string FindWasmClangxx() {
#ifdef WHISKDB_WASM_CLANGXX
    std::string cmake_clangxx = WHISKDB_WASM_CLANGXX;
    if (!cmake_clangxx.empty() && std::filesystem::exists(cmake_clangxx)) {
        return cmake_clangxx;
    }
#endif
    const char* home = std::getenv("HOME");
    if (home != nullptr) {
        std::filesystem::path whisk_tools_clang = std::filesystem::path(home) / ".whisk-tools" / "bin" / "clang++";
        if (std::filesystem::exists(whisk_tools_clang)) {
            return whisk_tools_clang.string();
        }
    }
    return "clang++";
}

inline std::filesystem::path FindSdkIncludeDir() {
#ifdef WHISKDB_SOURCE_DIR
    std::filesystem::path source_sdk = std::filesystem::path(WHISKDB_SOURCE_DIR) / "include" / "sdk";
    if (std::filesystem::exists(source_sdk)) {
        return source_sdk;
    }
#endif
    if (std::filesystem::exists("include/sdk")) {
        return std::filesystem::absolute("include/sdk");
    }
    if (std::filesystem::exists("../include/sdk")) {
        return std::filesystem::absolute("../include/sdk");
    }
    return "include/sdk";
}

inline Result<std::vector<uint8_t>> CompileWhiskCppToWasm(
    const std::filesystem::path& cpp_path,
    const std::filesystem::path& out_wasm_path) {
    if (!std::filesystem::exists(cpp_path)) {
        return MAKE_ERROR(ErrorCode::NotFound, "Source C++ file not found: {}", cpp_path.string());
    }

    std::string clangxx = FindWasmClangxx();
    std::filesystem::path sdk_inc = FindSdkIncludeDir();

    if (out_wasm_path.has_parent_path()) {
        std::filesystem::create_directories(out_wasm_path.parent_path());
    }

    std::string cmd = std::format(
        "\"{}\" --target=wasm32 -O3 -nostdlib -fno-exceptions -fno-rtti "
        "-Wl,--no-entry -Wl,--export-dynamic -Wall -Wextra -Wpedantic "
        "-I\"{}\" \"{}\" -o \"{}\"",
        clangxx,
        sdk_inc.string(),
        cpp_path.string(),
        out_wasm_path.string()
    );

    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        return MAKE_ERROR(ErrorCode::Internal, "clang++ wasm compilation failed with exit code {}: cmd={}", ret, cmd);
    }

    if (!std::filesystem::exists(out_wasm_path)) {
        return MAKE_ERROR(ErrorCode::Internal, "WASM output file not produced: {}", out_wasm_path.string());
    }

    std::ifstream file(out_wasm_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return MAKE_ERROR(ErrorCode::Internal, "Failed to open WASM output file: {}", out_wasm_path.string());
    }
    auto sz = file.tellg();
    if (sz < 0) {
        return MAKE_ERROR(ErrorCode::Internal, "Failed to read WASM output size");
    }
    std::vector<uint8_t> buffer(static_cast<size_t>(sz));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), sz)) {
        return MAKE_ERROR(ErrorCode::Internal, "Failed to read WASM output bytes");
    }

    return buffer;
}

inline Result<std::vector<uint8_t>> CompileWhiskSourceToWasm(
    std::string_view cpp_code,
    const std::filesystem::path& work_dir,
    std::string_view stem) {
    std::filesystem::create_directories(work_dir);
    std::filesystem::path cpp_path = work_dir / (std::string(stem) + ".cpp");
    std::filesystem::path wasm_path = work_dir / (std::string(stem) + ".wasm");

    std::ofstream out(cpp_path);
    if (!out.is_open()) {
        return MAKE_ERROR(ErrorCode::Internal, "Failed to write temp C++ source: {}", cpp_path.string());
    }
    out << cpp_code;
    out.close();

    return CompileWhiskCppToWasm(cpp_path, wasm_path);
}

} // namespace database::test_util

#endif // WHISKDB_TESTS_WASM_TEST_HELPER_HPP
