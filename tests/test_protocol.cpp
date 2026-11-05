#include "errors.hpp"
#include "network/protocol.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using database::ErrorCode;
using database::idx_t;
using database::IsValidOpcode;
using database::MakeError;
using database::Opcode;
using database::Protocol;
using database::ProtocolDispatcher;
using database::RequestView;
using database::ResponseView;
using database::Result;
using database::ToString;

// 1. IsValidOpcode and ToString(Opcode)
void TestOpcodeValidationAndToString() {
    std::println("--- [1/5] Testing Opcode Validation and ToString ---");

    // All 7 valid opcodes
    assert(IsValidOpcode(static_cast<uint8_t>(Opcode::Ping)));
    assert(ToString(Opcode::Ping) == "PING");

    assert(IsValidOpcode(static_cast<uint8_t>(Opcode::SqlQuery)));
    assert(ToString(Opcode::SqlQuery) == "SQL_QUERY");

    assert(IsValidOpcode(static_cast<uint8_t>(Opcode::WhiskPublish)));
    assert(ToString(Opcode::WhiskPublish) == "WHISK_PUBLISH");

    assert(IsValidOpcode(static_cast<uint8_t>(Opcode::WhiskCall)));
    assert(ToString(Opcode::WhiskCall) == "WHISK_CALL");

    assert(IsValidOpcode(static_cast<uint8_t>(Opcode::WhiskList)));
    assert(ToString(Opcode::WhiskList) == "WHISK_LIST");

    assert(IsValidOpcode(static_cast<uint8_t>(Opcode::WhiskLogs)));
    assert(ToString(Opcode::WhiskLogs) == "WHISK_LOGS");

    assert(IsValidOpcode(static_cast<uint8_t>(Opcode::Snapshot)));
    assert(ToString(Opcode::Snapshot) == "SNAPSHOT");

    // Invalid opcodes
    assert(!IsValidOpcode(0x00));
    assert(!IsValidOpcode(0x08));
    assert(!IsValidOpcode(0xFF));

    assert(ToString(static_cast<Opcode>(0x00)) == "UNKNOWN");
    assert(ToString(static_cast<Opcode>(0x08)) == "UNKNOWN");
    assert(ToString(static_cast<Opcode>(0xFF)) == "UNKNOWN");

    std::println("  Opcode validation and string conversion passed.");
}

// 2. Protocol::EncodeRequestInto and Protocol::DecodeRequest zero-copy RequestView
void TestEncodeDecodeRequestZeroCopy() {
    std::println("--- [2/5] Testing EncodeRequestInto & DecodeRequest (Zero-Copy) ---");

    // Test A: Empty payload
    {
        std::vector<uint8_t> req_buf;
        Protocol::EncodeRequestInto(Opcode::Ping, Protocol::FLAG_NONE, std::string_view(""), req_buf);

        assert(req_buf.size() == Protocol::REQUEST_HEADER_SIZE);
        assert(req_buf[0] == static_cast<uint8_t>(Opcode::Ping));
        assert(req_buf[1] == Protocol::FLAG_NONE);

        auto decoded = Protocol::DecodeRequest(req_buf);
        assert(decoded.has_value());
        assert(decoded->opcode == Opcode::Ping);
        assert(decoded->flags == Protocol::FLAG_NONE);
        assert(decoded->payload.empty());
        assert(decoded->PayloadAsStringView().empty());
    }

    // Test B: String payload with JSON flag
    {
        const std::string test_payload = "SELECT id, name FROM users WHERE active = 1;";
        std::vector<uint8_t> req_buf;
        Protocol::EncodeRequestInto(Opcode::SqlQuery, Protocol::FLAG_JSON, test_payload, req_buf);

        assert(req_buf.size() == Protocol::REQUEST_HEADER_SIZE + test_payload.size());
        assert(req_buf[0] == static_cast<uint8_t>(Opcode::SqlQuery));
        assert(req_buf[1] == Protocol::FLAG_JSON);

        auto decoded = Protocol::DecodeRequest(req_buf);
        assert(decoded.has_value());
        assert(decoded->opcode == Opcode::SqlQuery);
        assert(decoded->flags == Protocol::FLAG_JSON);
        assert(decoded->payload.size() == test_payload.size());
        assert(decoded->PayloadAsStringView() == test_payload);

        // Zero-copy verification: payload pointer must point directly inside req_buf
        assert(decoded->payload.data() == req_buf.data() + Protocol::REQUEST_HEADER_SIZE);
    }

    // Test C: Binary byte span payload
    {
        const std::vector<uint8_t> raw_binary = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00}; // WASM magic header
        std::vector<uint8_t> req_buf;
        Protocol::EncodeRequestInto(Opcode::WhiskPublish, Protocol::FLAG_BINARY, std::span<const uint8_t>(raw_binary), req_buf);

        assert(req_buf.size() == Protocol::REQUEST_HEADER_SIZE + raw_binary.size());
        assert(req_buf[0] == static_cast<uint8_t>(Opcode::WhiskPublish));
        assert(req_buf[1] == Protocol::FLAG_BINARY);

        auto decoded = Protocol::DecodeRequest(req_buf);
        assert(decoded.has_value());
        assert(decoded->opcode == Opcode::WhiskPublish);
        assert(decoded->flags == Protocol::FLAG_BINARY);
        assert(decoded->payload.size() == raw_binary.size());
        assert(std::memcmp(decoded->payload.data(), raw_binary.data(), raw_binary.size()) == 0);

        // Zero-copy verification
        assert(decoded->payload.data() == req_buf.data() + Protocol::REQUEST_HEADER_SIZE);
    }

    // Test D: Convenience wrapper Protocol::EncodeRequest
    {
        const std::string payload = "whisk_list_filter";
        auto req_buf = Protocol::EncodeRequest(Opcode::WhiskList, payload, Protocol::FLAG_JSON);

        auto decoded = Protocol::DecodeRequest(req_buf);
        assert(decoded.has_value());
        assert(decoded->opcode == Opcode::WhiskList);
        assert(decoded->flags == Protocol::FLAG_JSON);
        assert(decoded->PayloadAsStringView() == payload);
        assert(decoded->payload.data() == req_buf.data() + Protocol::REQUEST_HEADER_SIZE);
    }

    std::println("  Request encoding and zero-copy decoding verified.");
}

