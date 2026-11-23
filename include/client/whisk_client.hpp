#ifndef WHISKDB_CLIENT_WHISK_CLIENT_HPP
#define WHISKDB_CLIENT_WHISK_CLIENT_HPP

#include "common/types.hpp"
#include "errors.hpp"
#include "network/tcp_client.hpp"
#include "network/whisk_codec.hpp"
#include "sql/sql_types.hpp"
#include "sql/value_codec.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace database {

struct WhiskPublishResult {
    std::string module_name;
    int64_t version = 0;
    std::string sha256;
    std::vector<std::string> exported_funcs;
    uint64_t exec_us = 0;
};

struct WhiskCallResult {
    std::string result_json;
    uint64_t fuel_used = 0;
    uint64_t exec_us = 0;
};

struct WhiskListResult {
    std::vector<WhiskModuleSummary> modules;
    uint64_t exec_us = 0;
};

class WhiskClient {
public:
    WhiskClient();
    ~WhiskClient();

    WhiskClient(const WhiskClient&) = delete;
    WhiskClient& operator=(const WhiskClient&) = delete;
    WhiskClient(WhiskClient&&) = delete;
    WhiskClient& operator=(WhiskClient&&) = delete;

public:
    Result<void> Connect(const std::string& host, uint16_t port);
    Result<uint64_t> Ping(); /* round-trip microseconds */
    Result<QueryResult> Query(std::string_view sql, std::span<const Value> params = {}); /* server error status -> Result error with server message; QueryResult.exec_us filled from response */
    Result<WhiskPublishResult> PublishModule(std::string_view name, std::span<const uint8_t> wasm_bytes);
    Result<WhiskPublishResult> PublishModuleFromFile(std::string_view name, const std::filesystem::path& wasm_path);
    Result<WhiskCallResult> CallWhisk(std::string_view module_name, std::string_view func_name, std::string_view args_json = "{}");
    Result<WhiskListResult> ListModules();
    Result<std::string> Snapshot();
    Result<std::string> GetLogs(idx_t limit = LogsRequest::DEFAULT_LIMIT);
    void Close();
    void Disconnect() { Close(); }
    bool IsConnected() const;

private:
    TcpClient client_;
};

} // namespace database

namespace Database = database;

#endif // WHISKDB_CLIENT_WHISK_CLIENT_HPP
