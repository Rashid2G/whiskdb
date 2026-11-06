#include "network/server.hpp"

#include <chrono>
#include <deque>
#include <iostream>

namespace database {

class TcpSession : public std::enable_shared_from_this<TcpSession> {
public:
    TcpSession(asio::ip::tcp::socket socket, Server& server, RequestHandler& handler)
        : socket_(std::move(socket)),
          strand_(asio::make_strand(socket_.get_executor())),
          server_(server),
          handler_(handler) {
        read_buffer_.resize(64 * 1024);
    }

    ~TcpSession() {
        Close();
    }

    void Start() {
        asio::error_code ec;
        socket_.set_option(asio::ip::tcp::no_delay(true), ec);
        DoRead();
    }

    void Close() {
        bool expected = false;
        if (!is_closed_.compare_exchange_strong(expected, true)) {
            return;
        }

        asio::error_code ec;
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);

        if (auto self = weak_from_this().lock()) {
            server_.UnregisterSession(self);
        }
    }

private:
    void DoRead() {
        if (is_closed_ || should_close_after_write_) {
            return;
        }

        // Compact buffer if needed
        if (read_offset_ > 0) {
            if (read_offset_ == write_offset_) {
                read_offset_ = 0;
                write_offset_ = 0;
            } else if (read_buffer_.size() - write_offset_ < 4096) {
                size_t remaining = write_offset_ - read_offset_;
                std::memmove(read_buffer_.data(), read_buffer_.data() + read_offset_, remaining);
                read_offset_ = 0;
                write_offset_ = remaining;
            }
        }

        // Ensure sufficient capacity for reading
        if (read_buffer_.size() - write_offset_ < 4096) {
            read_buffer_.resize(read_buffer_.size() * 2);
        }

        auto self = shared_from_this();
        socket_.async_read_some(
            asio::buffer(read_buffer_.data() + write_offset_, read_buffer_.size() - write_offset_),
            asio::bind_executor(strand_, [this, self](const asio::error_code& ec, size_t bytes_transferred) {
                if (ec) {
                    Close();
                    return;
                }
                write_offset_ += bytes_transferred;
                ProcessIncoming();
            })
        );
    }

    void ProcessIncoming() {
        while (write_offset_ - read_offset_ >= Framing::HEADER_SIZE) {
            std::span<const uint8_t, 4> hdr(read_buffer_.data() + read_offset_, 4);
            auto len_res = Framing::DecodeHeader(hdr);
            if (!len_res) {
                // Oversized frame error: respond PayloadTooLarge and close
                Response err_resp;
                err_resp.status = ErrorCode::PayloadTooLarge;
                std::string msg = "Payload length exceeds maximum allowed";
                err_resp.payload.assign(msg.begin(), msg.end());
                err_resp.exec_us = 0;

                SendResponse(err_resp, /*close_after=*/true);
                return;
            }

            uint32_t payload_len = *len_res;
            size_t total_frame_len = Framing::HEADER_SIZE + payload_len;

            if (total_frame_len > read_buffer_.size()) {
                size_t remaining = write_offset_ - read_offset_;
                std::vector<uint8_t> new_buf(std::max(read_buffer_.size() * 2, total_frame_len + 8192));
                if (remaining > 0) {
                    std::memcpy(new_buf.data(), read_buffer_.data() + read_offset_, remaining);
                }
                read_buffer_ = std::move(new_buf);
                read_offset_ = 0;
                write_offset_ = remaining;
            }

            if (write_offset_ - read_offset_ < total_frame_len) {
                // Incomplete frame, wait for more data
                break;
            }

            std::span<const uint8_t> frame_payload(read_buffer_.data() + read_offset_ + Framing::HEADER_SIZE, payload_len);
            read_offset_ += total_frame_len;

            auto req_res = Protocol::DecodeRequest(frame_payload);
            if (!req_res) {
                Response err_resp;
                err_resp.status = req_res.error().Code();
                std::string msg = req_res.error().Message();
                err_resp.payload.assign(msg.begin(), msg.end());
                err_resp.exec_us = 0;

                // Unknown opcode or protocol error: respond, but keep connection open
                SendResponse(err_resp, /*close_after=*/false);
                continue;
            }

            const Request& req = *req_res;
            Response resp;

            if (req.opcode == Opcode::WhiskPublish ||
                req.opcode == Opcode::WhiskCall ||
                req.opcode == Opcode::WhiskList) {
                resp.status = ErrorCode::NotImplemented;
                std::string msg = "Opcode not implemented";
                resp.payload.assign(msg.begin(), msg.end());
                resp.exec_us = 0;
            } else {
                auto start_time = std::chrono::steady_clock::now();
                resp = handler_(req);
                auto end_time = std::chrono::steady_clock::now();
                if (resp.exec_us == 0) {
                    resp.exec_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
                }
            }

            SendResponse(resp, /*close_after=*/false);
        }

        if (!should_close_after_write_) {
            DoRead();
        }
    }

