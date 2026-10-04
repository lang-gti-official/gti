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
   toolchain checks in proportion to the change. Use a GTI compiler executable
   built with host optimization for every test that compiles or analyzes GTI
   (`Release`, `RelWithDebInfo`, or `MinSizeRel`); an unoptimized compiler is
   not valid verification evidence.
5. Update the canonical architecture or language document when implementation
   behavior changes. Update a plan when its status changes; do not describe
   planned behavior as current.
6. Keep generated, vendored, and unrelated user files out of task diffs.

## Release Channels And VERSION

Two channels publish this repository, and they answer different questions.

- **Nightly** republishes one rolling `nightly` release per commit on `main`.
  It is automatic, replaces its own assets, and needs no `VERSION` change.
  Every merged commit reaches users through this channel.
- **Stable** publishes `v<VERSION>` when `VERSION` changes. It is the channel
  someone pins to.

Advance `VERSION` only when a user of the toolchain would observe the change:
language surface, CLI or driver behavior, diagnostics they would see, packaged
contents, or a fix to any of those. Bump the patch component for a fix and the
minor component for new or changed surface.

Do **not** advance `VERSION` for work that is invisible from outside the
toolchain, including internal refactors, moving authority between compiler
phases, added tests, documentation, or plan updates. Nightly already delivers
that work. A commit that changes no user-visible behavior should leave
`VERSION` alone, and most commits during an internal campaign will.

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

## General Semantics Before Client Exceptions

- Separate the motivating library type or workload from the language rule that
  enables it. State the rule without naming a particular `std::*` type, and
  prove it with an unrelated user-defined type whenever the rule is intended to
  be general.
- A bounded first slice may deliberately support fewer shapes, but its boundary
  must be structural and recorded as such. Do not accumulate nominal compiler
  exceptions for individual views, ranges, callbacks, containers, or other
  public wrappers.
- For ownership, borrowing, and callable work, identify the shared semantic
  facts being extended: origin place or places, access mode, permitted escape
  boundary, loan endpoint, and transfer/drop behavior. Semantics owns those
  facts; HIR and MIR preserve or verify them; the backend must not rediscover
  them from representation.
- Before adding a new path, audit overlapping special cases. A generalization
  should remove or subsume them. If it cannot, explain why the new behavior is
  a distinct semantic family rather than another client-specific exception.
- Compiler-private capabilities remain appropriate only when ordinary GTI
  cannot express an invariant safely. Bind those capabilities by trusted
  declaration identity; never make a public standard-library spelling the
  source of language meaning.

## Constexpr Evolution

- Before extending `constexpr`, `static_assert`, compile-time branches, or any
  other constant context, read the `L-CONST-01` evaluator direction in
  [`docs/plans/implementation-sequence.md`](docs/plans/implementation-sequence.md#l-const-01-remaining-bounded-compile-time-programming).
  Treat the current semantic AST interpreter as implemented staging, not the
  desired place to duplicate each new runtime execution rule indefinitely.
- Distinguish semantic authority from the execution engine. Semantics still
  decides whether an operation is permitted in a constant context and consumes
  the resulting `ConstantValue`; this does not require program execution to
  remain implemented inside semantic analysis.
- Audit whether the requested slice can move the applicable function,
  constructor, control-flow, place, or aggregate behavior toward a verified
  body-local MIR constant-evaluation profile. Reuse MIR operations and typed
  identities rather than creating an unrelated CTFE bytecode or another type
  system.
- Preserve phase timing. `if constexpr`, static assertions, array extents, and
  value-generic arguments can shape semantic analysis before complete program
  HIR/MIR exists. Do not move evaluation after completed semantics without a
  staged or query-driven design that resolves that dependency.
- A bounded interim AST-evaluator addition must identify why MIR-backed
  migration is not coherent in the same slice, retain deterministic resource
  limits and source diagnostics, add compile-time/runtime differential evidence
  where runtime execution exists, and record the remaining migration work. Do
  not leave two permanent execution rules for the same operation.

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
