#include "common/types.hpp"
#include "errors.hpp"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

#include "client/table_format.hpp"
#include "client/whisk_client.hpp"

using namespace database;

namespace {

void PrintUsage(std::FILE* out) {
    std::println(out, "Usage: whisk [--host H] [--port P] <command> [args]");
    std::println(out, "");
    std::println(out, "Commands:");
    std::println(out, "  repl              Start interactive REPL (default)");
    std::println(out, "  ping              Ping server and print latency");
    std::println(out, "  sql \"<query>\"     Execute SQL statement and display result table");
    std::println(out, "  logs              Print server execution logs");
    std::println(out, "  snapshot          Trigger server snapshot");
    std::println(out, "");
    std::println(out, "Options:");
    std::println(out, "  --host H          Server hostname or IP (default: 127.0.0.1)");
    std::println(out, "  --port P          Server port (default: 8080)");
    std::println(out, "  -h, --help        Show this help message");
}

std::optional<uint16_t> ParsePort(std::string_view s) {
    if (s.empty()) {
        return std::nullopt;
    }
    uint32_t val = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        val = val * 10 + static_cast<uint32_t>(c - '0');
        if (val > 65535) {
            return std::nullopt;
        }
    }
    if (val == 0) {
        return std::nullopt;
    }
    return static_cast<uint16_t>(val);
}

void PrintReplError(std::string_view msg) {
    if (isatty(STDOUT_FILENO)) {
        std::println("\033[31merror: {}\033[0m", msg);
    } else {
        std::println("error: {}", msg);
    }
}

