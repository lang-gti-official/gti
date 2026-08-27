# Compiler Structural Refactor

> **Plan status:** Active structural gate. In-flight work was reconciled at
> `4d4c8ae242fe08511db8de41fcafe117dde06dba`; **R-STRUCT-00** is complete and
> **R-STRUCT-01** is complete and **R-STRUCT-02** is the active implementation
> slice. No new compiler feature
> campaign begins until the exit gate in this plan passes.
>
> This is an internal architecture plan. It does not define GTI language
> semantics and does not make generated C++ text a public compatibility
> contract.

## Outcome

The user-facing outcome is a compiler that can continue adding systems-language
and standard-library support without every change requiring edits to one
stateful analyzer, one whole-program verifier, one C++ body emitter, or one
compiler test executable.

The refactor must make these properties explicit:

- each compiler phase has one owner for mutable state and one directional
  dependency boundary;
- semantic registration, body analysis, concrete-instance analysis, MIR
  verification, optimization replay, backend admission, and text emission can
  be changed and tested independently;
- a focused implementation edit rebuilds only its owning translation units and
  their final link products;
- existing GTI semantics, diagnostics, optimized MIR, generated artifacts, and
  runtime behavior remain unchanged unless a separately reviewed correctness
  fix is required; and
- future backends continue to consume the self-contained `LoweredProgram`
  boundary rather than regaining AST, semantic-analyzer, or HIR body access.

This is not a line-count reduction project. A large file may remain large when
one invariant and one state owner genuinely require it. A smaller file must be
split when it combines independently mutable authorities.

## Gate Activation

The structural gate activates in two steps:

1. Work already in flight at adoption is allowed to complete its original
   scope. The integration owner reconciles those branches, runs the one broad
   validation pass, and records the resulting `main` commit as the refactor
   baseline.
2. From that baseline until **R-STRUCT-05** passes, no new compiler language,
   optimizer, backend, LSP-semantic, runtime-capability, or build-orchestration
   feature begins. Standard-library source work may continue when it needs no
   compiler change.

Allowed exceptions are narrowly scoped correctness or security fixes,
release/CI unblockers, and work explicitly required by the active structural
slice. An exception must preserve the structural baseline, add a focused
regression, and may not introduce a second authority or broaden a feature.
Documentation and editor presentation work may continue only when it does not
change compiler semantics or consume a module currently being migrated.

This gate does **not** pause or repurpose work that was already active when the
plan was written.

## Evidence Snapshot

The initial audit used `origin/main` at `8b15ffeb` on 2026-08-27. Re-run the
inventory at the reconciled baseline because active work may move the exact
line numbers.

The reconciled baseline is `4d4c8ae242fe08511db8de41fcafe117dde06dba`
(`VERSION` 0.321.0). Its controlled machine, toolchain, build-edge, binary,
fan-out, trace, diagnostic, census, six-configuration corpus, and raw-artifact
record is
[`compiler-structural-baseline.json`](compiler-structural-baseline.json).
That record supersedes the approximate audit sizes below for before/after
comparison; the table remains as the rationale captured when the plan was
accepted.

