---
name: WhiskDB MkDocs
description: Update WhiskDB documentation — explains what each MkDocs page is for, text styling standards inspired by industry-standard docs (Stripe, DuckDB, Tailwind), and how to build and verify docs. Use when adding or changing docs, config tables, CLI usage, C++ SDK guides, or architecture notes.
---

# WhiskDB MkDocs

## Page Purposes & Ownership

| Page | Purpose | Keep Out |
|---|---|---|
| `docs/index.md` | What WhiskDB is, why it exists, high-level architecture, team & capstone info, entry links | Deep low-level internals, changelogs |
| `docs/development.md` | Scope, objectives, milestone/progress status, durability trade-offs, external libraries | Coding standards (link instead) |
| `docs/developer-guide.md` | Normative engineering rules: C++23 style, naming, error & `D_ASSERT` policy, internal engine architecture (SQL, Storage, Network, SDK, CLI), Git workflow | End-user tutorials & basic install walkthroughs |
| `docs/user-guide.md` | User-facing usage: `whiskdb` server options, `whisk` CLI reference & REPL, C++ Client SDK (`WhiskClient`), query handling, configuration | Internal contributor policies & contributor PR checklists |

Navigation lives in `mkdocs.yml`. Add a page there only if a new top-level page is truly needed.

---

## Documentation & Text Style Standards

WhiskDB documentation adopts the scannable, developer-first writing standards of industry leaders (Stripe, DuckDB, Tailwind CSS):

### 1. Structure & Layout
- **Lead Paragraphs**: Every page and major H2 section starts with a concise 1–2 sentence summary explaining *what* it is and *why* it matters before diving into details.
- **Progressive Disclosure**: High-level concepts first, followed by concrete examples, followed by reference tables.
- **Reference Tables over Prose**: Use Markdown tables for CLI options, API signatures, configuration parameters, opcodes, and type mappings. Never write a paragraph listing flags.
- **Visual Callouts (Admonitions)**: Use `!!! note "..."`, `!!! tip "..."`, `!!! warning "..."`, or `!!! info "..."` to highlight trade-offs, performance nuances, or prerequisites.

### 2. Tone & Voice
- **Active & Direct**: Use the imperative mood for commands and actions ("Run `whisk repl`", "Configure the server port", "Initialize `WhiskClient`"). Avoid passive voice ("The command can be run by the user").
- **Concise & Factual**: Strip fluff, buzzwords, and redundant filler. State numbers, defaults, and behaviors explicitly.
- **Present Tense**: Write in the present tense ("WhiskDB logs mutations to the WAL", not "WhiskDB will be logging...").
- **Planned vs. Available**: Explicitly label unreleased/planned features with an `!!! info "Planned Feature"` badge; never describe unimplemented behavior as currently working.

### 3. Formatting & Code Conventions
- **Code Identifiers**: Always use backticks for types, methods, filenames, flags, and config keys: `WhiskClient`, `QueryResult`, `config.toml`, `--durability`, `fdatasync`.
- **Fenced Code Blocks**: Always specify the language tag (`cpp`, `sh`, `toml`, `sql`, `json`).
- **Production-Quality C++ Snippets**: Code examples must be copy-pasteable, compile-ready, and adhere to WhiskDB's C++23 standards:
  - Reside in `namespace database` (never `using namespace std;`).
  - Use `idx_t` for counts/indices.
  - Use `Result<T>` for error checking.
- **Link Integrity**: Use valid relative markdown links (`[Developer Guide](developer-guide.md#build--test-macros)`). Never link to local files outside `docs/` using relative file paths unless they are published documentation pages.

---

## Workflow

1. Change the documentation pages affected by the feature or task.
2. Ensure no broken links or orphaned paths.
3. Test documentation building:
   ```sh
   ~/.local/bin/mkdocs build --strict    # Verifies zero broken links and clean compilation
   ~/.local/bin/mkdocs serve             # Live preview at http://127.0.0.1:8000
   ```
4. `site/` is gitignored build output — never edit or commit it.
5. Commit with conventional commit format: `docs: ...` or `docs(scope): ...`.

---

## Documentation PR Checklist

- [ ] Clear page structure with lead summaries and scannable reference tables.
- [ ] Active voice and concise imperative instructions throughout.
- [ ] C++ SDK and CLI examples are tested, accurate, and follow C++23 standards.
- [ ] External libraries and architecture references match the current codebase.
- [ ] Internal Markdown links point to valid targets (`mkdocs build --strict` exits 0).
