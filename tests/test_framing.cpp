#include "framing.hpp"
#include "errors.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace database;

void TestNetworkHostRoundtrip() {
    std::println("Testing Framing::ToNetwork and Framing::ToHost round-trip...");

    const uint32_t test_values[] = {
        0U,
        1U,
        42U,
        1024U,
        4096U,
        65535U,
        Framing::MAX_PAYLOAD_SIZE,
        0x12345678U,
        0xDEADBEEFU,
        0xFFFFFFFFU
    };

    for (uint32_t val : test_values) {
        uint32_t net = Framing::ToNetwork(val);
        uint32_t host = Framing::ToHost(net);
        assert(host == val);
    }

    // Explicitly verify big-endian byte order in memory
    const uint32_t sample = 0x12345678U;
    const uint32_t net_sample = Framing::ToNetwork(sample);
    const auto* bytes = reinterpret_cast<const uint8_t*>(&net_sample);
    assert(bytes[0] == 0x12);
    assert(bytes[1] == 0x34);
    assert(bytes[2] == 0x56);
    assert(bytes[3] == 0x78);

    std::println("  [PASS] ToNetwork and ToHost round-trip verified.");
}

void TestEncodeDecodeHeader() {
    std::println("Testing Framing::EncodeHeader and Framing::DecodeHeader...");

    const uint32_t lengths[] = {
        0U,
        1U,
        4096U,
        65535U,
        Framing::MAX_PAYLOAD_SIZE
    };

    for (uint32_t len : lengths) {
        uint8_t header_buf[Framing::HEADER_SIZE] = {};
        Framing::EncodeHeader(len, header_buf);

        // Verify big-endian wire encoding
        assert(header_buf[0] == static_cast<uint8_t>((len >> 24) & 0xFF));
        assert(header_buf[1] == static_cast<uint8_t>((len >> 16) & 0xFF));
        assert(header_buf[2] == static_cast<uint8_t>((len >> 8) & 0xFF));
        assert(header_buf[3] == static_cast<uint8_t>(len & 0xFF));

        uint32_t decoded_len = Framing::DecodeHeader(header_buf);
        assert(decoded_len == len);
    }

    std::println("  [PASS] EncodeHeader and DecodeHeader for target lengths verified.");
}

void TestValidateAndDecodeHeader() {
    std::println("Testing Framing::ValidatePayloadLength and Framing::DecodeAndValidateHeader...");

    // 1. ValidatePayloadLength
    const uint32_t valid_lengths[] = {0U, 1U, 4096U, 65535U, Framing::MAX_PAYLOAD_SIZE};
    for (uint32_t len : valid_lengths) {
        auto valid_res = Framing::ValidatePayloadLength(len);
        assert(valid_res.has_value());
    }

    // Overflow beyond MAX_PAYLOAD_SIZE
    auto overflow_res = Framing::ValidatePayloadLength(Framing::MAX_PAYLOAD_SIZE + 1U);
    assert(!overflow_res.has_value());
    assert(overflow_res.error().Code() == ErrorCode::PayloadTooLarge);

    auto max_u32_res = Framing::ValidatePayloadLength(0xFFFFFFFFU);
    assert(!max_u32_res.has_value());
    assert(max_u32_res.error().Code() == ErrorCode::PayloadTooLarge);

    // 2. DecodeAndValidateHeader for valid headers
    for (uint32_t len : valid_lengths) {
        uint8_t header_buf[Framing::HEADER_SIZE] = {};
        Framing::EncodeHeader(len, header_buf);
        auto decode_res = Framing::DecodeAndValidateHeader(std::span<const uint8_t>(header_buf, Framing::HEADER_SIZE));
        assert(decode_res.has_value());
        assert(*decode_res == len);
    }

    // 3. DecodeAndValidateHeader returning ErrorCode::PayloadTooLarge for MAX_PAYLOAD_SIZE + 1
    uint8_t overflow_header[Framing::HEADER_SIZE] = {};
    Framing::EncodeHeader(Framing::MAX_PAYLOAD_SIZE + 1U, overflow_header);
    auto decode_overflow_res = Framing::DecodeAndValidateHeader(
        std::span<const uint8_t>(overflow_header, Framing::HEADER_SIZE));
    assert(!decode_overflow_res.has_value());
    assert(decode_overflow_res.error().Code() == ErrorCode::PayloadTooLarge);

    // 4. DecodeAndValidateHeader returning ErrorCode::ProtocolError for span < 4 bytes
    const uint8_t short_bytes[3] = {0x00, 0x01, 0x02};
    for (size_t short_len = 0; short_len < Framing::HEADER_SIZE; ++short_len) {
        auto short_res = Framing::DecodeAndValidateHeader(
            std::span<const uint8_t>(short_bytes, short_len));
        assert(!short_res.has_value());
        assert(short_res.error().Code() == ErrorCode::ProtocolError);
    }

    std::println("  [PASS] Validation and error reporting for headers verified.");
}

