#include "engine.hpp"

#include "logger.hpp"
#include "sql/value_codec.hpp"
#include "storage/snapshot_manager.hpp"
#include "storage/wal_reader.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>

namespace database {

namespace {

constexpr std::string_view WAL_FILE_NAME = "whiskdb.wal";
constexpr std::string_view SNAPSHOT_FILE_NAME = "snapshot.whisk";
constexpr idx_t LOGS_FETCH_LINES = 200;

/// Statements that change transaction state cannot be logged statement-by-statement:
/// replaying a rolled-back transaction would resurrect its writes.
bool IsTransactionControl(std::string_view sql) {
    idx_t pos = 0;
    while (pos < sql.size() && std::isspace(static_cast<unsigned char>(sql[pos]))) {
        pos++;
    }
    idx_t end = pos;
    while (end < sql.size() && std::isalpha(static_cast<unsigned char>(sql[end]))) {
        end++;
    }
    std::string keyword(sql.substr(pos, end - pos));
    std::transform(keyword.begin(), keyword.end(), keyword.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return keyword == "BEGIN" || keyword == "COMMIT" || keyword == "END" || keyword == "ROLLBACK" ||
           keyword == "SAVEPOINT" || keyword == "RELEASE";
}

std::vector<uint8_t> ToBytes(std::string_view text) {
    return {text.begin(), text.end()};
}

Response ErrorResponse(const Error& error, uint64_t exec_us) {
    Response response;
    response.status = error.Code();
    response.exec_us = exec_us;
    response.payload = ToBytes(error.Message());
    return response;
}

} // namespace

Result<EngineOptions> EngineOptions::FromConfig(const Config& config) {
    EngineOptions options;
    options.data_dir = config.DbFilePath();
    options.host = config.Host();
    options.port = config.Port();
    options.checkpoint_wal_records = config.CheckpointWalRecords();
    auto mode = ParseDurabilityMode(config.DurabilityModeName());
    if (!mode) {
        return std::unexpected(mode.error());
    }
    options.durability = *mode;
    return options;
}

Engine::Engine(EngineOptions options) : options_(std::move(options)) {}

Engine::~Engine() {
    StopServer();
}

std::filesystem::path Engine::WalPath() const {
    return options_.data_dir / WAL_FILE_NAME;
}

std::filesystem::path Engine::SnapshotPath() const {
    return options_.data_dir / SNAPSHOT_FILE_NAME;
}

Result<std::unique_ptr<Engine>> Engine::Open(EngineOptions options) {
    std::unique_ptr<Engine> engine(new Engine(std::move(options)));
    auto recovered = engine->Recover();
    if (!recovered) {
        return std::unexpected(recovered.error());
    }
    return engine;
}

Result<void> Engine::Recover() {
    auto sql = SqlEngine::Open();
    if (!sql) {
        return std::unexpected(sql.error());
    }
    sql_ = std::move(*sql);

    lsn_t next_lsn = 1;
    const bool durable = options_.durability != DurabilityMode::MemoryOnly;

    if (durable) {
        std::error_code ec;
        std::filesystem::create_directories(options_.data_dir, ec);
        if (ec) {
            return MAKE_ERROR(ErrorCode::IoError, "cannot create data directory '{}': {}",
                              options_.data_dir.string(), ec.message());
        }

        // 1. Latest snapshot (if any)
        auto snapshot = SnapshotManager::Load(SnapshotPath());
        if (snapshot) {
            auto loaded = sql_->Deserialize(snapshot->bytes);
            if (!loaded) {
                return std::unexpected(loaded.error());
            }
            recovery_.snapshot_loaded = true;
            recovery_.snapshot_lsn = snapshot->lsn;
            next_lsn = snapshot->lsn + 1;
        } else if (snapshot.error().Code() != ErrorCode::NotFound) {
            return std::unexpected(snapshot.error());
        }

        // 2. Replay committed statements newer than the snapshot
        auto replay = ReplayWal(
            WalPath(), recovery_.snapshot_lsn,
            [this](lsn_t lsn, WalRecordType type, std::span<const uint8_t> payload) -> Result<void> {
                if (type != WalRecordType::SqlStatement) {
                    return {};
                }
                auto statement = DecodeStatement(payload);
                if (!statement) {
                    return std::unexpected(statement.error());
                }
                auto executed = sql_->Execute(statement->sql, statement->params);
                if (!executed) {
                    return MAKE_ERROR(ErrorCode::CorruptedData, "WAL replay failed at LSN {}: {}", lsn,
                                      executed.error().Message());
                }
                return {};
            });
        if (!replay) {
            return std::unexpected(replay.error());
        }
        recovery_.wal_records_replayed = replay->records_applied;
        recovery_.wal_tail_truncated = replay->truncated_tail;
        next_lsn = std::max<lsn_t>(next_lsn, replay->last_lsn + 1);
        records_since_checkpoint_ = replay->records_applied;
    }

    WalOptions wal_options;
    wal_options.path = WalPath();
    wal_options.mode = options_.durability;
    auto wal = WalManager::Open(wal_options, next_lsn);
    if (!wal) {
        return std::unexpected(wal.error());
    }
    wal_ = std::move(*wal);

    LOG_INFO("Engine opened: mode={}, snapshot={} (lsn {}), WAL records replayed={}{}",
             ToString(options_.durability), recovery_.snapshot_loaded ? "loaded" : "none",
             recovery_.snapshot_lsn, recovery_.wal_records_replayed,
             recovery_.wal_tail_truncated ? ", torn tail truncated" : "");
    return {};
}

Result<QueryResult> Engine::ExecuteSql(std::string_view sql, std::span<const Value> params) {
    if (IsTransactionControl(sql)) {
        return MAKE_ERROR(ErrorCode::InvalidArgument,
                          "explicit transaction control (BEGIN/COMMIT/ROLLBACK/SAVEPOINT) is not supported "
                          "over SQL; each statement is atomic");
    }

    lsn_t lsn = 0;
    bool must_wait = false;
    QueryResult result;
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        auto executed = sql_->Execute(sql, params);
        if (!executed) {
            return executed;
        }
        result = std::move(*executed);

        if (result.is_mutation) {
            // Reused per thread so the hot path does not reallocate the WAL payload buffer
            thread_local std::vector<uint8_t> payload;
            payload.clear();
            EncodeStatement(payload, sql, params);

            auto appended = wal_->AppendNoWait(WalRecordType::SqlStatement, payload);
            if (!appended) {
                LOG_ERROR("WAL append failed after executing a statement: {}", appended.error());
                return std::unexpected(appended.error());
            }
            lsn = *appended;
            must_wait = true;
            records_since_checkpoint_++;

            const bool auto_checkpoint = options_.durability != DurabilityMode::MemoryOnly &&
                                         options_.checkpoint_wal_records > 0 &&
                                         records_since_checkpoint_ >= options_.checkpoint_wal_records;
            if (auto_checkpoint) {
                auto checkpointed = CheckpointLocked();
                if (!checkpointed) {
                    LOG_WARN("Automatic checkpoint failed: {}", checkpointed.error());
                } else {
                    must_wait = false;  // the snapshot already covers this record
                }
            }
        }
    }

