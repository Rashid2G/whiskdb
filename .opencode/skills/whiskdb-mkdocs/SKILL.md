---
name: WhiskDB MkDocs
description: Update WhiskDB documentation — explains what each MkDocs page is for, the required concise style, and how to build and verify docs. Use when adding or changing docs, config tables, CLI usage, or architecture notes.
---

# WhiskDB MkDocs

## Page purposes

| Page | Purpose | Keep out |
|---|---|---|
| `docs/index.md` | What WhiskDB is, why it exists, academic/team info, links | Deep internals, changelogs |
| `docs/development.md` | Scope, objectives, milestone/progress status, external libraries | Coding standards (link instead) |
| `docs/developer-guide.md` | Normative engineering rules: C++ style, naming, error policy, architecture, Git workflow | User tutorials |
| `docs/user-guide.md` | Install, configure, run, `whisk` CLI and SDK usage | Contributor rules |

Navigation lives in `mkdocs.yml`. Add a page there only if a new top-level page is truly needed.

## Style

- Short and factual; prefer a table over prose.
- Keep existing headings and structure; make surgical edits, never rewrite a page.
- Do not duplicate rules across pages — define once and link.
- Use relative links (`developer-guide.md#...`), fenced code blocks with a language.
- Commands must be copy-pasteable and match the actual CLI.
- Mark planned features as planned; never document unimplemented behaviour as available.
- Admonitions (`!!! note`, `!!! info`) for scope warnings, matching existing pages.

## Workflow

1. Change only the section affected by the PR.
2. Update existing tables/commands rather than appending new sections.
3. Build and check for broken links:
   ```sh
   mkdocs serve            # live preview at http://127.0.0.1:8000
   mkdocs build            # generated output in site/
   ```
4. `site/` is gitignored build output — never edit or commit it.
5. Commit as `docs: ...` and keep docs-only changes in their own PR when possible.

## Documentation PR checklist

- [ ] Only affected pages changed.
- [ ] No duplicated or contradictory rules.
- [ ] Commands and paths verified against the repo.
- [ ] `mkdocs build` succeeds.
- [ ] New page (if any) added to `mkdocs.yml` nav.
