#ifndef WHISK_GUEST_HPP
#define WHISK_GUEST_HPP

#include <stddef.h>
#include <stdint.h>

// -----------------------------------------------------------------------------
// Host Imports (exact match to Whisk Host ABI in src/wasm/host_abi.cpp)
// -----------------------------------------------------------------------------
extern "C" {
    [[clang::import_module("whisk"), clang::import_name("whisk_db_exec")]]
    int64_t whisk_db_exec(int32_t sql_ptr, int32_t sql_len, int32_t params_ptr, int32_t params_len);

    [[clang::import_module("whisk"), clang::import_name("whisk_db_query")]]
    int32_t whisk_db_query(int32_t sql_ptr, int32_t sql_len, int32_t params_ptr, int32_t params_len,
                           int32_t out_ptr, int32_t out_cap);

    [[clang::import_module("whisk"), clang::import_name("whisk_log")]]
    void whisk_log(int32_t level, int32_t msg_ptr, int32_t msg_len);
}

// -----------------------------------------------------------------------------
// Guest Static Arenas and Required Exports
// -----------------------------------------------------------------------------
alignas(8) static uint8_t g_whisk_heap[1024 * 1024];
static int32_t g_whisk_heap_offset = 8; // initial offset 8 so valid pointers are non-zero

alignas(8) static uint8_t g_whisk_result_buf[64 * 1024];
static int32_t g_whisk_result_len = 0;

extern "C" {

__attribute__((used, visibility("default")))
int32_t whisk_alloc(int32_t size) {
    if (size < 0) {
        return 0;
    }
    int32_t aligned = (g_whisk_heap_offset + 7) & ~7;
    if (static_cast<uint64_t>(aligned) + static_cast<uint64_t>(size) > sizeof(g_whisk_heap)) {
        return 0;
    }
    int32_t ptr = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&g_whisk_heap[aligned]));
    g_whisk_heap_offset = aligned + size;
    return ptr;
}

__attribute__((used, visibility("default")))
void whisk_reset_heap() {
    g_whisk_heap_offset = 8;
}

__attribute__((used, visibility("default")))
int32_t whisk_result_ptr() {
    return static_cast<int32_t>(reinterpret_cast<uintptr_t>(g_whisk_result_buf));
}

__attribute__((used, visibility("default")))
int32_t whisk_result_len() {
    return g_whisk_result_len;
}

} // extern "C"

// -----------------------------------------------------------------------------
// Developer Macros
// -----------------------------------------------------------------------------
#define WHISK_INIT() extern "C" __attribute__((used, visibility("default"))) int32_t __init__()
#define WHISK_UPDATE() extern "C" __attribute__((used, visibility("default"))) int32_t __update__()
#define WHISK_FUNC(name) extern "C" __attribute__((used, visibility("default"))) int32_t name([[maybe_unused]] int32_t _whisk_args_ptr, [[maybe_unused]] int32_t _whisk_args_len)
#define WHISK_TEST(name) extern "C" __attribute__((used, visibility("default"))) int32_t test_##name([[maybe_unused]] int32_t _whisk_args_ptr, [[maybe_unused]] int32_t _whisk_args_len)
#define WHISK_ARGS() whisk::Args(_whisk_args_ptr, _whisk_args_len)

// -----------------------------------------------------------------------------
// Namespace whisk: Zero-Libc Ergonomic Helpers
// -----------------------------------------------------------------------------
namespace whisk {

struct StrView {
    const char* ptr{nullptr};
    size_t len{0};

    constexpr StrView() noexcept = default;
    constexpr StrView(const char* p, size_t n) noexcept : ptr(p), len(n) {}
    constexpr StrView(const char* s) noexcept : ptr(s), len(0) {
        if (s != nullptr) {
            while (s[len] != '\0') {
                ++len;
            }
        }
    }

