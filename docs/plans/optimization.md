# GTI Optimization Architecture Proposal

> **Plan status:** Non-canonical staged design. The implemented optimizer
> boundary is documented in
> [`docs/architecture/optimization.md`](../architecture/optimization.md).

Status: accepted; bounded Milestone 1 foundation complete, first Milestone 2
shadow proof sufficient, Milestone 3 `scalar-leaf-v1`, `scalar-cfg-v1`, and
`scalar-direct-call-v1` plus `class-default-cleanup-v1` and
`owned-lifecycle-call-v1` cutovers complete, failure-capable closure active

Operational ordering is maintained in
[`implementation-sequence.md`](implementation-sequence.md). In particular, the
first transforming slice must bring only the MIR editor and invalidation it
uses; a general pass-manager framework is not scheduled before a real pass.
The immediate operational priority is now production MIR consumption, not
completing every compatibility fold in shadow mode.

This proposal defines how GTI can grow from one typed-HIR constant-folding pass
into a maintainable optimizer without making AST shape, the C++ emitter, or a
future backend part of optimization semantics. It is an architecture and
migration plan only. It does not implement new optimizations or change shipped
compiler behavior.

The implemented pipeline and current IR structures are documented in
[`docs/architecture/optimization.md`](../architecture/optimization.md). Benchmarking,
timings, optimization remarks, IR dumps, and safety-operation reports are
specified separately in
[`docs/plans/performance-tooling.md`](performance-tooling.md).

## Decision Summary

1. Keep checked AST and `SemanticModel` immutable after frontend analysis.
2. Keep typed HIR as the immutable concrete-instance and resolved-call graph.
3. Make MIR the authoritative executable representation for transformations
   that change control flow, values, places, calls, checks, or cleanup.
4. Replace emitter-specific optimization side tables with an owned optimized
   MIR result through a staged compatibility migration.
5. Centralize operation and instruction effects. A new MIR kind is incomplete
   until its purity, memory, trap, ownership, loan, drop, synchronization, and
   call behavior is classified.
6. Give passes controlled rewrite APIs, cached analyses with explicit
   invalidation, deterministic ordering, and a verifier that runs after every
   transformation in validation builds.
7. Preserve source provenance as transformations rewrite MIR; provenance is
   not an excuse to keep source expressions as optimization identity.
8. Require language-level proofs for removing checks, calls, drops, or other
   observable behavior. Native C++ optimizer behavior is never such a proof.
9. Add only the pass infrastructure and observability required by a concrete
   transform or production-emission client; backend cutover precedes broader
   shadow-pass coverage.
10. Treat `-O0` through `-O3` as different optimization effort, never different
    GTI semantics.

## Goals

- Make adding a local MIR optimization possible without changing the parser,
  semantic analyzer, HIR lowerer, or C++ expression visitor.
- Prevent new syntax, types, operations, ownership rules, and intrinsics from
  silently bypassing optimizer correctness requirements.
- Give every transformation a single authoritative IR, a declared proof basis,
  and structural verification.
- Preserve exact GTI behavior for traps, evaluation order, overflow, moves,
  borrows, drops, virtual dispatch, and target-dependent rules.
- Let the current C++ backend and a future LLVM backend consume the same
  optimized program.
- Keep optimization deterministic, inspectable, testable, and measurable.
- Permit incremental migration from the current implementation without a
  compiler-wide emitter rewrite in one change.

## Non-Goals

- Implementing the proposed passes or command-line tooling in this change.
- Selecting a final LLVM IR, object layout, virtual-table ABI, or calling
  convention.
- Promising particular performance from `-O1`, `-O2`, or `-O3`.
- Defining unresolved language behavior such as signed overflow by borrowing
  host C++ behavior.
- Moving target instruction selection, register allocation, vectorization, or
  machine scheduling into the GTI middle end.
- Treating emitted C++ text or native optimization reports as GTI IR.
- Stabilizing MIR dumps or pass names as a public compatibility ABI.

