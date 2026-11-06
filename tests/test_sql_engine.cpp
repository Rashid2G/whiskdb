#include "sql/sql_engine.hpp"
#include "sql/transaction_guard.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <print>
#include <thread>
#include <vector>

using namespace database;

void TestDdlDmlJoinIndex() {
    std::println("--- Test 1: DDL/INSERT/SELECT/UPDATE/DELETE/JOIN/CREATE INDEX ---");
    auto engine_res = SqlEngine::Open();
    assert(engine_res.has_value());
    auto& engine = *engine_res.value();

    // DDL
    auto ddl1 = engine.Execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);");
    assert(ddl1.has_value() && ddl1->is_mutation);

    auto ddl2 = engine.Execute("CREATE TABLE orders (id INTEGER PRIMARY KEY, user_id INTEGER, amount REAL);");
    assert(ddl2.has_value() && ddl2->is_mutation);

    // CREATE INDEX
    auto idx = engine.Execute("CREATE INDEX idx_orders_user ON orders(user_id);");
    assert(idx.has_value() && idx->is_mutation);

    // INSERT
    auto ins1 = engine.Execute("INSERT INTO users (id, name) VALUES (1, 'Alice');");
    assert(ins1.has_value() && ins1->rows_affected == 1 && ins1->last_insert_rowid == 1);

    auto ins2 = engine.Execute("INSERT INTO users (id, name) VALUES (2, 'Bob');");
    assert(ins2.has_value() && ins2->rows_affected == 1 && ins2->last_insert_rowid == 2);

    auto ins3 = engine.Execute("INSERT INTO orders (id, user_id, amount) VALUES (10, 1, 99.50);");
    assert(ins3.has_value() && ins3->rows_affected == 1 && ins3->last_insert_rowid == 10);

    auto ins4 = engine.Execute("INSERT INTO orders (id, user_id, amount) VALUES (20, 2, 149.99);");
    assert(ins4.has_value() && ins4->rows_affected == 1 && ins4->last_insert_rowid == 20);

    // SELECT with JOIN
    auto sel = engine.Execute("SELECT u.name, o.amount FROM users u JOIN orders o ON u.id = o.user_id ORDER BY o.id;");
    assert(sel.has_value());
    assert(sel->columns.size() == 2 && sel->columns[0] == "name" && sel->columns[1] == "amount");
    assert(sel->rows.size() == 2);
    assert(std::get<std::string>(sel->rows[0][0]) == "Alice");
    assert(std::abs(std::get<double>(sel->rows[0][1]) - 99.50) < 1e-6);
    assert(std::get<std::string>(sel->rows[1][0]) == "Bob");
    assert(std::abs(std::get<double>(sel->rows[1][1]) - 149.99) < 1e-6);

    // UPDATE
    auto upd = engine.Execute("UPDATE users SET name = 'Alice Cooper' WHERE id = 1;");
    assert(upd.has_value() && upd->rows_affected == 1 && upd->is_mutation);

    // Verify update
    auto sel_upd = engine.Execute("SELECT name FROM users WHERE id = 1;");
    assert(sel_upd.has_value() && sel_upd->rows.size() == 1);
    assert(std::get<std::string>(sel_upd->rows[0][0]) == "Alice Cooper");

    // DELETE
    auto del = engine.Execute("DELETE FROM orders WHERE user_id = 1;");
    assert(del.has_value() && del->rows_affected == 1 && del->is_mutation);

    auto sel_del = engine.Execute("SELECT COUNT(*) FROM orders WHERE user_id = 1;");
    assert(sel_del.has_value() && sel_del->rows.size() == 1);
    assert(std::get<int64_t>(sel_del->rows[0][0]) == 0);

    std::println("  PASSED");
}