    constexpr const char* data() const noexcept { return ptr; }
    constexpr size_t size() const noexcept { return len; }
    constexpr bool empty() const noexcept { return len == 0; }
    constexpr char operator[](size_t idx) const noexcept { return ptr[idx]; }

    constexpr bool equals(StrView o) const noexcept {
        if (len != o.len) return false;
        if (ptr == o.ptr) return true;
        for (size_t i = 0; i < len; ++i) {
            if (ptr[i] != o.ptr[i]) return false;
        }
        return true;
    }

    constexpr bool operator==(StrView o) const noexcept { return equals(o); }
    constexpr bool operator!=(StrView o) const noexcept { return !equals(o); }

    constexpr size_t find(StrView needle, size_t start = 0) const noexcept {
        if (needle.len == 0) {
            return (start <= len) ? start : ~size_t{0};
        }
        if (start + needle.len > len) {
            return ~size_t{0};
        }
        for (size_t i = start; i + needle.len <= len; ++i) {
            bool match = true;
            for (size_t j = 0; j < needle.len; ++j) {
                if (ptr[i + j] != needle.ptr[j]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                return i;
            }
        }
        return ~size_t{0};
    }

    constexpr size_t find(char c, size_t start = 0) const noexcept {
        for (size_t i = start; i < len; ++i) {
            if (ptr[i] == c) return i;
        }
        return ~size_t{0};
    }

    constexpr StrView substr(size_t pos, size_t count = ~size_t{0}) const noexcept {
        if (pos >= len) {
            return StrView();
        }
        size_t rem = len - pos;
        if (count > rem) {
            count = rem;
        }
        return StrView(ptr + pos, count);
    }
};

inline StrView Args(int32_t ptr, int32_t len) {
    if (ptr <= 0 || len <= 0) {
        return StrView();
    }
    return StrView(reinterpret_cast<const char*>(static_cast<uintptr_t>(ptr)), static_cast<size_t>(len));
}

template <size_t N>
class Buf {
public:
    constexpr Buf() noexcept = default;

    void append_char(char c) noexcept {
        if (len_ < N) {
            data_[len_++] = c;
        } else {
            overflow_ = true;
        }
    }

    void append(StrView sv) noexcept {
        for (size_t i = 0; i < sv.size(); ++i) {
            append_char(sv[i]);
        }
    }

    void append_int(int64_t v) noexcept {
        if (v == 0) {
            append_char('0');
            return;
        }
        uint64_t u = 0;
        if (v < 0) {
            append_char('-');
            u = ~static_cast<uint64_t>(v) + 1ULL;
        } else {
            u = static_cast<uint64_t>(v);
        }
        char tmp[24];
        size_t count = 0;
        while (u > 0) {
            tmp[count++] = static_cast<char>('0' + (u % 10));
            u /= 10;
        }
        for (size_t i = count; i > 0; --i) {
            append_char(tmp[i - 1]);
        }
    }

    void append_double(double d) noexcept {
        if (d != d) {
            append("null");
            return;
        }
        if (d < 0) {
            append_char('-');
            d = -d;
        }
        if (d >= 1e18) {
            append("null");
            return;
        }
        auto int_part = static_cast<uint64_t>(d);
        append_int(static_cast<int64_t>(int_part));
        append_char('.');
        double frac = d - static_cast<double>(int_part);
        for (int i = 0; i < 6; ++i) {
            frac *= 10.0;
            auto digit = static_cast<uint32_t>(frac);
            append_char(static_cast<char>('0' + digit));
            frac -= static_cast<double>(digit);
        }
        while (len_ > 2 && data_[len_ - 1] == '0' && data_[len_ - 2] != '.') {
            --len_;
        }
    }