## Current State And Architectural Risk

The frontend already constructs both concrete typed HIR and validated
structural MIR. MIR has basic blocks, explicit terminators, body-local value
definitions, indexed uses, typed places and projections, resolved calls,
static/virtual dispatch, moves, loans, drops, and structured construction.
These are the right foundations for a middle end.

`OptimizationPipeline` still runs its sole transforming pass over `HirProgram`.
That compatibility pass records constant replacements by `HirValueId` in
`OptimizationResult`; `CppEmitter` projects a source expression back to every
corresponding HIR value and applies a replacement only when all concrete
instances agree.

Milestone 1 now includes a second owned-MIR entry point and its first bounded
editor client. An
`OptimizationRequest` owns a MIR copy and returns an `OptimizedProgram` after
reusable structural verification. At `-O1+`, primitive scalar literals flowing
only through grouping identities are rewritten in MIR and compared by HIR
provenance with the compatibility constant result. The CLI passes that
snapshot through `BackendInput::mir`; `CppBackend` now re-verifies it and uses
it as the sole body authority for the `scalar-leaf-v1`, `scalar-cfg-v1`, and
`scalar-direct-call-v1` families plus `class-default-cleanup-v1` and
`owned-lifecycle-call-v1`. The current literal-identity transform changes
emitted values only for the first three.
MIR lowering and optimization share
reachability repair, value-use
indexing, and verification implemented in `src/compiler/mir.cpp`. `MirPrinter`
provides a complete deterministic snapshot, and exhaustive instruction,
operation, and intrinsic effect tables use enum count sentinels plus compile-
time size checks.
Verification also tracks active loans through reachable CFG paths, requiring
one producer, valid active uses and ends, balanced normal exits, and matching
loan state at joins.

MIR v20 added a separate bounded function-effect foundation rather than asking
the optimizer or backend to infer failure from HIR. MIR v21 makes the canonical
effect result cover functions, constructors, and destructors. The function
component retains the closed acyclic scalar/static-call proof and adds the exact
class-default-cleanup shape. A separate closed proof covers passive-scalar-class
constructors with one exact initializer stage per field, their source
destructors, and free-function graphs. The production owned-lifecycle selector
adds exact source/MIR graph and lifecycle coherence before using that effect
fact. Generic verification permits
conservative true summaries but rejects an unproved false claim and checks each
static call's exact `None` versus `DirectCall` propagation. Broader effect
dimensions and recursive fixed-point proofs remain client-gated.

The first editor accumulates guarded body/`{block,index}` literal replacements,
repairs dirty value uses, verifies a copied program, and commits atomically.
It records instruction/use invalidation while preserving CFG, reachability, and
freshly recomputed dominance. A general pass manager, cached analyses, broader
edit operations, and CLI dump options remain unimplemented. The bounded
literal-identity rewrite now changes generated artifacts only for the selected
MIR-backed `scalar-leaf-v1`, `scalar-cfg-v1`, and `scalar-direct-call-v1`
families.

That bridge is safe for the current narrow folding pass, but it creates four
long-term risks:

- source expression shape remains the place where an optimization becomes
  observable;
- multiple concrete instances are collapsed back onto one AST expression;
- CFG, place, use-def, loan, drop, and dispatch facts in MIR cannot participate;
- a second backend would have to reproduce C++ emitter interpretation of the
  side table.

The bridge must remain compatibility-only. New CFG or dataflow passes must not
extend it into a second, source-indexed middle end.

## Authority And IR Contract

### Checked AST and semantics

The checked AST owns syntax and source structure. `SemanticModel` owns resolved
language facts: exact types, access, ownership traits, selected overloads and
constructors, dispatch, lifecycle, source-unit visibility, and intrinsic
identity. Optimization may query these representations but must not mutate
them or repeat their resolution.

An optimization is invalid if it requires the backend to re-run name lookup,
overload selection, implicit conversion, ownership inference, or lifecycle
selection after the transformation.

