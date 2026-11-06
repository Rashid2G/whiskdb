#include "sql/value_codec.hpp"

#include <cassert>
#include <limits>
#include <print>

using namespace database;

void test_value_roundtrips() {
    std::println("Running test_value_roundtrips...");

    // 1. Null
    {
        std::vector<uint8_t> buf;
        Value v = std::monostate{};
        EncodeValue(buf, v);
        assert(buf.size() == 1);
        assert(buf[0] == static_cast<uint8_t>(ValueType::Null));

        std::span<const uint8_t> in(buf);
        auto decoded = DecodeValue(in);
        assert(decoded.has_value());
        assert(std::holds_alternative<std::monostate>(*decoded));
        assert(in.empty());
    }

    // 2. Int64
    const std::vector<int64_t> int_cases = {
        0, 1, -1, 42, -42,
        std::numeric_limits<int64_t>::max(),
        std::numeric_limits<int64_t>::min()
    };
    for (int64_t val : int_cases) {
        std::vector<uint8_t> buf;
        EncodeValue(buf, Value(val));
        assert(buf.size() == 9);
        assert(buf[0] == static_cast<uint8_t>(ValueType::Int64));

        std::span<const uint8_t> in(buf);
        auto decoded = DecodeValue(in);
        assert(decoded.has_value());
        assert(std::holds_alternative<int64_t>(*decoded));
        assert(std::get<int64_t>(*decoded) == val);
        assert(in.empty());
    }

    // 3. Double
    const std::vector<double> double_cases = {
        0.0, -0.0, 1.0, -1.0, 3.141592653589793, -2.718281828459045,
        1e-100, 1e100,
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()
    };
    for (double val : double_cases) {
        std::vector<uint8_t> buf;
        EncodeValue(buf, Value(val));
        assert(buf.size() == 9);
        assert(buf[0] == static_cast<uint8_t>(ValueType::Double));

        std::span<const uint8_t> in(buf);
        auto decoded = DecodeValue(in);
        assert(decoded.has_value());
        assert(std::holds_alternative<double>(*decoded));
        assert(std::get<double>(*decoded) == val);
        assert(in.empty());
    }

    // 4. Text (including empty and embedded NUL)
    std::string text_with_nul = "hello\0world";
    text_with_nul.resize(11);
    const std::vector<std::string> text_cases = {
        "", "a", "simple text", "Hello, 世界 🌍", text_with_nul
    };
    for (const auto& s : text_cases) {
        std::vector<uint8_t> buf;
        EncodeValue(buf, Value(s));
        assert(buf.size() == 1 + 4 + s.size());
        assert(buf[0] == static_cast<uint8_t>(ValueType::Text));

        std::span<const uint8_t> in(buf);
        auto decoded = DecodeValue(in);
        assert(decoded.has_value());
        assert(std::holds_alternative<std::string>(*decoded));
        assert(std::get<std::string>(*decoded) == s);
        assert(in.empty());
    }

    // 5. Blob (empty, small, embedded NUL)
    Blob empty_blob;
    Blob blob_with_nuls = {0x00, 0x01, 0x00, 0xFF, 0x00, 0x42};
    const std::vector<Blob> blob_cases = {
        empty_blob,
        Blob{0xDE, 0xAD, 0xBE, 0xEF},
        blob_with_nuls
    };
    for (const auto& b : blob_cases) {
        std::vector<uint8_t> buf;
        EncodeValue(buf, Value(b));
        assert(buf.size() == 1 + 4 + b.size());
        assert(buf[0] == static_cast<uint8_t>(ValueType::Blob));

        std::span<const uint8_t> in(buf);
        auto decoded = DecodeValue(in);
        assert(decoded.has_value());
        assert(std::holds_alternative<Blob>(*decoded));
        assert(std::get<Blob>(*decoded) == b);
        assert(in.empty());
    }
}

void test_value_rejections() {
    std::println("Running test_value_rejections...");

    // Empty buffer
    {
        std::vector<uint8_t> empty;
        std::span<const uint8_t> in(empty);
        auto res = DecodeValue(in);
        assert(!res.has_value());
        assert(res.error().Code() == ErrorCode::ProtocolError);
    }

    // Bad tag
    {
        std::vector<uint8_t> bad_tag = {0x7F};
        std::span<const uint8_t> in(bad_tag);
        auto res = DecodeValue(in);
        assert(!res.has_value());
        assert(res.error().Code() == ErrorCode::ProtocolError);
    }

    // Truncated int64
    {
        std::vector<uint8_t> trunc_int = {0x01, 0x01, 0x02, 0x03}; // only 3 bytes of payload instead of 8
        std::span<const uint8_t> in(trunc_int);
        auto res = DecodeValue(in);
        assert(!res.has_value());
        assert(res.error().Code() == ErrorCode::ProtocolError);
    }

    // Forged text length
    {
        std::vector<uint8_t> forged_text = {0x03, 0xFF, 0xFF, 0x00, 0x00, 'a', 'b'}; // len 65535, only 2 bytes present
        std::span<const uint8_t> in(forged_text);
        auto res = DecodeValue(in);
        assert(!res.has_value());
        assert(res.error().Code() == ErrorCode::ProtocolError);
    }

    // Forged blob length
    {
        std::vector<uint8_t> forged_blob = {0x04, 0x00, 0x01, 0x00, 0x00, 0xAA}; // len 256, only 1 byte present
        std::span<const uint8_t> in(forged_blob);
        auto res = DecodeValue(in);
        assert(!res.has_value());
        assert(res.error().Code() == ErrorCode::ProtocolError);
    }
}

