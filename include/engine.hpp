#ifndef WHISKDB_ENGINE_HPP
#define WHISKDB_ENGINE_HPP

#include "config.hpp"
#include "errors.hpp"
#include "network/protocol.hpp"
#include "network/server.hpp"
#include "sql/sql_engine.hpp"
#include "sql/sql_types.hpp"
#include "storage/durability.hpp"
#include "storage/wal_manager.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

namespace database {

/// Fully resolved runtime options for an Engine instance
struct EngineOptions {
    std::filesystem::path data_dir{"./data"};
    DurabilityMode durability = DurabilityMode::GroupCommit;
    std::string host{"127.0.0.1"};
    uint16_t port = 8080;
    idx_t io_threads = 2;
    idx_t checkpoint_wal_records = 100000;   ///< 0 disables automatic checkpoints

    /// Build options from config.toml values (validates durability_mode)
    static Result<EngineOptions> FromConfig(const Config& config);
};

/// What recovery found on startup (exposed for tests and logs)
struct RecoveryInfo {
    bool snapshot_loaded = false;
    lsn_t snapshot_lsn = 0;
    idx_t wal_records_replayed = 0;
    bool wal_tail_truncated = false;
};

/**
 * @brief WhiskDB core engine: in-memory SQL database + write-ahead log + snapshots + TCP server.
 *
 * Every mutating statement is executed and appended to the WAL in the same critical section, so
 * WAL order equals execution order. Durability (how long we wait before replying) follows
 * EngineOptions::durability.
 */
class Engine {
public:
    /// Opens the data directory, loads the latest snapshot and replays the WAL tail.
    static Result<std::unique_ptr<Engine>> Open(EngineOptions options);

    ~Engine();

public:
    /// Execute one SQL statement. Mutations are logged to the WAL before this returns.
    Result<QueryResult> ExecuteSql(std::string_view sql, std::span<const Value> params = {});

    /// Serialize the database to a snapshot and reset the WAL. Returns the snapshot LSN.
    Result<lsn_t> Checkpoint();

    /// Dispatch one wire request (used as the TCP server handler; callable directly in tests).
    Response Handle(const Request& request);

    Result<void> StartServer();
    void StopServer();
    uint16_t Port() const;

    const RecoveryInfo& Recovery() const noexcept { return recovery_; }
    const EngineOptions& Options() const noexcept { return options_; }

private:
    explicit Engine(EngineOptions options);

    Result<void> Recover();
    Result<lsn_t> CheckpointLocked();
    std::filesystem::path WalPath() const;
    std::filesystem::path SnapshotPath() const;

private:
    EngineOptions options_;
    RecoveryInfo recovery_;
    std::unique_ptr<SqlEngine> sql_;
    std::unique_ptr<WalManager> wal_;
    std::unique_ptr<Server> server_;
    std::mutex write_mutex_;                 ///< Orders execute + WAL append for all statements
    idx_t records_since_checkpoint_ = 0;
};

} // namespace database

namespace Database = database;

#endif // WHISKDB_ENGINE_HPP
