#ifndef WHISKDB_NETWORK_FRAMING_HPP
#define WHISKDB_NETWORK_FRAMING_HPP

#include "common/types.hpp"
#include "errors.hpp"

#include <cstdint>
#include <span>

namespace database {

struct Framing {
    static constexpr uint32_t MAX_PAYLOAD_SIZE = 16 * 1024 * 1024;
    static constexpr idx_t HEADER_SIZE = 4;

    static void EncodeHeader(uint32_t payload_len, std::span<uint8_t, 4> out);
    static Result<uint32_t> DecodeHeader(std::span<const uint8_t, 4> in);
};

} // namespace database

#endif // WHISKDB_NETWORK_FRAMING_HPP
