#ifndef WHISKDB_TCP_SESSION_HPP
#define WHISKDB_TCP_SESSION_HPP

#include "common/types.hpp"
#include "framing.hpp"

#include <asio.hpp>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace database {

class ConnectionManager;

using RequestHandler = std::function<std::string(const std::string&)>;
using BinaryRequestHandler = std::function<void(std::span<const uint8_t>, std::vector<uint8_t>&)>;

/**
 * @brief Represents a persistent TCP client session with reusable frame buffers.
 */
class TcpSession : public std::enable_shared_from_this<TcpSession> {
public:
    static constexpr idx_t INITIAL_BUFFER_CAPACITY = 4096;

    TcpSession(asio::ip::tcp::socket socket,
               ConnectionManager& manager,
               RequestHandler handler,
               BinaryRequestHandler binary_handler = nullptr);
    ~TcpSession();

    TcpSession(const TcpSession&) = delete;
    TcpSession& operator=(const TcpSession&) = delete;

public:
    /// Begin processing the session
    void Start();
    void start() { Start(); }

    /// Gracefully stop and close the session socket
    void Stop();
    void stop() { Stop(); }

    /// Check if the session is currently open
    bool IsOpen() const;
    bool is_open() const { return IsOpen(); }

    /// Get client remote endpoint description
    std::string RemoteEndpointStr() const;
    std::string remote_endpoint_str() const { return RemoteEndpointStr(); }

private:
    void DoReadHeader();
    void DoReadBody(uint32_t payload_len);
    void HandlePayload(std::span<const uint8_t> request_bytes);
    void DoWrite(std::vector<uint8_t> frame);
    void StartWrite();

private:
    asio::ip::tcp::socket socket_;
    asio::strand<asio::any_io_executor> strand_;
    ConnectionManager& manager_;
    RequestHandler handler_;
    BinaryRequestHandler binary_handler_;

    uint8_t header_buffer_[Framing::HEADER_SIZE]{};
    std::vector<uint8_t> body_buffer_;
    std::vector<uint8_t> scratch_response_payload_;
    std::deque<std::vector<uint8_t>> write_queue_;

    bool stopped_{false};
};

} // namespace database

namespace Database = database;

#endif // WHISKDB_TCP_SESSION_HPP