| Surface | Size at audit | Mixed responsibilities that justify action |
| --- | ---: | --- |
| `src/compiler/semantic_analyzer.cpp` | 34,016 lines | reusable-session reset, declaration registration and lookup, AST body visitation, overload/generic resolution, constexpr execution, ownership and loan flow, program initialization, tooling occurrence publication, diagnostics, and concrete-instance reanalysis share one `SemanticVisitor::Impl` |
| `tests/compiler_tests.cpp` | 32,589 lines | about 120 independently named frontend, semantic, ownership, lifecycle, generic, constexpr, formatting, query, and end-to-end tests compile into one target and share one anonymous-namespace harness |
| `src/compiler/cpp_mir_body_emitter.cpp` | 30,423 lines | MIR shape recognition, backend preflight, representation checks, body facts, place/value spelling, instruction/terminator text, failure/lifecycle schedules, initializer text, and corpus support share one translation unit |
| `src/compiler/mir.cpp` | 17,948 lines | model queries, index repair, canonical rewrites, body verification, whole-program contracts, defined-failure effect derivation, optimization-coherence replay, and constant folding share one translation unit |
| `tests/cpp_mir_body_emitter_tests.cpp` | 9,807 lines | preflight, representation, text, lifecycle/failure, mutation, and aggregate corpus tests share one source even though CTest shards only runtime execution |
| `src/compiler/cpp_emitter.cpp` | 8,554 lines | program orchestration, declaration/type/lifecycle emission, native/thread adapters, generated support preamble, failure helpers, and entry/module adapters share one mutable writer |
| `src/compiler/mir_lowering.cpp` | 8,306 lines | body-local CFG/lifetime lowering, whole-program instance assembly, effect fixed-point orchestration, and compiler-generated hosted-startup construction share one implementation unit |
| `tests/optimizer_tests.cpp` | 7,389 lines | pass-manager, transformation, replay, and optimization-family tests still have a broad rebuild and failure surface despite newer focused analysis suites |
| `src/compiler/lowered_program.cpp` | 2,854 lines | conversion helpers, immutable model queries, construction, and public lowered-program verification share one implementation despite having different mutation contracts |
| `include/gti/semantic_analyzer.h` | 2,337 lines | core semantic types, published records, tooling database, semantic model, printer, analyzer facade, and instance-analysis contract remain one include boundary |
| `include/gti/mir.h` | 1,647 lines | the plain MIR data model and unrelated verifier/rewrite/effect APIs have the same include fan-out |

The measured compile evidence must be interpreted correctly:

- on an uncontended local Release build, `semantic_analyzer.cpp` took about
  6.6 seconds, `compiler_tests.cpp` about 5.1 seconds, and `mir.cpp` about
  3.9 seconds;
- concurrent clean worktrees without a shared compiler cache inflated the same
  edges to roughly 47-72 seconds and 37-130 seconds; and
- the 100 percent CPU/thermal event was primarily uncoordinated Ninja, CTest,
  and nested Clang concurrency, not proof that source decomposition alone will
  fix resource scheduling.

The structural work should improve cold parallelism, focused rebuilds, and
reviewability. Cross-agent build caps, CTest resource accounting, and compiler
caching remain separate build-system responsibilities.

The completed [compiled compiler library migration](archive/compiler-library-migration.md)
is a foundation, not a failed attempt to repeat. It already moved algorithms
out of public headers and established the `gti_compiler`, `gti_cpp_backend`,
and `gti_driver` boundaries. This plan decomposes responsibilities *within*
those compiled subsystems.

## Architectural Invariants

Every slice must preserve all of the following:

1. **Directional phase authority.** Source loading and syntax feed semantics;
   semantics feed HIR; HIR feeds MIR; verified MIR and declaration/
   representation records feed `LoweredProgram`; backends consume only that
   lowered program. No reverse phase dependency is introduced.
2. **One semantic authority.** Extracted services read or mutate one catalog,
   one body-analysis session, and one published `SemanticModel`. They do not
   copy language rules into convenience helpers.
3. **Snapshot ownership.** `FrontendResult` continues to own `Program` for at
   least as long as AST-backed semantic facts. No extracted service retains AST
   pointers, IDs, substitutions, or diagnostics in process-global state.
4. **One MIR truth.** Verification, effect derivation, optimization replay, and
   backend admission consume the same immutable MIR model and exact IDs. A
   split must not create a permissive secondary verifier.
5. **Backend admission before spelling.** C++ text writers consume a complete,
   immutable emission plan or body-facts object produced by preflight. Writers
   may not independently rediscover whether a MIR shape is supported.
6. **No semantic migration.** Accepted GTI behavior, diagnostic codes and
   spans, ownership/lifecycle rules, evaluation order, and runtime ABI remain
   fixed during mechanical extraction. A correctness fix is isolated and
   reviewed as such.
7. **No internal compatibility burden.** Retired private classes, helper APIs,
   headers, and implementation routes are removed as soon as their replacement
   passes its gate. GTI does not preserve compiler-internal source or ABI
   compatibility. This does not permit changing accepted language semantics or
   public runtime/native contracts.
