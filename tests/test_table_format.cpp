#include "client/table_format.hpp"
#include "sql/sql_types.hpp"

#include <cassert>
#include <print>
#include <string>
#include <vector>

using namespace database;

void TestEmptyResult() {
    QueryResult qr;
    qr.columns = {"id", "name"};
    qr.rows = {};
    qr.exec_us = 120; // 0.12 ms

    std::string table = FormatResultTable(qr);
    assert(table.find("+----+------+") != std::string::npos);
    assert(table.find("| id | name |") != std::string::npos);
    assert(table.find("0 row(s) in 0.12 ms\n") != std::string::npos);
}

void TestNullValues() {
    QueryResult qr;
    qr.columns = {"val"};
    qr.rows = {{std::monostate{}}};
    qr.exec_us = 50; // 0.05 ms

    std::string table = FormatResultTable(qr);
    assert(table.find("| NULL |") != std::string::npos);
    assert(table.find("1 row(s) in 0.05 ms\n") != std::string::npos);
}

void TestMixedTypesAndAlignment() {
    QueryResult qr;
    qr.columns = {"n", "i", "d", "t", "b"};
    qr.rows = {{
        std::monostate{},
        static_cast<int64_t>(42),
        3.14159,
        std::string("hello"),
        Blob{0xde, 0xad, 0xbe, 0xef}
    }};
    qr.exec_us = 1500; // 1.50 ms

    std::string table = FormatResultTable(qr);
    // NULL is text-aligned (left)
    assert(table.find("NULL") != std::string::npos);
    // 42 is right-aligned
    assert(table.find(" 42 ") != std::string::npos);
    // double is shortened
    assert(table.find("3.14159") != std::string::npos);
    // text
    assert(table.find("hello") != std::string::npos);
    // blob
    assert(table.find("x'deadbeef'") != std::string::npos);
    // footer
    assert(table.find("1 row(s) in 1.50 ms\n") != std::string::npos);
}

void TestWideAndLongCells() {
    QueryResult qr;
    qr.columns = {"long_text", "escaped_text", "long_blob"};
    std::string long_str(70, 'a');
    std::string expected_cap = std::string(57, 'a') + "...";
    std::string multi_line = "line1\nline2\rline3";
    Blob long_blob;
    for (uint8_t i = 0; i < 20; ++i) {
        long_blob.push_back(i);
    }
    qr.rows = {{long_str, multi_line, long_blob}};
    qr.exec_us = 200;

    std::string table = FormatResultTable(qr);
    // Long cell capped at 60 chars
    assert(table.find(expected_cap) != std::string::npos);
    assert(table.find(long_str) == std::string::npos);
    // Newlines escaped
    assert(table.find("line1\\nline2\\rline3") != std::string::npos);
    // Blob > 16 bytes truncated with ...
    assert(table.find("x'000102030405060708090a0b0c0d0e0f...'") != std::string::npos);
}

void TestUnicodeWidths() {
    QueryResult qr;
    // Header "AB" has 2 code points. "你好" has 2 code points (6 UTF-8 bytes).
    // The column width should be 2, so the border segment is +----+ (2 + 2 = 4 dashes).
    // If bytes were counted instead, column width would be 6, and border would have 8 dashes (+--------+).
    qr.columns = {"AB"};
    qr.rows = {{"你好"}, {"Café"}}; // "Café" is 4 code points, 5 bytes. Column width becomes 4.
    qr.exec_us = 300;

    std::string table = FormatResultTable(qr);
    // Column width is 4 (from Café). Border segment has 4 + 2 = 6 dashes (+------+).
    assert(table.find("+------+") != std::string::npos);
    assert(table.find("| AB   |") != std::string::npos);
    assert(table.find("| 你好   |") != std::string::npos);
    assert(table.find("| Café |") != std::string::npos);
}

void TestDmlFooter() {
    QueryResult qr;
    qr.columns = {};
    qr.rows = {};
    qr.rows_affected = 42;
    qr.last_insert_rowid = 1234;
    qr.exec_us = 2500; // 2.50 ms
    qr.is_mutation = true;

    std::string table = FormatResultTable(qr);
    assert(table == "OK, 42 row(s) affected (last insert rowid 1234) in 2.50 ms\n");
}

void TestSplitStatements() {
    // 1. Quotes containing ';'
    {
        std::string buf = "SELECT 'hello;world';";
        auto stmts = SplitStatements(buf);
        assert(stmts.size() == 1);
        assert(stmts[0] == "SELECT 'hello;world';");
        assert(buf.empty());
    }

    // 2. Escaped quotes
    {
        std::string buf = "SELECT 'it''s ; fine';";
        auto stmts = SplitStatements(buf);
        assert(stmts.size() == 1);
        assert(stmts[0] == "SELECT 'it''s ; fine';");
        assert(buf.empty());
    }

    // 3. Multiple statements per line
    {
        std::string buf = "SELECT 1; SELECT 2; SELECT 3;";
        auto stmts = SplitStatements(buf);
        assert(stmts.size() == 3);
        assert(stmts[0] == "SELECT 1;");
        assert(stmts[1] == "SELECT 2;");
        assert(stmts[2] == "SELECT 3;");
        assert(buf.empty());
    }

    // 4. Remainder retained
    {
        std::string buf = "SELECT 1; SELECT 2";
        auto stmts = SplitStatements(buf);
        assert(stmts.size() == 1);
        assert(stmts[0] == "SELECT 1;");
        assert(buf == "SELECT 2");
    }

    // 5. Whitespace-only input
    {
        std::string buf = "   \t\n  ";
        auto stmts = SplitStatements(buf);
        assert(stmts.empty());
    }
}

int main() {
    TestEmptyResult();
    TestNullValues();
    TestMixedTypesAndAlignment();
    TestWideAndLongCells();
    TestUnicodeWidths();
    TestDmlFooter();
    TestSplitStatements();

    std::println("test_table_format: OK");
    return 0;
}
