---
name: WhiskDB Testing
description: Write and run WhiskDB tests — standalone assert-based test executables registered with CTest, plus sanitizer and stress/benchmark runs. Use when adding tests, fixing a failing test, or producing verification evidence for a PR or review.
---

# WhiskDB Testing

## Harness

WhiskDB does **not** use GoogleTest. Each test is a standalone executable that uses `assert` and `std::println`, with a `main()` that calls test functions and returns `0` on success.

Follow the existing files: `tests/test_framing.cpp`, `tests/test_protocol.cpp`, `tests/test_network_stress.cpp`.

## Add a test

1. Create `tests/test_<area>.cpp`:

```cpp
#include "logger.hpp"
#include "errors.hpp"

#include <cassert>
#include <print>

namespace {

using database::Logger;
using database::LogLevel;

void TestHappyPath() {
    std::println("Testing ...");
    assert(/* actual */ == /* expected */);
    std::println("  [PASS] ...");
}

} // namespace

int main() {
    Logger::SetLevel(LogLevel::Warn);
    TestHappyPath();
    std::println("ALL <AREA> TESTS PASSED!");
    return 0;
}
```

2. Register it in `CMakeLists.txt`:

```cmake
add_executable(test_<area> tests/test_<area>.cpp)
target_link_libraries(test_<area> PRIVATE database_lib)
add_test(NAME test_<area> COMMAND test_<area>)
```

3. Build and run:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/test_<area>          # for detailed per-test output
```

`assert` is disabled in Release builds (`NDEBUG`). Always verify with a Debug build, or add `-UNDEBUG`.

## What to cover

Every behaviour change needs three layers:

| Layer | Example |
|---|---|
| Happy path | Valid frame round-trip, successful opcode dispatch |
| Boundary | `0`, `1`, `MAX_PAYLOAD_SIZE`, empty payload |
| Error / negative | Oversized frame → `ErrorCode::PayloadTooLarge`, unknown opcode, disconnected client |

For concurrency and network code, also test:

- Fragmented reads (1 byte at a time).
- Coalesced/pipelined frames in one write.
- Multiple concurrent clients with all requests accounted for.
- Clean shutdown: no detached threads, no use-after-free.

## Determinism rules

- Bind to port `0` and read `Server::Port()`; never hard-code a port.
- Use `StartBackground()` / `Stop()` and join every thread.
- Avoid `sleep` for synchronisation; if a short wait is unavoidable (e.g. accepting a close), comment why.
- No external network, no wall-clock-dependent assertions.

## Sanitizers

Run before requesting review when touching memory, buffers, or threads:

```sh
cmake -B build/asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build/asan -j
ctest --test-dir build/asan --output-on-failure
```

`build/` is gitignored, so `build/asan` stays out of the diff.

## Evidence to report

Report the command and the real result, not a summary:

- `ctest` → `5/5 tests passed, 0.14s`
- `./build/test_network_stress` → `6,400 requests completed in 214ms`
- ASAN/UBSAN → `clean, 0 leaks, 0 UB reports`

Never invent timings. If a test does not measure something, do not claim it does.
