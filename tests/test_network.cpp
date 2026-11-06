#include "client/whisk_client.hpp"
#include "network/framing.hpp"
#include "network/protocol.hpp"
#include "network/server.hpp"
#include "network/tcp_client.hpp"
#include "sql/value_codec.hpp"

#include <asio.hpp>
#include <cassert>
#include <chrono>
#include <print>
#include <thread>
#include <vector>

using namespace database;

Response fake_handler(const Request& req) {
    Response resp;
    resp.status = ErrorCode::Ok;
    resp.exec_us = 25;

    if (req.opcode == Opcode::Ping) {
        if (req.payload.empty()) {
            std::string pong = "PONG";
            resp.payload.assign(pong.begin(), pong.end());
        } else {
            // Echo payload for pipelining verification
            resp.payload = req.payload;
        }
    } else if (req.opcode == Opcode::SqlQuery) {
        auto stmt_res = DecodeStatement(req.payload);
        QueryResult qr;
        if (stmt_res) {
            qr.columns = {"echo_sql", "num_params"};
            qr.rows = {
                { Value(stmt_res->sql), Value(static_cast<int64_t>(stmt_res->params.size())) }
            };
            qr.rows_affected = 1;
            qr.exec_us = 42;
        }
        EncodeQueryResult(resp.payload, qr);
    } else if (req.opcode == Opcode::Logs) {
        std::string logs = "system: ready\nwhisk: initialized";
        resp.payload.assign(logs.begin(), logs.end());
    } else if (req.opcode == Opcode::Snapshot) {
        std::string snap = "SNAPSHOT 001 OK";
        resp.payload.assign(snap.begin(), snap.end());
    } else {
        resp.status = ErrorCode::NotImplemented;
    }
    return resp;
}

void test_whisk_client_api(uint16_t port) {
    std::println("Running test_whisk_client_api...");
    WhiskClient client;
    auto conn_res = client.Connect("127.0.0.1", port);
    assert(conn_res.has_value());
    assert(client.IsConnected());

    // 1. Ping
    auto ping_res = client.Ping();
    assert(ping_res.has_value());
    assert(*ping_res > 0);

    // 2. Query
    std::vector<Value> params = { int64_t(99), std::string("test") };
    auto qr_res = client.Query("SELECT * FROM items WHERE id = ? AND tag = ?;", params);
    assert(qr_res.has_value());
    assert(qr_res->columns.size() == 2);
    assert(qr_res->columns[0] == "echo_sql");
    assert(qr_res->rows.size() == 1);
    assert(std::get<std::string>(qr_res->rows[0][0]) == "SELECT * FROM items WHERE id = ? AND tag = ?;");
    assert(std::get<int64_t>(qr_res->rows[0][1]) == 2);
    assert(qr_res->exec_us > 0);

    // 3. Logs
    auto logs_res = client.GetLogs();
    assert(logs_res.has_value());
    assert(*logs_res == "system: ready\nwhisk: initialized");

    // 4. Snapshot
    auto snap_res = client.Snapshot();
    assert(snap_res.has_value());
    assert(*snap_res == "SNAPSHOT 001 OK");

    client.Close();
    assert(!client.IsConnected());
}

void test_concurrent_clients(uint16_t port) {
    std::println("Running test_concurrent_clients (50 clients)...");
    constexpr int NUM_CLIENTS = 50;
    constexpr int REQS_PER_CLIENT = 20;
    std::vector<std::thread> threads;
    threads.reserve(NUM_CLIENTS);

    for (int i = 0; i < NUM_CLIENTS; ++i) {
        threads.emplace_back([port]() {
            TcpClient client;
            auto conn_res = client.Connect("127.0.0.1", port);
            assert(conn_res.has_value());

            for (int r = 0; r < REQS_PER_CLIENT; ++r) {
                Request req;
                req.opcode = Opcode::Ping;
                auto resp = client.Send(req);
                assert(resp.has_value());
                assert(resp->status == ErrorCode::Ok);
                std::string msg(reinterpret_cast<const char*>(resp->payload.data()), resp->payload.size());
                assert(msg == "PONG");
            }
            client.Close();
        });
    }

    for (auto& th : threads) {
        th.join();
    }
}

