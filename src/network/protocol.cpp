#include "network/protocol.hpp"

namespace database {

void Protocol::EncodeRequest(std::vector<uint8_t>& out, const Request& req) {
    uint32_t payload_len = static_cast<uint32_t>(1 + req.payload.size());
    size_t orig_size = out.size();
    out.resize(orig_size + 4);
    std::span<uint8_t, 4> hdr(out.data() + orig_size, 4);
    Framing::EncodeHeader(payload_len, hdr);

    out.push_back(static_cast<uint8_t>(req.opcode));
    out.insert(out.end(), req.payload.begin(), req.payload.end());
}

Result<Request> Protocol::DecodeRequest(std::span<const uint8_t> in) {
    if (in.size() >= 4) {
        uint32_t len = (static_cast<uint32_t>(in[0]) << 24) |
                       (static_cast<uint32_t>(in[1]) << 16) |
                       (static_cast<uint32_t>(in[2]) << 8) |
                       static_cast<uint32_t>(in[3]);
        if (len == in.size() - 4) {
            in = in.subspan(4);
        }
    }

    if (in.empty()) {
        return MakeError(ErrorCode::ProtocolError, "Request payload is empty");
    }

    uint8_t raw_opcode = in[0];
    if (!IsValidOpcode(raw_opcode)) {
        return MakeError(ErrorCode::UnknownOpcode, "Unknown opcode");
    }

    Request req;
    req.opcode = static_cast<Opcode>(raw_opcode);
    auto payload_span = in.subspan(1);
    req.payload.assign(payload_span.begin(), payload_span.end());
    return req;
}

void Protocol::EncodeResponse(std::vector<uint8_t>& out, const Response& resp) {
    uint32_t payload_len = static_cast<uint32_t>(RESPONSE_HEADER_SIZE + resp.payload.size());
    size_t orig_size = out.size();
    out.resize(orig_size + 4);
    std::span<uint8_t, 4> hdr(out.data() + orig_size, 4);
    Framing::EncodeHeader(payload_len, hdr);

    uint16_t status_val = static_cast<uint16_t>(resp.status);
    out.push_back(static_cast<uint8_t>((status_val >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(status_val & 0xFF));

    for (int i = 7; i >= 0; --i) {
        out.push_back(static_cast<uint8_t>((resp.exec_us >> (i * 8)) & 0xFF));
    }

    out.insert(out.end(), resp.payload.begin(), resp.payload.end());
}

Result<Response> Protocol::DecodeResponse(std::span<const uint8_t> in) {
    if (in.size() >= 4) {
        uint32_t len = (static_cast<uint32_t>(in[0]) << 24) |
                       (static_cast<uint32_t>(in[1]) << 16) |
                       (static_cast<uint32_t>(in[2]) << 8) |
                       static_cast<uint32_t>(in[3]);
        if (len == in.size() - 4) {
            in = in.subspan(4);
        }
    }

    if (in.size() < RESPONSE_HEADER_SIZE) {
        return MakeError(ErrorCode::ProtocolError, "Response buffer smaller than response header size");
    }

    uint16_t status_val = (static_cast<uint16_t>(in[0]) << 8) | static_cast<uint16_t>(in[1]);
    uint64_t exec_us = 0;
    for (int i = 0; i < 8; ++i) {
        exec_us = (exec_us << 8) | static_cast<uint64_t>(in[2 + i]);
    }

    Response resp;
    resp.status = static_cast<ErrorCode>(status_val);
    resp.exec_us = exec_us;
    auto payload_span = in.subspan(RESPONSE_HEADER_SIZE);
    resp.payload.assign(payload_span.begin(), payload_span.end());
    return resp;
}

} // namespace database