// 3. Protocol::EncodeResponseInto and Protocol::DecodeResponse zero-copy ResponseView
void TestEncodeDecodeResponseZeroCopy() {
    std::println("--- [3/5] Testing EncodeResponseInto & DecodeResponse (Zero-Copy) ---");

    // Test A: Empty payload with Ok status and exec_time_us
    {
        std::vector<uint8_t> resp_buf;
        constexpr uint32_t exec_us = 42;
        Protocol::EncodeResponseInto(ErrorCode::Ok, exec_us, std::string_view(""), resp_buf);

        assert(resp_buf.size() == Protocol::RESPONSE_HEADER_SIZE);

        auto decoded = Protocol::DecodeResponse(resp_buf);
        assert(decoded.has_value());
        assert(decoded->IsOk());
        assert(decoded->status == ErrorCode::Ok);
        assert(decoded->exec_time_us == exec_us);
        assert(decoded->payload.empty());
        assert(decoded->PayloadAsStringView().empty());
    }

    // Test B: Non-empty payload, non-zero execution time, big-endian round-trip
    {
        const std::string json_response = "{\"status\":\"success\",\"records\":[1,2,3]}";
        constexpr uint32_t exec_us = 1234567; // Multi-byte uint32 for endianness check
        std::vector<uint8_t> resp_buf;
        Protocol::EncodeResponseInto(ErrorCode::Ok, exec_us, json_response, resp_buf);

        assert(resp_buf.size() == Protocol::RESPONSE_HEADER_SIZE + json_response.size());

        auto decoded = Protocol::DecodeResponse(resp_buf);
        assert(decoded.has_value());
        assert(decoded->IsOk());
        assert(decoded->status == ErrorCode::Ok);
        assert(decoded->exec_time_us == exec_us);
        assert(decoded->payload.size() == json_response.size());
        assert(decoded->PayloadAsStringView() == json_response);

        // Zero-copy verification: payload pointer must point directly inside resp_buf
        assert(decoded->payload.data() == resp_buf.data() + Protocol::RESPONSE_HEADER_SIZE);
    }

    // Test C: Error status with binary span payload
    {
        const std::string err_text = "Relation 'orders' does not exist";
        std::span<const uint8_t> err_span(reinterpret_cast<const uint8_t*>(err_text.data()), err_text.size());
        constexpr uint32_t exec_us = 999;
        std::vector<uint8_t> resp_buf;
        Protocol::EncodeResponseInto(ErrorCode::TableDoesNotExist, exec_us, err_span, resp_buf);

        auto decoded = Protocol::DecodeResponse(resp_buf);
        assert(decoded.has_value());
        assert(!decoded->IsOk());
        assert(decoded->status == ErrorCode::TableDoesNotExist);
        assert(decoded->exec_time_us == exec_us);
        assert(decoded->PayloadAsStringView() == err_text);
        assert(decoded->payload.data() == resp_buf.data() + Protocol::RESPONSE_HEADER_SIZE);
    }

    // Test D: Convenience wrapper Protocol::EncodeResponse
    {
        const std::string payload = "snapshot_saved";
        auto resp_buf = Protocol::EncodeResponse(ErrorCode::Ok, 8888, payload);

        auto decoded = Protocol::DecodeResponse(resp_buf);
        assert(decoded.has_value());
        assert(decoded->IsOk());
        assert(decoded->status == ErrorCode::Ok);
        assert(decoded->exec_time_us == 8888);
        assert(decoded->PayloadAsStringView() == payload);
        assert(decoded->payload.data() == resp_buf.data() + Protocol::RESPONSE_HEADER_SIZE);
    }

    std::println("  Response encoding and zero-copy decoding verified.");
}

