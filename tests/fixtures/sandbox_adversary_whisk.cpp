#include "whisk_guest.hpp"

namespace {

[[clang::noinline]] int32_t RecurseForever(int32_t depth) {
    if (depth < 0) {
        return 0;
    }
    volatile uint8_t frame[1024];
    frame[0] = static_cast<uint8_t>(depth & 0xFF);
    frame[1023] = static_cast<uint8_t>((depth >> 8) & 0xFF);
    return RecurseForever(depth + 1) + static_cast<int32_t>(frame[0]) + static_cast<int32_t>(frame[1023]);
}

} // namespace

WHISK_INIT() {
    int64_t rc = whisk::Exec(
        "CREATE TABLE IF NOT EXISTS safety_items (id INTEGER PRIMARY KEY, label TEXT NOT NULL)");
    if (rc < 0) {
        return whisk::Error("init failed");
    }
    return whisk::Ok("{\"init\":true}");
}

WHISK_FUNC(valid_insert) {
    whisk::StrView args = WHISK_ARGS();
    int64_t id = 0;
    whisk::StrView label;
    if (!whisk::JsonGetInt(args, "id", id) || !whisk::JsonGetStr(args, "label", label)) {
        return whisk::Error("invalid args");
    }

    whisk::Buf<256> params;
    params.append_char('[');
    params.append_int(id);
    params.append_char(',');
    params.append_json_str(label);
    params.append_char(']');

    int64_t rc = whisk::Exec("INSERT INTO safety_items (id, label) VALUES (?1, ?2)", params.view());
    if (rc < 0) {
        return whisk::Error("insert failed");
    }
    return whisk::Ok("{\"status\":\"ok\"}");
}

WHISK_FUNC(trap_after_insert) {
    (void)whisk::Exec("INSERT INTO safety_items (id, label) VALUES (1, 'dirty')");
    __builtin_trap();
}

WHISK_FUNC(infinite_loop_after_insert) {
    (void)whisk::Exec("INSERT INTO safety_items (id, label) VALUES (2, 'loop')");
    volatile int32_t x = 0;
    while (true) {
        x = x + 1;
    }
}

WHISK_FUNC(oob_pointer_attack) {
    int64_t exec_rc = whisk_db_exec(0x7FFFFFFF, 100, 0, 0);
    int32_t query_rc = whisk_db_query(0, 10, 0, 0, 0x7FFFFFFF, 64);

    whisk::Buf<128> out;
    out.append("{\"exec_rc\":");
    out.append_int(exec_rc);
    out.append(",\"query_rc\":");
    out.append_int(static_cast<int64_t>(query_rc));
    out.append_char('}');
    return whisk::Ok(out.view());
}

WHISK_FUNC(stack_overflow_recursion) {
    (void)whisk::Exec("INSERT INTO safety_items (id, label) VALUES (4, 'stack')");
    volatile int32_t sink = RecurseForever(1);
    return sink == 0 ? whisk::Ok("{\"status\":\"unreachable\"}") : whisk::Error("unreachable");
}

WHISK_FUNC(memory_bomb) {
    int32_t grow_rc = static_cast<int32_t>(__builtin_wasm_memory_grow(0, 1024));
    if (grow_rc != -1) {
        return whisk::Error("memory_grow unexpectedly succeeded");
    }
    return whisk::Ok("{\"grow_rc\":-1}");
}

WHISK_FUNC(escape_transaction_attack) {
    (void)whisk::Exec("INSERT INTO safety_items (id, label) VALUES (6, 'escape')");
    int64_t commit_rc = whisk::Exec("COMMIT");
    int64_t rollback_rc = whisk::Exec("ROLLBACK");
    int64_t drop_rc = whisk::Exec("DROP TABLE _whisk_modules");

    whisk::Buf<256> out;
    out.append("{\"commit_rc\":");
    out.append_int(commit_rc);
    out.append(",\"rollback_rc\":");
    out.append_int(rollback_rc);
    out.append(",\"drop_rc\":");
    out.append_int(drop_rc);
    out.append_char('}');
    return whisk::Error(out.view());
}
