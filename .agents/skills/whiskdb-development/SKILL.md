---
name: whiskdb-development
description: Use when building, testing, developing, or verifying WhiskDB C++ code, running debug or release builds, executing test suites with cmake macros, or adding new development scripts.
---

# WhiskDB Development & Build Skill

This skill defines the standard commands and workflows for building and testing WhiskDB.

## 1. Quick Build & Test Macros

All core development commands can be executed via short `cmake` macros directly from anywhere in the repository:

| Macro Command | Action | Output Directory |
|---|---|---|
| `cmake build-debug` | Configures (if needed) and compiles the full project in Debug mode | `build/debug/` |
| `cmake build-release` | Configures (if needed) and compiles the full project in Release mode | `build/release/` |
| `cmake run-tests` | Compiles Debug and executes all 10 test suites via CTest with failure output | `build/debug/` |
| `cmake run-tests <filter>` | Runs only test suites matching the regex filter (e.g. `cmake run-tests wal`) | `build/debug/` |
| `cmake clean` | Removes compiled build directories | `build/debug`, `build/release` |

### Native CMake & Ninja Alternatives

The macros map directly to standard CMake workflows:
- `scripts/macro.sh build-debug` / `scripts/macro.sh run-tests`
- `cmake --build build/debug --target run-tests`
- `cmake --workflow --preset run-tests`

## 2. Expanding Macros with New Scripts

To add a new script macro (e.g., `benchmark`, `coverage`, `lint`):

1. Open [`scripts/macro.sh`](file:///home/rashid/whiskdb/scripts/macro.sh).
2. Add a new command function:
   ```bash
   cmd_benchmark() {
     cmd_build_release
     echo "==> Running benchmarks..."
     # Add benchmark execution commands here
   }
   ```
3. Register the command pattern in the `case "$SUBCOMMAND" in` switch block in [`scripts/macro.sh`](file:///home/rashid/whiskdb/scripts/macro.sh):
   ```bash
   benchmark|bench)
     cmd_benchmark "$@"
     ;;
   ```
4. The macro is immediately runnable as `cmake benchmark` or `scripts/macro.sh benchmark`.

## 3. Mandatory Development Flow for Agents

1. **Check & Update Issue**:
   - Always find the relevant GitHub Issue first: `gh issue list` / `gh issue view <N>`.
   - Comment on the issue stating work is starting: `gh issue comment <N> --body "Starting work on..."`.
2. **Feature Branch from `dev`**:
   - `git checkout dev && git pull origin dev`
   - `git checkout -b feat/issue-<N>-<short-slug>`
3. **Build & Test**:
   - Verify Debug build: `cmake build-debug`
   - Run test suite: `cmake run-tests`
   - Zero compiler warnings (`-Wall -Wextra -Wpedantic`) and zero UBSan errors.
4. **Commit**:
   - Conventional commit referencing the issue: `feat(scope): <message> (Closes #<N>)`.
5. **Push & Create Pull Request (MANDATORY)**:
   - `git push -u origin <branch>`
   - `gh pr create --base dev --head <branch> --title "..." --body "Closes #<N> ..."`
   - **Never finish a task without opening or updating the Pull Request.**
