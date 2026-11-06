#include "client/whisk_client.hpp"

#include <chrono>

namespace database {

WhiskClient::WhiskClient() = default;
WhiskClient::~WhiskClient() = default;

Result<void> WhiskClient::Connect(const std::string& host, uint16_t port) {
    return client_.Connect(host, port);
}

Result<uint64_t> WhiskClient::Ping() {
    auto start = std::chrono::steady_clock::now();

    Request req;
    req.opcode = Opcode::Ping;
    req.payload.clear();

    auto resp_res = client_.Send(req);
    if (!resp_res) {
        return MakeError(resp_res.error().Code(), resp_res.error().Message());
    }

    if (resp_res->status != ErrorCode::Ok) {
        std::string msg(reinterpret_cast<const char*>(resp_res->payload.data()), resp_res->payload.size());
        if (msg.empty()) {
            msg = std::string(ToString(resp_res->status));
        }
        return MakeError(resp_res->status, msg);
    }

    auto end = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

Result<QueryResult> WhiskClient::Query(std::string_view sql, std::span<const Value> params) {
    std::vector<uint8_t> payload;
    EncodeStatement(payload, sql, params);

    Request req;
    req.opcode = Opcode::SqlQuery;
    req.payload = std::move(payload);

    auto resp_res = client_.Send(req);
    if (!resp_res) {
        return MakeError(resp_res.error().Code(), resp_res.error().Message());
    }

    if (resp_res->status != ErrorCode::Ok) {
        std::string msg(reinterpret_cast<const char*>(resp_res->payload.data()), resp_res->payload.size());
        if (msg.empty()) {
            msg = std::string(ToString(resp_res->status));
        }
        return MakeError(resp_res->status, msg);
    }

    auto qr_res = DecodeQueryResult(resp_res->payload);
    if (!qr_res) {
        return MakeError(qr_res.error().Code(), qr_res.error().Message());
    }

    qr_res->exec_us = resp_res->exec_us;
    return *qr_res;
}

Result<std::string> WhiskClient::Snapshot() {
    Request req;
    req.opcode = Opcode::Snapshot;
    req.payload.clear();

    auto resp_res = client_.Send(req);
    if (!resp_res) {
        return MakeError(resp_res.error().Code(), resp_res.error().Message());
    }

    if (resp_res->status != ErrorCode::Ok) {
        std::string msg(reinterpret_cast<const char*>(resp_res->payload.data()), resp_res->payload.size());
        if (msg.empty()) {
            msg = std::string(ToString(resp_res->status));
        }
        return MakeError(resp_res->status, msg);
    }

    return std::string(reinterpret_cast<const char*>(resp_res->payload.data()), resp_res->payload.size());
}

Result<std::string> WhiskClient::GetLogs() {
    Request req;
    req.opcode = Opcode::Logs;
    req.payload.clear();

    auto resp_res = client_.Send(req);
    if (!resp_res) {
        return MakeError(resp_res.error().Code(), resp_res.error().Message());
    }

    if (resp_res->status != ErrorCode::Ok) {
        std::string msg(reinterpret_cast<const char*>(resp_res->payload.data()), resp_res->payload.size());
        if (msg.empty()) {
            msg = std::string(ToString(resp_res->status));
        }
        return MakeError(resp_res->status, msg);
    }

    return std::string(reinterpret_cast<const char*>(resp_res->payload.data()), resp_res->payload.size());
}

void WhiskClient::Close() {
    client_.Close();
}

bool WhiskClient::IsConnected() const {
    return client_.IsConnected();
}

} // namespace database
