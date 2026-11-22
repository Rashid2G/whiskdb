#include "sql/sql_engine.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>

namespace database {

namespace {

struct StmtResetter {
    sqlite3_stmt* stmt{nullptr};

    ~StmtResetter() {
        if (stmt != nullptr) {
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
        }
    }
};

struct ScopedStmtFinalizer {
    sqlite3_stmt* stmt{nullptr};

    ~ScopedStmtFinalizer() {
        if (stmt != nullptr) {
            sqlite3_finalize(stmt);
        }
    }
};

bool IsCatalogTable(const char* name) {
    if (name == nullptr) {
        return false;
    }
    constexpr std::string_view prefix = "_whisk_";
    std::string_view s(name);
    if (s.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) != prefix[i]) {
            return false;
        }
    }
    return true;
}

bool IsDdlStatement(std::string_view sql) {
    size_t pos = 0;
    while (pos < sql.size()) {
        if (std::isspace(static_cast<unsigned char>(sql[pos]))) {
            pos++;
            continue;
        }
        // Skip line comments: --
        if (pos + 1 < sql.size() && sql[pos] == '-' && sql[pos + 1] == '-') {
            pos += 2;
            while (pos < sql.size() && sql[pos] != '\n') {
                pos++;
            }
            continue;
        }
        // Skip block comments: /* ... */
        if (pos + 1 < sql.size() && sql[pos] == '/' && sql[pos + 1] == '*') {
            pos += 2;
            while (pos + 1 < sql.size() && !(sql[pos] == '*' && sql[pos + 1] == '/')) {
                pos++;
            }
            if (pos + 1 < sql.size()) {
                pos += 2;
            }
            continue;
        }
        break;
    }
    if (pos >= sql.size()) {
        return false;
    }
    size_t end = pos;
    while (end < sql.size() && std::isalpha(static_cast<unsigned char>(sql[end]))) {
        end++;
    }
    std::string_view word = sql.substr(pos, end - pos);
    auto iequals = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) {
                return false;
            }
        }
        return true;
    };
    return iequals(word, "CREATE") || iequals(word, "DROP") || iequals(word, "ALTER");
}

} // namespace

SqlEngine::SqlEngine(PrivateTag, sqlite3* db, idx_t cache_capacity)
    : db_(db),
      statement_cache_(std::make_unique<StatementCache>(cache_capacity)),
      internal_catalog_depth_(0) {
    if (db_ != nullptr) {
        sqlite3_set_authorizer(db_, &SqlEngine::AuthorizerCallback, this);
    }
}

SqlEngine::~SqlEngine() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (statement_cache_ != nullptr) {
        statement_cache_->Clear();
    }
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

Result<std::unique_ptr<SqlEngine>> SqlEngine::Open(idx_t cache_capacity) {
    sqlite3* db = nullptr;
    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) {
        std::string err_msg = (db != nullptr) ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
        if (db != nullptr) {
            sqlite3_close(db);
        }
        return MakeError(ErrorCode::InvalidArgument, err_msg);
    }

    // In-memory engine configuration pragmas
    char* errmsg = nullptr;
    const char* pragmas =
        "PRAGMA foreign_keys = ON; "
        "PRAGMA temp_store = MEMORY; "
        "PRAGMA synchronous = OFF; "
        "PRAGMA journal_mode = MEMORY;";
    rc = sqlite3_exec(db, pragmas, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string msg = (errmsg != nullptr) ? errmsg : sqlite3_errmsg(db);
        if (errmsg != nullptr) {
            sqlite3_free(errmsg);
        }
        sqlite3_close(db);
        return MakeError(ErrorCode::InvalidArgument, msg);
    }

    return std::make_unique<SqlEngine>(PrivateTag{}, db, cache_capacity);
}

SqlEngine::CatalogWriteGuard SqlEngine::AcquireCatalogWriteGuard() {
    return CatalogWriteGuard(this);
}

