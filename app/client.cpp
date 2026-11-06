#include "errors.hpp"
#include "logger.hpp"
#include "tcp_client.hpp"

#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    uint16_t port = 8080;

    if (argc > 1) {
        host = argv[1];
    }
    if (argc > 2) {
        port = static_cast<uint16_t>(std::stoi(argv[2]));
    }

    try {
        LOG_INFO("Connecting to {}:{}...", host, port);
        Database::TcpClient client(host, port);
        client.connect();
        LOG_INFO("Connected! Demonstrating persistent TCP connection with length-prefix framing...");

        std::vector<std::string> queries = {
            "PING",
            "SET user:1 \"Rashid\"",
            "GET user:1",
            "SELECT * FROM graduation_project",
            "QUIT"
        };

        for (size_t i = 0; i < queries.size(); ++i) {
            const auto& q = queries[i];
            LOG_INFO("Request #{}: sending '{}' ({} bytes)", i + 1, q, q.size());

            std::string response = client.send_request(q);

            LOG_INFO("Response #{}: received '{}' ({} bytes)", i + 1, response, response.size());
        }

        LOG_INFO("Persistent connection test completed successfully over 1 single socket connection!");
        client.disconnect();
    } catch (...) {
        Database::handle_top_level_exception();
        return 1;
    }

    return 0;
}
