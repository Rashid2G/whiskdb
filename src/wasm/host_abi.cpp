#include "wasm/host_abi.hpp"

#include "logger.hpp"
#include "sql/transaction_guard.hpp"
#include "wasm/wasm_runtime.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace database {

namespace {

/**
 * @brief Checks if a [ptr, ptr + len) range is within guest linear memory.
 */
bool CheckBounds(int32_t ptr, int32_t len, size_t mem_size) noexcept {
    if (ptr < 0 || len < 0) {
        return false;
    }
    // Perform 64-bit addition to prevent int32 overflow wrap-around
    return (static_cast<uint64_t>(ptr) + static_cast<uint64_t>(len)) <= mem_size;
}

struct MemoryView {
    uint8_t* data = nullptr;
    size_t size = 0;
};

/**
 * @brief Looks up the exported guest linear memory named "memory".
 */
std::optional<MemoryView> GetCallerMemory(wasmtime_caller_t* caller, wasmtime_context_t* store_ctx) noexcept {
    // Caller and store context must be non-null
    D_ASSERT(caller != nullptr);
    D_ASSERT(store_ctx != nullptr);

    wasmtime_extern_t mem_ext{};
    if (!wasmtime_caller_export_get(caller, "memory", 6, &mem_ext)) {
        return std::nullopt;
    }
    if (mem_ext.kind != WASMTIME_EXTERN_MEMORY) {
        return std::nullopt;
    }
    uint8_t* data = wasmtime_memory_data(store_ctx, &mem_ext.of.memory);
    size_t size = wasmtime_memory_data_size(store_ctx, &mem_ext.of.memory);
    return MemoryView{data, size};
}

/**
 * @brief Skips whitespace, SQL line comments (-- ...), and block comments (/ * ... * /).
 */
std::string_view SkipSqlWhitespaceAndComments(std::string_view sql) noexcept {
    size_t i = 0;
    while (i < sql.size()) {
        if (std::isspace(static_cast<unsigned char>(sql[i]))) {
            ++i;
            continue;
        }
        if (i + 1 < sql.size() && sql[i] == '-' && sql[i + 1] == '-') {
            i += 2;
            while (i < sql.size() && sql[i] != '\n') {
                ++i;
            }
            continue;
        }
        if (i + 1 < sql.size() && sql[i] == '/' && sql[i + 1] == '*') {
            i += 2;
            while (i + 1 < sql.size() && !(sql[i] == '*' && sql[i + 1] == '/')) {
                ++i;
            }
            if (i + 1 < sql.size()) {
                i += 2;
            } else {
                i = sql.size();
            }
            continue;
        }
        break;
    }
    return sql.substr(i);
}

/**
 * @brief Extracts the first uppercase alphanumeric/underscore SQL keyword.
 */
std::string ExtractFirstKeyword(std::string_view sql) {
    std::string_view trimmed = SkipSqlWhitespaceAndComments(sql);
    size_t len = 0;
    while (len < trimmed.size() &&
           (std::isalnum(static_cast<unsigned char>(trimmed[len])) || trimmed[len] == '_')) {
        ++len;
    }
    std::string kw;
    kw.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        kw.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(trimmed[i]))));
    }
    return kw;
}

/**
 * @brief Checks if a statement keyword is a forbidden transaction control statement.
 */
bool IsForbiddenTransactionControl(std::string_view first_keyword) noexcept {
    return (first_keyword == "BEGIN" ||
            first_keyword == "COMMIT" ||
            first_keyword == "END" ||
            first_keyword == "ROLLBACK" ||
            first_keyword == "SAVEPOINT" ||
            first_keyword == "RELEASE");
}

void AppendJsonEscapedString(std::string& out, std::string_view s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    out.push_back('"');
}

void AppendJsonValue(std::string& out, const Value& val) {
    std::visit(
        [&out](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                out += "null";
            } else if constexpr (std::is_same_v<T, int64_t>) {
                out += std::to_string(v);
            } else if constexpr (std::is_same_v<T, double>) {
                if (std::isfinite(v)) {
                    out += std::format("{}", v);
                } else {
                    out += "null";
                }
            } else if constexpr (std::is_same_v<T, std::string>) {
                AppendJsonEscapedString(out, v);
            } else if constexpr (std::is_same_v<T, Blob>) {
                out.push_back('"');
                for (uint8_t b : v) {
                    out += std::format("{:02x}", b);
                }
                out.push_back('"');
            }
        },
        val);
}

