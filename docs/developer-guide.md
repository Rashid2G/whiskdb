# Developer Guide

Welcome to the WhiskDB Developer Guide. This document defines the engineering standards, architecture, and collaboration workflow for all team members contributing to the project.

---

## External Libraries

WhiskDB leverages external C++ libraries for low-level networking, protocols, and performance-critical operations:

| Library | Component / Header | Purpose | Description |
|---|---|---|---|
| **[Boost.Beast](https://www.boost.org/doc/libs/release/libs/beast/)** | `boost/beast` (`#include <boost/beast.hpp>`) | Networking & WebSocket | Provides HTTP and WebSocket protocol implementations built on top of **Boost.Asio**. Powers WhiskDB's client connection handling, real-time reactive subscriptions, and network communication layer. |

### Boost.Beast Details

- **Header & Namespace**: `<boost/beast.hpp>`, `boost::beast` (with `boost::beast::websocket`)
- **Underlying I/O Engine**: Built on top of **Boost.Asio** (`boost::asio`) for scalable, asynchronous, non-blocking network I/O.
- **Key Responsibilities**:
    - **WebSocket Protocol Handling**: Manages full RFC 6455 compliance, including handshake negotiation, framing, binary and text messages, and ping/pong keep-alives.
    - **Real-Time Client Subscriptions**: Powers the network communication layer that broadcasts data mutations and subscription updates to connected clients with low latency.
    - **Zero-Copy Buffer Integration**: Interoperates with memory buffers efficiently to match WhiskDB's in-engine execution model.
- **Integration & Build**: Managed via **vcpkg** (`vcpkg.json`) and linked in `CMakeLists.txt` via CMake's `find_package(Boost REQUIRED)`.

---

## Coding Standards & Conventions

To maintain a clean and reliable codebase across the team, all contributors should follow these conventions:

### C++ Style

#### Memory & Pointers
- **Smart pointers only**: Never use raw `malloc`, `new`, or `delete`. Use smart pointers for all dynamic allocations.
- **Prefer `std::unique_ptr`**: Default to `std::unique_ptr` for exclusive ownership. Use `std::shared_ptr` only when multiple owners truly need shared lifetime management.

#### Types & Constants
- **Fixed-width integers**: Use explicit fixed-width integers (`int8_t`, `int16_t`, `int32_t`, `int64_t`, `uint8_t`, `uint16_t`, `uint32_t`, `uint64_t`) instead of standard types like `int`, `long`, or `uint`.
- **Indices and offsets**: Use `idx_t` instead of `size_t` for counts, offsets, and collection indices.
- **Scoped enums (`enum class`)**: Always use `enum class` rather than plain C-style enums. Explicitly specify the underlying storage type (e.g., `: uint8_t`) only when the enum is serialized.
- **No magic numbers**: Name literal values using `constexpr` variables to provide clear meaning and context.

#### Const & References
- **Use `const` whenever possible**: Mark variables, member functions, and parameters `const` by default if they should not be modified.
- **Pass by reference**: Favor references over raw pointers for function arguments.
- **Const references for non-trivial types**: Pass objects that are expensive to copy (e.g., `std::vector`, `std::string`) as `const &`.

#### Functions & Control Flow
- **Return early**: Keep nesting shallow and avoid deep `if`/`else` branches by returning early.
- **Always use braces**: Use curly braces `{}` for all `if` statements and loops, even for single-line bodies.
- **Range-based loops**: Prefer modern range-based `for` loops whenever iterating:
  ```cpp
  for (const auto& item : items) {
      // process item
  }
  ```
- **Virtual overrides**: When overriding a virtual method, omit the `virtual` keyword and explicitly use `override` or `final`.

#### Namespaces
- **No namespace imports**: Never write `using namespace std;` or import entire namespaces into source or header files.
- **Core namespace**: All functions and classes under the core engine (`src/`) must reside inside the `database` namespace.

#### Class Layout
Keep class structures consistent and easy to scan:
1. Public constructor(s) and public member variables.
2. Public member functions.
3. Private helper functions.
4. Private member variables.

```cpp
class MyClass {
public:
    MyClass();

    int my_public_variable;

public:
    void MyFunction();

private:
    void MyPrivateFunction();

private:
    int my_private_variable;
};
```

---

### Naming Conventions

Keep names clear and descriptive. Avoid single-letter variables except for standard loop counters in simple, non-nested loops.

| Target | Convention | Example |
|---|---|---|
| **Files** | Lowercase with underscores (`snake_case`) | `abstract_operator.cpp` |
| **Types** (classes, structs, enums, aliases) | Capitalized words (`PascalCase`) | `BaseColumn`, `TupleBuffer` |
| **Variables & Members** | Lowercase with underscores (`snake_case`) | `chunk_size`, `buffer_idx` |
| **Functions & Methods** | Capitalized words (`PascalCase`) | `GetChunk()`, `ExecutePlan()` |
| **Loop Indices** | Meaningful names in nested loops | `column_idx`, `row_idx` (`i` is only allowed in single loops) |

*(Note: These naming guidelines are partially enforced automatically by `clang-tidy`.)*

---

### Error Handling & Assertions

#### Exceptions vs. Return Values
- **Exceptions are for hard failures**: Use exceptions only when an error terminates a query (such as SQL syntax errors, missing tables, or resource exhaustion).
- **Return values for normal flow**: If an error is an expected situation that normal execution can handle, return a status or result code instead of throwing.
- **Test exception paths**: Always write unit tests that trigger expected exceptions. If an error condition cannot be reproduced in a test, consider whether it should be an invariant assertion instead (rare cases like out-of-memory are exceptions).

#### Assertions with `D_ASSERT`
- **Catch developer bugs**: Use `D_ASSERT` to verify internal assumptions and developer errors. Assertions should never be triggered by invalid user input.
- **Assert generously with context**: Add assertions whenever an assumption must hold, and always include a brief comment explaining the expected state:
  ```cpp
  // Validate that the target column index exists within bounds
  D_ASSERT(column_idx < column_count);
  ```

---

### AI Usage Policy

AI coding assistants are helpful tools, but keep usage disciplined:
- **Take full ownership**: Never submit AI-generated code that you do not fully understand and cannot explain or debug yourself.
- **Verify before submitting**: Make sure any generated code adheres to our project conventions, passes tests, and doesn't introduce unwanted dependencies or formatting issues.

---

## Build & Test Macros

Standard development operations can be executed with short unified macros:

| Macro | Purpose |
|---|---|
| `cmake build-debug` | Configures & compiles in Debug mode (`build/debug/`) |
| `cmake build-release` | Configures & compiles in Release mode (`build/release/`) |
| `cmake run-tests` | Compiles Debug and executes all test suites with verbose failure reporting |
| `cmake run-tests <regex>` | Runs test suites matching the filter regex (e.g. `cmake run-tests wal`) |
| `cmake clean` | Removes build directories |

*All macros are implemented in [`scripts/macro.sh`](../scripts/macro.sh) and can be extended by adding new command functions.*

---

## Git & Collaboration Workflow

### Mandatory 5-Step Development Flow

All contributors and AI agents must follow this explicit lifecycle for every task:

1. **Check & Update GitHub Issues**:
   - Inspect open issues: `gh issue list` and `gh issue view <N>`.
   - Post an initial comment on the issue acknowledging work has started.
2. **Branch from `dev`**:
   - Sync `dev`: `git checkout dev && git pull origin dev`.
   - Branch: `git checkout -b feat/issue-<N>-<short-slug>`.
3. **Build & Test with Macros**:
   - Run `cmake build-debug` and `cmake run-tests`.
   - Ensure 0 compiler warnings and all tests pass cleanly.
4. **Conventional Commit & Push**:
   - `feat(<scope>): <description> (Closes #<N>)`.
   - `git push -u origin feat/issue-<N>-<short-slug>`.
5. **Open Pull Request & Close Issues (MANDATORY)**:
   - Target branch is `dev`: `gh pr create --base dev --head feat/issue-<N>-<short-slug> ...`.
   - Work is never considered finished until the Pull Request is opened and linked.
   - Close or reference issues with `Closes #<N>`.

### Branching Strategy

| Branch | Purpose |
|---|---|
| `main` | Production-ready, stable milestone releases. Protected. |
| `dev` | Active integration branch. All features merge here first. |

### Commit Message Convention

Keep commit messages concise and descriptive using the standard format:
- `feat(sql): add index scan operator (Closes #12)`
- `fix(wal): handle torn page recovery in replay (Closes #17)`
- `docs(dev): update developer guide conventions`

### Pull Request (PR) Checklist

Before submitting a PR for review, make sure:
- [ ] Code follows our C++ style and naming conventions (passes `clang-tidy`).
- [ ] Code compiles warning-free via `cmake build-debug`.
- [ ] All tests pass cleanly via `cmake run-tests`.
- [ ] Assertions use `D_ASSERT` with explanatory comments.
- [ ] PR targets `dev` and references `Closes #<N>`.
- [ ] Any AI-assisted code has been fully reviewed, understood, and tested.

