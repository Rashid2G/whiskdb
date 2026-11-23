#include "client/whisk_client.hpp"

#include <chrono>
#include <fstream>
#include <limits>

namespace database {

namespace {

Error MakeServerError(const Response& resp) {
    std::string msg(reinterpret_cast<const char*>(resp.payload.data()), resp.payload.size());
    if (msg.empty()) {
        msg = std::string(ToString(resp.status));
    }
    return Error(resp.status, std::move(msg));
}

} // namespace

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
        return std::unexpected(MakeServerError(*resp_res));
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
        return std::unexpected(MakeServerError(*resp_res));
    }

    auto qr_res = DecodeQueryResult(resp_res->payload);
    if (!qr_res) {
        return MakeError(qr_res.error().Code(), qr_res.error().Message());
    }

    qr_res->exec_us = resp_res->exec_us;
    return *qr_res;
}

Result<WhiskPublishResult> WhiskClient::PublishModule(std::string_view name, std::span<const uint8_t> wasm_bytes) {
    WhiskPublishRequest pub_req;
    pub_req.module_name = std::string(name);
    pub_req.wasm_bytes.assign(wasm_bytes.begin(), wasm_bytes.end());

    Request req;
    req.opcode = Opcode::WhiskPublish;
    auto enc_res = EncodeWhiskPublishRequest(req.payload, pub_req);
    if (!enc_res) {
        return MakeError(enc_res.error().Code(), enc_res.error().Message());
    }

    auto resp_res = client_.Send(req);
    if (!resp_res) {
        return MakeError(resp_res.error().Code(), resp_res.error().Message());
    }

    if (resp_res->status != ErrorCode::Ok) {
        return std::unexpected(MakeServerError(*resp_res));
    }

    auto dec_res = DecodeWhiskPublishResponse(resp_res->payload);
    if (!dec_res) {
        return MakeError(dec_res.error().Code(), dec_res.error().Message());
    }

    WhiskPublishResult out;
    out.module_name = std::move(dec_res->module_name);
    out.version = dec_res->version;
    out.sha256 = std::move(dec_res->sha256);
    out.exported_funcs = std::move(dec_res->exported_funcs);
    out.exec_us = resp_res->exec_us;
    return out;
}

Result<WhiskPublishResult> WhiskClient::PublishModuleFromFile(
    std::string_view name,
    const std::filesystem::path& wasm_path) {
    std::error_code ec;
    if (!std::filesystem::exists(wasm_path, ec) || ec) {
        return MAKE_ERROR(ErrorCode::NotFound, "WASM file not found: {}", wasm_path.string());
    }

    std::ifstream file(wasm_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return MAKE_ERROR(ErrorCode::IoError, "Failed to open WASM file: {}", wasm_path.string());
    }

    auto file_size = file.tellg();
    if (file_size <= 0) {
        return MAKE_ERROR(ErrorCode::IoError, "WASM file is empty or unreadable: {}", wasm_path.string());
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(file_size));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), file_size)) {
        return MAKE_ERROR(ErrorCode::IoError, "Failed to read WASM file: {}", wasm_path.string());
    }

    return PublishModule(name, bytes);
}

Result<WhiskCallResult> WhiskClient::CallWhisk(
    std::string_view module_name,
    std::string_view func_name,
    std::string_view args_json) {
    WhiskCallRequest call_req;
    call_req.module_name = std::string(module_name);
    call_req.func_name = std::string(func_name);
    call_req.args_json = std::string(args_json);

    Request req;
    req.opcode = Opcode::WhiskCall;
    auto enc_res = EncodeWhiskCallRequest(req.payload, call_req);
    if (!enc_res) {
        return MakeError(enc_res.error().Code(), enc_res.error().Message());
    }

    auto resp_res = client_.Send(req);
    if (!resp_res) {
        return MakeError(resp_res.error().Code(), resp_res.error().Message());
    }

    if (resp_res->status != ErrorCode::Ok) {
        return std::unexpected(MakeServerError(*resp_res));
    }

    auto dec_res = DecodeWhiskCallResponse(resp_res->payload);
    if (!dec_res) {
        return MakeError(dec_res.error().Code(), dec_res.error().Message());
    }

    WhiskCallResult out;
    out.result_json = std::move(dec_res->result_json);
    out.fuel_used = dec_res->fuel_used;
    out.exec_us = resp_res->exec_us;
    return out;
}

Result<WhiskListResult> WhiskClient::ListModules() {
    Request req;
    req.opcode = Opcode::WhiskList;
    req.payload.clear();

    auto resp_res = client_.Send(req);
    if (!resp_res) {
        return MakeError(resp_res.error().Code(), resp_res.error().Message());
    }

    if (resp_res->status != ErrorCode::Ok) {
        return std::unexpected(MakeServerError(*resp_res));
    }

    auto dec_res = DecodeWhiskListResponse(resp_res->payload);
    if (!dec_res) {
        return MakeError(dec_res.error().Code(), dec_res.error().Message());
    }

    WhiskListResult out;
    out.modules = std::move(dec_res->modules);
    out.exec_us = resp_res->exec_us;
    return out;
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
        return std::unexpected(MakeServerError(*resp_res));
    }

    return std::string(reinterpret_cast<const char*>(resp_res->payload.data()), resp_res->payload.size());
}

Result<std::string> WhiskClient::GetLogs(idx_t limit) {
    LogsRequest logs_req;
    if (limit > static_cast<idx_t>(std::numeric_limits<uint32_t>::max())) {
        logs_req.limit = std::numeric_limits<uint32_t>::max();
    } else {
        logs_req.limit = static_cast<uint32_t>(limit);
    }

    Request req;
    req.opcode = Opcode::Logs;
    EncodeLogsRequest(req.payload, logs_req);

    auto resp_res = client_.Send(req);
    if (!resp_res) {
        return MakeError(resp_res.error().Code(), resp_res.error().Message());
    }

    if (resp_res->status != ErrorCode::Ok) {
        return std::unexpected(MakeServerError(*resp_res));
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