wasm_trap_t* WhiskDbExecCallback(
    void* /*env*/,
    wasmtime_caller_t* caller,
    const wasmtime_val_t* args,
    size_t /*nargs*/,
    wasmtime_val_t* results,
    size_t /*nresults*/
) {
    wasmtime_context_t* store_ctx = wasmtime_caller_context(caller);
    auto* ctx = static_cast<WhiskCallContext*>(wasmtime_context_get_data(store_ctx));
    results[0].kind = WASMTIME_I64;

    if (ctx == nullptr || ctx->sql == nullptr) {
        results[0].of.i64 = -static_cast<int64_t>(ErrorCode::Internal);
        return nullptr;
    }

    auto mem_opt = GetCallerMemory(caller, store_ctx);
    if (!mem_opt) {
        ctx->last_error = Error(ErrorCode::WasmMemoryOutOfBounds, "Memory export 'memory' not found");
        results[0].of.i64 = -static_cast<int64_t>(ErrorCode::WasmMemoryOutOfBounds);
        return nullptr;
    }

    int32_t sql_ptr = args[0].of.i32;
    int32_t sql_len = args[1].of.i32;
    int32_t params_ptr = args[2].of.i32;
    int32_t params_len = args[3].of.i32;

    if (!CheckBounds(sql_ptr, sql_len, mem_opt->size) ||
        !CheckBounds(params_ptr, params_len, mem_opt->size)) {
        ctx->last_error = Error(ErrorCode::WasmMemoryOutOfBounds, "Pointer out of guest memory bounds");
        results[0].of.i64 = -static_cast<int64_t>(ErrorCode::WasmMemoryOutOfBounds);
        return nullptr;
    }

    std::string_view sql_sv(reinterpret_cast<const char*>(mem_opt->data + sql_ptr), static_cast<size_t>(sql_len));
    std::string_view params_sv(reinterpret_cast<const char*>(mem_opt->data + params_ptr), static_cast<size_t>(params_len));

    std::string first_kw = ExtractFirstKeyword(sql_sv);
    if (IsForbiddenTransactionControl(first_kw)) {
        ctx->last_error = Error(ErrorCode::InvalidArgument, "Transaction control statements are forbidden in Whisks");
        results[0].of.i64 = -static_cast<int64_t>(ErrorCode::InvalidArgument);
        return nullptr;
    }

    auto params_res = ParseHostAbiParamsJson(params_sv);
    if (!params_res) {
        ctx->last_error = params_res.error();
        results[0].of.i64 = -static_cast<int64_t>(params_res.error().Code());
        return nullptr;
    }

    std::vector<Value> params = std::move(*params_res);
    auto res = ctx->sql->Execute(sql_sv, params);
    if (!res) {
        ctx->last_error = res.error();
        results[0].of.i64 = -static_cast<int64_t>(res.error().Code());
        return nullptr;
    }

    if (res->is_mutation) {
        ctx->mutating_statements.push_back(DecodedStatement{
            .sql = std::string(sql_sv),
            .params = std::move(params)
        });
    }

    results[0].of.i64 = static_cast<int64_t>(res->rows_affected);
    return nullptr;
}

