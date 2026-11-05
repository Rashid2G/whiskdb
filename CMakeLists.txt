cmake_minimum_required(VERSION 3.25)
project(database
        VERSION 0.1.0
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE DEBUG)
endif()

if(EXISTS "${PROJECT_SOURCE_DIR}/vcpkg_installed/x64-linux")
  list(APPEND CMAKE_PREFIX_PATH "${PROJECT_SOURCE_DIR}/vcpkg_installed/x64-linux")
endif()

find_package(asio CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_library(database_lib
  src/common/logger.cpp
  src/common/errors.cpp
  src/network/connection_manager.cpp
  src/network/tcp_session.cpp
  src/network/network.cpp
  src/network/protocol.cpp
  src/engine.cpp
)

target_include_directories(database_lib PUBLIC
  ${PROJECT_SOURCE_DIR}/include
)

target_compile_options(database_lib PRIVATE -Wall -Wextra -Wpedantic)

target_link_libraries(database_lib PUBLIC
  asio::asio
  tomlplusplus::tomlplusplus
  Threads::Threads
)

add_executable(app app/main.cpp)
target_link_libraries(app PRIVATE database_lib)

add_executable(client app/client.cpp)
target_link_libraries(client PRIVATE database_lib)

enable_testing()

add_executable(test_logger_errors tests/test_logger_errors.cpp)
target_link_libraries(test_logger_errors PRIVATE database_lib)
add_test(NAME test_logger_errors COMMAND test_logger_errors)

if(EXISTS "${PROJECT_SOURCE_DIR}/tests/test_framing.cpp")
  add_executable(test_framing tests/test_framing.cpp)
  target_link_libraries(test_framing PRIVATE database_lib)
  add_test(NAME test_framing COMMAND test_framing)
endif()

if(EXISTS "${PROJECT_SOURCE_DIR}/tests/test_protocol.cpp")
  add_executable(test_protocol tests/test_protocol.cpp)
  target_link_libraries(test_protocol PRIVATE database_lib)
  add_test(NAME test_protocol COMMAND test_protocol)
endif()

add_executable(test_network_stress tests/test_network_stress.cpp)
target_link_libraries(test_network_stress PRIVATE database_lib)
add_test(NAME test_network_stress COMMAND test_network_stress)