    void append_json_str(StrView sv) noexcept {
        append_char('"');
        for (size_t i = 0; i < sv.size(); ++i) {
            char c = sv[i];
            switch (c) {
                case '"': append_char('\\'); append_char('"'); break;
                case '\\': append_char('\\'); append_char('\\'); break;
                case '\b': append_char('\\'); append_char('b'); break;
                case '\f': append_char('\\'); append_char('f'); break;
                case '\n': append_char('\\'); append_char('n'); break;
                case '\r': append_char('\\'); append_char('r'); break;
                case '\t': append_char('\\'); append_char('t'); break;
                default: {
                    unsigned char uc = static_cast<unsigned char>(c);
                    if (uc < 0x20) {
                        append("\\u00");
                        char hi = static_cast<char>(uc >> 4);
                        char lo = static_cast<char>(uc & 0x0F);
                        append_char(static_cast<char>(hi < 10 ? '0' + hi : 'a' + hi - 10));
                        append_char(static_cast<char>(lo < 10 ? '0' + lo : 'a' + lo - 10));
                    } else {
                        append_char(c);
                    }
                    break;
                }
            }
        }
        append_char('"');
    }

    constexpr StrView view() const noexcept {
        return StrView(data_, len_);
    }

    constexpr const char* data() const noexcept {
        return data_;
    }

    void clear() noexcept {
        len_ = 0;
        overflow_ = false;
    }

    constexpr size_t size() const noexcept {
        return len_;
    }

    constexpr size_t capacity() const noexcept {
        return N;
    }

    constexpr bool overflowed() const noexcept {
        return overflow_;
    }

private:
    char data_[N]{};
    size_t len_{0};
    bool overflow_{false};
};

inline bool JsonGetInt(StrView json, StrView key, int64_t& out) {
    if (json.empty() || key.empty()) {
        return false;
    }
    Buf<128> needle_buf;
    needle_buf.append_char('"');
    needle_buf.append(key);
    needle_buf.append_char('"');
    StrView needle = needle_buf.view();

    size_t pos = 0;
    while (pos < json.size()) {
        size_t found = json.find(needle, pos);
        if (found == ~size_t{0}) {
            return false;
        }
        size_t p = found + needle.size();
        while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' || json[p] == '\r')) {
            ++p;
        }
        if (p < json.size() && json[p] == ':') {
            ++p;
            while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' || json[p] == '\r')) {
                ++p;
            }
            if (p < json.size() && (json[p] == '-' || (json[p] >= '0' && json[p] <= '9'))) {
                bool neg = false;
                if (json[p] == '-') {
                    neg = true;
                    ++p;
                }
                if (p < json.size() && json[p] >= '0' && json[p] <= '9') {
                    uint64_t val = 0;
                    while (p < json.size() && json[p] >= '0' && json[p] <= '9') {
                        val = val * 10 + static_cast<uint64_t>(json[p] - '0');
                        ++p;
                    }
                    if (neg) {
                        out = -static_cast<int64_t>(val);
                    } else {
                        out = static_cast<int64_t>(val);
                    }
                    return true;
                }
            }
        }
        pos = found + 1;
    }
    return false;
}

inline int64_t JsonGetIntOr(StrView json, StrView key, int64_t fallback = 0) {
    int64_t val = fallback;
    if (JsonGetInt(json, key, val)) {
        return val;
    }
    return fallback;
}

inline bool JsonGetStr(StrView json, StrView key, StrView& out) {
    if (json.empty() || key.empty()) {
        return false;
    }
    Buf<128> needle_buf;
    needle_buf.append_char('"');
    needle_buf.append(key);
    needle_buf.append_char('"');
    StrView needle = needle_buf.view();

    size_t pos = 0;
    while (pos < json.size()) {
        size_t found = json.find(needle, pos);
        if (found == ~size_t{0}) {
            return false;
        }
        size_t p = found + needle.size();
        while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' || json[p] == '\r')) {
            ++p;
        }
        if (p < json.size() && json[p] == ':') {
            ++p;
            while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' || json[p] == '\r')) {
                ++p;
            }
            if (p < json.size() && json[p] == '"') {
                ++p;
                size_t start = p;
                while (p < json.size()) {
                    if (json[p] == '\\') {
                        p += 2;
                        continue;
                    }
                    if (json[p] == '"') {
                        out = StrView(json.data() + start, p - start);
                        return true;
                    }
                    ++p;
                }
            }
        }
        pos = found + 1;
    }
    return false;
}