int SqlEngine::AuthorizerCallback(void* user_data, int action_code, const char* arg1,
                                  const char* arg2, const char* /*db_name*/,
                                  const char* /*trigger_or_view*/) {
    auto* self = static_cast<SqlEngine*>(user_data);
    if (self == nullptr) {
        return SQLITE_OK;
    }

    if (self->internal_catalog_depth_.load(std::memory_order_acquire) == 0) {
        // Deny when internal catalog access is not active:
        // 1) Direct table modification on _whisk_* tables
        if ((action_code == SQLITE_CREATE_TABLE || action_code == SQLITE_DROP_TABLE ||
             action_code == SQLITE_INSERT || action_code == SQLITE_UPDATE ||
             action_code == SQLITE_DELETE) &&
            IsCatalogTable(arg1)) {
            return SQLITE_DENY;
        }
        // 2) Table alteration on _whisk_* tables (arg1 is db name, arg2 is table name in SQLite)
        if (action_code == SQLITE_ALTER_TABLE && (IsCatalogTable(arg1) || IsCatalogTable(arg2))) {
            return SQLITE_DENY;
        }
        // 3) Indexes and triggers on _whisk_* tables (arg2 is target table name)
        if ((action_code == SQLITE_CREATE_INDEX || action_code == SQLITE_DROP_INDEX ||
             action_code == SQLITE_CREATE_TRIGGER || action_code == SQLITE_DROP_TRIGGER) &&
            IsCatalogTable(arg2)) {
            return SQLITE_DENY;
        }
    }

    return SQLITE_OK;
}

Error SqlEngine::MakeSqliteError(int rc, std::string_view override_msg) const {
    std::string msg;
    if (!override_msg.empty()) {
        msg = std::string(override_msg);
    } else {
        const char* msg_ptr = (db_ != nullptr) ? sqlite3_errmsg(db_) : sqlite3_errstr(rc);
        msg = (msg_ptr != nullptr) ? msg_ptr : sqlite3_errstr(rc);
    }

    ErrorCode code = ErrorCode::InvalidArgument;
    if ((rc & 0xFF) == SQLITE_AUTH) {
        code = ErrorCode::AccessDenied;
    } else if (rc == SQLITE_NOMEM) {
        code = ErrorCode::OutOfMemory;
    } else if (msg.find("no such table") != std::string::npos) {
        code = ErrorCode::TableDoesNotExist;
    } else if ((rc & 0xFF) == SQLITE_CONSTRAINT || msg.find("already exists") != std::string::npos) {
        code = ErrorCode::AlreadyExists;
    }

    return Error(code, std::move(msg));
}

