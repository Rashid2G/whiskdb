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

include(FetchContent)
find_package(Threads REQUIRED)

# SQLite (amalgamation, compiled into a static lib with C compiler)
find_package(SQLite3 QUIET)
if(NOT SQLite3_FOUND)
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
  FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0 GIT_SHALLOW TRUE SOURCE_SUBDIR _header_only_)
  FetchContent_MakeAvailable(tomlplusplus)
  add_library(toml_headers INTERFACE)
  target_include_directories(toml_headers SYSTEM INTERFACE ${tomlplusplus_SOURCE_DIR}/include)
endif()

# Wasmtime C API
set(WASMTIME_SEARCH_PATHS
  ${WASMTIME_ROOT}
  $ENV{WASMTIME_ROOT}
  ${CMAKE_PREFIX_PATH}
  /usr/local
  /usr
  $ENV{HOME}/.local
  $ENV{HOME}/.whisk-tools
)

find_path(WASMTIME_INCLUDE_DIR wasmtime.h
  PATHS ${WASMTIME_SEARCH_PATHS}
  PATH_SUFFIXES include
)

find_library(WASMTIME_LIBRARY
  NAMES libwasmtime.so wasmtime libwasmtime libwasmtime.a
  PATHS ${WASMTIME_SEARCH_PATHS}
  PATH_SUFFIXES lib lib64
)

if(NOT WASMTIME_INCLUDE_DIR OR NOT WASMTIME_LIBRARY)
  FetchContent_Declare(wasmtime_c_api
    URL https://github.com/bytecodealliance/wasmtime/releases/download/v50.0.1/wasmtime-v50.0.1-x86_64-linux-c-api.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(wasmtime_c_api)
  set(WASMTIME_INCLUDE_DIR "${wasmtime_c_api_SOURCE_DIR}/include")
  if(EXISTS "${wasmtime_c_api_SOURCE_DIR}/lib/libwasmtime.so")
    set(WASMTIME_LIBRARY "${wasmtime_c_api_SOURCE_DIR}/lib/libwasmtime.so")
  else()
    set(WASMTIME_LIBRARY "${wasmtime_c_api_SOURCE_DIR}/lib/libwasmtime.a")
  endif()
endif()

add_library(wasmtime_lib INTERFACE)
target_include_directories(wasmtime_lib SYSTEM INTERFACE ${WASMTIME_INCLUDE_DIR})
target_link_libraries(wasmtime_lib INTERFACE ${WASMTIME_LIBRARY})
if(WASMTIME_LIBRARY MATCHES "\\.a$")
  target_link_libraries(wasmtime_lib INTERFACE Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# ---------------------------------------------------------------------------
# Core library: every src/**/*.cpp
# ---------------------------------------------------------------------------
file(GLOB_RECURSE WHISKDB_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
add_library(whiskdb_core STATIC ${WHISKDB_SOURCES})
target_include_directories(whiskdb_core PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(whiskdb_core PUBLIC asio_headers toml_headers SQLite::SQLite3 wasmtime_lib)
target_compile_options(whiskdb_core PUBLIC -Wall -Wextra -Wpedantic)

if(WHISKDB_SANITIZE)
  target_compile_options(whiskdb_core PUBLIC -fsanitize=undefined -fno-sanitize-recover=undefined)
  target_link_options(whiskdb_core PUBLIC -fsanitize=undefined)
endif()

# ---------------------------------------------------------------------------
# Executables (app/*.cpp -> server `whiskdb`, CLI `whisk`)
# ---------------------------------------------------------------------------
file(GLOB WHISKDB_APPS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/app/*.cpp)
foreach(app_src ${WHISKDB_APPS})
  get_filename_component(app_name ${app_src} NAME_WE)
  add_executable(${app_name} ${app_src})
  target_link_libraries(${app_name} PRIVATE whiskdb_core)
endforeach()

# ---------------------------------------------------------------------------
# Tests (tests/test_*.cpp -> one executable + one ctest entry per file)
# ---------------------------------------------------------------------------
enable_testing()

find_program(WHISKDB_WASM_CLANGXX NAMES clang++ PATHS "$ENV{HOME}/.whisk-tools/bin" /usr/bin /usr/local/bin)

function(add_whisk_test test_name test_src)
  if(NOT TARGET ${test_name})
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} PRIVATE whiskdb_core)
    target_include_directories(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    target_compile_options(${test_name} PRIVATE -UNDEBUG)
    target_compile_definitions(${test_name} PRIVATE WHISKDB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    if(WHISKDB_WASM_CLANGXX)
      target_compile_definitions(${test_name} PRIVATE WHISKDB_WASM_CLANGXX="${WHISKDB_WASM_CLANGXX}")
    endif()
    add_test(NAME ${test_name} COMMAND ${test_name} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  endif()
endfunction()

# Explicitly register core test suites via add_whisk_test
add_whisk_test(test_config tests/test_config.cpp)
add_whisk_test(test_wasm_runtime tests/test_wasm_runtime.cpp)
add_whisk_test(test_guest_sdk tests/test_guest_sdk.cpp)

file(GLOB WHISKDB_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/test_*.cpp)
foreach(test_src ${WHISKDB_TESTS})
  get_filename_component(test_name ${test_src} NAME_WE)
  add_whisk_test(${test_name} ${test_src})
endforeach()

# Convenient custom target to execute tests: `cmake --build <dir> --target run-tests`
add_custom_target(run-tests
  COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  COMMENT "Running WhiskDB test suites via CTest..."
)
