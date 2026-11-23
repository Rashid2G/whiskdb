#include "common/types.hpp"
#include "errors.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <format>
#include <iostream>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include "client/table_format.hpp"
#include "client/whisk_client.hpp"

using namespace database;

namespace {

constexpr idx_t SHA256_SHORT_LEN = 12;

void PrintUsage(std::FILE* out) {
    std::println(out, "Usage: whisk [--host H] [--port P] [--json] <command> [args]");
    std::println(out, "");
    std::println(out, "Commands:");
    std::println(out, "  repl                                      Start interactive REPL (default)");
    std::println(out, "  ping                                      Ping server and print latency");
    std::println(out, "  sql \"<query>\"                             Execute SQL statement and display result table");
    std::println(out, "  publish <module_name> <file.wasm>         Publish a WebAssembly Whisk module");
    std::println(out, "  call <module> <func> [json_args]          Invoke a Whisk function (default args: \"{{}}\")");
    std::println(out, "  list                                      List deployed Whisk modules");
    std::println(out, "  logs [-n <limit> | --lines <limit>]       Print server execution logs (default: 200)");
    std::println(out, "  snapshot                                  Trigger server snapshot");
    std::println(out, "");
    std::println(out, "Options:");
    std::println(out, "  --host H                                  Server hostname or IP (default: 127.0.0.1)");
    std::println(out, "  --port P                                  Server port (default: 8080)");
    std::println(out, "  --json                                    Output machine-readable JSON");
    std::println(out, "  -h, --help                                Show this help message");
}

std::string_view ErrorText(const Error& err) {
    return err.Message().empty() ? ToString(err.Code()) : err.Message();
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

std::optional<idx_t> ParseLogLimit(std::string_view s) {
    if (s.empty()) {
        return std::nullopt;
    }
    uint64_t val = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        val = val * 10 + static_cast<uint64_t>(c - '0');
        if (val > LogsRequest::MAX_LIMIT) {
            return std::nullopt;
        }
    }
    if (val < LogsRequest::MIN_LIMIT) {
        return std::nullopt;
    }
    return static_cast<idx_t>(val);
}

std::string ShortSha256(std::string_view sha) {
    if (sha.size() > SHA256_SHORT_LEN) {
        return std::string(sha.substr(0, SHA256_SHORT_LEN));
    }
    return std::string(sha);
}

std::string JoinStrings(const std::vector<std::string>& items, std::string_view sep) {
    std::string out;
    for (idx_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            out += sep;
        }
        out += items[i];
    }
    return out;
}

std::string FormatJsonStringArray(const std::vector<std::string>& items) {
    std::string out = "[";
    for (idx_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            out += ',';
        }
        out += '"';
        out += EscapeJsonString(items[i]);
        out += '"';
    }
    out += ']';
    return out;
}

bool LooksLikeJsonValue(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    if (s.empty()) {
        return false;
    }
    char c = s.front();
    return c == '{' || c == '[' || c == '"' || c == 't' || c == 'f' || c == 'n' || c == '-' ||
           (c >= '0' && c <= '9');
}

void PrintPublishResult(const WhiskPublishResult& res, bool as_json) {
    if (as_json) {
        std::println("{{\"module_name\":\"{}\",\"version\":{},\"sha256\":\"{}\",\"exported_funcs\":{},\"exec_us\":{}}}",
                     EscapeJsonString(res.module_name),
                     res.version,
                     EscapeJsonString(res.sha256),
                     FormatJsonStringArray(res.exported_funcs),
                     res.exec_us);
        return;
    }
    std::string funcs = res.exported_funcs.empty() ? "(none)" : JoinStrings(res.exported_funcs, ", ");
    std::println("Published module '{}' (v{}, sha256 {})",
                 res.module_name,
                 res.version,
                 ShortSha256(res.sha256));
    std::println("Functions: {}", funcs);
    std::println("Run Time: {} us", res.exec_us);
}

void PrintCallResult(const WhiskCallResult& res, bool as_json) {
    if (as_json) {
        std::string result_field;
        if (res.result_json.empty()) {
            result_field = "null";
        } else if (LooksLikeJsonValue(res.result_json)) {
            result_field = res.result_json;
        } else {
            result_field = std::format("\"{}\"", EscapeJsonString(res.result_json));
        }
        std::println("{{\"result\":{},\"fuel_used\":{},\"exec_us\":{}}}",
                     result_field,
                     res.fuel_used,
                     res.exec_us);
        return;
    }
    std::println("{}", res.result_json);
    std::println("Run Time: {} us | Fuel: {}", res.exec_us, res.fuel_used);
}

