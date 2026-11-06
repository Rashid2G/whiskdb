#include "config.hpp"
#include "errors.hpp"
#include "logger.hpp"
#include "network.hpp"
#include "engine.hpp"

#include <exception>

auto main(int /*argc*/, char * /*argv*/[]) -> int {
    try {
        LOG_INFO("WhiskDB booting...");

        Database::Config config(Database::config_file_path);
		Database::Engine engine(config);
        Database::Server server(config);

        // Persistent TCP request handler hook
        server.SetRequestHandler([](const std::string &request) -> std::string {
            LOG_DEBUG("Received request: {}", request);
            return "{\"status\":\"ok\",\"echo\":\"" + request + "\"}";
        });

        LOG_INFO("WhiskDB configured on {}:{}", config.host(), config.port());
        server.Run();
    } catch (...) {
        Database::handle_top_level_exception();
        return 1;
    }

    return 0;
}
