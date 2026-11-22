#include "network/whisk_codec.hpp"

#include <cassert>
#include <print>
#include <vector>

using namespace database;

void test_whisk_publish_request() {
    std::println("Running test_whisk_publish_request...");

    // 1. Valid round-trip: normal
    {
        WhiskPublishRequest req{
            .module_name = "math_v1",
            .wasm_bytes = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00}
        };

        std::vector<uint8_t> buf;
        auto enc_res = EncodeWhiskPublishRequest(buf, req);
        assert(enc_res.has_value());

        auto dec_res = DecodeWhiskPublishRequest(buf);
        assert(dec_res.has_value());
        assert(dec_res->module_name == req.module_name);
        assert(dec_res->wasm_bytes == req.wasm_bytes);
        assert(*dec_res == req);
    }

    // 2. Valid round-trip: empty wasm_bytes
    {
        WhiskPublishRequest req{
            .module_name = "empty_wasm",
            .wasm_bytes = {}
        };

        std::vector<uint8_t> buf;
        auto enc_res = EncodeWhiskPublishRequest(buf, req);
        assert(enc_res.has_value());

        auto dec_res = DecodeWhiskPublishRequest(buf);
        assert(dec_res.has_value());
        assert(*dec_res == req);
    }

    // 3. Valid round-trip: large wasm_bytes (256 KiB)
    {
        std::vector<uint8_t> large_wasm(256 * 1024);
        for (size_t i = 0; i < large_wasm.size(); ++i) {
            large_wasm[i] = static_cast<uint8_t>((i * 31 + 7) & 0xFF);
        }

        WhiskPublishRequest req{
            .module_name = "large_module",
            .wasm_bytes = std::move(large_wasm)
        };

        std::vector<uint8_t> buf;
        auto enc_res = EncodeWhiskPublishRequest(buf, req);
        assert(enc_res.has_value());

        auto dec_res = DecodeWhiskPublishRequest(buf);
        assert(dec_res.has_value());
        assert(*dec_res == req);
    }

    // 4. Rejection of empty module_name in encode (ErrorCode::InvalidArgument)
    {
        WhiskPublishRequest req{
            .module_name = "",
            .wasm_bytes = {0x01, 0x02}
        };

        std::vector<uint8_t> buf;
        auto enc_res = EncodeWhiskPublishRequest(buf, req);
        assert(!enc_res.has_value());
        assert(enc_res.error().Code() == ErrorCode::InvalidArgument);
    }

    // 5. Rejection of empty module_name in decode (ErrorCode::ProtocolError)
    {
        // Wire: [u32 BE name_len = 0][u32 BE wasm_len = 0]
        std::vector<uint8_t> raw = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        auto dec_res = DecodeWhiskPublishRequest(raw);
        assert(!dec_res.has_value());
        assert(dec_res.error().Code() == ErrorCode::ProtocolError);
    }

    // 6. Hostile/boundary tests: every prefix truncation
    {
        WhiskPublishRequest req{
            .module_name = "mod",
            .wasm_bytes = {0xAA, 0xBB}
        };
        std::vector<uint8_t> buf;
        auto enc_res = EncodeWhiskPublishRequest(buf, req);
        assert(enc_res.has_value());

        for (size_t len = 0; len < buf.size(); ++len) {
            std::span<const uint8_t> trunc(buf.data(), len);
            auto res = DecodeWhiskPublishRequest(trunc);
            assert(!res.has_value());
            assert(res.error().Code() == ErrorCode::ProtocolError);
        }
    }

    // 7. Hostile test: trailing extra byte
    {
        WhiskPublishRequest req{
            .module_name = "mod",
            .wasm_bytes = {0xAA, 0xBB}
        };
        std::vector<uint8_t> buf;
        auto enc_res = EncodeWhiskPublishRequest(buf, req);
        assert(enc_res.has_value());

        buf.push_back(0xFF); // Trailing garbage
        auto res = DecodeWhiskPublishRequest(buf);
        assert(!res.has_value());
        assert(res.error().Code() == ErrorCode::ProtocolError);
    }

    // 8. Hostile test: absurd 0xFFFFFFFF lengths
    {
        // Absurd name length
        std::vector<uint8_t> forged_name = {0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x02};
        auto res1 = DecodeWhiskPublishRequest(forged_name);
        assert(!res1.has_value());
        assert(res1.error().Code() == ErrorCode::ProtocolError);

        // Absurd wasm length
        std::vector<uint8_t> forged_wasm = {
            0x00, 0x00, 0x00, 0x03, 'm', 'o', 'd',
            0xFF, 0xFF, 0xFF, 0xFF
        };
        auto res2 = DecodeWhiskPublishRequest(forged_wasm);
        assert(!res2.has_value());
        assert(res2.error().Code() == ErrorCode::ProtocolError);
    }
}