void test_pipelined_requests(uint16_t port) {
    std::println("Running test_pipelined_requests (10,000 pipelined in-order)...");
    constexpr uint32_t NUM_REQS = 10000;

    asio::io_context io;
    asio::ip::tcp::socket sock(io);
    asio::ip::tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), port);
    sock.connect(ep);
    sock.set_option(asio::ip::tcp::no_delay(true));

    // Construct a big batch of 10,000 requests
    std::vector<uint8_t> batch;
    batch.reserve(NUM_REQS * 16);

    for (uint32_t i = 0; i < NUM_REQS; ++i) {
        Request req;
        req.opcode = Opcode::Ping;
        req.payload.resize(4);
        req.payload[0] = static_cast<uint8_t>((i >> 24) & 0xFF);
        req.payload[1] = static_cast<uint8_t>((i >> 16) & 0xFF);
        req.payload[2] = static_cast<uint8_t>((i >> 8) & 0xFF);
        req.payload[3] = static_cast<uint8_t>(i & 0xFF);
        EncodeRequest(batch, req);
    }

    // Send all 10,000 requests in pipelined fashion
    asio::write(sock, asio::buffer(batch));

    // Read and verify 10,000 responses sequentially in exact order
    std::vector<uint8_t> recv_buf;
    recv_buf.resize(64 * 1024);
    size_t recv_offset = 0;
    size_t processed = 0;

    for (uint32_t expected_id = 0; expected_id < NUM_REQS; ++expected_id) {
        // Read until we have at least 4 bytes header
        while (recv_offset < 4) {
            size_t n = sock.read_some(asio::buffer(recv_buf.data() + recv_offset, recv_buf.size() - recv_offset));
            recv_offset += n;
        }

        std::span<const uint8_t, 4> hdr(recv_buf.data(), 4);
        auto len_res = Framing::DecodeHeader(hdr);
        assert(len_res.has_value());
        uint32_t payload_len = *len_res;
        size_t total_frame_len = 4 + payload_len;

        if (total_frame_len > recv_buf.size()) {
            recv_buf.resize(total_frame_len * 2);
        }

        while (recv_offset < total_frame_len) {
            size_t n = sock.read_some(asio::buffer(recv_buf.data() + recv_offset, recv_buf.size() - recv_offset));
            recv_offset += n;
        }

        std::span<const uint8_t> frame(recv_buf.data(), total_frame_len);
        auto resp_res = DecodeResponse(frame);
        assert(resp_res.has_value());
        assert(resp_res->status == ErrorCode::Ok);
        assert(resp_res->payload.size() == 4);

        uint32_t received_id = (static_cast<uint32_t>(resp_res->payload[0]) << 24) |
                               (static_cast<uint32_t>(resp_res->payload[1]) << 16) |
                               (static_cast<uint32_t>(resp_res->payload[2]) << 8) |
                               static_cast<uint32_t>(resp_res->payload[3]);
        assert(received_id == expected_id);

        size_t leftover = recv_offset - total_frame_len;
        if (leftover > 0) {
            std::memmove(recv_buf.data(), recv_buf.data() + total_frame_len, leftover);
        }
        recv_offset = leftover;
        processed++;
    }

    assert(processed == NUM_REQS);
    sock.close();
}

void test_partial_reads(uint16_t port) {
    std::println("Running test_partial_reads (one byte at a time)...");
    asio::io_context io;
    asio::ip::tcp::socket sock(io);
    sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));

    Request req;
    req.opcode = Opcode::Ping;
    std::string msg = "ONE_BYTE_STREAM";
    req.payload.assign(msg.begin(), msg.end());

    std::vector<uint8_t> frame;
    EncodeRequest(frame, req);

    for (uint8_t b : frame) {
        asio::write(sock, asio::buffer(&b, 1));
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    std::array<uint8_t, 4> resp_hdr{};
    asio::read(sock, asio::buffer(resp_hdr));
    auto len_res = Framing::DecodeHeader(resp_hdr);
    assert(len_res.has_value());

    std::vector<uint8_t> resp_payload(*len_res);
    asio::read(sock, asio::buffer(resp_payload));
    auto resp = DecodeResponse(resp_payload);
    assert(resp.has_value());
    assert(resp->status == ErrorCode::Ok);
    std::string echo(reinterpret_cast<const char*>(resp->payload.data()), resp->payload.size());
    assert(echo == msg);

    sock.close();
}

void test_oversized_frame(uint16_t port) {
    std::println("Running test_oversized_frame...");
    asio::io_context io;
    asio::ip::tcp::socket sock(io);
    sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));

    // Send header claiming payload > 16MB (e.g. MAX_PAYLOAD_SIZE + 1)
    std::array<uint8_t, 4> oversize_hdr{};
    Framing::EncodeHeader(Framing::MAX_PAYLOAD_SIZE + 1, oversize_hdr);
    asio::write(sock, asio::buffer(oversize_hdr));

    // Server must respond with PayloadTooLarge
    std::array<uint8_t, 4> resp_hdr{};
    asio::error_code ec;
    asio::read(sock, asio::buffer(resp_hdr), ec);
    assert(!ec);
    auto len_res = Framing::DecodeHeader(resp_hdr);
    assert(len_res.has_value());

    std::vector<uint8_t> resp_payload(*len_res);
    asio::read(sock, asio::buffer(resp_payload), ec);
    assert(!ec);

    auto resp = DecodeResponse(resp_payload);
    assert(resp.has_value());
    assert(resp->status == ErrorCode::PayloadTooLarge);

    // After sending PayloadTooLarge, server closes connection
    uint8_t dummy;
    asio::read(sock, asio::buffer(&dummy, 1), ec);
    assert(ec == asio::error::eof || ec == asio::error::connection_reset);

    sock.close();
}

