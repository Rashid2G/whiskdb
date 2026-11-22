#include "common/sha256.hpp"
#include "engine.hpp"
#include "errors.hpp"
#include "sql/catalog.hpp"
#include "sql/sql_engine.hpp"

#include <cassert>
#include <filesystem>
#include <format>
#include <print>
#include <string>
#include <unistd.h>
#include <vector>

using namespace database;

namespace {

std::filesystem::path MakeTempDir(std::string_view tag) {
    auto dir = std::filesystem::temp_directory_path() /
               std::format("whiskdb_test_catalog_{}_{}", static_cast<int>(::getpid()), tag);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void TestSha256NistVectors() {
    std::println("--- Test: SHA-256 NIST Vectors ---");

    // Vector 1: Empty string
    assert(Sha256Hex("") ==
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // Vector 2: "abc"
    assert(Sha256Hex("abc") ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // Vector 3: 56-byte multi-block string
    assert(Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // Vector 4: 1,000,000 repetitions of 'a'
    std::string million_a(1000000, 'a');
    assert(Sha256Hex(million_a) ==
           "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    // Test std::span<const uint8_t> overload
    std::vector<uint8_t> abc_bytes = {'a', 'b', 'c'};
    assert(Sha256Hex(abc_bytes) ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

void TestSystemCatalogCrudAndWalReplay() {
    std::println("--- Test: SystemCatalog CRUD & WAL Replay ---");

    auto sql_res = SqlEngine::Open();
    assert(sql_res.has_value());
    auto& sql = *sql_res.value();

    // Idempotent initialization
    assert(SystemCatalog::Initialize(sql).has_value());
    assert(SystemCatalog::Initialize(sql).has_value());

    // Empty module name rejected
    ModuleMetadata empty_name_mod;
    auto bad_upsert = SystemCatalog::UpsertModule(sql, empty_name_mod);
    assert(!bad_upsert.has_value());
    assert(bad_upsert.error().Code() == ErrorCode::InvalidArgument);

    std::vector<DecodedStatement> wal_log;

    // Module alpha v1
    ModuleMetadata mod_alpha;
    mod_alpha.name = "mod_alpha";
    mod_alpha.wasm_bytes = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    mod_alpha.exported_funcs = {"alloc", "run"};
    auto v1_res = SystemCatalog::UpsertModule(sql, mod_alpha, &wal_log);
    assert(v1_res.has_value());
    assert(*v1_res == 1);
    assert(wal_log.size() == 1);

    // Module alpha v2 (upgrade)
    mod_alpha.wasm_bytes.push_back(0x42);
    mod_alpha.exported_funcs = {"alloc", "run", "stop"};
    auto v2_res = SystemCatalog::UpsertModule(sql, mod_alpha, &wal_log);
    assert(v2_res.has_value());
    assert(*v2_res == 2);
    assert(wal_log.size() == 2);

    // Module alpha v3 (upgrade again)
    mod_alpha.wasm_bytes.push_back(0x99);
    mod_alpha.exported_funcs = {"alloc", "run", "stop", "inspect"};
    auto v3_res = SystemCatalog::UpsertModule(sql, mod_alpha, &wal_log);
    assert(v3_res.has_value());
    assert(*v3_res == 3);
    assert(wal_log.size() == 3);

    // Module beta v1
    ModuleMetadata mod_beta;
    mod_beta.name = "mod_beta";
    mod_beta.wasm_bytes = {0xde, 0xad, 0xbe, 0xef};
    mod_beta.exported_funcs = {"ping"};
    auto beta_v1 = SystemCatalog::UpsertModule(sql, mod_beta, &wal_log);
    assert(beta_v1.has_value());
    assert(*beta_v1 == 1);
    assert(wal_log.size() == 4);

    // GetModule verification
    auto get_alpha = SystemCatalog::GetModule(sql, "mod_alpha");
    assert(get_alpha.has_value() && get_alpha->has_value());
    const auto& meta_a = **get_alpha;
    assert(meta_a.name == "mod_alpha");
    assert(meta_a.version == 3);
    assert(meta_a.sha256 == Sha256Hex(mod_alpha.wasm_bytes));
    assert(meta_a.wasm_bytes == mod_alpha.wasm_bytes);
    assert(meta_a.exported_funcs.size() == 4);
    assert(meta_a.exported_funcs[0] == "alloc" && meta_a.exported_funcs[3] == "inspect");
    assert(meta_a.updated_at > 0);

    auto get_nonexistent = SystemCatalog::GetModule(sql, "nonexistent");
    assert(get_nonexistent.has_value() && !get_nonexistent->has_value());

    // ListModules ordering check (mod_alpha, mod_beta)
    auto list_res = SystemCatalog::ListModules(sql);
    assert(list_res.has_value());
    assert(list_res->size() == 2);
    assert((*list_res)[0].name == "mod_alpha");
    assert((*list_res)[1].name == "mod_beta");

    // RecordMigration
    auto mig1 = SystemCatalog::RecordMigration(sql, "mod_alpha", 1, "__init__", &wal_log);
    assert(mig1.has_value());
    auto mig2 = SystemCatalog::RecordMigration(sql, "mod_alpha", 2, "__update__", &wal_log);
    assert(mig2.has_value());
    assert(wal_log.size() == 6);

    // Check migration table contents
    auto mig_rows = sql.Execute("SELECT module_name, version, hook_name FROM _whisk_migrations ORDER BY id ASC;");
    assert(mig_rows.has_value());
    assert(mig_rows->rows.size() == 2);
    assert(std::get<std::string>(mig_rows->rows[0][0]) == "mod_alpha");
    assert(std::get<int64_t>(mig_rows->rows[0][1]) == 1);
    assert(std::get<std::string>(mig_rows->rows[0][2]) == "__init__");
    assert(std::get<std::string>(mig_rows->rows[1][2]) == "__update__");

    // RemoveModule
    auto rm_beta = SystemCatalog::RemoveModule(sql, "mod_beta", &wal_log);
    assert(rm_beta.has_value());
    assert(wal_log.size() == 7);

    auto get_beta_del = SystemCatalog::GetModule(sql, "mod_beta");
    assert(get_beta_del.has_value() && !get_beta_del->has_value());

    auto list_after_del = SystemCatalog::ListModules(sql);
    assert(list_after_del.has_value() && list_after_del->size() == 1);
    assert((*list_after_del)[0].name == "mod_alpha");

    // Replay WAL log into a fresh SqlEngine under CatalogWriteGuard
    auto fresh_sql_res = SqlEngine::Open();
    assert(fresh_sql_res.has_value());
    auto& fresh_sql = *fresh_sql_res.value();
    assert(SystemCatalog::Initialize(fresh_sql).has_value());

    {
        auto guard = fresh_sql.AcquireCatalogWriteGuard();
        for (const auto& entry : wal_log) {
            auto replay_res = fresh_sql.Execute(entry.sql, entry.params);
            assert(replay_res.has_value());
        }
    }

    auto fresh_alpha = SystemCatalog::GetModule(fresh_sql, "mod_alpha");
    assert(fresh_alpha.has_value() && fresh_alpha->has_value());
    assert((*fresh_alpha)->version == 3);
    assert((*fresh_alpha)->exported_funcs.size() == 4);

    auto fresh_beta = SystemCatalog::GetModule(fresh_sql, "mod_beta");
    assert(fresh_beta.has_value() && !fresh_beta->has_value());

    auto fresh_migs = fresh_sql.Execute("SELECT count(*) FROM _whisk_migrations;");
    assert(fresh_migs.has_value() && fresh_migs->rows.size() == 1);
    assert(std::get<int64_t>(fresh_migs->rows[0][0]) == 2);
}

void TestAuthorizerProtection() {
    std::println("--- Test: SQLite Authorizer Protection ---");

    auto sql_res = SqlEngine::Open();
    assert(sql_res.has_value());
    auto& sql = *sql_res.value();
    assert(SystemCatalog::Initialize(sql).has_value());

    // User attempts to modify catalog tables directly without guard
    auto drop_res = sql.Execute("DROP TABLE _whisk_modules;");
    assert(!drop_res.has_value());
    assert(drop_res.error().Code() == ErrorCode::AccessDenied);

    // Case-insensitive check
    auto drop_upper_res = sql.Execute("DROP TABLE _WHISK_MODULES;");
    assert(!drop_upper_res.has_value());
    assert(drop_upper_res.error().Code() == ErrorCode::AccessDenied);

    auto del_res = sql.Execute("DELETE FROM _whisk_modules;");
    assert(!del_res.has_value());
    assert(del_res.error().Code() == ErrorCode::AccessDenied);

    auto alter_res = sql.Execute("ALTER TABLE _whisk_modules ADD COLUMN extra_col INTEGER;");
    assert(!alter_res.has_value());
    assert(alter_res.error().Code() == ErrorCode::AccessDenied);

    auto upd_res = sql.Execute("UPDATE _whisk_modules SET version = 99;");
    assert(!upd_res.has_value());
    assert(upd_res.error().Code() == ErrorCode::AccessDenied);

    auto ins_res = sql.Execute("INSERT INTO _whisk_modules (name, version, sha256, wasm_bytes, exported_funcs, updated_at) "
                               "VALUES ('hacked', 1, 'sha', X'00', '[]', 0);");
    assert(!ins_res.has_value());
    assert(ins_res.error().Code() == ErrorCode::AccessDenied);

    auto idx_res = sql.Execute("CREATE INDEX idx_whisk_sha ON _whisk_modules(sha256);");
    assert(!idx_res.has_value());
    assert(idx_res.error().Code() == ErrorCode::AccessDenied);

    auto trg_res = sql.Execute("CREATE TRIGGER trg_test BEFORE INSERT ON _whisk_modules BEGIN SELECT 1; END;");
    assert(!trg_res.has_value());
    assert(trg_res.error().Code() == ErrorCode::AccessDenied);

    // Checks on _whisk_migrations
    auto drop_mig = sql.Execute("DROP TABLE _whisk_migrations;");
    assert(!drop_mig.has_value());
    assert(drop_mig.error().Code() == ErrorCode::AccessDenied);

    auto ins_mig = sql.Execute("INSERT INTO _whisk_migrations (module_name, version, hook_name, executed_at) VALUES ('x', 1, 'h', 0);");
    assert(!ins_mig.has_value());
    assert(ins_mig.error().Code() == ErrorCode::AccessDenied);

    // Read queries on _whisk_* MUST SUCCEED
    auto sel_mod = sql.Execute("SELECT name, version FROM _whisk_modules;");
    assert(sel_mod.has_value());

    auto sel_mig = sql.Execute("SELECT id, module_name FROM _whisk_migrations;");
    assert(sel_mig.has_value());

    // Upsert module legitimately via SystemCatalog
    ModuleMetadata legit;
    legit.name = "legit_module";
    legit.wasm_bytes = {0x01, 0x02};
    legit.exported_funcs = {"entry"};
    assert(SystemCatalog::UpsertModule(sql, legit).has_value());

    // Security Invariant: Statement cache bypass prevents cached privileged statement reuse!
    std::vector<Value> params{
        std::string("hacked2"),
        int64_t{10},
        std::string("sha"),
        Blob{0x00},
        std::string("[]"),
        int64_t{0}
    };
    auto re_insert = sql.Execute(
        "INSERT INTO _whisk_modules (name, version, sha256, wasm_bytes, exported_funcs, updated_at) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6) "
        "ON CONFLICT(name) DO UPDATE SET "
        "version = excluded.version, "
        "sha256 = excluded.sha256, "
        "wasm_bytes = excluded.wasm_bytes, "
        "exported_funcs = excluded.exported_funcs, "
        "updated_at = excluded.updated_at", params);
    assert(!re_insert.has_value());
    assert(re_insert.error().Code() == ErrorCode::AccessDenied);

    auto re_del = sql.Execute("DELETE FROM _whisk_modules WHERE name = 'legit_module';");
    assert(!re_del.has_value());
    assert(re_del.error().Code() == ErrorCode::AccessDenied);

    auto re_upd = sql.Execute("UPDATE _whisk_modules SET version = 999 WHERE name = 'legit_module';");
    assert(!re_upd.has_value());
    assert(re_upd.error().Code() == ErrorCode::AccessDenied);

    // Verify legitimate module is untouched
    auto meta_read = SystemCatalog::GetModule(sql, "legit_module");
    assert(meta_read.has_value() && meta_read->has_value());
    assert((*meta_read)->version == 1);
}

void TestAuthorizerSurvivesSerializeDeserialize() {
    std::println("--- Test: Authorizer Survives Serialize/Deserialize ---");

    auto sql1_res = SqlEngine::Open();
    assert(sql1_res.has_value());
    auto& sql1 = *sql1_res.value();
    assert(SystemCatalog::Initialize(sql1).has_value());

    ModuleMetadata mod;
    mod.name = "serialized_mod";
    mod.wasm_bytes = {0x11, 0x22};
    mod.exported_funcs = {"init"};
    assert(SystemCatalog::UpsertModule(sql1, mod).has_value());

    auto snap = sql1.Serialize();
    assert(snap.has_value());

    auto sql2_res = SqlEngine::Open();
    assert(sql2_res.has_value());
    auto& sql2 = *sql2_res.value();

    assert(sql2.Deserialize(snap.value()).has_value());

    // Authorizer must still be enforced on deserialized database
    auto drop_res = sql2.Execute("DROP TABLE _whisk_modules;");
    assert(!drop_res.has_value());
    assert(drop_res.error().Code() == ErrorCode::AccessDenied);

    auto del_res = sql2.Execute("DELETE FROM _whisk_modules WHERE name = 'serialized_mod';");
    assert(!del_res.has_value());
    assert(del_res.error().Code() == ErrorCode::AccessDenied);

    // Read works
    auto sel_res = sql2.Execute("SELECT name, version FROM _whisk_modules;");
    assert(sel_res.has_value() && sel_res->rows.size() == 1);
    assert(std::get<std::string>(sel_res->rows[0][0]) == "serialized_mod");
}

void TestDdlStatementCacheInvalidation() {
    std::println("--- Test: DDL StatementCache Invalidation ---");

    auto sql_res = SqlEngine::Open();
    assert(sql_res.has_value());
    auto& sql = *sql_res.value();

    // 1. CREATE TABLE invalidation
    assert(sql.Execute("CREATE TABLE ddl_test (id INTEGER PRIMARY KEY, v TEXT);").has_value());
    assert(sql.GetCacheStats().size == 0);

    // Insert rows
    assert(sql.Execute("INSERT INTO ddl_test VALUES (1, 'hello');").has_value());
    assert(sql.Execute("INSERT INTO ddl_test VALUES (2, 'world');").has_value());

    // Execute query to populate statement cache
    std::vector<Value> p1{int64_t{1}};
    auto q1 = sql.Execute("SELECT v FROM ddl_test WHERE id = ?1;", p1);
    assert(q1.has_value());
    std::vector<Value> p2{int64_t{2}};
    auto q2 = sql.Execute("SELECT v FROM ddl_test WHERE id = ?1;", p2);
    assert(q2.has_value());
    assert(sql.GetCacheStats().hits >= 1);
    assert(sql.GetCacheStats().size >= 1);

    // 2. ALTER TABLE invalidates cache
    assert(sql.Execute("ALTER TABLE ddl_test ADD COLUMN tag TEXT DEFAULT 'def';").has_value());
    assert(sql.GetCacheStats().size == 0);

    // Query new column works without schema staleness
    auto q3 = sql.Execute("SELECT v, tag FROM ddl_test WHERE id = 1;");
    assert(q3.has_value());
    assert(q3->columns.size() == 2);
    assert(std::get<std::string>(q3->rows[0][1]) == "def");

    // 3. DROP TABLE invalidates cache
    assert(sql.Execute("DROP TABLE ddl_test;").has_value());
    assert(sql.GetCacheStats().size == 0);

    // 4. Multi-statement ExecuteScript DDL invalidates cache
    assert(sql.ExecuteScript("CREATE TABLE script_t (val INT);").has_value());
    assert(sql.GetCacheStats().size == 0);

    assert(sql.Execute("INSERT INTO script_t VALUES (42);").has_value());
    assert(sql.Execute("SELECT val FROM script_t;").has_value());
    assert(sql.Execute("SELECT val FROM script_t;").has_value());
    assert(sql.GetCacheStats().size >= 1);

    assert(sql.ExecuteScript("DROP TABLE script_t;").has_value());
    assert(sql.GetCacheStats().size == 0);
}

void TestEngineOpenAndRecovery() {
    std::println("--- Test: Engine::Open & Snapshot/Recovery with Catalog ---");

    auto temp_dir = MakeTempDir("engine_cat");

    {
        EngineOptions opts;
        opts.data_dir = temp_dir;
        opts.durability = DurabilityMode::Strict;
        opts.host = "127.0.0.1";
        opts.port = 0;

        auto eng_res = Engine::Open(opts);
        assert(eng_res.has_value());
        auto& engine = *eng_res.value();

        // System catalog tables exist immediately upon Open
        auto sel_mod = engine.ExecuteSql("SELECT name, version FROM _whisk_modules;");
        assert(sel_mod.has_value());

        auto sel_mig = engine.ExecuteSql("SELECT id FROM _whisk_migrations;");
        assert(sel_mig.has_value());

        // Create user table and checkpoint
        assert(engine.ExecuteSql("CREATE TABLE records (id INT);").has_value());
        assert(engine.ExecuteSql("INSERT INTO records VALUES (100);").has_value());

        auto cp_res = engine.Checkpoint();
        assert(cp_res.has_value());
    }

    // Restart engine from snapshot
    {
        EngineOptions opts;
        opts.data_dir = temp_dir;
        opts.durability = DurabilityMode::Strict;
        opts.host = "127.0.0.1";
        opts.port = 0;

        auto eng_res = Engine::Open(opts);
        assert(eng_res.has_value());
        auto& engine = *eng_res.value();

        assert(engine.Recovery().snapshot_loaded);

        // Catalog tables must still be present and usable
        auto sel_mod = engine.ExecuteSql("SELECT name, version FROM _whisk_modules;");
        assert(sel_mod.has_value());

        auto sel_rec = engine.ExecuteSql("SELECT id FROM records;");
        assert(sel_rec.has_value() && sel_rec->rows.size() == 1);
        assert(std::get<int64_t>(sel_rec->rows[0][0]) == 100);
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
}

} // namespace

int main() {
    TestSha256NistVectors();
    TestSystemCatalogCrudAndWalReplay();
    TestAuthorizerProtection();
    TestAuthorizerSurvivesSerializeDeserialize();
    TestDdlStatementCacheInvalidation();
    TestEngineOpenAndRecovery();

    std::println("All System Catalog & SHA-256 tests passed successfully!");
    return 0;
}