// 4. Negative tests: DecodeRequest < 2 bytes, unknown opcode, DecodeResponse < 6 bytes
void TestNegativeDecoding() {
    std::println("--- [4/5] Testing Negative Decoding & Protocol Errors ---");

    // DecodeRequest: buffer too short (< 2 bytes)
    {
        // 0 bytes
        std::vector<uint8_t> empty_buf;
        auto res0 = Protocol::DecodeRequest(empty_buf);
        assert(!res0.has_value());
        assert(res0.error().Code() == ErrorCode::ProtocolError);

        // 1 byte
        std::vector<uint8_t> one_byte = {static_cast<uint8_t>(Opcode::Ping)};
        auto res1 = Protocol::DecodeRequest(one_byte);
        assert(!res1.has_value());
        assert(res1.error().Code() == ErrorCode::ProtocolError);
    }

    // DecodeRequest: unknown opcode
    {
        // 0x00
        const std::vector<uint8_t> unk0 = {0x00, 0x00};
        auto res_unk0 = Protocol::DecodeRequest(unk0);
        assert(!res_unk0.has_value());
        assert(res_unk0.error().Code() == ErrorCode::UnknownOpcode);

        // 0x08
        const std::vector<uint8_t> unk8 = {0x08, 0x00};
        auto res_unk8 = Protocol::DecodeRequest(unk8);
        assert(!res_unk8.has_value());
        assert(res_unk8.error().Code() == ErrorCode::UnknownOpcode);

        // 0xFF
        const std::vector<uint8_t> unkff = {0xFF, 0x01, 'a', 'b', 'c'};
        auto res_unkff = Protocol::DecodeRequest(unkff);
        assert(!res_unkff.has_value());
        assert(res_unkff.error().Code() == ErrorCode::UnknownOpcode);
    }

    // DecodeResponse: buffer too short (< 6 bytes)
    {
        // 0 bytes
        std::vector<uint8_t> empty_buf;
        auto res0 = Protocol::DecodeResponse(empty_buf);
        assert(!res0.has_value());
        assert(res0.error().Code() == ErrorCode::ProtocolError);

        // 1 byte through 5 bytes
        for (idx_t len = 1; len < Protocol::RESPONSE_HEADER_SIZE; ++len) {
            std::vector<uint8_t> short_buf(len, 0x00);
            auto res = Protocol::DecodeResponse(short_buf);
            assert(!res.has_value());
            assert(res.error().Code() == ErrorCode::ProtocolError);
        }
    }

    std::println("  Negative decoding cases passed.");
}

