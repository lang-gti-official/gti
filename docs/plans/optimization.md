# GTI Optimization Architecture Proposal

> **Plan status:** Non-canonical staged design. The implemented optimizer
> boundary is documented in
> [`docs/architecture/optimization.md`](../architecture/optimization.md).

Status: accepted; Milestones 1 through 5 are implemented for their bounded
local families. Milestone 6 has verified per-instance effects and exact closed
call-summary queries, but no interprocedural transformation. The completed
exclusive runtime-performance campaign and every follow-on optimizer row are
sequenced only in
[`implementation-sequence.md`](implementation-sequence.md).

Implementation update (2026-08-22): ADR 016 supersedes the migration mechanics
in this proposal. `BackendInput`, HIR replacement-table emission, mixed AST/HIR
body routes, and frontend-aware backend snapshots have been removed. The
driver now constructs one verified `LoweredProgram` after optimization, and
all production backends consume only that value. References below to those
deleted routes describe historical milestones, not current architecture.

Implementation update (2026-08-24): stable MIR instruction identity, typed
transformation evidence and independent replay, complete conservative
per-instance effects, scalar block parameters/edge arguments, acyclic scalar
promotion, unreachable cleanup, dead unused computation removal, the
client-backed pass/analysis manager, and opt-in MIR/report observability are
implemented. O1 owns this local pipeline. O2/O3 additionally run independent
bounds, initialized-storage-state, and arithmetic-overflow discharge passes
backed by canonical body-local proofs. O-MIR-03 and the first O-MIR-04 slice are
no longer blocked.

Implementation update (2026-08-24): O-MIR-05 adds one deterministic monotone
forward-dataflow substrate, migrates safety analysis without changing its
lattice or accepted proofs, and enables proof-carrying SCCP at O2/O3. O-MIR-06
adds dominance-derived natural-loop forests, bounded invariant and affine
induction facts. O-MIR-07 adds body-local place alias/escape facts and uses
them to preserve disjoint safety state across writes. The latter two are
analysis foundations, not loop or memory transformations.

Implementation update (2026-08-25): the O-MIR-05 through O-MIR-07 exit gates
are reconciled as complete. The shared solver now validates canonical
terminator slots and block-argument arity, rejects executable-edge retraction,
and preserves switch-default flow in the migrated safety client. Loop analysis
rejects repeated-edge block-argument ambiguity. Alias analysis treats union
fields as overlapping and analysis caches reject cross-program snapshot reuse.
O-MIR-08 (loop-derived bounds proof) and O-MIR-09 (one measured alias-safe
memory family) remain measured-defer, while the bounded interprocedural
campaign client was subsumed by `O-PERF-02`; standalone O-MIR-10 is parked.
None is implied complete by the analysis foundations.

Performance-priority update (2026-08-27): `O-PERF-00` through `O-PERF-03`,
`O-PERF-05`, and `O-GAME-01` are complete, and `O-PERF-04` is measured-defer
on exact clean commit `4dbb50bd`. The exclusive compiler gate is released.
The fixed-array bounds control remained a measured no-op. The admitted
complete-state fixed-array overflow workload originally measured 283.568 ms in
GTI versus 60.253 ms in idiomatic C++; exact remainder/mask transfer,
alias-safe fixed-array content envelopes, and finite loop-carried accumulator
thresholds now discharge all five hot arithmetic outcomes and all three hot
bounds outcomes. The clean implementation measured 61.807 versus 61.111 ms at
O2 and 61.055 versus 61.113 ms at O3. No campaign control regressed by more
than 0.4% relative to its semantic C++ twin. Bounded contextual summaries then
transported exact scalar and container-element facts through the closed vector
call family, then established one prefix-storage length authority and removed
only independently replayed private rechecks. A bounded follow-up carries the
same scalar envelope through exact indexed return borrows and finite closed-call
loop bounds; vector medians are now 1.042x idiomatic at O2 and 1.049x at O3.
The genuinely opaque scalar-loop preflight exposed only 0.34% value, so no
versioning transform was admitted. The residual backend audit then measured
the scalar-FP client at 1.005x semantic C++ and the closed-call vector client
at 0.109x semantic and 1.044x idiomatic; their native hot loops contain no
surviving call or failure-ABI boundary, so no speculative representation
change was admitted. Native, virtual, recursive,
owning-transfer, conflicting-context, mutating-call, and non-convergent
boundaries remain conservative. Exact report identities, raw samples,
profiles, MIR/report counters, and emitted-code hashes live in
[`performance-tooling.md`](performance-tooling.md).

