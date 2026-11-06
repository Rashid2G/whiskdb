#include "network.hpp"
#include "logger.hpp"

namespace database {

Server::Server(const Config& config)
    : host_(config.Host()),
      port_(config.Port()),
      io_context_(),
      acceptor_(io_context_) {}

Server::Server(std::string host, uint16_t port)
    : host_(std::move(host)),
      port_(port),
      io_context_(),
      acceptor_(io_context_) {}

Server::~Server() {
    Stop();
}

void Server::SetRequestHandler(RequestHandler handler) {
    request_handler_ = std::move(handler);
}

void Server::SetBinaryRequestHandler(BinaryRequestHandler handler) {
    binary_request_handler_ = std::move(handler);
}

void Server::UseProtocolDispatcher(std::shared_ptr<ProtocolDispatcher> dispatcher) {
    // Validate that the supplied protocol dispatcher pointer is non-null
    D_ASSERT(dispatcher != nullptr);
    protocol_dispatcher_ = std::move(dispatcher);
    binary_request_handler_ = [disp = protocol_dispatcher_](
                                  std::span<const uint8_t> req,
                                  std::vector<uint8_t>& resp_out) {
        disp->Dispatch(req, resp_out);
    };
}

void Server::Bind() {
    if (is_bound_.exchange(true)) {
        return;
    }

    asio::ip::tcp::endpoint endpoint(asio::ip::make_address(host_), port_);
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();

    port_ = acceptor_.local_endpoint().port();

    LOG_INFO("TCP Server listening on {}:{} (persistent connection with length-prefix framing)",
             host_, port_);

    DoAccept();
}

void Server::Run() {
    try {
        if (io_context_.stopped()) {
            io_context_.restart();
        }
        Bind();
        io_context_.run();
    } catch (const std::exception& e) {
        LOG_ERROR("Fatal exception in Server::Run: {}", e.what());
        throw;
    }
}

void Server::StartBackground(idx_t worker_threads) {
    // Validate that at least one background worker thread is requested
    D_ASSERT(worker_threads >= 1);
    if (io_context_.stopped()) {
        io_context_.restart();
    }
    Bind();
    worker_threads_.reserve(worker_threads);
    for (idx_t i = 0; i < worker_threads; ++i) {
        worker_threads_.emplace_back([this]() {
            io_context_.run();
        });
    }
}

void Server::Stop() {
    if (acceptor_.is_open()) {
        LOG_INFO("Stopping TCP Server on {}:{}...", host_, port_);
        asio::error_code ec;
        acceptor_.close(ec);
    }
    connection_manager_.StopAll();
    io_context_.stop();

    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    worker_threads_.clear();
    is_bound_.store(false);
}

idx_t Server::ActiveConnections() const {
    return connection_manager_.ActiveConnections();
}

void Server::DoAccept() {
    acceptor_.async_accept(
        [this](const asio::error_code& ec, asio::ip::tcp::socket socket) {
            if (!ec) {
                auto session = std::make_shared<TcpSession>(
                    std::move(socket),
                    connection_manager_,
                    request_handler_,
                    binary_request_handler_);
                session->Start();
            } else if (ec != asio::error::operation_aborted) {
                LOG_WARN("TCP accept error: {}", ec.message());
            }

            if (acceptor_.is_open()) {
                DoAccept();
            }
        });
}

} // namespace database