8. **Request-owned state only.** Extraction may introduce immutable views and
   short-lived service objects, but not mutable singletons or hidden caches
   whose lifetime exceeds a frontend/backend request.

The intended dependency direction after the campaign is:

```text
source/syntax
  -> semantic catalog + semantic model
  -> semantic body/instance analysis
  -> HIR
  -> MIR model + lowering + verification
  -> MIR optimization + proof replay
  -> LoweredProgram
  -> backend representation plan + backend text writer
  -> driver/CLI

LSP/formatter -> frontend snapshot and compiler-owned read-only queries
```

## R-STRUCT-00: Reconcile And Freeze Evidence

**Purpose:** establish one trusted baseline without interrupting already active
work.

1. Reconcile the in-flight branches into `main` and record the exact commit.
2. Run one integration-owned broad suite. Other workers use focused tests only;
   local builds remain capped at `--parallel 2` and CTest at `--parallel 1`
   while the shared machine policy requires those limits.
3. Record clean Debug and Release build timings, no-op timing, object-edge
   timings from Ninja, binary sizes, a representative frontend trace, and
   generated C++ hashes for the existing corpus.
4. Record the source/header inventory and direct include fan-out at that exact
   commit.
5. Freeze diagnostic snapshots, MIR/lowered-program printers, the MIR census,
   optimizer report/replay output, and C++20/C++23 O0/O1/O3 corpus behavior as
   equivalence oracles.

**Exit evidence:** complete. The committed
[`compiler-structural-baseline.json`](compiler-structural-baseline.json) names
the machine/toolchain, configuration, concurrency, commit, commands, and raw
artifact locations. No refactor result is compared across unlike concurrency
or competing worktrees.

## R-STRUCT-01: Test Ownership And Analysis Sessions

This slice creates safe migration homes before moving substantial algorithms.

### Semantic session correctness

`SemanticVisitor::Impl::check(const Program&)` and `check(const Expr&)`
currently maintain separate manual reset lists. At the audit commit the
expression path omitted state reset by the program path, including
`internalNamespaceSymbols`, `recursiveLayoutFields`, and `currentReturnType`.
The public facade permits visitor reuse, so this is a correctness risk rather
than a style concern.

- Introduce one explicit request/session state owner and one reset operation.
- Make both entry points establish a fresh session through that operation.
- Add program-to-program and program-to-expression reuse tests proving that
  names, IDs, AST pointers, flow state, diagnostics, and return context do not
  survive.
- Keep target, source-graph configuration, tooling mode, and metrics sink as
  constructor configuration rather than resettable semantic results.

### Compiler test partition

Move `tests/compiler_tests.cpp` into a small shared fixture plus bounded suites:

- frontend/source/parser and recovery;
- declarations, types, overloads, concepts, and generics;
- ownership, borrows, lifecycle, ranges, and callables;
- constexpr, configuration, numerics, and static initialization;
- diagnostics, formatting, and language queries; and
- an intentionally small end-to-end frontend/HIR/MIR/backend suite.

Use either several focused executables or several translation units with
independently selectable suite names, based on measured build/link cost. Do not
create one executable per test. Preserve every assertion and retain one broad
aggregate CTest entry for parity.

Apply the same ownership rule to `cpp_mir_body_emitter_tests.cpp`: separate
preflight/representation, ordinary text, lifecycle/failure, and corpus protocol
tests while preserving the existing exact-census aggregate sweeps.

**Exit evidence:** complete. Program-to-program and program-to-expression reuse
regressions prove that names, IDs, AST pointers, loans, diagnostics, and
return/expression context are cleared by one request reset. The
`compiler_pipeline` aggregate first validates a 125-test manifest and then runs
six independently buildable suites exactly once. The emitter census validates
81 registrations across four executables; its 79 non-corpus tests and both
per-source corpus sweeps pass. The largest compiler-test source is 15,784 lines
(down from 34,112), and the largest emitter-test source is 4,870 lines (down
from 10,161). Existing diagnostic assertions and the corpus production-text
agreement gate pass without a reviewed output change.