inline bool QueryHasRows(StrView query_json) {
    size_t pos = query_json.find("\"rows\":[[");
    if (pos != ~size_t{0}) {
        return true;
    }
    size_t rows_pos = query_json.find("\"rows\"");
    if (rows_pos == ~size_t{0}) {
        return false;
    }
    size_t p = rows_pos + 6;
    while (p < query_json.size() && (query_json[p] == ' ' || query_json[p] == '\t' || query_json[p] == '\n' || query_json[p] == '\r')) {
        ++p;
    }
    if (p >= query_json.size() || query_json[p] != ':') {
        return false;
    }
    ++p;
    while (p < query_json.size() && (query_json[p] == ' ' || query_json[p] == '\t' || query_json[p] == '\n' || query_json[p] == '\r')) {
        ++p;
    }
    if (p >= query_json.size() || query_json[p] != '[') {
        return false;
    }
    ++p;
    while (p < query_json.size() && (query_json[p] == ' ' || query_json[p] == '\t' || query_json[p] == '\n' || query_json[p] == '\r')) {
        ++p;
    }
    return (p < query_json.size() && query_json[p] == '[');
}

inline bool QueryRowInt(StrView query_json, size_t col_idx, int64_t& out) {
    size_t rows_pos = query_json.find("\"rows\"");
    if (rows_pos == ~size_t{0}) return false;
    size_t p = rows_pos + 6;
    while (p < query_json.size() && query_json[p] != ':') ++p;
    if (p >= query_json.size()) return false;
    ++p; // skip ':'
    while (p < query_json.size() && query_json[p] != '[') ++p;
    if (p >= query_json.size()) return false;
    ++p; // skip outer '['
    while (p < query_json.size() && query_json[p] != '[') {
        if (query_json[p] == ']') return false; // empty rows: []
        ++p;
    }
    if (p >= query_json.size()) return false;
    ++p; // skip row's '['

    for (size_t c = 0; c < col_idx; ++c) {
        while (p < query_json.size() && (query_json[p] == ' ' || query_json[p] == '\t' || query_json[p] == '\n' || query_json[p] == '\r')) ++p;
        if (p >= query_json.size()) return false;
        if (query_json[p] == '"') {
            ++p;
            while (p < query_json.size()) {
                if (query_json[p] == '\\') { p += 2; continue; }
                if (query_json[p] == '"') { ++p; break; }
                ++p;
            }
        } else {
            while (p < query_json.size() && query_json[p] != ',' && query_json[p] != ']') {
                ++p;
            }
        }
        while (p < query_json.size() && (query_json[p] == ' ' || query_json[p] == '\t' || query_json[p] == '\n' || query_json[p] == '\r')) ++p;
        if (p >= query_json.size() || query_json[p] != ',') return false;
        ++p; // skip ','
    }

    while (p < query_json.size() && (query_json[p] == ' ' || query_json[p] == '\t' || query_json[p] == '\n' || query_json[p] == '\r')) ++p;
    if (p >= query_json.size()) return false;
    bool neg = false;
    if (query_json[p] == '-') {
        neg = true;
        ++p;
    }
    if (p >= query_json.size() || query_json[p] < '0' || query_json[p] > '9') {
        return false;
    }
    uint64_t val = 0;
    while (p < query_json.size() && query_json[p] >= '0' && query_json[p] <= '9') {
        val = val * 10 + static_cast<uint64_t>(query_json[p] - '0');
        ++p;
    }
    if (neg) {
        out = -static_cast<int64_t>(val);
    } else {
        out = static_cast<int64_t>(val);
    }
    return true;
}