void TestValueTypesRoundTrip() {
    std::println("--- Test 2: All 5 Value Types Round Trip incl. Blobs with NUL and NULL ---");
    auto engine_res = SqlEngine::Open();
    assert(engine_res.has_value());
    auto& engine = *engine_res.value();

    auto create = engine.Execute("CREATE TABLE types_test (c_null TEXT, c_int INTEGER, c_real REAL, c_text TEXT, c_blob BLOB);");
    assert(create.has_value());

    Blob test_blob = {0x00, 0x12, 0x00, 0xFE, 0x00, 0xAA, 0x00};
    std::vector<Value> params = {
        std::monostate{},
        static_cast<int64_t>(-9223372036854775807LL),
        3.141592653589793,
        std::string("hello\0world", 11),
        test_blob
    };

    auto ins = engine.Execute("INSERT INTO types_test VALUES (?1, ?2, ?3, ?4, ?5);", params);
    assert(ins.has_value());

    auto sel = engine.Execute("SELECT c_null, c_int, c_real, c_text, c_blob FROM types_test;");
    assert(sel.has_value());
    assert(sel->rows.size() == 1);

    const auto& row = sel->rows[0];
    assert(std::holds_alternative<std::monostate>(row[0]));
    assert(std::holds_alternative<int64_t>(row[1]) && std::get<int64_t>(row[1]) == -9223372036854775807LL);
    assert(std::holds_alternative<double>(row[2]) && std::abs(std::get<double>(row[2]) - 3.141592653589793) < 1e-9);
    assert(std::holds_alternative<std::string>(row[3]) && std::get<std::string>(row[3]).size() == 11);
    assert(std::get<std::string>(row[3]) == std::string("hello\0world", 11));
    assert(std::holds_alternative<Blob>(row[4]));
    const auto& fetched_blob = std::get<Blob>(row[4]);
    assert(fetched_blob.size() == test_blob.size());
    assert(fetched_blob == test_blob);

    std::println("  PASSED");
}

void TestParamBinding() {
    std::println("--- Test 3: Param Binding ---");
    auto engine_res = SqlEngine::Open();
    assert(engine_res.has_value());
    auto& engine = *engine_res.value();

    std::vector<Value> params = {
        static_cast<int64_t>(42),
        std::string("param_test"),
        2.71828
    };

    auto res = engine.Execute("SELECT ?1 AS num, ?2 AS str, ?3 AS flt;", params);
    assert(res.has_value());
    assert(res->columns.size() == 3);
    assert(res->columns[0] == "num" && res->columns[1] == "str" && res->columns[2] == "flt");
    assert(res->rows.size() == 1);
    assert(std::get<int64_t>(res->rows[0][0]) == 42);
    assert(std::get<std::string>(res->rows[0][1]) == "param_test");
    assert(std::abs(std::get<double>(res->rows[0][2]) - 2.71828) < 1e-5);

    std::println("  PASSED");
}

void TestRowsAffectedAndLastInsertRowid() {
    std::println("--- Test 4: rows_affected / last_insert_rowid ---");
    auto engine_res = SqlEngine::Open();
    assert(engine_res.has_value());
    auto& engine = *engine_res.value();

    auto cr = engine.Execute("CREATE TABLE items (id INTEGER PRIMARY KEY, v INT);");
    assert(cr.has_value() && cr->rows_affected == 0);

    auto ins1 = engine.Execute("INSERT INTO items VALUES (101, 1);");
    assert(ins1.has_value() && ins1->rows_affected == 1 && ins1->last_insert_rowid == 101);

    auto ins2 = engine.Execute("INSERT INTO items VALUES (102, 1);");
    assert(ins2.has_value() && ins2->rows_affected == 1 && ins2->last_insert_rowid == 102);

    auto upd = engine.Execute("UPDATE items SET v = 2 WHERE v = 1;");
    assert(upd.has_value() && upd->rows_affected == 2);

    auto del = engine.Execute("DELETE FROM items WHERE v = 2;");
    assert(del.has_value() && del->rows_affected == 2);

    auto sel = engine.Execute("SELECT * FROM items;");
    assert(sel.has_value() && sel->rows_affected == 0 && sel->last_insert_rowid == 0);

    std::println("  PASSED");
}

