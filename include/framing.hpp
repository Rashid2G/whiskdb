#ifndef WHISKDB_FRAMING_HPP
#define WHISKDB_FRAMING_HPP

#include "common/types.hpp"
#include "errors.hpp"

#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace database {

/**
 * @brief Wire protocol framing: 4-byte big-endian length prefix followed by payload.
 *
 * Wire format:
 * +-----------------------------------+-----------------------------+
 * | Length (4 bytes, Network/Big-End) | Payload (Length bytes)      |
 * +-----------------------------------+-----------------------------+
 */
struct Framing {
    /// 4 bytes header for uint32_t length
    static constexpr idx_t HEADER_SIZE = sizeof(uint32_t);

    /// Maximum permitted payload (16 MB default) to prevent memory exhaustion
    static constexpr uint32_t MAX_PAYLOAD_SIZE = 16U * 1024U * 1024U;

    /// Convert 32-bit integer from host to network byte order
    static inline uint32_t ToNetwork(uint32_t host_val) noexcept {
        return htonl(host_val);
    }
    static inline uint32_t to_network(uint32_t host_val) noexcept {
        return ToNetwork(host_val);
    }

    /// Convert 32-bit integer from network to host byte order
    static inline uint32_t ToHost(uint32_t net_val) noexcept {
        return ntohl(net_val);
    }
    static inline uint32_t to_host(uint32_t net_val) noexcept {
        return ToHost(net_val);
    }

    /// Validate that payload length does not exceed MAX_PAYLOAD_SIZE
    static inline Result<void> ValidatePayloadLength(uint32_t payload_len) noexcept {
        if (payload_len > MAX_PAYLOAD_SIZE) {
            return MakeError(ErrorCode::PayloadTooLarge,
                             std::format("Payload size {} exceeds maximum allowed {} bytes",
                                         payload_len, MAX_PAYLOAD_SIZE));
        }
        return {};
    }

    /// Encode length prefix into an array of 4 bytes
    static inline void EncodeHeader(uint32_t payload_len, uint8_t* out_header) noexcept {
        // Validate that output header buffer pointer is non-null
        D_ASSERT(out_header != nullptr);
        uint32_t net_len = ToNetwork(payload_len);
        std::memcpy(out_header, &net_len, HEADER_SIZE);
    }
    static inline void encode_header(uint32_t payload_len, uint8_t* out_header) noexcept {
        EncodeHeader(payload_len, out_header);
    }

    /// Decode length prefix from 4 bytes
    static inline uint32_t DecodeHeader(const uint8_t* header_bytes) noexcept {
        // Validate that input header buffer pointer is non-null
        D_ASSERT(header_bytes != nullptr);
        uint32_t net_len = 0;
        std::memcpy(&net_len, header_bytes, HEADER_SIZE);
        return ToHost(net_len);
    }
    static inline uint32_t decode_header(const uint8_t* header_bytes) noexcept {
        return DecodeHeader(header_bytes);
    }

    /// Decode and validate length prefix from a byte span
    static inline Result<uint32_t> DecodeAndValidateHeader(std::span<const uint8_t> header_span) noexcept {
        if (header_span.size() < HEADER_SIZE) {
            return MakeError(ErrorCode::ProtocolError, "Header span smaller than 4 bytes");
        }
        uint32_t len = DecodeHeader(header_span.data());
        if (auto res = ValidatePayloadLength(len); !res) {
            return std::unexpected(res.error());
        }
        return len;
    }

    /// Pack a binary payload span into a reusable output buffer (zero heap allocation when capacity suffices)
    static inline Result<void> PackFrameInto(std::span<const uint8_t> payload,
                                             std::vector<uint8_t>& out_frame) {
        if (payload.size() > MAX_PAYLOAD_SIZE) {
            return MakeError(ErrorCode::PayloadTooLarge,
                             std::format("Payload size {} exceeds maximum allowed {} bytes",
                                         payload.size(), MAX_PAYLOAD_SIZE));
        }
        const uint32_t payload_len = static_cast<uint32_t>(payload.size());
        out_frame.resize(HEADER_SIZE + payload_len);
        EncodeHeader(payload_len, out_frame.data());
        if (payload_len > 0) {
            std::memcpy(out_frame.data() + HEADER_SIZE, payload.data(), payload_len);
        }
        return {};
    }

    /// Pack a string_view payload into a reusable output buffer
    static inline Result<void> PackFrameInto(std::string_view payload,
                                             std::vector<uint8_t>& out_frame) {
        std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
        return PackFrameInto(bytes, out_frame);
    }

    /// Pack a binary payload span into a new length-prefixed wire frame
    static inline Result<std::vector<uint8_t>> PackFrameBytes(std::span<const uint8_t> payload) {
        std::vector<uint8_t> frame;
        if (auto res = PackFrameInto(payload, frame); !res) {
            return std::unexpected(res.error());
        }
        return frame;
    }

    /// Pack a payload string_view into a length-prefixed wire frame
    static inline std::vector<uint8_t> PackFrame(std::string_view payload) {
        // Validate that string payload fits within MAX_PAYLOAD_SIZE
        D_ASSERT(payload.size() <= MAX_PAYLOAD_SIZE);
        std::vector<uint8_t> frame(HEADER_SIZE + payload.size());
        EncodeHeader(static_cast<uint32_t>(payload.size()), frame.data());
        if (!payload.empty()) {
            std::memcpy(frame.data() + HEADER_SIZE, payload.data(), payload.size());
        }
        return frame;
    }

    static inline std::vector<uint8_t> pack_frame(std::string_view payload) {
        return PackFrame(payload);
    }
};

} // namespace database

namespace Database = database;

#endif // WHISKDB_FRAMING_HPP