void PrintListResult(const WhiskListResult& res, bool as_json) {
    if (as_json) {
        std::string out = "{\"modules\":[";
        for (idx_t i = 0; i < res.modules.size(); ++i) {
            if (i > 0) {
                out += ',';
            }
            const auto& m = res.modules[i];
            out += std::format("{{\"name\":\"{}\",\"version\":{},\"sha256\":\"{}\",\"exported_funcs\":{},\"updated_at\":{}}}",
                               EscapeJsonString(m.name),
                               m.version,
                               EscapeJsonString(m.sha256),
                               FormatJsonStringArray(m.exported_funcs),
                               m.updated_at);
        }
        out += std::format("],\"exec_us\":{}}}", res.exec_us);
        std::println("{}", out);
        return;
    }

    QueryResult qr;
    qr.columns = {"name", "version", "sha256", "functions"};
    qr.exec_us = res.exec_us;
    qr.rows.reserve(res.modules.size());
    for (const auto& m : res.modules) {
        std::vector<Value> row;
        row.reserve(4);
        row.emplace_back(m.name);
        row.emplace_back(std::format("v{}", m.version));
        row.emplace_back(ShortSha256(m.sha256));
        row.emplace_back(JoinStrings(m.exported_funcs, ", "));
        qr.rows.push_back(std::move(row));
    }
    std::print("{}", FormatResultTable(qr));
}

void PrintReplError(std::string_view msg) {
    if (isatty(STDOUT_FILENO)) {
        std::println("\033[31merror: {}\033[0m", msg);
    } else {
        std::println("error: {}", msg);
    }
}

std::string_view TrimView(std::string_view sv) {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
        sv.remove_suffix(1);
    }
    return sv;
}

