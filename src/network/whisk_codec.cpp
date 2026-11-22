#include "network/whisk_codec.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>

namespace database {

namespace {

void WriteU32BE(std::vector<uint8_t>& out, uint32_t val) {
    const size_t orig_size = out.size();
    out.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(val & 0xFF));
    // Verify that exactly 4 bytes were appended
    D_ASSERT(out.size() == orig_size + 4);
}

void WriteU64BE(std::vector<uint8_t>& out, uint64_t val) {
    const size_t orig_size = out.size();
    for (int i = 7; i >= 0; --i) {
        out.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
    }
    // Verify that exactly 8 bytes were appended
    D_ASSERT(out.size() == orig_size + 8);
}

uint32_t ReadU32BE(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

uint64_t ReadU64BE(const uint8_t* p) {
    uint64_t val = 0;
    for (int i = 0; i < 8; ++i) {
        val = (val << 8) | static_cast<uint64_t>(p[i]);
    }
    return val;
}

void WriteStringBE(std::vector<uint8_t>& out, std::string_view str) {
    WriteU32BE(out, static_cast<uint32_t>(str.size()));
    out.insert(out.end(), str.begin(), str.end());
}

Result<std::string> ReadStringBE(std::span<const uint8_t>& in) {
    if (in.size() < 4) {
        return MakeError(ErrorCode::ProtocolError, "Truncated string length prefix");
    }
    uint32_t len = ReadU32BE(in.data());
    in = in.subspan(4);
    if (len > in.size()) {
        return MakeError(ErrorCode::ProtocolError, "String length exceeds remaining payload");
    }
    std::string s(reinterpret_cast<const char*>(in.data()), len);
    in = in.subspan(len);
    return s;
}

} // namespace

// ============================================================================
// Opcode::WhiskPublish Codecs
// ============================================================================

Result<void> EncodeWhiskPublishRequest(std::vector<uint8_t>& out, const WhiskPublishRequest& req) {
    if (req.module_name.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "module_name cannot be empty");
    }
    WriteStringBE(out, req.module_name);
    WriteU32BE(out, static_cast<uint32_t>(req.wasm_bytes.size()));
    out.insert(out.end(), req.wasm_bytes.begin(), req.wasm_bytes.end());
    return {};
}

Result<WhiskPublishRequest> DecodeWhiskPublishRequest(std::span<const uint8_t> in) {
    auto name_res = ReadStringBE(in);
    if (!name_res) {
        return MakeError(name_res.error().Code(), name_res.error().Message());
    }
    if (name_res->empty()) {
        return MakeError(ErrorCode::ProtocolError, "module_name cannot be empty");
    }

    if (in.size() < 4) {
        return MakeError(ErrorCode::ProtocolError, "Truncated wasm_bytes length prefix");
    }
    uint32_t wasm_len = ReadU32BE(in.data());
    in = in.subspan(4);

    if (wasm_len > in.size()) {
        return MakeError(ErrorCode::ProtocolError, "wasm_bytes length exceeds remaining payload");
    }

    std::vector<uint8_t> wasm_bytes(in.data(), in.data() + wasm_len);
    in = in.subspan(wasm_len);

    if (!in.empty()) {
        return MakeError(ErrorCode::ProtocolError, "Trailing extra bytes in WhiskPublishRequest");
    }

    return WhiskPublishRequest{
        .module_name = std::move(*name_res),
        .wasm_bytes = std::move(wasm_bytes)
    };
}

void EncodeWhiskPublishResponse(std::vector<uint8_t>& out, const WhiskPublishResponse& resp) {
    WriteStringBE(out, resp.module_name);
    WriteU64BE(out, std::bit_cast<uint64_t>(resp.version));
    WriteStringBE(out, resp.sha256);
    WriteU32BE(out, static_cast<uint32_t>(resp.exported_funcs.size()));
    for (const auto& fn : resp.exported_funcs) {
        WriteStringBE(out, fn);
    }
}