The final game update on clean implementation commit `c5bbfaa8` measured
58.503 ms in GTI, 208.126 ms in semantic-equivalent C++, and 58.454 ms in
idiomatic C++, with an identical complete-state record. Fifty-three of 85
source safety operations are discharged through independently replayed MIR
proofs. Fixed-array, vector, scalar, managed-thread, compile-time, code-size,
and memory controls are recorded and accounted for in the performance plan.

The audit proposal is not adopted verbatim. The live safety analysis already
has a loop fixpoint and widening; prefix storage has no unchecked-read
vocabulary; the flagship vector inputs are statically bounded across difficult
closed call/container boundaries; and wrapping block speculation does not by
itself preserve GTI's first failure, left-to-right recurrence, or partial
effects. The completed exclusive sequence is `O-PERF-00` through `O-GAME-01`
in
[`implementation-sequence.md`](implementation-sequence.md). This architecture
plan does not own or duplicate that order.

Operational ordering is maintained in
[`implementation-sequence.md`](implementation-sequence.md). Infrastructure
continues to land only with a real client: the current analysis cache exists
for scalar promotion, folding, unreachable cleanup, and dead computation
removal, SCCP, safety analysis, and alias-aware safety invalidation. Loop/range
and alias facts remain available for later measured performance rows, but they
do not dictate a pass. General loop transforms, MemorySSA, devirtualization, an
LLVM backend, and broad optimizer frameworks remain deferred until a measured
bottleneck and the sole operational sequence require them.

This proposal defines how GTI grows from typed-HIR compatibility folding into
a maintainable optimizer without making AST shape, the C++ emitter, or a
future backend part of optimization semantics. Implemented behavior is
canonical only where recorded in
[`docs/architecture/optimization.md`](../architecture/optimization.md).

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

- Promising every future pass or analysis named by this staged plan.
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

Validated structural MIR is the sole executable-body authority. It has stable
body/block/instruction/value identities, typed block parameters and edge
arguments, places and projections, calls and dispatch, moves, loans, drops,
failure and cleanup schedules, per-instance effects, and deterministic
printing.

`OptimizationRequest` owns canonical source MIR and returns an
`OptimizedProgram`. At O1 and above the implemented pipeline performs
conservative scalar promotion, literal/compute/branch folding, unreachable
cleanup, and dead unused computation removal. The small analysis manager
caches CFG, dominance, value-use, effect, promotion, safety, sparse-constant,
natural-loop/range, and place alias/escape facts and revises/invalidates them
explicitly. O2/O3 additionally run proof-carrying SCCP and the implemented
safety-discharge families. Alias analysis already narrows safety-state
invalidation; loop/range facts do not yet authorize a transformation.

Every edit is expressed by stable MIR identity and typed proof evidence.
`verifyMirOptimizationCoherence` independently replays those records from
canonical source MIR; `LoweredProgramBuilder` consumes the result before
publishing optimized MIR to backends. A future backend does not reproduce any
optimizer decision.

HIR constant replacements remain only for non-body representation surfaces and
as a transitional shadow comparison for the existing fold. The bridge cannot
authorize a new CFG or dataflow transformation. Remaining architectural risk
is therefore bounded to retiring that final representation compatibility path,
not split executable-body authority.

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

MIR is not yet ready for every optimization. Body-local place alias/escape and
verified call effects are available, but memory versions, interprocedural
pointer identity, general temporary lifetime, concrete object layout, ABI, and
some primitive edge semantics remain incomplete. The capability gates below
make those gaps explicit instead of allowing passes to guess.

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
[the compiler library migration record](archive/compiler-library-migration.md).
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

Current reusable analyses, each introduced with a concrete client or the
nearest bounded prerequisite, are:

- predecessor/successor and reachability information;
- dominator trees;
- value definition/use queries;
- call graph and conservative call effects;
- place alias/escape facts;
- natural-loop, invariant, affine induction, and relational-bound facts;
- sparse typed constants and executable edges; and
- safety-specific integer ranges and known predicates.

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
| `-O2` | O1 plus proof-carrying SCCP and bounded bounds/storage/overflow check discharge; loop/range and alias/escape facts are available only to validated clients | request native `-O2` |
| `-O3` | currently the same GTI middle-end transforms as O2; interprocedural work remains gated by closed-world, cost, lifetime, and layout contracts | request native `-O3` |

Size-oriented optimization is a future independent policy; do not silently
make `-O2` or `-O3` mean size optimization. Debuggability options and remark
verbosity are also independent of semantic optimization level.

## Capability-Gated Pass Roadmap

| Pass family | Status | Authoritative IR | Required capabilities |
| --- | --- | --- | --- |
| typed constant folding | bounded family implemented | MIR operations | exact constant representation; operation edge semantics; trap classification; constant replacement rewrite |
| CFG simplification | bounded branch/unreachable family implemented | MIR blocks and terminators | predecessor/successor repair; reachability; cleanup-edge preservation; block verifier |
| dead-value elimination | bounded family implemented | MIR instructions and indexed uses | complete effect/trap table; drop/loan awareness; definition/use repair |
| sparse constant propagation | bounded scalar SSA family implemented | MIR CFG and values | executable-edge solver; typed lattice; typed proof/replay; CFG rewrite |
| copy propagation | deferred to a measured client | MIR values and places | dominance; exact type/access preservation; move and loan barriers |
| redundant load/store removal | analysis gates partly complete; transform absent | MIR places | place alias/escape; exact call effects; memory versions or a proved local substitute; lifetime boundaries |
| bounds/check elimination | bounded read/storage/arithmetic families implemented | MIR checked operations | integer semantics; range/predicate analysis; explicit check identity; proof reporting |
| loop optimization | analysis gate complete; transform absent | MIR loops and values | natural-loop forest; invariants/inductions; transform-specific legality and replay; cost evidence |
| devirtualization | deferred | HIR instance graph plus MIR calls | closed-world reachability; exact receiver proof; override graph; lifecycle/layout compatibility |
| inlining/specialization | one candidate among the `O-PERF-02` choices; transform absent | HIR call graph plus MIR bodies | conservative call effects; recursion handling; cost model; provenance composition; cleanup and temporary lifetime correctness; exact profile/proxy evidence |

Do not implement a pass when one of its gates is represented only by C++
emission behavior. Add the missing backend-neutral fact first.

The implemented local sequence is:

1. MIR constant folding;
2. conditional and switch simplification;
3. unreachable block removal;
4. dead non-trapping value removal;
5. sparse propagation and another CFG cleanup; and
6. range-based check elimination.

Loop/range and place alias/escape analyses are now explicit prerequisites, not
implemented transforms. O-MIR-08 and O-MIR-09 remain measured-defer;
`O-PERF-02` may consume the bounded O-MIR-10 gates only if closed-call
profiling selects that mechanism. The maintained sequence in
`implementation-sequence.md` does not leave an open-ended "future
propagation" phase.

Each cleanup repetition must be intentional and bounded in the configured
pipeline.

## C++ Backend Migration

The migration must avoid two permanent optimization authorities.

### Stage A: establish MIR infrastructure without output changes

Status: complete. Reusable verification/repair, deterministic printing,
complete conservative effects, stable typed edits, source replay, the
client-backed pass/analysis manager, and opt-in dumps/reports are implemented.
General infrastructure still grows only with clients.

- Maintain deterministic MIR printing, public validation utilities, effect
  traits, and the bounded controlled editor. Add broader editor or analysis
  management only with a transform that consumes it.
- Return an `OptimizedProgram`; keep `-O0` unchanged while changed passes carry
  explicit reports and fresh verification.
- Keep the existing HIR `OptimizationResult` and C++ emitter behavior intact.
- Verify that `-O0` is structurally identical and every queued edit is atomic.

### Stage B: prove the shadow path

Status: complete for the compatibility fold. Primitive identities plus admitted
comparison, logical, floating arithmetic, and conversion computations are
implemented and cross-checked. New passes do not use shadow authority.

- Preserve the implemented primitive identity cross-check until its body family
  consumes optimized MIR.
