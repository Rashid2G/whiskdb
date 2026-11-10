---
name: Bug Report
about: Create a report to help us improve WhiskDB
title: 'fix(<scope>): '
labels: ['bug']
assignees: ''
---

## Description
A clear and concise description of what the bug is.

## Reproduction Steps
Steps to reproduce the behavior:
1. Configure or build using: `cmake ...`
2. Start server or invoke CLI: `whisk ...`
3. Execute SQL query or Whisk module: `...`
4. See error:

```sql
-- SQL snippet if applicable
```

## Expected Behavior
A clear and concise description of what you expected to happen.

## Actual Behavior & Logs
Output, stack traces, compiler errors, or sanitizer logs:

```text
<paste output / ASAN / UBSAN / logs here>
```

## Environment
- **OS**: Linux (e.g. Ubuntu 24.04, Debian Trixie)
- **Architecture**: x86_64
- **Compiler**: GCC 14 / Clang 18+
- **Commit / Branch**: `dev` @ `<commit-hash>`

## Additional Context
Add any other context, durability mode settings (`strict`, `group_commit`, `relaxed`, `memory_only`), or details about the problem here.
