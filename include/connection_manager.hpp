#ifndef WHISKDB_CONNECTION_MANAGER_HPP
#define WHISKDB_CONNECTION_MANAGER_HPP

#include "common/types.hpp"

#include <memory>
#include <mutex>
#include <unordered_set>

namespace database {

class TcpSession;

/**
 * @brief Manages active TCP connections, allowing thread-safe tracking,
 *        graceful disconnection, and statistics for active sessions.
 */
class ConnectionManager {
public:
    ConnectionManager() = default;
    ~ConnectionManager() = default;

    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;

public:
    /// Register a newly connected session
    void Start(std::shared_ptr<TcpSession> session);
    void start(std::shared_ptr<TcpSession> session) { Start(std::move(session)); }

    /// Deregister a session (called when client disconnects or encounters error)
    void Stop(std::shared_ptr<TcpSession> session);
    void stop(std::shared_ptr<TcpSession> session) { Stop(std::move(session)); }

    /// Disconnect all active sessions (e.g. during server shutdown)
    void StopAll();
    void stop_all() { StopAll(); }

    /// Get current number of active persistent connections
    idx_t ActiveConnections() const;
    idx_t active_connections() const { return ActiveConnections(); }

private:
    mutable std::mutex mutex_;
    std::unordered_set<std::shared_ptr<TcpSession>> sessions_;
};

} // namespace database

namespace Database = database;

#endif // WHISKDB_CONNECTION_MANAGER_HPP
