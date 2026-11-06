#ifndef WHISKDB_CLIENT_WHISK_CLIENT_HPP
#define WHISKDB_CLIENT_WHISK_CLIENT_HPP

#include "common/types.hpp"
#include "errors.hpp"
#include "network/tcp_client.hpp"
#include "sql/sql_types.hpp"
#include "sql/value_codec.hpp"

#include <span>
#include <string>
#include <string_view>

namespace database {

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
    Result<std::string> Snapshot();
    Result<std::string> GetLogs();
    void Close();
    bool IsConnected() const;

private:
    TcpClient client_;
};

} // namespace database

#endif // WHISKDB_CLIENT_WHISK_CLIENT_HPP
