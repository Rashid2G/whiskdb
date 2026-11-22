#ifndef WHISKDB_SQL_SQL_ENGINE_HPP
#define WHISKDB_SQL_SQL_ENGINE_HPP

#include "common/types.hpp"
#include "errors.hpp"
#include "sql/sql_types.hpp"
#include "sql/statement_cache.hpp"

#include <sqlite3.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

namespace database {

class SqlEngine {
public:
    struct CacheStats {
        idx_t hits;
        idx_t misses;
        idx_t size;
    };

    struct PrivateTag {
        explicit PrivateTag() = default;
    };

    class CatalogWriteGuard {
    public:
        CatalogWriteGuard() noexcept = default;
        explicit CatalogWriteGuard(SqlEngine* engine) noexcept : engine_(engine) {
            if (engine_ != nullptr) {
                engine_->internal_catalog_depth_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        ~CatalogWriteGuard() {
            if (engine_ != nullptr) {
                engine_->internal_catalog_depth_.fetch_sub(1, std::memory_order_release);
            }
        }

        CatalogWriteGuard(const CatalogWriteGuard&) = delete;
        CatalogWriteGuard& operator=(const CatalogWriteGuard&) = delete;

        CatalogWriteGuard(CatalogWriteGuard&& other) noexcept : engine_(other.engine_) {
            other.engine_ = nullptr;
        }
        CatalogWriteGuard& operator=(CatalogWriteGuard&& other) noexcept {
            if (this != &other) {
                if (engine_ != nullptr) {
                    engine_->internal_catalog_depth_.fetch_sub(1, std::memory_order_release);
                }
                engine_ = other.engine_;
                other.engine_ = nullptr;
            }
            return *this;
        }

    private:
        SqlEngine* engine_{nullptr};
    };

public:
    static Result<std::unique_ptr<SqlEngine>> Open(idx_t cache_capacity = 128);
    explicit SqlEngine(PrivateTag, sqlite3* db, idx_t cache_capacity);
    ~SqlEngine();

    SqlEngine(const SqlEngine&) = delete;
    SqlEngine& operator=(const SqlEngine&) = delete;
    SqlEngine(SqlEngine&&) = delete;
    SqlEngine& operator=(SqlEngine&&) = delete;

public:
    [[nodiscard]] CatalogWriteGuard AcquireCatalogWriteGuard();

    // Execute exactly ONE statement (error ErrorCode::InvalidArgument if trailing non-whitespace statements).
    // Uses the prepared-statement cache. Binds params positionally (?1..).
    // Fills QueryResult incl. columns, typed rows, rows_affected, last_insert_rowid, exec_us (steady_clock, microseconds), is_mutation (= !sqlite3_stmt_readonly(stmt), so true for DDL/DML).
    Result<QueryResult> Execute(std::string_view sql, std::span<const Value> params = {});

    // Run a multi-statement script (used for WAL replay / bootstrap). No result rows.
    Result<void> ExecuteScript(std::string_view sql);

    // Whole-database snapshot via sqlite3_serialize / sqlite3_deserialize (replace contents; invalidate the statement cache first).
    Result<std::vector<uint8_t>> Serialize();
    Result<void> Deserialize(std::span<const uint8_t> bytes);

    CacheStats GetCacheStats() const;

private:
    Error MakeSqliteError(int rc, std::string_view override_msg = {}) const;

    static int AuthorizerCallback(void* user_data, int action_code, const char* arg1,
                                  const char* arg2, const char* db_name,
                                  const char* trigger_or_view);

private:
    mutable std::mutex mutex_;
    sqlite3* db_{nullptr};
    std::unique_ptr<StatementCache> statement_cache_;
    std::atomic<int> internal_catalog_depth_{0};
};

} // namespace database

namespace Database = database;

#endif // WHISKDB_SQL_SQL_ENGINE_HPP
