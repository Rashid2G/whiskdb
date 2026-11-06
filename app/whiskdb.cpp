// whiskdb: server executable.
//   whiskdb [--config <file>] [--data-dir <dir>] [--host <ip>] [--port <n>] [--durability <mode>]
#include "config.hpp"
#include "engine.hpp"
#include "logger.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <print>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_stop{false};

void OnSignal(int) {
    g_stop.store(true);
}

} // namespace

int main(int argc, char** argv) {
    using namespace database;

    std::string config_path = config_file_path;
    for (int arg_idx = 1; arg_idx + 1 < argc; arg_idx++) {
        if (std::string_view(argv[arg_idx]) == "--config") {
            config_path = argv[arg_idx + 1];
        }
    }

    Config config(config_path);
    auto options = EngineOptions::FromConfig(config);
    if (!options) {
        std::println(stderr, "error: {}", options.error().Message());
        return 2;
    }

    for (int arg_idx = 1; arg_idx < argc; arg_idx++) {
        const std::string_view flag = argv[arg_idx];
        const bool has_value = arg_idx + 1 < argc;
        if (flag == "--help" || flag == "-h") {
            std::println("usage: whiskdb [--config f] [--data-dir d] [--host ip] [--port n] [--durability "
                         "strict|group_commit|relaxed|memory_only]");
            return 0;
        }
        if (!has_value) {
            continue;
        }
        const std::string value = argv[arg_idx + 1];
        if (flag == "--data-dir") {
            options->data_dir = value;
        } else if (flag == "--host") {
            options->host = value;
        } else if (flag == "--port") {
            options->port = static_cast<uint16_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (flag == "--durability") {
            auto mode = ParseDurabilityMode(value);
            if (!mode) {
                std::println(stderr, "error: {}", mode.error().Message());
                return 2;
            }
            options->durability = *mode;
        }
    }

    auto engine = Engine::Open(*options);
    if (!engine) {
        std::println(stderr, "failed to open database: {}", engine.error());
        return 1;
    }
    auto started = (*engine)->StartServer();
    if (!started) {
        std::println(stderr, "failed to start server: {}", started.error());
        return 1;
    }

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    LOG_INFO("Shutting down");
    (*engine)->StopServer();
    return 0;
}