## R-STRUCT-02: Frontend And Semantic Boundaries

The semantic refactor is serialized because its current state is shared.
Algorithms move only after their state boundary exists.

### 0. Source graph inputs

Separate lexical-unit acquisition from source-graph recursion before semantic
catalog work depends on either concern:

- a `LoadedLexicalUnit` service owns file/override/cache acquisition, content
  digesting, lexing, and the exact token/lexical-diagnostic result; and
- a configuration-directive processor owns the bounded `#if`/`#elif`/`#else`
  selection over that lexical unit.

`SourceLoader` retains canonical path and package policy, include resolution,
cycle/load-once behavior, recursive graph construction, source provenance, and
diagnostic inclusion chains. This extraction must not change directive
semantics or opportunistically move the current source-loader diagnostic
families between phases.

### 1. Catalog ownership

Extract a private `SemanticCatalog` that owns declaration identities and
indexes: namespaces and visibility, aliases, concepts, classes, enums,
overload sets, inheritance/member tables, exact specializations, native
declarations, and generic-parameter declarations. A registration pass mutates
the catalog in the existing prepass order. Body and instance analysis receive
read-only catalog access.

The catalog is not the published semantic snapshot. `SemanticModel` continues
to own expression, binding, call, loan, lifecycle, occurrence, and tooling facts
consumed by HIR and queries.

### 2. Callable declaration authority

Replace the manually synchronized public `FunctionInfo` and private
`FunctionCandidate` declaration metadata with one immutable callable
declaration record owned by the catalog. Overload resolution may build a small
candidate view containing only substitution, receiver/access, dispatch, and
viability state. Model publication uses one explicit projection from the
declaration record.

This slice must not alter exact-match overload resolution, constraints,
default arguments, ownership boundaries, or diagnostics.

### 3. Body-scoped services

Move mutable state into explicit body-scoped aggregates before moving code:

- `ConstexprInterpreter` owns its local environment, recursion/step budget,
  substitutions, and structured failure result. It uses the existing pure
  arithmetic operations in `constant_evaluator.cpp` and does not broaden
  constexpr semantics.
- `LoanFlowState` owns active loans, carriers, conditional/loop flow, and
  endpoint publication.
- `ValueFlowState` owns initialization/move state and control-flow joins.

Loan and value-flow operations remain interleaved with semantic AST visitation;
they are not moved into MIR or converted into an unsound post-pass.

### 4. Concrete-instance handoff

Introduce a narrow `SemanticInstanceProvider`. `HirLowerer` receives
`const SemanticModel&` plus that provider, never a mutable whole
`SemanticVisitor`. The first implementation may adapt the existing delta/
rebase mechanism. The provider ultimately owns the catalog, base snapshot, and
per-request instance session needed for function, constructor, destructor, and
field-initializer reanalysis.

### 5. Header boundary after ownership

Only after the private ownership seams are working, measure and introduce at
most these public boundaries:

- semantic core types and IDs;
- semantic records/model/query storage; and
- analyzer plus instance-provider facade.

Callers include the narrowest contract. Do not split the semantic header into
arbitrary feature headers or make `FrontendResult` release the AST lifetime
that semantic facts require.

**Exit evidence:** source visibility, classes/inheritance, exact
specializations, concepts, callable ownership, constexpr/static assertions,
borrow/lifecycle, generic/variadic instances, and HIR instance discovery pass
their focused suites and the aggregate oracle. A semantic implementation edit
does not recompile LSP, driver, backend, or unrelated tests solely through a
public umbrella include.

## R-STRUCT-03: MIR Construction, Verification, And Replay

Preserve `MirProgram` as one model while separating operations that have
different mutation and proof contracts.

### MIR model and queries

Keep plain MIR records, IDs, body lookup, use indexes, and deterministic
enumeration together. Move verifier, rewrite, effect, and replay declarations
out of the model include where that measurably reduces fan-out. Do not make
plain records opaque merely to reduce header size.

### Canonical editor and proof replay

Put reachability/use-index repair and canonical instruction/block rewrites in
one editor component. Keep each rewrite's validation and optimization-record
replay with the canonical operation that defines it. Optimization passes
request edits through this component; they do not mutate vectors directly and
then teach a separate verifier to accept the result.

