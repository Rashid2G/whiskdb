---
name: Feature Request
about: Suggest an idea or new capability for WhiskDB
title: 'feat(<scope>): '
labels: ['enhancement']
assignees: ''
---

## Summary & Problem Statement
A concise description of what feature is needed and the problem or use case it addresses.

## Proposed Solution & Architecture
Detailed description of the proposed design following DuckDB-style C++23 patterns and WhiskDB architecture:
- Target components (`src/sql/`, `src/storage/`, `src/network/`, `app/`, etc.)
- Memory management & zero-copy considerations
- Durability and transaction lifecycle impact

## API & CLI Changes
Describe any changes to:
- SQL grammar / syntax
- Client CLI commands (`whisk ...`)
- Server options or `config.toml`

## Alternatives Considered
A clear description of any alternative solutions or features considered and why they were rejected.

## Acceptance Criteria & Test Plan
- [ ] Unit tests added in `tests/` covering normal and boundary cases.
- [ ] Deterministic end-to-end tests passing via `cmake run-tests`.
- [ ] Zero compiler warnings (`-Wall -Wextra -Wpedantic`) and 0 ASAN/UBSAN errors.
- [ ] Documentation updated in `docs/`.

## Additional Context
Add any other context, benchmarks, or references here.
