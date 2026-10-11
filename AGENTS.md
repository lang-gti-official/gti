# GTI Repository Guidance

GTI is an independent, experimental, statically typed systems language with
strong C++ syntax familiarity, explicit ownership, and deterministic cleanup.
Its goal is a fully usable systems language with excellent safety and sound
architecture. The reference compiler currently generates native code through
C++; GTI is not a C++ wrapper, and that backend does not define its semantics.

## Language Direction

- Design general GTI facilities that let applications and libraries implement
  systems abstractions in ordinary source. Prefer source-defined public policy
  wherever its invariants can be expressed, while retaining compiler/runtime
  mechanisms for layout, object lifetime, checked ownership, target operations,
  or other facts that source cannot establish. Justify that split per operation;
  source-defined does not mean removing necessary compiler support.
- Keep C++ spelling and familiarity strong when they fit coherent GTI semantics.
  Neither C++ compatibility nor generated C++ behavior is the acceptance rule.
  Safe guarantees and explicit unsafe obligations belong to GTI itself.
- Earlier assumptions, bounded slices, private capabilities, and deferred work
  may have been appropriate for an earlier language. Reassess their rationale
  against current capabilities and the systems-language goal. Record an explicit
  revision when a decision changes; historical restrictions are not permanent
  requirements merely because they were once accepted.
- Describe remaining work as goals with named prerequisites, current evidence,
  and acceptance criteria. Missing support calls for implementing or scheduling
  the prerequisite, rather than treating it as a reason to abandon the outcome.
  Preserve honest implementation status and mandatory safety/verification rules;
  goal-oriented planning does not authorize unvalidated behavior or bypasses.

## Start Here

- Read [`docs/index.md`](docs/index.md) before substantial compiler,
  language-design, runtime, build, or LSP work.
- Treat source code and tests as the authority for the current implementation
  when documentation disagrees. Record and resolve the disagreement rather
  than guessing intent.
- Use repository skills under [`.codex/skills/`](.codex/skills/) for repeatable
  workflows. Skills describe how to work; they do not replace canonical docs.

## Implementation Task Lifecycle

- Apply [agent-workflow](.codex/skills/agent-workflow/SKILL.md) to implementation
  tasks. The owner's standing authorization is to publish task checkpoints,
  open/update the task PR, merge validated in-scope work through that PR, sync
  primary local `main`, and safely clean up that task's integrated branch and
  worktree. An implementation is normally complete when local `main` contains
  the merged change and is ready for the user's local install script.
- Explicit requests such as "PR only", "leave local", "do not merge", or
  "review first" override automatic integration. Read-only reviews/discussions
  do not authorize implementation or publication. Installation remains opt-in.
- GitHub (`gti-lang-official/gti`, remote `origin`) is the canonical host for
  branches, pull requests, issues, CI and releases. The self-hosted Gitea
  repository (remote `gitea`) is a backup mirror of `main` and the tags that
  the helper's `sync` pushes after every integration; never open PRs or
  issues there.
- Begin from current canonical `origin/main` in a clean task-specific branch
  and worktree. Publish the branch immediately; create a draft PR after the
  first meaningful checkpoint. Commit and push coherent checkpoints before
  pauses/handoffs and at completion. Do not leave meaningful task work only in
  an opaque local checkout or reuse another task's branch.
- Run every full build and every full or broad test suite through the shared
  build window, whether or not the helper runs it:
  `python3 scripts/build_window.py --kind build -- cmake --build <tree> ...` and
  `python3 scripts/build_window.py --kind suite -- ctest --test-dir <tree> ...`.
  The window is one counted semaphore under the Git common directory, shared
  by every worktree and session on the machine, so concurrent agents stop
  stacking full suites on each other. Focused tests (`ctest -R '^name$'`, one
  harness fixture) and single-file compiles run directly. `verify` takes a
  local suite slot itself; `verify --remote-steps-file <plan.json>` instead uses
  the configured remote host's window. `build_window.py --status` shows local
  holders and `remote_runner.py status` shows remote holders.
- Offload heavy validation with `python3 scripts/remote_runner.py run ...`.
  Configure the shared SSH connection once; the wrapper snapshots the selected
  worktree, uses the Mac mini's shared build window, retrieves logs/results,
  and removes remote source/build trees even after failures or cancellation.
  Use `--detach`, `status` and `fetch` for asynchronous work. Follow the
  [remote-runner contract](docs/architecture/agent-workflow.md#mac-mini-remote-runs);
  do not leave ad hoc clones or builds on the Mini.
- Use `scripts/agent_workflow.py` for lifecycle operations. Reconcile upstream,
  validate the resulting head/base, and merge only through GitHub PRs. Respect
  required checks/reviews. No direct pushes to `main`, rewritten shared history,
  or administrative bypasses. The owner authorizes the helper's `protection
  --configure` to establish this PR-only/up-to-date rule; preserve existing
  review and status requirements.
- Preserve unrelated primary-checkout edits and unique commits during sync.
  Confirm remote integration before archiving/removing a task worktree or
  deleting its branches. Preserve needed ignored files and any residual source
  first. Explicitly mark and publish abandoned/superseded work; never silently
  discard it. See [the lifecycle contract](docs/architecture/agent-workflow.md).

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
6. Every new language feature must ship with an accompanying runnable example
   under `examples/` in the same change. Add a focused example or extend an
   existing one to demonstrate the feature explicitly, including its intended
   use and important supported boundaries. Test fixtures and documentation
   snippets alone do not satisfy this requirement. Validate the example with
   an optimized GTI compiler and update the applicable example index, corpus
   registration, and census expectations. A feature is not complete without
   its example and passing example evidence.
7. Keep generated, vendored, and unrelated user files out of task diffs.

## Release Channels And VERSION

Two channels publish this repository, and they answer different questions.
Both publish from the GitHub release workflow (`.github/workflows/release.yml`)
of the canonical repository `gti-lang-official/gti`, for Linux x64 and Apple
Silicon; the Gitea mirror publishes nothing.

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
  may also establish the general optimization architecture needed for efficient
  systems programs; a pre-existing benchmark regression is not a prerequisite.
- Treat 1.0 as a soft, revisable goal meaning a full-featured language ready
  for systems programming, not as a horizon used to defer essential
  capabilities. Preserve GTI's accepted safety and phase-authority rules while
  pursuing that goal.

## Runtime Performance Requirement

- Runtime performance is a first-class design and delivery requirement, not
  optional follow-up work after feature completion. Design language facilities,
  IR, failure paths, library representations and allocation policy for efficient
  successful execution from the outset.
- Architectural reasoning about unnecessary work, abstraction boundaries and
  missing general optimization support is sufficient to justify performance
  work. Do not require a benchmark, proxy speedup or user-reported regression
  before designing or implementing that support. Name the affected operation
  families, expected cost model and correctness obligations instead.
- Measurement validates results; it is not permission to begin. For changes
  affecting generated execution, validate representative optimized workloads
  and report runtime, allocation, code-size and relevant build/memory costs.
  Do not claim speedups without evidence or parity between different contracts.
- A material runtime regression fails performance acceptance unless the owner
  explicitly accepts the documented tradeoff. Passing correctness tests or
  opening a follow-up issue does not waive that requirement. Preserve safety,
  unsafe obligations, defined failure and phase authority; performance does not
  authorize unchecked safe APIs or nominal compiler exceptions.
- Follow [ADR 012](docs/decisions/012-outcome-first-systems-readiness.md#runtime-performance-requirement-2026-10-09).

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
