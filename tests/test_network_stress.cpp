#include "connection_manager.hpp"
#include "errors.hpp"
#include "framing.hpp"
#include "logger.hpp"
#include "network.hpp"
#include "network/protocol.hpp"
#include "tcp_client.hpp"

#include <asio.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <print>
#include <string>
#include <thread>
#include <vector>

namespace {

using database::ErrorCode;
using database::Framing;
using database::idx_t;
using database::LogLevel;
using database::Logger;
using database::Opcode;
using database::Protocol;
using database::ProtocolDispatcher;
using database::RequestView;
using database::Result;
using database::Server;
using database::TcpClient;

void TestPersistentConnectionAndOpcodeDispatch() {
    std::println("--- [1/5] Testing Persistent TCP Connection & Opcode Dispatch ---");

    auto dispatcher = std::make_shared<ProtocolDispatcher>();
    dispatcher->RegisterHandler(Opcode::SqlQuery, [](const RequestView& req) -> Result<std::string> {
        return std::format("{{\"rows\":1,\"sql\":\"{}\"}}", req.PayloadAsStringView());
    });
    dispatcher->RegisterHandler(Opcode::WhiskCall, [](const RequestView& req) -> Result<std::string> {
        if (req.PayloadAsStringView() == "trap") {
            return database::MakeError(ErrorCode::WasmTrap, "Simulated guest trap");
        }
        return std::string("{\"result\":\"ok\"}");
    });

    Server server("127.0.0.1", 0);
    server.UseProtocolDispatcher(dispatcher);
    server.StartBackground(2);

    const uint16_t port = server.Port();
    D_ASSERT(port > 0);

    TcpClient client("127.0.0.1", port);
    auto conn_res = client.ConnectResult();
    assert(conn_res.has_value());
    assert(client.IsConnected());

    // Send 100 sequential requests over the exact same persistent TCP connection
    for (idx_t i = 0; i < 100; ++i) {
        auto ping_res = client.CallOpcode(Opcode::Ping, "hello");
        assert(ping_res.has_value());
        assert(ping_res->IsOk());
        assert(ping_res->PayloadAsStringView() == "hello");
    }

    auto sql_res = client.CallOpcode(Opcode::SqlQuery, "SELECT 42", Protocol::FLAG_JSON);
    assert(sql_res.has_value());
    assert(sql_res->IsOk());
    assert(sql_res->PayloadAsStringView() == "{\"rows\":1,\"sql\":\"SELECT 42\"}");

    auto err_res = client.CallOpcode(Opcode::WhiskCall, "trap");
    assert(err_res.has_value());
    assert(!err_res->IsOk());
    assert(err_res->status == ErrorCode::WasmTrap);
    assert(err_res->PayloadAsStringView() == "Simulated guest trap");

    assert(server.ActiveConnections() == 1);
    client.Disconnect();

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(server.ActiveConnections() == 0);

    server.Stop();
    std::println("Persistent TCP Connection & Opcode Dispatch PASSED!");
}

void TestFragmentedByteByByteStream() {
    std::println("--- [2/5] Testing Fragmented 1-Byte-at-a-Time TCP Stream Reassembly ---");

    Server server("127.0.0.1", 0);
    server.SetRequestHandler([](const std::string& req) -> std::string {
        return "ACK:" + req;
    });
    server.StartBackground(1);

    asio::io_context io_ctx;
    asio::ip::tcp::socket raw_sock(io_ctx);
    asio::ip::tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), server.Port());
    raw_sock.connect(ep);
    raw_sock.set_option(asio::ip::tcp::no_delay(true));

    const std::string payload = "FRAGMENTED_SQL_REQUEST_PAYLOAD_TEST";
    std::vector<uint8_t> frame = Framing::PackFrame(payload);

    // drip-feed 1 byte at a time over TCP to force partial reads in TcpSession
    for (idx_t i = 0; i < frame.size(); ++i) {
        asio::write(raw_sock, asio::buffer(&frame[i], 1));
    }

    uint8_t resp_hdr[Framing::HEADER_SIZE]{};
    asio::read(raw_sock, asio::buffer(resp_hdr, Framing::HEADER_SIZE));
    const uint32_t resp_len = Framing::DecodeHeader(resp_hdr);
    assert(resp_len == 4 + payload.size());

    std::string resp_body(resp_len, '\0');
    asio::read(raw_sock, asio::buffer(resp_body.data(), resp_len));
    assert(resp_body == "ACK:" + payload);

    raw_sock.close();
    server.Stop();
    std::println("Fragmented TCP Stream Reassembly PASSED!");
}

