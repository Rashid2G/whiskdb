#include "common/types.hpp"
#include "config.hpp"
#include "engine.hpp"
#include "storage/durability.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <limits>
#include <print>
#include <string>
#include <string_view>
#include <type_traits>

using namespace database;

namespace {

void TestCoreTypes() {
    // DuckDB-style idx_t index and count type
    static_assert(std::is_same_v<idx_t, uint64_t>, "idx_t must be uint64_t");
    static_assert(sizeof(idx_t) == 8, "idx_t must be 8 bytes (64-bit)");
    static_assert(std::is_unsigned_v<idx_t>, "idx_t must be unsigned");
    static_assert(std::is_integral_v<idx_t>, "idx_t must be an integral type");

    // lsn_t log sequence number type
    static_assert(std::is_same_v<lsn_t, uint64_t>, "lsn_t must be uint64_t");
    static_assert(sizeof(lsn_t) == 8, "lsn_t must be 8 bytes");

    // Fixed-width integer aliases
    static_assert(std::is_same_v<int8_t, std::int8_t>);
    static_assert(std::is_same_v<int16_t, std::int16_t>);
    static_assert(std::is_same_v<int32_t, std::int32_t>);
    static_assert(std::is_same_v<int64_t, std::int64_t>);
    static_assert(std::is_same_v<uint8_t, std::uint8_t>);
    static_assert(std::is_same_v<uint16_t, std::uint16_t>);
    static_assert(std::is_same_v<uint32_t, std::uint32_t>);
    static_assert(std::is_same_v<uint64_t, std::uint64_t>);

    // Byte and pointer aliases
    static_assert(std::is_same_v<data_t, uint8_t>);
    static_assert(std::is_same_v<data_ptr_t, uint8_t*>);
    static_assert(std::is_same_v<const_data_ptr_t, const uint8_t*>);

    // Runtime bounds verification
    idx_t max_idx = std::numeric_limits<idx_t>::max();
    assert(max_idx == UINT64_MAX);
    assert(sizeof(idx_t) == sizeof(uint64_t));
}

void TestValidTomlParsing() {
    constexpr std::string_view valid_toml = R"(
[config]
host = "192.168.1.50"
port = 9090
data_dir = "/var/lib/whiskdb/data"
page_size = 8192
durability_mode = "strict"
wal_group_commit_ms = 25
checkpoint_wal_records = 50000
wasm_max_fuel = 200000000
wasm_max_memory_mb = 128
)";

    auto config = Config::FromString(valid_toml);
    assert(config.IsValid());
    assert(config.is_valid());

    assert(config.Host() == "192.168.1.50");
    assert(config.host() == "192.168.1.50");
    assert(config.Port() == 9090);
    assert(config.port() == 9090);
    assert(config.DataDir() == "/var/lib/whiskdb/data");
    assert(config.data_dir() == "/var/lib/whiskdb/data");
    assert(config.DbFilePath() == "/var/lib/whiskdb/data");
    assert(config.db_file_path() == "/var/lib/whiskdb/data");
    assert(config.PageSize() == 8192);
    assert(config.page_size() == 8192);
    assert(config.DurabilityModeName() == "strict");
    assert(config.durability_mode() == "strict");
    assert(config.Durability() == DurabilityMode::Strict);
    assert(config.WalGroupCommitMs() == 25);
    assert(config.wal_group_commit_ms() == 25);
    assert(config.CheckpointWalRecords() == 50000);
    assert(config.checkpoint_wal_records() == 50000);
    assert(config.WasmMaxFuel() == 200000000);
    assert(config.wasm_max_fuel() == 200000000);
    assert(config.WasmMaxMemoryMb() == 128);
    assert(config.wasm_max_memory_mb() == 128);

    // Verify file loading path with a temporary file
    const std::string tmp_path = "./test_temp_valid_config.toml";
    {
        std::ofstream out(tmp_path);
        out << valid_toml;
    }
    Config file_config(tmp_path);
    assert(file_config.IsValid());
    assert(file_config.Host() == "192.168.1.50");
    assert(file_config.Port() == 9090);
    assert(file_config.DataDir() == "/var/lib/whiskdb/data");
    assert(file_config.DurabilityModeName() == "strict");
    assert(file_config.WalGroupCommitMs() == 25);
    assert(file_config.WasmMaxFuel() == 200000000);
    assert(file_config.WasmMaxMemoryMb() == 128);
    std::filesystem::remove(tmp_path);
}

void TestDurabilityModes() {
    struct TestCase {
        std::string mode_str;
        DurabilityMode expected;
    };

    const TestCase cases[] = {
        {"strict", DurabilityMode::Strict},
        {"sync", DurabilityMode::Strict},
        {"STRICT", DurabilityMode::Strict},
        {"group_commit", DurabilityMode::GroupCommit},
        {"batched", DurabilityMode::GroupCommit},
        {"Group_Commit", DurabilityMode::GroupCommit},
        {"relaxed", DurabilityMode::Relaxed},
        {"async", DurabilityMode::Relaxed},
        {"RELAXED", DurabilityMode::Relaxed},
        {"memory_only", DurabilityMode::MemoryOnly},
        {"ephemeral", DurabilityMode::MemoryOnly},
        {"Memory_Only", DurabilityMode::MemoryOnly},
    };

    for (const auto& tc : cases) {
        std::string toml = "[config]\ndurability_mode = \"" + tc.mode_str + "\"\n";
        auto config = Config::FromString(toml);
        assert(config.IsValid());
        assert(config.Durability() == tc.expected);
        assert(config.DurabilityModeName() == tc.mode_str);
    }
}