wasm_trap_t* WhiskDbQueryCallback(
    void* /*env*/,
    wasmtime_caller_t* caller,
    const wasmtime_val_t* args,
    size_t /*nargs*/,
    wasmtime_val_t* results,
    size_t /*nresults*/
) {
    wasmtime_context_t* store_ctx = wasmtime_caller_context(caller);
    auto* ctx = static_cast<WhiskCallContext*>(wasmtime_context_get_data(store_ctx));
    results[0].kind = WASMTIME_I32;

    if (ctx == nullptr || ctx->sql == nullptr) {
        results[0].of.i32 = -static_cast<int32_t>(ErrorCode::Internal);
        return nullptr;
    }

    auto mem_opt = GetCallerMemory(caller, store_ctx);
    if (!mem_opt) {
        ctx->last_error = Error(ErrorCode::WasmMemoryOutOfBounds, "Memory export 'memory' not found");
        results[0].of.i32 = -static_cast<int32_t>(ErrorCode::WasmMemoryOutOfBounds);
        return nullptr;
    }

    int32_t sql_ptr = args[0].of.i32;
    int32_t sql_len = args[1].of.i32;
    int32_t params_ptr = args[2].of.i32;
    int32_t params_len = args[3].of.i32;
    int32_t out_ptr = args[4].of.i32;
    int32_t out_cap = args[5].of.i32;

    if (!CheckBounds(sql_ptr, sql_len, mem_opt->size) ||
        !CheckBounds(params_ptr, params_len, mem_opt->size) ||
        !CheckBounds(out_ptr, out_cap, mem_opt->size)) {
        ctx->last_error = Error(ErrorCode::WasmMemoryOutOfBounds, "Pointer out of guest memory bounds");
        results[0].of.i32 = -static_cast<int32_t>(ErrorCode::WasmMemoryOutOfBounds);
        return nullptr;
    }

    std::string_view sql_sv(reinterpret_cast<const char*>(mem_opt->data + sql_ptr), static_cast<size_t>(sql_len));
    std::string_view params_sv(reinterpret_cast<const char*>(mem_opt->data + params_ptr), static_cast<size_t>(params_len));

    std::string first_kw = ExtractFirstKeyword(sql_sv);
    if (IsForbiddenTransactionControl(first_kw)) {
        ctx->last_error = Error(ErrorCode::InvalidArgument, "Transaction control statements are forbidden in Whisks");
        results[0].of.i32 = -static_cast<int32_t>(ErrorCode::InvalidArgument);
        return nullptr;
    }

    auto params_res = ParseHostAbiParamsJson(params_sv);
    if (!params_res) {
        ctx->last_error = params_res.error();
        results[0].of.i32 = -static_cast<int32_t>(params_res.error().Code());
        return nullptr;
    }

    bool need_guard = (first_kw != "SELECT" && first_kw != "EXPLAIN" && first_kw != "PRAGMA");
    std::optional<TransactionGuard> guard;
    if (need_guard) {
        auto guard_res = TransactionGuard::Begin(*ctx->sql);
        if (!guard_res) {
            ctx->last_error = guard_res.error();
            results[0].of.i32 = -static_cast<int32_t>(guard_res.error().Code());
            return nullptr;
        }
        guard = std::move(*guard_res);
    }

    auto res = ctx->sql->Execute(sql_sv, *params_res);
    if (!res) {
        if (guard) {
            (void)guard->Rollback();
        }
        ctx->last_error = res.error();
        results[0].of.i32 = -static_cast<int32_t>(res.error().Code());
        return nullptr;
    }

    if (res->is_mutation) {
        if (guard) {
            (void)guard->Rollback();
        }
        ctx->last_error = Error(ErrorCode::InvalidArgument, "Mutating statement cannot be executed via whisk_db_query");
        results[0].of.i32 = -static_cast<int32_t>(ErrorCode::InvalidArgument);
        return nullptr;
    }

    if (guard) {
        (void)guard->Commit();
    }

    std::string json = SerializeQueryResultToJson(*res);
    if (json.size() > static_cast<size_t>(out_cap)) {
        ctx->last_error = Error(ErrorCode::OutOfMemory, "Output buffer capacity too small for query result JSON");
        results[0].of.i32 = -static_cast<int32_t>(ErrorCode::OutOfMemory);
        return nullptr;
    }

    if (!json.empty()) {
        std::memcpy(mem_opt->data + out_ptr, json.data(), json.size());
    }

    results[0].of.i32 = static_cast<int32_t>(json.size());
    return nullptr;
}

wasm_trap_t* WhiskLogCallback(
    void* /*env*/,
    wasmtime_caller_t* caller,
    const wasmtime_val_t* args,
    size_t /*nargs*/,
    wasmtime_val_t* /*results*/,
    size_t /*nresults*/
) {
    wasmtime_context_t* store_ctx = wasmtime_caller_context(caller);
    auto* ctx = static_cast<WhiskCallContext*>(wasmtime_context_get_data(store_ctx));

    if (ctx == nullptr) {
        return nullptr;
    }

    auto mem_opt = GetCallerMemory(caller, store_ctx);
    if (!mem_opt) {
        ctx->last_error = Error(ErrorCode::WasmMemoryOutOfBounds, "Memory export 'memory' not found");
        return nullptr;
    }

    int32_t level = args[0].of.i32;
    int32_t msg_ptr = args[1].of.i32;
    int32_t msg_len = args[2].of.i32;

    if (!CheckBounds(msg_ptr, msg_len, mem_opt->size)) {
        ctx->last_error = Error(ErrorCode::WasmMemoryOutOfBounds, "Pointer out of guest memory bounds");
        return nullptr;
    }

    std::string_view msg_sv(reinterpret_cast<const char*>(mem_opt->data + msg_ptr), static_cast<size_t>(msg_len));

    switch (level) {
        case 0:
            LOG_TRACE("[whisk:{}] {}", ctx->module_name, msg_sv);
            break;
        case 1:
            LOG_DEBUG("[whisk:{}] {}", ctx->module_name, msg_sv);
            break;
        case 2:
            LOG_INFO("[whisk:{}] {}", ctx->module_name, msg_sv);
            break;
        case 3:
            LOG_WARN("[whisk:{}] {}", ctx->module_name, msg_sv);
            break;
        case 4:
            LOG_ERROR("[whisk:{}] {}", ctx->module_name, msg_sv);
            break;
        default:
            LOG_INFO("[whisk:{}] {}", ctx->module_name, msg_sv);
            break;
    }

    return nullptr;
}

} // namespace

