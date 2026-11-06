#include "network/protocol.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstring>

namespace database {

void Protocol::EncodeRequestInto(Opcode opcode,
                                 uint8_t flags,
                                 std::span<const uint8_t> payload,
                                 std::vector<uint8_t>& out_buf) {
    out_buf.resize(REQUEST_HEADER_SIZE + payload.size());
    out_buf[0] = static_cast<uint8_t>(opcode);
    out_buf[1] = flags;
    if (!payload.empty()) {
        std::memcpy(out_buf.data() + REQUEST_HEADER_SIZE, payload.data(), payload.size());
    }
}

void Protocol::EncodeRequestInto(Opcode opcode,
                                 uint8_t flags,
                                 std::string_view payload,
                                 std::vector<uint8_t>& out_buf) {
    std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    EncodeRequestInto(opcode, flags, bytes, out_buf);
}

std::vector<uint8_t> Protocol::EncodeRequest(Opcode opcode,
                                             std::string_view payload,
                                             uint8_t flags) {
    std::vector<uint8_t> buf;
    EncodeRequestInto(opcode, flags, payload, buf);
    return buf;
}

Result<RequestView> Protocol::DecodeRequest(std::span<const uint8_t> raw_frame) noexcept {
    if (raw_frame.size() < REQUEST_HEADER_SIZE) {
        return MakeError(ErrorCode::ProtocolError,
                         std::format("Request frame too short ({} bytes, expected >= {})",
                                     raw_frame.size(), REQUEST_HEADER_SIZE));
    }

    const uint8_t raw_op = raw_frame[0];
    if (!IsValidOpcode(raw_op)) {
        return MakeError(ErrorCode::UnknownOpcode,
                         std::format("Unknown wire protocol opcode: 0x{:02X}", raw_op));
    }

    RequestView view;
    view.opcode = static_cast<Opcode>(raw_op);
    view.flags = raw_frame[1];
    view.payload = raw_frame.subspan(REQUEST_HEADER_SIZE);
    return view;
}

void Protocol::EncodeResponseInto(ErrorCode status,
                                  uint32_t exec_time_us,
                                  std::span<const uint8_t> payload,
                                  std::vector<uint8_t>& out_buf) {
    out_buf.resize(RESPONSE_HEADER_SIZE + payload.size());

    const uint16_t net_status = htons(static_cast<uint16_t>(status));
    const uint32_t net_exec_us = htonl(exec_time_us);

    std::memcpy(out_buf.data(), &net_status, sizeof(uint16_t));
    std::memcpy(out_buf.data() + sizeof(uint16_t), &net_exec_us, sizeof(uint32_t));

    if (!payload.empty()) {
        std::memcpy(out_buf.data() + RESPONSE_HEADER_SIZE, payload.data(), payload.size());
    }
}

void Protocol::EncodeResponseInto(ErrorCode status,
                                  uint32_t exec_time_us,
                                  std::string_view payload,
                                  std::vector<uint8_t>& out_buf) {
    std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    EncodeResponseInto(status, exec_time_us, bytes, out_buf);
}

std::vector<uint8_t> Protocol::EncodeResponse(ErrorCode status,
                                              uint32_t exec_time_us,
                                              std::string_view payload) {
    std::vector<uint8_t> buf;
    EncodeResponseInto(status, exec_time_us, payload, buf);
    return buf;
}

Result<ResponseView> Protocol::DecodeResponse(std::span<const uint8_t> raw_frame) noexcept {
    if (raw_frame.size() < RESPONSE_HEADER_SIZE) {
        return MakeError(ErrorCode::ProtocolError,
                         std::format("Response frame too short ({} bytes, expected >= {})",
                                     raw_frame.size(), RESPONSE_HEADER_SIZE));
    }

    uint16_t net_status = 0;
    uint32_t net_exec_us = 0;
    std::memcpy(&net_status, raw_frame.data(), sizeof(uint16_t));
    std::memcpy(&net_exec_us, raw_frame.data() + sizeof(uint16_t), sizeof(uint32_t));

    ResponseView view;
    view.status = static_cast<ErrorCode>(ntohs(net_status));
    view.exec_time_us = ntohl(net_exec_us);
    view.payload = raw_frame.subspan(RESPONSE_HEADER_SIZE);
    return view;
}

ProtocolDispatcher::ProtocolDispatcher() {
    // Default built-in handler for Opcode::Ping
    RegisterHandler(Opcode::Ping, [](const RequestView& req) -> Result<std::string> {
        if (req.payload.empty()) {
            return std::string("PONG");
        }
        return std::string(req.PayloadAsStringView());
    });
}

void ProtocolDispatcher::RegisterHandler(Opcode opcode, OpcodeHandler handler) {
    const idx_t op_idx = static_cast<idx_t>(opcode);
    // Validate that opcode index fits within fixed handler table bounds
    D_ASSERT(op_idx < MAX_OPCODES);
    handlers_[op_idx] = std::move(handler);
}

void ProtocolDispatcher::Dispatch(std::span<const uint8_t> raw_request,
                                  std::vector<uint8_t>& out_response) const {
    const auto start_tp = std::chrono::steady_clock::now();

    auto decoded = Protocol::DecodeRequest(raw_request);
    if (!decoded) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start_tp).count();
        Protocol::EncodeResponseInto(decoded.error().Code(),
                                     static_cast<uint32_t>(elapsed),
                                     decoded.error().Message(),
                                     out_response);
        return;
    }

    const idx_t op_idx = static_cast<idx_t>(decoded->opcode);
    // Validate that decoded opcode index is within bounds
    D_ASSERT(op_idx < MAX_OPCODES);

    const auto& handler = handlers_[op_idx];
    if (!handler) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start_tp).count();
        Protocol::EncodeResponseInto(
            ErrorCode::NotImplemented,
            static_cast<uint32_t>(elapsed),
            std::format("No handler registered for opcode {}", ToString(decoded->opcode)),
            out_response);
        return;
    }

    auto result = handler(*decoded);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start_tp).count();
    const uint32_t exec_us = static_cast<uint32_t>(elapsed);

    if (result) {
        Protocol::EncodeResponseInto(ErrorCode::Ok, exec_us, *result, out_response);
    } else {
        Protocol::EncodeResponseInto(result.error().Code(),
                                     exec_us,
                                     result.error().Message(),
                                     out_response);
    }
}

} // namespace database
