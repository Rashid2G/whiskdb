#include "common/crc32.hpp"
#include "common/types.hpp"

#include <cassert>
#include <cstring>
#include <print>
#include <string_view>
#include <vector>

using namespace database;

namespace {

std::span<const uint8_t> AsBytes(std::string_view s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

} // namespace

int main() {
    // Standard check value for CRC-32/ISO-HDLC
    assert(Crc32(AsBytes("123456789")) == 0xCBF43926U);
    assert(Crc32({}) == 0U);
    assert(Crc32(AsBytes("a")) == 0xE8B7BE43U);
    assert(Crc32(AsBytes("The quick brown fox jumps over the lazy dog")) == 0x414FA339U);

    // Incremental equals one-shot, at every split point, across the slicing-by-8 boundary
    std::vector<uint8_t> data(1000);
    for (idx_t i = 0; i < data.size(); i++) {
        data[i] = static_cast<uint8_t>((i * 131 + 7) & 0xFF);
    }
    uint32_t whole = Crc32(data);
    for (idx_t split = 0; split <= data.size(); split++) {
        std::span<const uint8_t> all(data);
        uint32_t part = Crc32(all.first(split));
        part = Crc32(all.subspan(split), part);
        assert(part == whole);
    }

    // Three-way incremental and byte-at-a-time
    uint32_t bytewise = 0;
    for (uint8_t byte : data) {
        bytewise = Crc32(std::span<const uint8_t>(&byte, 1), bytewise);
    }
    assert(bytewise == whole);

    // Different data gives a different checksum
    data[500] ^= 1;
    assert(Crc32(data) != whole);

    std::println("test_crc32: OK");
    return 0;
}
