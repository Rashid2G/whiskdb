#include "whisk_guest.hpp"

WHISK_INIT() {
    int64_t rc1 = whisk::Exec(
        "CREATE TABLE IF NOT EXISTS accounts ("
        "id INTEGER PRIMARY KEY, "
        "owner TEXT NOT NULL, "
        "balance INTEGER NOT NULL CHECK(balance >= 0)"
        ");");
    if (rc1 < 0) {
        return whisk::Error("failed to create accounts table");
    }

    int64_t rc2 = whisk::Exec(
        "CREATE TABLE IF NOT EXISTS transfers ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "from_id INTEGER NOT NULL, "
        "to_id INTEGER NOT NULL, "
        "amount INTEGER NOT NULL, "
        "created_at INTEGER DEFAULT 0"
        ");");
    if (rc2 < 0) {
        return whisk::Error("failed to create transfers table");
    }

    return whisk::Ok("{\"status\":\"ok\"}");
}

WHISK_FUNC(create_account) {
    whisk::StrView args = WHISK_ARGS();

    int64_t id = 0;
    whisk::StrView owner;
    int64_t balance = 0;

    if (!whisk::JsonGetInt(args, "id", id)) {
        return whisk::Error("missing id");
    }
    if (!whisk::JsonGetStr(args, "owner", owner) || owner.empty()) {
        return whisk::Error("missing owner");
    }
    if (!whisk::JsonGetInt(args, "balance", balance)) {
        return whisk::Error("missing balance");
    }
    if (balance < 0) {
        return whisk::Error("balance must be non-negative");
    }

    whisk::Buf<512> params;
    params.append_char('[');
    params.append_int(id);
    params.append_char(',');
    params.append_json_str(owner);
    params.append_char(',');
    params.append_int(balance);
    params.append_char(']');

    int64_t affected = whisk::Exec(
        "INSERT INTO accounts (id, owner, balance) VALUES (?1, ?2, ?3);",
        params.view());
    if (affected != 1) {
        return whisk::Error("failed to create account");
    }

    whisk::Buf<512> resp;
    resp.append("{\"status\":\"created\",\"id\":");
    resp.append_int(id);
    resp.append(",\"owner\":");
    resp.append_json_str(owner);
    resp.append(",\"balance\":");
    resp.append_int(balance);
    resp.append_char('}');

    return whisk::Ok(resp.view());
}

WHISK_FUNC(transfer_funds) {
    whisk::StrView args = WHISK_ARGS();

    int64_t from_id = 0;
    int64_t to_id = 0;
    int64_t amount = 0;

    if (!whisk::JsonGetInt(args, "from", from_id) ||
        !whisk::JsonGetInt(args, "to", to_id) ||
        !whisk::JsonGetInt(args, "amount", amount)) {
        return whisk::Error("missing transfer parameters");
    }
    if (amount <= 0) {
        return whisk::Error("amount must be positive");
    }
    if (from_id == to_id) {
        return whisk::Error("cannot transfer to self");
    }

    // 1. Check sender balance
    whisk::Buf<128> sel_params;
    sel_params.append_char('[');
    sel_params.append_int(from_id);
    sel_params.append_char(']');

    whisk::StrView q_res = whisk::Query(
        "SELECT balance FROM accounts WHERE id = ?1;",
        sel_params.view());
    if (q_res.empty() || !whisk::QueryHasRows(q_res)) {
        return whisk::Error("sender not found");
    }

    int64_t sender_balance = 0;
    if (!whisk::QueryFirstInt(q_res, sender_balance)) {
        return whisk::Error("failed to read sender balance");
    }
    if (sender_balance < amount) {
        return whisk::Error("insufficient funds");
    }

    // 2. Debit sender
    whisk::Buf<128> debit_params;
    debit_params.append_char('[');
    debit_params.append_int(amount);
    debit_params.append_char(',');
    debit_params.append_int(from_id);
    debit_params.append_char(']');

    int64_t debited = whisk::Exec(
        "UPDATE accounts SET balance = balance - ?1 WHERE id = ?2;",
        debit_params.view());
    if (debited != 1) {
        return whisk::Error("failed to debit sender");
    }

    // 3. Credit receiver (failure here automatically rolls back sender debit)
    whisk::Buf<128> credit_params;
    credit_params.append_char('[');
    credit_params.append_int(amount);
    credit_params.append_char(',');
    credit_params.append_int(to_id);
    credit_params.append_char(']');

    int64_t credited = whisk::Exec(
        "UPDATE accounts SET balance = balance + ?1 WHERE id = ?2;",
        credit_params.view());
    if (credited != 1) {
        return whisk::Error("recipient not found");
    }

    // 4. Insert transfer audit log
    whisk::Buf<128> log_params;
    log_params.append_char('[');
    log_params.append_int(from_id);
    log_params.append_char(',');
    log_params.append_int(to_id);
    log_params.append_char(',');
    log_params.append_int(amount);
    log_params.append_char(']');

    int64_t logged = whisk::Exec(
        "INSERT INTO transfers (from_id, to_id, amount) VALUES (?1, ?2, ?3);",
        log_params.view());
    if (logged != 1) {
        return whisk::Error("failed to record transfer");
    }

    whisk::LogInfo("transfer committed");

    whisk::Buf<256> resp;
    resp.append("{\"status\":\"committed\",\"from\":");
    resp.append_int(from_id);
    resp.append(",\"to\":");
    resp.append_int(to_id);
    resp.append(",\"amount\":");
    resp.append_int(amount);
    resp.append_char('}');

    return whisk::Ok(resp.view());
}