    if (must_wait) {
        // Waiting outside the lock lets concurrent writers share one fdatasync (group commit)
        auto durable = wal_->WaitDurable(lsn);
        if (!durable) {
            return std::unexpected(durable.error());
        }
    }
    return result;
}

Result<lsn_t> Engine::CheckpointLocked() {
    if (options_.durability == DurabilityMode::MemoryOnly) {
        return MAKE_ERROR(ErrorCode::InvalidArgument, "snapshots are disabled in memory_only mode");
    }
    auto flushed = wal_->Flush();
    if (!flushed) {
        return std::unexpected(flushed.error());
    }
    const lsn_t lsn = wal_->LastLsn();
    auto bytes = sql_->Serialize();
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    auto saved = SnapshotManager::Save(SnapshotPath(), lsn, *bytes);
    if (!saved) {
        return std::unexpected(saved.error());
    }
    // Only after the snapshot is durable is it safe to drop the WAL it replaces
    auto reset = wal_->Reset(lsn + 1);
    if (!reset) {
        return std::unexpected(reset.error());
    }
    records_since_checkpoint_ = 0;
    LOG_INFO("Checkpoint complete at LSN {} ({} bytes)", lsn, bytes->size());
    return lsn;
}

Result<lsn_t> Engine::Checkpoint() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    return CheckpointLocked();
}

Response Engine::Handle(const Request& request) {
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed_us = [&started]() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now() - started)
                                         .count());
    };

    Response response;
    switch (request.opcode) {
    case Opcode::Ping:
        response.payload = ToBytes("PONG");
        break;
    case Opcode::SqlQuery: {
        auto statement = DecodeStatement(request.payload);
        if (!statement) {
            return ErrorResponse(statement.error(), elapsed_us());
        }
        auto result = ExecuteSql(statement->sql, statement->params);
        if (!result) {
            return ErrorResponse(result.error(), elapsed_us());
        }
        result->exec_us = elapsed_us();
        EncodeQueryResult(response.payload, *result);
        break;
    }
    case Opcode::Logs: {
        std::string text;
        for (const auto& line : Logger::GetRecentLogs(LOGS_FETCH_LINES)) {
            text += line;
            text += '\n';
        }
        response.payload = ToBytes(text);
        break;
    }
    case Opcode::Snapshot: {
        auto lsn = Checkpoint();
        if (!lsn) {
            return ErrorResponse(lsn.error(), elapsed_us());
        }
        response.payload = ToBytes(std::format("snapshot saved at LSN {}", *lsn));
        break;
    }
    default:
        return ErrorResponse(Error(ErrorCode::NotImplemented, "opcode not available in this version"),
                             elapsed_us());
    }
    response.exec_us = elapsed_us();
    return response;
}

Result<void> Engine::StartServer() {
    if (server_) {
        return MAKE_ERROR(ErrorCode::AlreadyExists, "server already running");
    }
    server_ = std::make_unique<Server>(
        options_.host, options_.port, [this](const Request& request) { return Handle(request); },
        options_.io_threads);
    auto started = server_->Start();
    if (!started) {
        server_.reset();
        return started;
    }
    LOG_INFO("WhiskDB listening on {}:{}", options_.host, server_->Port());
    return {};
}

void Engine::StopServer() {
    if (server_) {
        server_->Stop();
        server_.reset();
    }
}

uint16_t Engine::Port() const {
    return server_ ? server_->Port() : options_.port;
}

} // namespace database
