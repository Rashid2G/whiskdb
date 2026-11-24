## Summary
<!-- Provide a clear, concise overview of what this pull request changes or adds. -->

## Motivation & Context
<!-- Why is this change required? What problem does it solve? -->

## Linked Issues
Closes #<N>

## Verification & Test Evidence
<!-- Provide exact command outputs or verification evidence. -->
- `./scripts/macro.sh build-debug` (0 compiler warnings)
- `./scripts/macro.sh run-tests` (all test suites passing)

```text
<!-- Paste ctest or benchmark output here -->
```

## Checklist
- [ ] Code follows WhiskDB C++23 style and naming conventions (`clang-tidy`).
- [ ] Code compiles warning-free (`-Wall -Wextra -Wpedantic`).
- [ ] All tests pass cleanly via `./scripts/macro.sh run-tests`.
- [ ] Documentation updated in `docs/` when behavior, config, or CLI changed.
- [ ] PR targets `dev` and links the relevant issue.
