#ifndef WHISKDB_NETWORK_SERVER_HPP
#define WHISKDB_NETWORK_SERVER_HPP

#include "common/types.hpp"
#include "errors.hpp"
#include "network/protocol.hpp"

#include <asio.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace database {

using RequestHandler = std::function<Response(const Request&)>;

class TcpSession;

/**
 * @brief Multi-threaded Asio asynchronous TCP server for WhiskDB.
 *
 * NOTE: Request handler invocations are synchronous on the Asio I/O threads.
 * If handlers perform heavy compute or blocking operations, this can stall the
 * respective I/O thread.
 */
class Server {
public:
    Server(std::string host, uint16_t port, RequestHandler handler, idx_t io_threads = 2);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

public:
    Result<void> Start();
    void Stop();
    uint16_t Port() const;
    idx_t ActiveConnections() const;

private:
    void DoAccept();
    void RegisterSession(const std::shared_ptr<TcpSession>& session);
    void UnregisterSession(const std::shared_ptr<TcpSession>& session);

    friend class TcpSession;

private:
    std::string host_;
    uint16_t port_{0};
    uint16_t bound_port_{0};
    RequestHandler handler_;
    idx_t io_threads_count_{2};

    asio::io_context io_context_;
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor_;
    std::vector<std::thread> worker_threads_;
    std::atomic<bool> is_running_{false};

    mutable std::mutex sessions_mutex_;
    std::unordered_set<std::shared_ptr<TcpSession>> active_sessions_;
    std::atomic<idx_t> active_connections_{0};
};

} // namespace database

#endif // WHISKDB_NETWORK_SERVER_HPP