Result<QueryResult> SqlEngine::Execute(std::string_view sql, std::span<const Value> params) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto is_space = [](char c) {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    };

    auto first_non_space = std::find_if_not(sql.begin(), sql.end(), is_space);
    if (first_non_space == sql.end()) {
        return MakeError(ErrorCode::InvalidArgument, "SQL statement is empty");
    }

    const bool internal_bypass = (internal_catalog_depth_.load(std::memory_order_acquire) > 0);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_stmt* temp_stmt = nullptr;

    if (internal_bypass) {
        // Critical Security Invariant: bypass statement cache to avoid caching privileged statements
        const char* tail = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &temp_stmt, &tail);
        if (rc != SQLITE_OK) {
            return std::unexpected(MakeSqliteError(rc));
        }
        if (temp_stmt == nullptr) {
            return MakeError(ErrorCode::InvalidArgument, "SQL statement is empty");
        }

        const char* end = sql.data() + sql.size();
        while (tail < end && is_space(*tail)) {
            ++tail;
        }
        if (tail < end) {
            sqlite3_finalize(temp_stmt);
            return MakeError(ErrorCode::InvalidArgument, "Multiple or trailing statements not allowed in Execute");
        }
        stmt = temp_stmt;
    } else {
        stmt = statement_cache_->Get(sql);
        if (stmt == nullptr) {
            sqlite3_stmt* prepared_stmt = nullptr;
            const char* tail = nullptr;
            int rc = sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &prepared_stmt, &tail);
            if (rc != SQLITE_OK) {
                return std::unexpected(MakeSqliteError(rc));
            }
            if (prepared_stmt == nullptr) {
                return MakeError(ErrorCode::InvalidArgument, "SQL statement is empty");
            }

            // Verify that exactly ONE statement was supplied (no trailing non-whitespace statements)
            const char* end = sql.data() + sql.size();
            while (tail < end && is_space(*tail)) {
                ++tail;
            }
            if (tail < end) {
                sqlite3_finalize(prepared_stmt);
                return MakeError(ErrorCode::InvalidArgument, "Multiple or trailing statements not allowed in Execute");
            }

            statement_cache_->Put(sql, prepared_stmt);
            stmt = prepared_stmt;
        }
    }

    // Temporary statements are finalized on exit; cached statements are reset on exit
    ScopedStmtFinalizer finalizer{temp_stmt};
    StmtResetter resetter{temp_stmt == nullptr ? stmt : nullptr};

    auto start_time = std::chrono::steady_clock::now();

    QueryResult result;
    result.is_mutation = (sqlite3_stmt_readonly(stmt) == 0);

    // Bind positional parameters (?1, ?2, ...)
    for (size_t i = 0; i < params.size(); ++i) {
        int param_idx = static_cast<int>(i + 1);
        int bind_rc = SQLITE_OK;
        const auto& val = params[i];

        std::visit([&](const auto& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                bind_rc = sqlite3_bind_null(stmt, param_idx);
            } else if constexpr (std::is_same_v<T, int64_t>) {
                bind_rc = sqlite3_bind_int64(stmt, param_idx, arg);
            } else if constexpr (std::is_same_v<T, double>) {
                bind_rc = sqlite3_bind_double(stmt, param_idx, arg);
            } else if constexpr (std::is_same_v<T, std::string>) {
                bind_rc = sqlite3_bind_text(stmt, param_idx, arg.data(), static_cast<int>(arg.size()), SQLITE_TRANSIENT);
            } else if constexpr (std::is_same_v<T, Blob>) {
                const void* blob_data = arg.data();
                if (blob_data == nullptr) {
                    static const uint8_t empty_byte = 0;
                    blob_data = &empty_byte;
                }
                bind_rc = sqlite3_bind_blob(stmt, param_idx, blob_data, static_cast<int>(arg.size()), SQLITE_TRANSIENT);
            }
        }, val);

        if (bind_rc != SQLITE_OK) {
            return std::unexpected(MakeSqliteError(bind_rc));
        }
    }

    // Populate column names for queries producing result rows
    int col_count = sqlite3_column_count(stmt);
    if (col_count > 0) {
        result.columns.reserve(static_cast<size_t>(col_count));
        for (int col_idx = 0; col_idx < col_count; ++col_idx) {
            const char* name = sqlite3_column_name(stmt, col_idx);
            result.columns.emplace_back(name != nullptr ? name : "");
        }
    }

    // Step statement and fetch rows
    while (true) {
        int step_rc = sqlite3_step(stmt);
        if (step_rc == SQLITE_ROW) {
            std::vector<Value> row;
            row.reserve(static_cast<size_t>(col_count));
            for (int col_idx = 0; col_idx < col_count; ++col_idx) {
                int col_type = sqlite3_column_type(stmt, col_idx);
                switch (col_type) {
                    case SQLITE_INTEGER:
                        row.emplace_back(static_cast<int64_t>(sqlite3_column_int64(stmt, col_idx)));
                        break;
                    case SQLITE_FLOAT:
                        row.emplace_back(sqlite3_column_double(stmt, col_idx));
                        break;
                    case SQLITE_TEXT: {
                        const auto* text_ptr = sqlite3_column_text(stmt, col_idx);
                        int bytes = sqlite3_column_bytes(stmt, col_idx);
                        row.emplace_back(text_ptr != nullptr ? std::string(reinterpret_cast<const char*>(text_ptr), static_cast<size_t>(bytes)) : std::string());
                        break;
                    }
                    case SQLITE_BLOB: {
                        const auto* blob_ptr = sqlite3_column_blob(stmt, col_idx);
                        int bytes = sqlite3_column_bytes(stmt, col_idx);
                        Blob blob;
                        if (bytes > 0 && blob_ptr != nullptr) {
                            const auto* byte_ptr = static_cast<const uint8_t*>(blob_ptr);
                            blob.assign(byte_ptr, byte_ptr + bytes);
                        }
                        row.emplace_back(std::move(blob));
                        break;
                    }
                    case SQLITE_NULL:
                    default:
                        row.emplace_back(std::monostate{});
                        break;
                }
            }
            result.rows.push_back(std::move(row));
        } else if (step_rc == SQLITE_DONE) {
            break;
        } else {
            return std::unexpected(MakeSqliteError(step_rc));
        }
    }

    if (result.is_mutation) {
        result.rows_affected = static_cast<idx_t>(sqlite3_changes64(db_));
        result.last_insert_rowid = sqlite3_last_insert_rowid(db_);
    } else {
        result.rows_affected = 0;
        result.last_insert_rowid = 0;
    }

    // Critical Safety Invariant: Invalidate statement cache on DDL without dangling pointers.
    // Reset statement and clear resetter before Clear() to prevent UAF in ~StmtResetter.
    if (IsDdlStatement(sql)) {
        if (resetter.stmt != nullptr) {
            sqlite3_reset(resetter.stmt);
            sqlite3_clear_bindings(resetter.stmt);
            resetter.stmt = nullptr;
        }
        statement_cache_->Clear();
    }

    auto end_time = std::chrono::steady_clock::now();
    result.exec_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count());

    return result;
}

