#ifndef WHISKDB_NETWORK_WHISK_CODEC_HPP
#define WHISKDB_NETWORK_WHISK_CODEC_HPP

#include "common/types.hpp"
#include "errors.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace database {

// ============================================================================
// Opcode::WhiskPublish (0x03)
// ============================================================================

struct WhiskPublishRequest {
    std::string module_name;
    std::vector<uint8_t> wasm_bytes;

    bool operator==(const WhiskPublishRequest&) const = default;
};

struct WhiskPublishResponse {
    std::string module_name;
    int64_t version = 0;
    std::string sha256;
    std::vector<std::string> exported_funcs;

    bool operator==(const WhiskPublishResponse&) const = default;
};

// ============================================================================
// Opcode::WhiskCall (0x04)
// ============================================================================

struct WhiskCallRequest {
    std::string module_name;
    std::string func_name;
    std::string args_json;

    bool operator==(const WhiskCallRequest&) const = default;
};

struct WhiskCallResponse {
    std::string result_json;
    uint64_t fuel_used = 0;

    bool operator==(const WhiskCallResponse&) const = default;
};

// ============================================================================
// Opcode::WhiskList (0x05)
// ============================================================================

struct WhiskModuleSummary {
    std::string name;
    int64_t version = 0;
    std::string sha256;
    std::vector<std::string> exported_funcs;
    int64_t updated_at = 0; // unix epoch microseconds

    bool operator==(const WhiskModuleSummary&) const = default;
};

struct WhiskListResponse {
    std::vector<WhiskModuleSummary> modules;

    bool operator==(const WhiskListResponse&) const = default;
};

// ============================================================================
// Opcode::Logs (0x06)
// ============================================================================

struct LogsRequest {
    static constexpr uint32_t DEFAULT_LIMIT = 200;
    static constexpr uint32_t MIN_LIMIT = 1;
    static constexpr uint32_t MAX_LIMIT = 10000;
    uint32_t limit = DEFAULT_LIMIT;

    bool operator==(const LogsRequest&) const = default;
};

// ============================================================================
// Codec Functions
// ============================================================================

Result<void> EncodeWhiskPublishRequest(std::vector<uint8_t>& out, const WhiskPublishRequest& req);
Result<WhiskPublishRequest> DecodeWhiskPublishRequest(std::span<const uint8_t> in);

void EncodeWhiskPublishResponse(std::vector<uint8_t>& out, const WhiskPublishResponse& resp);
Result<WhiskPublishResponse> DecodeWhiskPublishResponse(std::span<const uint8_t> in);

Result<void> EncodeWhiskCallRequest(std::vector<uint8_t>& out, const WhiskCallRequest& req);
Result<WhiskCallRequest> DecodeWhiskCallRequest(std::span<const uint8_t> in);

void EncodeWhiskCallResponse(std::vector<uint8_t>& out, const WhiskCallResponse& resp);
Result<WhiskCallResponse> DecodeWhiskCallResponse(std::span<const uint8_t> in);

void EncodeWhiskListResponse(std::vector<uint8_t>& out, const WhiskListResponse& resp);
Result<WhiskListResponse> DecodeWhiskListResponse(std::span<const uint8_t> in);

void EncodeLogsRequest(std::vector<uint8_t>& out, const LogsRequest& req);
Result<LogsRequest> DecodeLogsRequest(std::span<const uint8_t> in);

} // namespace database

namespace Database = database;

#endif // WHISKDB_NETWORK_WHISK_CODEC_HPP
