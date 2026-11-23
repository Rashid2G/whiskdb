#include "engine.hpp"

#include "logger.hpp"
#include "sql/catalog.hpp"
#include "sql/transaction_guard.hpp"
#include "sql/value_codec.hpp"
#include "storage/snapshot_manager.hpp"
#include "storage/wal_reader.hpp"
#include "wasm/host_abi.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <limits>
#include <optional>

namespace database {

namespace {

constexpr std::string_view WAL_FILE_NAME = "whiskdb.wal";
constexpr std::string_view SNAPSHOT_FILE_NAME = "snapshot.whisk";

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

bool IsReadOnlyQuery(std::string_view sql) {
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
    return keyword == "SELECT" || keyword == "EXPLAIN" || keyword == "PRAGMA";
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
    options.wal_group_commit_ms = config.WalGroupCommitMs();
    options.wasm_max_fuel = config.WasmMaxFuel();
    options.wasm_max_memory_mb = config.WasmMaxMemoryMb();
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

    auto cat_init = SystemCatalog::Initialize(*sql_);
    if (!cat_init) {
        return std::unexpected(cat_init.error());
    }

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
            auto init_snap = SystemCatalog::Initialize(*sql_);
            if (!init_snap) {
                return std::unexpected(init_snap.error());
            }
            recovery_.snapshot_loaded = true;
            recovery_.snapshot_lsn = snapshot->lsn;
            next_lsn = snapshot->lsn + 1;
        } else if (snapshot.error().Code() != ErrorCode::NotFound) {
            return std::unexpected(snapshot.error());
        }