void TestMissingFileLoadsDefaults() {
    Config config("/tmp/definitely_nonexistent_whiskdb_config_file_98765.toml");
    // Missing file should not crash; it logs a warning and marks is_valid as false
    assert(!config.IsValid());
    assert(!config.is_valid());

    // Defaults must be fully loaded and intact
    assert(config.Host() == "0.0.0.0");
    assert(config.Port() == 8080);
    assert(config.DataDir() == "./data");
    assert(config.PageSize() == 4096);
    assert(config.DurabilityModeName() == "group_commit");
    assert(config.Durability() == DurabilityMode::GroupCommit);
    assert(config.CheckpointWalRecords() == 100000);
    assert(config.WalGroupCommitMs() == 10);
    assert(config.WasmMaxFuel() == 100000000);
    assert(config.WasmMaxMemoryMb() == 64);
}

void TestEmptySectionsAndPartialKeys() {
    // Empty TOML string
    auto empty_config = Config::FromString("");
    assert(empty_config.IsValid());
    assert(empty_config.Host() == "0.0.0.0");
    assert(empty_config.Port() == 8080);
    assert(empty_config.DataDir() == "./data");
    assert(empty_config.DurabilityModeName() == "group_commit");
    assert(empty_config.WalGroupCommitMs() == 10);
    assert(empty_config.WasmMaxFuel() == 100000000);
    assert(empty_config.WasmMaxMemoryMb() == 64);

    // Empty section
    auto empty_section = Config::FromString("[config]\n");
    assert(empty_section.IsValid());
    assert(empty_section.Host() == "0.0.0.0");
    assert(empty_section.Port() == 8080);
    assert(empty_section.DataDir() == "./data");
    assert(empty_section.DurabilityModeName() == "group_commit");

    // Partial override only changes specified keys
    auto partial = Config::FromString("[config]\nport = 4444\nhost = \"10.0.0.1\"\n");
    assert(partial.IsValid());
    assert(partial.Port() == 4444);
    assert(partial.Host() == "10.0.0.1");
    // Other fields retain defaults
    assert(partial.DataDir() == "./data");
    assert(partial.DurabilityModeName() == "group_commit");
    assert(partial.WalGroupCommitMs() == 10);
    assert(partial.WasmMaxFuel() == 100000000);
    assert(partial.WasmMaxMemoryMb() == 64);

    // Alternative key names for backward compatibility
    auto compat = Config::FromString("[config]\nipv4 = \"127.0.0.2\"\ndata_directory = \"./my_data\"\n");
    assert(compat.IsValid());
    assert(compat.Host() == "127.0.0.2");
    assert(compat.DataDir() == "./my_data");
}

void TestMalformedToml() {
    const std::string_view malformed_cases[] = {
        "[config\n unclosed section header",
        "[config]\n port = 1234 = = = invalid syntax",
        "[config]\n host = \"unclosed string",
        "[config]\n key = [1, 2, unclosed array\n"
    };

    for (const auto& bad_toml : malformed_cases) {
        auto config = Config::FromString(bad_toml);
        // Must handle error gracefully without throwing
        assert(!config.IsValid());
        // Defaults must be safely preserved
        assert(config.Host() == "0.0.0.0");
        assert(config.Port() == 8080);
        assert(config.DataDir() == "./data");
        assert(config.DurabilityModeName() == "group_commit");
        assert(config.Durability() == DurabilityMode::GroupCommit);
    }
}

void TestInvalidDurabilityMode() {
    // Unknown mode string
    auto cfg_unknown = Config::FromString("[config]\ndurability_mode = \"ultra_fast_turbo\"\n");
    assert(!cfg_unknown.IsValid());
    assert(cfg_unknown.DurabilityModeName() == "group_commit");
    assert(cfg_unknown.Durability() == DurabilityMode::GroupCommit);

    // Non-string durability mode
    auto cfg_non_string = Config::FromString("[config]\ndurability_mode = 9999\n");
    assert(!cfg_non_string.IsValid());
    assert(cfg_non_string.DurabilityModeName() == "group_commit");
    assert(cfg_non_string.Durability() == DurabilityMode::GroupCommit);
}

void TestEngineOptionsIntegration() {
    constexpr std::string_view valid_toml = R"(
[config]
host = "10.1.2.3"
port = 7000
data_dir = "/tmp/engine_test"
durability_mode = "relaxed"
checkpoint_wal_records = 12345
)";

    auto config = Config::FromString(valid_toml);
    assert(config.IsValid());

    auto opts_res = EngineOptions::FromConfig(config);
    assert(opts_res.has_value());
    const auto& opts = *opts_res;
    assert(opts.host == "10.1.2.3");
    assert(opts.port == 7000);
    assert(opts.data_dir == "/tmp/engine_test");
    assert(opts.durability == DurabilityMode::Relaxed);
    assert(opts.checkpoint_wal_records == 12345);

    // Test with missing file / defaults
    Config default_config("/tmp/missing_file.toml");
    auto default_opts = EngineOptions::FromConfig(default_config);
    assert(default_opts.has_value());
    assert(default_opts->host == "0.0.0.0");
    assert(default_opts->port == 8080);
    assert(default_opts->data_dir == "./data");
    assert(default_opts->durability == DurabilityMode::GroupCommit);
    assert(default_opts->checkpoint_wal_records == 100000);
}

} // namespace

int main() {
    TestCoreTypes();
    TestValidTomlParsing();
    TestDurabilityModes();
    TestMissingFileLoadsDefaults();
    TestEmptySectionsAndPartialKeys();
    TestMalformedToml();
    TestInvalidDurabilityMode();
    TestEngineOptionsIntegration();

    std::println("test_config: OK");
    return 0;
}