void test_whisk_publish_response() {
    std::println("Running test_whisk_publish_response...");

    // 1. Valid round-trip: normal
    {
        WhiskPublishResponse resp{
            .module_name = "auth_v2",
            .version = 1042,
            .sha256 = "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9",
            .exported_funcs = {"init", "login", "register", "logout"}
        };

        std::vector<uint8_t> buf;
        EncodeWhiskPublishResponse(buf, resp);

        auto dec_res = DecodeWhiskPublishResponse(buf);
        assert(dec_res.has_value());
        assert(*dec_res == resp);
    }

    // 2. Valid round-trip: empty exported_funcs
    {
        WhiskPublishResponse resp{
            .module_name = "empty_fns",
            .version = -1,
            .sha256 = "",
            .exported_funcs = {}
        };

        std::vector<uint8_t> buf;
        EncodeWhiskPublishResponse(buf, resp);

        auto dec_res = DecodeWhiskPublishResponse(buf);
        assert(dec_res.has_value());
        assert(*dec_res == resp);
    }

    // 3. Hostile/boundary tests: every prefix truncation
    {
        WhiskPublishResponse resp{
            .module_name = "test_mod",
            .version = 42,
            .sha256 = "abc",
            .exported_funcs = {"f1", "f2"}
        };
        std::vector<uint8_t> buf;
        EncodeWhiskPublishResponse(buf, resp);

        for (size_t len = 0; len < buf.size(); ++len) {
            std::span<const uint8_t> trunc(buf.data(), len);
            auto res = DecodeWhiskPublishResponse(trunc);
            assert(!res.has_value());
            assert(res.error().Code() == ErrorCode::ProtocolError);
        }
    }

    // 4. Hostile test: trailing extra byte
    {
        WhiskPublishResponse resp{
            .module_name = "mod",
            .version = 1,
            .sha256 = "s",
            .exported_funcs = {}
        };
        std::vector<uint8_t> buf;
        EncodeWhiskPublishResponse(buf, resp);
        buf.push_back(0x00);

        auto res = DecodeWhiskPublishResponse(buf);
        assert(!res.has_value());
        assert(res.error().Code() == ErrorCode::ProtocolError);
    }

    // 5. Hostile test: absurd 0xFFFFFFFF lengths and counts
    {
        // Absurd sha length
        std::vector<uint8_t> forged_sha = {
            0x00, 0x00, 0x00, 0x01, 'm',
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
            0xFF, 0xFF, 0xFF, 0xFF
        };
        auto res1 = DecodeWhiskPublishResponse(forged_sha);
        assert(!res1.has_value());
        assert(res1.error().Code() == ErrorCode::ProtocolError);

        // Absurd fn_count
        std::vector<uint8_t> forged_count = {
            0x00, 0x00, 0x00, 0x01, 'm',
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x00,
            0xFF, 0xFF, 0xFF, 0xFF
        };
        auto res2 = DecodeWhiskPublishResponse(forged_count);
        assert(!res2.has_value());
        assert(res2.error().Code() == ErrorCode::ProtocolError);
    }
}

