#include "whisk_guest.hpp"

WHISK_INIT() {
    int64_t rc1 = whisk::Exec(
        "CREATE TABLE IF NOT EXISTS products ("
        "id INTEGER PRIMARY KEY, "
        "name TEXT NOT NULL, "
        "stock INTEGER NOT NULL CHECK(stock >= 0), "
        "price_cents INTEGER NOT NULL"
        ");");
    if (rc1 < 0) {
        return whisk::Error("failed to create products table");
    }

    int64_t rc2 = whisk::Exec(
        "CREATE TABLE IF NOT EXISTS orders ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "user_id INTEGER NOT NULL, "
        "product_id INTEGER NOT NULL, "
        "qty INTEGER NOT NULL, "
        "total_cents INTEGER NOT NULL"
        ");");
    if (rc2 < 0) {
        return whisk::Error("failed to create orders table");
    }

    return whisk::Ok("{\"status\":\"ok\"}");
}

WHISK_FUNC(create_product) {
    whisk::StrView args = WHISK_ARGS();

    int64_t id = 0;
    whisk::StrView name;
    int64_t stock = 0;
    int64_t price_cents = 0;

    if (!whisk::JsonGetInt(args, "id", id)) {
        return whisk::Error("missing id");
    }
    if (!whisk::JsonGetStr(args, "name", name) || name.empty()) {
        return whisk::Error("missing name");
    }
    if (!whisk::JsonGetInt(args, "stock", stock)) {
        return whisk::Error("missing stock");
    }
    if (!whisk::JsonGetInt(args, "price_cents", price_cents)) {
        return whisk::Error("missing price_cents");
    }
    if (stock < 0) {
        return whisk::Error("stock must be non-negative");
    }
    if (price_cents < 0) {
        return whisk::Error("price_cents must be non-negative");
    }

    whisk::Buf<512> params;
    params.append_char('[');
    params.append_int(id);
    params.append_char(',');
    params.append_json_str(name);
    params.append_char(',');
    params.append_int(stock);
    params.append_char(',');
    params.append_int(price_cents);
    params.append_char(']');

    int64_t affected = whisk::Exec(
        "INSERT INTO products (id, name, stock, price_cents) VALUES (?1, ?2, ?3, ?4);",
        params.view());
    if (affected != 1) {
        return whisk::Error("failed to create product");
    }

    whisk::Buf<512> resp;
    resp.append("{\"status\":\"created\",\"id\":");
    resp.append_int(id);
    resp.append(",\"name\":");
    resp.append_json_str(name);
    resp.append(",\"stock\":");
    resp.append_int(stock);
    resp.append(",\"price_cents\":");
    resp.append_int(price_cents);
    resp.append_char('}');

    return whisk::Ok(resp.view());
}

WHISK_FUNC(checkout_item) {
    whisk::StrView args = WHISK_ARGS();

    int64_t user_id = 0;
    int64_t product_id = 0;
    int64_t qty = 0;

    if (!whisk::JsonGetInt(args, "user_id", user_id) ||
        !whisk::JsonGetInt(args, "product_id", product_id) ||
        !whisk::JsonGetInt(args, "qty", qty)) {
        return whisk::Error("missing checkout parameters");
    }
    if (qty <= 0) {
        return whisk::Error("qty must be positive");
    }

    whisk::Buf<128> sel_params;
    sel_params.append_char('[');
    sel_params.append_int(product_id);
    sel_params.append_char(']');

    whisk::StrView q_res = whisk::Query(
        "SELECT stock, price_cents FROM products WHERE id = ?1;",
        sel_params.view());
    if (q_res.empty() || !whisk::QueryHasRows(q_res)) {
        return whisk::Error("product not found");
    }

    int64_t stock = 0;
    int64_t price_cents = 0;
    if (!whisk::QueryRowInt(q_res, 0, stock) || !whisk::QueryRowInt(q_res, 1, price_cents)) {
        return whisk::Error("failed to read product details");
    }
    if (stock < qty) {
        return whisk::Error("insufficient stock");
    }

    whisk::Buf<128> upd_params;
    upd_params.append_char('[');
    upd_params.append_int(qty);
    upd_params.append_char(',');
    upd_params.append_int(product_id);
    upd_params.append_char(']');

    int64_t updated = whisk::Exec(
        "UPDATE products SET stock = stock - ?1 WHERE id = ?2;",
        upd_params.view());
    if (updated != 1) {
        return whisk::Error("failed to decrement stock");
    }

    int64_t total_cents = qty * price_cents;

    whisk::Buf<128> ins_params;
    ins_params.append_char('[');
    ins_params.append_int(user_id);
    ins_params.append_char(',');
    ins_params.append_int(product_id);
    ins_params.append_char(',');
    ins_params.append_int(qty);
    ins_params.append_char(',');
    ins_params.append_int(total_cents);
    ins_params.append_char(']');

    int64_t inserted = whisk::Exec(
        "INSERT INTO orders (user_id, product_id, qty, total_cents) VALUES (?1, ?2, ?3, ?4);",
        ins_params.view());
    if (inserted != 1) {
        return whisk::Error("failed to insert order");
    }

    whisk::LogInfo("checkout completed");

    whisk::Buf<256> resp;
    resp.append("{\"status\":\"ordered\",\"user_id\":");
    resp.append_int(user_id);
    resp.append(",\"product_id\":");
    resp.append_int(product_id);
    resp.append(",\"qty\":");
    resp.append_int(qty);
    resp.append(",\"total_cents\":");
    resp.append_int(total_cents);
    resp.append_char('}');

    return whisk::Ok(resp.view());
}

WHISK_FUNC(restock) {
    whisk::StrView args = WHISK_ARGS();

    int64_t product_id = 0;
    int64_t qty = 0;

    if (!whisk::JsonGetInt(args, "product_id", product_id) ||
        !whisk::JsonGetInt(args, "qty", qty)) {
        return whisk::Error("missing restock parameters");
    }
    if (qty <= 0) {
        return whisk::Error("qty must be positive");
    }

    whisk::Buf<128> upd_params;
    upd_params.append_char('[');
    upd_params.append_int(qty);
    upd_params.append_char(',');
    upd_params.append_int(product_id);
    upd_params.append_char(']');

    int64_t updated = whisk::Exec(
        "UPDATE products SET stock = stock + ?1 WHERE id = ?2;",
        upd_params.view());
    if (updated != 1) {
        return whisk::Error("product not found");
    }

    whisk::Buf<128> sel_params;
    sel_params.append_char('[');
    sel_params.append_int(product_id);
    sel_params.append_char(']');

    whisk::StrView q_res = whisk::Query(
        "SELECT stock FROM products WHERE id = ?1;",
        sel_params.view());
    int64_t new_stock = 0;
    if (q_res.empty() || !whisk::QueryFirstInt(q_res, new_stock)) {
        return whisk::Error("failed to read updated stock");
    }

    whisk::Buf<256> resp;
    resp.append("{\"status\":\"restocked\",\"product_id\":");
    resp.append_int(product_id);
    resp.append(",\"stock\":");
    resp.append_int(new_stock);
    resp.append_char('}');

    return whisk::Ok(resp.view());
}