Result<void> SqlEngine::ExecuteScript(std::string_view sql) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string sql_str(sql);
    char* errmsg = nullptr;
    int rc = sqlite3_exec(db_, sql_str.c_str(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string msg = (errmsg != nullptr) ? errmsg : sqlite3_errmsg(db_);
        if (errmsg != nullptr) {
            sqlite3_free(errmsg);
        }
        return std::unexpected(MakeSqliteError(rc, msg));
    }
    statement_cache_->Clear();
    return {};
}

Result<std::vector<uint8_t>> SqlEngine::Serialize() {
    std::lock_guard<std::mutex> lock(mutex_);

    sqlite3_int64 size = 0;
    unsigned char* ptr = sqlite3_serialize(db_, "main", &size, 0);
    if (ptr == nullptr && size > 0) {
        return MakeError(ErrorCode::OutOfMemory, "sqlite3_serialize memory allocation failed");
    }

    std::vector<uint8_t> bytes;
    if (size > 0 && ptr != nullptr) {
        bytes.assign(ptr, ptr + size);
        sqlite3_free(ptr);
    }
    return bytes;
}

Result<void> SqlEngine::Deserialize(std::span<const uint8_t> bytes) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Invalidate cached statements before disconnecting database
    statement_cache_->Clear();

    if (bytes.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "Cannot deserialize empty byte span");
    }

    auto* buf = static_cast<unsigned char*>(sqlite3_malloc64(bytes.size()));
    if (buf == nullptr) {
        return MakeError(ErrorCode::OutOfMemory, "sqlite3_malloc64 failed for deserialize");
    }

    std::memcpy(buf, bytes.data(), bytes.size());
    int rc = sqlite3_deserialize(db_, "main", buf, static_cast<sqlite3_int64>(bytes.size()),
                                 static_cast<sqlite3_int64>(bytes.size()),
                                 SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE);
    if (rc != SQLITE_OK) {
        return std::unexpected(MakeSqliteError(rc));
    }

    // Reapply authorizer after deserialize
    sqlite3_set_authorizer(db_, &SqlEngine::AuthorizerCallback, this);

    // Reapply session pragmas on the reconnected main database
    char* errmsg = nullptr;
    const char* pragmas =
        "PRAGMA foreign_keys = ON; "
        "PRAGMA temp_store = MEMORY; "
        "PRAGMA synchronous = OFF; "
        "PRAGMA journal_mode = MEMORY;";
    rc = sqlite3_exec(db_, pragmas, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string msg = (errmsg != nullptr) ? errmsg : sqlite3_errmsg(db_);
        if (errmsg != nullptr) {
            sqlite3_free(errmsg);
        }
        return std::unexpected(MakeSqliteError(rc, msg));
    }

    return {};
}

SqlEngine::CacheStats SqlEngine::GetCacheStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return CacheStats{
        .hits = statement_cache_->Hits(),
        .misses = statement_cache_->Misses(),
        .size = statement_cache_->Size()
    };
}

} // namespace database