- Port another compatibility fold only when a measured transform client or an
  active migration phase requires it.
- Treat disagreement as a compiler test failure, not as permission to choose
  whichever result the C++ emitter prefers.

### Stage C: consume optimized MIR

Status: complete. Every executable body consumes verified optimized MIR through
the sealed `LoweredProgram`; there is no AST/HIR execution fallback.

- Preserve verified MIR as the sole executable-body authority.
- Keep AST/HIR available only before the lowered backend frontier for syntax,
  semantics, concrete instances, and representation facts.
- Reject an unsupported MIR body for the whole backend; never reintroduce a
  mixed or fallback execution route.
- Test generated behavior at `-O0` and each enabled GTI level while requesting
  native `-O0` in structural optimizer tests so the native compiler cannot hide
  a GTI middle-end error.

### Stage D: retire the source replacement bridge

Status: executable-body bridge retired. `BackendInput` and frontend-aware
backend snapshots are removed; every backend consumes only `LoweredProgram`.
HIR compatibility replacements remain solely for bounded non-body
representation surfaces and the existing fold's shadow comparison.

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

Status: complete; broader framework remains client-gated

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

- the optimized MIR owned by `LoweredProgram` drives every executable-body
  backend;
- source/optimized coherence is proven once at lowered-program construction;
- no pass queries AST shape or C++ spelling;
- bodies have no AST/HIR execution fallback;
- the exact census and `-O0`/C++20 versus `-O3`/C++23 corpus oracle pass, with
  focused wider matrices where representation or lifecycle risk warrants them.

### Milestone 4: local CFG and value optimization

Status: complete for the bounded local scalar/CFG family.

- Implemented scalar block parameters/edge arguments, conservative acyclic
  scalar promotion, bounded branch simplification, reachability cleanup, and
  dead non-trapping value removal.
- Implemented deterministic executable-edge dataflow and typed scalar SCCP at
  O2/O3. Sparse literal and branch edits carry exact evidence and are replayed
  independently from source MIR.
- Further CFG/value work is assigned only through numbered O-MIR milestones.

Acceptance criteria:

- cleanup, loans, moves, and traps are preserved by structural and runtime
  tests;
- each pass declares and tests analysis invalidation;
- the pass pipeline is deterministic and terminates within its documented
  bound.

### Milestone 5: proof-carrying safety optimization

**Implementation status:** the first bounded slice is complete. Exact
fixed-array read bounds, exact sparse-storage read bounds/initialization, and
checked integer add/subtract/multiply/negate overflow outcomes can be
discharged at O2/O3. Canonical analysis, typed outcome metadata, controlled
editing, independent replay, backend consumption, schema-v3 per-family
reporting, near-miss tests, and O0/O2/O3 native-`-O0` differential evidence are
implemented together. Array writes/borrows, prefix storage, remaining numeric
operators, and interprocedural range proofs remain future measured slices.
Schema v3 additionally records sparse-constant proof vocabulary. Place alias
facts now preserve known-disjoint safety state across writes but do not
discharge checks.

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

#### O-MIR-08 measured gate

The 2026-08-26 exact-main audit deferred the loop-derived fixed-array client.
It found a broad MIR proof near miss but not an accepted runtime optimization:

- a 4-element affine source loop converges before interval widening and
  discharges its fixed-array read, while the equivalent 64-element loop widens
  the loop-carried place fact and retains its read and write checks;
- ordinary source MIR lowering does not create block parameters or edge
  arguments. The only production creator is `rewriteMirScalarPlace`, whose
  current topological-order requirement rejects every cyclic CFG. O-MIR-06's
  affine induction tests therefore exercise constructed MIR rather than a
  source-produced optimization client;
- `examples/12-fixed-arrays.gti` already discharges the canonical short loop,
  so widening O-MIR-08 around that case would be a no-op;
- `vector-checked-loop` retains four dynamic vector bounds checks that require
  interprocedural/inlining facts rather than fixed-array induction;
- the managed-thread workload retains 34 bounds checks, but its source MIR has
  no loop-carried block parameters and its checks are not a bounded O-MIR-08
  fixed-array client;
- the new `fixed-array-loop-proof` workload records the 64-element widening
  case. At native O3, current checked emission and a manual direct-access upper
  bound produced identical hot-function assembly and no runtime improvement.

