#ifndef WHISKDB_CONFIG_HPP
#define WHISKDB_CONFIG_HPP

#include "common/types.hpp"
#include "logger.hpp"
#include "storage/durability.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>

namespace database {

const std::string config_file_path{"./config.toml"};

class Config {
public:
    Config() {
        ResetDefaults();
    }

    explicit Config(const std::string& file_path) {
        LoadFromFile(file_path);
    }

    static Config FromString(std::string_view toml_content) {
        Config config;
        config.LoadFromString(toml_content);
        return config;
    }

    void LoadFromString(std::string_view toml_content) {
        ResetDefaults();
        try {
            config_ = toml::parse(toml_content);
            ParseTable();
        } catch (const std::exception& e) {
            LOG_WARN("Could not parse config string: {}. Using default configuration.", e.what());
            is_valid_ = false;
        }
    }

    void LoadFromFile(const std::string& file_path) {
        ResetDefaults();
        try {
            config_ = toml::parse_file(file_path);
            ParseTable();
        } catch (const std::exception& e) {
            LOG_WARN("Could not parse config '{}': {}. Using default configuration.",
                     file_path, e.what());
            is_valid_ = false;
        }
    }

    bool IsValid() const noexcept { return is_valid_; }
    bool is_valid() const noexcept { return is_valid_; }

    const std::string& DataDir() const noexcept { return data_dir_; }
    const std::string& data_dir() const noexcept { return data_dir_; }
    const std::string& DbFilePath() const noexcept { return data_dir_; }
    const std::string& db_file_path() const noexcept { return data_dir_; }

    uint16_t PageSize() const noexcept { return page_size_; }
    uint16_t page_size() const noexcept { return page_size_; }

    const std::string& Host() const noexcept { return host_; }
    const std::string& host() const noexcept { return host_; }

    uint16_t Port() const noexcept { return port_; }
    uint16_t port() const noexcept { return port_; }

    /// Raw durability mode text (strict | group_commit | relaxed | memory_only)
    const std::string& DurabilityModeName() const noexcept { return durability_mode_; }
    const std::string& durability_mode() const noexcept { return durability_mode_; }

    DurabilityMode Durability() const noexcept {
        auto mode = ParseDurabilityMode(durability_mode_);
        return mode ? *mode : DurabilityMode::GroupCommit;
    }

    /// Snapshot and reset the WAL after this many WAL records (0 disables auto-checkpoint)
    idx_t CheckpointWalRecords() const noexcept { return checkpoint_wal_records_; }
    idx_t checkpoint_wal_records() const noexcept { return checkpoint_wal_records_; }

    /// Group commit batch interval in milliseconds
    idx_t WalGroupCommitMs() const noexcept { return wal_group_commit_ms_; }
    idx_t wal_group_commit_ms() const noexcept { return wal_group_commit_ms_; }

    /// Maximum fuel allowed for WASM execution before trap
    idx_t WasmMaxFuel() const noexcept { return wasm_max_fuel_; }
    idx_t wasm_max_fuel() const noexcept { return wasm_max_fuel_; }

    /// Maximum memory (in MB) allocated for WASM execution
    idx_t WasmMaxMemoryMb() const noexcept { return wasm_max_memory_mb_; }
    idx_t wasm_max_memory_mb() const noexcept { return wasm_max_memory_mb_; }

private:
    void ResetDefaults() {
        data_dir_ = "./data";
        page_size_ = 4096;
        host_ = "0.0.0.0";
        port_ = 8080;
        durability_mode_ = "group_commit";
        checkpoint_wal_records_ = 100000;
        wal_group_commit_ms_ = 10;
        wasm_max_fuel_ = 100000000;
        wasm_max_memory_mb_ = 64;
        is_valid_ = true;
    }