void TestCoalescedMultiFrameWrites() {
    std::println("--- [3/5] Testing Coalesced Multi-Frame TCP Writes (Pipelining) ---");

    Server server("127.0.0.1", 0);
    server.SetRequestHandler([](const std::string& req) -> std::string {
        return "ECHO:" + req;
    });
    server.StartBackground(2);

    asio::io_context io_ctx;
    asio::ip::tcp::socket raw_sock(io_ctx);
    asio::ip::tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), server.Port());
    raw_sock.connect(ep);

    constexpr idx_t num_frames = 25;
    std::vector<uint8_t> coalesced_buffer;
    for (idx_t i = 0; i < num_frames; ++i) {
        auto frame = Framing::PackFrame(std::format("MSG_{}", i));
        coalesced_buffer.insert(coalesced_buffer.end(), frame.begin(), frame.end());
    }

    // Write all 25 frames in a single OS send syscall
    asio::write(raw_sock, asio::buffer(coalesced_buffer));

    // Read all 25 framed responses in exact order
    for (idx_t i = 0; i < num_frames; ++i) {
        uint8_t hdr[Framing::HEADER_SIZE]{};
        asio::read(raw_sock, asio::buffer(hdr, Framing::HEADER_SIZE));
        const uint32_t len = Framing::DecodeHeader(hdr);
        std::string body(len, '\0');
        asio::read(raw_sock, asio::buffer(body.data(), len));
        assert(body == std::format("ECHO:MSG_{}", i));
    }

    raw_sock.close();
    server.Stop();
    std::println("Coalesced Multi-Frame TCP Pipelining PASSED!");
}

void TestOversizedFrameRejectionAndIsolation() {
    std::println("--- [4/5] Testing Oversized Frame (>16 MB) Rejection & Client Isolation ---");

    Server server("127.0.0.1", 0);
    server.SetRequestHandler([](const std::string& req) -> std::string {
        return req;
    });
    server.StartBackground(2);

    // Connect a well-behaved client first
    TcpClient good_client("127.0.0.1", server.Port());
    assert(good_client.ConnectResult().has_value());
    assert(good_client.SendRequest("ping1") == "ping1");

    // Connect a malicious client sending a header claiming 32 MB (> MAX_PAYLOAD_SIZE)
    asio::io_context io_ctx;
    asio::ip::tcp::socket bad_sock(io_ctx);
    bad_sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), server.Port()));

    uint8_t bad_hdr[Framing::HEADER_SIZE]{};
    Framing::EncodeHeader(Framing::MAX_PAYLOAD_SIZE + 1024U, bad_hdr);
    asio::write(bad_sock, asio::buffer(bad_hdr, Framing::HEADER_SIZE));

    // Server must immediately close bad_sock
    uint8_t dummy[4]{};
    asio::error_code ec;
    asio::read(bad_sock, asio::buffer(dummy, sizeof(dummy)), ec);
    assert(ec == asio::error::eof || ec == asio::error::connection_reset);
    bad_sock.close();

    // Well-behaved client must remain unaffected
    assert(good_client.SendRequest("ping2") == "ping2");
    good_client.Disconnect();

    server.Stop();
    std::println("Oversized Frame Rejection & Client Isolation PASSED!");
}

void TestConcurrentClientsStress() {
    std::println("--- [5/5] Testing 32 Concurrent Clients Stress Test (6,400 Requests) ---");

    auto dispatcher = std::make_shared<ProtocolDispatcher>();
    std::atomic<idx_t> total_dispatched{0};

    dispatcher->RegisterHandler(Opcode::SqlQuery, [&total_dispatched](const RequestView& req) -> Result<std::string> {
        total_dispatched.fetch_add(1, std::memory_order_relaxed);
        return std::string(req.PayloadAsStringView());
    });

    Server server("127.0.0.1", 0);
    server.UseProtocolDispatcher(dispatcher);
    server.StartBackground(4);

    constexpr idx_t client_count = 32;
    constexpr idx_t requests_per_client = 200;

    std::vector<std::thread> clients;
    clients.reserve(client_count);

    for (idx_t client_idx = 0; client_idx < client_count; ++client_idx) {
        clients.emplace_back([port = server.Port(), client_idx]() {
            TcpClient client("127.0.0.1", port);
            auto conn = client.ConnectResult();
            assert(conn.has_value());

            for (idx_t req_idx = 0; req_idx < requests_per_client; ++req_idx) {
                const std::string msg = std::format("client_{}_req_{}", client_idx, req_idx);
                auto res = client.CallOpcode(Opcode::SqlQuery, msg, Protocol::FLAG_JSON);
                assert(res.has_value());
                assert(res->IsOk());
                assert(res->PayloadAsStringView() == msg);
            }
            client.Disconnect();
        });
    }

    for (auto& t : clients) {
        t.join();
    }

    assert(total_dispatched.load() == client_count * requests_per_client);
    server.Stop();
    std::println("32 Concurrent Clients Stress Test (6,400 Requests) PASSED!");
}

} // namespace

int main() {
    Logger::SetLevel(LogLevel::Warn);
    std::println("=======================================================");
    std::println("Running WhiskDB Network Stress & Boundary Test Suite");
    std::println("=======================================================");

    TestPersistentConnectionAndOpcodeDispatch();
    TestFragmentedByteByByteStream();
    TestCoalescedMultiFrameWrites();
    TestOversizedFrameRejectionAndIsolation();
    TestConcurrentClientsStress();

    std::println("=======================================================");
    std::println("ALL NETWORK STRESS & BOUNDARY TESTS PASSED!");
    std::println("=======================================================");
    return 0;
}