### Typed HIR

HIR remains the immutable whole-program graph of concrete class, callable,
constructor, destructor, and lambda instances. It is authoritative for stable
instance identity, concrete generic substitution, resolved call edges, virtual
roots, class/base relationships, and source-to-instance provenance.

HIR analyses may guide interprocedural decisions. Transformations that change
executable behavior must materialize their result in MIR. Do not create a
second mutable HIR and a second family of CFG or place analyses.

The existing HIR constant-replacement table remains temporarily supported only
for the current C++ emitter migration. It is not the API for new passes.

### MIR

MIR becomes the optimization authority for executable bodies. It owns:

- basic blocks, edges, and terminators;
- typed value definitions and uses;
- typed places, projections, reads, writes, and moves;
- calls, construction, intrinsics, and dispatch mode;
- loans, borrow ends, drops, and lexical cleanup;
- operations with language-defined behavior and possible traps;
- source and HIR provenance retained for diagnostics and reports.

An optimized MIR program must be valid input to every backend. No backend may
select a different GTI transformation because of emitted language, host
compiler, or platform spelling.

MIR is not yet ready for every optimization. General temporary lifetime,
precise aliasing, call effects, concrete object layout, ABI, and some primitive
edge semantics remain incomplete. The capability gates below make those gaps
explicit instead of allowing passes to guess.

## Proposed Pipeline

The intended steady-state pipeline is:

```text
Frontend
  -> immutable checked AST + SemanticModel
  -> immutable concrete typed HIR
  -> validated unoptimized MIR
        |
        v
OptimizationPipeline
  -> MIR canonicalization
  -> analyses and MIR transformations
  -> verification and deterministic report
        |
        v
OptimizedProgram
  -> optimized MIR + provenance + optimization report
        |
        +-> C++ backend
        +-> future LLVM backend
```

The driver supplies the same `TargetInfo` to semantics, MIR lowering,
optimization, and backend generation. When optimization eventually requires
layout, introduce one backend-neutral `TargetMachineInfo` or data-layout
contract shared by relevant phases. Do not query a C++ compiler from a pass.

The LSP continues to stop at `FrontendResult`; it does not need optimized MIR
for semantic language queries. Developer IR views may explicitly request the
optimization pipeline.

## Optimizer Ownership Model

The current `MirProgram` exposes only const accessors and grants mutation to
`MirLowerer`. Preserve that encapsulation. Do not make every program vector
public to enable passes.

Introduce one optimizer-owned mutation boundary. Exact names may change, but
the conceptual API is:

```cpp
struct OptimizationRequest {
  const HirProgram &hir;
  MirProgram mir;
  OptimizationLevel level;
  TargetInfo target;
  OptimizationOptions options;
};

struct OptimizedProgram {
  MirProgram mir;
  OptimizationReport report;
};

class MirProgramEditor;
class MirBodyEditor;
```

The optimizer takes MIR by value or unique ownership and returns a new owned
result. The unoptimized `FrontendResult::mir` stays available for debugging and
differential tests. The CLI may move a MIR copy when that ownership becomes
practical; shared mutable MIR is forbidden.

Editors provide operations such as replacing an operand, replacing a
terminator, splitting or removing a block, replacing a value with a constant,
and erasing a proven removable instruction. Editors are responsible for
revision tracking and delegate ID repair, reachability, use indexing, and
validation to shared MIR utilities.

Passes must not update `valueUses`, definition locations, block reachability,
or IDs ad hoc. A central repair/canonicalization step either incrementally
maintains those facts or rebuilds them after a pass. Start with rebuilding for
simplicity; optimize the compiler only after profiles justify incremental
maintenance.

## Pass Manager And Analysis Contract

Keep `include/gti/optimizer.h` as the small public facade. Move growing
infrastructure behind focused headers rather than allowing another monolithic
header:

```text
include/gti/optimization/
  analysis.h
  effects.h
  pass.h
  pass_manager.h
  report.h
  rewrite.h
  verifier.h
  analyses/
  passes/
```

