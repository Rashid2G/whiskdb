#ifndef WHISKDB_NETWORK_PROTOCOL_HPP
#define WHISKDB_NETWORK_PROTOCOL_HPP

#include "common/types.hpp"
#include "errors.hpp"
#include "network/framing.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace database {

enum class Opcode : uint8_t {
    Ping         = 0x01,
    SqlQuery     = 0x02,
    WhiskPublish = 0x03,
    WhiskCall    = 0x04,
    WhiskList    = 0x05,
    Logs         = 0x06,
    Snapshot     = 0x07
};

constexpr bool IsValidOpcode(uint8_t raw_opcode) noexcept {
    return raw_opcode >= static_cast<uint8_t>(Opcode::Ping) &&
           raw_opcode <= static_cast<uint8_t>(Opcode::Snapshot);
}

constexpr std::string_view ToString(Opcode opcode) noexcept {
    switch (opcode) {
        case Opcode::Ping:         return "Ping";
        case Opcode::SqlQuery:     return "SqlQuery";
        case Opcode::WhiskPublish: return "WhiskPublish";
        case Opcode::WhiskCall:    return "WhiskCall";
        case Opcode::WhiskList:    return "WhiskList";
        case Opcode::Logs:         return "Logs";
        case Opcode::Snapshot:     return "Snapshot";
    }
    return "Unknown";
}

struct Request {
    Opcode opcode{Opcode::Ping};
    std::vector<uint8_t> payload;
};

struct Response {
    ErrorCode status = ErrorCode::Ok;
    uint64_t exec_us = 0;
    std::vector<uint8_t> payload;

    bool IsOk() const noexcept { return status == ErrorCode::Ok; }
};

struct Protocol {
    static constexpr idx_t REQUEST_HEADER_SIZE = 1;     // [u8 opcode]
    static constexpr idx_t RESPONSE_HEADER_SIZE = 10;   // [u16 BE ErrorCode][u64 BE exec_us]

    /// Encodes request frame into `out` (appends: [u32 BE len][u8 opcode][payload])
    static void EncodeRequest(std::vector<uint8_t>& out, const Request& req);

    /// Decodes request from frame (or body without 4B length prefix)
    static Result<Request> DecodeRequest(std::span<const uint8_t> in);

    /// Encodes response frame into `out` (appends: [u32 BE len][u16 BE ErrorCode][u64 BE exec_us][payload])
    static void EncodeResponse(std::vector<uint8_t>& out, const Response& resp);

    /// Decodes response from frame (or body without 4B length prefix)
    static Result<Response> DecodeResponse(std::span<const uint8_t> in);
};

inline void EncodeRequest(std::vector<uint8_t>& out, const Request& req) {
    Protocol::EncodeRequest(out, req);
}

inline Result<Request> DecodeRequest(std::span<const uint8_t> in) {
    return Protocol::DecodeRequest(in);
}

inline void EncodeResponse(std::vector<uint8_t>& out, const Response& resp) {
    Protocol::EncodeResponse(out, resp);
}

inline Result<Response> DecodeResponse(std::span<const uint8_t> in) {
    return Protocol::DecodeResponse(in);
}

} // namespace database

#endif // WHISKDB_NETWORK_PROTOCOL_HPP
