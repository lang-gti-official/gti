# GTI — Claude Code Guidance

**Read [`AGENTS.md`](AGENTS.md) first.** It is the canonical, agent-neutral
repository guidance, including the owner's standing authorization for
implementation tasks. This file adds only what is specific to Claude Code and
does not restate it. Put durable rules in `AGENTS.md`, not here.

## Orientation

- [`docs/index.md`](docs/index.md) is the documentation map. Read it before
  substantial compiler, language, runtime, build, or LSP work.
- [`docs/architecture/overview.md`](docs/architecture/overview.md) has an
  "Authority By Layer" table near the top. That table is the fastest way to
  decide which layer owns a change.
- [`docs/plans/implementation-sequence.md`](docs/plans/implementation-sequence.md)
  is the single active work queue. Before executable language work, check its
  Ready Queue and the campaign sections at the top of the file; an exclusive
  campaign limits what may proceed. The backend-authority recovery campaign is
  complete, and its permanent rule remains: new executable features extend
  verified MIR rather than adding an AST/HIR-only backend path.
- `AGENTS.md` sections that older Claude guidance did not reflect:
  **Language Direction**, **Implementation Task Lifecycle**,
  **Release Channels And VERSION**,
  **General Semantics Before Client Exceptions**, and **Constexpr Evolution**.

## Task Lifecycle

Canonical `origin` is `https://git.internal.howetotech.dev/gti-lang/gti.git`.
If an older worktree still names GitHub, follow the current primary checkout's
`AGENTS.md` and workflow skill, not that worktree's stale hosting instructions.

An implementation task ends with a merged Gitea PR and a synchronized primary
`main`, through the `agent-workflow` skill and
[`docs/architecture/agent-workflow.md`](docs/architecture/agent-workflow.md).
`main` is protected against direct pushes. Read-only reviews and audits create
no branch or PR, and explicit "PR only", "leave local", or "review first"
instructions override integration. What is specific to Claude Code:

- Claude Code's managed worktrees live under `.claude/worktrees/`, inside the
  primary checkout, and `scripts/agent_workflow.py` refuses them. Let its
  `start` command create the task worktree at an absolute path outside the
  primary checkout (the session scratchpad directory works) and do the task
  there. Leave the session's managed worktree clean; the app archives it.
- Task branches use the `codex/` prefix whatever the client; the helper
  enforces it.
- The Git stash stack is shared by every worktree and session. Never use a bare
  `git stash` or `git stash pop`.
- Full builds and full suites go through the shared build window, because
  several Claude Code sessions commonly validate at once on this machine:
  `python3 scripts/build_window.py --kind build -- cmake --build build-release -j 8`
  and `python3 scripts/build_window.py --kind suite -- ctest --test-dir build-release -j 12 --test-load 12`.
  Focused `ctest -R '^name$'` runs and single-file `gti` compiles need no slot.
  The helper's `verify` takes a suite slot itself and no longer holds the
  metadata lock while checks run, so `start --adopt`, `status`, `checkpoint`
  and `cleanup` do not wait on a peer's validation. Check
  `python3 scripts/build_window.py --status` before starting heavy work.

## Skills

Project skills live in [`.claude/skills/`](.claude/skills). Invoke them with
the Skill tool, or `/<name>`:

| Skill | Use for |
| --- | --- |
| `agent-workflow` | task branch, checkpoints, PR merge, local-main sync, cleanup |
| `implement-language-feature` | GTI syntax or language semantics |
| `implement-build-feature` | driver, `gti.toml`, projects, packaging |
| `add-diagnostic` | new or revised compiler diagnostics |
| `lsp-synchronization` | `gti_lsp`, editor tooling, Tree-sitter |
| `compiler-architecture-review` | structural or ownership changes |
| `verify-change` | selecting and running proportionate checks |
| `finish-release` | `VERSION` and `Release-Impact` trailer policy for PR delivery |

`AGENTS.md` points agents at [`.codex/skills/`](.codex/skills), which hold the
canonical workflows. Each `.claude/skills/<name>/SKILL.md` ports its Codex
counterpart, because Claude cannot act on Codex's `$skill` invocation syntax or
its `agents/openai.yaml` manifests. The intended differences are invocation
wording, the planner path in `verify-change`, and Claude Code worktree handling
in `agent-workflow`. When a workflow changes in substance, update both copies in
the same change. If they disagree, the Codex copy is canonical: fix the Claude
copy to match it.

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
6. **A language rule is not a library exception.** State it without naming a
   `std::*` type and prove it with an unrelated user-defined type; do not add
   nominal compiler cases for one view, range, callback, or container.
7. **Grow `constexpr` toward MIR execution.** Read the `L-CONST-01` direction
   before extending any constant context, and do not leave two permanent
   execution rules for one operation.

## Working Notes

- Prefer updating an existing canonical document over adding a new one. The
  repository deliberately avoids `v2`, `-final`, and implementation-note files.
- Documentation carries the project's state record; commit messages are terse
  and `git log` is not a reliable guide to direction. Read the owning plan and
  architecture documents instead.
- Build directories (`build/`, `build-release/`, `build-asan/`,
  `build-sanitized/`) are generated. Keep them out of diffs.