Declarations remain under `include/gti/`, but optimization implementations
should move into the compiled `gti_compiler` target as described by
[the compiler library migration plan](compiler-library-migration.md).
The focused layout is an ownership boundary and must not become another
monolithic implementation header.

A pass declares its stable developer name, program or body scope, minimum
optimization level, required analyses, and outcome. An illustrative contract
is:

```cpp
struct PassOutcome {
  bool changed = false;
  PreservedAnalyses preserved;
};

class MirPass {
public:
  virtual std::string_view name() const = 0;
  virtual PassOutcome run(MirPassContext &, MirProgramEditor &) = 0;
};
```

`AnalysisManager` owns cached results keyed by program/body identity and IR
revision. A transformation explicitly preserves or invalidates analyses.
Unknown preservation invalidates the cache conservatively. Analyses never
silently mutate MIR.

Initial reusable analyses should be introduced only as demanded by a pass:

- predecessor/successor and reachability information;
- dominator trees;
- value definition/use queries;
- local liveness;
- call graph and conservative call effects;
- place alias/escape facts;
- integer ranges and known predicates.

Do not add an analysis because mature compilers commonly have one. Add it with
the first transformation and tests that demonstrate its contract.

Pass order is explicit per optimization level. Repeated groups use a fixed,
documented iteration bound or a monotonic worklist; never use an unbounded
`while (changed)` loop. Pass traversal follows stored instance, body, block,
instruction, and operand order so identical inputs produce identical output.

## Effect And Safety Model

Dead-code removal, reordering, propagation, and commoning all require more than
a `pure` boolean. Define centralized traits for every `MirInstructionKind`,
`MirOperation`, intrinsic, and eventually resolved call. The model must answer
at least whether an instruction:

- reads or writes a place or unknown memory;
- allocates or invokes runtime behavior;
- may trap or performs a checked operation;
- starts, ends, or depends on a loan;
- copies, moves, initializes, or drops a value;
- invokes static or virtual user code;
- depends on target semantics;
- may synchronize with another execution context;
- is speculatable, removable when unused, or safely reorderable.

These are separate traits. A computation that has no memory effect may still
trap and therefore may not be deleted or moved. A read may be repeatable only
when alias and call effects prove no intervening write. A destructor call is
observable even when its result is unused.

Use a conservative default for ordinary and virtual calls until a checked
effect-summary system exists. Intrinsics receive explicit summaries by semantic
intrinsic identity, never by function spelling. Unknown effects block the
optimization; they do not become pure. Runtime and user-code calls remain
possible synchronization barriers until a checked summary proves otherwise.

The following rules are mandatory:

- preserve left-to-right GTI evaluation and short-circuit behavior;
- preserve all language-defined traps unless a proof makes them unreachable;
- preserve move consumption, active-drop state, destruction order, and exactly
  one drop of each active value;
- preserve loan creation, alias restrictions, and borrow end points;
- preserve exact static/virtual dispatch unless closed-world type facts prove
  a replacement;
- preserve NaN, signed-zero, rounding, and comparison behavior once GTI's
  floating-point contract is explicit;
- preserve target-conditional selection and use one target throughout;
- never derive signed overflow, shift, division, remainder, conversion, or
  indexing semantics from the optimizer host language.

## MIR Evolution Guardrail

Adding a `MirInstructionKind`, `MirOperation`, operand kind, terminator, place
projection, intrinsic, or lifecycle effect requires all of the following in
the same change:

1. structural validation;
2. deterministic printing;
3. value/use and CFG indexing coverage;
4. effect and trap classification;
5. optimizer audit for every exhaustive operation family;
6. backend consumption or a deliberate unsupported diagnostic;
7. focused MIR and optimization-level equivalence tests;
8. an update to this proposal's capability matrix when it changes a gate.