        // 2. Replay committed statements newer than the snapshot
        auto catalog_guard = sql_->AcquireCatalogWriteGuard();
        auto replay = ReplayWal(
            WalPath(), recovery_.snapshot_lsn,
            [this](lsn_t lsn, WalRecordType type, std::span<const uint8_t> payload) -> Result<void> {
                if (type == WalRecordType::Checkpoint) {
                    return {};
                }
                if (type == WalRecordType::SqlStatement) {
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
                }
                if (type == WalRecordType::SqlBatch) {
                    auto batch = DecodeStatementBatch(payload);
                    if (!batch) {
                        return MAKE_ERROR(ErrorCode::CorruptedData, "WAL batch decode failed at LSN {}: {}", lsn,
                                          batch.error().Message());
                    }
                    auto guard = TransactionGuard::Begin(*sql_);
                    if (!guard) {
                        return MAKE_ERROR(ErrorCode::CorruptedData, "WAL batch transaction begin failed at LSN {}: {}",
                                          lsn, guard.error().Message());
                    }
                    for (const auto& stmt : *batch) {
                        auto executed = sql_->Execute(stmt.sql, stmt.params);
                        if (!executed) {
                            return MAKE_ERROR(ErrorCode::CorruptedData, "WAL batch statement execution failed at LSN {}: {}",
                                              lsn, executed.error().Message());
                        }
                    }
                    auto committed = guard->Commit();
                    if (!committed) {
                        return MAKE_ERROR(ErrorCode::CorruptedData, "WAL batch transaction commit failed at LSN {}: {}",
                                          lsn, committed.error().Message());
                    }
                    return {};
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
    wal_options.group_commit_interval_us = static_cast<uint32_t>(options_.wal_group_commit_ms * 1000);
    auto wal = WalManager::Open(wal_options, next_lsn);
    if (!wal) {
        return std::unexpected(wal.error());
    }
    wal_ = std::move(*wal);

    auto wasm = WasmRuntime::Create(WasmConfig{options_.wasm_max_fuel, options_.wasm_max_memory_mb});
    if (!wasm) {
        return std::unexpected(wasm.error());
    }
    wasm_ = std::move(*wasm);

    auto abi = RegisterHostAbi(wasm_->Linker());
    if (!abi) {
        return std::unexpected(abi.error());
    }

    registry_ = std::make_unique<ModuleRegistry>(*wasm_);
    auto loaded_modules = registry_->LoadFromCatalog(*sql_);
    if (!loaded_modules) {
        return std::unexpected(loaded_modules.error());
    }

    LOG_INFO("Engine opened: mode={}, snapshot={} (lsn {}), WAL records replayed={}{}, WASM modules loaded={}",
             ToString(options_.durability), recovery_.snapshot_loaded ? "loaded" : "none",
             recovery_.snapshot_lsn, recovery_.wal_records_replayed,
             recovery_.wal_tail_truncated ? ", torn tail truncated" : "", *loaded_modules);
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
        std::optional<TransactionGuard> tx;
        if (!IsReadOnlyQuery(sql)) {
            auto tx_res = TransactionGuard::Begin(*sql_);
            if (!tx_res) {
                return std::unexpected(tx_res.error());
            }
            tx = std::move(*tx_res);
        }

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

            if (tx) {
                auto committed = tx->Commit();
                if (!committed) {
                    return std::unexpected(committed.error());
                }
            }

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
        } else if (tx) {
            auto committed = tx->Commit();
            if (!committed) {
                return std::unexpected(committed.error());
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

Result<WhiskPublishResponse> Engine::PublishModule(std::string_view name, std::span<const uint8_t> wasm_bytes) {
    // Engine::Recover always initializes sql_, wal_, and registry_ before Engine::Open returns
    D_ASSERT(sql_ != nullptr && wal_ != nullptr && registry_ != nullptr);

    lsn_t lsn = 0;
    bool must_wait = false;
    WhiskPublishResponse resp;

    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        auto tx = TransactionGuard::Begin(*sql_);
        if (!tx) {
            return std::unexpected(tx.error());
        }

        auto prepared = registry_->PreparePublish(*sql_, name, wasm_bytes);
        if (!prepared) {
            return std::unexpected(prepared.error());
        }

        const bool mutated = !prepared->mutating_statements.empty();
        if (mutated) {
            thread_local std::vector<uint8_t> payload;
            payload.clear();
            EncodeStatementBatch(payload, prepared->mutating_statements);

            auto appended = wal_->AppendNoWait(WalRecordType::SqlBatch, payload);
            if (!appended) {
                LOG_ERROR("WAL append failed during PublishModule('{}'): {}", name, appended.error());
                return std::unexpected(appended.error());
            }
            lsn = *appended;
            must_wait = true;
        }

        auto committed = tx->Commit();
        if (!committed) {
            return std::unexpected(committed.error());
        }

        if (mutated) {
            records_since_checkpoint_++;
            const bool auto_checkpoint = options_.durability != DurabilityMode::MemoryOnly &&
                                         options_.checkpoint_wal_records > 0 &&
                                         records_since_checkpoint_ >= options_.checkpoint_wal_records;
            if (auto_checkpoint) {
                auto checkpointed = CheckpointLocked();
                if (!checkpointed) {
                    LOG_WARN("Automatic checkpoint failed: {}", checkpointed.error());
                } else {
                    must_wait = false;
                }
            }
        }

        resp.module_name = prepared->module.name;
        resp.version = prepared->module.version;
        resp.sha256 = prepared->module.sha256;
        resp.exported_funcs = prepared->module.exported_funcs;
        registry_->CommitCachedModule(std::move(prepared->module));
    }

    if (must_wait) {
        auto durable = wal_->WaitDurable(lsn);
        if (!durable) {
            return std::unexpected(durable.error());
        }
    }
    return resp;
}

Result<WhiskCallResponse> Engine::CallWhisk(std::string_view module_name, std::string_view func_name,
                                            std::string_view args_json) {
    // Engine::Recover always initializes sql_, wal_, wasm_, and registry_ before Engine::Open returns
    D_ASSERT(sql_ != nullptr && wal_ != nullptr && wasm_ != nullptr && registry_ != nullptr);

    auto cached = registry_->Get(module_name);
    if (!cached) {
        return MAKE_ERROR(ErrorCode::NotFound, "Whisk module '{}' not found", module_name);
    }

    if (std::find(cached->exported_funcs.begin(), cached->exported_funcs.end(), func_name) ==
        cached->exported_funcs.end()) {
        return MAKE_ERROR(ErrorCode::NotFound, "Function '{}' not found in Whisk module '{}'",
                          func_name, module_name);
    }

    if (args_json.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        return MAKE_ERROR(ErrorCode::InvalidArgument, "Whisk arguments exceed maximum size");
    }

    lsn_t lsn = 0;
    bool must_wait = false;
    WhiskCallResponse resp;

    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        auto tx = TransactionGuard::Begin(*sql_);
        if (!tx) {
            return std::unexpected(tx.error());
        }

        WhiskCallContext ctx{sql_.get(), std::string(module_name), {}, std::nullopt};
        auto store = wasm_->CreateStore(&ctx);
        if (!store) {
            return std::unexpected(store.error());
        }

        wasmtime_instance_t instance{};
        wasm_trap_t* trap = nullptr;
        wasmtime_error_t* err = wasmtime_linker_instantiate(
            wasm_->Linker(), store->Context(), cached->compiled.get(), &instance, &trap);
        if (err) {
            return std::unexpected(WasmRuntime::TranslateError(err));
        }
        if (trap) {
            return std::unexpected(WasmRuntime::TranslateTrap(trap));
        }

        wasmtime_extern_t mem_ext{};
        if (!wasmtime_instance_export_get(store->Context(), &instance, "memory", 6, &mem_ext) ||
            mem_ext.kind != WASMTIME_EXTERN_MEMORY) {
            return MAKE_ERROR(ErrorCode::WasmError, "Whisk module '{}' missing exported 'memory'", module_name);
        }

        wasmtime_extern_t alloc_ext{};
        if (!wasmtime_instance_export_get(store->Context(), &instance, "whisk_alloc", 11, &alloc_ext) ||
            alloc_ext.kind != WASMTIME_EXTERN_FUNC) {
            return MAKE_ERROR(ErrorCode::WasmError, "Whisk module '{}' missing exported 'whisk_alloc'", module_name);
        }

        wasmtime_extern_t res_ptr_ext{};
        if (!wasmtime_instance_export_get(store->Context(), &instance, "whisk_result_ptr", 16, &res_ptr_ext) ||
            res_ptr_ext.kind != WASMTIME_EXTERN_FUNC) {
            return MAKE_ERROR(ErrorCode::WasmError, "Whisk module '{}' missing exported 'whisk_result_ptr'",
                              module_name);
        }

        wasmtime_extern_t res_len_ext{};
        if (!wasmtime_instance_export_get(store->Context(), &instance, "whisk_result_len", 16, &res_len_ext) ||
            res_len_ext.kind != WASMTIME_EXTERN_FUNC) {
            return MAKE_ERROR(ErrorCode::WasmError, "Whisk module '{}' missing exported 'whisk_result_len'",
                              module_name);
        }

        wasmtime_extern_t fn_ext{};
        if (!wasmtime_instance_export_get(store->Context(), &instance, func_name.data(), func_name.size(), &fn_ext) ||
            fn_ext.kind != WASMTIME_EXTERN_FUNC) {
            return MAKE_ERROR(ErrorCode::NotFound, "Function '{}' not found in Whisk module '{}'",
                              func_name, module_name);
        }

        int32_t args_ptr = 0;
        const int32_t args_len = static_cast<int32_t>(args_json.size());
        if (!args_json.empty()) {
            wasmtime_val_t alloc_arg[1];
            alloc_arg[0].kind = WASMTIME_I32;
            alloc_arg[0].of.i32 = args_len;
            wasmtime_val_t alloc_res[1];
            err = wasmtime_func_call(store->Context(), &alloc_ext.of.func, alloc_arg, 1, alloc_res, 1, &trap);
            if (err) {
                return std::unexpected(WasmRuntime::TranslateError(err));
            }
            if (trap) {
                return std::unexpected(WasmRuntime::TranslateTrap(trap));
            }
            if (alloc_res[0].kind != WASMTIME_I32 || alloc_res[0].of.i32 <= 0) {
                return MAKE_ERROR(ErrorCode::WasmError, "whisk_alloc failed in module '{}'", module_name);
            }
            args_ptr = alloc_res[0].of.i32;

            uint8_t* mem_data = wasmtime_memory_data(store->Context(), &mem_ext.of.memory);
            const size_t mem_size = wasmtime_memory_data_size(store->Context(), &mem_ext.of.memory);
            if (static_cast<uint64_t>(args_ptr) + args_json.size() > mem_size) {
                return MAKE_ERROR(ErrorCode::WasmError, "whisk_alloc returned out-of-bounds pointer in module '{}'",
                                  module_name);
            }
            std::memcpy(mem_data + args_ptr, args_json.data(), args_json.size());
        }

        wasmtime_val_t call_args[2];
        call_args[0].kind = WASMTIME_I32;
        call_args[0].of.i32 = args_ptr;
        call_args[1].kind = WASMTIME_I32;
        call_args[1].of.i32 = args_len;
        wasmtime_val_t call_res[1];
        err = wasmtime_func_call(store->Context(), &fn_ext.of.func, call_args, 2, call_res, 1, &trap);
        const uint64_t fuel_used = store->ConsumedFuel();
        if (err) {
            return std::unexpected(WasmRuntime::TranslateError(err));
        }
        if (trap) {
            return std::unexpected(WasmRuntime::TranslateTrap(trap));
        }
        if (call_res[0].kind != WASMTIME_I32) {
            return MAKE_ERROR(ErrorCode::WasmError, "Whisk '{}.{}' did not return i32 status",
                              module_name, func_name);
        }
        const int32_t rc = call_res[0].of.i32;

        wasmtime_val_t ptr_res[1];
        err = wasmtime_func_call(store->Context(), &res_ptr_ext.of.func, nullptr, 0, ptr_res, 1, &trap);
        if (err) {
            return std::unexpected(WasmRuntime::TranslateError(err));
        }
        if (trap) {
            return std::unexpected(WasmRuntime::TranslateTrap(trap));
        }
        wasmtime_val_t len_res[1];
        err = wasmtime_func_call(store->Context(), &res_len_ext.of.func, nullptr, 0, len_res, 1, &trap);
        if (err) {
            return std::unexpected(WasmRuntime::TranslateError(err));
        }
        if (trap) {
            return std::unexpected(WasmRuntime::TranslateTrap(trap));
        }

        if (ptr_res[0].kind != WASMTIME_I32 || len_res[0].kind != WASMTIME_I32) {
            return MAKE_ERROR(ErrorCode::WasmError, "Invalid result buffer metadata from Whisk '{}.{}'",
                              module_name, func_name);
        }
        const int32_t out_ptr = ptr_res[0].of.i32;
        const int32_t out_len = len_res[0].of.i32;

        // Re-query guest memory after the call in case linear memory grew
        const uint8_t* mem_data = wasmtime_memory_data(store->Context(), &mem_ext.of.memory);
        const size_t mem_size = wasmtime_memory_data_size(store->Context(), &mem_ext.of.memory);
        if (out_ptr < 0 || out_len < 0 ||
            static_cast<uint64_t>(out_ptr) + static_cast<uint64_t>(out_len) > mem_size) {
            return MAKE_ERROR(ErrorCode::WasmError, "Whisk '{}.{}' returned out-of-bounds result buffer",
                              module_name, func_name);
        }
        std::string output_str(reinterpret_cast<const char*>(mem_data + out_ptr),
                               static_cast<size_t>(out_len));

        if (rc != 0) {
            return MAKE_ERROR(ErrorCode::WasmError, "Whisk '{}.{}' failed (rc={}): {}",
                              module_name, func_name, rc, output_str);
        }

        const bool mutated = !ctx.mutating_statements.empty();
        if (mutated) {
            thread_local std::vector<uint8_t> payload;
            payload.clear();
            EncodeStatementBatch(payload, ctx.mutating_statements);

            auto appended = wal_->AppendNoWait(WalRecordType::SqlBatch, payload);
            if (!appended) {
                LOG_ERROR("WAL append failed during CallWhisk('{}.{}'): {}", module_name, func_name,
                          appended.error());
                return std::unexpected(appended.error());
            }
            lsn = *appended;
            must_wait = true;
        }

        auto committed = tx->Commit();
        if (!committed) {
            return std::unexpected(committed.error());
        }

        if (mutated) {
            records_since_checkpoint_++;
            const bool auto_checkpoint = options_.durability != DurabilityMode::MemoryOnly &&
                                         options_.checkpoint_wal_records > 0 &&
                                         records_since_checkpoint_ >= options_.checkpoint_wal_records;
            if (auto_checkpoint) {
                auto checkpointed = CheckpointLocked();
                if (!checkpointed) {
                    LOG_WARN("Automatic checkpoint failed: {}", checkpointed.error());
                } else {
                    must_wait = false;
                }
            }
        }

        resp.result_json = std::move(output_str);
        resp.fuel_used = fuel_used;
    }

    if (must_wait) {
        auto durable = wal_->WaitDurable(lsn);
        if (!durable) {
            return std::unexpected(durable.error());
        }
    }
    return resp;
}

Result<WhiskListResponse> Engine::ListModules() {
    // Engine::Recover always initializes registry_ before Engine::Open returns
    D_ASSERT(registry_ != nullptr);
    auto cached_list = registry_->List();
    WhiskListResponse resp;
    resp.modules.reserve(cached_list.size());
    for (auto& mod : cached_list) {
        WhiskModuleSummary summary;
        summary.name = std::move(mod.name);
        summary.version = mod.version;
        summary.sha256 = std::move(mod.sha256);
        summary.exported_funcs = std::move(mod.exported_funcs);
        summary.updated_at = mod.updated_at;
        resp.modules.push_back(std::move(summary));
    }
    return resp;
}

SqlEngine::CacheStats Engine::GetSqlCacheStats() const {
    // Engine::Recover always initializes sql_ before Engine::Open returns
    D_ASSERT(sql_ != nullptr);
    return sql_->GetCacheStats();
}

Result<std::pair<lsn_t, bool>> Engine::AppendSqlBatchLocked(std::span<const DecodedStatement> statements) {
    thread_local std::vector<uint8_t> payload;
    payload.clear();
    EncodeStatementBatch(payload, statements);

    auto appended = wal_->AppendNoWait(WalRecordType::SqlBatch, payload);
    if (!appended) {
        LOG_ERROR("WAL append failed for batch: {}", appended.error());
        return std::unexpected(appended.error());
    }
    lsn_t lsn = *appended;
    bool must_wait = true;
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
    return std::make_pair(lsn, must_wait);
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
    case Opcode::WhiskPublish: {
        auto req = DecodeWhiskPublishRequest(request.payload);
        if (!req) {
            return ErrorResponse(req.error(), elapsed_us());
        }
        auto result = PublishModule(req->module_name, req->wasm_bytes);
        if (!result) {
            return ErrorResponse(result.error(), elapsed_us());
        }
        EncodeWhiskPublishResponse(response.payload, *result);
        break;
    }
    case Opcode::WhiskCall: {
        auto req = DecodeWhiskCallRequest(request.payload);
        if (!req) {
            return ErrorResponse(req.error(), elapsed_us());
        }
        auto result = CallWhisk(req->module_name, req->func_name, req->args_json);
        if (!result) {
            return ErrorResponse(result.error(), elapsed_us());
        }
        EncodeWhiskCallResponse(response.payload, *result);
        break;
    }
    case Opcode::WhiskList: {
        if (!request.payload.empty()) {
            return ErrorResponse(Error(ErrorCode::ProtocolError, "WhiskList request payload must be empty"),
                                 elapsed_us());
        }
        auto result = ListModules();
        if (!result) {
            return ErrorResponse(result.error(), elapsed_us());
        }
        EncodeWhiskListResponse(response.payload, *result);
        break;
    }
    case Opcode::Logs: {
        auto req = DecodeLogsRequest(request.payload);
        if (!req) {
            return ErrorResponse(req.error(), elapsed_us());
        }
        std::string text;
        for (const auto& line : Logger::GetRecentLogs(req->limit)) {
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