std::string_view NextToken(std::string_view& sv) {
    sv = TrimView(sv);
    idx_t end = 0;
    while (end < sv.size() && !std::isspace(static_cast<unsigned char>(sv[end]))) {
        ++end;
    }
    std::string_view tok = sv.substr(0, end);
    sv.remove_prefix(end);
    sv = TrimView(sv);
    return tok;
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
            std::string_view sv = TrimView(line);

            if (sv.starts_with('.')) {
                std::string_view rest = sv;
                std::string_view cmd = NextToken(rest);

                if (cmd == ".quit" || cmd == ".exit") {
                    break;
                }
                if (cmd == ".help") {
                    std::println("Commands:");
                    std::println("  .help                                Show this help message");
                    std::println("  .ping                                Ping the server and measure round-trip time");
                    std::println("  .publish <module_name> <file.wasm>   Publish a WebAssembly Whisk module");
                    std::println("  .call <module> <func> [json_args]    Invoke a Whisk function");
                    std::println("  .list                                List deployed Whisk modules");
                    std::println("  .logs [N]                            Print server logs (default: 200)");
                    std::println("  .snapshot                            Trigger server snapshot");
                    std::println("  .quit                                Exit the REPL");
                    std::println("  .exit                                Exit the REPL");
                    continue;
                }
                if (cmd == ".ping") {
                    auto ping_res = client.Ping();
                    if (!ping_res) {
                        PrintReplError(ErrorText(ping_res.error()));
                    } else {
                        std::println("PONG in {} us", *ping_res);
                    }
                    continue;
                }
                if (cmd == ".publish") {
                    std::string_view mod_name = NextToken(rest);
                    std::string_view file_path = rest;
                    if (mod_name.empty() || file_path.empty()) {
                        PrintReplError("usage: .publish <module_name> <file.wasm>");
                        continue;
                    }
                    auto pub_res = client.PublishModuleFromFile(mod_name, std::string(file_path));
                    if (!pub_res) {
                        PrintReplError(ErrorText(pub_res.error()));
                    } else {
                        PrintPublishResult(*pub_res, false);
                    }
                    continue;
                }
                if (cmd == ".call") {
                    std::string_view mod_name = NextToken(rest);
                    std::string_view func_name = NextToken(rest);
                    if (mod_name.empty() || func_name.empty()) {
                        PrintReplError("usage: .call <module_name> <func_name> [json_args]");
                        continue;
                    }
                    std::string_view args_json = rest.empty() ? std::string_view("{}") : rest;
                    auto call_res = client.CallWhisk(mod_name, func_name, args_json);
                    if (!call_res) {
                        PrintReplError(ErrorText(call_res.error()));
                    } else {
                        PrintCallResult(*call_res, false);
                    }
                    continue;
                }
                if (cmd == ".list") {
                    auto list_res = client.ListModules();
                    if (!list_res) {
                        PrintReplError(ErrorText(list_res.error()));
                    } else {
                        PrintListResult(*list_res, false);
                    }
                    continue;
                }
                if (cmd == ".logs") {
                    idx_t limit = LogsRequest::DEFAULT_LIMIT;
                    if (!rest.empty()) {
                        auto parsed = ParseLogLimit(rest);
                        if (!parsed) {
                            PrintReplError(std::format("invalid log limit '{}' (expected {}-{})",
                                                       rest,
                                                       LogsRequest::MIN_LIMIT,
                                                       LogsRequest::MAX_LIMIT));
                            continue;
                        }
                        limit = *parsed;
                    }
                    auto logs_res = client.GetLogs(limit);
                    if (!logs_res) {
                        PrintReplError(ErrorText(logs_res.error()));
                    } else {
                        std::print("{}", *logs_res);
                        if (!logs_res->empty() && logs_res->back() != '\n') {
                            std::println("");
                        }
                    }
                    continue;
                }
                if (cmd == ".snapshot") {
                    auto snap_res = client.Snapshot();
                    if (!snap_res) {
                        PrintReplError(ErrorText(snap_res.error()));
                    } else {
                        std::println("{}", *snap_res);
                    }
                    continue;
                }
                PrintReplError(std::format("unknown command '{}'", cmd));
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
                PrintReplError(ErrorText(res.error()));
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
    bool output_json = false;
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
        } else if (arg == "--json") {
            output_json = true;
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
                     ErrorText(conn_res.error()));
        return 1;
    }

    if (command == "ping") {
        auto ping_res = client.Ping();
        if (!ping_res) {
            std::println(stderr, "error: {}", ErrorText(ping_res.error()));
            return 1;
        }
        if (output_json) {
            std::println("{{\"pong\":true,\"latency_us\":{}}}", *ping_res);
        } else {
            std::println("PONG in {} us", *ping_res);
        }
        return 0;
    }

    if (command == "sql") {
        if (command_args.empty()) {
            std::println(stderr, "error: missing SQL query");
            PrintUsage(stderr);
            return 2;
        }
        std::string query = JoinStrings(command_args, " ");
        auto query_res = client.Query(query);
        if (!query_res) {
            std::println(stderr, "error: {}", ErrorText(query_res.error()));
            return 1;
        }
        if (output_json) {
            std::print("{}", FormatResultJson(*query_res));
        } else {
            std::print("{}", FormatResultTable(*query_res));
        }
        return 0;
    }

    if (command == "publish") {
        if (command_args.size() != 2) {
            std::println(stderr, "error: usage: whisk publish <module_name> <file.wasm>");
            return 2;
        }
        auto pub_res = client.PublishModuleFromFile(command_args[0], command_args[1]);
        if (!pub_res) {
            std::println(stderr, "error: {}", ErrorText(pub_res.error()));
            return 1;
        }
        PrintPublishResult(*pub_res, output_json);
        return 0;
    }

    if (command == "call") {
        if (command_args.size() < 2 || command_args.size() > 3) {
            std::println(stderr, "error: usage: whisk call <module_name> <func_name> [json_args]");
            return 2;
        }
        std::string_view args_json = command_args.size() == 3 ? std::string_view(command_args[2])
                                                              : std::string_view("{}");
        auto call_res = client.CallWhisk(command_args[0], command_args[1], args_json);
        if (!call_res) {
            std::println(stderr, "error: {}", ErrorText(call_res.error()));
            return 1;
        }
        PrintCallResult(*call_res, output_json);
        return 0;
    }

    if (command == "list") {
        if (!command_args.empty()) {
            std::println(stderr, "error: usage: whisk list");
            return 2;
        }
        auto list_res = client.ListModules();
        if (!list_res) {
            std::println(stderr, "error: {}", ErrorText(list_res.error()));
            return 1;
        }
        PrintListResult(*list_res, output_json);
        return 0;
    }

    if (command == "logs") {
        idx_t limit = LogsRequest::DEFAULT_LIMIT;
        for (idx_t k = 0; k < command_args.size(); ++k) {
            std::string_view a = command_args[k];
            if (a == "-n" || a == "--lines") {
                if (k + 1 >= command_args.size()) {
                    std::println(stderr, "error: {} requires an argument", a);
                    return 2;
                }
                auto parsed = ParseLogLimit(command_args[++k]);
                if (!parsed) {
                    std::println(stderr, "error: invalid log limit '{}' (expected {}-{})",
                                 command_args[k],
                                 LogsRequest::MIN_LIMIT,
                                 LogsRequest::MAX_LIMIT);
                    return 2;
                }
                limit = *parsed;
            } else if (a.starts_with("--lines=")) {
                std::string_view val_sv = a.substr(8);
                auto parsed = ParseLogLimit(val_sv);
                if (!parsed) {
                    std::println(stderr, "error: invalid log limit '{}' (expected {}-{})",
                                 val_sv,
                                 LogsRequest::MIN_LIMIT,
                                 LogsRequest::MAX_LIMIT);
                    return 2;
                }
                limit = *parsed;
            } else if (a.starts_with("-n=")) {
                std::string_view val_sv = a.substr(3);
                auto parsed = ParseLogLimit(val_sv);
                if (!parsed) {
                    std::println(stderr, "error: invalid log limit '{}' (expected {}-{})",
                                 val_sv,
                                 LogsRequest::MIN_LIMIT,
                                 LogsRequest::MAX_LIMIT);
                    return 2;
                }
                limit = *parsed;
            } else {
                std::println(stderr, "error: unknown logs option '{}'", a);
                return 2;
            }
        }
        auto logs_res = client.GetLogs(limit);
        if (!logs_res) {
            std::println(stderr, "error: {}", ErrorText(logs_res.error()));
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
            std::println(stderr, "error: {}", ErrorText(snap_res.error()));
            return 1;
        }
        if (output_json) {
            std::println("{{\"message\":\"{}\"}}", EscapeJsonString(*snap_res));
        } else {
            std::println("{}", *snap_res);
        }
        return 0;
    }

    if (command == "repl") {
        return RunRepl(client);
    }

    std::println(stderr, "error: unknown command '{}'", command);
    PrintUsage(stderr);
    return 2;
}