void test_statement_codec() {
    std::println("Running test_statement_codec...");

    // Roundtrip with params
    {
        std::vector<Value> params = {
            int64_t(101),
            std::string("WhiskDB"),
            std::monostate{},
            3.14,
            Blob{0x01, 0x02}
        };
        std::string sql = "INSERT INTO users (id, name, opt, score, data) VALUES (?, ?, ?, ?, ?);";

        std::vector<uint8_t> out;
        EncodeStatement(out, sql, params);

        auto dec_res = DecodeStatement(out);
        assert(dec_res.has_value());
        assert(dec_res->sql == sql);
        assert(dec_res->params.size() == params.size());
        assert(std::get<int64_t>(dec_res->params[0]) == 101);
        assert(std::get<std::string>(dec_res->params[1]) == "WhiskDB");
        assert(std::holds_alternative<std::monostate>(dec_res->params[2]));
        assert(std::get<double>(dec_res->params[3]) == 3.14);
        assert(std::get<Blob>(dec_res->params[4]) == (Blob{0x01, 0x02}));
    }

    // Empty params
    {
        std::string sql = "SELECT * FROM users;";
        std::vector<uint8_t> out;
        EncodeStatement(out, sql, {});

        auto dec_res = DecodeStatement(out);
        assert(dec_res.has_value());
        assert(dec_res->sql == sql);
        assert(dec_res->params.empty());
    }

    // Hostile input: forged SQL length
    {
        std::vector<uint8_t> forged = {0xFF, 0x00, 0x00, 0x00, 'S', 'E', 'L'};
        auto res = DecodeStatement(forged);
        assert(!res.has_value());
        assert(res.error().Code() == ErrorCode::ProtocolError);
    }

    // Hostile input: forged param count
    {
        std::vector<uint8_t> forged = {
            0x04, 0x00, 0x00, 0x00, 'T', 'E', 'S', 'T',
            0xFF, 0xFF, 0x00, 0x00 // 65535 params, 0 bytes follow
        };
        auto res = DecodeStatement(forged);
        assert(!res.has_value());
        assert(res.error().Code() == ErrorCode::ProtocolError);
    }

    // Trailing garbage
    {
        std::string sql = "SELECT 1;";
        std::vector<uint8_t> out;
        EncodeStatement(out, sql, {});
        out.push_back(0xCC); // 1 byte garbage

        auto res = DecodeStatement(out);
        assert(!res.has_value());
        assert(res.error().Code() == ErrorCode::ProtocolError);
    }
}

void test_query_result_codec() {
    std::println("Running test_query_result_codec...");

    // DML result without rows
    {
        QueryResult r;
        r.rows_affected = 3;
        r.last_insert_rowid = 42;
        r.exec_us = 120;
        r.is_mutation = true;

        std::vector<uint8_t> out;
        EncodeQueryResult(out, r);

        auto dec = DecodeQueryResult(out);
        assert(dec.has_value());
        assert(dec->columns.empty());
        assert(dec->rows.empty());
        assert(dec->rows_affected == 3);
        assert(dec->last_insert_rowid == 42);
        assert(dec->exec_us == 120);
        assert(dec->is_mutation == true);
    }

    // SELECT result with rows
    {
        QueryResult r;
        r.columns = {"id", "name", "rating"};
        r.rows = {
            { int64_t(1), std::string("Alice"), 4.5 },
            { int64_t(2), std::string("Bob"), 5.0 },
            { int64_t(3), std::monostate{}, -1.0 }
        };
        r.rows_affected = 0;
        r.last_insert_rowid = 0;
        r.exec_us = 450;
        r.is_mutation = false;

        std::vector<uint8_t> out;
        EncodeQueryResult(out, r);

        auto dec = DecodeQueryResult(out);
        assert(dec.has_value());
        assert(dec->columns.size() == 3);
        assert(dec->columns[0] == "id");
        assert(dec->columns[1] == "name");
        assert(dec->columns[2] == "rating");
        assert(dec->rows.size() == 3);
        assert(std::get<int64_t>(dec->rows[0][0]) == 1);
        assert(std::get<std::string>(dec->rows[0][1]) == "Alice");
        assert(std::get<double>(dec->rows[0][2]) == 4.5);
        assert(std::holds_alternative<std::monostate>(dec->rows[2][1]));
        assert(dec->exec_us == 450);
        assert(!dec->is_mutation);
    }

    // Hostile input: forged column count
    {
        std::vector<uint8_t> forged = {0xFF, 0xFF, 0x00, 0x00}; // 65535 cols
        auto res = DecodeQueryResult(forged);
        assert(!res.has_value());
        assert(res.error().Code() == ErrorCode::ProtocolError);
    }

    // Trailing garbage in QueryResult
    {
        QueryResult r;
        std::vector<uint8_t> out;
        EncodeQueryResult(out, r);
        out.push_back(0xAA); // trailing garbage

        auto res = DecodeQueryResult(out);
        assert(!res.has_value());
        assert(res.error().Code() == ErrorCode::ProtocolError);
    }
}

int main() {
    std::println("=== Running Value Codec Tests ===");
    test_value_roundtrips();
    test_value_rejections();
    test_statement_codec();
    test_query_result_codec();
    std::println("=== All Value Codec Tests Passed! ===");
    return 0;
}