// 5. ProtocolDispatcher tests
void TestProtocolDispatcher() {
    std::println("--- [5/5] Testing ProtocolDispatcher ---");

    ProtocolDispatcher dispatcher;

    // A: Built-in Opcode::Ping with empty payload -> "PONG"
    {
        auto ping_req = Protocol::EncodeRequest(Opcode::Ping, "", Protocol::FLAG_NONE);
        std::vector<uint8_t> resp_buf;
        dispatcher.Dispatch(ping_req, resp_buf);

        auto dec = Protocol::DecodeResponse(resp_buf);
        assert(dec.has_value());
        assert(dec->IsOk());
        assert(dec->status == ErrorCode::Ok);
        assert(dec->PayloadAsStringView() == "PONG");
    }

    // B: Built-in Opcode::Ping with non-empty payload -> echoed payload
    {
        const std::string echo_msg = "heartbeat-test-payload-12345";
        auto ping_req = Protocol::EncodeRequest(Opcode::Ping, echo_msg, Protocol::FLAG_NONE);
        std::vector<uint8_t> resp_buf;
        dispatcher.Dispatch(ping_req, resp_buf);

        auto dec = Protocol::DecodeResponse(resp_buf);
        assert(dec.has_value());
        assert(dec->IsOk());
        assert(dec->status == ErrorCode::Ok);
        assert(dec->PayloadAsStringView() == echo_msg);
    }

    // C: Unregistered opcode returning ErrorCode::NotImplemented
    {
        // Opcode::Snapshot is not registered by default
        auto snap_req = Protocol::EncodeRequest(Opcode::Snapshot, "", Protocol::FLAG_NONE);
        std::vector<uint8_t> resp_buf;
        dispatcher.Dispatch(snap_req, resp_buf);

        auto dec = Protocol::DecodeResponse(resp_buf);
        assert(dec.has_value());
        assert(!dec->IsOk());
        assert(dec->status == ErrorCode::NotImplemented);
        assert(dec->PayloadAsStringView().find("No handler registered for opcode SNAPSHOT") != std::string_view::npos);
    }

    // D: Custom registered Opcode::SqlQuery handler (success & error paths)
    {
        dispatcher.RegisterHandler(Opcode::SqlQuery, [](const RequestView& req) -> Result<std::string> {
            if (req.PayloadAsStringView() == "SELECT 1;") {
                return std::string("{\"rows\":[[1]]}");
            }
            return MakeError(ErrorCode::InvalidArgument, "syntax error near unexpected token");
        });

        // Success case
        {
            auto sql_req = Protocol::EncodeRequest(Opcode::SqlQuery, "SELECT 1;", Protocol::FLAG_JSON);
            std::vector<uint8_t> resp_buf;
            dispatcher.Dispatch(sql_req, resp_buf);

            auto dec = Protocol::DecodeResponse(resp_buf);
            assert(dec.has_value());
            assert(dec->IsOk());
            assert(dec->status == ErrorCode::Ok);
            assert(dec->PayloadAsStringView() == "{\"rows\":[[1]]}");
        }

        // Error case
        {
            auto sql_req = Protocol::EncodeRequest(Opcode::SqlQuery, "SELCT ERROR", Protocol::FLAG_JSON);
            std::vector<uint8_t> resp_buf;
            dispatcher.Dispatch(sql_req, resp_buf);

            auto dec = Protocol::DecodeResponse(resp_buf);
            assert(dec.has_value());
            assert(!dec->IsOk());
            assert(dec->status == ErrorCode::InvalidArgument);
            assert(dec->PayloadAsStringView() == "syntax error near unexpected token");
        }
    }

    // E: Custom registered Opcode::WhiskCall handler (success & error paths)
    {
        dispatcher.RegisterHandler(Opcode::WhiskCall, [](const RequestView& req) -> Result<std::string> {
            if (req.PayloadAsStringView() == "math.add [10, 20]") {
                return std::string("{\"result\": 30}");
            }
            return MakeError(ErrorCode::WasmTrap, "unreachable executed: integer overflow in guest");
        });

        // Success case
        {
            auto call_req = Protocol::EncodeRequest(Opcode::WhiskCall, "math.add [10, 20]", Protocol::FLAG_JSON);
            std::vector<uint8_t> resp_buf;
            dispatcher.Dispatch(call_req, resp_buf);

            auto dec = Protocol::DecodeResponse(resp_buf);
            assert(dec.has_value());
            assert(dec->IsOk());
            assert(dec->status == ErrorCode::Ok);
            assert(dec->PayloadAsStringView() == "{\"result\": 30}");
        }

        // Error case
        {
            auto call_req = Protocol::EncodeRequest(Opcode::WhiskCall, "math.add [max, 1]", Protocol::FLAG_JSON);
            std::vector<uint8_t> resp_buf;
            dispatcher.Dispatch(call_req, resp_buf);

            auto dec = Protocol::DecodeResponse(resp_buf);
            assert(dec.has_value());
            assert(!dec->IsOk());
            assert(dec->status == ErrorCode::WasmTrap);
            assert(dec->PayloadAsStringView() == "unreachable executed: integer overflow in guest");
        }
    }

    // F: Dispatch with malformed request (decoding error)
    {
        // 1 byte request frame (fails DecodeRequest with ProtocolError)
        const std::vector<uint8_t> bad_req = {static_cast<uint8_t>(Opcode::Ping)};
        std::vector<uint8_t> resp_buf;
        dispatcher.Dispatch(bad_req, resp_buf);

        auto dec = Protocol::DecodeResponse(resp_buf);
        assert(dec.has_value());
        assert(!dec->IsOk());
        assert(dec->status == ErrorCode::ProtocolError);
    }

    std::println("  ProtocolDispatcher tests passed.");
}

} // namespace

int main() {
    std::println("==================================================");
    std::println("Running WhiskDB Wire Protocol & Dispatcher Tests");
    std::println("==================================================");

    TestOpcodeValidationAndToString();
    TestEncodeDecodeRequestZeroCopy();
    TestEncodeDecodeResponseZeroCopy();
    TestNegativeDecoding();
    TestProtocolDispatcher();

    std::println("==================================================");
    std::println("All Wire Protocol & Dispatcher unit tests PASSED!");
    std::println("==================================================");
    return 0;
}