Prefer exhaustive switches without a permissive `default` in classification
and transformation code so enum growth causes a compiler error or test failure.
Add a coverage test that iterates every enum value through the verifier,
printer, and effect table where C++ enum structure permits it.

This contract is the primary defense against architectural drift: a language
feature cannot be considered complete while its middle-end behavior is
unclassified.

## Provenance, Identity, And Determinism

Optimization identity is MIR program/body/block/instruction/value identity,
not an AST address. Preserve `HirValueId`, `HirStatementId`, source unit, and
source spans as provenance on rewritten operations and blocks.

When multiple operations combine, retain a primary origin and an ordered set
of contributing origins for reports. When an operation is synthesized, retain
the controlling branch, call, or declaration origin. Diagnostics remain a
frontend concern; optimization provenance supports IR inspection, remarks,
debug information, and failure triage.

Rewriting may compact body-local IDs at a canonicalization boundary. If IDs are
compacted, all definitions, uses, places, loans, edges, and reports must be
remapped together. IDs need stability only within one IR snapshot. Reports and
golden dumps must not depend on pointer values or unordered-container order.

## Optimization Level Policy

All levels preserve identical GTI-observable behavior. Initially, a higher
level may run the same pass set as a lower one.

| Level | GTI middle-end policy | Native backend policy |
| --- | --- | --- |
| `-O0` | no transforming passes; optional validation and dumps only | request native `-O0` |
| `-O1` | bounded local canonicalization, constant folding, branch simplification, unreachable cleanup, and removal of unused non-trapping computations | request native `-O1` |
| `-O2` | function-local dataflow, sparse propagation, range proofs, check elimination, and alias-safe load/store work after their gates exist | request native `-O2` |
| `-O3` | evidence-backed interprocedural work such as devirtualization or inlining only after call effects, cost policy, lifetime, and layout contracts exist | request native `-O3` |

Size-oriented optimization is a future independent policy; do not silently
make `-O2` or `-O3` mean size optimization. Debuggability options and remark
verbosity are also independent of semantic optimization level.

## Capability-Gated Pass Roadmap

| Pass family | Authoritative IR | Required capabilities before implementation |
| --- | --- | --- |
| typed constant folding | MIR operations | exact constant representation; operation edge semantics; trap classification; constant replacement rewrite |
| CFG simplification | MIR blocks and terminators | predecessor/successor repair; reachability; cleanup-edge preservation; block verifier |
| dead-value elimination | MIR instructions and indexed uses | complete effect/trap table; drop/loan awareness; definition/use repair |
| sparse constant propagation | MIR CFG and values | dominance or executable-edge analysis; mutation barriers; typed lattice; CFG rewrite |
| copy propagation | MIR values and places | dominance; exact type/access preservation; move and loan barriers |
| redundant load/store removal | MIR places | place alias and escape analysis; call write effects; projection overlap; lifetime boundaries |
| bounds/check elimination | MIR checked operations | integer semantics; range/predicate analysis; explicit check identity; proof reporting |
| devirtualization | HIR instance graph plus MIR calls | closed-world reachability; exact receiver proof; override graph; lifecycle/layout compatibility |
| inlining | HIR call graph plus MIR bodies | conservative call effects; recursion handling; cost model; provenance composition; cleanup and temporary lifetime correctness |

Do not implement a pass when one of its gates is represented only by C++
emission behavior. Add the missing backend-neutral fact first.

The likely local sequence after infrastructure is:

1. MIR constant folding;
2. conditional and switch simplification;
3. unreachable block removal;
4. dead non-trapping value removal;
5. sparse propagation and another CFG cleanup;
6. range-based check elimination;
7. place optimizations only after alias and effects are explicit.

Each cleanup repetition must be intentional and bounded in the configured
pipeline.

## C++ Backend Migration

The migration must avoid two permanent optimization authorities.

### Stage A: establish MIR infrastructure without output changes

Status: bounded foundation complete; general infrastructure grows only with
clients. Reusable verification/repair, deterministic printing, effect traits,
the owned result, and one atomic replacement editor with invalidation tests are
implemented. General pass/analysis management and dump options remain.