    void ParseTable() {
        const toml::table* tbl = nullptr;
        if (auto node = config_["config"]; node.is_table()) {
            tbl = node.as_table();
        }

        auto get_string = [&](std::string_view key) -> std::optional<std::string> {
            if (tbl) {
                if (auto val = (*tbl)[key].value<std::string>()) {
                    return *val;
                }
            }
            if (auto val = config_[key].value<std::string>()) {
                return *val;
            }
            return std::nullopt;
        };

        auto get_int = [&](std::string_view key) -> std::optional<int64_t> {
            if (tbl) {
                if (auto val = (*tbl)[key].value<int64_t>()) {
                    return *val;
                }
            }
            if (auto val = config_[key].value<int64_t>()) {
                return *val;
            }
            return std::nullopt;
        };

        // data_dir (fallback to data_directory or db_file)
        if (auto v = get_string("data_dir")) {
            data_dir_ = *v;
        } else if (auto v = get_string("data_directory")) {
            data_dir_ = *v;
        } else if (auto v = get_string("db_file")) {
            data_dir_ = *v;
        }

        // host (fallback to ipv4)
        if (auto v = get_string("host")) {
            host_ = *v;
        } else if (auto v = get_string("ipv4")) {
            host_ = *v;
        }

        // port
        if (auto v = get_int("port")) {
            port_ = static_cast<uint16_t>(*v);
        }

        // page_size
        if (auto v = get_int("page_size")) {
            page_size_ = static_cast<uint16_t>(*v);
        }

        // durability_mode
        if (auto v = get_string("durability_mode")) {
            auto parsed = ParseDurabilityMode(*v);
            if (parsed) {
                durability_mode_ = *v;
            } else {
                LOG_WARN("Invalid durability_mode '{}' in config: {}. Falling back to default 'group_commit'.",
                         *v, parsed.error().Message());
                durability_mode_ = "group_commit";
                is_valid_ = false;
            }
        } else if (tbl && (*tbl)["durability_mode"]) {
            LOG_WARN("Invalid non-string durability_mode in config. Falling back to default 'group_commit'.");
            durability_mode_ = "group_commit";
            is_valid_ = false;
        }

        // checkpoint_wal_records
        if (auto v = get_int("checkpoint_wal_records")) {
            checkpoint_wal_records_ = static_cast<idx_t>(*v);
        }

        // wal_group_commit_ms (check [config], [wal], and root)
        if (auto v = get_int("wal_group_commit_ms")) {
            wal_group_commit_ms_ = static_cast<idx_t>(*v);
        } else if (auto val = config_["wal"]["group_commit_ms"].value<int64_t>()) {
            wal_group_commit_ms_ = static_cast<idx_t>(*val);
        }

        // wasm_max_fuel (check [config], [wasm], and root)
        if (auto v = get_int("wasm_max_fuel")) {
            wasm_max_fuel_ = static_cast<idx_t>(*v);
        } else if (auto val = config_["wasm"]["max_fuel"].value<int64_t>()) {
            wasm_max_fuel_ = static_cast<idx_t>(*val);
        } else if (auto val = config_["wasm"]["wasm_max_fuel"].value<int64_t>()) {
            wasm_max_fuel_ = static_cast<idx_t>(*val);
        }

        // wasm_max_memory_mb (check [config], [wasm], and root)
        if (auto v = get_int("wasm_max_memory_mb")) {
            wasm_max_memory_mb_ = static_cast<idx_t>(*v);
        } else if (auto val = config_["wasm"]["max_memory_mb"].value<int64_t>()) {
            wasm_max_memory_mb_ = static_cast<idx_t>(*val);
        } else if (auto val = config_["wasm"]["wasm_max_memory_mb"].value<int64_t>()) {
            wasm_max_memory_mb_ = static_cast<idx_t>(*val);
        }
    }

private:
    toml::table config_;
    std::string data_dir_{"./data"};
    uint16_t page_size_{4096};
    std::string host_{"0.0.0.0"};
    uint16_t port_{8080};
    std::string durability_mode_{"group_commit"};
    idx_t checkpoint_wal_records_{100000};
    idx_t wal_group_commit_ms_{10};
    idx_t wasm_max_fuel_{100000000};
    idx_t wasm_max_memory_mb_{64};
    bool is_valid_{true};
};

} // namespace database

namespace Database = database;

#endif // WHISKDB_CONFIG_HPP
