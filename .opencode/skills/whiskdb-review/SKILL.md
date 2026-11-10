---
name: WhiskDB Review
description: Review a WhiskDB pull request or diff — verify the branch builds, tests pass, conventions hold, and every performance claim is backed by real measured numbers. Use when asked to review, audit, or approve a PR, or to check work before merging.
---

# WhiskDB Review

Review from the code and the running build, never from the PR description.

## Process

1. Read the linked issue (`gh issue view <N>`) and its acceptance criteria.
2. Check out the PR branch and inspect the diff:
   ```sh
   gh pr checkout <N>
   gh pr diff <N>
   git log --oneline origin/dev..HEAD
   ```
3. Build and run the suite yourself:
   ```sh
   cmake -B build -DCMAKE_BUILD_TYPE=Debug
   cmake --build build -j
   ctest --test-dir build --output-on-failure
   ```
4. Run the relevant test binary directly for per-case output and timing.
5. If the PR claims a speed, scale, or memory improvement, reproduce the measurement.

## Evidence over claims

Every review must include results you observed. Paste the actual command and output.

```text
Verified on <commit sha>:
- Build: clean with -Wall -Wextra -Wpedantic
- ctest: 5/5 passed in 0.14s
- ./build/test_network_stress: 6,400 requests in 214ms (~29,900 req/s)
- Feature metric (when applicable): subscription fan-out to 1,000 clients, p95 12ms, 0 missed updates
- ASAN/UBSAN: clean
```

Rules:

- A review that only says "looks good" is incomplete.
- If the PR says "faster" or "supports X users" and there is no measurement, mark it **unverified** and request one.
- Use numbers you actually observed; never copy them from the PR body.
- If something cannot be measured yet (e.g. benchmark suite not implemented), say so explicitly.

## Checklist

**Scope**
- [ ] Branch is based on `dev`, named `feat|fix|docs/issue-<N>-<slug>`.
- [ ] PR body links the issue (`Closes #<N>` if solving completely, or `Part of #<N>` if partial).
- [ ] No unrelated files, debug prints, or commented-out code.

**Correctness**
- [ ] Boundary conditions handled (empty, max size, overflow).
- [ ] Resource lifetime is RAII; no leaks, no dangling views.
- [ ] Threads joined / stopped cleanly; shared state synchronised.
- [ ] Network paths handle partial reads, disconnects, and malformed frames.

**Conventions**
- [ ] `namespace database`, `PascalCase` types/methods, `snake_case` variables.
- [ ] `idx_t` for counts/indices; fixed-width integers.
- [ ] No raw `new`/`delete`/`malloc`; no `using namespace std;`.
- [ ] Braces on all control flow; class sections in the required order.

**Error policy**
- [ ] Expected failures return `Result<T>` / `ErrorCode`.
- [ ] `D_ASSERT` only for internal invariants, with a comment.
- [ ] `FatalError` only for truly unrecoverable conditions.

**Performance**
- [ ] No avoidable copies at wire/WAL/WASM boundaries (`string_view`, `span`).
- [ ] Per-session buffers reused; no hidden per-request allocation.
- [ ] No blocking I/O or lock held across a hot path.

**Tests & docs**
- [ ] New behaviour has happy-path, boundary, and negative tests.
- [ ] Test target registered in `CMakeLists.txt` and passing.
- [ ] Docs updated when behaviour/config/CLI changed (`whiskdb-mkdocs`).

## Verdict format

Group findings by severity with `file:line` references:

```markdown
## Review — PR #<N>

### Blockers
- `src/network/tcp_session.cpp:142` — buffer is resized per request; reuse it.

### Should fix
- ...

### Nits
- ...

### Evidence
<commands and observed output>

**Verdict:** Request changes | Approve
```

Approve only when the build is clean, tests pass, and the evidence block is present.