    void SendResponse(const Response& resp, bool close_after) {
        if (close_after) {
            should_close_after_write_ = true;
        }

        std::vector<uint8_t> encoded;
        Protocol::EncodeResponse(encoded, resp);
        write_queue_.push_back(std::move(encoded));

        if (!is_writing_) {
            DoWrite();
        }
    }

    void DoWrite() {
        if (write_queue_.empty()) {
            if (should_close_after_write_) {
                Close();
            }
            return;
        }

        is_writing_ = true;
        write_buffer_.clear();
        while (!write_queue_.empty()) {
            const auto& chunk = write_queue_.front();
            write_buffer_.insert(write_buffer_.end(), chunk.begin(), chunk.end());
            write_queue_.pop_front();
        }

        auto self = shared_from_this();
        asio::async_write(
            socket_,
            asio::buffer(write_buffer_),
            asio::bind_executor(strand_, [this, self](const asio::error_code& ec, size_t /*bytes_transferred*/) {
                is_writing_ = false;
                if (ec) {
                    Close();
                    return;
                }
                if (!write_queue_.empty()) {
                    DoWrite();
                } else if (should_close_after_write_) {
                    Close();
                }
            })
        );
    }

private:
    asio::ip::tcp::socket socket_;
    asio::strand<asio::any_io_executor> strand_;
    Server& server_;
    RequestHandler& handler_;

    std::vector<uint8_t> read_buffer_;
    size_t read_offset_{0};
    size_t write_offset_{0};

    std::vector<uint8_t> write_buffer_;
    std::deque<std::vector<uint8_t>> write_queue_;
    bool is_writing_{false};
    bool should_close_after_write_{false};
    std::atomic<bool> is_closed_{false};
};

Server::Server(std::string host, uint16_t port, RequestHandler handler, idx_t io_threads)
    : host_(std::move(host)),
      port_(port),
      handler_(std::move(handler)),
      io_threads_count_(io_threads > 0 ? io_threads : 1) {}

Server::~Server() {
    Stop();
}

Result<void> Server::Start() {
    if (is_running_) {
        return {};
    }

    asio::error_code ec;
    asio::ip::tcp::endpoint endpoint;

    asio::ip::tcp::resolver resolver(io_context_);
    auto endpoints = resolver.resolve(host_, std::to_string(port_), ec);
    if (ec) {
        return MakeError(ErrorCode::Internal, "Failed to resolve host: " + ec.message());
    }
    endpoint = *endpoints.begin();

    acceptor_ = std::make_unique<asio::ip::tcp::acceptor>(io_context_);
    acceptor_->open(endpoint.protocol(), ec);
    if (ec) {
        return MakeError(ErrorCode::Internal, "Failed to open acceptor: " + ec.message());
    }

    acceptor_->set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
    acceptor_->bind(endpoint, ec);
    if (ec) {
        return MakeError(ErrorCode::Internal, "Failed to bind: " + ec.message());
    }

    acceptor_->listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        return MakeError(ErrorCode::Internal, "Failed to listen: " + ec.message());
    }

    bound_port_ = acceptor_->local_endpoint(ec).port();
    if (ec) {
        return MakeError(ErrorCode::Internal, "Failed to get bound port: " + ec.message());
    }

    is_running_ = true;
    DoAccept();

    worker_threads_.reserve(io_threads_count_);
    for (idx_t i = 0; i < io_threads_count_; ++i) {
        worker_threads_.emplace_back([this]() {
            io_context_.run();
        });
    }

    return {};
}

void Server::DoAccept() {
    if (!is_running_ || !acceptor_) {
        return;
    }

    acceptor_->async_accept([this](const asio::error_code& ec, asio::ip::tcp::socket socket) {
        if (!ec) {
            auto session = std::make_shared<TcpSession>(std::move(socket), *this, handler_);
            RegisterSession(session);
            session->Start();
        }

        if (is_running_) {
            DoAccept();
        }
    });
}

void Server::Stop() {
    bool expected = true;
    if (!is_running_.compare_exchange_strong(expected, false)) {
        return;
    }

    if (acceptor_) {
        asio::error_code ec;
        acceptor_->close(ec);
    }

    std::vector<std::shared_ptr<TcpSession>> sessions_to_close;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_to_close.assign(active_sessions_.begin(), active_sessions_.end());
    }

    for (auto& session : sessions_to_close) {
        session->Close();
    }

    io_context_.stop();

    for (auto& th : worker_threads_) {
        if (th.joinable()) {
            th.join();
        }
    }
    worker_threads_.clear();
}

uint16_t Server::Port() const {
    return bound_port_;
}

idx_t Server::ActiveConnections() const {
    return active_connections_.load();
}

void Server::RegisterSession(const std::shared_ptr<TcpSession>& session) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    active_sessions_.insert(session);
    active_connections_ = active_sessions_.size();
}

void Server::UnregisterSession(const std::shared_ptr<TcpSession>& session) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    active_sessions_.erase(session);
    active_connections_ = active_sessions_.size();
}

} // namespace database