WHISK_FUNC(get_balance) {
    whisk::StrView args = WHISK_ARGS();

    int64_t id = 0;
    if (!whisk::JsonGetInt(args, "id", id)) {
        return whisk::Error("missing id");
    }

    whisk::Buf<128> params;
    params.append_char('[');
    params.append_int(id);
    params.append_char(']');

    whisk::StrView q_res = whisk::Query(
        "SELECT balance FROM accounts WHERE id = ?1;",
        params.view());
    if (q_res.empty() || !whisk::QueryHasRows(q_res)) {
        return whisk::Error("account not found");
    }

    int64_t balance = 0;
    if (!whisk::QueryFirstInt(q_res, balance)) {
        return whisk::Error("failed to parse balance");
    }

    whisk::Buf<256> resp;
    resp.append("{\"id\":");
    resp.append_int(id);
    resp.append(",\"balance\":");
    resp.append_int(balance);
    resp.append_char('}');

    return whisk::Ok(resp.view());
}

WHISK_FUNC(buggy_transfer_trap) {
    whisk::StrView args = WHISK_ARGS();
    int64_t from_id = whisk::JsonGetIntOr(args, "from", 1);
    int64_t amount = whisk::JsonGetIntOr(args, "amount", 100);

    whisk::Buf<128> debit_params;
    debit_params.append_char('[');
    debit_params.append_int(amount);
    debit_params.append_char(',');
    debit_params.append_int(from_id);
    debit_params.append_char(']');

    (void)whisk::Exec(
        "UPDATE accounts SET balance = balance - ?1 WHERE id = ?2;",
        debit_params.view());

    __builtin_trap();
}

WHISK_FUNC(infinite_loop) {
    whisk::StrView args = WHISK_ARGS();
    int64_t from_id = whisk::JsonGetIntOr(args, "from", 1);
    int64_t amount = whisk::JsonGetIntOr(args, "amount", 100);

    whisk::Buf<128> debit_params;
    debit_params.append_char('[');
    debit_params.append_int(amount);
    debit_params.append_char(',');
    debit_params.append_int(from_id);
    debit_params.append_char(']');

    (void)whisk::Exec(
        "UPDATE accounts SET balance = balance - ?1 WHERE id = ?2;",
        debit_params.view());

    volatile int x = 0;
    while (true) {
        x = x + 1;
    }
    return whisk::Ok("{}");
}
