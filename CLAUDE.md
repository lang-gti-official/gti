# GTI — Claude Code Guidance

**Read [`AGENTS.md`](AGENTS.md) first.** It is the canonical, agent-neutral
repository guidance: documentation roles, the every-change checklist, outcome
priority, and repository boundaries. This file adds only what is specific to
Claude Code and does not restate it. Put durable rules in `AGENTS.md`, not
here.

## Orientation

- [`docs/index.md`](docs/index.md) is the documentation map. Read it before
  substantial compiler, language, runtime, build, or LSP work.
- [`docs/architecture/overview.md`](docs/architecture/overview.md) opens with
  an authority-by-layer table. That table is the fastest way to decide which
  layer owns a change.
- [`docs/plans/implementation-sequence.md`](docs/plans/implementation-sequence.md)
  is the single active work queue. Check its active row before starting
  executable language work — a backend-authority campaign may have paused it.

## Skills

Project skills live in [`.claude/skills/`](.claude/skills). Invoke them with
the Skill tool, or `/<name>`:

| Skill | Use for |
| --- | --- |
| `implement-language-feature` | GTI syntax or language semantics |
| `implement-build-feature` | driver, `gti.toml`, projects, packaging |
| `add-diagnostic` | new or revised compiler diagnostics |
| `lsp-synchronization` | `gti_lsp`, editor tooling, Tree-sitter |
| `compiler-architecture-review` | structural or ownership changes |
| `verify-change` | selecting and running proportionate checks |
| `finish-release` | commit, version, tag, and release dispatch |

`.codex/skills/` holds the Codex copies of the same workflows. They use Codex's
`$skill` invocation syntax and `agents/openai.yaml` manifests, neither of which
Claude can act on. Edit the copy under `.claude/skills/` for Claude and keep
`.codex/skills/` for Codex; when a workflow changes in substance, update both.

## Non-Negotiables

These are the invariants most easily violated by a model working from C++
intuition. `AGENTS.md` and the architecture docs are authoritative.

1. **Source and tests outrank documentation.** When they disagree, resolve the
   disagreement explicitly; do not guess intent.
2. **Never infer GTI semantics from C++ resemblance or from emitted C++.**
   Generated C++ is a representation, never a language definition.
3. **Keep phase authority directional** — syntax in lexer/parser/AST, resolved
   meaning in semantics, concrete instances in HIR, body-local control flow and
   effects in MIR, representation in backends. A later stage must not
   compensate for a fact the earlier stage should have produced.
4. **`docs/plans/` is not evidence of implementation.** Treat a plan milestone
   as unimplemented until source and tests prove otherwise.
5. **Run `git status --short` before starting** and preserve unrelated work.
   The tree often carries in-flight changes.

## Working Notes

- Prefer updating an existing canonical document over adding a new one. The
  repository deliberately avoids `v2`, `-final`, and implementation-note files.
- Documentation carries the project's state record; commit messages are terse
  and `git log` is not a reliable guide to direction. Read the owning plan and
  architecture documents instead.
- Build directories (`build/`, `build-release/`, `build-asan/`,
  `build-sanitized/`) are generated. Keep them out of diffs.