### Verifier ownership

Separate these responsibilities behind one public `verifyMirProgram` facade:

- body-local CFG, value, place, ownership, loan, drop, and failure checks;
- cross-body identities, generated adapters, class/lifecycle contracts, effect
  summaries, and program initialization/hosted-startup checks; and
- optimization coherence, which verifies source and optimized programs and
  replays the exact proof records.

The facade aggregates structured `MirVerificationResult` values. It must fail
closed if a component is missing; no verifier component may silently downgrade
an unsupported contract.

Defined-failure effect derivation belongs beside MIR program analysis, not in
the data-model implementation or the optimizer pass manager.

### Lowered-program closure

Keep `LoweredProgram` pointer-free and immutable while separating its current
implementation responsibilities:

- model lookup and deterministic enumeration;
- construction from the verified frontend/MIR inventories; and
- public lowered-program verification.

`LoweredProgramBuilder` remains the only conversion boundary. Its verifier
must consume only the completed lowered representation and may not reach back
into AST, semantic, or HIR state. Backends continue to receive the same
self-contained contract.

### Lowering ownership

Split `mir_lowering.cpp` along existing lifetimes:

- a body lowerer owns body-local IDs, scopes, loans, drops, CFG, and ordered
  expression/statement lowering;
- a program lowerer owns instance inventory and the bounded failure-effect
  fixed point; and
- a hosted-startup builder owns the compiler-generated entry adapter schedule.

All three produce the same MIR records and use the same verifier. Hosted startup
does not become backend-generated policy.

**Exit evidence:** MIR printer output, body/program verification diagnostics,
failure effects, optimization records and reports, replay rejection tests,
MIR and lowered-program census, hosted startup, lowered-program rejection, and
O0/O1/O3 behavior are unchanged. Controlled mutation tests prove that omitting
any verifier component fails closed.

## R-STRUCT-04: C++ Backend Admission And Text Ownership

The C++ backend keeps the existing public `CppBackend`/`LoweredProgram`
boundary and decomposes its private implementation.

### One body emission plan

Convert the current body analysis into an immutable
`CppMirBodyEmissionPlan` (name provisional) that contains every approved MIR
shape, representation row, place/value strategy, lifecycle/failure schedule,
and required generated helper. Shape-recognition helpers remain with the plan
builder until their ownership is stable.

The body text writer consumes this plan. It may format approved operations but
must not rerun capability predicates or admit a shape independently. Backend
preflight remains fail-closed and covers every body before any artifact is
published.

### Text writers by output responsibility

Extract private writers for:

- ordinary body instructions, places, values, edge arguments, and
  terminators;
- initializer/module schedules;
- lifecycle and defined-failure support text;
- declaration/type/member/lifecycle definitions;
- native callback and managed-thread adapters; and
- hosted entry/module adapters.

Each writer receives an immutable plan plus a small shared output/indentation
sink. Writers do not own semantic lookup or mutable whole-program analysis.

### Generated support preamble

Move the large backend support preamble and helper fragments out of
`CppEmitter::Impl` into a dedicated compiled component with explicit feature
selection (`expected`, storage, threading, failure helpers, and so on). Keep
the emitted bytes stable. Do not turn the preamble into public runtime API or
move source-defined standard-library policy into the compiler.

`CppEmitter::Impl` ends as the program-level coordinator: verify lowered input,
build the representation snapshot and program/body plans, invoke writers in a
deterministic order, and return the artifact.

**Exit evidence:** representation snapshots and program plans remain
deterministic; all preflight mutation tests still reject; C++20/C++23 and
O0/O1/O3 corpus output is byte-identical to the baseline; native bindings,
failure, lifecycle, managed-thread, module initialization, and installed
toolchain tests pass.

## R-STRUCT-05: Dependency And Build Closure

After source ownership is explicit, make the build describe it.

1. Move compiler sources into responsibility directories only when the move
   accompanies an extracted API. Do not perform a directory-only churn pass.