void TestIsMutation() {
    std::println("--- Test 5: is_mutation true / false ---");
    auto engine_res = SqlEngine::Open();
    assert(engine_res.has_value());
    auto& engine = *engine_res.value();

    auto r_ddl = engine.Execute("CREATE TABLE t (x INT);");
    assert(r_ddl.has_value() && r_ddl->is_mutation == true);

    auto r_ins = engine.Execute("INSERT INTO t VALUES (1);");
    assert(r_ins.has_value() && r_ins->is_mutation == true);

    auto r_upd = engine.Execute("UPDATE t SET x = 2;");
    assert(r_upd.has_value() && r_upd->is_mutation == true);

    auto r_idx = engine.Execute("CREATE INDEX idx_t_x ON t(x);");
    assert(r_idx.has_value() && r_idx->is_mutation == true);

    auto r_del = engine.Execute("DELETE FROM t;");
    assert(r_del.has_value() && r_del->is_mutation == true);

    auto r_sel = engine.Execute("SELECT * FROM t;");
    assert(r_sel.has_value() && r_sel->is_mutation == false);

    auto r_sel_lit = engine.Execute("SELECT 1;");
    assert(r_sel_lit.has_value() && r_sel_lit->is_mutation == false);

    std::println("  PASSED");
}

void TestErrorPaths() {
    std::println("--- Test 6: Error Paths ---");
    auto engine_res = SqlEngine::Open();
    assert(engine_res.has_value());
    auto& engine = *engine_res.value();

    // Syntax error
    auto err_syn = engine.Execute("SELECT FROM WHERE INVALID;");
    assert(!err_syn.has_value());
    assert(err_syn.error().Code() == ErrorCode::InvalidArgument);
    assert(!err_syn.error().Message().empty());

    // Missing table
    auto err_tbl = engine.Execute("SELECT * FROM nonexistent_table;");
    assert(!err_tbl.has_value());
    assert(err_tbl.error().Code() == ErrorCode::TableDoesNotExist);
    assert(err_tbl.error().Message().find("no such table") != std::string::npos);

    // Duplicate PK constraint
    auto cr = engine.Execute("CREATE TABLE pk_test (id INT PRIMARY KEY);");
    assert(cr.has_value());
    auto ins_ok = engine.Execute("INSERT INTO pk_test VALUES (1);");
    assert(ins_ok.has_value());
    auto ins_dup = engine.Execute("INSERT INTO pk_test VALUES (1);");
    assert(!ins_dup.has_value());
    assert(ins_dup.error().Code() == ErrorCode::AlreadyExists);
    assert(!ins_dup.error().Message().empty());

    // Two statements in one Execute
    auto err_two = engine.Execute("SELECT 1; SELECT 2;");
    assert(!err_two.has_value());
    assert(err_two.error().Code() == ErrorCode::InvalidArgument);

    auto err_two_nospace = engine.Execute("SELECT 1;SELECT 2;");
    assert(!err_two_nospace.has_value());
    assert(err_two_nospace.error().Code() == ErrorCode::InvalidArgument);

    // Empty SQL
    auto err_empty = engine.Execute("");
    assert(!err_empty.has_value());
    assert(err_empty.error().Code() == ErrorCode::InvalidArgument);

    auto err_space = engine.Execute("   \t\n  ");
    assert(!err_space.has_value());
    assert(err_space.error().Code() == ErrorCode::InvalidArgument);

    std::println("  PASSED");
}