The smallest sound prerequisite is a core scalar-promotion extension in
`rewriteMirScalarPlace` (`src/compiler/mir.cpp`) with two inseparable parts.
First, construct block parameters and matching edge arguments for one
reducible, single-latch loop at a time. Second, narrowly admit a
failure-sensitive body when the promoted place is a non-owning trivial scalar
that is absent from invoke operands/results, cleanup metadata, loans, drop
obligations, aliases, and failure-path loads, and whose instructions meet the
existing simple-load/write and trivial-ownership filters. Carry the scalar
value through both invoke edges wherever a downstream load requires it without
changing the invoke, failure origin, failure edge, cleanup order, ownership
event, or checked operation. Keep rejecting all other failure-sensitive,
cleanup-sensitive, and non-trivial cases. Verify the candidate body, rebuild
value uses, retain stable provenance, and add positive and near-miss coverage
for multiple latches, irreducible control flow, failure-path uses, calls
touching the place, cleanup, checked arithmetic, and mutation barriers.

This prerequisite needs no MIR representation change; existing block
parameters and edge arguments are sufficient. It does require a change in the
core MIR implementation, so an optimizer pass must not duplicate it. The
prerequisite is documented but is not active work merely to improve the
fixed-array bounds workload. It is also not an automatic prerequisite of
`O-PERF-01`: current safety analysis already carries place facts through a
fixpoint, and that row first tests a fixed-array content summary plus precise
widening/transfer functions. Reconsider cyclic scalar promotion only when a
focused source-MIR probe demonstrates that place-based analysis cannot carry
the required fact. Implement O-MIR-08 only if a replayable loop fact reaches
real source MIR and the workload shows runtime value after native
optimization. The safety client must consume only loop, dominance, alias,
effect, and fixed-array facts, record near misses, invalidate safety analysis
after edits, replay coherently, use `MirProgramEditor`, preserve O0 identity
and all checked failure behavior, and remain backend-neutral.

### Milestone 6: interprocedural optimization

Status: the first bounded contextual proof-summary client is complete under
`O-PERF-02`; broader interprocedural transformation remains deferred.

- Conservative per-instance call effects and exact closed static-summary
  queries are implemented and independently verified.
- One deterministic whole-program analysis transports exact scalar arguments,
  returns, mutable object fields, private prefix-storage state, and
  structurally admitted indexed-element envelopes through concrete
  source-defined static calls and constructors. It uses bounded rounds and
  per-context updates, stores values separately from effects, and rejects
  recursion, native or virtual calls, owning transfers, synchronization,
  incomplete call families, and unsupported cleanup/lifetime shapes.
- The analysis is snapshot-scoped. Any semantic body edit invalidates its
  program-wide cache and all derived body safety facts; independently replayed
  metadata attachment explicitly preserves the immutable result. Replay
  validates all claims against one source and one candidate snapshot rather
  than recomputing the whole program per claim.
- Closed-world reachability as a removal authority, body cloning,
  devirtualization, and inlining are not implemented.
- Devirtualization remains deferred. Do not generalize the selected family
  until temporary lifetime, cleanup, layout, closed-world, and cost-model
  requirements are met and another workload measures value.

Acceptance criteria:

- unknown or virtual calls remain conservative without exact proof;
- provenance spans inlined or specialized bodies coherently;
- benchmarks show value across workload categories;
- a future backend consumes the same transformed calls.

## Unresolved Gates

Resolve these before the pass families that depend on them:

- memory versioning for transformations that must distinguish intervening
  writes; add MemorySSA only after a measured client proves existing place and
  effect facts insufficient;
- transform-specific loop legality, failure-order proof, and cost evidence;
- general temporary lifetime and active-drop transitions in MIR;
- backend-neutral object layout, data layout, virtual tables, and calling
  conventions;
- the closed-world boundary once separate compilation or dynamic linkage exists;
- cost models and size policy based on representative benchmark data.

Each gate must become a language contract or backend-neutral IR fact. Resolving
one inside a pass or `CppEmitter` is architectural drift.

## Documentation And Release Effects

The implemented optimizer and observability milestones change shipped
compiler/CLI behavior and require a `VERSION` increase. For later milestones,
update together:

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