2. Keep `gti_compiler` and `gti_cpp_backend` as the installed public targets.
   Introduce private object/static targets only when they enforce the phase DAG
   without export/link cycles; otherwise keep one target and enforce include
   direction with a repository check.
3. Move test target registration to `tests/CMakeLists.txt` or focused helper
   files so the root CMake file owns products and options rather than every
   test executable.
4. Narrow includes after the dependency graph is real. Do not add forwarding
   umbrella headers that restore the same fan-out for all internal callers.
5. Measure, rather than assume, whether target subdivision, precompiled
   headers, unity builds, or IPO help. Unity builds are not an acceptable way
   to hide responsibility boundaries and are excluded from the refactor gate.

### Final measurements

Compare against **R-STRUCT-00** on the same machine, compiler, configuration,
cache state, and concurrency:

- clean Debug and Release wall/CPU time;
- no-op and implementation-only rebuilds for semantics, MIR, optimizer, body
  emission, and one test suite;
- longest and 95th-percentile object compile edge;
- compiler, backend, CLI, and LSP binary/archive sizes;
- representative GTI frontend and end-to-end compilation latency; and
- generated artifact hashes and runtime outputs.

The gate requires:

- no unexplained clean-build, compiler-latency, or binary-size regression over
  10 percent;
- at least a 40 percent reduction in the longest uncontended compiler/test
  object edge, or a documented measurement showing that the edge ceased to be
  on the critical path and a different bounded target was achieved;
- implementation-only edits rebuild only the owning objects and required link
  products, not unrelated phase consumers; and
- the broad verification matrix, installed-library/toolchain checks,
  Tree-sitter, formatter, LSP protocol, documentation index, MIR census,
  optimizer replay, and corpus/native runtime gates pass.

Feature development resumes only after the final report records these results,
all **R-STRUCT** rows are marked complete in
[`implementation-sequence.md`](implementation-sequence.md), and the canonical
architecture documents describe the new source ownership. A partially split
file or improved line count is not sufficient.

## Areas That Deliberately Remain Cohesive

- Parser grammar, cursor/contextual token handling, and error recovery remain
  together unless a measured dependency shows a real independent owner.
- The closed AST node hierarchy remains together; declaration/expression/
  statement file splitting alone has no authority benefit.
- `FrontendResult` retains the AST and semantic snapshot lifetime relationship.
- `SemanticDiagnosticReporter` remains the shared diagnostic-construction seam.
- `constant_evaluator.cpp` remains the pure constant arithmetic layer beneath
  the bounded constexpr interpreter.
- The MIR plain-data model and exact ID vocabulary remain one representation.
- A canonical MIR rewrite stays with the proof/replay contract that authorizes
  it.
- `LoweredProgram` remains the single immutable backend input.
- Driver, CLI, LSP transport, formatter, Tree-sitter, runtime, and standard
  library are not split merely because compiler work is paused. They change
  only when evidence identifies a responsibility violation or the active
  extraction requires an interface update.

## Explicit Non-Goals

- changing GTI syntax, type rules, ownership, lifetime, failure, concurrency,
  constexpr, generic, or standard-library semantics;
- redesigning HIR or MIR, introducing SSA/MemorySSA, or starting an LLVM
  backend;
- preserving retired private headers, helper names, generated C++ formatting,
  or compiler-internal ABI;
- splitting every large file, enforcing an arbitrary line limit, or creating
  one class/file/test per feature;
- introducing mutable global registries, service locators, plugins, or a
  generic pass framework without a current client;
- using C++ modules, unity builds, precompiled headers, LTO, or a compiler cache
  as a substitute for ownership boundaries; or
- mixing user-visible features or opportunistic cleanup into migration
  commits.

## Change Discipline

Each implementation commit owns one migration seam, moves state before
algorithms, and leaves the tree buildable. The commit description names:

- old and new authority;
- dependency direction;
- moved data and its lifetime;
- exact focused parity tests;
- generated/MIR/diagnostic equivalence evidence; and
- build measurements affected by the slice.

If a proposed extraction needs broad semantic changes to become possible, stop
and document that dependency. Do not hide a redesign inside a mechanical
refactor.