- Maintain deterministic MIR printing, public validation utilities, effect
  traits, and the bounded controlled editor. Add broader editor or analysis
  management only with a transform that consumes it.
- Return an `OptimizedProgram`; keep `-O0` unchanged while changed passes carry
  explicit reports and fresh verification.
- Keep the existing HIR `OptimizationResult` and C++ emitter behavior intact.
- Verify that `-O0` is structurally identical and every queued edit is atomic.

### Stage B: prove the shadow path

Status: bounded proof complete and sufficient for cutover. Primitive scalar
grouping identities are implemented and cross-checked. Other grouping values,
unary, comparison, logical, arithmetic, and conversion families remain useful
future transform work, but they are not prerequisites for a production MIR
body consumer.

- Preserve the implemented primitive identity cross-check until its body family
  consumes optimized MIR.
- Port another compatibility fold only when a measured transform client or an
  active migration phase requires it.
- Treat disagreement as a compiler test failure, not as permission to choose
  whichever result the C++ emitter prefers.

### Stage C: consume optimized MIR

Status: active immediate backend-authority recovery campaign; four production
families are complete and the broader failure-free construction/normal-cleanup
closure is active.

- Retain `scalar-leaf-v1`, `scalar-cfg-v1`, `scalar-direct-call-v1`, and
  `class-default-cleanup-v1` as completed optimized-MIR production families.
- Continue directly through the largest coherent remaining failure-free
  construction/normal-cleanup closure, then the failure-capable closures.
- During mixed emission, retain one authoritative source for each complete
  body. Never combine AST evaluation order with MIR-rewritten CFG for the same
  body.
- Retain AST and HIR only for declarations, source spelling, and representation
  facts not yet present in MIR.
- Co-deliver missing M-EXEC/M-FAIL representation and verification with the
  production family that consumes it; do not stop at an IR-only checkpoint.
- Make each migrated production selection route non-fallback. Reusable
  compatibility code may continue to serve unmigrated families and the public
  direct-emitter API until the final cutover.
- Test generated behavior at `-O0` and each enabled GTI level while requesting
  native `-O0` in structural optimizer tests so the native compiler cannot hide
  a GTI middle-end error.

### Stage D: retire the source replacement bridge

- Remove `OptimizationResult::replacement(const HirProgram&, const Expr&)` and
  emitter dependence on HIR constant side tables.
- `BackendInput` already carries the optimized MIR snapshot; make every backend
  that emits executable bodies consume that snapshot.
- Keep HIR available for instance/declaration metadata until MIR owns all
  backend-required program structure.

Do not add new passes to the legacy HIR replacement path during this migration.
Fixing a correctness bug in the existing pass remains allowed.

## Verification Strategy

### Structural tests

- Validate unoptimized MIR before the first pass and optimized MIR after every
  changed pass in compiler tests and validation builds.
- Test each editor operation for ID, edge, definition, use, place, loan, and
  reachability repair.
- Test analysis invalidation by running a transformation that changes each fact
  an analysis consumes.
- Test deterministic pass order and byte-identical IR dumps for identical
  inputs.
- Add direct fixtures for traps, moves, borrows, drops, virtual calls,
  constructors, short-circuit CFG, and target-selected branches.

### Semantic equivalence tests

- Run representative programs at `-O0` and every enabled level and compare
  stdout, stderr contract, exit status, and runtime failure category.
- Include programs whose correct behavior is a bounds, conversion, shift,
  division, owner, or storage failure; an optimization must not turn a required
  failure into success or a different failure.
- Exercise C++20 and C++23 when their representations differ.
- Keep invalid-source diagnostics identical across optimization levels because
  optimization runs only after a valid frontend.
- Test virtual dispatch, move-only cleanup, return loans, and constructor/drop
  order before enabling transformations that touch their bodies.

### Pass-focused tests

