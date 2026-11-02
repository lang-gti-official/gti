# GTI Repository Guidance

**Canonical host (2026-10-29): [GitHub](https://github.com/lang-gti-official/gti).**
`origin` must use `https://github.com/lang-gti-official/gti.git` for source,
branches, PRs, issues and CI, and toolchain releases (compiler archives and the
Neovim plugin's downloads) move to GitHub Releases. Gitea
(`git.internal.howetotech.dev`) keeps only the standalone reference website and
the `gti add` package registry (`gti-lang` library packages such as libc); its
`gti-lang/gti` `main` is frozen and accepts no merges. GitHub
history carries rewritten commit identities (identical trees, new commit IDs),
so Gitea-era branches and task records move over by tree identity (`rehost`).
Every Gitea issue was migrated to GitHub with a new number. An issue or PR
number cited before 2026-10-29 is a Gitea number: translate it with
`docs/plans/archive/gitea-issue-map.md`.
When resuming an old worktree, read the current primary checkout's guidance and
workflow skill, and use its helper with `--repo <task-worktree>`.

GTI is an independent, experimental, statically typed systems language with
strong C++ syntax familiarity, explicit ownership, and deterministic cleanup.
Its goal is a fully usable systems language with excellent safety and sound
architecture. LLVM is the primary development backend. The C++ backend is
deprecated as the primary backend and receives bug fixes only through v1.0,
preserving its existing supported behavior as a comparison oracle. The shipped
default remains C++ until LLVM passes its production-promotion gates; that
transitional default does not authorize parallel feature development. GTI is
not a C++ wrapper, and neither backend defines its semantics.

## Backend Development Policy

- Prioritize new backend capabilities, optimization and feature implementation
  in LLVM. Do not implement C++ feature parity or catch-up work during the
  pre-1.0 campaign. C++ changes are limited to bug fixes needed to preserve its
  existing behavior and usefulness as an oracle, including repairs for shared
  interface changes. Do not disguise missing feature support as a bug fix.
- Retain valid C++ regression and differential checks. Where C++ lacks support
  or has a documented defect, require independent GTI semantic/MIR and native
  execution evidence; do not weaken LLVM acceptance, silently skip a comparator,
  or extend C++ merely to make a new LLVM feature testable.
- After v1.0, C++ catch-up may be reconsidered by an explicit owner decision;
  development does not automatically resume and parity is not promised. This
  policy does not remove the backend or prematurely change CLI defaults.
- See [ADR 031](docs/decisions/031-native-llvm-backend-foundation.md#backend-development-policy-2026-10-25)
  for the budget rationale, support distinction and promotion boundary.

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

- At the start of each new task, rename your own chat's sidebar title to a
  short, specific description of the current work; update it when the task
  changes. Use the available chat-title control (for example,
  `set_thread_title`, targeting the current chat). On successful completion,
  rename it `Complete — <task>`. Pending validation, a handoff, a blocker or
  cancellation is not completion: use an accurate status instead. If this
  client has no title control, state that limitation in the task update and
  continue; never claim the sidebar was renamed or rename another agent's chat.
- Apply [agent-workflow](.codex/skills/agent-workflow/SKILL.md) to implementation
  tasks. The owner's standing authorization is to publish task checkpoints,
  open/update the task PR, merge validated in-scope work through that PR, sync
  primary local `main`, and safely clean up that task's integrated branch and
  worktree. An implementation is normally complete when local `main` contains
  the merged change and is ready for the user's local install script.
- Explicit requests such as "PR only", "leave local", "do not merge", or
  "review first" override automatic integration. Read-only reviews/discussions
  do not authorize implementation or publication. Installation remains opt-in.
- **Contributors validate their change; the merge queue validates
  integration.** Before a task is complete, run only the focused checks its
  changed invariant needs: the exact reproducer or new regression (for a fix,
  failing on the parent revision for the intended reason), the owning
  subsystem's tests, and the affected cross-phase and tooling consumers, plus
  `compiler_pipeline` and the native smoke. Record exactly what ran with
  `verify`. Never run the complete suite locally to finish a feature or fix:
  the merge queue runs it on the exact candidate that becomes `main`, on every
  required platform. Deferring it to the queue is complete work, not a gap;
  never describe focused evidence as full validation. Run broad local suites
  only to diagnose an integration failure assigned to your change (and then
  only its failing tests), when the owner asks, or in the degraded mode below.
- Classify by [risk](docs/architecture/agent-workflow.md#risk-based-batch-validation),
  which decides how broad the focused checks are, never by the word "ICE" or
  patch size. T1 confined fixes need a meaningful regression, owning and
  affected cross-phase checks plus compiler smoke; T2 semantic/shared compiler
  changes add relevant subsystem coverage; T3 system, ABI/runtime and
  optimizer-invariant changes add their platform, runtime and performance
  gates. Record the original failure where practical, with bounded execution
  for resource-exhaustion cases and an explicit reason when reproduction is
  unavailable. Code-generation fixes must check resulting execution, cleanup
  and rejection behavior where applicable, not just absence of the crash.
- **CI is the required integration gate.** The `main` ruleset requires pull
  requests, the merge queue and the `CI gate` check, with no bypass for any
  role. A PR's `CI gate` runs the fast checks (lint, workflow regressions, and
  a macOS arm64 and Linux x64 build with `feedback-fast` and
  `compiler_pipeline`); the queue's `CI gate` runs the complete suite in
  cost-balanced bundled-release shards on both platforms, the sanitizer subset
  and installed-toolchain checks, produces the production archives, and proves
  the shards covered every registered test. Explicitly allowlisted orchestration-only changes run lint and workflow
  regressions instead; compiler/runtime, build/packaging, validation selection,
  mixed and unknown changes retain full integration coverage. See the CI
  classification contract in `docs/architecture/agent-workflow.md`.
  A failure in the queue belongs to the change that introduced it: its
  owner repairs it, and other agents never weaken unrelated expectations to
  accommodate it. Do not start a local suite to duplicate a queue run.
- Degraded mode: if GitHub Actions is unavailable for more than four hours, a
  designated integrator may validate one combined candidate with `batch` on a
  remote runner and record that evidence on the PR. Agents never fall back to
  per-PR local full suites.
- Every completed implementation task must be submitted to the `preview` source branch through
  `python3 scripts/agent_workflow.py preview submit <task> --tests <focused-ctest-names>`
  after focused validation passes and its PR is published, unless the user explicitly
  requests no preview or no publication. No additional permission is needed.
  Original PRs continue full validation and review. Submission is
  detached: do not wait for it before normal integration, merge,
  or task cleanup. Preview failure/conflict/staleness is not a main-PR blocker.
  The helper owns the preview queue, candidate checkout, validation and publication;
  use `preview status` only when its outcome is relevant. Select tasks explicitly; never sweep unrelated open
  PRs into preview. Admission rebuilds the combined source and requires every
  included feature's registered focused tests plus the automated example gate.
  Follow [the preview contract](docs/architecture/agent-workflow.md#early-combined-preview),
  including its platform/interactive coverage, exact inclusion records and
  append-only removal. Preview is never merged wholesale into main and never
  weakens the original PR's acceptance.
  `install_neovim_local.py` defaults to preview; `--local` selects the calling
  checkout. Running the installer still requires an installation request.
- GitHub at `https://github.com/lang-gti-official/gti.git` (remote `origin`)
  is canonical for branches, PRs, issues and CI. The helper authenticates
  through the GitHub CLI (`gh auth login`) or `GH_TOKEN`, never a stored
  token. Gitea keeps only the reference website and the `gti add` package
  registry; nothing else is published there. Gitea-era task records move over with explicit
  `rehost <task> --pr <number>`, which requires the migrated PR head to carry
  the task's published tree; never reuse a historical PR number by assumption
  or rewrite a live verification record.
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
  local suite slot itself; `verify --focused` instead permits automation-only
  or source-only GTI stdlib owning checks directly after checking the diff.
  Source-only stdlib changes may include their GTI fixtures/examples, Python
  test harnesses, census metadata and documentation, but not compiler, native
  runtime or build-system changes. Run exact owning checks with an optimized
  compiler; full builds and broad suites still require the shared window.
  Compiler components
  use structured `batch enroll`, not this exception.
  `verify --remote-steps-file <plan.json>` instead uses
  the configured remote host's window. `build_window.py --status` shows local
  holders and `remote_runner.py status` shows remote holders.
  T1 batch admission uses exact focused checks directly; T2/T3 admission uses
  a suite reservation for its broader subsystem checks. Do not expand the
  shared window or steal reservations to get around a queue.
- Offload heavy validation with `python3 scripts/remote_runner.py run ...`.
  Select `--runner mini`, `--runner ubuntu-6`, or `--runner ubuntu-8` before the
  subcommand, or set `GTI_REMOTE_RUNNER` for remote lifecycle verification.
  Inspect `status --all` to spread work across idle, platform-appropriate hosts;
  Linux checks do not replace required macOS evidence. The wrapper snapshots
  the selected worktree, uses that host's shared build window, retrieves logs/results,
  and removes remote source/build trees even after failures or cancellation.
  Use `--detach`, `status` and `fetch` for asynchronous work. Follow the
  [remote-runner contract](docs/architecture/agent-workflow.md#remote-buildtest-hosts);
  detached run IDs remember their original connection. Do not leave ad hoc
  clones or builds on any runner.
- Use `scripts/agent_workflow.py` for lifecycle operations. `finish` marks the
  PR ready and either labels it for a rollup (phase `rollup`), which the rollup
  bot combines with other green PRs into one queue entry, or, for Risk-system
  diffs and `--no-rollup`, registers GitHub merge-when-ready for the verified
  head. GitHub waits for required PR checks/reviews (`awaiting-ci`) and enters
  the merge queue (`queued`) automatically. `rollup`, `awaiting-ci` and `queued`
  are completion points with recorded focused validation; do not poll healthy
  CI or retry `finish` just to enter the queue. Failures/conflicts still need
  repair. Use helper checkpoints/reconciliation for later edits: they withdraw
  pending merge intent before publishing a new head, which needs fresh validation.
  `sync` advances local `main` once GitHub merges the PR.
  Reconcile only to resolve conflicts: an advancing `main` does not void
  focused evidence. No direct pushes to `main`, rewritten shared history, or
  administrative bypasses. The owner authorizes the helper's
  `protection --configure` to establish the `main` ruleset.
- Preserve unrelated primary-checkout edits and unique commits during sync.
  Confirm remote integration before archiving/removing a task worktree or
  deleting its branches. Preserve needed ignored files and any residual source
  first. Explicitly mark and publish abandoned/superseded work; never silently
  discard it. See [the lifecycle contract](docs/architecture/agent-workflow.md).

## Execution Discipline

- Preserve analysis quality and every required validation gate. There is no
  arbitrary cap on design review, reasoning depth, or reconsidering a decision.
  Counterexamples, uncertain invariants, alternative designs, performance and
  maintainability concerns are valid reasons to investigate further, even when
  no external code has changed. At checkpoints, retain the question, findings
  and remaining proof obligations so review resumes from evidence.
- Bound routine tool/log output to at most 200 lines and 16 KiB per response,
  including the aggregate of nested tool results. Save build/test output and
  structured evidence under the normal local/remote retention contract; expose
  a summary and log path, then targeted excerpts. Use
  `python3 scripts/bounded_log.py <log>` for saved log tails (default 40 lines,
  8 KiB). Line limits alone do not bound giant JSON lines. A larger investigation
  needs a stated question and focused chunks, not a wholesale dump of logs,
  ignored files, session history, or repository trees. These are display limits,
  not permission to shorten execution or conceal failures/truncated evidence.
- Read activated skill instructions completely. Use the documentation index
  and heading/search queries to select relevant canonical-document sections;
  do not dump entire manuals by default. Preserve read/decision state across
  compaction within a turn, and revisit changed or unresolved material.
- Retain each live session handle or remote run ID. Resume/wait on that same
  operation; do not launch duplicate validation because output is quiet. Use
  interruptible product waits, with individual waits no longer than 60 seconds.
  For remote status polling, start at 30–60 seconds and back off to 2–5 minutes
  when unchanged, using bounded interruptible waits between polls. A normal
  queue is not a blocker. Investigate stalls from elapsed time, timeouts and
  liveness evidence, not silence alone; never steal another agent's slot.
- If only an external result remains, wait rather than filling time with
  repeated status/document reads or artificial review work. Independent,
  useful analysis may continue; waiting is not a prohibition on deeper review.
  Keep a compact acceptance/evidence ledger with source identities, commands,
  results, log paths, unresolved findings and pending handles. Resume that
  ledger after compaction instead of restarting completed audits.
- Reconcile near the final validation boundary. If main advances during a run,
  retain its exact-head evidence, inspect the upstream delta, then perform fresh
  proportionate validation on the reconciled head/base. Re-run every affected,
  failed or required broad/platform gate; never relabel old evidence as current
  or weaken safety, performance, optimized-compiler or PR requirements to save
  time. Explain materially increased cost or duration to the user with the
  reason, completed evidence and remaining work.

Detailed procedures live in the
[execution contract](docs/architecture/agent-workflow.md#bounded-output-analysis-and-waiting).

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

The nightly, beta and stable channels serve different release stages.
Toolchain releases publish to GitHub Releases on `lang-gti-official/gti`.
Full integration in `.github/workflows/ci.yml` builds and validates the nightly
archives for `linux-x64` and `darwin-arm64` on GitHub-hosted runners;
`nightly.yml` publishes those existing artifacts without rebuilding. The release
train (`train.yml`, `beta.yml`, `backport.yml`, `rollback.yml`, logic in
`scripts/release_train.py`) retains versioned beta builds and byte-for-byte
stable promotion. `linux-arm64` waits
for a reviewed linux-aarch64 runtime compiler profile. The Neovim installer
downloads from GitHub Releases. The
frozen Gitea workflow (`.gitea/workflows/release.yml`) no longer runs. Do not
publish toolchain releases to Gitea. The `gti add` package registry stays
on Gitea and is unaffected. See "Published Toolchain Channels" in
`docs/architecture/build-and-driver.md`.

- **Preview** distributes the separately admitted combined `preview` source through
  `preview.yml`, with focused combined admission, installed checks, source and
  inclusion identity on both packaged platforms. It is not full-main CI or
  nightly evidence and never promotes into main. `gtiup` manages preview,
  nightly, beta and stable toolchains; Neovim managed mode uses the selected
  matching compiler/LSP/parser without downloading its own tools. Installation
  remains opt-in. Never move the rolling preview tag by hand.
- **Nightly** republishes one rolling `nightly` prerelease, daily, from the
  selected `main` commit with successful full CI and retained production
  artifacts. It verifies exact source/run/attempt, platform, configuration and
  checksums before publication; it does not compile or rerun the full suite.
  Merge-queue CI normally supplies this evidence. A full CI dispatch on main
  supplies commissioning/recovery evidence after a merge exception or missing
  artifacts. PR-only or automation-only passes cannot qualify. Missing evidence
  stops publication rather than relabelling an older compiler. Nightly replaces
  its own assets and needs no `VERSION` change.
- **Beta** is `v<VERSION>-beta.<K>`, a GitHub prerelease. Every 48 hours the
  train cuts the published nightly's commit, once it has been on `main` for 24
  hours, into a `Release-Cut:` PR that the merge queue lands like any other
  change; `beta.yml` then builds that cut commit, opens the
  `beta/<major>.<minor>` line and publishes beta 1. Fixes reach a line only as
  `git cherry-pick -x` backports of `main` commits through `backport.yml`; a
  user-visible backport ends with a patch release cut.
- **Stable** is `v<VERSION>`, the channel someone pins to and the tag Lazy's
  `version = "*"` resolves (it ignores prerelease tags). The train promotes a
  beta that soaked 48 hours with no open `release-blocker` issue naming its
  version, behind the protected `stable` environment, by republishing the
  beta's archives byte for byte. Stable tags and releases are immutable.
  `rollback.yml` withdraws a beta or yanks a stable version through the
  `release-status` branch, which the installer reads; it never installs a
  yanked version or another version in its place.

Never push a `v*` tag or the `release-status` branch by hand, change a
`beta/*` line only through a backport PR, and never move or delete any of them.
See "GitHub release train" in `docs/architecture/build-and-driver.md`.

Pull requests never edit `VERSION` or `CHANGELOG.md`. A change a toolchain user
would observe (language surface, CLI or driver behavior, diagnostics they would
see, packaged contents, or a fix to any of those) adds one
[changelog fragment](changes/README.md), `changes/<name>.<kind>.md`, with kind
`breaking`, `feature` or `fix`. Invisible work (internal refactors, moving
authority between compiler phases, tests, documentation, plans) needs no
fragment, or an `internal` one, or the `Release-Impact: internal` trailers.
A release cut (`scripts/cut_release.py`) assembles pending fragments into one
CHANGELOG section and the next `VERSION`; only its `Release-Cut:` commit
changes `VERSION`, so concurrent pull requests never collide on a version.

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
