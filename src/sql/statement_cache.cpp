#include "sql/statement_cache.hpp"
#include "errors.hpp"

namespace database {

StatementCache::StatementCache(idx_t capacity)
    : capacity_(capacity) {}

StatementCache::~StatementCache() {
    Clear();
}

StatementCache::StatementCache(StatementCache&& other) noexcept
    : capacity_(other.capacity_),
      hits_(other.hits_),
      misses_(other.misses_),
      list_(std::move(other.list_)),
      map_(std::move(other.map_)) {
    other.capacity_ = 0;
    other.hits_ = 0;
    other.misses_ = 0;
}

StatementCache& StatementCache::operator=(StatementCache&& other) noexcept {
    if (this != &other) {
        Clear();
        capacity_ = other.capacity_;
        hits_ = other.hits_;
        misses_ = other.misses_;
        list_ = std::move(other.list_);
        map_ = std::move(other.map_);
        other.capacity_ = 0;
        other.hits_ = 0;
        other.misses_ = 0;
    }
    return *this;
}

sqlite3_stmt* StatementCache::Get(std::string_view sql) {
    auto it = map_.find(sql);
    if (it == map_.end()) {
        ++misses_;
        return nullptr;
    }

    ++hits_;
    // Move accessed entry to front of LRU list
    list_.splice(list_.begin(), list_, it->second);
    sqlite3_stmt* stmt = it->second->stmt;

    // Invariant: cached statement must not be null
    D_ASSERT(stmt != nullptr);

    // Reset and clear bindings on reuse as required
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    return stmt;
}

void StatementCache::Put(std::string_view sql, sqlite3_stmt* stmt) {
    if (stmt == nullptr) {
        return;
    }

    if (capacity_ == 0) {
        sqlite3_finalize(stmt);
        return;
    }

    auto it = map_.find(sql);
    if (it != map_.end()) {
        if (it->second->stmt != stmt) {
            // Evict and finalize previous statement handle for this key
            sqlite3_finalize(it->second->stmt);
            it->second->stmt = stmt;
        }
        list_.splice(list_.begin(), list_, it->second);
        return;
    }

    // Evict least recently used entries if at capacity
    while (list_.size() >= capacity_) {
        auto& back = list_.back();
        // Invariant: evicted statement handle is valid
        D_ASSERT(back.stmt != nullptr);
        sqlite3_finalize(back.stmt);
        map_.erase(back.sql);
        list_.pop_back();
    }

    list_.push_front(CacheEntry{std::string(sql), stmt});
    map_.emplace(list_.front().sql, list_.begin());
}

void StatementCache::Clear() {
    for (auto& entry : list_) {
        if (entry.stmt != nullptr) {
            sqlite3_finalize(entry.stmt);
            entry.stmt = nullptr;
        }
    }
    list_.clear();
    map_.clear();
}

void StatementCache::SetCapacity(idx_t new_capacity) {
    capacity_ = new_capacity;
    while (list_.size() > capacity_) {
        auto& back = list_.back();
        // Invariant: evicted statement handle is valid
        D_ASSERT(back.stmt != nullptr);
        sqlite3_finalize(back.stmt);
        map_.erase(back.sql);
        list_.pop_back();
    }
}

} // namespace database
