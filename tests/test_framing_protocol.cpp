#include "network/framing.hpp"
#include "network/protocol.hpp"

#include <array>
#include <cassert>
#include <print>

using namespace database;

void test_framing_boundaries() {
    std::println("Running test_framing_boundaries...");

    // 1. Boundary: 0 bytes
    {
        std::array<uint8_t, 4> hdr{};
        Framing::EncodeHeader(0, hdr);
        auto res = Framing::DecodeHeader(hdr);
        assert(res.has_value());
        assert(*res == 0);
    }

    // 2. Normal payload: 1024 bytes
    {
        std::array<uint8_t, 4> hdr{};
        Framing::EncodeHeader(1024, hdr);
        auto res = Framing::DecodeHeader(hdr);
        assert(res.has_value());
        assert(*res == 1024);
    }

    // 3. Boundary: Framing::MAX_PAYLOAD_SIZE (16MB)
    {
        std::array<uint8_t, 4> hdr{};
        Framing::EncodeHeader(Framing::MAX_PAYLOAD_SIZE, hdr);
        auto res = Framing::DecodeHeader(hdr);
        assert(res.has_value());
        assert(*res == Framing::MAX_PAYLOAD_SIZE);
    }

    // 4. Boundary: Framing::MAX_PAYLOAD_SIZE + 1 (Oversize)
    {
        std::array<uint8_t, 4> hdr{};
        Framing::EncodeHeader(Framing::MAX_PAYLOAD_SIZE + 1, hdr);
        auto res = Framing::DecodeHeader(hdr);
        assert(!res.has_value());
        assert(res.error().Code() == ErrorCode::PayloadTooLarge);
    }

    // 5. Huge length: 0xFFFFFFFF
    {
        std::array<uint8_t, 4> hdr = {0xFF, 0xFF, 0xFF, 0xFF};
        auto res = Framing::DecodeHeader(hdr);
        assert(!res.has_value());
        assert(res.error().Code() == ErrorCode::PayloadTooLarge);
    }
}

void test_protocol_request() {
    std::println("Running test_protocol_request...");

    // Test every valid opcode
    const std::vector<Opcode> valid_opcodes = {
        Opcode::Ping,
        Opcode::SqlQuery,
        Opcode::WhiskPublish,
        Opcode::WhiskCall,
        Opcode::WhiskList,
        Opcode::Logs,
        Opcode::Snapshot
    };

    for (auto op : valid_opcodes) {
        Request req{
            .opcode = op,
            .payload = {0x01, 0x02, 0x03, 0x04}
        };

        std::vector<uint8_t> frame;
        EncodeRequest(frame, req);

        // Frame must have 4 bytes header + 1 byte opcode + 4 bytes payload = 9 bytes
        assert(frame.size() == 9);

        // Full frame decode
        auto dec_res = DecodeRequest(frame);
        assert(dec_res.has_value());
        assert(dec_res->opcode == op);
        assert(dec_res->payload == req.payload);

        // Frame body only (without 4-byte header)
        std::span<const uint8_t> body(frame.data() + 4, frame.size() - 4);
        auto dec_body_res = DecodeRequest(body);
        assert(dec_body_res.has_value());
        assert(dec_body_res->opcode == op);
        assert(dec_body_res->payload == req.payload);
    }

    // Bad/unknown opcodes: 0x00, 0x08, 0xFF
    const std::vector<uint8_t> bad_opcodes = {0x00, 0x08, 0x09, 0x55, 0xFF};
    for (uint8_t bad_op : bad_opcodes) {
        std::vector<uint8_t> body = {bad_op, 0x11, 0x22};
        auto res = DecodeRequest(body);
        assert(!res.has_value());
        assert(res.error().Code() == ErrorCode::UnknownOpcode);
    }

    // Truncated request body (0 bytes)
    {
        std::vector<uint8_t> empty_body;
        auto res = DecodeRequest(empty_body);
        assert(!res.has_value());
        assert(res.error().Code() == ErrorCode::ProtocolError);
    }
}

void test_protocol_response() {
    std::println("Running test_protocol_response...");

    // Normal response with payload
    {
        Response resp{
            .status = ErrorCode::Ok,
            .exec_us = 9876543210ULL,
            .payload = {'P', 'O', 'N', 'G'}
        };

        std::vector<uint8_t> frame;
        EncodeResponse(frame, resp);

        // 4B len + 2B status + 8B exec_us + 4B payload = 18 bytes
        assert(frame.size() == 18);

        // Full frame decode
        auto dec = DecodeResponse(frame);
        assert(dec.has_value());
        assert(dec->status == ErrorCode::Ok);
        assert(dec->exec_us == 9876543210ULL);
        assert(dec->payload == resp.payload);

        // Frame body only
        std::span<const uint8_t> body(frame.data() + 4, frame.size() - 4);
        auto dec_body = DecodeResponse(body);
        assert(dec_body.has_value());
        assert(dec_body->status == ErrorCode::Ok);
        assert(dec_body->exec_us == 9876543210ULL);
        assert(dec_body->payload == resp.payload);
    }

    // Error response
    {
        Response resp{
            .status = ErrorCode::PayloadTooLarge,
            .exec_us = 0,
            .payload = {'O', 'v', 'e', 'r', 's', 'i', 'z', 'e', 'd'}
        };

        std::vector<uint8_t> frame;
        EncodeResponse(frame, resp);

        auto dec = DecodeResponse(frame);
        assert(dec.has_value());
        assert(dec->status == ErrorCode::PayloadTooLarge);
        assert(dec->exec_us == 0);
        assert(dec->payload == resp.payload);
    }

    // Truncated response (< 10 bytes body)
    {
        std::vector<uint8_t> trunc_body = {0x00, 0x00, 0x01, 0x02}; // only 4 bytes instead of 10
        auto res = DecodeResponse(trunc_body);
        assert(!res.has_value());
        assert(res.error().Code() == ErrorCode::ProtocolError);
    }
}

int main() {
    std::println("=== Running Framing & Protocol Tests ===");
    test_framing_boundaries();
    test_protocol_request();
    test_protocol_response();
    std::println("=== All Framing & Protocol Tests Passed! ===");
    return 0;
}
