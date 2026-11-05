#ifndef WHISKDB_TCP_CLIENT_HPP
#define WHISKDB_TCP_CLIENT_HPP

#include "common/types.hpp"
#include "errors.hpp"
#include "framing.hpp"
#include "network/protocol.hpp"

#include <asio.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace database {

/**
 * @brief High-speed persistent TCP client using standalone Asio.
 *
 * Keeps a persistent TCP socket open across multiple sequential requests,
 * reusing internal frame buffers to avoid per-request heap allocations.
 */
class TcpClient {
public:
    static constexpr idx_t INITIAL_BUFFER_CAPACITY = 4096;

    TcpClient(const std::string& host, uint16_t port)
        : host_(host), port_(port), io_context_(), socket_(io_context_) {
        write_frame_buf_.reserve(INITIAL_BUFFER_CAPACITY);
        read_payload_buf_.reserve(INITIAL_BUFFER_CAPACITY);
        request_encode_buf_.reserve(INITIAL_BUFFER_CAPACITY);
    }

    ~TcpClient() {
        Disconnect();
    }

    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

public:
    /// Connect to the server, returning Result<void> for expected connection errors
    Result<void> ConnectResult() {
        asio::error_code ec;
        asio::ip::tcp::resolver resolver(io_context_);
        auto endpoints = resolver.resolve(host_, std::to_string(port_), ec);
        if (ec) {
            return MakeError(ErrorCode::IoError,
                             std::format("Failed to resolve {}:{}: {}", host_, port_, ec.message()));
        }

        asio::connect(socket_, endpoints, ec);
        if (ec) {
            return MakeError(ErrorCode::ConnectionClosed,
                             std::format("Failed to connect to {}:{}: {}", host_, port_, ec.message()));
        }

        socket_.set_option(asio::ip::tcp::no_delay(true), ec);
        socket_.set_option(asio::socket_base::keep_alive(true), ec);
        return {};
    }

    /// Connect to the server (throws DatabaseException on failure for legacy callers)
    void Connect() {
        auto res = ConnectResult();
        if (!res) {
            throw DatabaseException(res.error().Code(), res.error().Message());
        }
    }
    void connect() { Connect(); }

    /// Send a binary frame and return a zero-copy span into the client's reusable read buffer
    Result<std::span<const uint8_t>> SendFrameZeroCopy(std::span<const uint8_t> payload) {
        if (!socket_.is_open()) {
            return MakeError(ErrorCode::ConnectionClosed, "Socket is not connected");
        }

        if (auto pack_res = Framing::PackFrameInto(payload, write_frame_buf_); !pack_res) {
            return std::unexpected(pack_res.error());
        }

        asio::error_code ec;
        asio::write(socket_, asio::buffer(write_frame_buf_), ec);
        if (ec) {
            return MakeError(ErrorCode::ConnectionReset,
                             std::format("Socket write failed: {}", ec.message()));
        }

        uint8_t header_buf[Framing::HEADER_SIZE]{};
        asio::read(socket_, asio::buffer(header_buf, Framing::HEADER_SIZE), ec);
        if (ec) {
            return MakeError(ErrorCode::ConnectionClosed,
                             std::format("Socket header read failed: {}", ec.message()));
        }

        const uint32_t resp_len = Framing::DecodeHeader(header_buf);
        if (auto valid = Framing::ValidatePayloadLength(resp_len); !valid) {
            return std::unexpected(valid.error());
        }

        read_payload_buf_.resize(resp_len);
        if (resp_len > 0) {
            asio::read(socket_, asio::buffer(read_payload_buf_.data(), resp_len), ec);
            if (ec) {
                return MakeError(ErrorCode::ConnectionClosed,
                                 std::format("Socket body read failed: {}", ec.message()));
            }
        }

        return std::span<const uint8_t>(read_payload_buf_.data(), resp_len);
    }

    /// Send a binary Opcode request and decode the structured ResponseView
    Result<ResponseView> CallOpcode(Opcode opcode,
                                    std::string_view payload = "",
                                    uint8_t flags = Protocol::FLAG_NONE) {
        Protocol::EncodeRequestInto(opcode, flags, payload, request_encode_buf_);
        auto frame_res = SendFrameZeroCopy(request_encode_buf_);
        if (!frame_res) {
            return std::unexpected(frame_res.error());
        }
        return Protocol::DecodeResponse(*frame_res);
    }

    /// Send a string request and return Result<std::string>
    Result<std::string> SendRequestResult(std::string_view payload) {
        std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
        auto span_res = SendFrameZeroCopy(bytes);
        if (!span_res) {
            return std::unexpected(span_res.error());
        }
        return std::string(reinterpret_cast<const char*>(span_res->data()), span_res->size());
    }

    /// Send a string request (legacy throwing API)
    std::string SendRequest(std::string_view payload) {
        auto res = SendRequestResult(payload);
        if (!res) {
            throw std::runtime_error(res.error().ToString());
        }
        return std::move(*res);
    }
    std::string send_request(std::string_view payload) {
        return SendRequest(payload);
    }

    /// Disconnect socket
    void Disconnect() noexcept {
        if (socket_.is_open()) {
            asio::error_code ec;
            socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            socket_.close(ec);
        }
    }
    void disconnect() noexcept { Disconnect(); }

    bool IsConnected() const noexcept {
        return socket_.is_open();
    }
    bool is_connected() const noexcept {
        return IsConnected();
    }

    asio::ip::tcp::socket& RawSocket() noexcept { return socket_; }

private:
    std::string host_;
    uint16_t port_;
    asio::io_context io_context_;
    asio::ip::tcp::socket socket_;

    std::vector<uint8_t> write_frame_buf_;
    std::vector<uint8_t> read_payload_buf_;
    std::vector<uint8_t> request_encode_buf_;
};

} // namespace database

namespace Database = database;

#endif // WHISKDB_TCP_CLIENT_HPP
