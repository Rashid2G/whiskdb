cmake_minimum_required(VERSION 3.21)
project(whiskdb VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

option(WHISKDB_SANITIZE "Enable UBSAN (and ASAN where supported)" OFF)

# ---------------------------------------------------------------------------
# Dependencies: check vcpkg/installed prefix first, fallback to FetchContent
# ---------------------------------------------------------------------------
if(EXISTS "${PROJECT_SOURCE_DIR}/vcpkg_installed/x64-linux")
  list(APPEND CMAKE_PREFIX_PATH "${PROJECT_SOURCE_DIR}/vcpkg_installed/x64-linux")
endif()

find_package(Threads REQUIRED)

# SQLite (amalgamation, compiled into a static lib with C compiler)
find_package(SQLite3 QUIET)
if(NOT SQLite3_FOUND)
  include(FetchContent)
  FetchContent_Declare(sqlite_amalgamation
    URL https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(sqlite_amalgamation)
  add_library(sqlite3_static STATIC ${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c)
  target_include_directories(sqlite3_static PUBLIC ${sqlite_amalgamation_SOURCE_DIR})
  target_compile_definitions(sqlite3_static PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION=1)
  set_target_properties(sqlite3_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_library(SQLite::SQLite3 ALIAS sqlite3_static)
endif()

# standalone Asio (header only)
find_package(asio CONFIG QUIET)
if(asio_FOUND)
  add_library(asio_headers INTERFACE)
  target_link_libraries(asio_headers INTERFACE asio::asio)
else()
  find_path(ASIO_INCLUDE_DIR asio.hpp)
  if(NOT ASIO_INCLUDE_DIR)
    include(FetchContent)
    FetchContent_Declare(asio
      GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
      GIT_TAG asio-1-30-2 GIT_SHALLOW TRUE SOURCE_SUBDIR _header_only_)
    FetchContent_MakeAvailable(asio)
    set(ASIO_INCLUDE_DIR ${asio_SOURCE_DIR}/asio/include)
  endif()
  add_library(asio_headers INTERFACE)
  target_include_directories(asio_headers SYSTEM INTERFACE ${ASIO_INCLUDE_DIR})
  target_compile_definitions(asio_headers INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
  target_link_libraries(asio_headers INTERFACE Threads::Threads)
endif()

# toml++ (header only)
find_package(tomlplusplus QUIET)
if(tomlplusplus_FOUND)
  add_library(toml_headers INTERFACE)
  target_link_libraries(toml_headers INTERFACE tomlplusplus::tomlplusplus)
else()
  include(FetchContent)
  FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0 GIT_SHALLOW TRUE SOURCE_SUBDIR _header_only_)
  FetchContent_MakeAvailable(tomlplusplus)
  add_library(toml_headers INTERFACE)
  target_include_directories(toml_headers SYSTEM INTERFACE ${tomlplusplus_SOURCE_DIR}/include)
endif()

# ---------------------------------------------------------------------------
# Core library: explicit list of Milestone 1 & 2 sources
# ---------------------------------------------------------------------------
add_library(whiskdb_core STATIC
  src/common/logger.cpp
  src/common/errors.cpp
  src/engine.cpp
  src/network/connection_manager.cpp
  src/network/tcp_session.cpp
  src/network/network.cpp
  src/network/protocol.cpp
)
target_include_directories(whiskdb_core PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(whiskdb_core PUBLIC asio_headers toml_headers SQLite::SQLite3)
target_compile_options(whiskdb_core PUBLIC -Wall -Wextra -Wpedantic)

if(WHISKDB_SANITIZE)
  target_compile_options(whiskdb_core PUBLIC -fsanitize=undefined -fno-sanitize-recover=undefined)
  target_link_options(whiskdb_core PUBLIC -fsanitize=undefined)
endif()

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
add_executable(app app/main.cpp)
target_link_libraries(app PRIVATE whiskdb_core)

add_executable(client app/client.cpp)
target_link_libraries(client PRIVATE whiskdb_core)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

set(WHISKDB_TEST_TARGETS
  test_logger_errors
  test_framing
  test_protocol
  test_network_stress
)

foreach(test_name ${WHISKDB_TEST_TARGETS})
  if(EXISTS "${CMAKE_SOURCE_DIR}/tests/${test_name}.cpp")
    add_executable(${test_name} tests/${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE whiskdb_core)
    target_include_directories(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    target_compile_options(${test_name} PRIVATE -UNDEBUG)
    add_test(NAME ${test_name} COMMAND ${test_name} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  endif()
endforeach()