void test_whisk_call_request() {
    std::println("Running test_whisk_call_request...");

    // 1. Valid round-trip: empty args_json
    {
        WhiskCallRequest req{
            .module_name = "analytics",
            .func_name = "flush",
            .args_json = ""
        };

        std::vector<uint8_t> buf;
        auto enc_res = EncodeWhiskCallRequest(buf, req);
        assert(enc_res.has_value());

        auto dec_res = DecodeWhiskCallRequest(buf);
        assert(dec_res.has_value());
        assert(*dec_res == req);
    }

    // 2. Valid round-trip: JSON args_json
    {
        WhiskCallRequest req{
            .module_name = "orders",
            .func_name = "place_order",
            .args_json = R"({"user_id": 42, "items": [{"id": 1, "qty": 2}], "coupon": null})"
        };

        std::vector<uint8_t> buf;
        auto enc_res = EncodeWhiskCallRequest(buf, req);
        assert(enc_res.has_value());

        auto dec_res = DecodeWhiskCallRequest(buf);
        assert(dec_res.has_value());
        assert(*dec_res == req);
    }

    // 3. Rejection of empty module_name or func_name in encode (ErrorCode::InvalidArgument)
    {
        WhiskCallRequest req_empty_mod{
            .module_name = "",
            .func_name = "fn",
            .args_json = "{}"
        };
        std::vector<uint8_t> buf1;
        auto res1 = EncodeWhiskCallRequest(buf1, req_empty_mod);
        assert(!res1.has_value());
        assert(res1.error().Code() == ErrorCode::InvalidArgument);

        WhiskCallRequest req_empty_fn{
            .module_name = "mod",
            .func_name = "",
            .args_json = "{}"
        };
        std::vector<uint8_t> buf2;
        auto res2 = EncodeWhiskCallRequest(buf2, req_empty_fn);
        assert(!res2.has_value());
        assert(res2.error().Code() == ErrorCode::InvalidArgument);

        WhiskCallRequest req_both_empty{
            .module_name = "",
            .func_name = "",
            .args_json = ""
        };
        std::vector<uint8_t> buf3;
        auto res3 = EncodeWhiskCallRequest(buf3, req_both_empty);
        assert(!res3.has_value());
        assert(res3.error().Code() == ErrorCode::InvalidArgument);
    }

    // 4. Rejection of empty module_name or func_name in decode (ErrorCode::ProtocolError)
    {
        // mod_len = 0, fn_len = 2 ("fn"), args_len = 0
        std::vector<uint8_t> raw1 = {
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x02, 'f', 'n',
            0x00, 0x00, 0x00, 0x00
        };
        auto res1 = DecodeWhiskCallRequest(raw1);
        assert(!res1.has_value());
        assert(res1.error().Code() == ErrorCode::ProtocolError);

        // mod_len = 3 ("mod"), fn_len = 0, args_len = 0
        std::vector<uint8_t> raw2 = {
            0x00, 0x00, 0x00, 0x03, 'm', 'o', 'd',
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00
        };
        auto res2 = DecodeWhiskCallRequest(raw2);
        assert(!res2.has_value());
        assert(res2.error().Code() == ErrorCode::ProtocolError);
    }

    // 5. Hostile/boundary tests: every prefix truncation
    {
        WhiskCallRequest req{
            .module_name = "mod",
            .func_name = "fn",
            .args_json = "123"
        };
        std::vector<uint8_t> buf;
        auto enc_res = EncodeWhiskCallRequest(buf, req);
        assert(enc_res.has_value());

        for (size_t len = 0; len < buf.size(); ++len) {
            std::span<const uint8_t> trunc(buf.data(), len);
            auto res = DecodeWhiskCallRequest(trunc);
            assert(!res.has_value());
            assert(res.error().Code() == ErrorCode::ProtocolError);
        }
    }

    // 6. Hostile test: trailing extra byte
    {
        WhiskCallRequest req{
            .module_name = "mod",
            .func_name = "fn",
            .args_json = "123"
        };
        std::vector<uint8_t> buf;
        auto enc_res = EncodeWhiskCallRequest(buf, req);
        assert(enc_res.has_value());

        buf.push_back(0xCC);
        auto res = DecodeWhiskCallRequest(buf);
        assert(!res.has_value());
        assert(res.error().Code() == ErrorCode::ProtocolError);
    }

    // 7. Hostile test: absurd 0xFFFFFFFF lengths
    {
        std::vector<uint8_t> forged_args = {
            0x00, 0x00, 0x00, 0x01, 'm',
            0x00, 0x00, 0x00, 0x01, 'f',
            0xFF, 0xFF, 0xFF, 0xFF
        };
        auto res = DecodeWhiskCallRequest(forged_args);
        assert(!res.has_value());
        assert(res.error().Code() == ErrorCode::ProtocolError);
    }
}