void test_invalid_opcode_connection_alive(uint16_t port) {
    std::println("Running test_invalid_opcode_connection_alive...");
    asio::io_context io;
    asio::ip::tcp::socket sock(io);
    sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));

    // Send invalid opcode 0xAA
    std::vector<uint8_t> frame;
    frame.resize(4);
    Framing::EncodeHeader(2, std::span<uint8_t, 4>(frame.data(), 4));
    frame.push_back(0xAA); // invalid opcode
    frame.push_back(0x01); // dummy payload

    asio::write(sock, asio::buffer(frame));

    // Read error response
    std::array<uint8_t, 4> resp_hdr{};
    asio::read(sock, asio::buffer(resp_hdr));
    auto len_res = Framing::DecodeHeader(resp_hdr);
    assert(len_res.has_value());
    std::vector<uint8_t> resp_payload(*len_res);
    asio::read(sock, asio::buffer(resp_payload));
    auto resp = DecodeResponse(resp_payload);
    assert(resp.has_value());
    assert(resp->status == ErrorCode::UnknownOpcode);

    // Ensure connection is STILL ALIVE by sending a valid Ping
    Request ping_req{ .opcode = Opcode::Ping, .payload = {} };
    std::vector<uint8_t> ping_frame;
    EncodeRequest(ping_frame, ping_req);
    asio::write(sock, asio::buffer(ping_frame));

    asio::read(sock, asio::buffer(resp_hdr));
    len_res = Framing::DecodeHeader(resp_hdr);
    assert(len_res.has_value());
    resp_payload.resize(*len_res);
    asio::read(sock, asio::buffer(resp_payload));
    resp = DecodeResponse(resp_payload);
    assert(resp.has_value());
    assert(resp->status == ErrorCode::Ok);
    std::string pong(reinterpret_cast<const char*>(resp->payload.data()), resp->payload.size());
    assert(pong == "PONG");

    sock.close();
}

void test_client_disconnect_mid_frame(uint16_t port) {
    std::println("Running test_client_disconnect_mid_frame...");
    asio::io_context io;
    asio::ip::tcp::socket sock(io);
    sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));

    // Send 4-byte header indicating 100 bytes payload
    std::array<uint8_t, 4> hdr{};
    Framing::EncodeHeader(100, hdr);
    asio::write(sock, asio::buffer(hdr));

    // Send only 5 bytes, then abruptly close socket
    std::array<uint8_t, 5> partial = {0x01, 0x02, 0x03, 0x04, 0x05};
    asio::write(sock, asio::buffer(partial));

    sock.close(); // Abrupt disconnect mid-frame

    // Give server a moment to clean up session without crashing
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

void test_stop_promptness(uint16_t port, Server& server) {
    std::println("Running test_stop_promptness...");
    // Open 5 live connections
    asio::io_context io;
    std::vector<asio::ip::tcp::socket> sockets;
    for (int i = 0; i < 5; ++i) {
        sockets.emplace_back(io);
        sockets.back().connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    }

    auto start = std::chrono::steady_clock::now();
    server.Stop();
    auto end = std::chrono::steady_clock::now();

    auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::println("Server::Stop() completed in {} ms", stop_ms);
    assert(stop_ms < 1000); // Must return promptly (< 1s)

    for (auto& s : sockets) {
        asio::error_code ec;
        s.close(ec);
    }
}

int main() {
    std::println("=== Running Network Tests ===");

    Server server("127.0.0.1", 0, fake_handler, 4);
    auto start_res = server.Start();
    assert(start_res.has_value());

    uint16_t port = server.Port();
    assert(port > 0);
    std::println("Server started on ephemeral port {}", port);

    test_whisk_client_api(port);
    test_concurrent_clients(port);
    test_pipelined_requests(port);
    test_partial_reads(port);
    test_oversized_frame(port);
    test_invalid_opcode_connection_alive(port);
    test_client_disconnect_mid_frame(port);
    test_stop_promptness(port, server);

    std::println("=== All Network Tests Passed! ===");
    return 0;
}