Result<void> RegisterHostAbi(wasmtime_linker_t* linker) {
    // Linker pointer must not be null
    D_ASSERT(linker != nullptr);

    // 1. whisk_db_exec(i32, i32, i32, i32) -> i64
    {
        wasm_valtype_t* params_exec[4] = {
            wasm_valtype_new_i32(),
            wasm_valtype_new_i32(),
            wasm_valtype_new_i32(),
            wasm_valtype_new_i32(),
        };
        wasm_valtype_vec_t p_vec_exec;
        wasm_valtype_vec_new(&p_vec_exec, 4, params_exec);

        wasm_valtype_t* results_exec[1] = {
            wasm_valtype_new_i64(),
        };
        wasm_valtype_vec_t r_vec_exec;
        wasm_valtype_vec_new(&r_vec_exec, 1, results_exec);

        wasm_functype_t* ty_exec = wasm_functype_new(&p_vec_exec, &r_vec_exec);
        wasmtime_error_t* err = wasmtime_linker_define_func(
            linker, "whisk", 5, "whisk_db_exec", 13, ty_exec, WhiskDbExecCallback, nullptr, nullptr);
        wasm_functype_delete(ty_exec);
        if (err) {
            return std::unexpected(WasmRuntime::TranslateError(err));
        }
    }

    // 2. whisk_db_query(i32, i32, i32, i32, i32, i32) -> i32
    {
        wasm_valtype_t* params_query[6] = {
            wasm_valtype_new_i32(),
            wasm_valtype_new_i32(),
            wasm_valtype_new_i32(),
            wasm_valtype_new_i32(),
            wasm_valtype_new_i32(),
            wasm_valtype_new_i32(),
        };
        wasm_valtype_vec_t p_vec_query;
        wasm_valtype_vec_new(&p_vec_query, 6, params_query);

        wasm_valtype_t* results_query[1] = {
            wasm_valtype_new_i32(),
        };
        wasm_valtype_vec_t r_vec_query;
        wasm_valtype_vec_new(&r_vec_query, 1, results_query);

        wasm_functype_t* ty_query = wasm_functype_new(&p_vec_query, &r_vec_query);
        wasmtime_error_t* err = wasmtime_linker_define_func(
            linker, "whisk", 5, "whisk_db_query", 14, ty_query, WhiskDbQueryCallback, nullptr, nullptr);
        wasm_functype_delete(ty_query);
        if (err) {
            return std::unexpected(WasmRuntime::TranslateError(err));
        }
    }

    // 3. whisk_log(i32, i32, i32) -> void
    {
        wasm_valtype_t* params_log[3] = {
            wasm_valtype_new_i32(),
            wasm_valtype_new_i32(),
            wasm_valtype_new_i32(),
        };
        wasm_valtype_vec_t p_vec_log;
        wasm_valtype_vec_new(&p_vec_log, 3, params_log);

        wasm_valtype_vec_t r_vec_log;
        wasm_valtype_vec_new_empty(&r_vec_log);

        wasm_functype_t* ty_log = wasm_functype_new(&p_vec_log, &r_vec_log);
        wasmtime_error_t* err = wasmtime_linker_define_func(
            linker, "whisk", 5, "whisk_log", 9, ty_log, WhiskLogCallback, nullptr, nullptr);
        wasm_functype_delete(ty_log);
        if (err) {
            return std::unexpected(WasmRuntime::TranslateError(err));
        }
    }

    return {};
}

