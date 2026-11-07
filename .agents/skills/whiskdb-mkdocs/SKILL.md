---
name: WhiskDB MkDocs
description: Update WhiskDB documentation — explains what each MkDocs page is for, text styling standards inspired by industry-standard docs (Stripe, DuckDB, Tailwind), and how to build, verify, and deploy docs to the dedicated gh-pages branch.
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

## Branching & Deployment Rules (CRITICAL)

> [!IMPORTANT]
> **Documentation belongs strictly to its own branch: `gh-pages`**.
>
> - **Never target `dev`**: Documentation updates must **never** be merged or committed to the `dev` branch or any other code branch.
> - **Dedicated `gh-pages` Deployment**: All documentation builds are compiled and deployed directly to the dedicated `gh-pages` branch using `mkdocs gh-deploy`.
> - **Do not open PRs to `dev` for documentation-only changes**: Deploy docs directly to `gh-pages` so the live GitHub Pages site is updated immediately.

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

1. Edit the documentation pages under `docs/` or update `mkdocs.yml`.
2. Verify documentation builds cleanly without warnings:
   ```sh
   mkdocs build --strict    # Verifies zero broken links and clean compilation
   mkdocs serve             # Optional: live local preview at http://127.0.0.1:8000
   ```
3. Deploy directly to the dedicated `gh-pages` branch:
   ```sh
   mkdocs gh-deploy         # Builds and pushes directly to origin/gh-pages
   ```
4. Never push or PR documentation-only files to `dev` or `master`.
5. Verify live site: https://Rashid2G.github.io/whiskdb/

---

## Documentation PR & Release Checklist

- [ ] Clear page structure with lead summaries and scannable reference tables.
- [ ] Active voice and concise imperative instructions throughout.
- [ ] C++ SDK and CLI examples are tested, accurate, and follow C++23 standards.
- [ ] External libraries and architecture references match the current codebase.
- [ ] Internal Markdown links point to valid targets (`mkdocs build --strict` exits 0).
- [ ] Deployed directly to `gh-pages` via `mkdocs gh-deploy` (not merged into `dev`).
