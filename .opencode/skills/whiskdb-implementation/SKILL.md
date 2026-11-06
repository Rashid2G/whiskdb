---
name: WhiskDB Implementation
description: Implement a WhiskDB GitHub issue end to end — branch off dev, follow the C++23/namespace database rules, add tests, run ctest, and open a PR targeting dev. Use when asked to build, fix, refactor, or ship code in this repository.
---

# WhiskDB Implementation

## Read first

1. `AGENTS.md` — hard engineering rules.
2. `plan.md` — milestone and issue intent.
3. The GitHub issue — acceptance criteria and test requirements.
4. The nearest existing header/source/test — copy its pattern.

If no issue exists, create one first with the `whiskdb-issue` skill.

## Start

```sh
git fetch origin
git switch dev && git pull --ff-only
git switch -c feat/issue-<N>-<slug>   # fix/... or docs/...
gh issue view <N>
```

Never commit directly to `dev` or `main`. One issue = one branch = one PR.

## Where code goes

| Path | Contents |
|---|---|
| `include/` | Public headers, all in `namespace database` |
| `src/common/` | Logger, errors, shared utilities |
| `src/network/` | Asio server, sessions, framing, wire protocol |
| `src/engine.cpp` | Engine entry point |
| `app/` | `app` (server) and `client` executables |
| `tests/` | Standalone test executables (see `whiskdb-testing`) |
| `docs/` | MkDocs pages (see `whiskdb-mkdocs`) |

## Non-negotiables

- All engine code in `namespace database`; no `using namespace std;`.
- Types/methods `PascalCase`; files/variables/members `snake_case`.
- `idx_t` for counts/indices/offsets; fixed-width integers elsewhere.
- No raw `new`/`delete`/`malloc`; prefer `std::unique_ptr`.
- Expected failures return `Result<T>` / `ErrorCode`; `D_ASSERT` for internal invariants only; `FatalError` for unrecoverable failures.
- Zero-copy at boundaries: `std::string_view`, `std::span<const uint8_t>`. Reuse per-session/per-request buffers.
- Braces on every `if`/`for`/`while`; return early.
- Replace magic numbers with `constexpr`.

Full details: `AGENTS.md` and `docs/developer-guide.md`.

## Workflow

1. Implement the smallest coherent change that satisfies the issue.
2. Add or update tests immediately — not in a follow-up PR.
3. Build and run the suite:
   ```sh
   cmake -B build -DCMAKE_BUILD_TYPE=Debug
   cmake --build build -j
   ctest --test-dir build --output-on-failure
   ```
4. Update docs only when behaviour, config, or CLI changes (`whiskdb-mkdocs`).
5. Commit with Conventional Commits: `feat:`, `fix:`, `refactor:`, `test:`, `docs:`.
6. Push and open the PR against `dev`:
   ```sh
   git push -u origin HEAD
   gh pr create --base dev --title "<type>(<area>): <summary>" --body-file /tmp/pr-body.md
   ```
7. Link the PR in the reply and report the real test output.

## PR body

```markdown
## Summary
What changed and why.

Closes #<N>

## Key Changes
- ...

## Verification
Paste the actual command and output:
`ctest --test-dir build --output-on-failure` → N/N passed in X.XXs

## Checklist
- [ ] Follows C++23 and `namespace database` conventions
- [ ] Tests cover happy path, boundary, and error path
- [ ] No commented-out code or debug prints left behind
- [ ] Docs updated if behaviour/config/CLI changed
```

## Done means

- Issue acceptance criteria met.
- New tests registered in `CMakeLists.txt` and passing.
- `ctest --output-on-failure` fully green.
- PR targets `dev`, closes the issue, and includes real output.
- No unrelated files in the diff.
