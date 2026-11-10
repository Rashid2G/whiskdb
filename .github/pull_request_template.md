## Summary
<!-- Provide a clear, concise overview of what this pull request changes or adds. -->

## Motivation & Context
<!-- Why is this change required? What problem does it solve? -->

## Linked Issues
Closes #<N>

## Verification & Test Evidence
<!-- Provide exact command outputs or verification evidence. Real numbers only; no estimates. -->
- `cmake build-debug` (0 compiler warnings)
- `cmake run-tests` (all test suites passing)

```text
<!-- Paste ctest or benchmark output here -->
```

## Developer Guide Checklist
Before submitting a PR for review, verify all items below:
- [ ] Code follows our C++ style and naming conventions (passes `clang-tidy`).
- [ ] Code compiles warning-free via `cmake build-debug`.
- [ ] All tests pass cleanly via `cmake run-tests`.
- [ ] Assertions use `D_ASSERT` with explanatory comments.
- [ ] PR targets `dev` and references `Closes #<N>`.
- [ ] Any AI-assisted code has been fully reviewed, understood, and tested.
