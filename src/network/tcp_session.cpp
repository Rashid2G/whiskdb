#include "tcp_session.hpp"
#include "connection_manager.hpp"
#include "errors.hpp"
#include "logger.hpp"

namespace database {

TcpSession::TcpSession(asio::ip::tcp::socket socket,
                       ConnectionManager& manager,
                       RequestHandler handler,
                       BinaryRequestHandler binary_handler)
    : socket_(std::move(socket)),
      manager_(manager),
      handler_(std::move(handler)),
      binary_handler_(std::move(binary_handler)) {
    body_buffer_.reserve(INITIAL_BUFFER_CAPACITY);
    scratch_response_payload_.reserve(INITIAL_BUFFER_CAPACITY);
}

TcpSession::~TcpSession() {
    Stop();
}

void TcpSession::Start() {
    asio::error_code ec;
    socket_.set_option(asio::ip::tcp::no_delay(true), ec);
    socket_.set_option(asio::socket_base::keep_alive(true), ec);

    LOG_DEBUG("TCP session started from {}", RemoteEndpointStr());

    manager_.Start(shared_from_this());
    DoReadHeader();
}

void TcpSession::Stop() {
    if (stopped_) {
        return;
    }
    stopped_ = true;

    LOG_DEBUG("TCP session closed for {}", RemoteEndpointStr());

    asio::error_code ec;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket_.close(ec);
}

bool TcpSession::IsOpen() const {
    return !stopped_ && socket_.is_open();
}

std::string TcpSession::RemoteEndpointStr() const {
    asio::error_code ec;
    auto endpoint = socket_.remote_endpoint(ec);
    if (!ec) {
        return endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
    }
    return "unknown";
}

void TcpSession::DoReadHeader() {
    if (stopped_) {
        return;
    }

    auto self = shared_from_this();
    asio::async_read(
        socket_,
        asio::buffer(header_buffer_, Framing::HEADER_SIZE),
        [this, self](const asio::error_code& ec, std::size_t /*bytes_transferred*/) {
            if (!ec) {
                const uint32_t payload_len = Framing::DecodeHeader(header_buffer_);

                if (auto valid = Framing::ValidatePayloadLength(payload_len); !valid) {
                    LOG_ERROR("Received payload size {} exceeds limit of {} bytes from {}. Closing connection.",
                              payload_len, Framing::MAX_PAYLOAD_SIZE, RemoteEndpointStr());
                    manager_.Stop(self);
                    return;
                }

                if (payload_len == 0) {
                    HandlePayload(std::span<const uint8_t>{});
                } else {
                    DoReadBody(payload_len);
                }
            } else {
                if (ec != asio::error::operation_aborted) {
                    LOG_DEBUG("Connection ended for {}: {}", RemoteEndpointStr(), ec.message());
                    manager_.Stop(self);
                }
            }
        });
}

void TcpSession::DoReadBody(uint32_t payload_len) {
    if (stopped_) {
        return;
    }

    // Validate that payload length is within the maximum frame limit
    D_ASSERT(payload_len <= Framing::MAX_PAYLOAD_SIZE);
    body_buffer_.resize(payload_len);
    auto self = shared_from_this();

    asio::async_read(
        socket_,
        asio::buffer(body_buffer_.data(), payload_len),
        [this, self, payload_len](const asio::error_code& ec, std::size_t /*bytes_transferred*/) {
            if (!ec) {
                HandlePayload(std::span<const uint8_t>(body_buffer_.data(), payload_len));
            } else {
                if (ec != asio::error::operation_aborted) {
                    LOG_DEBUG("Read body error for {}: {}", RemoteEndpointStr(), ec.message());
                    manager_.Stop(self);
                }
            }
        });
}

void TcpSession::HandlePayload(std::span<const uint8_t> request_bytes) {
    std::vector<uint8_t> frame;

    if (binary_handler_) {
        scratch_response_payload_.clear();
        binary_handler_(request_bytes, scratch_response_payload_);
        auto pack_res = Framing::PackFrameInto(scratch_response_payload_, frame);
        if (!pack_res) {
            LOG_ERROR("Failed to pack binary response frame: {}", pack_res.error().ToString());
            manager_.Stop(shared_from_this());
            return;
        }
    } else if (handler_) {
        std::string request_str(reinterpret_cast<const char*>(request_bytes.data()), request_bytes.size());
        std::string response_str = handler_(request_str);
        frame = Framing::PackFrame(response_str);
    } else {
        frame = Framing::PackFrame("OK");
    }

    DoWrite(std::move(frame));
    DoReadHeader();
}

void TcpSession::DoWrite(std::vector<uint8_t> frame) {
    if (stopped_) {
        return;
    }

    const bool write_in_progress = !write_queue_.empty();
    write_queue_.push_back(std::move(frame));

    if (!write_in_progress) {
        StartWrite();
    }
}

void TcpSession::StartWrite() {
    if (stopped_ || write_queue_.empty()) {
        return;
    }

    auto self = shared_from_this();
    asio::async_write(
        socket_,
        asio::buffer(write_queue_.front()),
        [this, self](const asio::error_code& ec, std::size_t /*bytes_transferred*/) {
            if (!ec) {
                write_queue_.pop_front();
                if (!write_queue_.empty()) {
                    StartWrite();
                }
            } else {
                if (ec != asio::error::operation_aborted) {
                    LOG_WARN("Write response error for {}: {}", RemoteEndpointStr(), ec.message());
                    manager_.Stop(self);
                }
            }
        });
}

} // namespace database
