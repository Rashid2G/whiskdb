#ifndef WHISKDB_NETWORK_TCP_CLIENT_HPP
#define WHISKDB_NETWORK_TCP_CLIENT_HPP

#include "common/types.hpp"
#include "errors.hpp"
#include "network/protocol.hpp"

#include <asio.hpp>
#include <chrono>
#include <string>
#include <vector>

namespace database {

/**
 * @brief Blocking persistent TCP client with reused buffers and timeout support.
 */
class TcpClient {
public:
    TcpClient();
    ~TcpClient();

    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;
    TcpClient(TcpClient&&) = delete;
    TcpClient& operator=(TcpClient&&) = delete;

public:
    Result<void> Connect(const std::string& host, uint16_t port);
    Result<Response> Send(const Request& req);
    void Close();
    bool IsConnected() const;

    void SetTimeout(std::chrono::milliseconds timeout);
    std::chrono::milliseconds GetTimeout() const;

private:
    void ApplySocketTimeouts();

private:
    asio::io_context io_context_;
    asio::ip::tcp::socket socket_;
    std::chrono::milliseconds timeout_{std::chrono::seconds(10)};

    std::vector<uint8_t> send_buffer_;
    std::vector<uint8_t> recv_buffer_;
};

} // namespace database

#endif // WHISKDB_NETWORK_TCP_CLIENT_HPP