void test_whisk_call_response() {
    std::println("Running test_whisk_call_response...");

    // 1. Valid round-trip: normal
    {
        WhiskCallResponse resp{
            .result_json = R"({"success": true, "new_balance": 1500})",
            .fuel_used = 125000ULL
        };

        std::vector<uint8_t> buf;
        EncodeWhiskCallResponse(buf, resp);

        auto dec_res = DecodeWhiskCallResponse(buf);
        assert(dec_res.has_value());
        assert(*dec_res == resp);
    }

    // 2. Valid round-trip: empty result_json and zero fuel
    {
        WhiskCallResponse resp{
            .result_json = "",
            .fuel_used = 0
        };

        std::vector<uint8_t> buf;
        EncodeWhiskCallResponse(buf, resp);

        auto dec_res = DecodeWhiskCallResponse(buf);
        assert(dec_res.has_value());
        assert(*dec_res == resp);
    }

    // 3. Hostile/boundary tests: every prefix truncation
    {
        WhiskCallResponse resp{
            .result_json = "{\"ok\":1}",
            .fuel_used = 999
        };
        std::vector<uint8_t> buf;
        EncodeWhiskCallResponse(buf, resp);

        for (size_t len = 0; len < buf.size(); ++len) {
            std::span<const uint8_t> trunc(buf.data(), len);
            auto res = DecodeWhiskCallResponse(trunc);
            assert(!res.has_value());
            assert(res.error().Code() == ErrorCode::ProtocolError);
        }
    }

    // 4. Hostile test: trailing extra byte
    {
        WhiskCallResponse resp{
            .result_json = "{}",
            .fuel_used = 1
        };
        std::vector<uint8_t> buf;
        EncodeWhiskCallResponse(buf, resp);
        buf.push_back(0x42);

        auto res = DecodeWhiskCallResponse(buf);
        assert(!res.has_value());
        assert(res.error().Code() == ErrorCode::ProtocolError);
    }

    // 5. Hostile test: absurd 0xFFFFFFFF result_json length
    {
        std::vector<uint8_t> forged = {0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x02};
        auto res = DecodeWhiskCallResponse(forged);
        assert(!res.has_value());
        assert(res.error().Code() == ErrorCode::ProtocolError);
    }
}

void test_whisk_list_response() {
    std::println("Running test_whisk_list_response...");

    // 1. Valid round-trip: empty modules
    {
        WhiskListResponse resp{
            .modules = {}
        };

        std::vector<uint8_t> buf;
        EncodeWhiskListResponse(buf, resp);

        auto dec_res = DecodeWhiskListResponse(buf);
        assert(dec_res.has_value());
        assert(*dec_res == resp);
        assert(dec_res->modules.empty());
    }

    // 2. Valid round-trip: single module
    {
        WhiskListResponse resp{
            .modules = {
                WhiskModuleSummary{
                    .name = "single_mod",
                    .version = 1,
                    .sha256 = "abc123sha",
                    .exported_funcs = {"run"},
                    .updated_at = 1711200000000000LL
                }
            }
        };

        std::vector<uint8_t> buf;
        EncodeWhiskListResponse(buf, resp);

        auto dec_res = DecodeWhiskListResponse(buf);
        assert(dec_res.has_value());
        assert(*dec_res == resp);
    }

    // 3. Valid round-trip: multi-entry modules
    {
        WhiskListResponse resp{
            .modules = {
                WhiskModuleSummary{
                    .name = "mod_alpha",
                    .version = 10,
                    .sha256 = "sha_alpha",
                    .exported_funcs = {"f1", "f2", "f3"},
                    .updated_at = 1000LL
                },
                WhiskModuleSummary{
                    .name = "mod_beta",
                    .version = 20,
                    .sha256 = "sha_beta",
                    .exported_funcs = {},
                    .updated_at = 2000LL
                },
                WhiskModuleSummary{
                    .name = "mod_gamma",
                    .version = -5,
                    .sha256 = "sha_gamma",
                    .exported_funcs = {"init"},
                    .updated_at = 3000LL
                }
            }
        };

        std::vector<uint8_t> buf;
        EncodeWhiskListResponse(buf, resp);

        auto dec_res = DecodeWhiskListResponse(buf);
        assert(dec_res.has_value());
        assert(*dec_res == resp);
    }

    // 4. Hostile/boundary tests: every prefix truncation
    {
        WhiskListResponse resp{
            .modules = {
                WhiskModuleSummary{
                    .name = "m",
                    .version = 1,
                    .sha256 = "s",
                    .exported_funcs = {"f"},
                    .updated_at = 100
                }
            }
        };
        std::vector<uint8_t> buf;
        EncodeWhiskListResponse(buf, resp);

        for (size_t len = 0; len < buf.size(); ++len) {
            std::span<const uint8_t> trunc(buf.data(), len);
            auto res = DecodeWhiskListResponse(trunc);
            assert(!res.has_value());
            assert(res.error().Code() == ErrorCode::ProtocolError);
        }
    }

    // 5. Hostile test: trailing extra byte
    {
        WhiskListResponse resp{
            .modules = {}
        };
        std::vector<uint8_t> buf;
        EncodeWhiskListResponse(buf, resp);
        buf.push_back(0xDE);

        auto res = DecodeWhiskListResponse(buf);
        assert(!res.has_value());
        assert(res.error().Code() == ErrorCode::ProtocolError);
    }

    // 6. Hostile test: absurd 0xFFFFFFFF counts and lengths
    {
        // Absurd module count
        std::vector<uint8_t> forged_count = {0xFF, 0xFF, 0xFF, 0xFF};
        auto res1 = DecodeWhiskListResponse(forged_count);
        assert(!res1.has_value());
        assert(res1.error().Code() == ErrorCode::ProtocolError);

        // Absurd fn_count inside summary
        std::vector<uint8_t> forged_fn_count = {
            0x00, 0x00, 0x00, 0x01, // count = 1
            0x00, 0x00, 0x00, 0x01, 'm', // name
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, // ver
            0x00, 0x00, 0x00, 0x01, 's', // sha
            0xFF, 0xFF, 0xFF, 0xFF // absurd fn_count
        };
        auto res2 = DecodeWhiskListResponse(forged_fn_count);
        assert(!res2.has_value());
        assert(res2.error().Code() == ErrorCode::ProtocolError);
    }
}

