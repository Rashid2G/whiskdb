#include "connection_manager.hpp"
#include "errors.hpp"
#include "tcp_session.hpp"

namespace database {

void ConnectionManager::Start(std::shared_ptr<TcpSession> session) {
    // Validate that registered session pointer is non-null
    D_ASSERT(session != nullptr);
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.insert(std::move(session));
}

void ConnectionManager::Stop(std::shared_ptr<TcpSession> session) {
    if (!session) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.erase(session);
    }
    session->Stop();
}

void ConnectionManager::StopAll() {
    std::unordered_set<std::shared_ptr<TcpSession>> sessions_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_copy = std::move(sessions_);
        sessions_.clear();
    }
    for (const auto& session : sessions_copy) {
        if (session) {
            session->Stop();
        }
    }
}

idx_t ConnectionManager::ActiveConnections() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<idx_t>(sessions_.size());
}

} // namespace database
