#ifndef WHISKDB_NETWORK_HPP
#define WHISKDB_NETWORK_HPP

#include "common/types.hpp"
#include "config.hpp"
#include "connection_manager.hpp"
#include "framing.hpp"
#include "network/protocol.hpp"
#include "tcp_session.hpp"

#include <asio.hpp>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace database {

/**
 * @brief High-performance asynchronous TCP server using standalone Asio.
 *
 * Implements persistent TCP connections with length-prefixed framing and
 * optional binary Opcode dispatching via ProtocolDispatcher.
 */
class Server {
public:
    explicit Server(const Config& config);
    Server(std::string host, uint16_t port);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

public:
    /// Configure the string request handler callback
    void SetRequestHandler(RequestHandler handler);
    void set_request_handler(RequestHandler handler) { SetRequestHandler(std::move(handler)); }

    /// Configure the zero-copy binary request handler callback
    void SetBinaryRequestHandler(BinaryRequestHandler handler);

    /// Bind a ProtocolDispatcher to handle binary Opcode frames automatically
    void UseProtocolDispatcher(std::shared_ptr<ProtocolDispatcher> dispatcher);

    /// Bind the acceptor socket (populates Port() when port_ == 0)
    void Bind();

    /// Start listening and run the event loop (blocking)
    void Run();
    void start() { Run(); }

    /// Start listening on background worker thread(s) (non-blocking)
    void StartBackground(idx_t worker_threads = 1);

    /// Stop server and disconnect all active connections
    void Stop();
    void stop() { Stop(); }

    /// Get bound port (useful when initialized with port 0 for tests)
    uint16_t Port() const noexcept { return port_; }
    uint16_t port() const noexcept { return port_; }

    /// Get current active connection count
    idx_t ActiveConnections() const;
    idx_t active_connections() const { return ActiveConnections(); }

    /// Access connection manager
    ConnectionManager& GetConnectionManager() noexcept { return connection_manager_; }
    ConnectionManager& connection_manager() noexcept { return connection_manager_; }

private:
    void DoAccept();

private:
    std::string host_;
    uint16_t port_;
    asio::io_context io_context_;
    asio::ip::tcp::acceptor acceptor_;
    ConnectionManager connection_manager_;
    RequestHandler request_handler_;
    BinaryRequestHandler binary_request_handler_;
    std::shared_ptr<ProtocolDispatcher> protocol_dispatcher_;
    std::vector<std::thread> worker_threads_;
    std::atomic<bool> is_bound_{false};
};

} // namespace database

namespace Database = database;

#endif // WHISKDB_NETWORK_HPP