Result<WhiskPublishResponse> DecodeWhiskPublishResponse(std::span<const uint8_t> in) {
    auto name_res = ReadStringBE(in);
    if (!name_res) {
        return MakeError(name_res.error().Code(), name_res.error().Message());
    }

    if (in.size() < 8) {
        return MakeError(ErrorCode::ProtocolError, "Truncated version in WhiskPublishResponse");
    }
    uint64_t raw_ver = ReadU64BE(in.data());
    in = in.subspan(8);
    int64_t version = std::bit_cast<int64_t>(raw_ver);

    auto sha_res = ReadStringBE(in);
    if (!sha_res) {
        return MakeError(sha_res.error().Code(), sha_res.error().Message());
    }

    if (in.size() < 4) {
        return MakeError(ErrorCode::ProtocolError, "Truncated exported_funcs count");
    }
    uint32_t fn_count = ReadU32BE(in.data());
    in = in.subspan(4);

    // Each function string requires at least 4 bytes of length header
    if (static_cast<uint64_t>(fn_count) * 4 > in.size()) {
        return MakeError(ErrorCode::ProtocolError, "exported_funcs count exceeds remaining payload");
    }

    std::vector<std::string> exported_funcs;
    exported_funcs.reserve(std::min<size_t>(fn_count, 1024));

    for (uint32_t i = 0; i < fn_count; ++i) {
        auto fn_res = ReadStringBE(in);
        if (!fn_res) {
            return MakeError(fn_res.error().Code(), fn_res.error().Message());
        }
        exported_funcs.push_back(std::move(*fn_res));
    }

    if (!in.empty()) {
        return MakeError(ErrorCode::ProtocolError, "Trailing extra bytes in WhiskPublishResponse");
    }

    return WhiskPublishResponse{
        .module_name = std::move(*name_res),
        .version = version,
        .sha256 = std::move(*sha_res),
        .exported_funcs = std::move(exported_funcs)
    };
}

// ============================================================================
// Opcode::WhiskCall Codecs
// ============================================================================

Result<void> EncodeWhiskCallRequest(std::vector<uint8_t>& out, const WhiskCallRequest& req) {
    if (req.module_name.empty() || req.func_name.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "module_name and func_name cannot be empty");
    }
    WriteStringBE(out, req.module_name);
    WriteStringBE(out, req.func_name);
    WriteStringBE(out, req.args_json);
    return {};
}

Result<WhiskCallRequest> DecodeWhiskCallRequest(std::span<const uint8_t> in) {
    auto mod_res = ReadStringBE(in);
    if (!mod_res) {
        return MakeError(mod_res.error().Code(), mod_res.error().Message());
    }
    auto fn_res = ReadStringBE(in);
    if (!fn_res) {
        return MakeError(fn_res.error().Code(), fn_res.error().Message());
    }
    auto args_res = ReadStringBE(in);
    if (!args_res) {
        return MakeError(args_res.error().Code(), args_res.error().Message());
    }

    if (mod_res->empty() || fn_res->empty()) {
        return MakeError(ErrorCode::ProtocolError, "module_name and func_name cannot be empty");
    }

    if (!in.empty()) {
        return MakeError(ErrorCode::ProtocolError, "Trailing extra bytes in WhiskCallRequest");
    }

    return WhiskCallRequest{
        .module_name = std::move(*mod_res),
        .func_name = std::move(*fn_res),
        .args_json = std::move(*args_res)
    };
}

void EncodeWhiskCallResponse(std::vector<uint8_t>& out, const WhiskCallResponse& resp) {
    WriteStringBE(out, resp.result_json);
    WriteU64BE(out, resp.fuel_used);
}

Result<WhiskCallResponse> DecodeWhiskCallResponse(std::span<const uint8_t> in) {
    auto res_json = ReadStringBE(in);
    if (!res_json) {
        return MakeError(res_json.error().Code(), res_json.error().Message());
    }

    if (in.size() < 8) {
        return MakeError(ErrorCode::ProtocolError, "Truncated fuel_used in WhiskCallResponse");
    }
    uint64_t fuel = ReadU64BE(in.data());
    in = in.subspan(8);

    if (!in.empty()) {
        return MakeError(ErrorCode::ProtocolError, "Trailing extra bytes in WhiskCallResponse");
    }

    return WhiskCallResponse{
        .result_json = std::move(*res_json),
        .fuel_used = fuel
    };
}

// ============================================================================
// Opcode::WhiskList Codecs
// ============================================================================

void EncodeWhiskListResponse(std::vector<uint8_t>& out, const WhiskListResponse& resp) {
    WriteU32BE(out, static_cast<uint32_t>(resp.modules.size()));
    for (const auto& mod : resp.modules) {
        WriteStringBE(out, mod.name);
        WriteU64BE(out, std::bit_cast<uint64_t>(mod.version));
        WriteStringBE(out, mod.sha256);
        WriteU32BE(out, static_cast<uint32_t>(mod.exported_funcs.size()));
        for (const auto& fn : mod.exported_funcs) {
            WriteStringBE(out, fn);
        }
        WriteU64BE(out, std::bit_cast<uint64_t>(mod.updated_at));
    }
}