inline bool QueryFirstInt(StrView query_json, int64_t& out) {
    return QueryRowInt(query_json, 0, out);
}

// -----------------------------------------------------------------------------
// Database, Log & Response Helpers
// -----------------------------------------------------------------------------
inline int64_t Exec(StrView sql, StrView params_json = StrView("[]", 2)) {
    int32_t sql_ptr = static_cast<int32_t>(reinterpret_cast<uintptr_t>(sql.data()));
    int32_t sql_len = static_cast<int32_t>(sql.size());
    int32_t p_ptr = static_cast<int32_t>(reinterpret_cast<uintptr_t>(params_json.data()));
    int32_t p_len = static_cast<int32_t>(params_json.size());
    return whisk_db_exec(sql_ptr, sql_len, p_ptr, p_len);
}

alignas(8) static char g_whisk_query_buf[64 * 1024];
static int32_t g_last_query_status = 0;

inline StrView Query(StrView sql, StrView params_json = StrView("[]", 2)) {
    int32_t sql_ptr = static_cast<int32_t>(reinterpret_cast<uintptr_t>(sql.data()));
    int32_t sql_len = static_cast<int32_t>(sql.size());
    int32_t p_ptr = static_cast<int32_t>(reinterpret_cast<uintptr_t>(params_json.data()));
    int32_t p_len = static_cast<int32_t>(params_json.size());
    int32_t out_ptr = static_cast<int32_t>(reinterpret_cast<uintptr_t>(g_whisk_query_buf));
    int32_t out_cap = static_cast<int32_t>(sizeof(g_whisk_query_buf));

    int32_t rc = whisk_db_query(sql_ptr, sql_len, p_ptr, p_len, out_ptr, out_cap);
    g_last_query_status = rc;
    if (rc < 0) {
        return StrView();
    }
    return StrView(g_whisk_query_buf, static_cast<size_t>(rc));
}

inline int32_t LastQueryStatus() {
    return g_last_query_status;
}

inline void LogTrace(StrView msg) {
    whisk_log(0, static_cast<int32_t>(reinterpret_cast<uintptr_t>(msg.data())), static_cast<int32_t>(msg.size()));
}

inline void LogDebug(StrView msg) {
    whisk_log(1, static_cast<int32_t>(reinterpret_cast<uintptr_t>(msg.data())), static_cast<int32_t>(msg.size()));
}

inline void LogInfo(StrView msg) {
    whisk_log(2, static_cast<int32_t>(reinterpret_cast<uintptr_t>(msg.data())), static_cast<int32_t>(msg.size()));
}

inline void LogWarn(StrView msg) {
    whisk_log(3, static_cast<int32_t>(reinterpret_cast<uintptr_t>(msg.data())), static_cast<int32_t>(msg.size()));
}

inline void LogError(StrView msg) {
    whisk_log(4, static_cast<int32_t>(reinterpret_cast<uintptr_t>(msg.data())), static_cast<int32_t>(msg.size()));
}

inline int32_t Ok(StrView json_response) {
    size_t to_copy = json_response.size();
    if (to_copy > sizeof(g_whisk_result_buf)) {
        to_copy = sizeof(g_whisk_result_buf);
    }
    for (size_t i = 0; i < to_copy; ++i) {
        g_whisk_result_buf[i] = static_cast<uint8_t>(json_response[i]);
    }
    g_whisk_result_len = static_cast<int32_t>(to_copy);
    return 0;
}

inline int32_t Error(StrView error_msg) {
    size_t to_copy = error_msg.size();
    if (to_copy > sizeof(g_whisk_result_buf)) {
        to_copy = sizeof(g_whisk_result_buf);
    }
    for (size_t i = 0; i < to_copy; ++i) {
        g_whisk_result_buf[i] = static_cast<uint8_t>(error_msg[i]);
    }
    g_whisk_result_len = static_cast<int32_t>(to_copy);
    return 1;
}

} // namespace whisk

#endif // WHISK_GUEST_HPP
