#include "network/framing.hpp"

namespace database {

void Framing::EncodeHeader(uint32_t payload_len, std::span<uint8_t, 4> out) {
    out[0] = static_cast<uint8_t>((payload_len >> 24) & 0xFF);
    out[1] = static_cast<uint8_t>((payload_len >> 16) & 0xFF);
    out[2] = static_cast<uint8_t>((payload_len >> 8) & 0xFF);
    out[3] = static_cast<uint8_t>(payload_len & 0xFF);
}

Result<uint32_t> Framing::DecodeHeader(std::span<const uint8_t, 4> in) {
    uint32_t len = (static_cast<uint32_t>(in[0]) << 24) |
                   (static_cast<uint32_t>(in[1]) << 16) |
                   (static_cast<uint32_t>(in[2]) << 8) |
                   static_cast<uint32_t>(in[3]);

    if (len > MAX_PAYLOAD_SIZE) {
        return MakeError(ErrorCode::PayloadTooLarge, "Frame payload length exceeds MAX_PAYLOAD_SIZE (16MB)");
    }
    return len;
}

} // namespace database
