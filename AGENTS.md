# GTI Repository Guidance

GTI is an experimental, statically typed systems language with C++-familiar
syntax, explicit ownership, deterministic cleanup, and a native C++ backend.

## Start Here

- Read [`docs/index.md`](docs/index.md) before substantial compiler,
  language-design, runtime, build, or LSP work.
- Treat source code and tests as the authority for the current implementation
  when documentation disagrees. Record and resolve the disagreement rather
  than guessing intent.
- Use repository skills under [`.codex/skills/`](.codex/skills/) for repeatable
  workflows. Skills describe how to work; they do not replace canonical docs.

## Documentation Roles

- `docs/architecture/` describes implemented compiler and tooling
  architecture.
- `docs/language/` describes implemented/current language semantics and the
  working specification.
- `docs/decisions/` records significant architectural and language rationale.
- `docs/plans/` describes proposed or incomplete work and is not evidence that
  a feature is implemented.

Prefer updating an existing canonical document over creating a new architecture
document. Before adding Markdown, decide whether the information belongs in an
existing architecture, language, decision, or plan document. Do not create
one-off implementation-note or redesign files when an existing category fits.

## Every Change

1. Run `git status --short` and preserve unrelated work.
2. Trace current behavior through the owning compiler layer; do not infer GTI
   semantics from C++ resemblance or emitted C++.
3. Keep phase authority directional: syntax in the lexer/parser/AST, resolved
   language meaning in semantics, concrete instances in HIR, body-local control
   flow and effects in MIR, and representation choices in backends.
4. Add focused positive and negative tests at the layer that owns the rule.
   Run broader relevant CTest, CLI, LSP, Tree-sitter, formatter, or installed
   toolchain checks in proportion to the change.
5. Update the canonical architecture or language document when implementation
   behavior changes. Update a plan when its status changes; do not describe
   planned behavior as current.
6. Keep generated, vendored, and unrelated user files out of task diffs.

## Outcome Priority

- Substantial compiler, language, runtime, or library work must name the
  user-facing program, API, or workflow it unlocks.
- Prefer the smallest coherent vertical slice through the applicable compiler,
  library, tooling, documentation, and test layers. Infrastructure-only work
  must fix a correctness issue or be the nearest prerequisite of a named
  outcome.
- Treat 1.0 as a soft, revisable goal meaning a full-featured language ready
  for systems programming, not as a horizon used to defer essential
  capabilities. Preserve GTI's accepted safety and phase-authority rules while
  pursuing that goal.

## Repository Boundaries

- `include/gti/`, `src/compiler/`: frontend, semantics, HIR, MIR,
  optimization, and backend contracts.
- `include/gti/driver/`, `src/driver/`: reusable compilation, project,
  artifact, and native-toolchain orchestration.
- `src/cli/`, `src/lsp/`: protocol and presentation entry points.
- `stdlib/`, `runtime/`: source-defined library policy and narrow host/runtime
  facilities.
- `tests/`, `examples/`: regression coverage and executable language examples.

Nested `AGENTS.md` files may add directory-specific rules. None currently
override this root guidance.
