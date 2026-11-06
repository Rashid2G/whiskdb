#include "sql/transaction_guard.hpp"
#include "sql/sql_engine.hpp"

#include <atomic>
#include <format>

namespace database {

namespace {
std::atomic<uint64_t> g_savepoint_counter{1};
} // namespace

TransactionGuard::TransactionGuard(SqlEngine& engine, std::string name) noexcept
    : engine_(&engine),
      name_(std::move(name)),
      is_active_(true) {}

TransactionGuard::~TransactionGuard() {
    if (is_active_ && engine_ != nullptr) {
        // Drop-without-commit triggers automatic rollback
        (void)Rollback();
    }
}

TransactionGuard::TransactionGuard(TransactionGuard&& other) noexcept
    : engine_(other.engine_),
      name_(std::move(other.name_)),
      is_active_(other.is_active_) {
    other.engine_ = nullptr;
    other.is_active_ = false;
}

TransactionGuard& TransactionGuard::operator=(TransactionGuard&& other) noexcept {
    if (this != &other) {
        if (is_active_ && engine_ != nullptr) {
            (void)Rollback();
        }
        engine_ = other.engine_;
        name_ = std::move(other.name_);
        is_active_ = other.is_active_;
        other.engine_ = nullptr;
        other.is_active_ = false;
    }
    return *this;
}

Result<TransactionGuard> TransactionGuard::Begin(SqlEngine& engine) {
    uint64_t id = g_savepoint_counter.fetch_add(1, std::memory_order_relaxed);
    std::string name = std::format("whisk_sp_{}", id);

    auto res = engine.ExecuteScript(std::format("SAVEPOINT {};", name));
    if (!res.has_value()) {
        return MakeError(res.error().Code(), res.error().Message());
    }

    return TransactionGuard(engine, std::move(name));
}

Result<void> TransactionGuard::Commit() {
    if (!is_active_ || engine_ == nullptr) {
        return MakeError(ErrorCode::InvalidArgument, "TransactionGuard is not active or already completed");
    }

    is_active_ = false;
    auto res = engine_->ExecuteScript(std::format("RELEASE SAVEPOINT {};", name_));
    if (!res.has_value()) {
        return MakeError(res.error().Code(), res.error().Message());
    }
    return {};
}

Result<void> TransactionGuard::Rollback() {
    if (!is_active_ || engine_ == nullptr) {
        return MakeError(ErrorCode::InvalidArgument, "TransactionGuard is not active or already completed");
    }

    is_active_ = false;
    auto res = engine_->ExecuteScript(std::format("ROLLBACK TO SAVEPOINT {}; RELEASE SAVEPOINT {};", name_, name_));
    if (!res.has_value()) {
        return MakeError(res.error().Code(), res.error().Message());
    }
    return {};
}

} // namespace database
