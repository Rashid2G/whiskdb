#ifndef WHISKDB_SQL_STATEMENT_CACHE_HPP
#define WHISKDB_SQL_STATEMENT_CACHE_HPP

#include "common/types.hpp"

#include <sqlite3.h>

#include <list>
#include <string>
#include <string_view>
#include <unordered_map>

namespace database {

/**
 * @brief Transparent string hash for string_view lookups in unordered_map without allocations.
 */
struct TransparentStringHash {
    using is_transparent = void;

    size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
    size_t operator()(const std::string& s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
};

/**
 * @brief LRU cache of compiled sqlite3_stmt* handles keyed by SQL text.
 *
 * Thread-safety: SqlEngine's internal mutex guarantees single-threaded access.
 */
class StatementCache {
public:
    explicit StatementCache(idx_t capacity = 128);
    ~StatementCache();

    StatementCache(const StatementCache&) = delete;
    StatementCache& operator=(const StatementCache&) = delete;

    StatementCache(StatementCache&& other) noexcept;
    StatementCache& operator=(StatementCache&& other) noexcept;

public:
    /// Look up cached statement. Returns nullptr on cache miss.
    /// On cache hit, statement is reset, bindings cleared, moved to LRU front, and hits counter incremented.
    sqlite3_stmt* Get(std::string_view sql);

    /// Insert compiled statement into the cache. Evicts LRU if at capacity.
    void Put(std::string_view sql, sqlite3_stmt* stmt);

    /// Finalize all cached statements and clear the cache.
    void Clear();

    /// Invalidate cache (alias for Clear).
    void Invalidate() {
        Clear();
    }

    idx_t Hits() const noexcept { return hits_; }
    idx_t Misses() const noexcept { return misses_; }
    idx_t Size() const noexcept { return static_cast<idx_t>(list_.size()); }
    idx_t Capacity() const noexcept { return capacity_; }

    void SetCapacity(idx_t new_capacity);

private:
    struct CacheEntry {
        std::string sql;
        sqlite3_stmt* stmt{nullptr};
    };

    idx_t capacity_{128};
    idx_t hits_{0};
    idx_t misses_{0};
    std::list<CacheEntry> list_;
    std::unordered_map<std::string, std::list<CacheEntry>::iterator, TransparentStringHash, std::equal_to<>> map_;
};

} // namespace database

namespace Database = database;

#endif // WHISKDB_SQL_STATEMENT_CACHE_HPP