void TestStatementCacheAndLruEviction() {
    std::println("--- Test 7: Statement Cache Hits & LRU Eviction ---");

    // Test repeated SQL cache hits
    {
        auto engine_res = SqlEngine::Open();
        assert(engine_res.has_value());
        auto& engine = *engine_res.value();

        auto stats0 = engine.GetCacheStats();
        assert(stats0.hits == 0 && stats0.misses == 0 && stats0.size == 0);

        auto r1 = engine.Execute("SELECT 100;");
        assert(r1.has_value());
        auto stats1 = engine.GetCacheStats();
        assert(stats1.hits == 0 && stats1.misses == 1 && stats1.size == 1);

        auto r2 = engine.Execute("SELECT 100;");
        assert(r2.has_value());
        auto stats2 = engine.GetCacheStats();
        assert(stats2.hits == 1 && stats2.misses == 1 && stats2.size == 1);

        auto r3 = engine.Execute("SELECT 100;");
        assert(r3.has_value());
        auto stats3 = engine.GetCacheStats();
        assert(stats3.hits == 2 && stats3.misses == 1 && stats3.size == 1);

        auto r4 = engine.Execute("SELECT 200;");
        assert(r4.has_value());
        auto stats4 = engine.GetCacheStats();
        assert(stats4.hits == 2 && stats4.misses == 2 && stats4.size == 2);

        auto r5 = engine.Execute("SELECT 200;");
        assert(r5.has_value());
        auto stats5 = engine.GetCacheStats();
        assert(stats5.hits == 3 && stats5.misses == 2 && stats5.size == 2);
    }

    // Test LRU eviction with small capacity = 2
    {
        auto engine_res = SqlEngine::Open(2);
        assert(engine_res.has_value());
        auto& engine = *engine_res.value();

        // 1. Execute SQL A -> Miss (1), Size 1: [A]
        (void)engine.Execute("SELECT 'A';");
        auto s1 = engine.GetCacheStats();
        assert(s1.misses == 1 && s1.hits == 0 && s1.size == 1);

        // 2. Execute SQL B -> Miss (2), Size 2: [B, A]
        (void)engine.Execute("SELECT 'B';");
        auto s2 = engine.GetCacheStats();
        assert(s2.misses == 2 && s2.hits == 0 && s2.size == 2);

        // 3. Execute SQL A -> Hit (1), Size 2: [A, B] (A promoted to MRU)
        (void)engine.Execute("SELECT 'A';");
        auto s3 = engine.GetCacheStats();
        assert(s3.misses == 2 && s3.hits == 1 && s3.size == 2);

        // 4. Execute SQL C -> Miss (3), Size 2: [C, A] (B is evicted)
        (void)engine.Execute("SELECT 'C';");
        auto s4 = engine.GetCacheStats();
        assert(s4.misses == 3 && s4.hits == 1 && s4.size == 2);

        // 5. Execute SQL A -> Hit (2): [A, C]
        (void)engine.Execute("SELECT 'A';");
        auto s5 = engine.GetCacheStats();
        assert(s5.misses == 3 && s5.hits == 2 && s5.size == 2);

        // 6. Execute SQL B -> Miss (4): B was evicted! [B, A] (C evicted)
        (void)engine.Execute("SELECT 'B';");
        auto s6 = engine.GetCacheStats();
        assert(s6.misses == 4 && s6.hits == 2 && s6.size == 2);
    }

    std::println("  PASSED");
}

