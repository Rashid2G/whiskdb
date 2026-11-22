#include "whisk_guest.hpp"

WHISK_INIT() {
    int64_t rc = whisk::Exec("CREATE TABLE IF NOT EXISTS counters (name TEXT PRIMARY KEY, val INTEGER NOT NULL);");
    if (rc < 0) {
        return whisk::Error("failed to create counters table");
    }
    return whisk::Ok("{\"status\":\"ok\"}");
}

WHISK_FUNC(increment) {
    whisk::StrView args = WHISK_ARGS();
    whisk::StrView name_slice;
    whisk::StrView name = "default";
    if (whisk::JsonGetStr(args, "name", name_slice) && !name_slice.empty()) {
        name = name_slice;
    }
    int64_t delta = whisk::JsonGetIntOr(args, "delta", 1);

    // Build parameterized query for UPSERT
    whisk::Buf<256> params;
    params.append_char('[');
    params.append_json_str(name);
    params.append_char(',');
    params.append_int(delta);
    params.append_char(']');

    int64_t affected = whisk::Exec(
        "INSERT INTO counters (name, val) VALUES (?1, ?2) ON CONFLICT(name) DO UPDATE SET val = val + excluded.val;",
        params.view());
    if (affected < 0) {
        return whisk::Error("failed to upsert counter");
    }

    // Query current counter value
    whisk::Buf<256> q_params;
    q_params.append_char('[');
    q_params.append_json_str(name);
    q_params.append_char(']');

    whisk::StrView q_res = whisk::Query("SELECT val FROM counters WHERE name = ?1;", q_params.view());
    if (q_res.empty() || !whisk::QueryHasRows(q_res)) {
        return whisk::Error("failed to query counter value");
    }

    int64_t val = 0;
    if (!whisk::QueryFirstInt(q_res, val)) {
        return whisk::Error("failed to parse counter value");
    }

    whisk::LogInfo("counter incremented");

    whisk::Buf<256> resp;
    resp.append("{\"name\":");
    resp.append_json_str(name);
    resp.append(",\"val\":");
    resp.append_int(val);
    resp.append_char('}');

    return whisk::Ok(resp.view());
}

WHISK_FUNC(get) {
    whisk::StrView args = WHISK_ARGS();
    whisk::StrView name_slice;
    whisk::StrView name = "default";
    if (whisk::JsonGetStr(args, "name", name_slice) && !name_slice.empty()) {
        name = name_slice;
    }

    whisk::Buf<256> q_params;
    q_params.append_char('[');
    q_params.append_json_str(name);
    q_params.append_char(']');

    whisk::StrView q_res = whisk::Query("SELECT val FROM counters WHERE name = ?1;", q_params.view());
    if (q_res.empty() || !whisk::QueryHasRows(q_res)) {
        return whisk::Error("counter not found");
    }

    int64_t val = 0;
    if (!whisk::QueryFirstInt(q_res, val)) {
        return whisk::Error("failed to parse counter value");
    }

    whisk::Buf<256> resp;
    resp.append("{\"name\":");
    resp.append_json_str(name);
    resp.append(",\"val\":");
    resp.append_int(val);
    resp.append_char('}');

    return whisk::Ok(resp.view());
}
