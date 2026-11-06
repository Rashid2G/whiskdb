#ifndef WHISKDB_NETWORK_PROTOCOL_HPP
#define WHISKDB_NETWORK_PROTOCOL_HPP

#include "common/types.hpp"
#include "errors.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace database {

/**
 * @brief WhiskDB Wire Protocol Opcodes.
 *
 * Every length-prefixed TCP frame carries a 2-byte Request Header:
 * +--------------------+--------------------+------------------------+
 * | Opcode (1 byte)    | Flags (1 byte)     | Payload (N bytes)      |
 * +--------------------+--------------------+------------------------+
 *
 * Every length-prefixed TCP response carries a 6-byte Response Header:
 * +----------------------------+----------------------------+------------------------+
 * | Status (2B, Big-Endian)    | ExecTimeUs (4B, Big-End)   | Payload (M bytes)      |
 * +----------------------------+----------------------------+------------------------+
 */
enum class Opcode : uint8_t {
    Ping         = 0x01,
    SqlQuery     = 0x02,
    WhiskPublish = 0x03,
    WhiskCall    = 0x04,
    WhiskList    = 0x05,
    WhiskLogs    = 0x06,
    Snapshot     = 0x07
};

constexpr bool IsValidOpcode(uint8_t raw_opcode) noexcept {
    return raw_opcode >= static_cast<uint8_t>(Opcode::Ping) &&
           raw_opcode <= static_cast<uint8_t>(Opcode::Snapshot);
}

constexpr std::string_view ToString(Opcode opcode) noexcept {
    switch (opcode) {
        case Opcode::Ping:         return "PING";
        case Opcode::SqlQuery:     return "SQL_QUERY";
        case Opcode::WhiskPublish: return "WHISK_PUBLISH";
        case Opcode::WhiskCall:    return "WHISK_CALL";
        case Opcode::WhiskList:    return "WHISK_LIST";
        case Opcode::WhiskLogs:    return "WHISK_LOGS";
        case Opcode::Snapshot:     return "SNAPSHOT";
    }
    return "UNKNOWN";
}

/**
 * @brief Zero-copy view of a decoded client request inside a frame buffer.
 */
struct RequestView {
    Opcode opcode{Opcode::Ping};
    uint8_t flags{0};
    std::span<const uint8_t> payload{};

    std::string_view PayloadAsStringView() const noexcept {
        return std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size());
    }
};

/**
 * @brief Zero-copy view of a decoded server response inside a frame buffer.
 */
struct ResponseView {
    ErrorCode status{ErrorCode::Ok};
    uint32_t exec_time_us{0};
    std::span<const uint8_t> payload{};

    bool IsOk() const noexcept { return status == ErrorCode::Ok; }

    std::string_view PayloadAsStringView() const noexcept {
        return std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size());
    }
};

/**
 * @brief Binary encoding/decoding routines for the WhiskDB wire protocol.
 */
struct Protocol {
    static constexpr idx_t REQUEST_HEADER_SIZE = 2;
    static constexpr idx_t RESPONSE_HEADER_SIZE = 6;

    static constexpr uint8_t FLAG_NONE   = 0x00;
    static constexpr uint8_t FLAG_JSON   = 0x01;
    static constexpr uint8_t FLAG_BINARY = 0x02;

    /// Encode a request (Opcode + Flags + Payload) into a reusable byte vector
    static void EncodeRequestInto(Opcode opcode,
                                  uint8_t flags,
                                  std::span<const uint8_t> payload,
                                  std::vector<uint8_t>& out_buf);

    static void EncodeRequestInto(Opcode opcode,
                                  uint8_t flags,
                                  std::string_view payload,
                                  std::vector<uint8_t>& out_buf);

    static std::vector<uint8_t> EncodeRequest(Opcode opcode,
                                              std::string_view payload = "",
                                              uint8_t flags = FLAG_NONE);

    /// Decode a raw frame payload into a zero-copy RequestView
    static Result<RequestView> DecodeRequest(std::span<const uint8_t> raw_frame) noexcept;

    /// Encode a response (Status + ExecTimeUs + Payload) into a reusable byte vector
    static void EncodeResponseInto(ErrorCode status,
                                   uint32_t exec_time_us,
                                   std::span<const uint8_t> payload,
                                   std::vector<uint8_t>& out_buf);

    static void EncodeResponseInto(ErrorCode status,
                                   uint32_t exec_time_us,
                                   std::string_view payload,
                                   std::vector<uint8_t>& out_buf);

    static std::vector<uint8_t> EncodeResponse(ErrorCode status,
                                               uint32_t exec_time_us,
                                               std::string_view payload);

    /// Decode a raw response frame payload into a zero-copy ResponseView
    static Result<ResponseView> DecodeResponse(std::span<const uint8_t> raw_frame) noexcept;
};

/**
 * @brief Server-side Opcode Dispatcher with built-in microsecond latency measurement.
 */
class ProtocolDispatcher {
public:
    using OpcodeHandler = std::function<Result<std::string>(const RequestView&)>;

    ProtocolDispatcher();
    ~ProtocolDispatcher() = default;

public:
    /// Register or override a handler for a specific Opcode
    void RegisterHandler(Opcode opcode, OpcodeHandler handler);

    /// Dispatch a raw frame payload and write the encoded binary response into out_response
    void Dispatch(std::span<const uint8_t> raw_request,
                  std::vector<uint8_t>& out_response) const;

private:
    static constexpr idx_t MAX_OPCODES = 16;
    std::array<OpcodeHandler, MAX_OPCODES> handlers_{};
};

} // namespace database

namespace Database = database;

#endif // WHISKDB_NETWORK_PROTOCOL_HPP