Result<WhiskListResponse> DecodeWhiskListResponse(std::span<const uint8_t> in) {
    if (in.size() < 4) {
        return MakeError(ErrorCode::ProtocolError, "Truncated WhiskListResponse count");
    }
    uint32_t count = ReadU32BE(in.data());
    in = in.subspan(4);

    // Minimum wire size of one summary:
    // name_len(4) + ver(8) + sha_len(4) + fn_count(4) + updated_at(8) = 28 bytes
    constexpr uint64_t MIN_MODULE_WIRE_SIZE = 28;
    if (static_cast<uint64_t>(count) * MIN_MODULE_WIRE_SIZE > in.size()) {
        return MakeError(ErrorCode::ProtocolError, "modules count exceeds remaining payload");
    }

    std::vector<WhiskModuleSummary> modules;
    modules.reserve(std::min<size_t>(count, 1024));

    for (uint32_t i = 0; i < count; ++i) {
        auto name_res = ReadStringBE(in);
        if (!name_res) {
            return MakeError(name_res.error().Code(), name_res.error().Message());
        }

        if (in.size() < 8) {
            return MakeError(ErrorCode::ProtocolError, "Truncated version in WhiskModuleSummary");
        }
        uint64_t raw_ver = ReadU64BE(in.data());
        in = in.subspan(8);
        int64_t version = std::bit_cast<int64_t>(raw_ver);

        auto sha_res = ReadStringBE(in);
        if (!sha_res) {
            return MakeError(sha_res.error().Code(), sha_res.error().Message());
        }

        if (in.size() < 4) {
            return MakeError(ErrorCode::ProtocolError, "Truncated fn_count in WhiskModuleSummary");
        }
        uint32_t fn_count = ReadU32BE(in.data());
        in = in.subspan(4);

        if (static_cast<uint64_t>(fn_count) * 4 > in.size()) {
            return MakeError(ErrorCode::ProtocolError, "fn_count exceeds remaining payload");
        }

        std::vector<std::string> exported_funcs;
        exported_funcs.reserve(std::min<size_t>(fn_count, 1024));

        for (uint32_t j = 0; j < fn_count; ++j) {
            auto fn_res = ReadStringBE(in);
            if (!fn_res) {
                return MakeError(fn_res.error().Code(), fn_res.error().Message());
            }
            exported_funcs.push_back(std::move(*fn_res));
        }

        if (in.size() < 8) {
            return MakeError(ErrorCode::ProtocolError, "Truncated updated_at in WhiskModuleSummary");
        }
        uint64_t raw_updated = ReadU64BE(in.data());
        in = in.subspan(8);
        int64_t updated_at = std::bit_cast<int64_t>(raw_updated);

        modules.push_back(WhiskModuleSummary{
            .name = std::move(*name_res),
            .version = version,
            .sha256 = std::move(*sha_res),
            .exported_funcs = std::move(exported_funcs),
            .updated_at = updated_at
        });
    }

    if (!in.empty()) {
        return MakeError(ErrorCode::ProtocolError, "Trailing extra bytes in WhiskListResponse");
    }

    return WhiskListResponse{
        .modules = std::move(modules)
    };
}

// ============================================================================
// Opcode::Logs Codec
// ============================================================================

void EncodeLogsRequest(std::vector<uint8_t>& out, const LogsRequest& req) {
    uint32_t clamped = std::clamp(req.limit, LogsRequest::MIN_LIMIT, LogsRequest::MAX_LIMIT);
    WriteU32BE(out, clamped);
}

Result<LogsRequest> DecodeLogsRequest(std::span<const uint8_t> in) {
    if (in.empty()) {
        return LogsRequest{.limit = LogsRequest::DEFAULT_LIMIT};
    }
    if (in.size() == 4) {
        uint32_t raw_limit = ReadU32BE(in.data());
        uint32_t clamped = std::clamp(raw_limit, LogsRequest::MIN_LIMIT, LogsRequest::MAX_LIMIT);
        return LogsRequest{.limit = clamped};
    }
    return MakeError(ErrorCode::ProtocolError, "Invalid LogsRequest payload length (expected 0 or 4 bytes)");
}

} // namespace database