int RunRepl(WhiskClient& client) {
    std::string buffer;
    std::string line;
    const bool is_stdin_tty = (isatty(STDIN_FILENO) != 0);

    while (true) {
        bool is_continuation = !buffer.empty();
        if (is_stdin_tty) {
            std::print("{}", is_continuation ? "   ...> " : "whisk> ");
            std::fflush(stdout);
        }

        if (!std::getline(std::cin, line)) {
            if (is_stdin_tty) {
                std::println("");
            }
            break;
        }

        // Check for dot commands if not in continuation
        if (!is_continuation) {
            std::string_view sv = line;
            while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
                sv.remove_prefix(1);
            }
            while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
                sv.remove_suffix(1);
            }

            if (sv.starts_with('.')) {
                if (sv == ".quit" || sv == ".exit") {
                    break;
                }
                if (sv == ".help") {
                    std::println("Commands:");
                    std::println("  .help       Show this help message");
                    std::println("  .ping       Ping the server and measure round-trip time");
                    std::println("  .logs       Print server logs");
                    std::println("  .snapshot   Trigger server snapshot");
                    std::println("  .quit       Exit the REPL");
                    std::println("  .exit       Exit the REPL");
                    continue;
                }
                if (sv == ".ping") {
                    auto ping_res = client.Ping();
                    if (!ping_res) {
                        PrintReplError(ping_res.error().Message().empty()
                                           ? ToString(ping_res.error().Code())
                                           : ping_res.error().Message());
                    } else {
                        std::println("PONG in {} us", *ping_res);
                    }
                    continue;
                }
                if (sv == ".logs") {
                    auto logs_res = client.GetLogs();
                    if (!logs_res) {
                        PrintReplError(logs_res.error().Message().empty()
                                           ? ToString(logs_res.error().Code())
                                           : logs_res.error().Message());
                    } else {
                        std::print("{}", *logs_res);
                        if (!logs_res->empty() && logs_res->back() != '\n') {
                            std::println("");
                        }
                    }
                    continue;
                }
                if (sv == ".snapshot") {
                    auto snap_res = client.Snapshot();
                    if (!snap_res) {
                        PrintReplError(snap_res.error().Message().empty()
                                           ? ToString(snap_res.error().Code())
                                           : snap_res.error().Message());
                    } else {
                        std::println("{}", *snap_res);
                    }
                    continue;
                }
                PrintReplError(std::format("unknown command '{}'", sv));
                continue;
            }
        }

        if (buffer.empty()) {
            buffer = line;
        } else {
            buffer += "\n";
            buffer += line;
        }

        auto statements = SplitStatements(buffer);
        for (const auto& stmt : statements) {
            auto start = std::chrono::steady_clock::now();
            auto res = client.Query(stmt);
            auto end = std::chrono::steady_clock::now();

            if (!res) {
                PrintReplError(res.error().Message().empty()
                                   ? ToString(res.error().Code())
                                   : res.error().Message());
            } else {
                std::print("{}", FormatResultTable(*res));
                auto dur_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
                std::println("(round trip {:.2f} ms)", static_cast<double>(dur_us) / 1000.0);
            }
        }

        bool has_non_ws = false;
        for (char c : buffer) {
            if (!std::isspace(static_cast<unsigned char>(c))) {
                has_non_ws = true;
                break;
            }
        }
        if (!has_non_ws) {
            buffer.clear();
        }
    }

    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    uint16_t port = 8080;
    std::string command;
    std::vector<std::string> command_args;

    for (idx_t i = 1; i < static_cast<idx_t>(argc); ++i) {
        std::string_view arg = argv[i];
        if (arg == "--host") {
            if (i + 1 >= static_cast<idx_t>(argc)) {
                std::println(stderr, "error: --host requires an argument");
                PrintUsage(stderr);
                return 2;
            }
            host = argv[++i];
        } else if (arg.starts_with("--host=")) {
            host = std::string(arg.substr(7));
        } else if (arg == "--port") {
            if (i + 1 >= static_cast<idx_t>(argc)) {
                std::println(stderr, "error: --port requires an argument");
                PrintUsage(stderr);
                return 2;
            }
            std::string_view port_str = argv[++i];
            auto port_opt = ParsePort(port_str);
            if (!port_opt) {
                std::println(stderr, "error: invalid port '{}'", port_str);
                PrintUsage(stderr);
                return 2;
            }
            port = *port_opt;
        } else if (arg.starts_with("--port=")) {
            std::string_view port_str = arg.substr(7);
            auto port_opt = ParsePort(port_str);
            if (!port_opt) {
                std::println(stderr, "error: invalid port '{}'", port_str);
                PrintUsage(stderr);
                return 2;
            }
            port = *port_opt;
        } else if (arg == "-h" || arg == "--help") {
            PrintUsage(stdout);
            return 0;
        } else if (arg.starts_with("-")) {
            std::println(stderr, "error: unknown option '{}'", arg);
            PrintUsage(stderr);
            return 2;
        } else {
            command = std::string(arg);
            for (idx_t j = i + 1; j < static_cast<idx_t>(argc); ++j) {
                command_args.emplace_back(argv[j]);
            }
            break;
        }
    }

    if (command.empty()) {
        command = "repl";
    }

    WhiskClient client;
    auto conn_res = client.Connect(host, port);
    if (!conn_res) {
        std::println(stderr, "error: failed to connect to {}:{}: {}", host, port,
                     conn_res.error().Message().empty() ? ToString(conn_res.error().Code())
                                                        : conn_res.error().Message());
        return 1;
    }

    if (command == "ping") {
        auto ping_res = client.Ping();
        if (!ping_res) {
            std::println(stderr, "error: {}",
                         ping_res.error().Message().empty() ? ToString(ping_res.error().Code())
                                                            : ping_res.error().Message());
            return 1;
        }
        std::println("PONG in {} us", *ping_res);
        return 0;
    }

    if (command == "sql") {
        if (command_args.empty()) {
            std::println(stderr, "error: missing SQL query");
            PrintUsage(stderr);
            return 2;
        }
        std::string query;
        for (idx_t k = 0; k < command_args.size(); ++k) {
            if (k > 0) {
                query += " ";
            }
            query += command_args[k];
        }
        auto query_res = client.Query(query);
        if (!query_res) {
            std::println(stderr, "error: {}",
                         query_res.error().Message().empty() ? ToString(query_res.error().Code())
                                                             : query_res.error().Message());
            return 1;
        }
        std::print("{}", FormatResultTable(*query_res));
        return 0;
    }

    if (command == "logs") {
        auto logs_res = client.GetLogs();
        if (!logs_res) {
            std::println(stderr, "error: {}",
                         logs_res.error().Message().empty() ? ToString(logs_res.error().Code())
                                                            : logs_res.error().Message());
            return 1;
        }
        std::print("{}", *logs_res);
        if (!logs_res->empty() && logs_res->back() != '\n') {
            std::println("");
        }
        return 0;
    }

    if (command == "snapshot") {
        auto snap_res = client.Snapshot();
        if (!snap_res) {
            std::println(stderr, "error: {}",
                         snap_res.error().Message().empty() ? ToString(snap_res.error().Code())
                                                            : snap_res.error().Message());
            return 1;
        }
        std::println("{}", *snap_res);
        return 0;
    }

    if (command == "repl") {
        return RunRepl(client);
    }

    std::println(stderr, "error: unknown command '{}'", command);
    PrintUsage(stderr);
    return 2;
}
