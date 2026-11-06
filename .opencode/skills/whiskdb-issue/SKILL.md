---
name: WhiskDB Issue
description: Write a WhiskDB GitHub issue with the project's milestone, label, acceptance-criteria, and test-requirement format. Use when planning work, breaking a milestone into tasks, or when implementation starts without an existing issue.
---

# WhiskDB Issue

Every task starts as a GitHub issue. One issue = one branch = one PR.

## Before creating

- Search for duplicates: `gh issue list --search "<keywords>" --state all`.
- Place it in the right milestone from `plan.md` (M1–M7).
- Keep it to one deliverable that can be finished and tested in roughly 1–2 days.
- If it depends on another issue, state the dependency in the body.

## Labels

Pick exactly one `area:*`, one `type:*`, and one `priority:*`:

- Areas: `area:core`, `area:network`, `area:sql`, `area:storage`, `area:wasm`, `area:client-sdk`, `area:cli`, `area:ci-docs`
- Types: `type:feature`, `type:test`, `type:docs`, `type:refactor`
- Priority: `priority:high`, `priority:medium`

## Template

Match the existing issues (`gh issue view 5`):

```markdown
## Overview
One paragraph: what this delivers and why it matters.

## Architectural & DuckDB-Style C++23 Design Notes
- Key design decisions, data formats, limits, ownership.
- Zero-copy / buffer-reuse expectations.
- Error policy: what returns `Result<T>`, what is a `D_ASSERT`.

## Target Files
- `include/...`
- `src/...`
- `tests/...`

## Acceptance Criteria
- [ ] Specific, observable behaviour.
- [ ] Boundary and failure behaviour.
- [ ] Docs/config updated if needed.

## Strict Testing & Quality Gates
- [ ] Happy-path test.
- [ ] Boundary test (zero/empty/max).
- [ ] Negative/error test.
- [ ] ASAN/UBSAN clean if memory or threads are touched.
- [ ] `ctest --output-on-failure` passes.
```

## Create it

```sh
gh issue create \
  --title "<Area>: <imperative summary>" \
  --label "area:network,type:feature,priority:high" \
  --milestone "M2: Zero-Copy TCP Networking & Wire Protocol" \
  --body-file /tmp/issue.md
```

## Quality bar

- Acceptance criteria are testable, not vague ("fast", "robust").
- If performance matters, state the metric and how it will be measured.
- Explicitly list what is out of scope.
- No issue should require more than one PR to close.