- Assert optimized MIR shape directly; generated C++ text is secondary.
- Pair every applied rewrite with a near-miss case that lacks one required
  proof.
- Require a deterministic applied or missed remark when the reporting
  milestone lands.
- Test pass idempotence where the pass claims canonical output.
- Add randomized or property-driven equivalence testing only after deterministic
  printers and failure classification exist.

Benchmarks decide priority and cost policy, not correctness. CI should not use
wall-clock pass thresholds. Follow the performance tooling proposal for
measurement and reporting.

## Anti-Drift Review Checklist

Every compiler feature or IR change must answer:

- What is its exact observable behavior, including traps and cleanup?
- Which semantic record makes that behavior authoritative?
- How is it represented in every concrete HIR instance?
- Which MIR instruction, operation, place, edge, or lifecycle record owns it?
- What are its effect and call-summary classifications?
- Can it synchronize, and if unknown, does MIR conservatively prevent movement
  or removal across it?
- Can existing passes inspect it exhaustively, or must they conservatively
  stop?
- Does the verifier reject malformed forms?
- Does the deterministic printer expose enough information to debug it?
- Does the C++ backend consume the same meaning without re-inferring it?
- Which `-O0` versus optimized equivalence test prevents regression?

Every optimization change must additionally answer:

- What fact proves the rewrite?
- Which analysis produces that fact and what invalidates it?
- Which observable effects prevent movement or removal?
- How are source provenance and identities preserved or remapped?
- Is the pass deterministic and bounded?
- What near-miss test proves the pass is conservative?
- What metric or workload justifies enabling it at this level?

These questions belong in code review and in the
`gti-language` change workflow. If a question cannot be answered from GTI IR,
the optimization is blocked on architecture rather than delegated to C++.

## Implementation Milestones

### Milestone 0: document and freeze the boundary

- Adopt this proposal and cross-link it from compiler architecture, the agent
  skill, and the optimization change guide.
- Treat new HIR source-replacement passes as out of scope.
- Record unresolved semantic gates explicitly.

Acceptance criteria:

- an agent can identify the current bridge and intended MIR authority without
  reading `CppEmitter` end to end;
- optimization changes have one documented impact and review checklist;
- this documentation-only milestone does not change `VERSION`.

### Milestone 1: MIR integrity and bounded editor

Status: bounded foundation complete; broader framework is client-gated

- Extract reusable MIR validation, reachability, use indexing, deterministic
  printing, effect traits, controlled editors, and pass management.
- Add identity-pipeline and invalidation tests.
- Add optional before/after dumps using the performance tooling contract.

Acceptance criteria:

- `-O0` produces structurally identical MIR and schedules no transform;
- the first editor client repairs only its declared facts and preserves CFG;
- a deliberately malformed rewrite fails verification with a useful internal
  error;
- adding a MIR enum member fails classification coverage until handled;
- no generated artifact changes when optimization is disabled.

### Milestone 2: first MIR transform in shadow mode

Status: bounded proof complete. The primitive scalar grouping family is
implemented; matching every compatibility fold is explicitly not a prerequisite
for Milestone 3.

- Preserve comparison of the implemented MIR and legacy HIR decision.
- Add another fold, statistic, or remark only with a concrete optimization or
  migration client.

Acceptance criteria:

- the implemented primitive identity fold has matching MIR and near-miss
  coverage;
- shadow comparison does not itself authorize a MIR rewrite;
- disagreements are visible in tests and reports;
- MIR uses the same backend-neutral checked-integer evaluator as the legacy HIR
  pass: only value outcomes fold, while every failure outcome retains its
  checked operation.

### Milestone 3: optimized MIR reaches the C++ backend

Status: complete for source executable bodies.

- Preserve the general verified-MIR body route and its sealed whole-program
  preflight.
- Add a new transform only with replayable proof and an extension to
  `verifyMirOptimizationCoherence`.
- Keep HIR constant analysis confined to representation surfaces and shadow
  comparison; it is not executable body authority.