void TestPackFrameInto() {
    std::println("Testing Framing::PackFrameInto buffer reuse and payload limit...");

    std::vector<uint8_t> frame_buffer;
    frame_buffer.reserve(4096);
    const size_t initial_capacity = frame_buffer.capacity();
    assert(initial_capacity >= 4096);

    // 1. Pack frame into pre-allocated buffer
    const std::string payload_1(256, 'A');
    auto res_1 = Framing::PackFrameInto(payload_1, frame_buffer);
    assert(res_1.has_value());
    assert(frame_buffer.size() == Framing::HEADER_SIZE + payload_1.size());
    assert(frame_buffer.capacity() == initial_capacity);
    assert(Framing::DecodeHeader(frame_buffer.data()) == payload_1.size());
    assert(std::memcmp(frame_buffer.data() + Framing::HEADER_SIZE, payload_1.data(), payload_1.size()) == 0);

    // 2. Pack smaller frame reusing the same capacity without reallocating
    const std::string payload_2(64, 'B');
    auto res_2 = Framing::PackFrameInto(payload_2, frame_buffer);
    assert(res_2.has_value());
    assert(frame_buffer.size() == Framing::HEADER_SIZE + payload_2.size());
    assert(frame_buffer.capacity() == initial_capacity);
    assert(Framing::DecodeHeader(frame_buffer.data()) == payload_2.size());
    assert(std::memcmp(frame_buffer.data() + Framing::HEADER_SIZE, payload_2.data(), payload_2.size()) == 0);

    // 3. Pack empty payload (0 bytes) reusing capacity
    const std::string_view payload_empty = "";
    auto res_empty = Framing::PackFrameInto(payload_empty, frame_buffer);
    assert(res_empty.has_value());
    assert(frame_buffer.size() == Framing::HEADER_SIZE);
    assert(frame_buffer.capacity() == initial_capacity);
    assert(Framing::DecodeHeader(frame_buffer.data()) == 0);

    // 4. Pack binary payload using std::span<const uint8_t>
    const std::vector<uint8_t> binary_payload = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02};
    auto res_bin = Framing::PackFrameInto(std::span<const uint8_t>(binary_payload), frame_buffer);
    assert(res_bin.has_value());
    assert(frame_buffer.size() == Framing::HEADER_SIZE + binary_payload.size());
    assert(frame_buffer.capacity() == initial_capacity);
    assert(Framing::DecodeHeader(frame_buffer.data()) == binary_payload.size());
    assert(std::memcmp(frame_buffer.data() + Framing::HEADER_SIZE, binary_payload.data(), binary_payload.size()) == 0);

    // 5. Reject payload exceeding MAX_PAYLOAD_SIZE
    std::vector<uint8_t> oversized_payload(Framing::MAX_PAYLOAD_SIZE + 1U, 0x7F);
    auto res_oversized = Framing::PackFrameInto(std::span<const uint8_t>(oversized_payload), frame_buffer);
    assert(!res_oversized.has_value());
    assert(res_oversized.error().Code() == ErrorCode::PayloadTooLarge);

    std::println("  [PASS] PackFrameInto buffer reuse and payload bounds verified.");
}

int main() {
    std::println("=== WhiskDB Framing Unit Tests ===");
    TestNetworkHostRoundtrip();
    TestEncodeDecodeHeader();
    TestValidateAndDecodeHeader();
    TestPackFrameInto();
    std::println("=== All Framing Unit Tests Passed ===");
    return 0;
}