void TestTransactionGuard() {
    std::println("--- Test 8: TransactionGuard (Commit, Rollback, Drop, Nesting) ---");
    auto engine_res = SqlEngine::Open();
    assert(engine_res.has_value());
    auto& engine = *engine_res.value();

    auto cr = engine.Execute("CREATE TABLE tx_test (id INT PRIMARY KEY, val TEXT);");
    assert(cr.has_value());

    // 1. Commit
    {
        auto tx = TransactionGuard::Begin(engine);
        assert(tx.has_value());
        auto ins = engine.Execute("INSERT INTO tx_test VALUES (1, 'one');");
        assert(ins.has_value());
        auto c = tx->Commit();
        assert(c.has_value());
    }
    auto sel1 = engine.Execute("SELECT COUNT(*) FROM tx_test WHERE id = 1;");
    assert(sel1.has_value() && std::get<int64_t>(sel1->rows[0][0]) == 1);

    // 2. Explicit Rollback
    {
        auto tx = TransactionGuard::Begin(engine);
        assert(tx.has_value());
        auto ins = engine.Execute("INSERT INTO tx_test VALUES (2, 'two');");
        assert(ins.has_value());
        auto rb = tx->Rollback();
        assert(rb.has_value());
    }
    auto sel2 = engine.Execute("SELECT COUNT(*) FROM tx_test WHERE id = 2;");
    assert(sel2.has_value() && std::get<int64_t>(sel2->rows[0][0]) == 0);

    // 3. Drop-without-commit automatic rollback
    {
        auto tx = TransactionGuard::Begin(engine);
        assert(tx.has_value());
        auto ins = engine.Execute("INSERT INTO tx_test VALUES (3, 'three');");
        assert(ins.has_value());
        // tx destructor called without commit or rollback
    }
    auto sel3 = engine.Execute("SELECT COUNT(*) FROM tx_test WHERE id = 3;");
    assert(sel3.has_value() && std::get<int64_t>(sel3->rows[0][0]) == 0);

    // 4. Nested rollback of inner only
    {
        auto outer = TransactionGuard::Begin(engine);
        assert(outer.has_value());

        auto ins_outer = engine.Execute("INSERT INTO tx_test VALUES (4, 'outer');");
        assert(ins_outer.has_value());

        {
            auto inner = TransactionGuard::Begin(engine);
            assert(inner.has_value());

            auto ins_inner = engine.Execute("INSERT INTO tx_test VALUES (5, 'inner');");
            assert(ins_inner.has_value());

            auto rb_inner = inner->Rollback();
            assert(rb_inner.has_value());
        }

        auto commit_outer = outer->Commit();
        assert(commit_outer.has_value());
    }
    // id 4 must exist, id 5 must NOT exist
    auto sel4 = engine.Execute("SELECT COUNT(*) FROM tx_test WHERE id = 4;");
    assert(sel4.has_value() && std::get<int64_t>(sel4->rows[0][0]) == 1);

    auto sel5 = engine.Execute("SELECT COUNT(*) FROM tx_test WHERE id = 5;");
    assert(sel5.has_value() && std::get<int64_t>(sel5->rows[0][0]) == 0);

    std::println("  PASSED");
}

void TestSerializeDeserialize() {
    std::println("--- Test 9: Serialize -> Deserialize Snapshot ---");
    auto engine1_res = SqlEngine::Open();
    assert(engine1_res.has_value());
    auto& engine1 = *engine1_res.value();

    assert(engine1.Execute("CREATE TABLE snap (id INT, v TEXT);").has_value());
    assert(engine1.Execute("INSERT INTO snap VALUES (10, 'alpha');").has_value());
    assert(engine1.Execute("INSERT INTO snap VALUES (20, 'beta');").has_value());

    auto snap_res = engine1.Serialize();
    assert(snap_res.has_value() && !snap_res.value().empty());
    const auto& bytes = snap_res.value();

    auto engine2_res = SqlEngine::Open();
    assert(engine2_res.has_value());
    auto& engine2 = *engine2_res.value();

    auto deser_res = engine2.Deserialize(bytes);
    assert(deser_res.has_value());

    auto sel = engine2.Execute("SELECT id, v FROM snap ORDER BY id;");
    assert(sel.has_value());
    assert(sel->rows.size() == 2);
    assert(std::get<int64_t>(sel->rows[0][0]) == 10 && std::get<std::string>(sel->rows[0][1]) == "alpha");
    assert(std::get<int64_t>(sel->rows[1][0]) == 20 && std::get<std::string>(sel->rows[1][1]) == "beta");

    // Ensure engine2 is writable after deserialization
    auto ins2 = engine2.Execute("INSERT INTO snap VALUES (30, 'gamma');");
    assert(ins2.has_value() && ins2->rows_affected == 1);

    std::println("  PASSED");
}

