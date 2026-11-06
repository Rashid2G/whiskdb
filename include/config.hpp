#ifndef WHISKDB_CONFIG_HPP
#define WHISKDB_CONFIG_HPP

#include "common/types.hpp"
#include "logger.hpp"

#include <cstdint>
#include <string>
#include <toml++/toml.hpp>

namespace database {

const std::string config_file_path{"./config.toml"};

class Config {
public:
    explicit Config(const std::string& file_path = config_file_path) {
        try {
            config_ = toml::parse_file(file_path);
        } catch (const std::exception& e) {
            LOG_WARN("Could not parse config '{}': {}. Using default configuration.",
                     file_path, e.what());
        }

        if (config_["config"]["db_file"].is_string()) {
            db_file_path_ = config_["config"]["db_file"].value_or("./data");
        } else {
            db_file_path_ = config_["config"]["data_directory"].value_or("./data");
        }

        page_size_ = static_cast<uint16_t>(config_["config"]["page_size"].value_or(4096));

        if (config_["config"]["host"].is_string()) {
            host_ = config_["config"]["host"].value_or("0.0.0.0");
        } else if (config_["config"]["ipv4"].is_string()) {
            host_ = config_["config"]["ipv4"].value_or("0.0.0.0");
        } else {
            host_ = "0.0.0.0";
        }

        port_ = static_cast<uint16_t>(config_["config"]["port"].value_or(8080));

        durability_mode_ = config_["config"]["durability_mode"].value_or(std::string("group_commit"));
        checkpoint_wal_records_ = static_cast<idx_t>(
            config_["config"]["checkpoint_wal_records"].value_or(100000));
    }

public:
    const std::string& DbFilePath() const noexcept { return db_file_path_; }
    const std::string& db_file_path() const noexcept { return db_file_path_; }

    uint16_t PageSize() const noexcept { return page_size_; }
    uint16_t page_size() const noexcept { return page_size_; }

    const std::string& Host() const noexcept { return host_; }
    const std::string& host() const noexcept { return host_; }

    uint16_t Port() const noexcept { return port_; }
    uint16_t port() const noexcept { return port_; }

    /// Raw durability mode text (strict | group_commit | relaxed | memory_only), validated by Engine
    const std::string& DurabilityModeName() const noexcept { return durability_mode_; }

    /// Take a snapshot and reset the WAL after this many WAL records (0 disables auto-checkpoint)
    idx_t CheckpointWalRecords() const noexcept { return checkpoint_wal_records_; }

private:
    toml::table config_;
    std::string db_file_path_{"./data"};
    uint16_t page_size_{4096};
    std::string host_{"0.0.0.0"};
    uint16_t port_{8080};
    std::string durability_mode_{"group_commit"};
    idx_t checkpoint_wal_records_{100000};
};

} // namespace database

namespace Database = database;

#endif // WHISKDB_CONFIG_HPP
