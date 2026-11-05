#include "engine.hpp"
#include "logger.hpp"

namespace database {

Engine::Engine() {
    LOG_DEBUG("Starting Engine...");
}

Engine::Engine(const Config& config) : db_data_file_path_(config.DbFilePath()) {
    LOG_DEBUG("Starting Engine with data path '{}'...", db_data_file_path_);
}

void Engine::Start() {}

void Engine::Run() {}

} // namespace database
