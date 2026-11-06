#ifndef WHISKDB_SQL_TRANSACTION_GUARD_HPP
#define WHISKDB_SQL_TRANSACTION_GUARD_HPP

#include "errors.hpp"

#include <string>

namespace database {

class SqlEngine;

/**
 * @brief RAII nested savepoint guard.
 *
 * Issues SAVEPOINT whisk_sp_<n> on Begin.
 * Commit issues RELEASE whisk_sp_<n>.
 * Rollback issues ROLLBACK TO whisk_sp_<n> and RELEASE whisk_sp_<n>.
 * Destructor automatically rolls back if neither Commit nor Rollback was called.
 */
class TransactionGuard {
public:
    static Result<TransactionGuard> Begin(SqlEngine& engine);
    ~TransactionGuard();

    TransactionGuard(const TransactionGuard&) = delete;
    TransactionGuard& operator=(const TransactionGuard&) = delete;

    TransactionGuard(TransactionGuard&& other) noexcept;
    TransactionGuard& operator=(TransactionGuard&& other) noexcept;

public:
    Result<void> Commit();
    Result<void> Rollback();

    bool IsActive() const noexcept { return is_active_; }
    const std::string& SavepointName() const noexcept { return name_; }

private:
    TransactionGuard(SqlEngine& engine, std::string name) noexcept;

private:
    SqlEngine* engine_{nullptr};
    std::string name_;
    bool is_active_{false};
};

} // namespace database

namespace Database = database;

#endif // WHISKDB_SQL_TRANSACTION_GUARD_HPP