void test_logs_request() {
    std::println("Running test_logs_request...");

    // 1. Empty buffer (0 bytes) -> DEFAULT_LIMIT (200)
    {
        std::vector<uint8_t> empty_buf;
        auto res = DecodeLogsRequest(empty_buf);
        assert(res.has_value());
        assert(res->limit == LogsRequest::DEFAULT_LIMIT);
        assert(res->limit == 200);
    }

    // 2. Encode and decode roundtrips with clamping
    // Limit = 0 (clamped to 1)
    {
        LogsRequest req{.limit = 0};
        std::vector<uint8_t> buf;
        EncodeLogsRequest(buf, req);
        assert(buf.size() == 4);

        auto dec = DecodeLogsRequest(buf);
        assert(dec.has_value());
        assert(dec->limit == 1);
    }

    // Limit = 1 (boundary min)
    {
        LogsRequest req{.limit = 1};
        std::vector<uint8_t> buf;
        EncodeLogsRequest(buf, req);

        auto dec = DecodeLogsRequest(buf);
        assert(dec.has_value());
        assert(dec->limit == 1);
    }

    // Limit = 500 (normal)
    {
        LogsRequest req{.limit = 500};
        std::vector<uint8_t> buf;
        EncodeLogsRequest(buf, req);

        auto dec = DecodeLogsRequest(buf);
        assert(dec.has_value());
        assert(dec->limit == 500);
    }

    // Limit = 999999 (clamped to 10000)
    {
        LogsRequest req{.limit = 999999};
        std::vector<uint8_t> buf;
        EncodeLogsRequest(buf, req);

        auto dec = DecodeLogsRequest(buf);
        assert(dec.has_value());
        assert(dec->limit == 10000);
    }

    // 3. Raw decode clamping directly from hostile/forged wire frames
    {
        // Raw 0 -> clamped to 1
        std::vector<uint8_t> raw_zero = {0x00, 0x00, 0x00, 0x00};
        auto res_zero = DecodeLogsRequest(raw_zero);
        assert(res_zero.has_value());
        assert(res_zero->limit == 1);

        // Raw 0xFFFFFFFF -> clamped to 10000
        std::vector<uint8_t> raw_max = {0xFF, 0xFF, 0xFF, 0xFF};
        auto res_max = DecodeLogsRequest(raw_max);
        assert(res_max.has_value());
        assert(res_max->limit == 10000);
    }

    // 4. Invalid payload lengths (must reject anything != 0 and != 4)
    const std::vector<std::vector<uint8_t>> invalid_payloads = {
        {0x01},                         // 1 byte
        {0x01, 0x02},                   // 2 bytes
        {0x01, 0x02, 0x03},             // 3 bytes
        {0x00, 0x00, 0x00, 0x64, 0xAA}, // 5 bytes
        {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08} // 8 bytes
    };

    for (const auto& payload : invalid_payloads) {
        auto res = DecodeLogsRequest(payload);
        assert(!res.has_value());
        assert(res.error().Code() == ErrorCode::ProtocolError);
    }
}

int main() {
    std::println("=== Running Whisk Codec Tests ===");
    test_whisk_publish_request();
    test_whisk_publish_response();
    test_whisk_call_request();
    test_whisk_call_response();
    test_whisk_list_response();
    test_logs_request();
    std::println("=== All Whisk Codec Tests Passed! ===");
    return 0;
}
