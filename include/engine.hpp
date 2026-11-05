#ifndef WHISKDB_ENGINE_HPP
#define WHISKDB_ENGINE_HPP

#include "config.hpp"

#include <string>

namespace database {

class Engine {
public:
    Engine();
    explicit Engine(const Config& config);
    ~Engine() = default;

public:
    void Start();
    void start() { Start(); }

    void Run();
    void run() { Run(); }

private:
    std::string db_data_file_path_;
};

} // namespace database

namespace Database = database;

#endif // WHISKDB_ENGINE_HPP