Acceptance criteria:

- `BackendInput::mir` is observably consumed;
- one optimized MIR snapshot drives every executable-body backend;
- no pass queries AST shape or C++ spelling;
- bodies have no AST/HIR execution fallback;
- the exact census and `-O0`/C++20 versus `-O3`/C++23 corpus oracle pass, with
  focused wider matrices where representation or lifecycle risk warrants them.

### Milestone 4: local CFG and value optimization

- Add bounded CFG simplification, reachability cleanup, and dead non-trapping
  value removal.
- Add propagation only with the required dominance and effect analyses.

Acceptance criteria:

- cleanup, loans, moves, and traps are preserved by structural and runtime
  tests;
- each pass declares and tests analysis invalidation;
- the pass pipeline is deterministic and terminates within its documented
  bound.

### Milestone 5: proof-carrying safety optimization

- Represent checked integer operations and their failure categories explicitly
  enough for range proofs to discharge individual checks.
- Measure and implement bounds, initialized-storage state, and arithmetic
  overflow as separate check families. Add the minimum CFG, dominance,
  predicate, and loop facts required by the selected family; range analysis is
  not mechanically ordered ahead of CFG simplification or predicate
  propagation when those passes establish its proof inputs.
- Remove checks only with a recorded proof. Backend assumptions may consume a
  verified MIR fact, but emitted C++ shape, public wrapper names, and intended
  class invariants are not proof.
- Preserve left-to-right checked arithmetic. A widened accumulator plus one
  final range check is not equivalent when an intermediate operation can
  overflow. Consider loop versioning only when a side-effect-free preflight
  selects a fast path and the original checked scalar loop remains an exactly
  equivalent fallback for failure origin, order, cleanup, and partial effects.
- Integrate safety-operation reporting.

Acceptance criteria:

- every discharged check has a GTI-level proof and a deterministic applied
  remark identifying its operation family, body, source site, and proof;
- near-miss checks remain in MIR and emitted code;
- O0/O2/O3 differential tests preserve success, failure category and origin,
  cleanup, aliasing, mutation barriers, and boundary behavior for every
  optimized family;
- native vectorization remarks and assembly support performance conclusions
  without becoming semantic authority.

### Milestone 6: interprocedural optimization

- Add conservative call effects and closed-world reachability.
- Consider devirtualization and inlining only after temporary lifetime, cleanup,
  layout, and cost-model requirements are met.

Acceptance criteria:

- unknown or virtual calls remain conservative without exact proof;
- provenance spans inlined or specialized bodies coherently;
- benchmarks show value across workload categories;
- a future backend consumes the same transformed calls.

## Unresolved Gates

Resolve these before the pass families that depend on them:

- a complete instruction/intrinsic/call effect model;
- place alias and escape rules across calls and returned references;
- general temporary lifetime and active-drop transitions in MIR;
- backend-neutral object layout, data layout, virtual tables, and calling
  conventions;
- the closed-world boundary once separate compilation or dynamic linkage exists;
- cost models and size policy based on representative benchmark data.

Each gate must become a language contract or backend-neutral IR fact. Resolving
one inside a pass or `CppEmitter` is architectural drift.

## Documentation And Release Effects

This proposal changes no shipped compiler behavior and requires no `VERSION`
increase. When milestones land, update together:

- this proposal's current-state, capability, and milestone status;
- `docs/architecture/optimization.md`, `hir.md`, and `mir.md`;
- MIR and optimizer maintenance contracts in the agent skill;
- `docs/plans/performance-tooling.md` for dumps, timings, and reports;
- README and CLI help for user-visible optimization behavior;
- focused compiler, CLI, and backend tests;
- `VERSION` when shipped compiler behavior or tooling changes.

The architecture is ready for sustained optimization work when a new local MIR
pass can be added with a pass implementation, focused analyses, verifier and
equivalence tests, and pipeline registration—without editing parser nodes,
semantic resolution, source-expression mapping, or backend-specific selection.