Result<std::vector<Value>> ParseHostAbiParamsJson(std::string_view params_json) {
    size_t pos = 0;
    while (pos < params_json.size() && std::isspace(static_cast<unsigned char>(params_json[pos]))) {
        ++pos;
    }
    if (pos == params_json.size()) {
        return std::vector<Value>{};
    }

    if (params_json[pos] != '[') {
        return MAKE_ERROR(ErrorCode::InvalidArgument, "JSON params must start with '['");
    }
    ++pos; // skip '['

    std::vector<Value> values;

    while (pos < params_json.size()) {
        while (pos < params_json.size() && std::isspace(static_cast<unsigned char>(params_json[pos]))) {
            ++pos;
        }
        if (pos >= params_json.size()) {
            return MAKE_ERROR(ErrorCode::InvalidArgument, "Unexpected end of JSON params array");
        }
        if (params_json[pos] == ']') {
            ++pos;
            break;
        }

        // Parse element value
        if (params_json[pos] == '"') {
            ++pos;
            std::string str_val;
            while (pos < params_json.size() && params_json[pos] != '"') {
                if (params_json[pos] == '\\') {
                    ++pos;
                    if (pos >= params_json.size()) {
                        return MAKE_ERROR(ErrorCode::InvalidArgument, "Truncated escape sequence in JSON string");
                    }
                    char esc = params_json[pos];
                    switch (esc) {
                        case '"': str_val.push_back('"'); break;
                        case '\\': str_val.push_back('\\'); break;
                        case '/': str_val.push_back('/'); break;
                        case 'b': str_val.push_back('\b'); break;
                        case 'f': str_val.push_back('\f'); break;
                        case 'n': str_val.push_back('\n'); break;
                        case 'r': str_val.push_back('\r'); break;
                        case 't': str_val.push_back('\t'); break;
                        case 'u': {
                            if (pos + 4 >= params_json.size()) {
                                return MAKE_ERROR(ErrorCode::InvalidArgument, "Truncated \\uXXXX escape in JSON string");
                            }
                            uint32_t cp = 0;
                            for (int k = 1; k <= 4; ++k) {
                                char h = params_json[pos + k];
                                cp <<= 4;
                                if (h >= '0' && h <= '9') {
                                    cp |= static_cast<uint32_t>(h - '0');
                                } else if (h >= 'a' && h <= 'f') {
                                    cp |= static_cast<uint32_t>(h - 'a' + 10);
                                } else if (h >= 'A' && h <= 'F') {
                                    cp |= static_cast<uint32_t>(h - 'A' + 10);
                                } else {
                                    return MAKE_ERROR(ErrorCode::InvalidArgument, "Invalid hex digit in \\uXXXX escape");
                                }
                            }
                            pos += 4;
                            // Check surrogate pair
                            if (cp >= 0xD800 && cp <= 0xDBFF) {
                                if (pos + 6 < params_json.size() &&
                                    params_json[pos + 1] == '\\' && params_json[pos + 2] == 'u') {
                                    uint32_t low = 0;
                                    bool valid_low = true;
                                    for (int k = 3; k <= 6; ++k) {
                                        char h = params_json[pos + k];
                                        low <<= 4;
                                        if (h >= '0' && h <= '9') {
                                            low |= static_cast<uint32_t>(h - '0');
                                        } else if (h >= 'a' && h <= 'f') {
                                            low |= static_cast<uint32_t>(h - 'a' + 10);
                                        } else if (h >= 'A' && h <= 'F') {
                                            low |= static_cast<uint32_t>(h - 'A' + 10);
                                        } else {
                                            valid_low = false;
                                            break;
                                        }
                                    }
                                    if (valid_low && low >= 0xDC00 && low <= 0xDFFF) {
                                        cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                                        pos += 6;
                                    }
                                }
                            }
                            // UTF-8 encode code point
                            if (cp <= 0x7F) {
                                str_val.push_back(static_cast<char>(cp));
                            } else if (cp <= 0x7FF) {
                                str_val.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                                str_val.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                            } else if (cp <= 0xFFFF) {
                                str_val.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                                str_val.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                                str_val.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                            } else {
                                str_val.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                                str_val.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                                str_val.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                                str_val.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                            }
                            break;
                        }
                        default:
                            return MAKE_ERROR(ErrorCode::InvalidArgument, "Invalid escape sequence in JSON string");
                    }
                } else {
                    str_val.push_back(params_json[pos]);
                }
                ++pos;
            }
            if (pos >= params_json.size() || params_json[pos] != '"') {
                return MAKE_ERROR(ErrorCode::InvalidArgument, "Unclosed quote in JSON string parameter");
            }
            ++pos; // skip closing quote
            values.push_back(Value(std::move(str_val)));
        } else if (params_json.substr(pos, 4) == "null") {
            values.push_back(Value(std::monostate{}));
            pos += 4;
        } else if (params_json.substr(pos, 4) == "true") {
            values.push_back(Value(int64_t{1}));
            pos += 4;
        } else if (params_json.substr(pos, 5) == "false") {
            values.push_back(Value(int64_t{0}));
            pos += 5;
        } else if (params_json[pos] == '-' || (params_json[pos] >= '0' && params_json[pos] <= '9')) {
            size_t start = pos;
            bool is_float = false;
            while (pos < params_json.size() &&
                   ((params_json[pos] >= '0' && params_json[pos] <= '9') ||
                    params_json[pos] == '-' || params_json[pos] == '+' ||
                    params_json[pos] == '.' || params_json[pos] == 'e' || params_json[pos] == 'E')) {
                if (params_json[pos] == '.' || params_json[pos] == 'e' || params_json[pos] == 'E') {
                    is_float = true;
                }
                ++pos;
            }
            std::string_view num_sv = params_json.substr(start, pos - start);
            if (is_float) {
                double val = 0.0;
                auto [ptr, ec] = std::from_chars(num_sv.data(), num_sv.data() + num_sv.size(), val);
                if (ec != std::errc{} || ptr != num_sv.data() + num_sv.size()) {
                    return MAKE_ERROR(ErrorCode::InvalidArgument, "Invalid floating point number in JSON params");
                }
                values.push_back(Value(val));
            } else {
                int64_t val = 0;
                auto [ptr, ec] = std::from_chars(num_sv.data(), num_sv.data() + num_sv.size(), val);
                if (ec != std::errc{} || ptr != num_sv.data() + num_sv.size()) {
                    return MAKE_ERROR(ErrorCode::InvalidArgument, "Invalid integer in JSON params");
                }
                values.push_back(Value(val));
            }
        } else {
            return MAKE_ERROR(ErrorCode::InvalidArgument, "Unsupported token in JSON params array");
        }

        // After element: expect ',' or ']'
        while (pos < params_json.size() && std::isspace(static_cast<unsigned char>(params_json[pos]))) {
            ++pos;
        }
        if (pos >= params_json.size()) {
            return MAKE_ERROR(ErrorCode::InvalidArgument, "Unexpected end of JSON params array after element");
        }
        if (params_json[pos] == ',') {
            ++pos;
            // Check for illegal trailing comma before ']'
            while (pos < params_json.size() && std::isspace(static_cast<unsigned char>(params_json[pos]))) {
                ++pos;
            }
            if (pos < params_json.size() && params_json[pos] == ']') {
                return MAKE_ERROR(ErrorCode::InvalidArgument, "Trailing comma in JSON array");
            }
        } else if (params_json[pos] == ']') {
            ++pos;
            break;
        } else {
            return MAKE_ERROR(ErrorCode::InvalidArgument, "Expected ',' or ']' after element in JSON array");
        }
    }

    // Check for trailing non-whitespace characters
    while (pos < params_json.size() && std::isspace(static_cast<unsigned char>(params_json[pos]))) {
        ++pos;
    }
    if (pos != params_json.size()) {
        return MAKE_ERROR(ErrorCode::InvalidArgument, "Trailing characters after JSON array");
    }

    return values;
}

std::string SerializeQueryResultToJson(const QueryResult& result) {
    std::string out;
    out.reserve(64 + result.rows.size() * 32);

    out += "{\"columns\":[";
    for (size_t i = 0; i < result.columns.size(); ++i) {
        if (i > 0) {
            out.push_back(',');
        }
        AppendJsonEscapedString(out, result.columns[i]);
    }
    out += "],\"rows\":[";
    for (size_t r = 0; r < result.rows.size(); ++r) {
        if (r > 0) {
            out.push_back(',');
        }
        out.push_back('[');
        const auto& row = result.rows[r];
        for (size_t c = 0; c < row.size(); ++c) {
            if (c > 0) {
                out.push_back(',');
            }
            AppendJsonValue(out, row[c]);
        }
        out.push_back(']');
    }
    out += "]}";

    return out;
}

} // namespace database