void TestConcurrentExecute() {
    std::println("--- Test 10: Concurrent Execute (4 threads x 500 inserts) ---");
    auto engine_res = SqlEngine::Open();
    assert(engine_res.has_value());
    auto& engine = *engine_res.value();

    assert(engine.Execute("CREATE TABLE concurrent_test (id INTEGER PRIMARY KEY AUTOINCREMENT, t_id INT, val INT);").has_value());

    constexpr int thread_count = 4;
    constexpr int per_thread_inserts = 500;
    std::vector<std::jthread> threads;
    threads.reserve(thread_count);

    for (int t = 0; t < thread_count; ++t) {
        threads.emplace_back([&engine, t]() {
            for (int i = 0; i < per_thread_inserts; ++i) {
                std::vector<Value> params = {static_cast<int64_t>(t), static_cast<int64_t>(i)};
                auto res = engine.Execute("INSERT INTO concurrent_test (t_id, val) VALUES (?1, ?2);", params);
                assert(res.has_value());
            }
        });
    }

    // Wait for all threads to finish
    threads.clear();

    auto count_res = engine.Execute("SELECT COUNT(*) FROM concurrent_test;");
    assert(count_res.has_value() && count_res->rows.size() == 1);
    int64_t total = std::get<int64_t>(count_res->rows[0][0]);
    assert(total == thread_count * per_thread_inserts);

    std::println("  PASSED (total rows = {})", total);
}

void TestPerfSanity() {
    std::println("--- Test 11: Performance Sanity (50k cached INSERTs) ---");
    auto engine_res = SqlEngine::Open();
    assert(engine_res.has_value());
    auto& engine = *engine_res.value();

    assert(engine.Execute("CREATE TABLE perf_bench (id INTEGER PRIMARY KEY, v INT);").has_value());

    constexpr int total_inserts = 50'000;

    auto start = std::chrono::high_resolution_clock::now();

    {
        auto guard = TransactionGuard::Begin(engine);
        assert(guard.has_value());

        for (int i = 0; i < total_inserts; ++i) {
            std::vector<Value> params = {static_cast<int64_t>(i), static_cast<int64_t>(i * 3)};
            auto res = engine.Execute("INSERT INTO perf_bench VALUES (?1, ?2);", params);
            assert(res.has_value());
        }

        auto commit_res = guard->Commit();
        assert(commit_res.has_value());
    }

    auto end = std::chrono::high_resolution_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
    double ops_per_sec = (static_cast<double>(total_inserts) / elapsed_ms) * 1000.0;

    std::println("  Executed {} cached inserts in {:.2f} ms ({:.0f} ops/sec)",
                 total_inserts, elapsed_ms, ops_per_sec);

    // Sanity check: must complete under 5 seconds
    assert(elapsed_ms < 5000.0);

    auto sel = engine.Execute("SELECT COUNT(*) FROM perf_bench;");
    assert(sel.has_value() && std::get<int64_t>(sel->rows[0][0]) == total_inserts);

    std::println("  PASSED");
}

int main() {
    std::println("=========================================");
    std::println("Running WhiskDB SqlEngine Test Suite");
    std::println("=========================================");

    TestDdlDmlJoinIndex();
    TestValueTypesRoundTrip();
    TestParamBinding();
    TestRowsAffectedAndLastInsertRowid();
    TestIsMutation();
    TestErrorPaths();
    TestStatementCacheAndLruEviction();
    TestTransactionGuard();
    TestSerializeDeserialize();
    TestConcurrentExecute();
    TestPerfSanity();

    std::println("\n=========================================");
    std::println("ALL SQL ENGINE TESTS PASSED SUCCESSFULLY!");
    std::println("=========================================");
    return 0;
}
