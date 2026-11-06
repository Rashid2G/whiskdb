#include "network/tcp_client.hpp"

#include <array>
#include <cerrno>
#include <sys/socket.h>
#include <sys/time.h>

namespace database {

TcpClient::TcpClient()
    : socket_(io_context_) {
    send_buffer_.reserve(64 * 1024);
    recv_buffer_.reserve(64 * 1024);
}

TcpClient::~TcpClient() {
    Close();
}

Result<void> TcpClient::Connect(const std::string& host, uint16_t port) {
    if (IsConnected()) {
        Close();
    }

    asio::error_code ec;
    asio::ip::tcp::resolver resolver(io_context_);
    auto endpoints = resolver.resolve(host, std::to_string(port), ec);
    if (ec) {
        return MakeError(ErrorCode::ConnectionClosed, "Failed to resolve host: " + ec.message());
    }

    asio::connect(socket_, endpoints, ec);
    if (ec) {
        Close();
        return MakeError(ErrorCode::ConnectionClosed, "Failed to connect: " + ec.message());
    }

    socket_.set_option(asio::ip::tcp::no_delay(true), ec);
    ApplySocketTimeouts();

    return {};
}

void TcpClient::ApplySocketTimeouts() {
    if (!socket_.is_open()) {
        return;
    }

    struct timeval tv{};
    auto ms = timeout_.count();
    tv.tv_sec = ms / 1000;
    tv.tv_usec = static_cast<suseconds_t>((ms % 1000) * 1000);

    setsockopt(socket_.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(socket_.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

Result<Response> TcpClient::Send(const Request& req) {
    if (!IsConnected()) {
        return MakeError(ErrorCode::ConnectionClosed, "Client is not connected");
    }

    send_buffer_.clear();
    Protocol::EncodeRequest(send_buffer_, req);

    asio::error_code ec;
    asio::write(socket_, asio::buffer(send_buffer_), ec);
    if (ec) {
        Close();
        if (ec == asio::error::timed_out || ec == asio::error::would_block || ec == asio::error::try_again) {
            return MakeError(ErrorCode::Timeout, "Send operation timed out");
        }
        return MakeError(ErrorCode::ConnectionClosed, "Send failed: " + ec.message());
    }

    std::array<uint8_t, 4> hdr{};
    asio::read(socket_, asio::buffer(hdr), ec);
    if (ec) {
        Close();
        if (ec == asio::error::timed_out || ec == asio::error::would_block || ec == asio::error::try_again) {
            return MakeError(ErrorCode::Timeout, "Receive header timed out");
        }
        return MakeError(ErrorCode::ConnectionClosed, "Receive header failed: " + ec.message());
    }

    auto len_res = Framing::DecodeHeader(hdr);
    if (!len_res) {
        Close();
        return MakeError(len_res.error().Code(), len_res.error().Message());
    }

    uint32_t payload_len = *len_res;
    recv_buffer_.resize(payload_len);

    asio::read(socket_, asio::buffer(recv_buffer_), ec);
    if (ec) {
        Close();
        if (ec == asio::error::timed_out || ec == asio::error::would_block || ec == asio::error::try_again) {
            return MakeError(ErrorCode::Timeout, "Receive payload timed out");
        }
        return MakeError(ErrorCode::ConnectionClosed, "Receive payload failed: " + ec.message());
    }

    auto resp_res = Protocol::DecodeResponse(recv_buffer_);
    if (!resp_res) {
        return MakeError(resp_res.error().Code(), resp_res.error().Message());
    }

    return *resp_res;
}

void TcpClient::Close() {
    if (socket_.is_open()) {
        asio::error_code ec;
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }
}

bool TcpClient::IsConnected() const {
    return socket_.is_open();
}

void TcpClient::SetTimeout(std::chrono::milliseconds timeout) {
    timeout_ = timeout;
    ApplySocketTimeouts();
}

std::chrono::milliseconds TcpClient::GetTimeout() const {
    return timeout_;
}

} // namespace database
