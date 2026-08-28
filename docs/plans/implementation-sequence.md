# GTI Dependency-Ordered Implementation Sequence

> **Plan status:** Accepted operational sequencing and status plan. It does not
> define current language semantics or replace the detailed design documents
> for an individual subsystem.

Checkpoint: maintained against current `main`; canonical behavior remains in
`docs/language/` and `docs/architecture/`.

This document turns GTI's architecture reviews, language review, accepted
plans, and current implementation checkpoint into one executable work queue.
It answers three questions for every substantial piece of future work:

1. what must be decided or implemented first;
2. what one coherent change should accomplish; and
3. what evidence allows the next change to begin.

Current behavior remains authoritative in [`docs/language/`](../language/) and
[`docs/architecture/`](../architecture/). The durable release capability map
remains [`roadmap-to-1.0.md`](roadmap-to-1.0.md). Current implementation
evidence belongs in architecture documents and tests; the older
[`compiler-roadmap-status.md`](archive/compiler-roadmap-status.md) is retained
only as a historical checkpoint. Detailed domain decisions remain in the other
files under `docs/plans/`. The documents under
[`docs/third-party-audit/`](../third-party-audit/) are evidence and review
input, not a second implementation queue.

## How To Use This Plan

Ordinary feature work should select one row from this plan, or one explicitly
named bounded sub-slice. The immediate backend-authority recovery campaign is
the deliberate exception: one declared implementation phase may combine the
applicable `M-EXEC-01`, `M-FAIL-01`/`Q-FAIL-01`, and `M-BACK-01/02` slices when
that is required to put a complete body family into production. The phase is
the outcome boundary; it may land as several reviewable commits, but it must
not stop at an IR-only checkpoint when the matching production cutover is
within the declared scope.

Every row has one of these states:

| State | Meaning |
| --- | --- |
| **ready** | All prerequisites are implemented or the row is design-only. |
| **blocked** | A named prerequisite has not passed its exit gate. |
| **active** | One task owns the row and has recorded its bounded scope. |
| **done** | The implementation, tests, and canonical documentation passed the exit gate. |
| **measured defer** | No demonstrated client or payoff justifies the work yet. |
| **later breadth** | No current systems-readiness workload requires the broader form; evidence may move it earlier. |

Readiness role is separate from dependency state:

- **systems-ready** means the capability is required by one or more accepted
  readiness workloads;
- **bounded-first** means the first useful form should ship with its client
  while broader forms remain explicit;
- **design-first** means a cross-cutting contract must be settled before the
  first executable client; and
- **measured defer** means a benchmark, client, or ownership design must exist
  before implementation is scheduled.

Under [ADR 012](../decisions/012-outcome-first-systems-readiness.md), `1.0` is a
soft, revisable systems-readiness goal, not a scheduling horizon. Legacy
pre/post-version labels in superseded evidence do not control this queue.

For each selected row or recovery phase:

1. re-check the prerequisite evidence and current source rather than trusting
   an old audit line number;
2. state the user-facing program/API/workflow, owned files/layers, and explicit
   non-goals;
3. preserve the phase direction syntax -> semantics -> HIR -> MIR -> backend;
4. apply the executable-authority admission rule: new or changed runtime
   behavior must reach production through a verified MIR-emitted family, never
   through a new AST/semantic/HIR-only C++ emission case;
5. add focused positive, negative, structural, and diagnostic tests at the
   owning layer;
6. run the row's focused gate and the broader matrix it names;
7. update every covered row, the checkpoint, and the owning
   architecture/language
   document in the same change; and
8. stop when the declared row or recovery-phase exit gate passes.

## Decisions Made By This Sequence

This plan accepts the following conclusions from the reviews without treating
every review recommendation as a release commitment:

1. **Concurrency semantics are fixed before public concurrency.** ADR 008
   adopts safe data-race freedom, structural transfer/share facts, the concurrent-global
   boundary, sequentially consistent first atomics, and owned automatic-join
   threads. A bounded public profile is systems-readiness work; detach, weak
   memory-order breadth, and advanced reclamation are not.
2. **Concurrency starts from the adopted language model, not a pthread
   wrapper.** Implementation follows the capability, lifetime, ordered-
   execution, failure, runtime, and MIR prerequisites in this plan.
3. **The executable critical path remains places -> initialization ->
   temporaries/drops -> ordered evaluation -> defined-failure edges ->
   MIR-backed emission.** These facts unblock mutable ranges, allocators,
   richer FFI, backend-independent failure, and public concurrency.
4. **Layout queries precede allocator and broad FFI work.** `sizeof` and
   `alignof` may initially support only types whose layout GTI owns; this does
   not imply a stable GTI ABI, packed records, unions, or manual lifetime.
5. **A general pass manager is not a standalone prerequisite.** The first
   transforming MIR slice adds only the editor, repair, and invalidation it
   actually uses. Dominance continues to use full recomputation until a real
   client proves incremental maintenance worthwhile.
6. **A generic MIR dataflow solver does not automatically become semantic
   authority.** ADR 001 still makes semantics own language validity and loan
   endpoint selection. Any migration of ownership-state analysis requires a
   separate authority decision and cross-phase contract.
7. **LLVM remains a required dependency but not a default design answer.** It
   may implement language-neutral algorithms or private storage behind
   GTI-owned interfaces. It does not define GTI identities, semantics,
   serialized forms, public APIs, or cross-phase representations.
8. **Type interning remains deferred.** Types were about three per cent of
   retained semantic memory in the audit workload after the occurrence-table
   fix; snapshot/context ownership and a type-allocation benchmark are still
   missing.
9. **Evaluation is strictly left to right.** ADR 010 fixes receiver, argument,
   operand, assignment-target, initialization, full-expression, temporary,
   cleanup, and program-wide initialization order. M-LIFE/M-EXEC represent the
   rule; production conformance belongs to matching M-BACK migrations.
10. **New executable behavior must be MIR-owned.** New or changed language
    behavior that affects runtime values, effects, control flow, ownership,
    lifetime, cleanup, failure, or initialization must lower through verified
    MIR and pass the general backend preflight. Recording MIR while production
    executes a source-tree interpretation is not completion. AST, semantics,
    and HIR may still supply declaration, identity, layout, ABI, and target
    representation facts; they may not regain executable body authority.

## Immediate Backend-Authority Recovery Campaign

This campaign is **complete for source executable bodies**. The retained phase
list records how the cutover was reached. Follow-on backend work concerns
generated-item representation contracts, not restoration of the removed
AST/HIR executable route.

The campaign maximizes coherent work per phase rather than accumulating more
shadow infrastructure:

1. **First production seam — done (`M-BACK-01`, `scalar-leaf-v1`).** The
   deliberately failure-free fixed-width-integer leaf-function family now
   emits from verified optimized MIR at C++20/C++23 and O0/O1/O3. Ineligible
   bodies remain wholly on the explicit compatibility path; no body mixes MIR
   scheduling with AST/HIR evaluation.
2. **Scalar CFG expansion — done (`M-BACK-02`, `scalar-cfg-v1`).** The
   coherent call-free, failure-free, cleanup-free scalar CFG family now emits
   wholly from verified MIR: scalar locals and parameters, selected scalar
   computations, initialization and assignment, and branch, switch,
   short-circuit, loop, and return CFG. Its dedicated structural and runtime
   gates cover O0/O1/O3 and C++20/C++23 while near misses stay compatible.
3. **Scalar direct-call expansion — done (`M-BACK-02`,
   `scalar-direct-call-v1`).** Exact ordered scalar calls now execute wholly
   from verified MIR across a closed acyclic static-call graph. MIR v20 owns
   the bounded defined-failure summary used to normalize proved-false calls;
   dedicated mutation and O0/O1/O3 × C++20/C++23 gates keep recursion,
   checked/unknown targets, and header near misses compatible.
4. **Class-default cleanup — done (`M-BACK-02`,
   `class-default-cleanup-v1`).** Exact generated-default construction of empty
   concrete class locals, scalar return loading before cleanup, bounded source
   destructors, reverse lexical drops, and one normal cleanup boundary now emit
   wholly from verified MIR through strict raw-storage lifetime slots. Its
   dedicated structural and runtime gates cover O0/O1/O3 and C++20/C++23.
5. **Remaining failure-free closure — done (`M-BACK-02`,
   `owned-lifecycle-call-v1`).** One atomic acyclic free-function graph now
   constructs, moves, passes, transfers, and drops exact passive-scalar-field
   owners wholly from verified MIR. Exact constructor/destructor schedules,
   reverse HIR caller closure, strict lifetime slots, and fail-closed
   source/optimized-MIR coherence keep unsupported connected shapes wholly
   compatible.
6. **Failure-capable closure — done (`M-EXEC-01` +
   `M-FAIL-01`/`Q-FAIL-01` + `M-BACK-02`,
   `scalar-failure-callgraph-v1`).** One bounded hosted no-argument `int32_t`
   component now carries exact checked-integer records through an atomic closed
   call graph, runs verified failure cleanup, reports once, and exits 70. Its
   gate covers O0/O1/O3 and C++20/C++23; broader signatures, lifecycle failure,
   initialization, double failure, embedding, and callbacks remain separate
   work rather than implied completion of M-FAIL-01.
7. **Final authority cutover — done (`M-BACK-02`).** Every source body in the
   reviewed `tests/mir_census_baseline.json` inventory emits through
   `CppMirBodyEmitter` from verified optimized MIR and copied representation
   rows. Functions,
   constructors, destructors, lambdas, class initializers, module
   initialization, hosted startup, generic specializations, ownership,
   failure, storage, callable, and native-boundary forms are covered by the
   general route. The no-MIR `CppEmitter` construction path and executable AST
   statement emission are removed. The exact census, endpoint corpus oracle,
   structural mutation tests, and focused native runtime matrices provide the
   post-cutover evidence described below.

Backward compatibility with retired compiler-route behavior or with
the textual form of generated C++ is not a migration requirement. GTI has no
downstream source ecosystem that justifies preserving those implementation
details. A family that passes its MIR production exit gate deletes its replaced
AST/HIR production route immediately and cannot retain it behind a legacy mode
or fallback flag. No unmigrated source-body family or public direct-emitter API
remains. This does not
permit breaking an explicitly accepted language semantic, runtime ABI, or
native-interoperability contract.

### Post-Cutover Evidence

The differential oracle was removed with the old executable emitter. Keeping
that route callable for comparison would violate the architecture the oracle
was meant to establish. The current evidence is:

- an exact reviewed per-example census in `tests/mir_census_baseline.json`;
- `tests/mir_census_test.py`, which rejects every body-inventory change;
- `tests/mir_cutover_corpus_oracle_test.py`, which builds and runs every
  example at `-O0`/C++20 and `-O3`/C++23, checks exact body ownership, and
  compares observable behavior between endpoints;
- structural verifier and backend tests that mutate MIR and representation
  inventories to prove fail-closed behavior; and
- focused O0/O1/O3 by C++20/C++23 runtime matrices for high-risk ownership,
  cleanup, failure, and call-graph forms.

The census is an inventory contract, not a coverage percentage. Any added,
removed, or changed body count must be reviewed and the corpus oracle rerun
before its baseline is refreshed. Generated C++ text is not a public contract;
marker rows are only an ownership witness.

ADR 016 is complete. Hosted entry, program initialization, structural
operators, callable invocation, lifecycle cleanup, same-thread native callbacks,
and concrete generic function/constructor instances all have exact target-
independent identities, payloads, dependencies, and roots. Ordinary C/runtime
boundaries remain ABI declaration/body rows rather than generated wrappers.
`BackendInput` and the AST/semantic/HIR C++ declaration route are removed; all
production backends receive only an immutable `LoweredProgram` plus explicit
backend policy.

## Systems-Readiness Outcome Lanes

The queue exists to make these programs possible. A prerequisite row should be
scheduled with the nearest lane it unblocks instead of becoming an open-ended
infrastructure project.

| Outcome lane | First dependency path | Acceptance signal |
| --- | --- | --- |
| Real C-library wrapper | `S-LAYOUT-01/02` -> `S-ABI-01/02/03` -> opaque-handle sub-slice of `S-FFI-02` -> `S-FFI-02` families F1/F2/F3; independently `M-LIFE/M-FAIL/M-BACK` -> `S-CALL-01` | Layout-stable records, exact symbols, nominal pointer-only handles, and named same-thread callbacks share one generated C/C++ adapter header. A wrapper can hide C structs or C++ classes behind ordinary GTI RAII. The named acceptance client is GLFW 3.4: all 124 functions, all five records, callback typedefs, opaque handles, and Vulkan-facing declarations bind with no GTI-caused omission and cross a deterministic C harness. |
| Arena or pool allocator | `M-LIFE-01` + failure/layout facts -> `S-ALLOC-01/02` -> one `S-ALLOC-03` client | Application GTI owns allocation policy and one container/value family proves initialization, failure, and cleanup. |
| Multithreaded work queue | `M-LIFE/M-EXEC/M-FAIL` -> `C-MIR/RUNTIME/CALL` -> `C-ATOM/THREAD/SYNC` -> `C-CONFORM` | Owned tasks, SC atomics, mutex-guard access, join, and worker failure work through public GTI. |
| Renderer/game update loop | mutable ranges/views + completed vector/string + exact domain operators + time/files/allocation | A frame/update workload mutates collections and domain values without raw-pointer escape hatches. |
| Compiler-style AST/protocol | `M-LIFE-01` + layout -> `L-SUM-01` | Generic payload cases support exhaustive move/borrow matching and deterministic cleanup. |
| Fallible pipeline | `M-LIFE/M-EXEC` -> `L-ERR-01` plus `expected`/host APIs | Propagation is concise, exact-type, and cleanup-correct across a nontrivial call chain. |

The first four user-facing lanes should normally outrank unrelated
generalization work once their immediate prerequisites are ready. This does
not permit skipping a correctness dependency; it prevents proof machinery with
no named consumer from displacing a bounded executable slice.

## Verified Starting Point

The following foundations are complete and should not be reopened merely to
start a later phase:

| Foundation | Evidence at 0.144.0 |
| --- | --- |
| Numeric semantics | Checked fixed-width operators remain the default; explicit fixed-width wrapping, saturating, and `expected`-returning checked-result add/subtract/multiply share one private `APInt` authority and public `<std/numeric>` API; exact IEEE binary32 and binary64 use GTI-owned width-tagged bits and private `APFloat` computation. |
| Ownership | Shared read-only loan identity, bounded stable-place exclusive reborrows, parent suspension/reactivation, single-origin read-only owner dependencies, and exact single-threaded global/static borrow returns reach verified MIR. |
| MIR integrity | CFG, places, values, loans, drops, effects, use indexes, and deterministic printing exist; fresh GTI-ID dominance verifies value availability. MIR v18 retains bounded `Invoke`/`PropagateFailure` edges, fixed-record parameters, and deterministic failure cleanup while adding caller-owned ordinary-call parameter stages, normal-edge-only initialization for one cleanup-owning result shape, and exact local-detector or ownership-free static direct-call argument edges after an earlier prepared owner. MIR v19 added verifier-owned source/identity-fold provenance for `Compute/Literal` instructions so transformed production emission does not consult the HIR replacement table. MIR v20 introduced function definition provenance and the bounded function `mayRaiseDefinedFailure` vector. MIR v21 makes the canonical effect result cover functions, constructors, and destructors; retains the acyclic closed scalar/static-call proof; adds the exact class-default-cleanup proof; and derives the exact passive-scalar-class constructor/destructor/function closure consumed by `owned-lifecycle-call-v1` after separate source/MIR graph and lifecycle verification. The lowerer verifies an all-conservative provisional program, derives the three-kind result, then lowers and verifies the exact final facts; generic verification accepts conservative true but rejects unproved false claims and exact `None`/`DirectCall` propagation drift. It retains exact borrow origins, synchronization records, conservative `PackFold`, and artifact-local failure identity. |
| LLVM boundary | One mandatory LLVM 18-22 build; installed headers are LLVM-free; only the approved support link surface is used. |
| Compiler performance | LSP HIR-stopped analysis preserves concrete generic diagnostics while skipping failure metadata/MIR; indexed source locations, instance delta analysis, tooling-occurrence opt-out, and HIR instance indexing are implemented. |
| Driver/build | Direct compilation and manifest `build`, `check`, `run`, `test`, `clean`, and `metadata` share compiled compiler/driver libraries; executable/test kinds and direct/project execution-profile selection resolve through driver-owned plans. |
| Tooling | Formatter, Tree-sitter shipped-source parsing, diagnostics, semantic tokens, hover, completion, and definition have tested foundations. |
| Compiler-private capabilities | Source roles distinguish application, prelude, and physical standard-library units; `gti_internal` declarations and presentation are trusted-only, private types bind by exact prelude declaration identity, and application forging is `GTI-S2058`. |
| Transfer/share capabilities | `SemanticTypeTraits` and HIR retain structural transfer/share facts for concrete types; C++-familiar nominal attributes implement safe opt-out, interface requirements, and unsafe positive assertions with `GTI-S2059`. |
| Concurrent global policy | Explicit single-threaded/concurrent selection reaches semantics, HIR, and MIR; `GTI-S2060` enforces immutable share-capable process-wide storage only in the concurrent profile. |
| Synchronization IR | C-MIR-01 gives HIR/MIR backend-independent thread spawn/join, atomic load/store/RMW/compare-exchange, and mutex lock/unlock identities with validated atomic orders, concurrent-profile enforcement, deterministic serialization, and exhaustive non-speculatable/non-removable/non-reorderable effects. |
| Place/ownership authority | M-OWN-01 defines one snapshot/body-scoped value key, exhaustive equal/prefix/disjoint/may-alias relation, finite ownership-state transfer, and semantics -> HIR -> MIR authority/invalidation contract. |
| Temporary/drop authority | M-LIFE-01 gives supported lexical storage and materializing values typed HIR/MIR obligations, exact cleanup descriptors, lifecycle transitions, normal-edge verification, and recursive cleanup-owning global/static rejection. |
| Evaluation design | ADR 010 and Execution Section 4.2 define strict left-to-right evaluation, target-first assignment, direct destination materialization, LIFO full-expression obligations, reverse partial cleanup, and lexical dependency-first program initialization. |
| Target/layout queries | Exact `os`/`vendor`/`arch` facts and supported-triple errors feed one GTI-owned 64-bit little-endian layout. Type-only `sizeof`/`alignof` expose exact unsigned-64 frontend constants for supported scalars, pointers, integral scoped enums, passive unions, aliases, and positive concrete arrays; installed probes check the host facts against each native build target. |
| Performance measurement | A hermetic, threshold-free benchmark runner records strict workload descriptors, correctness digests, exact build commands and tool identities, emitted-code evidence, deterministic raw samples, and a checked-vector GTI/semantic-C++/idiomatic-C++ baseline. |
| Callable design | One accepted concrete identity, exact signature, read/mut/once capability, capture/lifecycle, and confined/owned escape contract serves algorithms, tasks, and callbacks; local copy/move environments plus exact generic return and one-field owner transport implement its current bounded lifecycle. |
| Concurrency design | ADR 008 defines explicit single-threaded/concurrent profiles, safe data-race freedom, transfer/share facts, owned-only automatic-join tasks, ordered scalar atomics, scoped mutex guards, global policy, and contained worker failure. Public managed threads, ordered scalar atomics, and mutex guards implement the bounded systems-ready surface. |
| Defined failure | ADR 007 defines allocation-free records, cleanup-preserving propagation, hosted/embedding/task containment, and original-record re-raise at join. Landed M-FAIL-01 slices provide exact outcome/propagation identity, deterministic artifact descriptors and detector sites, plus verified fixed-record failure edges and cleanup for eligible full-expression-root scalar operations and exact later local-detector or static direct-call arguments after prepared owners. |

MIR is the sole executable-body authority. It still has bounded verifier gaps
around complete ordered parameter/result materialization, additional nested or
owning failure edges, partial-constructor rollback, and double failure, but
those gaps cannot select another executable route. Object layout, ABI, and
generated representation are combined with optimized MIR at the complete
`LoweredProgram` frontier. Unsupported contracts fail before backend emission.

## Dependency Map

```mermaid
flowchart TD
  R["D-LANG-01 restriction ledger"] --> MA["D-MEM-02 adopted memory-model decision"]
  MD["D-MEM-01 memory-model proposal"] --> MA
  R --> EO["D-EXEC-01 adopted evaluation and full-expression contract"]
  R --> FC["D-FAIL-01 failure contract"]
  FC --> MA
  R --> DC["D-CALL-01 callable ownership and escape contract"]

  PA["M-OWN-01 place and ownership-state authority"] --> IP["M-OWN-02 indexed places and definite initialization"]
  IP --> TD["M-LIFE-01 temporary and active-drop authority"]
  EO --> TD
  IP --> SD["M-OWN-03 stored and escaping mutable dependencies"]
  TD --> SD
  TD --> OE["M-EXEC-01 per-family ordered MIR lowering"]
  FC --> MF["M-FAIL-01 + Q-FAIL-01 failure IR/runtime substrate"]
  OE --> MF
  OE --> MB1["M-BACK-01 scalar-leaf-v1 done"]
  MB1 --> MB2["M-BACK-02 executable-body authority cutover done"]
  MF --> MB2

  R --> DL["S-LAYOUT-01 target data-layout contract"]
  DL --> SA["S-LAYOUT-02 sizeof and alignof"]
  SA --> NR["S-ABI-01 native-record proposal"]
  NR --> NRI["S-ABI-02 bounded native records"]
  NRI --> NH["S-ABI-03 generated C/C++ bridge header"]
  SA --> AL["S-ALLOC-01 allocator and provenance contract"]
  TD --> AL
  FC --> AL

  R --> CP["I-CAP-01 private capability identity"]
  CP --> MF
  CP --> CT["C-TYPE-01 transfer/share capabilities"]
  MA --> CT
  CT --> CG["C-GLOBAL-01 global/static policy"]
  CG --> AT["C-ATOM-01 atomic first slice"]
  CG --> TH["C-THREAD-01 joined owned thread"]
  MB2 --> TH
  TD --> LC["L-CALL-01 foundational owned callables"]
  DC --> LC
  DC --> CC["C-CALL-01 thread-task contract"]
  CT --> CC
  MF --> CC
  CC --> TH
  SD --> MX["C-SYNC-01 mutex and guard-tied access"]
  TH --> MX

  IP --> RG["L-RANGE-01 dedicated element/range loans"]
  TD --> RG
  RG --> MI["L-RANGE-02 mutable iteration and invalidation"]
  MI --> VW["L-RANGE-03 spans and dynamic views"]
  VW --> ALG["L-RANGE-04 foundational algorithms"]
  LC --> ALG

  TD --> SUM["L-SUM-01 payload enums and matching"]
  TD --> Q["L-ERR-01 propagation operator"]
```

This is an abbreviated critical-path view. The `M-EXEC-01` edge into M-BACK is
per selected body family, not a requirement to finish the entire row before
production emission starts. Design rows may run in parallel. Rows that edit the
same semantic or lowering authority must serialize even if the graph has no
logical edge between them.

## Ready Queue

This is the sole compiler implementation queue. At adoption, already in-flight
work completed its original scope before the reconciled `main` commit activated
the exclusive structural gate. That gate has now passed; its preserved record
is the
[archived structural-refactor plan](archive/compiler-structural-refactor.md).

The structural sequence completed on 2026-08-28. Its detailed measurements and
exact equivalence evidence are retained in the
[archived structural-refactor record](archive/compiler-structural-refactor.md).

| Order | ID | State | Phase outcome | Exit evidence |
| --- | --- | --- | --- | --- |
| 1 | `R-STRUCT-00` | **complete** | Freeze one exact behavioral, dependency, and build baseline. | Reconciled commit `4d4c8ae2`, the integration broad validation, controlled build measurements, and the complete artifact/MIR/diagnostic oracles are recorded in [`compiler-structural-baseline.json`](compiler-structural-baseline.json). |
| 2 | `R-STRUCT-01` | **complete** | Consolidate semantic request reset and partition compiler/backend tests by ownership. | One request owner/reset passes reuse regressions; the manifest-backed 125-test compiler aggregate, 81-test emitter census, focused suites, and exact-census corpus sweeps pass. |
| 3 | `R-STRUCT-02` | **complete** | Separate lexical-unit/directive ownership, semantic catalog and body/session state, callable authority, constexpr/flow services, and HIR instance access. | Source/cache/recovery and the 125-test semantic aggregate pass; the sealed catalog, explicit callable projection, narrow HIR instance provider, three semantic headers, static boundary test, self-contained compilation, and exact three-object analyzer-facade rebuild prove the ownership/include boundary. |
| 4 | `R-STRUCT-03` | **complete** | Separate MIR model/query, canonical edit/replay, body/program verification, failure effects, body/program lowering, hosted startup, and lowered-program closure. | Separate service/header boundaries, fail-closed component mutation tests, optimizer suites, MIR/lowered census, hosted startup, and the 444-mode cutover oracle pass unchanged. |
| 5 | `R-STRUCT-04` | **complete** | Make one immutable C++ body-emission plan authoritative and separate backend preflight, support text, and output writers. | Pointer-free body/program plans outlive input, preflight remains fail-closed, writer responsibilities are separate, and baseline C++20/C++23 O0/O1/O3 artifacts are byte-identical. |
| 6 | `R-STRUCT-05` | **complete** | Close dependency, include, CMake/test, installed-target, and measured build boundaries. | Exact 444-configuration equivalence, 141/141 CTest, installed libraries/toolchain, formatter/Tree-sitter/LSP/docs, one-object focused rebuilds, -48.9% longest Release edge, and bounded shipped-binary/runtime latency pass; the measured clean-build and compiler-archive costs are explained in the archived report. |

From 2026-08-26 through 2026-08-27, the runtime-performance campaign was the
previous exclusive compiler gate. `O-PERF-00` through `O-PERF-05` and
`O-GAME-01` closed in order. The completed rows and their exact evidence remain
here as the baseline for later measured optimizer work. The later structural
gate is also complete; future rows follow their declared prerequisites and the
permanent executable-authority rule below.

While an exclusive row is active, the only exceptions are correctness or
security fixes, release/CI unblockers, and changes explicitly named as its
prerequisites. An exception must remain narrowly scoped, preserve the campaign
baseline, and state why it cannot wait. Documentation and editor-only work may
continue when it does not change compiler semantics or consume the active
compiler ownership boundary. An exclusive gate never permits unsafe shortcuts,
benchmark special cases, weaker failure semantics, or skipped verification.

During an exclusive campaign, complete one row before beginning the next. A
failed upper-bound proxy can complete a row only when its raw evidence
identifies the next bottleneck or proves the proposed family has no material
value. Update this table and the detailed row together; do not copy the
sequence into another plan.

| Order | ID | State | Prerequisite | Phase outcome | Exit evidence |
| --- | --- | --- | --- | --- | --- |
| 1 | `O-PERF-00` | **complete** | current benchmark runner and reconciled audits | Freeze reproducible attribution workloads and separate GTI optimization effects from native optimization effects. | Exact clean commit `a2bd29dc` records conclusive checked-array, checked-vector, scalar-chain, scalar-FP, control, game-update, and managed-thread baselines; strict generated arithmetic ceilings; and fixed-native O0/O1/O2/O3 attribution. |
| 2 | `O-PERF-01` | **complete** | `O-PERF-00` | Prove bounded loop-carried arithmetic safe with the existing MIR safety/replay architecture. | Exact clean commit `f1a4075f` discharges all five hot arithmetic outcomes and the three hot fixed-array bounds outcomes; GTI measured 1.011x idiomatic C++ at O2 and 0.999x at O3 with no control regression above 0.4%. |
| 3 | `O-PERF-02` | **complete** | `O-PERF-01` | Carry exact range/container facts through one closed static-call family only as required by the checked-vector client. | Exact clean commit `d6374eb9` uses bounded contextual summaries to discharge six overflow outcomes and four arithmetic-domain outcomes; the O2 vector median fell from 1,213.192 ms to 558.336 ms while all conservative near misses and independent replay passed. |
| 4 | `O-PERF-03` | **complete** | `O-PERF-02` | Establish one logical vector length authority and remove only proved redundant prefix-storage checks. | Exact clean commit `eaaea34d` removes vector's mirrored count and discharges seven public-accessor private prefix rechecks. Follow-up exact commit `4dbb50bd` carries the existing scalar envelope through exact indexed return borrows and finite closed-call loop bounds; vector medians are 1.042x idiomatic at O2 and 1.049x at O3. |
| 5 | `O-PERF-04` | **measured defer** | `O-PERF-03` plus a genuinely opaque checked-loop client | Select or reject one exact checked-loop fast path for operations that static analysis cannot discharge. | Exact clean commit `4dbb50bd` records a semantics-preserving source preflight at 538.225 ms versus 540.055 ms checked GTI, only 0.34% available value; checked GTI is already 1.003x idiomatic, so the required 30% entry gate rejects versioning. |
| 6 | `O-PERF-05` | **complete (measured no-op)** | `O-PERF-04` | Remove one measured residual backend/ABI representation cost after safety side exits are addressed. | Exact clean commit `9da44477` measures scalar GTI at 1.005x semantic C++ and the closed-call vector client at 0.109x semantic and 1.044x idiomatic; native hot loops contain no surviving call/failure boundary, so no backend change is justified. |
| 7 | `O-GAME-01` | **complete** | `O-PERF-00` through `O-PERF-05` | Prove the combined result in a deterministic game-style update workload. | Exact clean implementation commit `c5bbfaa8` records 58.503 ms GTI versus 208.126 ms semantic C++ and 58.454 ms idiomatic C++, identical complete-state output, 53 replayed safety discharges, stable generated artifacts, bounded compile/memory controls, and conclusive fixed-array, vector, scalar, and managed-thread controls. |

`P-MEASURE-01` benchmark breadth was consumed by `O-PERF-00`. The end of that
campaign made `A-CACHE-01` and other unfinished rows logically eligible under
their own prerequisites. The later `R-STRUCT` gate then controlled scheduling
until it completed on 2026-08-28. Completed performance and structural rows
remain historical prerequisites and do not occupy the queue.

`M-BACK-02` no longer blocks those outcome rows. After `R-STRUCT-05`, the
permanent executable-authority gate remains: any new executable feature must
extend verified MIR, its verifier, and the general backend preflight; it may
not add an AST/semantic/HIR-only C++ body case.

## Phase D: Cross-Cutting Language Decisions

These rows intentionally produce specifications or decisions before compiler
features. A design row must not smuggle in syntax or a runtime ABI merely to
make the proposal feel concrete.

### D-LANG-01: Restriction Ledger

- **State/role:** done; readiness roles recorded in the maintained
  [`language-alignment.md`](language-alignment.md) restriction ledger.
- **Prerequisites:** none.
- **Scope:** Expand [`language-alignment.md`](language-alignment.md) into a
  maintained ledger. For each current restriction, record whether it is a
  permanent safety/simplicity rule, an unimplemented proof, an unimplemented
  lowering, a library omission, or an undecided language choice. Record the
  readiness role, user-facing client, and the plan row that owns any change.
- **Required coverage:** at least the complete gap list in the external
  language audit, all explicit specification gaps in `docs/language/`, and all
  restrictions that were then justified by the transitional C++ backend.
- **Non-goals:** changing current language semantics or choosing syntax merely
  to complete the classification.
- **Exit gate:** no restriction says merely “deferred”; it names why, the
  current client/role, and what evidence permits reconsideration.
- **Completion evidence:** the ledger covers every external language-audit
  finding, every original alignment area, every explicit language-specification
  gap, and the architecture audit's backend-visible language gaps. Each entry
  has one class, readiness role, user-facing client, owner, and reconsideration
  evidence. ADR 012 supersedes the former version split and makes bounded
  concurrency, native records/callbacks, public allocator capability, payload
  sums, propagation, domain operators, and one associative container part of
  systems readiness.
- **Unlocks:** informed systems-readiness priorities and all other design rows.

### D-MEM-01: Concurrency And Memory-Model Proposal

- **State/role:** done; design-first proposal completed in
  [`concurrency-memory-model.md`](concurrency-memory-model.md). D-MEM-02 later
  adopted it in ADR 008.
- **Prerequisites:** current ownership/execution contracts; completed
  `D-LANG-01` records classifications that agree with this proposal.
- **Scope:** Create a focused plan under `docs/plans/`, not an ADR and not code.
  It must propose answers for:
  - whether safe GTI makes data races unrepresentable/ill-formed and what
    remains undefined inside `unsafe` raw-pointer code;
  - structural transfer and sharing capabilities for primitives, aggregates,
    classes, interfaces, `unique_ptr`, future shared owners, references,
    borrowed-state carriers, raw pointers, globals, and captured values;
  - how cleanup-owning or native-resource types opt out of structural transfer,
    whether user opt-in is permitted, and which opt-in requires an explicit
    unsafe proof because thread affinity is not derivable from fields alone;
  - whether any borrow can cross a thread boundary in the first model, and the
    additional proof required for scoped borrows later;
  - how `mut`, mutable static storage, interior mutability, and synchronized
    access interact;
  - the atomic value domains, initial sequential-consistency baseline, future
    memory-order vocabulary, and happens-before obligations;
  - thread creation, join, detach, thread-local state, initialization,
    destruction, failure, and process termination;
  - synchronization operations and conservative MIR/optimizer effects;
  - native/FFI threads entering GTI, runtime callbacks, and raw-pointer proof
    obligations; and
  - a staged semantic, HIR, MIR, backend, runtime, stdlib, diagnostic, and test
    matrix.
- **Non-goals:** choosing final source spelling where a semantic contract is
  sufficient, implementing `pthread`, adding `std::thread`, or borrowing C++'s
  memory model by reference.
- **Exit gate:** every question above has one recommended answer; genuine
  alternatives are isolated with consequences and a requested decision.
- **Completion evidence:** the focused proposal defined safe data-race
  freedom, unsafe race obligations, transfer/share derivation and nominal
  policy, first-model borrow/global/atomic/thread boundaries, FFI entry,
  synchronization effects, and the staged implementation/test matrix. It
  isolated executable horizon, automatic join, worker failure, and final
  spelling; D-MEM-02 has resolved those choices.
- **Unlocked:** completed `D-MEM-02`; neither design row unlocks concurrency
  code by itself.

### D-MEM-02: Adopt The Memory-Model Boundary

- **State/role:** done; durable decision adopted in
  [ADR 008](../decisions/008-safe-concurrency-memory-model.md).
- **Prerequisites:** `D-MEM-01`, `D-LANG-01`, and `D-FAIL-01`, all complete.
- **Scope:** Resolve the remaining choices in an ADR and update the current
  execution and ownership specifications with the accepted single-threaded and
  concurrent boundary. Adopt the restriction ledger's decision that
  transfer/share facts and concurrent-global policy precede public execution.
  ADR 012 now assigns the bounded public thread/atomic/mutex profile to the
  systems-readiness work-queue lane.
- **Required invariant:** the accepted rule must compose with the existing
  borrow model rather than adding a parallel runtime-only type-trait system.
- **Exit gate:** the ADR, language docs, roadmap horizon, and restriction ledger
  agree; compile-fail examples are specified for values that cannot cross or
  be shared across a thread boundary.
- **Completion evidence:** ADR 008 and canonical execution/ownership semantics
  adopt explicit single-threaded/concurrent profiles, safe data-race freedom,
  structural transfer/share with safe negative and unsafe positive nominal
  policy, owned-only crossing, concurrent globals, SC first atomics, automatic
  join, contained worker failure, native-entry obligations, and the
  bounded-first executable scope. The D-MEM-01 review matrix names
  the required positive and negative implementation cases without inventing
  source spelling before C-TYPE-01.
- **Unlocks:** `C-TYPE-01` design and the concurrency implementation lane.

### D-EXEC-01: Evaluation Order And Full Expressions

- **State/role:** done; prerequisite `D-LANG-01` is done; durable decision
  adopted in [ADR 010](../decisions/010-deterministic-evaluation-and-full-expressions.md)
  and [Execution Section 4.2](../language/execution.md#42-evaluation-order).
- **Prerequisites:** `D-LANG-01`.
- **Scope:** Specify evaluation order for call receivers, arguments, operators,
  assignment targets, initializers, conditional/short-circuit expressions,
  cross-source module/static initialization, temporary destruction, and
  cleanup. State when a full expression ends and when transient loans end.
  Choose a rule GTI can lower deterministically on every supported C++ backend.
- **Non-goals:** implementing emitter hoisting in the same change. Earlier IIFE
  and statement-hoisting sketches are unsound without explicit temporary
  lifetimes and remain rejected as semantic authority.
- **Exit gate:** canonical examples have one required order and destruction
  trace; the plan identifies the exact HIR/MIR facts and the backend migration
  slice needed to realize them.
- **Completion evidence:** strict left-to-right receiver, argument, operand,
  capture, pack, and initializer order; target-first one-time assignment-place
  evaluation; direct destination materialization; LIFO full-expression
  obligations; reverse successful partial cleanup; return publication; ordered
  hosted count/argument setup; and a lexical dependency-first program-
  initialization walk have required traces.
  The decision assigns source boundaries and the program-init plan to
  semantics; `FullExpressionId` and concrete operand roles to HIR;
  temporary/drop obligations to M-LIFE; ordered CFG and invalidation to M-EXEC;
  and production emission to M-BACK. Conservative semantic restrictions remain
  explicit verifier/lowering gaps; the production compatibility emitter no
  longer exists.
- **Unlocked:** `D-COMPAT-01`, `M-OWN-02`, and `M-LIFE-01` are complete, and
  the bounded `M-EXEC-01` ordinary-call and ordinary-constructor input
  schedules are implemented. The
  conservative
  both-argument overlap restriction may be removed per operation family only
  after ordered MIR and its matching production backend migration are
  authoritative.

### D-FAIL-01: Defined Failure And Embedding Contract

- **State/role:** done; prerequisite `D-LANG-01` is done; durable decision
  specified in
  [Execution §4.10](../language/execution.md#410-defined-runtime-failure),
  with rationale in [ADR 007](../decisions/007-defined-runtime-failure.md).
- **Prerequisites:** `D-LANG-01`.
- **Scope:** Decide the stable failure category, source-location token, report
  contract, exit status, cleanup performed, panic/termination hook, behavior
  when embedded, and boundary between terminating checks and APIs returning
  `expected`. Cover allocation failure and future thread failure explicitly.
- **Non-goals:** source exceptions, catch/resume syntax, native exception-ABI
  unwinding, or making programmer contract violations recoverable through
  ordinary GTI source. Compiler-managed non-resumable failure propagation is
  required to reach cleanup-preserving containment boundaries.
- **Exit gate:** all existing checked-integer, conversion, indexing, owner,
  private-storage, and allocation failures map to the contract; the runtime can
  later expose one narrow entry point without erasing categories.
- **Completion evidence:** stable `GTI-R0001` through `GTI-R0014` categories,
  artifact-qualified source tokens, status 70 and exact report framing,
  deterministic cleanup and double-failure behavior, observer constraints,
  program/embedding/task/callback boundaries, expected-versus-terminating
  policy, and every current trap mapping are canonical. Current emitter aborts
  remain an explicit M-FAIL-01/Q-FAIL-01 implementation gap.
- **Unlocked:** D-MEM-02's worker-policy adoption is complete. `M-FAIL-01`,
  public allocation design, hosted concurrency, and a future generated
  embedding boundary remain downstream clients.

### D-CALL-01: Callable Ownership And Escape Contract

- **State/role:** done; prerequisite `D-LANG-01` is done; design-first decision
  recorded in
  [`callable-ownership-and-escape.md`](callable-ownership-and-escape.md).
- **Prerequisites:** `D-LANG-01`.
- **Scope:** Define one GTI-owned callable model covering exact parameter and
  return shape, invocation-count capability, capture ownership, movement,
  destruction, escape, recursive/self-reference restrictions, and generic
  identity. Isolate the smaller contracts needed by range algorithms, consumed
  thread tasks, and native callbacks so those features reuse one representation
  without waiting for every ergonomic callable feature.
- **Non-goals:** choosing all capture syntax, cloning `std::function`, adding
  threads or callbacks, or using a C++ closure type as GTI identity.
- **Exit gate:** algorithm, owned-task, and native-callback examples map to one
  representation and capability vocabulary; unsupported borrowed escape and
  invocation-count cases have an explicit diagnostic contract.
- **Completion evidence:** one concrete lexical/class/function-item identity
  model, exact signatures, read/mut/once invocation capabilities, copy/move
  capture and lifecycle rules, confined versus exact owned transport, generic
  identity, recursion exclusions, cross-phase records, diagnostics, and test
  gates now serve all three clients without choosing final syntax or changing
  current lambda behavior.
- **Unlocks:** `L-CALL-01`, `C-CALL-01`, and `S-CALL-01`.

### D-COMPAT-01: Compatibility And Edition Policy

- **State/role:** done; independent release-policy decision adopted in
  [ADR 011](../decisions/011-language-compatibility-and-editions.md).
- **Prerequisites:** `D-LANG-01`, `D-EXEC-01`, `D-FAIL-01`, and `D-MEM-02`,
  all complete.
- **Scope:** Define how a 1.x compiler preserves old source meaning and how a
  future incompatible memory-model, evaluation, or ownership change would be
  selected. State that the current non-textual, direct-visibility `#include`
  spelling remains supported and how a future edition could introduce a module
  vocabulary without changing old source. Do not add an `edition` manifest key
  until the compiler actually implements and enforces it.
- **Exit gate:** release and deprecation policy is public, testable, and does
  not permit silently ignoring an unknown compatibility selector.
- **Completion evidence:** ADR 011 and Scope Section 1.6 define SemVer behavior
  before and after 1.0, Edition 1's protected meaning, correction and urgent
  safety rules, package/source-graph selection, a permanent Edition 1 default,
  hard failure for unknown selectors, deprecation/removal windows, and the
  stable non-textual direct-visibility `#include` contract. The future
  manifest/direct selectors remain rejected until one coherent implementation
  row can enforce and publish them.
- **Unlocks:** `Q-DEPRECATION-01` after `T-LSP-01`. A second edition requires a
  new bounded selector row; it cannot be introduced as an incidental manifest
  or parser change.

## Phase M: Places, Lifecycles, And Executable MIR Authority

This is the principal implementation critical path. The work should land in
small complete operation families, not as one replacement of semantic
analysis, HIR, MIR, and the backend.

### M-OWN-01: Ownership-State And Place Authority Decision

- **State/role:** done; design-first decision recorded in
  [`place-and-ownership-state.md`](place-and-ownership-state.md).
- **Prerequisites:** existing stable root/field/dereference loan places and MIR
  dominance.
- **Scope:** Define one GTI-owned `PlaceKey`/overlap contract for roots,
  fields, constant fixed-array indices, dynamic indices, dereferences, raw
  addresses, and opaque call results. Decide which flow facts semantics must
  still choose, which facts HIR carries, and which fixed-point properties MIR
  verifies or computes. Preserve ADR 001 explicitly.
- **Non-goals:** implementing every projection, moving semantic validity into
  the optimizer, or introducing a generic solver before naming its first
  client.
- **Exit gate:** examples for equal, prefix, disjoint, and may-alias places have
  one phase owner and one conservative answer; snapshot/body identity and
  invalidation rules are explicit.
- **Completion evidence:** one `PlaceDomain`/value-owned `PlaceKey` contract
  covers program/body/formal/receiver/temporary/materialized/raw/opaque roots
  and named-field, constant/dynamic-index, checked, raw, and opaque
  projections. The exhaustive relation is equal, either strict-prefix
  direction, disjoint, or may-alias. A finite uninitialized/available/moved
  state-set transfer assigns source validity and diagnostics to semantics,
  concrete keys/events to HIR, and reachable CFG fixed-point verification to
  MIR. The example matrix and invalidation table define snapshot, concrete
  instance, lifetime-epoch, projection dependency, and edit boundaries.
- **Unlocks:** `M-OWN-02` and `M-LIFE-01`, which are now complete; bounded range
  and allocator design subsequently became ready, and `S-ALLOC-01/02` are now
  complete.

### M-OWN-02: Indexed Places And Definite Initialization

- **State/role:** done; prerequisite `M-OWN-01` is done; bounded-first
  implementation.
- **Scope:** Add directly owned fixed-array constant-index places first. Track
  available, moved, and restored state across every reachable branch, loop
  backedge, return, and drop. Dynamic indexes remain may-alias until a separate
  proof exists. Extend in prompt-sized families: local arrays, fields containing
  arrays, then supported checked-owner projections.
- **Non-goals:** vector slot extraction, raw-pointer partial moves, or a general
  allocation model.
- **Focused tests:** move one element, reject whole-owner use while partial,
  restore before whole-owner use, preserve exact partial state at a drop
  boundary, disjoint constant elements, dynamic-index conservatism, joins and
  loops, HIR/MIR place identity, forged verifier cases.
- **Exit gate:** semantics and MIR agree on availability at every exit; no
  backend C++ behavior repairs an invalid state.
- **Completion evidence:** one shared `PlaceKey`/relation/state/event vocabulary
  now covers directly owned local arrays and fields containing fixed arrays.
  In-range constant elements move and restore independently; dynamic indices
  remain may-alias. Semantic branch/loop state, concrete HIR domains/events,
  and the reachable MIR fixed point agree. Focused tests cover whole-owner use
  while partial, disjoint elements, branch and loop joins, partial drop
  boundaries, distinct frontend snapshots/incompatible domains, forged event
  identity, missing restoration, use-before-initialization, and double
  initialization; CLI execution passes at O0/O3 and in the supported C++20
  compatibility mode.
- **Unlocks:** `M-LIFE-01`, payload enums, richer storage, and precise range
  elements.

### M-LIFE-01: Explicit Temporary And Drop Obligations

- **State/role:** done; `M-OWN-02` and `D-EXEC-01` are done; systems-readiness
  prerequisite shared by every accepted outcome lane.
- **Scope:** Give lexical storage, MIR temporary places, and owning SSA results
  explicit typed drop obligations. Model ownership transfer, moved-from
  structural destruction, reverse construction order, full-expression cleanup,
  path-conditional initialization, and normal failure-free exits. Retain exact
  destructor identity and active-drop requirements in MIR. Reject namespace
  globals and static fields whose concrete type requires active cleanup,
  including enclosing aggregates and generic instances, because GTI has no
  global/static shutdown authority.
- **Non-goals:** source/native exceptions, failure propagation, partial
  constructor rollback, global/static shutdown, custom move bodies, or
  aggregate drop trees beyond the supported place slice. M-FAIL-01 consumes
  this failure-free drop authority and adds the supported failure edges,
  rollback, and boundary cleanup required by Execution §4.10.
- **Focused tests:** discarded owning call result, nested temporary arguments,
  conditional temporary, short-circuit arms, return transfer, break/continue,
  reverse order, declared-cleanup global/static/aggregate rejection,
  double/missing drop mutations, and join-state mismatch.
- **Exit gate:** a MIR dataflow/verifier proves each supported obligation is
  initialized, transferred, and structurally dropped exactly once on every
  normal edge; cleanup-owning global/static storage is rejected before backend
  entry; runtime behavior matches at O0/O3.
- **Completion evidence:** Semantics now selects AST full-expression roots. HIR
  maps them to body-local identities and exact lexical/value drop descriptors.
  MIR maps those obligations to concrete places and records initialize, move,
  reparent, replace, transfer-out, and drop events; its fixed point rejects
  inactive sources, double/missing or out-of-order cleanup, forged boundary or
  destructor metadata, non-consuming transfer operands, and invalid predecessor
  conditionality. Reached full expressions retain ordered obligation membership
  and one verified cleanup boundary. Logical and conditional branch-local
  temporaries retain path-conditional obligations through their merge and
  clean up at the enclosing full-expression boundary; return,
  break/continue, nested call temporaries, and reverse lexical cleanup have
  focused coverage. Semantics recursively rejects aliases, arrays, bases,
  fields, captures, and concrete generic shapes requiring global/static active
  cleanup with `GTI-S2061`. Compiler tests and default-mode O0/O3 plus C++20
  compatibility CLI traces cover the supported normal-exit slice without
  treating native call argument order as authoritative.
- **Unlocks:** `M-EXEC-01`, allocators, payload enums, owned callables, and
  temporary ranges. It supplies one lifecycle prerequisite for scoped threads;
  `M-OWN-03` and a scope-join loan proof are still required.

### M-OWN-03: Stored And Escaping Mutable Dependencies

- **State/role:** ready; prerequisites `M-OWN-02` and `M-LIFE-01` are done;
  bounded-first systems-readiness work for mutex guards and mutable views.
- **Scope:** Extend the existing local parent/child loan graph to one stored or
  escaping mutable dependency with an exact stable origin. Define creation,
  parent suspension, movement, assignment, call/return propagation, path joins,
  child-first cleanup, and reactivation in semantics, HIR, and MIR. Start with
  one origin and one carrier shape before considering nesting or multiple
  owners.
- **Non-goals:** raw or opaque origins, dynamic-index precision, multi-origin
  carriers, arbitrary lifetime parameters, mutex syntax, or treating every
  stored reference as safe merely because the backend can represent it.
- **Focused tests:** stored guard-shaped carrier, returned mutable carrier,
  move and reassignment, early return, conditional creation, parent access
  while live, exact cleanup on every exit, and forged parent/escape graphs.
- **Exit gate:** every supported stored or escaping child has one authoritative
  origin and exactly one path-sensitive end; MIR rejects forged ancestry,
  premature parent access, double end, and escaping unsupported shapes.
- **Unlocks:** `C-SYNC-01`, scoped-thread guards, and richer mutable views.

### M-FAIL-01: Failure Operations And Cleanup Edges

- **State/role:** active for broader failure semantics; `D-FAIL-01`,
  `I-CAP-01`, `M-LIFE-01`, and the source-body backend cutover are done. Exact
  origin/site metadata, propagation and cleanup edges, hosted containment, and
  the versioned runtime record/report/firewall substrate are implemented for
  all currently admitted source forms. Double failure and newly introduced
  operation/result families still require explicit MIR contracts.
- **Scope:** Represent exact local categories/details and canonical frontend
  source anchors in HIR, then assign deterministic artifact-local site IDs for
  MIR through the failure-metadata builder. Represent call-like propagation
  separately so it preserves an existing record without a transitive origin
  set or caller re-siting. Add explicit failure
  successors across direct, virtual, constructor, and expected-observer calls;
  represent cleanup of supported initialized and partially initialized
  invocation-owned shapes and termination at the hosted-program boundary; and
  add reusable fixed-record, observer, reporting, arbitration, and firewall
  primitives for later backend/task/callback/embedding rows. Assemble the
  immutable artifact descriptor before backend entry from frontend source facts
  plus the pre-optimization site table. Extend the frontend/driver/backend
  handoff with SourceGraph, exact source bytes, and direct/project logical-root
  facts rather than reconstructing paths in a backend. Add identity-bound
  trusted public-container bounds origins so ordinary vector/string wrappers
  select their public details without backend call-stack or spelling inference.
  Materialize the owned-entry adapter as a compiler-generated hosted-startup
  HIR/MIR operation with the fixed negative-count and checked-count-conversion
  origins plus the source `main` anchor. Preserve the exact vector/string
  constructor and append calls with their callee-owned allocation records;
  there is no adapter-local owned-argument-allocation origin.
- **Landed bounded slice:** `DefinedFailureOperation` records exact local
  outcome sets at snapshot-local source-unit/line/offset origins and records
  direct, virtual, constructor, and callable propagation separately. Semantics
  classifies the current arithmetic, conversion, bounds, owner, expected,
  storage, allocation, call, construction, and resolved-operator families; HIR
  and MIR retain the identity without absolute paths. MIR verification rejects
  forged vocabulary, origins, duplicates, placement, target, or propagation,
  and optimizer effects preserve every such operation as an observable trap.
  The post-HIR metadata builder now derives logical names, deterministic
  external-unit route identities, definition-site coalescing, one-based
  `FailureSiteId` assignments, canonical descriptor bytes, and the SHA-256
  artifact identity before optimization. MIR retains and verifies exact local
  detector sites while propagation remains un-sited. MIR v17 additionally
  gives eligible full-expression-root scalar operations in function and lambda
  bodies one `Invoke` with distinct normal/failure successors, a body-local
  fixed-record failure parameter, active-loan ending, reverse-construction
  temporary/lexical cleanup, and an exact `PropagateFailure` endpoint. The
  verifier rejects normal predecessors, missing/forged invokes, record rewrite,
  failure-result use, misplaced cleanup, and cleanup-order drift. MIR v18 adds
  caller-owned class-value parameter stages to ordinary calls and initializes
  one eligible cleanup-owning call result only on the invoke success edge; the
  failure edge neither owns nor drops that unconstructed result. An exact local
  scalar `Compute` or `Load` used as a later indexed ordinary-call argument now
  receives an `Invoke` after any earlier class-value stages: failure destroys
  each stage once in reverse construction order before propagation, while only
  the final normal-path call transfers them. The verifier derives eligibility
  from exact value/input identity, dominance, and prepared obligations and
  rejects detached arguments or omitted stage cleanup. An ownership-free static
  direct call used as that later argument now receives the same bounded edge:
  it remains un-sited, forwards its existing record unchanged, unwinds the
  earlier outer stages, and feeds its result into exactly one normal-path
  `CallInput`. A nested call with its own owning parameter remains excluded.
  The co-delivered runtime substrate now fixes the C ABI for ordinary records,
  per-site allowed outcomes, immutable artifact descriptors, and the emergency
  envelope. It provides allocation-free Unicode-15.1 report formatting,
  partial/`EINTR`/broken-pipe-safe report I/O, observer firewalling over the
  original record, one process-terminal winner, and immediate status 70. The
  all-zero
  artifact/site runtime sentinel requires no descriptor; every nonzero record
  requires exact artifact, site, and outcome membership.
  General compound argument evaluation, assignment destinations, borrowed and
  owning results, constructors, partial rollback, hosted origins, generated
  containment, descriptor emission, and executable backend use have since
  landed through the general MIR route. Double failure and any operation shape
  not represented by current MIR remain open.
- **Exit gate:** every current failure family has exact semantic/HIR
  local-origin and source-anchor snapshots plus MIR outcome/site snapshots;
  artifact descriptor/digest tests are stable across optimization levels;
  verifier mutations reject forged outcomes, sites, cleanup, and boundaries;
  allocation-free C/runtime harnesses exercise record handoff and arbitration.
  Its co-delivered Q-FAIL-01 slice owns exact
  hosted report/status/observer and report-I/O tests. End-to-end source
  execution and C++20/C++23 parity are covered by the completed `M-BACK-02`
  route and its focused runtime matrices.

### M-EXEC-01: Ordered Expression And Call Lowering

- **State/role:** in progress; `M-LIFE-01` and `D-EXEC-01` are done. Concrete
  non-intrinsic ordinary calls, concrete ordinary constructors, and concretely
  resolved class `operator()` calls with scalar/reference parameters and
  eligible non-borrowed class-value parameters now retain exact HIR input roles
  and verified MIR receiver/argument/invocation order. Direct self-contained
  class-value construction/call results now also have verified read/mutable
  temporary receiver materialization and full-expression cleanup; the
  remaining families are systems-readiness implementation.
- **Scope:** Decompose one complete expression family into ordered MIR values
  and temporaries, including receivers, arguments, transient loans, and cleanup.
  Remaining work includes operators and one-time assignment places, compound
  expressions, and wider partial-construction/failure schedules. The semantic
  `ProgramInitializationPlan`, `HostedProgramEntryPlan`, closed later-storage
  effect proof, HIR plan copies, exact constant-substitution lowering, merged
  module MIR body, and production verification are landed. New slices extend
  those authorities rather than reconstructing source order or startup targets.
  Add structural verifier mutations for ordering, materialization,
  full-expression boundaries, and cleanup.
- **Landed bounded slices:** Eligible ordinary calls retain one HIR receiver
  and source-ordered arguments; eligible ordinary constructors retain no
  receiver, one exact constructor target, and at least one source-ordered
  argument. A concretely selected class `operator()` retains an exact read or
  mutable receiver borrow, reusable value receiver, or consuming `MoveValue`
  receiver for an explicit move or trailing-`&&` target, including once-callable
  fallback to an exact read/mutable overload. These use exact selected
  parameter types and value/class-copy/class-move/read-borrow/mutable-borrow
  roles. Direct ordinary constructor and by-value GTI call results of concrete
  self-contained class/struct type may additionally use exact read/mutable
  temporary-borrow receiver roles for non-virtual ordinary methods and reusable
  `operator()`, with one producer-matched mutable place and full-expression
  drop obligation. MIR emits
  one-use `CallInput` checkpoints and verifies call-site, role, index, type,
  dominance, and strict receiver-when-applicable, arguments, then `Call` or
  `Construct` order. For ordinary calls, each eligible class copy or move
  materializes one distinct caller-owned prepared-parameter obligation. Copy
  initializes the stage, move reparents its exact active source obligation when
  present, and the final call transfers every stage exactly once as the callee
  begins. Constructors retain direct checkpoint transfer until their partial-
  construction model lands. Mutation tests cover wrong sites,
  duplicate/abandoned and bypassed inputs, type drift, reordering, forged
  receiver/copy/move modes, missing or misplaced transfer, and erased target
  identity. Generated/default zero-argument and copy/move special construction,
  borrowed-state class values, packs, unresolved callables, operators other
  than concrete `operator()`, failure rollback, backend emission, and semantic
  borrow relaxation remain out of these slices.
- **Landed program-authority slice:** Semantics preserves ordered configured
  prelude roots and lexical dependency-first/source declaration order in one
  exact data-only/executable-step plan. `GTI-S2068` rejects a later-storage
  access or incomplete transitive effect closure, while exact representable
  value substitutions are retained through HIR and lowered to verified MIR
  constants rather than early loads. HIR exact-compares the semantic/HIR
  analysis seal, initialization/hosted plans, module inventory, and
  provenance. The generated hosted startup executes the merged verified
  `Module/0` body inside the containment boundary; generic static-storage
  scheduling remains a separate open contract.
- **Implemented bounded follow-on (`M-TEMP-ARG-01`; GTI 0.297.0):** A
  direct fresh, self-contained class/struct result may initialize one exact
  read-only `T&` call parameter through a full-expression-owned ephemeral
  loan. Existing exact candidates requiring no ephemeral borrow form the first
  viability tier; the new borrowed candidates are considered only when that
  tier is empty, so an exact owning overload remains preferred without C++
  conversion ranking. Semantics proves freshness and non-escape, HIR retains a
  dedicated argument role, MIR schedules producer/place/`BorrowRead`/call/loan
  end/drop, and production consumes that verified schedule. Mutable borrows,
  local or stored references, explicit moves, borrowed-state or escaping
  results, compound producers, and native calls remain excluded. The detailed
  contract and failure/runtime matrix are in
  [`mutable-temporary-receivers.md`](archive/mutable-temporary-receivers.md#implemented-follow-on-one-call-read-only-temporary-arguments).
- **Non-goals:** broad AST emitter rewrite, production body emission, or an
  IIFE workaround. M-BACK owns production C++ consumption of this schedule.
- **Exit gate:** deterministic HIR/MIR snapshots and verifier mutations prove
  effectful source order, one target evaluation, invocation after parameter
  setup, and balanced full-expression obligations for the selected family.
  Runtime O0/O3/native-compiler traces and compatibility-path removal belong to
  the matching M-BACK closed-body migration. The conservative semantic
  restriction is removed only when that production family is authoritative.
- **Unlocks:** each completed family may enter M-BACK immediately; completion
  of the whole row is not a global prerequisite. Production migration then
  unlocks more precise borrow acceptance, hosted threads, and optimizer control
  of those expressions.

### M-BACK-01: First MIR-Emitted Body Family

- **State/role:** done in 0.143.0. `scalar-leaf-v1` is the deliberately
  failure-free fixed-width-integer leaf-function family.
- **Prerequisite:** the selected family's control flow, values, places,
  lifecycles, and terminators are complete and verified in MIR. Completion of
  the entire `M-EXEC-01` row is not required.
- **Conditional prerequisite:** if the selected family can take a checked
  failure edge, the applicable `M-FAIL-01` and containment slices must also be
  done. The selected first family deliberately avoids that dependency.
- **Implemented scope:** `CppBackend` re-verifies its exact optimized MIR
  snapshot and emits eligible non-entry, non-generic free functions with
  fixed-width-integer parameters/results (plus a no-op `void` result),
  parameter loads, source or verified identity-fold literals, trivial
  full-expression boundaries, and one return. Every ineligible body remains
  wholly on the explicit compatibility path.
- **Exit evidence:** `mir_backend_first_family` and
  `mir_backend_first_family_runtime` prove a single MIR executable authority,
  optimized-instruction control, invalid/stale/forged-snapshot rejection, and
  O0/O1/O3 execution at C++20/C++23 while near-miss bodies remain compatible.
- **Unlocks:** executable use of matching `O-MIR-01` transforms, `O-MIR-03`,
  and the immediate consolidated `M-BACK-02` migration campaign.

### M-BACK-02: Complete MIR Body-Family Migration

- **State/role:** complete for source executable bodies. Every identity in
  `tests/mir_census_baseline.json` uses the verified-MIR production route.
  The named families below are historical migration stages, not current
  selectors or fallback authorities.
- **Completed scalar-CFG scope:** `scalar-cfg-v1` admits fixed-width integers,
  `bool`, and `char` in parameters and results, plus `void` results and
  unprojected scalar binding and temporary places; its verified literal,
  identity, logical-not, bitwise,
  equality, and ordering computations; loads, initialization, plain
  assignment, trivial boundaries; and
  `Branch`/`Goto`/`Switch`/`Return`/`Unreachable` CFG including loop backedges.
  Calls, checked failure, loans, drops, cleanup, construction, projections,
  and non-scalar ownership remain excluded.
- **Scalar-CFG exit evidence:** `mir_backend_scalar_cfg` and
  `mir_backend_scalar_cfg_runtime` prove exact whole-family selection, direct
  emitted-CFG control by verified MIR, compatibility near misses, optimized
  literal control, and O0/O1/O3 execution at C++20/C++23.
- **Completed scalar-direct-call scope:** `scalar-direct-call-v1` admits exact
  ordered scalar `CallInput`/`Call` stages across one closed acyclic static
  source-function graph. Each graph node independently satisfies the production
  declaration and scalar-CFG policy, every target remains coherent across
  semantics/HIR/MIR, and each call is normalized to ordinary continuation only
  under MIR v20's proved-false defined-failure summary. The summary is broader
  than production eligibility: a concrete internal, constrained, or
  `constexpr` body may validly summarize false while remaining compatible. A
  conservative true summary on an independently eligible graph rejects
  production emission as noncanonical; header-, body-, dispatch-, and
  cycle-ineligible graphs remain compatible. Once selected, later graph,
  identity, provenance, or instruction drift also rejects emission.
- **Scalar-direct-call exit evidence:** `mir_backend_scalar_direct_call` and
  `mir_backend_scalar_direct_call_runtime` prove whole-graph selection,
  exact target and ordered-input coherence, summary/provenance fail-closed
  mutations, cross-namespace and heterogeneous scalar calls, void and zero-arg
  calls, branch/loop/nested calls, IdentityFold emission, HIR-`for` and other
  near misses, and O0/O1/O3 execution at C++20/C++23.
- **Completed class-default-cleanup scope:** `class-default-cleanup-v1` admits
  non-entry, non-generic, zero-parameter source free functions with a non-void
  scalar result and one straight-line root scope. Each local is an exact empty,
  base-free, field-free, non-polymorphic concrete class created by
  generated-default `{}` construction with no constructor target. The return
  global is loaded before reverse lexical cleanup. Its exact public source
  destructors contain bounded scalar-literal assignments to mutable top-level
  globals and are proved failure-free by MIR v21. Production C++ uses strict
  raw-storage lifetime slots with explicit construction and destruction.
  Declared constructors, fields or bases, nested scopes, branches, calls,
  loans, and failure-capable destructors remain compatible.
- **Class-default-cleanup exit evidence:**
  `mir_backend_class_default_cleanup` and
  `mir_backend_class_default_cleanup_runtime` prove exact function/destructor
  selection, construction/reparent/drop/cleanup ordering, strict lifetime-slot
  representation, conservative and forged summary handling, HIR/MIR drift
  rejection, return-before-cleanup and reverse-destruction behavior, and
  O0/O1/O3 execution at C++20/C++23.
- **Completed owned-lifecycle scope:** `owned-lifecycle-call-v1` admits one
  atomic acyclic graph of eligible failure-free non-entry free functions plus
  exact concrete, base-free, non-polymorphic passive-scalar-field classes. Each
  class has one exact scalar constructor and destructor and no custom copy/move
  policy. Production emission consumes verified constructor initializer,
  construction, move, call-input, call, transfer, lexical cleanup, and drop
  stages through explicit lifetime slots; exact failure-free reverse callers
  join the same graph. Its bounded statement grammar covers blocks, scalar and
  owned variables, expressions, `if`, and `return`; comma, logical/conditional
  sub-CFG, loops, and switches remain compatible. Checked lifecycle bodies and
  all other near misses also stay wholly compatible.
- **Owned-lifecycle exit evidence:** `mir_backend_owned_lifecycle` rejects
  summary, initializer, same-typed-argument, destructor source/CFG, graph,
  call, transfer, and drop drift. `mir_backend_owned_lifecycle_runtime` runs the
  close-bit/first-close program at O0/O1/O3 under C++20/C++23 and proves moved
  inner cleanup precedes outer cleanup without a native double-destruction
  escape.
- **Completed hosted-failure scope:** `scalar-failure-callgraph-v1` admits one
  unique no-argument `int32_t` hosted entry plus its exact closed acyclic graph
  of `int32_t` source free functions. It reuses only exact v21 lifecycle-proof
  classes, admits checked fixed-integer add/subtract/multiply/divide/remainder,
  both shifts, negate, and dynamic conversion, and emits a private
  bool/out-result/record ABI. Local detectors create the exact artifact/site
  record, calls forward the same record pointer, MIR failure edges run exact
  drops before propagation, and only MIR Return publishes the result. Whole-
  program selection rejects incoming edges or class-representation users from
  every HIR body kind, cycles, native/virtual/normal-ABI edges, checked
  lifecycle bodies, dynamic initialization, and HIR/MIR drift. The single
  hosted boundary reports once through the runtime and exits 70; its catch-all
  immediately exits 70 without forging a GTI record.
- **Hosted-failure exit evidence:**
  `mir_backend_scalar_failure_callgraph` proves exact ABI, record forwarding,
  Return-only publication, reverse cleanup, source sites, complete reverse
  closure, C-linkage/virtual/lambda/initializer/class-use exclusions,
  value-owning array/generic containment with nonowning raw/reference retention,
  and fail-closed mutations. `mir_backend_scalar_failure_callgraph_runtime` runs
  normal and every admitted signed/unsigned detector outcome at O0/O1/O3 under
  C++20/C++23, requiring the exact report and status 70.
- **Completed scope:** Every source function, constructor, destructor, lambda,
  class initializer, module initializer, hosted-startup body, and verified
  native boundary shell is preflighted and emitted from MIR. The public no-MIR
  emitter and executable AST statement route are gone. Program initialization
  executes through its verified `Module/0` schedule and planned hosted adapter.
- **Completed backend frontier:** Generated structural, callable, lifecycle,
  native-callback, and concrete-instance families have exhaustive lowered
  contracts. C++ declaration/source assembly consumes lowered declarations,
  symbols, instances, MIR, and generated items. The transitional backend tuple
  and frontend-backed snapshot/emitter routes are deleted.
- **Historical migration ledger:** The checkpoints below explain how the
  general route was reached. Counts and compatibility references describe the
  state at each named release and are not current architecture.
- **Measured cutover position (0.150.0).** Production emits exactly one body
  from verified MIR across the 57-example corpus, a single `scalar-cfg-v1`.
  The six completed families each select correctly on their dedicated fixtures
  (3-25 bodies) but effectively never match shipped code, because each carries
  a whole-program selection contract rather than a per-body one.
  `CppMirBodyEmitter` reports 1953 of 2429 corpus bodies emitter-ready, but it
  is a fail-closed analysis gate with no text-emission step and no production
  caller, so that figure is a precondition, not emission. The
  `mir_emission_readiness` assertions inside `cpp_mir_body_emitter` now track
  it, including that every example still reaches MIR, since a rejected source
  silently inflates every readiness figure. That gate catches a
  compilation-breaking regression, which is the mode that masked two unsound
  attempts in this campaign, but it does not by itself establish semantic
  correctness: relaxing the loan exclusion in
  `requiresMirFailureControlFlow` alone raises readiness to 2006 with zero
  rejections and zero incoherent bodies, while leaving the enclosing scope to
  end a loan the failed operation never produced. Readiness movement is
  therefore necessary evidence for a cutover step, never sufficient.
- **The declaration gate is the binding constraint, and it is structural.**
  Instrumenting the real `selectedMirScalarCfg` over the corpus shows the
  first-match rejection reasons are operator 1124, member 944, generic 892,
  compiler-private 798, `constexpr` 432, non-scalar parameter type 268, entry
  114, with no function reaching the body-shape gate at all. A previous
  checkpoint recorded the opposite conclusion from an approximate model of that
  gate; the approximation wrongly excluded `Lifecycle`, which the real gate
  already admits as a pure full-expression marker. Treat the instrumented
  numbers above as authoritative.
- **Member and generic admission is not a gate relaxation.** After the
  declaration gate, the selector requires exactly one MIR function instance per
  source declaration with empty type and value arguments, and throws otherwise.
  A member of a generic class has one instance per instantiation, so admitting
  members makes the backend fail with "missing the MIR instance for an eligible
  source function". Emission is currently keyed per source declaration; the
  next increment therefore requires per-instance body emission rather than a
  wider selector.
- **Emitter capability ranked by bodies unlocked.** Of the 899 bodies outside
  the scalar-CFG set: `Lifecycle` 899, `Call` 647, `CallInput` 420,
  failure edges and checked operations 354, drops 47, `Construct` 26, loans and
  borrows 16. `Lifecycle` is required by every one of them and is already
  classified `RepresentedByMir`, so the first emission increment should cover
  `Lifecycle` plus the existing scalar-CFG set, then `Call`/`CallInput`.
- **Wiring landed (0.156.0-0.157.0).** (1) `buildCppMirBodyEmissionMapRows`
  is the production representation-row builder, reusing the extracted
  `cpp_representation` naming authorities. (2) `CppMirBodyEmitter::emitBodyText`
  is the text-emission step; the scalar-leaf, scalar-cfg, and
  scalar-direct-call families delegate their production body text to it, with
  receiver-place handling derived from MIR rather than family flags.
  (3) `CppBackend::generate` builds and owns the rows beside the program
  plan and passes them into the emitter, which no longer derives a spelling
  authority of its own. (4) Scalar-cfg admission is analysis-driven
  (0.159.0): the general emitter's fail-closed readiness plus its
  `supportsBodyText` vocabulary probe decide per body, the HIR body-shape
  walk and its two MIR shape/coherence re-proofs are deleted, and a
  declined body stays on the compatibility path instead of becoming a
  near-miss internal error. Declaration-identity facts, concrete-instance
  resolution, and the HIR/MIR metadata seal remain on the selection path.
- **Scalar-leaf and scalar-direct-call families dissolved (0.160.0).**
  Analysis-driven per-body admission covers every body the two families
  selected, so their selectors, whole-graph verification walkers, and text
  routes are deleted and their bodies publish under the general scalar-cfg
  label. Per-body admission also emits the ordinary callers those
  whole-graph contracts had to reject: constexpr, static-member, and
  internal-linkage callees stay on the compatibility path while calls to
  them emit from verified MIR. The text step asserts literal-value
  fidelity, so a forged out-of-range literal remains an internal error
  rather than implementation-defined C++. The remaining HIR-shaped
  admission lives in the three atomic families (scalar-failure-callgraph,
  class-default-cleanup, owned-lifecycle).
- **Storage locals, allocation, and the narrowed cleanup gate
  (0.197.0).** Storage-typed locals join the lifetime-slot
  representation (the slot classification widens beyond class owners),
  with slot values reaching storage staging, moves, and
  construct-by-move through the slot's checked accessor; allocation
  spells through mir_prefix_allocate_v1 publishing into its
  storage-typed result value; a storage value assigns into its owner
  field by move with the copy echo suppressed; and the blanket
  failure-cleanup gate narrows to its one real hazard — a cleanup
  destructor that may itself raise stays behind the unrepresented
  double-failure envelope, while bodies whose cleanup cannot raise are
  admitted and governed by the vocabulary probe, the MIR verifier
  owning drop-schedule correctness. The do-while fixture body converts
  under the narrowed gate, its compat loop-text pin inverted to the
  verified-MIR marker.
- **ADR 019: the generic-owner transformed-member boundary
  (0.196.0).** A failure-capable member of a generic owner emits per
  concrete instance as an explicit member specialization pair: the
  primary class template declares the transformed sibling
  (definition-free — the fixpoint guarantees only emitted
  specializations are referenced), each eligible instance defines
  `template <> Owner<Args>::name__gti_mir_failure(...)` with the
  general failure body, and the boundary wrapper is the explicit
  specialization of the original member routing failure through the
  runtime terminator — the free-generic pattern (0.182.0) carried onto
  members without friend machinery, receiver respelling, or whole-class
  specializations. The admission fixpoint recognizes generic-owner
  member callees through the same per-instance eligibility. Proven
  end to end behaviorally: the generic vector owner's out-of-bounds
  erase now reports the defined contract (exit 70,
  `index_out_of_bounds in private_storage`, the stdlib site) instead
  of the legacy storage abort, pinned in the runtime suite. Corpus
  markers 1271 -> 1276 with byte-identical defined-program behavior.
- **P-STORAGE-01 slice 4 begins: the storage failure form emits
  (0.195.0).** The general emitter spells prefix-storage operations
  through the shipped mir_prefix_*_v1 checked helpers: a value loaded
  from a storage-typed place is a staging class that never
  materializes (the call spells the field or binding lvalue directly,
  relocation staging a second place), Move instructions stage by-value
  elements, the modeling receiver on storage calls is validated and
  never spelled, and each operation publishes into the standard
  per-detector failure status whose returned outcome the existing
  status-sourced record write already carries — multi-outcome sites
  need no new record form. The first storage bodies leave the
  compatibility route: the concrete string owner's storage mutations
  emit from verified MIR across the corpus with byte-identical
  behavior; the generic vector owner's members remain
  compatibility-bound by the transformed-member boundary, and the
  reserve-class bodies wait on allocation, storage locals, and the
  cleanup drain vocabulary.
- **Write-staged receiver calls (0.194.0).** The receiver-carrying
  call convention gains the mutable form: a receiver staged as a write
  borrow reaches exactly a mutable member, a read borrow exactly a
  read-only member, with the same staged-place spelling and qualified
  member name in emission. This is the storage vocabulary's
  prerequisite — every container mutation begins with a write-staged
  receiver call — and the Chooser fixture pins the convention
  behaviorally through a mutable forwarding member.
- **P-STORAGE-01 wrapper migration: vector and string on prefix
  storage (0.193.0).** Both containers' internals moved wholly onto the
  prefix capability — fields, the iterators' storage borrows (the
  default-library borrow-carrier rule admits the prefix kind), and
  every operation: construction loops and push/emplace append at the
  live length, pop/clear/resize-down pop the last element, reserve and
  shrink relocate the complete prefix, and insert/erase use two new
  sealed invariant-preserving primitives (`prefix_storage_insert`,
  `prefix_storage_erase`) whose element shift and construction or
  destruction happen inside one runtime operation — strictly stronger
  than the sparse shift-then-construct pair, which exposed a
  partially initialized gap between two calls. The native evidence
  battery grew matching editing scenarios and guards across the same
  standard/optimization/sanitizer matrix. The example corpus stayed
  byte-identical in stdout and exit codes, the checked-vector
  benchmark workload passes on the migrated containers, and the
  emission-gate census keeps every body coherent. Sparse `storage<T>`
  remains the arbitrary-slot capability with both families rejecting
  each other's storage.
- **P-STORAGE-01 slice 3 (capability evidence): the native battery
  (0.192.0).** A dedicated suite compiles trusted generated programs
  natively and proves the prefix contract end to end: the behavioral
  fixture passes at C++20/C++23 across native -O0/-O2/-O3 and runs
  clean under dedicated generated-program ASan/UBSan builds — append
  order, logical-prefix reads and mutation through the borrow, pop
  destroying exactly the last element, relocation transferring the
  complete prefix while the moved-from shells correctly skip user
  destructor bodies (the lifecycle flag cleared by move), move
  transfer, and reverse destruction at scope exit, all pinned through
  a destructor-order checksum — and all five runtime guards trip
  their exact diagnostics. The comparative benchmark gate and the
  construction-failure rollback interplay bind at wrapper migration.
- **P-STORAGE-01 slice 2: the prefix-initialized storage capability
  (0.191.0).** A distinct compiler-private capability for
  vector/string-shaped owners: `gti_internal::prefix_storage<T>` binds
  by trusted declaration identity to its own type kind and a
  seven-intrinsic family (allocate, append, pop, read, read-mut,
  length, relocate) whose contract is the prefix invariant —
  construction appends exactly at the live length, destruction removes
  exactly the last live element, relocation transfers the complete
  prefix into an empty destination, and reads check the logical prefix
  structurally, with no per-slot initialization bitmap in the backend
  representation. Semantics, HIR, MIR (schema `mir-v31`), the
  optimizer effects tables, and the compatibility backend all bind the
  capability; borrows present as place-category element values exactly
  like the sparse reads; sparse `storage<T>` remains for
  arbitrary-slot owners, and each family rejects the other's storage
  type. Trusted-prelude fixtures pin frontend acceptance, the mutual
  rejection, and the emitted backend representation; the public
  wrappers stay unmigrated until the slice-3 evidence battery passes.
- **P-STORAGE-01 slice 1: the identity-bound public logical-size check
  (0.190.0).** A new trusted prelude intrinsic —
  `gti_internal::index_bounds_check(index, size)`,
  `IntrinsicKind::StorageBoundsCheck`, a pure two-scalar comparison
  whose only effect is the defined index_out_of_bounds failure — guards
  every public `vector` and `string` accessor (`at`, `operator[]`,
  `front`, `back`) against the logical size before the storage read.
  The outcome detail is identity-bound: semantics names the enclosing
  trusted default-library container, so the sites report
  `index_out_of_bounds` in `vector`/`string`, and an empty container's
  public indexing reports the logical bound ("container index out of
  logical bounds") rather than leaking the private storage invariant
  (`GTI-R0010` uninitialized access) — the cli scenario that pinned the
  leak migrates to the defined contract. Sparse `storage<T>` remains;
  the prefix-initialized capability, its evidence battery, and the
  backend capability row are the remaining P-STORAGE-01 slices.
- **Initializer bodies join general MIR emission: field and static
  slices (0.189.0).** A concrete class whose verified
  field-initializer body is passive — one straight-line block ending in
  Exit whose only work is literal materialization, bare
  default-initialization, and lifecycle boundaries — hands its in-class
  initializer spelling to the general route: staged literals spell from
  the MIR schedule through the shared range-asserting literal writer,
  bare defaults spell no initializer text, and the class publishes a
  field-initializers-instance marker after its definition. The
  verified-empty static-field-initializers body publishes its own
  marker. Fail-closed declines keep every other shape with
  compatibility: checked detectors (the in-class negative-literal
  initializers), storage reads, generic owners (template text is
  instance-shared), C-ABI records and unions (token-equivalence with
  the bridge header), and unsupported literal representations — the
  raw-pointer literal initially threw instead of declining, caught by
  the suite. Corpus markers 1072 -> 1261 (65 field + 124 static
  initializer bodies) with byte-identical stdout and exit codes; the
  Module bodies and checked field initializers remain for the next
  slice of this row.
- **Constructor bodies join general MIR emission, success form
  (0.188.0).** A constructor projects onto the shared scalar body facts
  like a mutable-receiver member — its receiver is inherently mutable
  under construction — and spells its verified initializer schedule
  inside the constructor body with no C++ member-initializer list.
  Fail-closed selection keeps every shape the body form cannot
  preserve: generic declarations keep their compatibility template
  (native compile on the length-parameterized constructor caught the
  missing gate), bases, unions, polymorphic and C-ABI owners decline,
  failure-capable construction stays with the rollback machinery, the
  family routes keep precedence, and the owner's in-class field
  initializers must be observation-free (bare default-initialization
  and literal stores only) so running them under the body form is
  indistinguishable from initializer-list suppression. Corpus markers
  1048 -> 1072 with byte-identical stdout and exit codes across all 57
  examples.
- **Receiver-carrying calls: borrow-staged call inputs and qualified
  member-call spelling (0.187.0).** A receiver or by-reference argument
  stages as a read borrow of a spellable place; the call spells the
  staged place followed by the qualified member name in both the
  success and the transformed defined-failure conventions, pinned
  behaviorally by a member chain whose transformed receiver call
  propagates the leaf record (exit 70, the member's own line). The
  failure selector admits operator members and reference parameters for
  concrete owners; the admission fixpoint sharpens member-callee
  availability to the selector's actual decision, so a member of a
  generic owner — which never emits a transformed form — correctly
  drops its callers; and the failure-free effect proof admits reference
  parameters with scalar field loads through their dereference carriers
  (v0.177's lesson: flip the summary, do not teach call sites to
  compensate). A staged receiver reached only through an implicit owner
  dereference declines — caught fail-loud by the native sweep before
  the type-identity rule closed it. Corpus markers 1038 -> 1048 with
  byte-identical stdout and exit codes; READY-NOVOCAB function bodies
  72 -> 46, retiring the iterator-comparison pool except the
  string-owner operators the storage fixpoint correctly withholds.
- **Loan erasure slice 1b: reference parameters, dereference chains,
  operator members (0.186.0, ADR 018).** A reference parameter keeps its
  C++ reference at the signature boundary and binds a pointer carrier in
  the body (`const auto *__gti_mir_p_N = &__gti_mir_arg_K;`); a
  dereference-projected place spells through its base carrier with field
  chains (`(*__gti_mir_p_N).field`); and operator members of generic
  owners join the member-specialization path — the friend adapters
  dispatch through the member unchanged, so the exclusion was
  unnecessary. The signature boundary admits single-argument reference
  parameters, and the fixture's reference-reader sibling
  (`compatibility_reference`) converts, inverting its old compatibility
  pin. Corpus markers 1030 -> 1038 with byte-identical stdout; the
  member fixture pins the pointer-carrier binding and dereference chain
  end to end.
- **Loan erasure slice 1a implemented (0.185.0, ADR 018).** The general
  emitter carries the core loan machinery: one hoisted pointer local per
  MIR loan identity (`const` follows access mode), `Borrow` as address-of
  through a composable place expression (bindings, storage globals,
  receiver fields, receiver field elements, sibling-array elements, loan
  carriers), `EndBorrow` as a boundary comment, and a returned loan as
  `return *__gti_mir_loan_<id>;` under the reference-returning ABI
  signature. The Borrow capability row ships as `mir_loan_pointer_v1`.
  Local and Return loan kinds are admitted; call-result, stored, and
  parameter loans decline until their slices. The first production
  consumer is a loan-returning accessor emitting as a generic-owner
  member specialization, pinned corpus-wide in the byte-agreement
  census; remaining slice-1 shapes (reference parameters, dereference
  projections for the iterator operators) follow.
- **Loan erasure representation ratified (ADR 018).** Loans erase to
  typed pointers with deref-at-use inside lowered bodies and C++
  references at ABI boundaries — the same lowering a production compiler
  applies to references — with compile-proven endpoints emitting as
  boundary comments and the Borrow capability row naming the contract
  (`mir_loan_pointer_v1`). The measured pool splits 49 storage-free
  failure-free bodies (slice 1), 31 storage-free checked bodies
  (slice 2), and 100 storage-bound bodies sequenced behind
  `P-STORAGE-01`. Success-edge loans cross the transformed failure ABI
  as pointer out-parameters when a measured pool demands it.
- **Member specializations for generic owners (0.184.0).** The
  specialization machinery extends to member functions of generic
  classes: each admitted concrete instance publishes as the explicit
  member specialization (`template <> RET Owner<Args>::name(...)`)
  holding the general body text, declared immediately after the class
  definition so no use can instantiate the member first, with the
  deferred template definition remaining for unadmitted instantiations.
  Owners whose representation collapses to a native alias decline (their
  spelling is no template-id), and a leading global qualifier is dropped
  after the return type to avoid maximal-munch misparsing — both caught
  fail-loud by the native sweep during development. The historical
  single-generic-instantiation hazard (substituted HIR bodies regressing
  examples 07/17 under declaration-keyed emission) does not apply to the
  per-instance form, and its old boundary pin inverts. Corpus markers
  1020 -> 1029; success-form only, since a transformed sibling cannot be
  specialized into a template.
- **Expected-returning checked intrinsics in the general vocabulary
  (0.183.0).** A checked-result arithmetic intrinsic produces its failure
  inside the Expected value with no edges, and now spells as the shipped
  `::gti_internal::backend::checked_*` helper with the error type as its
  template argument; the signature boundary admits an Expected of a
  scalar payload and an enum (or scalar) error as a passive
  value-semantic sum. The entire checked_add/sub/mul wrapper cohort
  converts through the specialization machinery: corpus markers
  948 -> 1020 with byte-identical stdout, and the byte-agreement census
  pin inverts from "no checked helper may appear" to "at least one
  must".
- **Per-instance template specializations from verified MIR (0.182.0).**
  A generic free-function declaration keeps its compatibility C++
  template, and every admitted MIR instance with concretely spellable
  substituted types additionally publishes per instance: a success-form
  instance as the explicit `template <>` specialization holding the
  general body text, a failure-form instance as its transformed
  suffix-named overload (parameter types disambiguate instances of one
  declaration) with the boundary wrapper as the specialization; both are
  forward-declared beside the primary template. Pack instances and
  parameter-typed substitutions stay on the primary template — the first
  cut leaked unsubstituted pack types into specializations and every
  variadic example failed native compile fail-loud before any test pin.
  The integer print chain now runs from verified MIR end to end
  (`println(42)` emits three specializations, the transformed
  array-walking digit printer, and byte-identical output — pinned in a
  new runtime scenario). Corpus markers 942 -> 948: the corpus prints
  strings far more than integers, so the chain's corpus footprint is
  honest but small; the machinery converts template-generic instances
  wholesale as later vocabulary lands.
- **Fixed arrays, the void failure ABI, and detector constants
  (0.181.0).** The general vocabulary gains fixed-array places
  (`std::array` rows), empty `Aggregate` value initialization, one-Index
  element places spelled as subscription on their sibling array,
  bounds-checked element loads and stores through the new shipped
  `mir_checked_array_read/write_v1` helpers (exact
  `INDEX_OUT_OF_BOUNDS`/`FIXED_ARRAY` record data), proven-safe dynamic
  indexes as plain subscription, unchecked `Convert` as `static_cast`,
  `numeric_cast` conversion intrinsics, constant operands on checked
  detectors, and the void transformed ABI (no out-parameter; the wrapper
  returns through the boundary). The Bounds and Aggregate capability rows
  now name genuinely shipped surfaces, the analysis retires its blanket
  dynamic-index rejection in favor of per-instruction vocabulary, and the
  effect proof admits string-view literals (the digit-printing chain's
  last conservative link). Corpus markers 880 -> 942 with byte-identical
  stdout; a dynamic out-of-bounds read reaches the defined contract at
  exit 70 (pinned end to end); the integer print chain is fully
  probe-admitted and awaits per-instance specialization emission for
  template-emitted generic prelude bodies — the next campaign.
- **Transformed-callee calls in the failure form (0.180.0, ADR 017
  slice 2).** A failure-form body may call a failure-capable GTI callee
  through the callee's own transformed body: the call assigns a
  per-call success bool, forwards the caller's record pointer unchanged,
  publishes into the scalar result (or a typed discard), and the paired
  `Invoke` branches on the success bool with no record write of its own.
  Admission becomes a greatest fixpoint: locally supported bodies drop
  out until every may-raise callee's transformed body is inside the set,
  so mutual recursion survives and hosted-family callees (a different
  spelling of the same convention) decline this slice. The first-family
  fixture proves the protocol end to end — a caller chain whose leaf
  overflow surfaces the leaf's own site through the caller's edge and
  the boundary wrapper at exit 70 — and its behavioral check caught a
  real miscompile during development (a stale result-assignment prefix
  clobbering the out-parameter after the call returned). Corpus markers
  hold at 880: the corpus's remaining failure-capable chains end in
  fixed-array or ownership vocabulary, so this slice's corpus yield
  arrives with those; the machinery is consumed and pinned by the
  fixture chain today.
- **Member bodies join the defined-failure boundary (0.179.0).** The
  per-body failure route admits ordinary non-operator members of one
  concrete non-generic class instantiation, mirroring the success route's
  member boundary: the transformed member and its same-signature wrapper
  are declared side by side in the class and defined through the deferred
  qualified form, with receiver constness following the declared
  mutability and the seal requiring exact receiver-mutability agreement.
  The prelude's `file_handle::release` cohort converts in every program
  (corpus markers 820 -> 880, failure-form bodies 4 -> 64, stdout
  byte-identical). The hosted suite's single-termination pin re-scopes to
  the hosted main, since each boundary wrapper now legitimately carries
  its own termination call.
- **String-view literals in the general vocabulary (0.178.0).** The probe
  and text step admit `std::string` literals of `StringView` type, spelled
  through the new single authority `cppMirStringViewLiteralSpelling`
  (`std::string_view{"…", N}` with escaped data and explicit length); the
  compatibility emitter's two literal sites delegate to it and its local
  `quote` helper is gone. This unlocked `println`, `print_decimal_digit`,
  and their instantiations across every program: corpus markers
  704 -> 820, stdout byte-identical, suite and oracle green (fixture
  prelude lists gain both names; first-family count 19 -> 21).
- **Failure-free effect proof widened to views, native calls, and
  intrinsics (0.177.0).** `deriveMirFunctionDefinedFailureEffects` admits
  passive string views throughout its function component, accepts
  C-linkage/runtime-binding call targets by their `None`-propagation
  language contract, and accepts wrapping/saturating intrinsic calls.
  Prelude I/O bodies whose may-raise summaries were purely conservative
  (`write_stdout`, `print`, and their chains) now prove failure-free at
  the MIR layer, their callers re-lower without Invoke edges, and the
  bodies convert through the existing success route — the phase-correct
  resolution of the transformed-callee question for conservative chains,
  produced at the layer that owns effect facts. Corpus markers 647 -> 704
  with byte-identical stdout; suite and oracle green; fixture prelude
  lists gain `print` (first-family count 18 -> 19).
- **Per-body defined-failure boundary, first slice (0.176.0, ADR 017).**
  The general emitter gains the failure form of its vocabulary: checked
  detectors spell as `mir_checked_*_v1` status helpers through one shared
  spelling authority, `Invoke` branches on the status and writes the exact
  MIR-owned record (site from the instruction, artifact identity from the
  program's failure metadata) on the failure edge, `PropagateFailure`
  returns false after its cleanup block, and `Return` publishes through
  the transformed out-parameter. A leaf failure-capable free function
  emits the transformed private body plus a same-signature boundary
  wrapper that routes failure into `gti_rt_failure_terminate_v1` — the
  structured report and exit 70 of the defined contract — so callers stay
  unchanged and no call-graph closure is required. Calls inside the
  failure form are admitted only toward proved-failure-free targets;
  failure-capable callees await the transformed-callee slice, and member
  receivers await the member site. Migrated behavior: leaf checked
  arithmetic now reports through the defined contract instead of the
  legacy abort (cli scenarios moved to the structured table; the
  first-family fixture pins the transformed body, wrapper, and a
  failure-path run at exit 70). Corpus markers 643 -> 647 with stdout
  byte-identical; the larger yield arrives with the follow-up slices.
- **String-view signatures and the C marshalling boundary (0.175.0).** The
  scalar-CFG signature gate admits `SemanticType::StringView` parameters
  and returns — a passive value-semantic view spelled `std::string_view`
  by its representation row — and the general vocabulary marshals view
  arguments to C-linkage targets through the shipped `to_c_string_view`
  converter, byte-identical to compatibility call sites; a view result
  from a C-linkage call stays declined (no reverse converter is
  modelled). The gap census that chose this slice also recorded the
  admitted-vs-marked truth: 355 of the previously admitted bodies are
  extern C-linkage declaration skeletons (phantom admissions with no GTI
  body), and the failure-record ABI pool now stands at ~480 Ready bodies
  awaiting its design release. Corpus markers 529 -> 643; program stdout
  byte-identical across all 57 examples; suite and oracle green. The
  first widening attempt shipped no marshalling and every example failed
  native compile — the fail-loud native boundary caught it before any pin.
- **Arithmetic-intrinsic vocabulary and constexpr admission (0.174.0).**
  The general emitter spells the six wrapping/saturating integer-arithmetic
  intrinsics as their shipped `::gti_internal::backend` helpers through one
  spelling authority shared with the compatibility call-site emission
  (`cppIntegerArithmeticIntrinsicSpelling`); the three checked kinds stay
  declined per body because their `Expected` result is outside the scalar
  vocabulary (pinned corpus-wide). The scalar-CFG declaration gate also
  stops rejecting `constexpr` declarations — GTI constant contexts are
  frontend-evaluated and the emitted monomorphized definition was never
  C++-`constexpr` on either route; the coherence seal keeps exact
  HIR/MIR/semantic constexpr agreement. Together these convert the
  admission census into production markers: corpus markers 385 -> 529 and
  general admission 881 -> 1,025 of 2,429 bodies, native sweep and
  differential oracle green. The `constexpr_target` fixture pins migrate
  from the compatibility list to the selected list.
- **NativeInterop and Intrinsic capability rows shipped (0.173.0).** The
  backend boundary now names the two shipped helper surfaces the analysis
  was withholding: `gti_rt_c_symbols_v1` (the runtime's C prototypes,
  declared by every emitted artifact and linked from gti_runtime) and
  `gti_internal_backend_helpers_v1` (the intrinsic lowering family). The
  honesty survey found exactly four native symbols emitted inside MIR
  bodies across the corpus (`gti_rt_write_stdout_byte`,
  `gti_rt_read_stdin_byte`, `gti_rt_read_file_byte`, `gti_rt_close_file`),
  each spelled through its body-name row byte-identically to compatibility
  emission; no intrinsic body emits because the text vocabulary declines
  what it cannot spell. Yield measured by the body census: general
  admission 186 -> 881 bodies and production corpus markers 157 -> 385,
  with the differential oracle and full native sweep green. The census
  also established the remaining large buckets: 362 Ready function bodies
  blocked on the failure-record ABI, 266 on deeper capabilities, and the
  initializer/module kinds are mostly empty Exit-terminated skeletons
  (137 non-empty field-initializer bodies) whose authority migration is a
  representation change, not a body-text slice.
- **Class-default-cleanup destructor route dissolved (0.172.0).** The
  general emitter admits `MirBodyKind::Destructor` bodies: the probe and
  text step share one body-facts projection (a destructor has no
  parameters and an inherently mutable receiver), and the store path
  learned to spell Symbol-rooted destinations through storage rows — the
  probe had always admitted global stores, but the text step previously
  emitted an undeclared binding for them (caught fail-loud by the fixture's
  native compile). The lifecycle-cleanup site now consults the general
  route first, and the family's destructor selector, body emitter, and
  every remaining `class-default-cleanup` predicate are deleted (~360
  lines); the label no longer exists in the compiler. Per-body admission
  also emits the ordinary destructors the family had to reject
  (`ExplicitDefault`, `FieldOwner`, and two empty corpus destructors),
  while the checked-arithmetic near miss stays compatible.
- **Mutable-receiver field writes admitted (0.171.0).** The scalar-CFG
  declaration gate no longer rejects mutable receivers: a member storing to
  its own scalar fields through `this` emits from verified MIR, with the
  field place bound as a non-const reference exactly when the declared
  receiver is mutable. The store direction is fail-closed at the vocabulary
  probe — an `Initialize` or `Assign` destination rooted at `This` under a
  read-only receiver declines the body — and the failure-free function
  proof drops its read-only-receiver restriction, since a scalar store
  through the receiver cannot raise.
- **Class-default-cleanup function route dissolved (0.170.0).** The
  general emitter gains the lifetime-slot vocabulary — sealed slot
  declarations spelled from the LifetimeStorage capability row,
  argument-less generated-default `.construct()`/`.destroy()`, storage
  reads through symbol rows, reparenting Initializes, and cleanup-boundary
  markers — and the family's function selector, its HIR/MIR body walks,
  and its text route are deleted. Per-body admission also emits the
  ordinary lifetimes the whole-family contract had to reject (declared
  zero-argument constructors, field-initialized classes, scoped and
  conditional locals), with the argument-carrying Construct bound
  fail-closed because `.construct()` cannot spell constructor arguments.
  The destructor-definition route remains the class-lifecycle authority
  until general Destructor-body emission lands.
- **Failure-free effect proofs widened (0.168.0).** The function proof
  admits read-only receivers' scalar field loads (the projection carrier
  plus one trivially droppable projected field cannot raise), and the
  destructor proof grows from the single-block literal/assign shape to the
  scalar-CFG shape: receiver field reads, loads, the scalar operation set,
  and Goto/Branch/Return control flow over proven values, with writes still
  confined to storage places. The owned-lifecycle fixture's scalar user
  destructor now summarizes provably failure-free, so its categorical
  double-failure flag never fires; the native-closing prelude destructor
  legitimately keeps its containment flag, so the double-failure envelope
  remains the path for native-cleanup destructors specifically. The emission probe and text step reject
  receiver-carrying calls at both layers, closing the hole the widening
  would otherwise open (a member call spelled without its receiver).
- **Construction readiness measured and pinned (0.167.0).** The rows now
  carry every function instance's emitted name (runtime bindings and
  C-linkage declarations included), the executable module body's own row,
  executable field-initializer body rows, and the DefinedFailure sealed
  helper the artifacts already ship. Under production rows, every
  constructor and field-initializer body of the owned-lifecycle fixture is
  analysis-Ready — pinned in the body-emitter suite — leaving exactly the
  destructor double-failure envelope, the withheld capability families,
  and the Stage-E containment proofs between the atomic families and
  dissolution.
- **Default-field rollback now composes into source constructors.** Every
  unoverridden active-cleanup field completed by the verified per-instance
  initializer body enters the constructor as an initially active
  `ConstructionRollback` on its exact This-rooted place. Program verification
  matches owner, field binding and symbol, type, cleanup descriptor, uniqueness,
  and declaration order; explicit constructor initializers suppress the
  corresponding default entry state. Failure drains the inherited obligations
  with later stages, while normal completion transfers them to the object.
  This closes the concrete `std::filesystem::path(std::string_view)` backend
  gap without source-name recognition or backend-inferred cleanup. State-bearing
  bases and the destructor double-failure envelope remain separate work.
- **Field-initializer bodies joined failure control flow (0.166.0).**
  Owning declaration initializers arm ConstructionRollback obligations on
  their field binding places at the persistent-storage Initialize site,
  FieldInitializers bodies route defined-failure edges under the same
  body-wide unarmed-transfer suppression (with the unarmed definition
  sharpened to attached transfers without ownership continuation), failure
  edges drain through the merged global reverse-order sequence, and the
  emission analysis retires MissingConstructionScheduleMir exactly for
  fully armed bodies. Remaining before family dissolution: base
  subobjects, static-field initializer bodies, and the destructor
  double-failure envelope.
- **Owned-parameter class fields unified onto rollback (0.164.0).** The
  stage branch admits owned-parameter initializers, the moved value's
  obligation reparents into the armed rollback obligation (the verifier's
  owned-move contract accepts that consumption beside the legacy transfer),
  and constructor failure edges now drain temporaries, scope bindings, and
  rollback as one globally reverse construction-ordered sequence behind a
  single boundary — the phase-ordered drain could not satisfy the
  primary-chain rule once parameter bindings sat below armed rollback.
  Unique-owner fields joined the staged form in 0.165.0; remaining legacy
  transfers cover only shapes outside the single-owning-argument stage.
- **Unarmed constructor transfers fail closed (0.163.0).** A constructor
  with any silent subobject transfer into `this` (owned-parameter and other
  unstaged forms) routes no defined-failure edges: lowering prescans the
  initializer list before placing any edge, `mirBodyRoutesFailureEdges`
  holds the body-wide verifier rule, and the emission analysis retires its
  categorical rollback issue exactly when a constructor's verified MIR
  carries complete rollback coverage (no state-bearing bases, no unarmed
  transfers, every non-trivially-droppable field armed).
- **Partial-construction rollback represented (0.162.0, schema v30).**
  Ordinary constructors stage each class field completed from one owning
  temporary as an Initialize that reparents into a ConstructionRollback
  obligation on the This-rooted field place; constructor bodies are admitted
  to MIR failure control flow, failure edges drain the armed set in reverse
  stage order, normal completion retires it, and the verifier rejects a
  forged leaking edge (pinned with a mutation test). Remaining before the
  atomic families can dissolve: base subobjects, owned-parameter stage
  unification, initializer-body schedules, and the destructor double-failure
  envelope.
- **Rows for the next dissolution measured and landed (0.161.0).** The
  production rows now carry namespace-global storage spellings,
  constructor/destructor body names, and the sealed lifetime-slot helper,
  which makes every non-entry scalar function body of the
  class-default-cleanup fixture analysis-Ready (pinned by the body-emitter
  suite). Dissolving the three remaining families is blocked on MIR
  authority, not representation: their constructor and destructor bodies
  still carry the partial-construction-rollback and double-failure
  structural gaps the atomic contracts currently license, so those
  families keep their selectors until that authority lands.
- **Admission widened per body (0.158.0-0.159.0).** The scalar-cfg gate no longer
  excludes compiler-private declarations, so standard-library scalar bodies
  select per body through the same fail-closed walk (corpus production
  emission 35 -> 150 bodies). A constexpr relaxation was measured to yield
  zero selected bodies and was reverted rather than kept as unexercised
  risk.
- **Exit gate:** all reachable GTI bodies have one executable authority; every
  current checked family preserves exact record, prior effects, and cleanup at
  O0/O1/O3 and C++20/C++23; native expected/assert/abort paths and duplicated
  emitter failure helpers are gone; closed-call-graph and verifier mutations
  prove that no failure channel crosses an unconverted caller.
- **Unlocks:** the backend-independent systems-readiness execution gate, generated embedding,
  native callbacks, and managed threads.

## Phase S: Systems Substrate — Layout, FFI, And Allocation

The external language review correctly identifies these as blockers for a
complete systems-language claim. Systems readiness requires real records,
callbacks, opaque handles, and an application allocator, not every C ABI or
manual-lifetime feature. The foundations are sequenced so those useful slices
do not expose C++ object layout as GTI semantics.

### S-LAYOUT-01: GTI Target Data-Layout Contract

- **State/role:** done; systems-readiness substrate for C interop and allocation.
- **Prerequisites:** existing `TargetInfo` triple, pointer width, and
  endianness.
- **Scope:** Define the target-property vocabulary, triple interpretation,
  unknown-property and supported cross-target behavior, then introduce a
  GTI-owned immutable data-layout description for the scalar domains and
  representation categories GTI actually supports. Include size, ABI
  alignment, preferred alignment where meaningful, pointer width, endianness,
  and unsupported-target diagnostics. The native compiler and any private LLVM
  query may populate it, but neither representation crosses the interface.
- **Non-goals:** stable GTI class ABI, vtable layout, packed records, unions,
  bit-fields, or copying LLVM `DataLayout` into public headers.
- **Exit gate:** supported target triples produce deterministic facts checked
  against the native ABI in installed-toolchain tests; unsupported layouts fail
  before code generation.
- **Completion evidence:** `TargetDataLayout` is an LLVM-free immutable scalar
  fact value with byte size, ABI/preferred alignment, 64-bit pointer width, and
  little endianness. `llvm::Triple` remains a private normalizer mapped into
  exact `os`/`vendor`/`arch` vocabulary; detailed parsing distinguishes
  malformed, architecture, endianness, and OS failures. `GTI-S2062` rejects an
  explicitly unsupported selected layout before parsing/semantics/backend, and
  the installed compiler-library smoke compares every current scalar domain
  with the native ABI. Cross-compilation toolchain selection and aggregate
  layout remain explicit non-goals.
- **Unlocks:** `S-LAYOUT-02`, atomic capability checks, native records, and
  allocator alignment.

### S-LAYOUT-02: Bounded `sizeof` And `alignof`

- **State/role:** done; prerequisites `D-LANG-01` and `S-LAYOUT-01` are
  complete; bounded systems-readiness substrate.
- **Scope:** Add grammar, semantic, constexpr, HIR, MIR, formatter,
  Tree-sitter, LSP, and backend support for types whose layout is already a GTI
  fact: primitives, one-level raw pointers, transparent aliases, and recursive
  fixed arrays with positive concrete extents. The source form is the reserved,
  type-only `sizeof(type)` or `alignof(type)` operator; both have exact
  `uint64_t` type and `alignof` reports ABI alignment. Reject incomplete,
  direct symbolic type parameters, zero-sized or symbolic arrays, overflowing
  arrays, nominal values, and backend-dependent types rather than asking
  emitted C++. Pointer layout does not require its pointee to have a layout
  contract. `S-ABI-02` now consumes the same facts for the explicit passive
  native-record family.
- **Non-goals:** `alignas`, packing, a stable layout for every ordinary class,
  expression operands, direct query expressions in array-extent grammar, or
  using host `sizeof` as a semantic proof.
- **Exit gate:** values are frontend constants and match native probes across
  all supported targets; invalid categories get stable diagnostics.
- **Completion evidence:** `LayoutQuery` retains the type-only syntax;
  semantics resolves aliases and derives checked recursive array facts solely
  from `TargetDataLayout`, recording `GTI-S2063` on unsupported types, symbolic
  or zero extents, and overflow. HIR preserves a layout-query value plus its
  exact constant; MIR lowers it to a literal; the C++ backend emits that number
  and never a native query. Formatter, Tree-sitter, Neovim, LSP, dedicated
  pipeline tests, synthetic supported-target checks, native ABI probes, and an
  installed compiler-library consumer cover the boundary.
- **Unlocks:** allocation contracts, serialization, bounded C records.

### S-ABI-01: Native Record And ABI Proposal

- **State/role:** done; design-first systems-readiness contract recorded in
  [ADR 013](../decisions/013-bounded-native-c-records.md).
- **Scope:** Decide how a source declaration opts into C-compatible record
  layout, which fields/types are allowed, padding and alignment, by-value versus
  pointer passage, ownership prohibition, target dependence, and diagnostics.
  Keep ordinary GTI classes out of the C ABI.
- **Non-goals:** C++ ABI, arbitrary inheritance, bit-fields, packed access,
  unions, or automatic headers/importers.
- **Exit gate:** the proposal defines source opt-in, target dependence, layout,
  allowed signatures, ownership exclusions, diagnostics, and a C-oracle test
  matrix without claiming implementation.
- **Completion evidence:** ADR 013 selects `[[c_abi]] struct`, a closed passive
  field family, source-order checked layout, by-value and one-level-pointer
  passage, recursive unsafe classification, and a native C oracle without
  importing ordinary class ABI.
- **Unlocks:** `S-ABI-02`.

### S-ABI-02: Bounded Native Record Implementation

- **State/role:** done; bounded systems-readiness implementation.
- **Scope:** Implement the accepted closed record family through syntax,
  semantics, HIR, MIR, backend, headers/runtime, and tooling.
- **Exit gate:** a C-compiled layout/call oracle agrees with GTI on every
  supported target; unsupported fields/signatures fail before backend entry.
- **Completion evidence:** syntax, semantic layout, `GTI-S2064`, layout-query
  composition, extern-C validation, HIR/MIR retention and verification,
  backend layout assertions, formatter, Tree-sitter, LSP, installed-library
  smoke, and the O0/O3 x C++20/C++23 C oracle are integrated. Pointer-to-pointer
  out parameters, callbacks, and ownership transfer remain separate rows.
- **Unlocks:** `S-ABI-03`, C struct APIs, platform address records, and broader
  networking.

### S-ABI-03: Generated C/C++ Bridge Header

- **State/role:** done; bounded systems-readiness tooling and representation
  refinement.
- **Scope:** Emit one compiler-owned header from checked `[[c_abi]]` records
  and source `extern "C"` declarations. Its C17 branch uses deterministic C
  record names and strict prototypes; its C++20/C++23 branch preserves exact
  GTI namespaces and declares the same symbols with C linkage. Both branches
  retain semantic size/alignment/offset assertions. Make native records pure
  representation declarations by rejecting field initializers, canonicalize
  their emitted C++ field spellings, and keep GTI lifecycle policy out of the
  cross-translation-unit definition. Expose the artifact through
  `NativeHeaderBackend`, the installed compiler library, and direct
  `--emit-native-header` mode.
- **Non-goals:** importing or parsing foreign headers, general C++ ABI,
  exceptions across C, callbacks, pointer-to-pointer out parameters, ownership
  transfer, or automatic project include-path publication.
- **Exit gate:** one generated header compiles unchanged as C17 and
  C++20/C++23; C and C++ implementations link together with a GTI caller at
  O0/O3; a namespaced record preserves C++ identity; emitted GTI and header C++
  definitions agree; installed consumers can construct the backend.
- **Completion evidence:** `native_header_pipeline` covers deterministic
  header shape, namespace/name mapping, canonical definitions, strict empty C
  parameter lists, and layout assertions. `native_header_c_cpp_oracle`
  generates the artifact through the CLI, compiles separate C and C++ shims,
  uses a private C++ class behind the adapter, and executes all four
  O0/O3-by-C++20/C++23 combinations. `GTI-S2064` rejects representation-field
  initializers before backend entry.
- **Unlocks:** checked adapter authoring for existing C and C++ libraries and
  the independent pointer-only opaque-handle sub-slice. Callback and annotated
  ownership-transfer families remain gated.

### S-CALL-01: Function Items And C Callback Boundary

- **State/role:** done for the bounded same-thread C-library client.
- **Scope:** A namespace-scope `using` declaration names an exact nullable
  native function type. The type may appear in passive native records and C
  parameters/results. Contextual conversion selects exactly one non-generic,
  non-member namespace GTI function with a body and exact parameter/result
  types. Semantics owns selection, HIR owns concrete target identity, and MIR
  owns adapter identity, target failure effect, `TerminateInvocation` policy,
  and native-exception firewall. The backend emits one process-lifetime
  `extern "C" noexcept` thunk, so same-thread native retention cannot outlive
  code or captured state.
- **Initial boundary:** the complete recursively bounded C ABI family,
  including fixed-width scalar/pointer, `c_string`, passive native records,
  and nested named callback aliases.
- **Non-goals:** direct callback-value invocation, C varargs, arbitrary casts,
  lambdas/captures, closure-to-`void*` erasure, inferred userdata ownership, or
  foreign/native-thread GTI entry. The latter still requires runtime attachment
  and the adopted concurrency capability rows.
- **Completion evidence:** `native_callback_pipeline` covers syntax,
  contextual exactness, null state, record fields, callback returns, HIR/MIR
  adapter verification, failure containment, C/C++ header shape, formatter,
  and negative forms. `glfw_3_4_c_oracle` exercises retained same-thread
  callbacks and the complete GLFW callback surface. A failing callback exits
  through status 70 without returning across C; LSP and Tree-sitter gates cover
  the same syntax and diagnostic.

### S-FFI-02: Additional C ABI Families

- **State/role:** done for the selected bounded families and GLFW 3.4
  acceptance client. The independent pointer-only opaque-handle family landed
  in 0.119.0. The demonstrated C-library API this row's families are selected
  against is **GLFW 3.4**: a real, widely used
  C library whose surface is broad enough to exercise records, strings,
  out-parameters, and callbacks, and small enough to bind completely.
  Callbacks remain owned by `S-CALL-01` rather than blocking handle identity.
- **Implemented sub-slice:** `[[c_opaque]] struct Name;` creates one nominal,
  incomplete, nongeneric, baseless handle usable only behind a one-level raw
  pointer. `[[c_abi]]` records and `extern "C"` signatures admit those pointers;
  the generated C17 branch emits an incomplete typedef and the C++20/C++23
  branch an exact namespaced forward declaration. C or C++ may complete the
  private representation, while GTI infers no layout, ownership, nullability,
  transfer, or cleanup. `GTI-S2065`, formatter, Tree-sitter, LSP, compiler
  library tests, and the mixed C/C++ native oracle cover the boundary.
- **Layering contract:** The `extern "C"` surface is a faithful mirror of the
  C declaration and makes no safety claim; safety is a wrapper written in
  ordinary GTI over that surface, per ADR 004. A family is admitted when the C
  declaration can be spelled exactly and its obligations stated, not when the
  declaration is provably safe. Restricting what may be *declared* pushes the
  problem outside the language, where GTI can say nothing about it at all.
- **Completed families, ordered against the GLFW binding.** A binding attempt
  at 0.199.0 bound 3 of 5 records and roughly 90 functions; GLFW 3.4 has 124
  public `GLFWAPI` declarations. The measured blockers below are now complete.
  Each family states
  initialization, retention, aliasing, nullability, cleanup, and unsafe
  obligations. A selected ownership family must build on the opaque identity
  rather than retroactively making raw handle pointers owners.
  Diagnostic codes are retained beside the family that owns them.

  - **F1 — fixed-array `[[c_abi]]` fields (complete).** Admit `T[N]` fields of otherwise
    admissible element types. ADR 013 already anticipates this: it excluded
    them only until their backend representation is defined directly rather
    than inherited from `std::array`. C array layout is unambiguous, so no
    layout proof is lost, and element access keeps GTI's existing bounds
    checking. Cheapest family; do it first.
    - *Diagnostics.* `GTI-S2064` stops naming fixed arrays. Two replace it:
      `GTI-S2069` a fixed-array `[[c_abi]]` field has a zero or negative
      extent; `GTI-S2070` its element type is itself outside the C ABI field
      set. Both name the field and its written type, as `GTI-S2064` does.
    - *Obligations.* Initialization: the array is inline record storage and
      follows the record, adding no per-field step. Retention: none; the
      elements are stored, not referenced. Aliasing: element access aliases
      only within the enclosing record, introducing no relation the record did
      not already have. Nullability: not applicable; an array field is not a
      pointer. Cleanup: none, because the admitted element types are exactly
      the passive ones already required. Unsafe: none; element access is
      ordinary bounds-checked GTI indexing, so an out-of-range read is a
      defined `GTI-R0007` failure rather than C's silent overread. F1 is the
      cheapest family precisely because every obligation is already discharged
      by the surrounding record.
    - *Acceptance.* `GLFWgamepadstate` (`uint8_t buttons[15]`,
      `float axes[6]`), which unblocks `glfwGetGamepadState`. Its measured C
      layout is size 40, alignment 4, `buttons` at 0, `axes` at 16; that
      target is pinned in `tests/native_record_c_oracle_test.py`. The C oracle
      now proves layout plus C-to-GTI and GTI-to-C element access at O0/O3
      under C++20/C++23, while native-header tests prove direct C declarators
      and reject a `std::array` representation.
  - **F2 — the C-string boundary (bounded boundary core complete; owner
    borrowing remains).** `char` stays excluded from `[[c_abi]]`
    records for the implementation-defined-signedness reason already recorded
    in `R-NATIVE-RECORDS`. The gap is that a C API's `const char*` means
    *pointer to NUL-terminated bytes*, and spelling it `const uint8_t*` loses
    that meaning and forces hand conversion at every call site. Define one
    boundary type carrying the NUL-terminated contract. The completed core
    maps `c_string` to `const char*`, converts complete static string views at
    exact call-input boundaries, supports nullable returns and `c_string*` out
    parameters, preserves exact-overload preference, and exposes no unchecked
    dereference or index operation. C17/C++20/C++23 oracle coverage exercises
    all of those forms.
    - *Remaining diagnostics.* `GTI-S2071` a `c_string` is constructed from a source
      with no proven NUL terminator; `GTI-S2072` a `c_string` borrow outlives
      the storage it views. The second reuses the existing loan machinery and
      its wording should match the current borrow diagnostics rather than
      introduce a second vocabulary for the same rule.
    - *Remaining owner obligations.* Initialization from an owner is never
      inferred from its C++ representation and never from a bare pointer
      outside `unsafe`. Retention is a borrow, and its loan is tracked
      against the owner, so the dangling `c_str()` that C++ permits silently
      becomes `GTI-S2072`. Aliasing: read-only, with no mutation through the
      view. Nullability: a `c_string` received from C may be null and reading
      one is a defined failure, not undefined behaviour; a GTI-produced one
      never is. Cleanup: none, as it is non-owning. Unsafe: required to build
      one from a raw pointer, not required to obtain one from a GTI string.
      The current core intentionally rejects retained string-view conversion;
      it does not claim these owner obligations before they exist.
    - *Acceptance.* `glfwGetVersionString`, `glfwGetMonitorName`,
      `glfwGetWindowTitle`, `glfwSetWindowTitle`, `glfwCreateWindow`,
      `glfwWindowHintString`, `glfwGetKeyName`, `glfwGetJoystickName`,
      `glfwGetJoystickGUID`, `glfwUpdateGamepadMappings`, `glfwGetGamepadName`,
      `glfwSetClipboardString`, `glfwGetClipboardString`, and
      `glfwExtensionSupported`.
  - **F3 — out-parameter and returned pointer-plus-count (complete).** C's "returns an
    array, writes the length through an out parameter" idiom. Admit it as one
    bounded boundary form rather than admitting `T**` generally, which would
    import pointer nesting and aliasing that `R-RAW-POINTERS` excludes as a
    durable rule.
    - *Diagnostics.* `GTI-S2073` `[[c_array]]` names something that is not an
      integer out-parameter of the same declaration; `GTI-S2074` a two-level
      pointer appears outside an annotated `[[c_array]]` boundary, which keeps
      the general `R-RAW-POINTERS` rejection intact and explains the one
      exception.
    - *Obligations.* Initialization: GTI's ordinary definite-initialization
      rule still requires storage for the count before its address is passed;
      the native call may overwrite it, and the returned pointer exists only
      after the call completes. The attribute does not create a hidden pair
      value. Retention: the returned array is owned by the C library and its
      lifetime is that library's contract; GTI infers nothing and each binding
      states it per function.
      Aliasing: elements may alias anything, and GTI makes no claim.
      Nullability: the array pointer may be null when the count is zero, and
      the wrapper must handle that rather than index it. Cleanup: none by GTI;
      an API that requires freeing the result belongs to a separate
      ownership-transfer family, not this one. Unsafe: every element access
      requires `unsafe`, and the intended shape is one `unsafe` block in the
      wrapper that converts the pair into an ordinary GTI view.
    - *Acceptance.* `glfwGetError`, `glfwGetMonitors`, and
      `glfwGetRequiredInstanceExtensions`.
    - *Completion evidence.* `[[c_array(count)]]` resolves one exact writable
      fixed-width integer out parameter and admits either one C ABI pointer or
      the bounded two-level return whose inner pointer is independently
      admissible. Semantics, HIR, MIR, the MIR verifier, C++ emission, native
      header generation, formatter, Tree-sitter, LSP hover/definition/tokens,
      and C17/C++20/C++23 oracle coverage agree. Explicit nested-pointer locals,
      parameters, fields, and unannotated returns remain rejected.
  - Callback semantics remain owned by completed `S-CALL-01`; this row consumes
    that type family when exact callback aliases occur in records and C
    parameters/results.
  - The surface these families are built against is written out in
    [`c-interop-target-surface.md`](archive/c-interop-target-surface.md): the raw
    mirror, the safe wrapper, and the acceptance program.
- **Later breadth:** C varargs, unions, bit-fields, and packed records remain
  separate proposals. `printf` alone is not sufficient justification for
  importing C's least checkable call surface.
- **Exit gate for each family:** the selected family passes a real C oracle across supported
  targets, has explicit initialization/retention/cleanup diagnostics, and adds
  no backend-derived type or ownership authority.
- **Row exit gate:** complete. With F1 through F3 and `S-CALL-01`, the GLFW 3.4
  header binds all 124 functions, all five records, every callback typedef, and
  the Vulkan-facing declarations without omission. The generated header
  compiles as C17 and C++20/C++23, all 124 function names are exact-compared
  with the official header inventory, representative calls link and run at
  O0/O3, and callback failure is contained. The fixture is regression-covered
  under `tests/fixtures/`; a real display/window smoke belongs to a downstream
  GLFW package because headless CI should not define language correctness.

### S-ALLOC-01: Allocator, Provenance, And Initialization Proposal

- **State/role:** complete; adopted by
  [ADR 022](../decisions/022-safe-typed-allocation.md) after `M-LIFE-01`,
  `D-LANG-01`, `D-FAIL-01`, `S-LAYOUT-02`, and `M-OWN-02`.
- **Scope:** Define allocator ownership, byte/typed provenance, size and
  alignment, allocation failure, zero-sized requests, initialization state,
  placement construction, destruction, deallocation, and interaction with
  `unsafe`. Start from safe typed storage and arenas rather than exposing
  source-level `new`/`delete`.
- **Exit gate:** the proposal can express an arena and pool without duplicate
  destruction, uninitialized safe reads, hidden owner creation, or C++ layout
  assumptions.
- **Completion evidence:** ADR 022 defines a typed move-only allocation origin,
  owner-carried release authority, checked per-slot lifetime state, receiver-
  tied borrows, failure-atomic `expected` allocation, minimum alignment,
  overflow/exhaustion separation, successful zero capacity, reverse cleanup,
  and the homogeneous arena/pool composition boundary without public raw
  addresses or manual deallocation.

### S-ALLOC-02: Narrow Public Allocator Capability

- **State/role:** complete first public slice; `S-ALLOC-01` and `I-CAP-01` are
  done; systems-readiness implementation.
- **Scope:** Implement one GTI-owned allocator interface over narrow trusted
  operations, plus a recoverable factory returning `expected` and a typed
  partial-storage owner on which an arena, pool, or container can build policy.
- **Non-goals:** global replacement allocation, manual raw deletion, polymorphic
  allocator hierarchies, or changing `std::vector` in the first slice.
- **Exit gate:** ownership, partial initialization, failure, and cleanup are
  visible in HIR/MIR and pass sanitizer/runtime failure tests.
- **Completion evidence:** `<std/memory>` implements ordinary source-defined
  `std::allocator`, `std::storage<T>`, `std::alignment`, and
  `std::alloc_errc` over exact trusted allocation/state operations. HIR/MIR
  retain recoverable allocation, observer, construction, borrow, destruction,
  lifecycle, failure-origin, and effect facts. The public owner requires
  `std::movable T` without borrowed state. The executable fixture covers
  zero size, invalid/over-alignment, byte overflow, resource exhaustion,
  sparse construct-by-value, checked read/mutation/destruction, movement,
  reverse clear/drop, and structured failure across C++20/C++23 and O0/O3,
  including cleanup when construction of an input value fails. Public forging,
  copying, variadic emplacement, and mutation during a receiver-tied borrow are
  rejected.
- **Unlocks:** `S-ALLOC-03` container integration.

### S-ALLOC-03: Bounded Arena And Pool Clients

- **State/role:** complete first composition slice; prerequisites
  `S-ALLOC-02` and `L-RANGE-02` are complete; bounded-first systems-readiness
  evidence for source-owned allocation policy.
- **Scope:** Implement fixed-capacity homogeneous arena and pool policies over
  typed storage without C++ allocator-traits complexity. Define movement,
  allocation/insertion failure, reset/erase invalidation, and cleanup before
  generalizing to allocator-aware growing containers.
- **Non-goals:** stateful allocator propagation, public raw storage, growth,
  heterogeneous allocation, or generation-bearing pool handles.
- **Exit gate:** the selected clients prove allocator lifetime, move-only
  propagation, failure, cleanup, reuse, and receiver-borrow invalidation across
  C++20/C++23 and O0/O3 without recognizing a public allocator name in compiler
  code.
- **Completion evidence:** `<std/memory>` implements ordinary source-defined
  `std::arena<T>`, `std::pool<T>`, and recoverable `try_make_*` factories over
  `std::storage<T>`. Arena insertion is monotonic until reset; pool insertion
  reuses the lowest empty position. Both are move-only, support successful zero
  capacity, distinguish allocation from full-policy errors, retain checked
  receiver-tied access, and destroy live values in reverse slot order. Runtime
  evidence covers reset/erase/reuse, bounds and empty-slot failures, cleanup
  while producing an input value fails, and deterministic teardown. Compile
  evidence rejects copying, nonmovable elements, forged storage adoption, and
  mutation during a retained element borrow. The compiler recognizes only the
  pre-existing exact private storage operations.

### S-FREE-01: Freestanding Target Profile

- **State/role:** later breadth; `S-LAYOUT-01`, `D-LANG-01`, and `D-FAIL-01`
  are done. A concrete embedded/kernel workload may move it earlier.
- **Scope:** Define a smaller prelude and explicit required runtime services for
  a freestanding target without changing core expression, ownership, or
  cleanup semantics. The first bounded sub-slice inventories current hosted
  runtime services and proposes the target profile; later sub-slices diagnose
  unavailable allocation/output/thread services at the source/runtime boundary.
- **Non-goals:** kernel support by merely omitting libraries, implicit target
  feature probing, or a stable ABI.
- **Exit gate:** the adopted profile enumerates every required runtime service;
  one installed freestanding smoke either builds using exactly those services
  or fails early with a target-capability diagnostic, never a native link
  surprise.

## Phase C: Concurrency And The Language Memory Model

D-MEM-01 is complete as design evidence. D-FAIL-01 selected contained worker
failure with cleanup and original-record preservation, and D-MEM-02 adopted
the complete boundary in ADR 008. ADR 012 assigns its bounded public profile to
the systems-readiness work-queue outcome. The rows below make the
implementation dependency explicit; their
presence is not authorization to skip a named capability, lifetime, runtime,
or MIR prerequisite.

### I-CAP-01: Secure Compiler-Private Capability Identity

- **State/role:** done in 0.97.0; prerequisite `D-LANG-01` is done; architecture
  fix and prerequisite for every new private atomic/thread
  capability.
- **Scope:** Reserve `gti_internal` against ordinary application declarations
  and references, while allowing the trusted prelude/runtime units that own it.
  Bind compiler-private types by trusted declaration identity rather than
  qualified spelling. Cover aliases, source-unit visibility, LSP presentation,
  and forged declarations.
- **Non-goals:** making public wrappers compiler-known or adding source
  attributes that grant intrinsic authority.
- **Exit gate:** applications cannot forge or directly consume private
  capabilities; ordinary GTI stdlib wrappers continue to work by selected
  identity.
- **Completion evidence:** source-unit roles grant trust only to the implicit
  prelude and physical configured standard-library units; root `gti_internal`
  declarations, direct references, and alias targets in application source
  produce `GTI-S2058`. Private owner/storage/text-view types bind through exact
  trusted-prelude class identities, aliases and public-signature publication
  preserve privacy, the backend consumes semantic capability facts, and shared
  completion/hover/definition/semantic-token queries filter application
  presentation. Focused application-forging, trusted-wrapper, source-role, and
  LSP tests pass.
- **Unlocks:** `C-TYPE-01`, atomics, threads, and a later allocator capability.

### C-TYPE-01: Transfer And Sharing Capabilities

- **State/role:** done; `I-CAP-01` and `D-MEM-02` are done; durable concurrency
  foundation.
- **Scope:** Add compiler-owned structural facts equivalent to the accepted
  transfer/share concepts without importing Rust spelling by default. Derive
  them through fields and concrete generic instances. Cover primitives,
  aggregates, classes, interfaces, unique owners, borrowed-state carriers, raw
  pointers, destructors, cleanup-owning resources, and native thread-affine
  handles. Implement the adopted explicit or unsafe opt-in/opt-out rule rather
  than deriving thread affinity from fields. Keep capabilities in semantics
  and HIR so library constraints query one authority.
- **Initial conservative boundary:** references, mutable borrowed-state values,
  raw pointers, and borrowed/reference captures of local storage do not cross a
  thread boundary;
  `unique_ptr<T>` transfers only when `T` does; no type is shareable merely
  because C++ can copy it.
- **Exit gate:** focused generic/nominal/recursive positive and negative tests,
  thread-affine cleanup and opt-in/opt-out cases, deterministic diagnostics,
  and no backend name recognition.
- **Evidence:** `SemanticTypeTraits`, `ClassTypeInfo`, lambda/binding records,
  and concrete HIR class instances retain the independent facts. Public
  `std::transferable`/`std::shareable` concepts query them. Declaration
  attributes implement safe negative policy, interface requirements, and
  unsafe positive assertions; `GTI-S2059`, formatter, Tree-sitter, hover, and
  semantic-token regressions cover the source/tooling boundary. Compiler tests
  cover primitives, arrays, expected alternatives, concrete generics,
  recursive unique ownership, borrowed views, raw pointers, cleanup, native
  handles, interfaces, and callable captures.
- **Unlocks:** atomics and thread APIs.

### C-GLOBAL-01: Global And Static Shared-State Policy

- **State/role:** done; adopted concurrent-profile policy.
- **Scope:** Apply the adopted policy to namespace globals and static class
  fields, including aliases, generic instances, native wrappers, cleanup-owning
  types, and internal linkage. Current semantics permits mutable globals, so
  diagnostics and migration notes must make any new restriction explicit.
- **Non-goals:** public threads, atomics, mutexes, a target `threads`
  capability, or a second initialization mechanism. D-EXEC-01's single
  program-initialization contract remains normative; M-LIFE/M-EXEC and the
  matching backend migration still own its executable schedule and the
  general single-threaded cleanup-owning-global restriction.
- **Exit gate:** safe source cannot create unsynchronized shared mutable global
  state; immutable/shareable values and approved synchronization wrappers
  follow one initialization-before-entry rule.
- **Evidence:** `TargetInfo::executionProfile` is selected explicitly by
  direct `--execution-profile` or a project profile's `execution-profile`
  field before frontend entry and is retained in `SemanticModel`, HIR, and
  MIR. `GTI-S2060` rejects mutable or non-share-capable namespace globals and
  static fields only in the concurrent profile, with nominal opt-out,
  cleanup, base, stored-reference, and field causes when available. Compiler,
  project-model, direct-CLI, and project-CLI tests cover aliases, concrete
  generics, raw/borrowed state, native-like opt-outs, cleanup, internal
  linkage, default compatibility, and explicit overrides.
  Concurrent selection creates no alternate initializer path; accepted values
  remain subject to the one D-EXEC-01 initialization-before-entry contract.
- **Unlocks:** the global-policy prerequisites of `C-ATOM-01` and
  `C-THREAD-01`; their MIR, runtime, lifetime, failure, callable, and backend
  prerequisites remain.

### C-MIR-01: Synchronization Operations And Effects

- **State/role:** done in 0.133.0; systems-readiness concurrency foundation.
- **Scope:** Represent atomic and synchronization operation categories in HIR
  and MIR, including the ordering dimensions selected by the memory-model
  decision. Extend the existing conservative operation-effect table first.
  `O-MIR-02` remains the sole owner of per-function summary computation and
  consumes these facts whenever that row is done. Unknown/native/user-code
  calls remain synchronizing until proven otherwise.
- **Non-goals:** reordering optimizations, LLVM IR atomics, or weakening the
  current conservative effect table.
- **Exit gate:** exhaustive enum/effect tables, HIR/MIR verifier tests, and
  optimizer tests proving synchronization operations are never speculated,
  removed, or reordered. Summary propagation tests remain owned by `O-MIR-02`.
- **Evidence:** `SynchronizationOperation` is carried from HIR calls into MIR
  v14 with thread spawn/join, atomic load/store/RMW/compare-exchange, and mutex
  lock/unlock identities. The MIR verifier enforces operation-specific order
  legality and rejects represented operations under the single-threaded
  profile. The completed public thread slice later added the narrow dormant
  trusted-wrapper exception required by the complete source-library inventory;
  it accepts only exact private thread intrinsics and never a task adapter.
  Exhaustive names/effects and mutation tests cover propagation,
  malformed orders, misplaced metadata, profile rejection, and optimizer
  barriers. No public wrapper or backend lowering was introduced.

### C-ATOM-01: Sequentially Consistent Atomic Scalars

- **State/role:** done; `C-MIR-01`, `C-GLOBAL-01`, and the target thread
  capability substrate in `C-RUNTIME-01` are complete. Exact backend lowering
  and the public source-defined wrapper now close the systems-readiness slice.
- **Scope:** Introduce an ordinary GTI standard-library atomic wrapper over a
  closed scalar allowlist with construction, load, store, exchange, and one
  compare-exchange shape. The first executable contract is sequentially
  consistent and makes no lock-free or stable-layout promise.
- **Non-goals:** relaxed/acquire/release spelling, atomic references,
  wait/notify, floating atomics, or arbitrary trivially-copyable classes.
- **Exit gate:** semantic type/capability tests, exact HIR/MIR operations,
  native multithreaded runtime tests through a controlled harness, TSAN where
  available, and optimizer barrier tests.
- **Unlocks:** `C-ORDER-01` and safe synchronized shared-state building blocks.
- **Completion evidence:** `<std/atomic>` provides move-only
  `std::atomic<T>` for `bool` and every fixed-width signed/unsigned integer,
  with construction, load, store, exchange, and strong compare-exchange. Exact
  declaration identity selects the compiler-private storage capability and
  intrinsics; semantic analysis, HIR, MIR verification, representation
  sealing, and the C++ backend all validate the operation and SC order. The
  `atomic_semantic_ir`, `atomic_end_to_end`, `atomic_lsp`, `atomic_tsan`,
  optimizer, corpus, standard-library surface, and installed nightly/release
  checks cover domain rejection, private forgery, target rejection, strong-CAS
  observed values, modification order, a deterministic SC store-buffer
  litmus, O0/O3, C++20/C++23, and ThreadSanitizer where supported.

### C-CALL-01: Thread-Task Transfer Contract

- **State/role:** initial exact `void()` task slice done in 0.309.0; exact
  owned-result transport completed in 0.313.0.
- **Scope:** Bind the GTI-owned callable representation to one consumed task
  shape. Prove transfer/share capabilities for the callable and every capture,
  record exactly-one invocation and task-entry metadata in HIR/MIR, and define
  return/failure behavior without adding a second callable representation. The
  implemented shapes are consumed `void()` task objects and exact
  context-required `Result()` task objects supported by the existing callable
  foundation; general callable storage and broader capture ergonomics do not
  block them.
- **Non-goals:** general callable storage, additional capture syntax, type
  erasure, implicit reference capture, coroutine frames, or native callback
  ABI; those remain owned by `L-CALL-01` or `S-CALL-01`.
- **Exit gate:** accepted task values transfer and drop exactly once; borrowed
  or non-transferable captures cannot enter a task; generic invocation retains
  exact calls and conservative effects.
- **Completion evidence:** `CallableOwnedTransportKind::ThreadTask` extends the
  existing owned-callable contract only for an exact private spawn identity.
  Fresh or explicitly moved exact `void()` and context-required `Result()`
  lambdas and nominal callables are accepted; borrowed, raw-pointer,
  non-transferable, mismatched-result, parameterized, copied named, and
  repeated-use tasks fail with `GTI-S2084` or ordinary move diagnostics.
  HIR/MIR retain one exact once-invocation adapter, exact result type, and
  optimizer synchronization barrier per spawn.

### C-THREAD-01: Joined Owned Thread With Owned Arguments

- **State/role:** initial source-defined owned thread slice done in 0.309.0;
  public `std::thread` spelling and `std::thread_result<Result>` completed in
  0.313.0.
- **Scope:** Make ordinary source-defined `std::thread` GTI's normal public
  managed-thread model. Move a transfer-capable callable and arguments into the
  thread, define join result/failure, and make handle destruction automatically
  join under the adopted contract. Do not add a separate C++-style
  detach-capable `std::thread` or a detach operation. References cannot cross
  unless a later scoped-borrow proof is designed.
- **Exit gate:** move-only argument transfer, rejection of borrow/raw/global
  violations, join cleanup on every normal edge, original-record re-raise
  distinct from recoverable native join error, runtime stress, and sanitizer
  coverage. Automatic join has the same re-raise behavior as explicit join.
  Initialization sequenced before spawn must happen-before task entry, and task
  completion must happen-before a successful join.
- **Unlocks:** owned thread convenience APIs and later scoped borrowing.
- **Completion evidence:** `<std/thread>` owns move-only handle policy,
  recoverable `start`/`join`, `joinable`, destructor autojoin, and the typed
  `std::thread_result<Result>` owner in ordinary GTI source. Exact
  HIR/MIR/Lowered task adapters and verified-MIR C++ lowering preserve
  spawn/join synchronization, once invocation, exact result transport,
  explicit result take or automatic abandon, creation cleanup, reverse worker
  cleanup, exact ordinary failure re-raise, and `GTI-R0014`. Direct/project and
  C++20/C++23 O0/O3 matrices cover noncapturing lambdas, owned move capture,
  nominal callables, handle movement, typed named and closure tasks,
  explicit/automatic join, deterministic result cleanup and failure output,
  and repeated spawn/join stress.

### C-RUNTIME-01: Target And Runtime Thread Capability

- **State/role:** done for the shared thread runtime capability; managed
  threads, atomics, and mutexes retain their own operation rows.
- **Scope:** Add an explicit target/runtime `threads` capability, private
  owning handle, generated task-entry thunk contract, thread-safe runtime
  diagnostics/allocation, fixed failure-record handoff, platform linkage, and
  early unsupported-target diagnostics. Classify existing output, file,
  socket, native-handle, and other
  hosted services for thread safety, output interleaving, handle transfer, and
  synchronization effects before public tasks can invoke them.
- **Non-goals:** public C function pointers, detach, or importing pthread types
  into GTI source.
- **Exit gate:** installed-toolchain runtime/library smoke tests pass on every
  platform that advertises the capability, captured worker records survive
  handoff without allocation or native unwinding, and unsupported targets fail
  before backend invocation.
- **Completion evidence:** `TargetInfo::RuntimeCapabilities::threads`, the
  concurrent profile check, identity-bound private handle operations, the
  version-one opaque C runtime ABI, native platform thread linkage, and exact
  ordinary/emergency worker record handoff are implemented. Runtime fault seams
  prove creation cleanup exactly once and recoverable join-state preservation;
  stdout calls are serialized, task capture never observes/reports on the
  worker, and installed nightly/release workflows run the public end-to-end
  matrix.

### C-SYNC-01: Mutex And Guard-Tied Mutable Access

- **State/role:** done; `C-THREAD-01` and the bounded `M-OWN-03` owner-tied
  carrier are complete.
- **Scope:** Model a mutex guard as an owner-tied capability whose lifetime
  gates mutable access. Integrate failure cleanup according to `D-FAIL-01`.
- **Non-goals:** poisoning, public manual unlock, recursive/read-write mutexes,
  fairness, or condition-variable wait/reacquire.
- **Exit gate:** unlock occurs exactly once on every edge; protected access
  cannot escape the guard; two threads pass race/stress tests.
- **Completion evidence:** source-defined `std::mutex<T>` and move-only,
  non-transferable `std::mutex_guard<T>` use sealed private storage/token
  capabilities. Semantic owner dependencies exclude relock/owner move and
  prevent temporary/local guard loans from escaping. HIR/MIR retain exact
  lock/unlock identities, guard-read loan origin, move transfer, and reverse
  cleanup. `mutex_semantic_ir` includes verifier mutations;
  `mutex_end_to_end` covers C++20/C++23 at O0/O3, move-only payloads,
  contention, and unlock-before-failure-propagation; `mutex_tsan` covers the
  shared counter on supported hosts. No poisoning is introduced: deterministic
  cleanup preserves the primary GTI failure and permits a later lock.

### C-ORDER-01: Explicit Atomic Memory Orders

- **State/role:** done; the validated `C-ATOM-01` client is complete.
- **Scope:** Add only the orderings accepted by `D-MEM-02`, with operation-
  specific legality and happens-before tests. Keep sequential consistency the
  simple default.
- **Exit gate:** every accepted load/store/RMW/CAS order has semantic and MIR
  legality coverage plus deterministic message-passing/litmus tests; forbidden
  combinations fail before backend invocation.
- **Completion evidence:** distinct source-defined
  `std::memory_order_relaxed`, `memory_order_acquire`,
  `memory_order_release`, `memory_order_acq_rel`, and `memory_order_seq_cst`
  tag types make operation legality an overload property rather than a runtime
  enum branch. Default operations remain SC. One-order strong CAS derives its
  standard failure order; the two-order form exposes exactly nine legal pairs.
  `atomic_semantic_ir` proves the complete HIR/MIR matrix and forbidden source
  combinations, optimizer and backend checks preserve/map exact metadata, and
  `atomic_end_to_end` proves release/acquire publication plus every order at
  O0/O3 under C++20/C++23. `atomic_tsan`, LSP, stdlib surface, corpus, and
  installed nightly/release checks cover the remaining boundaries.

### C-CONFORM-01: Concurrency Conformance Gate

- **State/role:** complete for public `std::thread`, ordered scalar atomics,
  and scoped mutex guards; active again when scoped threads or another public
  concurrency family is added.
- **Scope:** Add litmus tests, stress tests, TSAN jobs where supported,
  optimizer-ordering tests, native-entry tests, and documentation examples.
  No timing-sensitive test may be the sole correctness oracle.
- **Exit gate:** every public concurrency operation selected by `D-MEM-02` has
  deterministic semantic/IR tests, O0/O3 runtime equivalence, installed-
  toolchain coverage on each advertised target, sanitizer coverage where
  supported, and no unexplained reliance on native C++ ordering.
- **Current evidence:** `managed_thread_semantic_ir`,
  `managed_thread_runtime`, `managed_thread_end_to_end`, `atomic_semantic_ir`,
  `atomic_end_to_end`, `atomic_lsp`, `atomic_tsan`, `mutex_semantic_ir`,
  `mutex_end_to_end`, `mutex_tsan`, `lsp_protocol`, the stdlib surface/corpus
  gates, installed workflows, and sanitizer runs cover the
  shipped thread and ordered scalar-atomic surfaces. Timing is never a correctness
  oracle.

The following remain blocked beyond the initial concurrency profile: detached
threads; scoped borrowed captures; recursive/read-write locks; condition
variables before guard release/reacquisition is modeled; cross-thread shared
ownership before shared/weak ownership is specified; pointer atomics before
public pointer provenance; native callbacks before `S-CALL-01`; volatile/MMIO,
signals, coroutines, lock-free guarantees, atomic aggregates, and reclamation
schemes.

## Phase L: Owner-Dependent Library And Language Completion

These rows validate the execution/lifetime foundations in ordinary GTI. Public
containers and algorithms remain source-defined and must not gain compiler
name recognition.

### L-INIT-01: Contextual Fixed-Array Arguments

- **State/role:** done in 0.130.0; bounded generic initialization capability
  for ordinary APIs and container clients.
- **Implemented scope:** A brace argument to a named function, method, or
  constructor initializes one exact by-value `T[N]`. An inferred-only
  `uint64_t` function, method, or constructor value parameter may name that
  complete extent. Exact type/value arguments survive semantic selection and
  concrete HIR identity; the backend emits an explicit fixed-array value and
  never delegates list-overload selection to C++.
  `std::vector<int>({1, 2, 3})` is the first public client, not a
  compiler-recognized special case.
- **Non-goals:** `std::initializer_list`, common-type deduction, CTAD,
  list-preferred overloads, explicit callable value arguments, generic
  `operator()` brace context, or broader value-generic expressions.
- **Exit evidence:** generic functions, methods, and constructors,
  alpha-equivalent duplicate rejection, exact/ambiguous overloads, HIR/MIR
  identities, formatter, Tree-sitter, LSP hover, C++20/C++23 runtime, stdlib,
  and example coverage pass.

### L-RANGE-01: Dedicated Range And Element Loans

- **State/role:** done in 0.299.0; systems-readiness capability for
  container/game workloads.
- **Implemented scope:** Semantics records an exact read-only range plan for
  fixed-array indexed traversal and the existing nominal structural protocol.
  HIR and MIR retain the owner, range carrier, iterator/sentinel, per-iteration
  element place, access mode, selected operations, loans, loop edges, array
  extent, and owned-temporary/drop identity. Addressable ranges are borrowed;
  prvalues transfer into loop storage without copying a move-only owner.
  Element loans end before increment and every exit. MIR verifies normal,
  `continue`, `break`, `return`, nested-loop, and defined-failure cleanup before
  backend admission.
- **Non-goals:** pointer decay, generic view pipelines, parallel iteration, or
  mutable container iteration in the same slice.
- **Exit evidence:** fixed-array value/read-only and owned temporary range tests
  cover empty/non-empty, nested loops, `break`, `continue`, `return`, move-only
  owners, mutation/escape/reuse rejection, forged HIR/MIR rejection,
  C++20/C++23 O0/O3 runtime cleanup, and LSP publication/clearing without
  backend lifetime extension.
- **Unlocks:** mutable iteration and range invalidation.

### L-RANGE-02: Mutable Iteration And Invalidation

- **State/role:** done in 0.300.0; systems-readiness capability for in-place
  container/game workloads.
- **Implemented scope:** `mut T&` and `mut auto&` range declarations take one
  exclusive parent loan over a mutable fixed array, mutable nominal lvalue, or
  owned temporary. Fixed arrays use the existing indexed plan. Nominal ranges
  select exact `begin_mut()` plus `end()`, comparison, mutable dereference, and
  prefix increment; the compiler-generated iterator is the confined carrier
  of that parent. Each iteration creates one mutable child and ends descendants
  child-first before increment and on `continue`, `break`, `return`, normal
  cleanup, and failure cleanup. `std::vector` supplies the first mutable
  checked iterator. Whole-owner conflicts reject reserve, push, clear, moves,
  overlapping reads/traversals, and other invalidating access while the plan is
  active.
- **Non-goals:** public container invalidation traits, simultaneous disjoint
  mutable element traversal, user-retainable mutable stored iterators,
  iterator-position insertion/erasure, pointer ranges, or dynamic views.
- **Exit evidence:** focused semantics/HIR/MIR tests cover exact parent/child
  identities, immutable owners, missing mutable endpoints, overlap, mutation,
  escape, owner reuse, forged plans, and child-first endings. Runtime coverage
  exercises fixed arrays, `std::vector`, empty and owned temporary ranges,
  nested loops, `continue`, `break`, and `return` at O0/O3 under C++20/C++23,
  including exact normal/failure temporary cleanup. Formatter, C++ emission,
  LSP publication/clearing, and incomplete-source recovery are pinned.
- **Unlocks:** `L-RANGE-03` spans and dynamic views.

### L-RANGE-03: Spans And Dynamic Views

- **State/role:** completed for the bounded initial release; prerequisite
  `L-RANGE-02` is done; systems-readiness implementation.
- **Implemented scope:** Source-defined read-only `std::span<T>` values come
  from `std::vector<T>::as_span()` and `std::string::as_span()`, support checked
  access, iteration, subviews, empty ranges, local copies/moves, and exact
  factory/relay calls, and never expose a pointer-length pair. Dynamic
  `std::string_view` values come from `std::string::view()` while literal views
  remain ownerless static-storage values. Semantics, HIR, and verified MIR
  preserve each retained value's one read-only owner dependency through calls,
  returns, and drops; backend emission consumes that verified representation.
- **Safety boundary:** owner invalidation, movement, destruction, and local
  owner escape reject while a view is live. Dynamic text views also reject
  reassignment/reseating and storage in globals, fields, fixed arrays, or
  dynamic storage.
- **Deliberately deferred:** mutable views, multiple owners, nested or general
  stored dependency graphs, arbitrary pointer ranges, and unchecked public
  pointer-length construction.
- **Exit evidence:** focused semantics/HIR/MIR tests cover retained dependency
  summaries, invalidation, movement, escape, and nested-storage rejection.
  Runtime coverage exercises complete/sub/empty spans plus dynamic text-view
  copies, relays, checked access, drop, and owner reuse at O0/O3 under
  C++20/C++23. Focused LSP coverage publishes and clears the same diagnostics
  and exposes the source-defined `string::view` contract.

### L-CALL-01: Foundational Owned Callables And Captures

- **State/role:** in progress; `D-CALL-01`, `M-LIFE-01`, explicit confined
  boundary records, exact context-supplied value results, repeatable read/mut
  invocation, consuming once-callable cardinality, and ordered local
  copy/move closure environments are done. Exact same-type generic return and
  the bounded one-field generic owner are also done. A repeatable confined
  callable may now carry an independent exact-return transport when every
  reachable return moves that same parameter; this serves source-defined
  `std::for_each`. Broader public callable/predicate capabilities and external
  owners remain
  systems-readiness implementation shared by algorithms, tasks, and callbacks.
- **Scope:** Implement the accepted callable contract for exact return types,
  read/mut/once invocation capability, owned storage, and explicit move capture
  one independently tested sub-slice at a time. Keep callable identity,
  captures, movement, destruction, and invocation in GTI-owned
  semantic/HIR/MIR records. The completed confined slices already serve
  operation-based `std::accumulate`/`std::inner_product` and unary
  `std::transform_reduce`, plus `std::for_each`, including stateful mutable
  operation objects, without public-name recognition.
- **Non-goals:** immediately cloning `std::function`, arbitrary reference
  capture, thread transfer, native callback ABI, or hiding ownership in type
  erasure.
- **Exit gate:** the supported callable shapes move/drop exactly once, reject
  escaping borrowed captures, preserve exact generic calls/effects, and serve
  as ordinary GTI inputs to the first foundational range algorithms.

### L-RANGE-04: Foundational Range Algorithms

- **State/role:** substantially implemented; `L-RANGE-03` is complete. The
  implemented iterator/sentinel slices include
  multi-parameter concepts, validity-only trailing `requires`, exact
  input-iterator/sentinel/dereference/accumulator capabilities, distinct
  copy-construction capability, `std::forward_iterator`, source-defined
  `std::binary_search`, default and operation-based `std::accumulate`,
  homogeneous `std::inner_product`, unary `std::transform_reduce`, the
  confined predicate algorithms, exact iterator/sentinel `std::find` and
  `std::count`, and repeatable exact-return `std::for_each`. The
  writable/movement capability family is also implemented:
  `std::output_iterator`/`writes_to` (mut dereference plus increment),
  `subscripts_to_mut` (positioned mutable subscript),
  `bidirectional_iterator` (`--`), `random_access_iterator` (`+=`/`-=`), and
  `sized_sentinel_for` (iterator-to-sentinel distance) — with symbolic
  generic-body forms for dereference/subscript/compound assignment, concrete
  satisfaction checks, read-vector-cursor operator coverage, and the
  source-defined `std::copy`, whole-container `std::fill`/`std::transform`/
  `std::reverse`/`std::sort`, signed `std::advance`/`std::next`, and
  `std::prev` clients.
- **Remaining scope:** comparator `sort`; complete-range concepts extending
  the whole-container generic algorithms beyond `std::vector`; indexed
  element move-out; and further iterator/sentinel overloads behind a
  demonstrated subrange client. Iterator-pair in-place spellings remain
  excluded because `begin_mut()` and `end()` from one container overlap by
  design; semantics now diagnoses that conflict before MIR, while read-only
  methods on a parameter-held mutable cursor are supported. Iterator
  assignment, implicit conversions, overload ranking, and a parallel C++
  iterator-category hierarchy remain unauthorized.
- **Exit gate:** no public algorithm name is recognized by the compiler and
  value/borrow/move behavior is covered for every supported range category.

### L-NUM-01: Defined Integer Arithmetic Modes

- **State/role:** done in 0.111.0; systems-readiness arithmetic capability for
  renderer/game and low-level systems clients.
- **Implemented scope:** `<std/numeric>` provides exact overloads of
  `wrapping_add/sub/mul` and `saturating_add/sub/mul` for all eight fixed-width
  integer domains, plus `checked_add/sub/mul` returning
  `expected<T, std::arithmetic_errc>`. All nine public operations are
  non-failing, constexpr-capable within the documented observer subset, retain
  exact semantic/HIR/MIR identities, use the shared private `APInt` authority,
  and have explicit memory-free/non-trapping effects. O0/O3 and C++20/C++23
  execute the same public example. Ordinary operators remain checked and
  failure-producing.
- **Later breadth:** checked division/remainder/shift result families require a
  demonstrated client and exact zero-divisor/invalid-shift error policy;
  constant `error()` observation can widen with the general aggregate
  evaluator. An `unsafe` block never globally disables checks, and native
  overflow flags never become language semantics.
- **Exit evidence:** exhaustive fixed-width boundaries, expected success/error
  construction, bounded constexpr observation and failed-`value()` diagnostic,
  HIR/MIR identity/effects, LSP source identity, and O0/O3 × C++20/C++23 runtime
  parity pass.

### L-FLOAT-01: Binary64

- **State/role:** done in 0.110.0; systems-readiness numeric capability.
- **Scope:** `double` is exact IEEE-754 binary64; `d`/`D` selects it while
  unsuffixed decimals retain binary32 compatibility. GTI-owned width-tagged
  bits cross tokens, semantics, HIR, and MIR. Private `APFloat` owns parsing,
  arithmetic, comparisons, and conversions. Mixed arithmetic promotes to
  binary64, widening is implicit, narrowing is explicit, and the native
  policy verifies both IEEE host representations. TypeContext and vector/SIMD
  work remain separate.
- **Exit evidence:** the binary32 semantic/evaluator/MIR/backend/native matrix
  has binary64 parity across O0/O3 and C++20/C++23; formatter, Tree-sitter,
  LSP, layout, generic numeric, diagnostics, and shipped-example gates pass.

### L-SUM-01: Payload Enums And Exhaustive Matching

- **State/role:** in progress; prerequisites `D-LANG-01`, `M-OWN-02`,
  `M-LIFE-01`, and `S-LAYOUT-01` are done. The first passive, non-generic
  payload family is implemented across syntax, semantics, HIR, MIR, C++20/23,
  formatter, Tree-sitter, and LSP tooling. The cleanup-free class-payload
  family is also implemented: ordinary class values without active cleanup or
  borrowed state live inside alternatives, constructed from prvalues, named
  copies, or explicit moves (each an exact fused emission vocabulary), with
  copied class bindings armed and dropped like ordinary locals, payload-enum
  struct definitions scheduled after every class definition, and enum
  copy/move/ownership traits folded from payload components. Active-cleanup
  or borrowed-state payload constructions are rejected at the construction
  site with `GTI-S2067`.
- **Implemented slice:** Nominal payload cases construct by exact case calls.
  `switch` accepts qualified case patterns, creates immutable copied bindings,
  and requires a `default` or every variant. Semantic variant identity and
  exhaustiveness survive through HIR/MIR; the C++ backend uses a replaceable
  `std::variant` wrapper without making that library type source semantics.
- **Remaining scope:** Add variant-aware drop transport for owning
  (active-cleanup) payloads, ownership-aware move/borrow matching, partial
  initialization and drop, generic payloads, deterministic layout, and
  interleaved enum/class definition scheduling (which would also lift the
  construction-site-only rejection) in separately proven families.
- **Non-goals:** Implicit conversions, open variants, making `std::variant`
  compiler magic, or treating the separately implemented passive native union
  as a tagged sum or C ABI union.
- **Exit gate:** an ownership-capable generic payload family has exact
  partial-initialization/drop state, move/borrow and positive/negative tests,
  deterministic layout facts, and O0/O3 runtime parity. The passive first slice
  already has exact construction, exhaustive diagnostics, and runtime parity.

### L-ERR-01: Error Propagation Operator

- **State/role:** complete in 0.311.0; `M-LIFE-01`, `D-LANG-01`, and
  `D-EXEC-01` provide the cleanup and evaluation substrate.
- **Scope:** Define one explicit propagation form over `expected`, including
  conversion prohibition, early-return cleanup, source spans, HIR/MIR control
  flow, formatter, Tree-sitter, and LSP behavior.
- **Implemented slice:** Prefix `?expr` evaluates one `expected<T, E>` once,
  produces `T`/`void` on success, and returns `unexpected(E)` on failure only
  from an enclosing `expected<U, E>` callable. Named places copy; prvalues and
  explicit `std::move` operands move. HIR retains the selected extraction and
  transport plan; `mir-v52`/`mir-body-v46` carry explicit mutually exclusive
  early full-expression endpoints, cleanup, and return control flow; C++20 and
  C++23 spell the same verified behavior.
- **Completion evidence:** Positive and negative compiler tests cover exact
  types, invalid contexts, copy/move selection, lambdas, multiple ordered
  propagations, formatter stability, MIR verification, and both backend
  spellings. The runtime matrix covers scalar, `void`, owning temporary, named
  move-only, named copyable, and move-only error alternatives at C++20/C++23
  and O0/O3; Tree-sitter and LSP fixtures cover parsing, highlighting,
  formatting, and the `GTI-S2086` source range.
- **Exit gate (met):** success and propagation paths preserve exact
  result/error types, drop every live obligation once, retain source-facing
  spans through HIR/MIR, and pass formatter/parser/LSP/runtime plus full
  repository validation.

### L-OP-01: Broader Operators And Capability Policy

- **State/role:** first family implemented; the demonstrated client is
  renderer/math domain values (`vec2`/`vec3`-shaped types and a second
  integer-scalar domain type) driving one generic body.
- **Implemented family:** four structural capabilities — `adds<T>`,
  `subtracts<T>`, `negates<T>`, and `scales_by<T, Scalar>` — each proving one
  exact public read-only member returning an *owned* value of the domain type.
  They are spelled publicly as `std::addable`, `std::subtractable`,
  `std::negatable`, and `std::scalable_by`, composing into
  `std::vector_space<T, Scalar>` with `std::copyable<T>`. Generic bodies gain
  the matching symbolic candidates for binary `+`/`-`, unary `-`, and scalar
  `*` on a constrained type parameter; concrete instances re-select the real
  member declarations. `std::lerp` and `std::negated` in `<std/numeric>` are
  the source-defined clients, with exact `float`/`double` overloads of
  `std::lerp` for built-ins.
- **Deliberate boundary:** the family is member-only, matching the language's
  member-only user-operator rule. A built-in numeric does not satisfy it —
  admitting one would resolve its operators as symbolic member calls and drop
  the canonical local failure outcomes checked integer arithmetic must retain
  (observed directly while implementing this row). A borrowed operator result
  is also excluded, since the produced value would be tied to a receiver whose
  lifetime the requirement cannot name.
- **Remaining scope:** compound domain operators (`+=` and friends), a
  matrix/product family with distinct operand and result domains, and any
  ordering or hashing capability for domain values are separate families under
  the same one-family-at-a-time rule.
- **Exit gate (met for this family):** one lookup rule (exact member-only
  selection, no conversion), focused positive and negative diagnostics for each
  capability, and O0/O3 × C++20/C++23 runtime evidence over two domain types.
  No native C++ overload behavior is needed to explain accepted source.

### L-OWN-01: Shared And Weak Ownership

- **State/role:** blocked on `M-FAIL-01`; `M-LIFE-01`, `D-MEM-02`, and
  `I-CAP-01` are done; systems-readiness standard-library work.
- **Scope:** Implement source-defined `shared_ptr` and `weak_ptr` policy over
  narrow trusted control-block operations, and provide the selected
  recoverable unique/shared allocation factories. Make copy, move, observation,
  locking, drop, cycle limitations, and allocation failure explicit.
- **Initial boundary:** the first implementation is single-threaded. Making the
  control block cross-thread shareable is a distinct follow-up blocked on the
  adopted memory model and atomic implementation.
- **Exit gate:** exactly-once object/control-block destruction, weak expiration,
  cycles documented as a limitation, allocation failure, and HIR/MIR lifecycle
  tests.

### L-VALUE-01: Optional And Foundational Value Utilities

- **State/role:** blocked on `M-FAIL-01`; `M-LIFE-01` and `I-CAP-01` are
  done; systems-readiness standard-library work.
- **Scope:** Implement `optional<T>` over checked one-slot storage, then
  complete pair, comparison, swap/exchange, limits, and `expected` observers
  without compiler-recognized public names.
- **Exit gate:** empty/engaged construction, move-only values, assignment,
  destruction, failure, generic constraints, and constexpr-safe constants.

### L-CONST-01: Remaining Bounded Compile-Time Programming

- **State/role:** the bounded aggregate initial release is implemented for the
  ordinary `std::array<T, N>` constructor/observer and `std::gcd<T>` clients.
  Default generic arguments remain a separate final slice requiring
  declaration-owned exact identity.
- **Implemented scope:** The existing evaluator admits concrete non-pack type
  and `uint64_t` value-generic calls, exact scoped enums, concrete fixed arrays,
  fully initialized structural class/struct values, ordinary constexpr
  constructors, and read-only instance methods. Constants retain exact
  declaration/type/value identity through semantics and required HIR/MIR data;
  the backend only materializes an already-proven value. One shared 4096-step
  budget and 64-call-depth limit cover recursive/default aggregate work.
- **Remaining scope:** Default generic arguments are a separate final slice.
  References, allocation, aggregate-place mutation, payload enums, unions,
  inherited or cleanup-owning aggregates, generic packs, constexpr operators,
  mutable receivers, and polymorphism remain excluded. Do not widen exact
  qualified specialization into partial specialization or introduce a
  bytecode interpreter.
- **Initial exit gate:** met by positive/negative identity and limit tests plus
  an O0/O3 and C++20/C++23 runtime-parity matrix whose result is independent of
  native C++ constant evaluation.

### L-TEXT-01: Formatting, Parsing, And Text Policy

- **State/role:** in progress. Canonical base-10 conversion and direct stdout
  output are implemented for every fixed-width integer. Bounded sequential
  `{}` replacement for heterogeneous integral packs is also implemented in
  ordinary GTI through `std::format`, `std::try_print`, and
  `std::try_println`; malformed patterns and argument-count mismatches return
  `format_errc` before output. The source-text contract sub-slice is ready.
  This integral replacement slice is no longer blocked on `L-RANGE-03` or the
  remaining `L-CALL-01` work: indexed counted text, a bounded call-pack fold,
  and scalar byte output are sufficient. Dynamic borrowed views, generalized
  formatter customization, and range-oriented text algorithms still take
  their applicable range/callable prerequisites rather than being inferred
  from this fold. All are systems-readiness work. Each implemented float width
  joins formatting/parsing in its own numeric sub-slice rather than blocking
  the whole text layer.
- **Scope:** First specify source encoding, BOM/newline handling, Unicode
  identifiers/normalization, and source-offset behavior across the lexer,
  formatter, Tree-sitter, and LSP. Then state the public byte/UTF-8 policy,
  owning/view conversions, numeric parsing, formatting, and explicit failure.
  Prefer ordinary GTI algorithms and narrow host calls over exposing C buffers.
  The current formatter recognizes sequential `{}` plus `{{`/`}}`, accepts
  integral arguments only, and validates before stdout; indexing, names,
  specifiers, floats, and customization remain later breadth. Stderr remains a
  hosted service owned by `L-HOST-01`.
- **Exit gate:** the selected byte/UTF-8 policy is normative; owning/view and
  numeric round trips cover valid, invalid, boundary, and allocation-failure
  cases; APIs expose no native buffer lifetime and behave identically at O0/O3.

### L-HOST-01: Remaining Hosted Services

- **State/role:** blocked; `D-FAIL-01` is done; remaining prerequisites are
  `L-RANGE-03` and any bounded FFI record actually required by the selected
  service; bounded-first systems-readiness library work.
- **Scope:** The initial systems-ready minimum covers file writes/seeking, monotonic and wall-clock
  time, deterministic PRNG plus nondeterministic seed, process
  arguments/environment, and stderr, one service family per prompt.
  Recoverable failures return `expected`; public APIs expose no backend handles
  or raw C buffers. Broader filesystem traversal/watch, connected networking,
  and traffic buffers are client-driven sub-slices after a demonstrated need or
  their native-record and borrowed-buffer contracts exist.
- **Exit gate:** each selected service family has a target capability, exact
  ownership/failure contract, installed-toolchain runtime test, unsupported-
  target diagnostic, and no public backend handle or raw-buffer lifetime.

### L-CONT-01: Vector And String Completion

- **State/role:** active bounded vector slice; complete range integration still
  depends on `L-RANGE-03`; systems-readiness implementation for the
  renderer/game workload.
- **Scope:** Indexed vector insertion/erasure for move-only elements is
  implemented through checked private slot shifts. Complete iterator-position
  forms and corresponding string operations still require exact invalidation,
  owner-dependency, failure, and range behavior. Range
  sorting/search/partition/copy/move algorithms remain owned by `L-RANGE-04`.
- **Non-goals:** associative containers, allocator customization, or container
  proliferation before the shared protocol and benchmark evidence exist.
- **Exit gate:** insertion/erasure/string operations have exact invalidation
  and failure behavior across move-only and borrowed elements; semantics, MIR,
  runtime, sanitizers, and range integration agree.

### L-CONT-02: Associative Containers

- **State/role:** blocked; ADR 020's exact `std::hash<AppType>` customization
  mechanism is complete, while the remaining prerequisites are `L-CONT-01`,
  the hash/equality/ordering capability contracts, and accepted `S-ALLOC-01`;
  systems-readiness work for one demonstrated associative container.
- **Scope:** Specify exact hasher, equality, ordering, invalidation, allocation,
  and deterministic-iteration contracts before adding one hash or tree map.
- **Exit gate:** generic capability failures are semantic, allocation/failure
  follows the shared contracts, and runtime/benchmark evidence justifies the
  chosen first container.

## Phase O: Optimization And Backend Migration

Optimization may proceed in parallel with language design, but no transformed
operation controls output before its lifetime, failure, and effect contract is
complete.

### O-MIR-01: First Shadow Transform With Its Minimal Editor

- **State/role:** complete and superseded by stable proof-carrying rewrite
  infrastructure; systems-readiness implementation foundation.
- **Scope:** The original shadow slice introduced one constant fold and a
  minimal editor. Its temporary block-index addressing has since been removed;
  current edits use stable `{MirBodyAddress, MirInstructionId}` evidence,
  atomic apply/repair, verification, and explicit analysis invalidation.
- **Non-goals:** a general pass manager with no transforms, incremental
  dominance, `LoopInfo`, or new optimizations justified only by LLVM
  availability.
- **Exit gate:** shadow results match the current compatibility fold; malformed
  patches fail verification; deterministic dumps and effect/failure tests pass.
  Production use of the fixed-width-integer transformed result landed through
  `M-BACK-01`/`scalar-leaf-v1` plus eligible M-BACK-02 `scalar-cfg-v1` and
  `scalar-direct-call-v1` bodies; broader shadow-fold coverage was not their
  prerequisite.
- **Evidence:** the compatibility fold now uses stable
  `{MirBodyAddress, MirInstructionId}` identity and typed transformation
  records. Independent source-MIR replay validates every replacement before
  `LoweredProgram` publication. Primitive integer, binary32/binary64,
  character, boolean, and null grouping identities fold at `-O1+`; strings,
  trapping operations, and dynamic values remain conservative near-misses.
  O0 is byte-identical and malformed/stale/duplicate records fail closed.

### O-MIR-02: Per-Instance Effects

- **State/role:** complete foundation; broader effect precision is
  client-gated.
- **Implemented:** every concrete function, constructor, destructor, and
  lambda carries a verified summary for place/unknown-memory access,
  allocation, runtime and user-code invocation, traps, moves, drops, loans,
  synchronization, and receiver/argument escape. A deterministic call-graph
  fixed point derives summaries from complete MIR. Exact acyclic static calls
  may refine instruction effects; recursive SCCs, virtual dispatch,
  declarations, native boundaries, and unresolved calls remain conservative.
  Verification rejects under-approximation and permits conservative
  over-approximation. The older defined-failure facts remain their separate
  exact failure contract.
- **Remaining scope:** add precision only with a measured transformation that
  can prove an existing conservative boundary more exactly.
- **Exit gate:** separate-compilation assumptions are not invented; unknown or
  recursive calls remain safe barriers; focused optimizer tests cover each
  effect dimension.

### O-MIR-03: Local CFG/Value Optimization

- **State/role:** bounded production slice complete; SCCP continuation is
  complete in `O-MIR-05`.
- **Implemented:** scalar non-owning block parameters and edge arguments,
  conservative acyclic scalar-place promotion, bounded literal/compute/branch
  folding, unreachable-block cleanup, and dead unused computation removal.
  The pass/analysis manager revisions and conservatively invalidates all
  current analysis families. Current transformations are deterministic, typed,
  independently replayed, and consumed through the one lowered backend input.
- **Scope:** this row is closed; further CFG/value work requires its own
  numbered row, proof records, verifier coverage, and measured client.
- **Non-goals:** inlining, devirtualization, bounds-check removal, or loop
  analysis without a concrete proof/client.
- **Exit gate:** satisfied for the implemented local pipeline by verifier,
  report determinism, native-`-O0` structural tests, and O0/O1/O2/O3
  differential runtime coverage. Apply the same gate to every later pass.

### O-MIR-04: Proof-Carrying Safety Optimization

Executable tracking: [issue #36](https://github.com/LukeHHA/gti/issues/36).

- **State/role:** first production slice complete. O2/O3 run separate bounds,
  initialized-storage-state, and arithmetic-overflow discharge passes over one
  canonical body-local safety analysis; O0/O1 remain unchanged.
- **Implemented scope:** interval and branch-predicate proofs discharge
  in-range fixed-array scalar reads and non-overflowing checked integer
  add/subtract/multiply/negate. Exact sparse-storage state discharges read and
  mutable-read bounds plus initialized-slot outcomes only after a successful
  exact-slot construct not invalidated by destroy or an opaque mutation.
  Discharges retain the checked MIR instruction and failure CFG, name the exact
  local outcome, and carry stable related block/instruction/value/place
  identities. Controlled editing reruns the canonical analysis, coherence
  independently replays every proof from source MIR, and all backends consume
  only the sealed optimized instruction metadata.
- **Remaining scope:** widen one measured operation family at a time. Array
  writes/borrows, prefix storage, division/remainder/shifts/conversions,
  interprocedural range facts, and loop-derived safety proofs remain checked
  and must bring their own exact proof and evidence. Place-alias facts already
  refine mutation invalidation but never discharge a check by themselves.
- **Non-goals:** deleting checks by optimization level, recognizing public
  wrapper names, treating emitted C++ shape as proof, or replacing GTI's
  left-to-right intermediate overflow behavior with a widened final check.
- **Exit gate:** satisfied for the implemented families. Every removed check
  carries a reproducible proof tied to GTI places/ranges/effects and emits a
  deterministic applied optimization remark identifying the operation family,
  body, source site, and proof. Forged or stale proofs fail verification, and
  near-miss cases retain the check. O0/O2/O3 native-`-O0` differential tests
  preserve successful results, failure category and origin, cleanup,
  alias/mutation barriers, and boundary values. Native
  vectorization remarks and assembly may support a performance conclusion but
  never establish semantic validity. A loop-versioned fast path is permitted
  only when its side-effect-free preflight and checked scalar fallback preserve
  failure order, cleanup, and partial effects exactly.

### O-MIR-05: Deterministic Local Dataflow And SCCP

- **State/role:** complete bounded production slice. O2/O3 run SCCP; the
  existing safety analysis uses the shared solver at every optimization level
  where safety facts are requested.
- **Implemented:** one typed monotone forward solver owns deterministic FIFO
  scheduling, canonical terminator-slot executable-edge identity, update
  counts, bounded widening, malformed-CFG and block-argument rejection,
  monotone executable-edge growth, and a hard convergence limit. Safety
  retains its prior interval/storage lattice and exact proof results, including
  switch-default propagation; an independent FIFO oracle and the full safety
  suite cover migration equivalence. SCCP is the first new client and uses
  `unknown -> exact typed constant -> overdefined` facts over scalar non-owning
  SSA. Exact literal/branch edits carry stable evidence and independent
  source-MIR replay.
- **Non-goals:** place or memory propagation, ownership/loan facts, speculative
  trapping operations, unbounded fixpoint iteration, or using native optimizer
  results as evidence. Switch and invoke edge selection remain conservative.
- **Exit gate:** deterministic repeated results, safety-equivalent proofs,
  positive branch-merge propagation, conservative unknown/effect near misses,
  canonical else/default edge identity, malformed CFG/argument/transfer
  rejection, report schema/provenance coverage, and O0/O1 identity all pass.

### O-MIR-06: Natural Loops And Bounded Relational Ranges

- **State/role:** complete analysis foundation; no loop transformation is
  enabled by this row.
- **Implemented:** dominance backedges derive natural loops; same-header
  latches merge; strict containment produces a deterministic loop forest; and
  multi-entry SCCs are marked irreducible. Reducible loops record
  side-effect-free scalar invariants and a bounded single-latch affine
  induction through MIR block parameters with one literal initial value, one
  positive literal add/subtract step, and an optional invariant relational
  continuation bound. A predecessor with repeated edges to the header and
  ambiguous block arguments produces no induction fact.
- **Non-goals:** polyhedral models, unrestricted symbolic algebra, recurrence
  solving across calls or memory, irreducible-loop transforms, LICM,
  vectorization, loop versioning, or mutation of MIR.
- **Exit gate:** nested/merged-loop identity is deterministic; malformed or
  unsupported or repeated-edge-ambiguous recurrences produce no fact;
  irreducible CFGs are marked; and a hand-built block-parameter loop proves the
  exact induction/range record.

### O-MIR-07: Place Alias And Escape Facts

- **State/role:** complete body-local analysis foundation with alias-aware
  safety-state invalidation as its first client.
- **Implemented:** stable MIR places derive `must-alias`, `no-alias`, and
  `may-alias` for precise roots, fields, and constant projections. Verified
  ordinary-record sibling fields are disjoint; union sibling fields overlap.
  Address taking, returns, retained borrows, non-local stores, raw memory, and
  calls produce explicit escape reasons. Closed exact static summaries project
  receiver/argument escape; native, virtual, recursive, and unresolved calls
  remain conservative. Loan, value-rooted, dynamic, raw, and unclassified
  field relationships do not become `no-alias` without stronger MIR identity.
  Duplicate `must-alias` IDs share one safety fact key; writes preserve safety
  facts only for proved `no-alias` places. The analysis manager binds cached
  facts to one MIR program snapshot and rejects cross-snapshot reuse.
- **Non-goals:** interprocedural pointer identity, optimistic raw-pointer
  provenance, public-library-name recognition, MemorySSA, load commoning, or
  store elimination.
- **Exit gate:** relation symmetry and projection cases, raw/dynamic
  and union-overlap conservatism, exact-call escape, unknown-call escape,
  cache invalidation and snapshot isolation, disjoint-place fact preservation,
  and overlapping-place fact invalidation have focused evidence.

### O-PERF-00: Evidence Freeze And Benchmark Admission

- **State/role:** complete at exact clean commit `a2bd29dc`. This row reconciles the exact-main
  O-MIR-08 audit with the independent PR #164 audit before compiler code
  changes.
- **User outcome:** every following optimizer decision is tied to a durable,
  reproducible workload that distinguishes GTI optimization, native
  optimization, representation overhead, and deliberate safety cost.
- **Independently reproduced evidence:** with compiler `gti 0.312.9`
  (`d578914271ab316007cc48042483ec4893b29004eea4fa858ac0cc7ad4427194`)
  and Apple Clang 21 on arm64 Darwin, fresh emitted vector C++ matched SHA-256
  `932cfc1f2fddb44c0e23644cb703caf2947fcb83da3c0615b096a12a0e682938`.
  At fixed native O3, `vector-checked-loop` measured 117.285 ms median and a
  disposable generated-overflow proxy measured 9.519 ms with the same digest,
  a 12.32x ceiling; a stricter rebuild limited to the three MIR checked-helper
  guards confirmed the same order of magnitude. The fixed-array
  overflow probe measured 73.684 ms versus 15.882 ms, a 4.64x ceiling.
  Mandelbrot emitted at GTI O0/O1/O2/O3 and rebuilt at the same native O3
  measured 169.321/169.383/169.622/169.673 ms, establishing no material
  current GTI-pass runtime effect on that client.
- **Interpretation boundary:** an ablated binary preserves only the selected
  successful benchmark result; it deliberately violates GTI behavior for
  failing inputs. It is an attribution/upper-bound proxy, never proof that a
  proposed transform preserves semantics. PR #164's separate-native result is
  accepted; its claim that ordinary O0-to-O1 timing alone attributes gains to
  Clang is not.
- **Closed evidence:** report schema 4 bound every admitted workload to clean
  compiler/source commit `a2bd29dce07260697de916fd2a8aa54a59bb9ef1`
  and compiler SHA-256
  `5ff0d43de820dbd529525f7daac60fecf522d16134c1cf82afdecf8ff4149b82`.
  Five warmups and twenty seeded interleaved samples were conclusive for every
  variant. The complete-state fixed-array client measured 283.568 ms checked
  versus 60.377 ms for its strict three-helper proxy; the scaled vector client
  measured 1235.735 versus 83.189 ms. Separate schema-3 runs emitted at GTI
  O0/O1/O2/O3 and rebuilt at native O3 measured 152.597/152.008/151.073/150.768
  ms, and all four rebuilt executables had the same SHA-256. Optimization
  reports, MIR dumps, profiles, native disassembly, report hashes, and raw
  samples are retained in
  [`performance-tooling.md`](performance-tooling.md).
- **Scope:** admit long-running checked scalar-chain, fixed-array overflow,
  scalar-FP update, checked-vector, fixed-array/bounds control,
  managed-thread control, and staged game-update workloads into the canonical
  harness. Add a runner mode that holds native optimization fixed while GTI
  optimization changes. Record compiler/runtime/source hashes, exact commands,
  median/MAD and raw samples, optimization reports, profiles, disassembly, and
  normalized correctness/failure records. Scale every timing client above its
  declared process-startup floor; the audit's 3-4 ms FP probe is directional
  evidence only.
- **Exit gate:** clean exact-head runs reproduce the two overflow ceilings and
  the fixed-native optimizer comparison through checked-in workload
  descriptors and threshold-free CI smoke tests. The initial `O-GAME-01`
  scalar/container stage has a correctness-equivalent baseline. No compiler
  transform lands in this row.

### O-PERF-01: Loop-Carried Arithmetic Range Proof

- **State/role:** complete at exact clean commit
  `f1a4075f3848d3f24531f12364f227fde29b887d`; first implementation row after
  completed `O-PERF-00`.
- **Corrected starting point:** the canonical safety analysis already runs a
  deterministic loop fixpoint and widens interval facts after repeated block
  updates. It does not need a second solver. The current loss comes from
  widening expanding to the complete integer domain, missing exact
  remainder/mask transfer, and having no alias-safe fixed-array content
  summary. SCCP is a separate exact-constant client and is not interval
  authority.
- **Bounded scope:** add exact GTI remainder and bit-mask interval transfer for
  the admitted sign/domain cases; add one scalar fixed-array-root content
  interval joined on dynamic writes and invalidated on may-alias escape,
  calls, raw memory, lifetime change, or unknown mutation; then add only the
  loop-header threshold widening and bounded narrowing required by that
  client. Reuse `O-MIR-06` loop identity, `O-MIR-07` alias facts, the shared
  solver, the existing arithmetic discharge records, `MirEditor`, and
  independent replay. Source-MIR cyclic scalar promotion is not an entry
  prerequisite unless a focused probe proves the place-based analysis cannot
  carry the required fact.
- **Semantics gates:** each intermediate checked operation remains
  left-to-right. Remainder ranges follow GTI's signed and zero-divisor rules;
  a later modulo cannot excuse an earlier overflowing multiply/add. Array
  summaries never become per-element truth and never survive an unproved
  alias, call, escape, union overlap, or mutation.
- **Exit gate:** the admitted fixed-array overflow workload discharges more
  than 90% of its hot arithmetic outcomes and reaches at most 1.2x idiomatic
  C++ at O2/O3. Near misses for signed ranges, zero divisors, type extrema,
  alias/escape, unknown calls, irreducible loops, widening, and failure paths
  retain checks. O0 identity, exact failure origin/order, cleanup, raw reports,
  and all campaign controls pass with no conclusive regression above 5%. If a
  source-MIR proxy proves this bounded analysis cannot discharge the admitted
  client or cannot approach the runtime gate, close the row as measured-defer
  with that retained evidence rather than adding a broader solver by default.
- **Closed evidence:** the implementation added exact remainder and bit-mask
  interval transfer, a scalar fixed-array-root envelope with alias/effect
  invalidation, and a finite reducible-loop accumulator threshold on the
  existing dataflow solver. The admitted body discharged five of five hot
  arithmetic outcomes and all three hot fixed-array bounds outcomes. Five
  warmups and twenty seeded interleaved runs produced 61.807 ms GTI versus
  61.111 ms idiomatic C++ at O2, and 61.055 versus 61.113 ms at O3. Exact
  signed-minimum remainder, zero-divisor, dynamic-bound, repeated-write,
  alias-write, call-barrier, lifecycle, and runtime-failure tests retain the
  conservative boundary. No exact-head control regressed by more than 0.4%
  relative to its semantic-C++ twin.

### O-PERF-02: Closed-Call And Container Fact Transport

- **State/role:** complete on exact clean implementation commit `d6374eb9`;
  this row owns the first interprocedural performance client and subsumed the
  bounded campaign client of `O-MIR-10`.
- **Corrected client:** `vector-checked-loop` is deterministic and its hot
  values are bounded: initialization yields 1 through 17, the multiplier is
  selected as -1 or 1, and the complete sum remains in range. It is not
  evidence that static analysis is impossible. The missing fact crosses
  closed `scale`, `sum`, vector methods, and the prefix-storage boundary.
- **Bounded scope:** after profiling, select exactly one concrete,
  non-recursive static-call family: budgeted MIR inlining, fact-specialized
  body cloning, or contextual proof summaries. The selected form must carry
  exact argument, return, and admitted mutable-container content facts needed
  by this workload and no broader abstract interpretation. A return interval
  is a distinct revision-keyed interprocedural analysis result, not an effect
  flag; effect summaries continue to describe behavior rather than values.
- **Architecture gates:** reject native, virtual, recursive, open, owning-
  temporary, incomplete-cleanup, or unsupported failure shapes. Remap or key
  every body/value/place/failure identity within one optimized MIR snapshot;
  preserve Invoke edges, source provenance, left-to-right parameter/result
  materialization, loans, partial effects, and cleanup. Do not recognize
  `std::vector`, `scale`, `sum`, or another public spelling.
- **Exit gate:** the selected closed-call family moves the admitted vector
  arithmetic discharge counters and improves median runtime by at least 30%
  beyond the `O-PERF-01` exact-head baseline. Static/recursive/native/virtual,
  cleanup-owning, aliasing, and conflicting-call-context near misses remain
  conservative. Exact source-MIR replay, all backends, failure injection,
  deterministic budgets, and campaign controls pass. If no candidate proxy
  moves runtime, close this row with the retained evidence and proceed to the
  independently attributable prefix-storage row rather than landing
  interprocedural infrastructure.

The selected implementation is a bounded contextual proof-summary analysis,
not inlining or cloning. It transports exact scalar arguments, returns,
mutable receiver/argument objects, and structurally admitted indexed-container
element envelopes through concrete source-defined static calls and
constructors. A whole-program cache invalidates on any semantic body edit;
metadata-only proof attachment explicitly preserves it. Native, virtual,
recursive, owning-transfer, synchronization, unsupported cleanup, conflicting
context, and non-convergent families fail closed. Proof replay batches one
immutable source and candidate snapshot, so the new whole-program authority is
not recomputed per discharge.

At O2, five warmups and twenty seeded interleaved samples produced 558.336 ms
GTI, 715.229 ms semantic C++, and 87.947 ms idiomatic C++. The GTI median is
54.0% below the `O-PERF-01` exact-head vector baseline; normalizing each GTI
run to its semantic twin gives a 62.6% reduction. O3 produced 570.654 ms GTI,
571.371 ms semantic C++, and 57.471 ms idiomatic C++. Both exact-head reports
are conclusive and retain the same complete-state digest. Detailed report
identities, counters, and raw samples live in
[`performance-tooling.md`](performance-tooling.md).

### O-PERF-03: Single-Bound Prefix Storage Access

- **State/role:** complete on exact clean implementation commit `eaaea34d`;
  second-order container closure.
- **Corrected starting point:** current `std::vector` stores `count` while its
  private `prefix_storage` independently stores `length_`, and every public
  subscript performs a vector-detail bounds check followed by the prefix read's
  private bound check. Contrary to PR #164, unchecked read helpers currently
  exist only for sparse `storage<T>`; prefix storage has no unchecked-read
  vocabulary, and the safety analysis does not prove prefix reads.
- **Bounded scope:** establish one logical live-length authority, preferably by
  deriving vector size from prefix storage and removing duplicated `count`
  state if the source-defined API and mutation invariants permit it. Add a
  prefix-read discharge only when MIR proves the exact same index/length fact
  and no intervening length mutation or alias. A backend-only unchecked prefix
  representation helper may be added only as the consumer of that verified
  discharge; it is not a source intrinsic or trusted-library escape hatch.
- **Exit gate:** native evidence shows one logical check per non-discharged
  public access and no private recheck after a verified discharge. Vector and
  prefix lengths cannot drift by construction; append/insert/erase/pop/clear,
  relocation, failure rollback, move, iteration, empty/boundary access, and
  exact vector-versus-private-storage failure detail pass. The admitted vector
  workload improves by at least 5% and controls regress by no more than 5%.
  Test the duplicate-check proxy first; if it cannot reach that threshold and
  no correctness invariant requires the representation change, close the row
  as measured-defer without adding the new discharge vocabulary.

The implementation removes vector's mirrored `count`; private prefix storage
is now the sole live-length authority across construction, mutation,
relocation, iteration, and access. Safety analysis records an exact relation
between one canonical prefix-storage origin, the scalar origin of an index,
and a successful logical-size check whose length came from that same storage.
Prefix mutation/relocation, index writes, may-alias writes, opaque calls, and
joins without the relation invalidate it. The backend-only unchecked prefix
helper has no GTI source declaration and is selected only after independent
verified-MIR replay accepts the bounds discharge.

The exact vector surface records seven bounds discharges covering read-only and
mutable `front`, `back`, `at`, and `operator[]`; each retains its public vector
check and omits only the private prefix recheck. Native O0/O2/O3 evidence
preserves empty `front`, `back`, and `at` as `index_out_of_bounds in vector`,
while prefix capability evidence retains private guard, move, relocation,
reverse-destruction, mutation, and sanitizer coverage. At O2 the admitted
vector median fell from 558.336 ms to 433.264 ms; at O3 it fell from 570.654 ms
to 405.981 ms. Exact report identities, counters, controls, and raw samples live
in [`performance-tooling.md`](performance-tooling.md).

### O-PERF-04: Exact Fast Path For Undischargeable Checked Loops

- **State/role:** measured defer on exact clean commit `4dbb50bd`; the required
  source-proxy gate rejected a transform. Re-enter only with a benchmark whose
  hot inputs remain genuinely opaque after the static rows and whose exact
  proxy demonstrates at least 30% available value.
- **Rejected audit shape:** PR #164's wrapping block plus sticky predicate and
  replay is not accepted as semantics-preserving as written. Its own
  side-effect-free legality rule excludes the flagship `scale` loop because
  that loop mutates the caller's vector. A sticky flag also does not by itself
  preserve strict left-to-right integer recurrence, reordered intermediate
  overflow, first failing operation, or already-committed earlier effects.
  LLVM intrinsics cannot be GTI proof or a C++-backend prerequisite.
- **Admissible investigation:** compare a side-effect-free buffered block with
  exact replay against a preflight/versioned fast path that performs no
  observable mutation, then executes an unchecked block only after proving all
  of its checked operations safe; retain the original scalar checked block as
  fallback. Admit no call/user code, allocation, synchronization, raw memory,
  FP reassociation, loan/drop/lifecycle event, unstable read, escaping write,
  or may-alias mutation unless a complete snapshot/commit proof exists.
  Reordered integer evaluation must prove all introduced intermediates and the
  exact result, not just the original sequential prefixes.
- **Entry/exit gate:** a disposable source-MIR proxy on the opaque workload
  improves median runtime by at least 30% before infrastructure lands. The
  implementation then preserves exact successful output, first-failure site
  and record, partial mutation trace, cleanup, O0 identity, fallback behavior,
  and every rejected near miss. It must bring the checked-vector/core-loop gate
  to at most 1.5x idiomatic C++ or close as measured-defer with the next cause
  identified.

The admitted scalar recurrence performs 200 million checked additions whose
loop-invariant `argc` operand is runtime opaque. Its source preflight reads no
mutable state, admits only an exact range that proves the first and every
subsequent addition, uses public wrapping arithmetic in the proved lane, and
retains the original checked loop unchanged for every other input. Five
warmups and twenty seeded interleaved O3/native-O3 samples measured 540.055 ms
for checked GTI, 538.225 ms for the proxy, 539.767 ms for semantic C++, and
538.246 ms for idiomatic C++. The 0.34% proxy improvement is far below the 30%
entry threshold, while checked GTI is already 1.003x idiomatic.

An earlier record/vector experiment was rejected as the row proxy: its 29.28%
ceiling removed operations whose safety follows from modulo-bounded record
fields but whose facts are outside the current scalar element envelope, and it
retained public vector call/check overhead. That result identifies aggregate
element facts and container call boundaries as separate future profiling
targets; it does not justify checked-loop versioning. No CFG cloning, sticky
wrapping, replay machinery, public-name recognition, or backend-specific proof
was added.

### O-PERF-05: Residual Backend Representation Hygiene

- **State/role:** complete as a measured no-op on exact clean commit
  `9da44477`; the admitted clients meet the gate without a backend change.
- **Scope:** profile a scalar-FP/update and one closed-call client whose timed
  region exceeds 50 ms. Compare MIR-equivalent emitted C++ and semantic C++ at
  the same native settings. Test only surviving artifacts such as unnecessary
  out-of-line failure-ABI boundaries, repeated representation loads, blocked
  LICM, or missed target policy. `noexcept`, attributes, or C++ spelling are
  backend policy only after exact MIR effects prove them; strict GTI FP order
  and `-ffp-contract=off` remain unchanged.
- **Exit gate:** the admitted residual client reaches at most 1.15x semantic
  C++, or profiler/disassembly and an upper-bound proxy show less than 5%
  available value and close the row as a measured no-op. Every supported
  native compiler, MIR/backend differential, failure ABI, and campaign control
  remains correct.

The exact schema-3 campaign used native O3, strict floating-point flags, five
warmups, and twenty interleaved samples. `scalar-fp-update` measured 80.297 ms
in GTI versus 79.885 ms in semantic C++ and 79.680 ms in idiomatic C++.
`vector-checked-loop` measured 53.650 ms in GTI versus 491.655 ms in semantic
C++ and 51.400 ms in idiomatic C++. Native inspection found a nine-instruction
call-free hot loop in both scalar variants and fully inlined/vectorized vector
loops. The 0.5% scalar semantic gap and 4.4% vector idiomatic gap leave less
than 5% value; no backend annotation, ABI, or representation change was
admitted. Exact report identity and raw samples live in
[`performance-tooling.md`](performance-tooling.md).

### O-GAME-01: Game-Style Runtime Acceptance Workload

- **State/role:** complete; this row released the exclusive compiler gate on
  2026-08-27. Its first scalar/container baseline was admitted in `O-PERF-00`;
  unavailable public APIs remain explicit staged constituents rather than
  compiler-private simulations.
- **Bounded scope:** compare GTI, semantic-equivalent C++, and explicitly
  idiomatic C++ for the same fixed update stream and complete-state digest.
  Cover array-of-structures and structure-of-arrays math, container
  traversal/mutation, static/callable dispatch, bounded allocation/pool reuse,
  joined tasks, and a separately attributable C-FFI call client as public APIs
  become available. Keep rendering, file I/O, input generation, and reporting
  outside the timed region.
- **Evidence policy:** record median and tail update latency, work units, raw
  samples, exact compiler/toolchain/source identity, profiles, emitted-code
  hashes, optimization reports, and correctness/failure records. Shared CI
  remains threshold-free; gates run on the published stable machine.
- **Exit gate:** core checked kernels reach at most 1.5x idiomatic C++. The
  combined update reaches at most 1.5x semantic-equivalent C++ and 2x
  idiomatic C++, with no constituent above either applicable gate without a
  completed fix or measured-defer record. Fixed-array, managed-thread,
  compile-time, code-size, and memory controls have no unexplained regression;
  full compiler/frontend/backend/runtime and installed-toolchain gates pass.
  Closing this row releases the exclusive compiler gate.
- **Closure evidence:** clean implementation commit `c5bbfaa8` produced the
  exact `GTI-BENCH-1 digest:0000000012e84ad4 work-units:61440000` record for
  every variant. GTI measured 58.503 ms median (0.424 ms MAD), semantic C++
  208.126 ms, and idiomatic C++ 58.454 ms. The optimizer discharged 53 of 85
  source safety operations, including both bounds outcomes and all 36 hot
  overflow outcomes; emitted C++ fell to 320,030 bytes and the executable
  remained 115,200 bytes. Exact fixed-array, vector, scalar, and joined-thread
  controls were conclusive. O0 coherence now skips inapplicable whole-program
  safety replay: the same source used 56.26 MB maximum RSS versus 62.59 MB on
  pre-game commit `9da44477`. O3's 87.56 MB peak is the bounded closed-call
  aggregate analysis that supplies the new proofs, limited to 32 global rounds
  and eight updates per body rather than an unexplained all-level regression.
  Full raw samples, hashes, and control reports are recorded in
  [`performance-tooling.md`](performance-tooling.md).

### O-MIR-08: Loop-Derived Safety Proof Client

- **State/role:** measured defer; not implemented and not the active optimizer
  client.
- **Bounded scope:** consume only `O-MIR-06` single-latch affine facts to prove
  one existing fixed-array bounds family over a reducible loop. Reuse the
  existing safety-discharge transform and replay path; add no loop rewrite.
- **Entry/exit gate:** reconsider only when source MIR produces replayable
  loop-carried affine facts and an upper-bound proxy demonstrates material
  runtime value after native optimization. Then a representative checked loop
  must show removed operations and an end-to-end improvement; every induction,
  bound, overflow, early-exit, mutation, and irreducible near-miss retains
  checks; O0/O2/O3 runtime and failure-origin parity passes.

### O-MIR-09: Alias-Safe Local Memory Client

- **State/role:** measured defer; not implemented.
- **Bounded scope:** choose exactly one redundant-load or store-to-load family
  only after a benchmark demonstrates value. Add the smallest explicit local
  memory-version fact needed to prove no intervening write using `O-MIR-07`
  and exact call effects.
- **Entry/exit gate:** projected aliases, calls, raw memory, borrows, moves,
  drops, synchronization, and lifetime boundaries retain the operation unless
  proved safe. Introduce MemorySSA only if this measured family cannot remain
  sound and maintainable with bounded local versions.

### O-MIR-10: First Interprocedural Transform

- **State/role:** parked as a standalone row; its bounded campaign client was
  subsumed and completed by `O-PERF-02`.
- **Bounded scope:** if `O-PERF-02` selects inlining or body specialization,
  apply this row's closed concrete, non-recursive, non-native, non-virtual
  identity, cleanup, provenance, failure, and deterministic cost gates there.
  A distinct return-range summary remains value analysis rather than an effect
  flag. Devirtualization remains outside the campaign.
- **Entry/exit gate:** the row may reopen only for a new measured client not
  already served by the completed `O-PERF-02` summary mechanism.

Incremental dominance and LLVM bit-vector adoption remain measured/client-
gated. A future LLVM backend remains blocked on complete MIR layout, ABI,
temporary, drop, and executable authority; support-library adoption does not
imply it.

## Parallel Engineering Tracks

These rows improve maintainability, projects, and tooling without changing the
language critical path. They were parked during the completed runtime-
performance and `R-STRUCT` exclusive gates. Their table states and the active
ready queue now control scheduling by logical prerequisite.

### C-MIG: Compiled Compiler Library

The accepted detailed plan remains
[`compiler-library-migration.md`](archive/compiler-library-migration.md).
That migration is complete and supplies the compiled target foundation for the
completed internal decomposition in the
[archived structural-refactor plan](archive/compiler-structural-refactor.md);
it is not a competing active queue.

| ID | State | Next bounded result | Serialization rule |
| --- | --- | --- | --- |
| `C-MIG-02` | done | SourceLoader and parser algorithms compile behind their existing public contracts with recovery and diagnostics preserved. | Completed without a language behavior change. |
| `C-MIG-03` | done | Semantic records remain public while semantic analysis and concrete-instance reanalysis compile behind `SemanticVisitor`. | Completed without changing semantic authority. |
| `C-MIG-04` | done | HIR instance discovery/lowering and MIR body/CFG lowering compile behind public IR contracts. | Completed without changing lowering behavior. |
| `C-MIG-05` | done | MIR editing, analyses, effects, verification, printing, and current pass implementations compile behind public contracts. | Completed without changing O0/O1 behavior or pass verification. |
| `C-MIG-06` | done | C++ emission compiles behind `BackendInput` in an independently linked and installed `gti_cpp_backend`. | Completed without changing generated C++ or migrating AST emission to MIR. |
| `C-MIG-07` | done | Remaining frontend orchestration, semantic snapshot/query, constant-evaluation, formatting, language-query, and support algorithms compile behind existing contracts. | Completed with public records, templates, `constexpr` operations, trivial accessors, and exact-version facades intentionally retained in headers. |

### A-ID: Frontend Identity And Recovery

| ID | State | Scope and gate |
| --- | --- | --- |
| `A-ID-01` | ready | Add GTI-owned AST kind tags and `gti::isa/cast/dyn_cast`; migrate one heavy cluster at a time, serialized with any active `C-MIG-02/03` slice. Do not use LLVM casting. |
| `A-ID-02` | blocked | After `C-MIG-02`, first define source-unit and snapshot lifetime/serialization, then replace path strings in token/span identity with `SourceUnitId`, preserving deterministic diagnostics. |
| `A-ID-03` | blocked | After `A-ID-02`, add snapshot-local `NodeId` and migrate side tables incrementally to dense vectors. Raw AST pointers may remain local implementation details during transition, not cross-snapshot identity. |
| `A-FLOW-01` | measured defer | After `A-ID-03`, adopt dense binding state only if profiles still justify it; do not couple it to TypeContext. |
| `A-REC-01` | blocked | After `C-MIG-02` establishes the parser migration seam, add honest recovery/error expression nodes with formatter/LSP tests; no second declaration parser. |
| `A-CACHE-01` | blocked | After `C-MIG-02`, cache parsed immutable units by path/content identity so standard-library units are not re-lexed/reparsed per edit; never retain snapshot-local AST pointers across incompatible snapshots. |

### E-BACK: Backend Determinism And Concrete Instances

| ID | State | Scope and dependency |
| --- | --- | --- |
| `E-SYM-01` | measured defer | When `E-INST-01` or persistent build artifacts provide the client, replace counter-shaped generated symbols with deterministic declaration + concrete-argument identity. The printed form stays GTI-owned and does not require TypeContext pointer identity. |
| `E-INST-01` | blocked | After `M-BACK-01` and `E-SYM-01`, emit concrete GTI generic/class/function instances instead of asking native C++ templates to instantiate them, one instance family at a time. |
| `E-EMBED-01` | bounded-first | After `M-FAIL-01` and the allowlisted entry's closed-call-graph `M-BACK-02` slice, generate one host-callable containment wrapper over an allowlisted signature. The row itself owns context validity/poisoning, descriptor lifetime, observer configuration, record return, invocation cleanup, native-exception firewalling, and same-process re-entry tests; it is not yet the stable GTI ABI. |
| `E-ABI-01` | later breadth | Stable GTI ABI/separate compilation only after layout, names, concrete emission, package identity, and compatibility policy are complete. |

### P-MEASURE: Performance And Representation

Prefix-storage execution tracking:
[issue #37](https://github.com/LukeHHA/gti/issues/37).

| ID | State | Scope and gate |
| --- | --- | --- |
| `P-MEASURE-01` | complete benchmark substrate | The standard-library-only runner, strict descriptors, correctness digests, controlled output paths, raw samples, compiler/build identity, and GTI/semantic-C++/idiomatic-C++ workloads are implemented with threshold-free smoke coverage. `O-PERF-00` owns the fixed-native attribution mode and runtime workload breadth; `O-GAME-01` closed the exclusive campaign. |
| `P-STORAGE-01` | complete | Prerequisites delivered: the checked-vector fixture shipped through `P-MEASURE-01`, `M-FAIL-01`'s defined-failure contract is live, and the failure-capable `M-BACK-02` slices landed (0.176.0-0.189.0). Slice 1 shipped in 0.190.0: the identity-bound public logical-size check (`gti_internal::index_bounds_check`, `IntrinsicKind::StorageBoundsCheck`) guards every public vector/string accessor while sparse storage remains, reporting `GTI-R0007` with the enclosing trusted container's detail (`index_out_of_bounds` in `vector`/`string`) rather than leaking `GTI-R0010`. Slice 2 shipped in 0.191.0: the distinct compiler-private prefix-initialized storage capability (`gti_internal::prefix_storage<T>`, `CompilerCapabilityTypeKind::PrefixStorage`, `SemanticType::PrefixStorage`, the seven-intrinsic family, and the length-tracking backend representation with no per-slot bitmap) exists end to end through semantics, HIR, MIR (`mir-v31`), the effects tables, and the compatibility backend, proven by trusted-prelude fixtures; sparse `storage<T>` remains for arbitrary partial slots and the two capabilities reject each other's operations. Slice 3's capability-level evidence shipped in 0.192.0: the dedicated native evidence battery (`prefix_storage_native_evidence`) compiles trusted generated programs across C++20/C++23 and native -O0/-O2/-O3, runs the full behavioral contract (append order, logical-prefix reads and mutation, pop-last destruction, complete-prefix relocation with moved-from shells correctly skipping user destructor bodies, move transfer, reverse destruction at scope exit), trips all five runtime guards on their exact diagnostics, and runs the success path clean under dedicated generated-program AddressSanitizer/UndefinedBehaviorSanitizer builds. The wrapper migration shipped in 0.193.0: `vector` and `string` internals moved wholly onto the prefix capability (fields, iterators' storage borrows, and every operation), with the shift gap filled by two sealed invariant-preserving primitives — `prefix_storage_insert` and `prefix_storage_erase` — whose element shift and construction or destruction happen inside one runtime operation, so no partially initialized slot is ever observable at the language surface; the example corpus stayed byte-identical in stdout and exit codes and the checked-vector benchmark workload passes on the migrated containers. Sparse `storage<T>` remains the arbitrary-slot capability. The later backend slice is complete: the storage capability rows and general-emitter vocabulary admit these storage-gated bodies to verified-MIR emission. Historical: add the identity-bound public logical-size check while sparse storage remains so vector/string indexing reports `GTI-R0007` rather than leaking `GTI-R0010`. Then add a distinct compiler-private prefix-initialized storage capability for vector/string-shaped owners while preserving sparse `storage<T>` for arbitrary partial slots. Prefix construction appends exactly at the live length, destruction removes exactly the last live element, relocation transfers the complete prefix, and reads check the logical prefix. Semantics, HIR, MIR, effects, and every backend bind the capability by private declaration identity. Construction-failure rollback, relocation, move, clear/pop, reverse destruction, C++20/23, fixed-native-optimization O0/O2/O3 differential, dedicated generated-program ASan/UBSan, and benchmark evidence must pass before migrating the public wrappers. No trusted-source unchecked accessor or public-name special case is permitted. |

- Existing `--time-trace` and deterministic MIR printing count as completed
  foundations; do not reimplement them under the older performance plan.
- Add structured optimization/safety reports only when the first real MIR pass
  produces decisions worth reporting.
- `TypeContext` becomes ready only after a written snapshot/context lifetime,
  a type-allocation/churn benchmark, and evidence that identity migration pays
  for its cross-phase cost. Start with a GTI-owned interface and ordinary
  storage; compare private LLVM allocation/indexing only afterward.
- Shrinking `MirInstruction`, LLVM VFS, `lit`/`FileCheck`, private allocators,
  and sparse bitsets remain measured options rather than queued work.

### B-PROJECT: Builds, Packages, And Dependencies

The next order resolves drift between the build proposal and checkpoint:

| ID | State | Scope and gate |
| --- | --- | --- |
| `B-PROJECT-01` | complete | Project test targets and `gti test` use driver-owned immutable build plans. Deterministic all/named selection, independent outputs, runtime-failure continuation/propagation, metadata kind publication, direct-mode preservation, and installed CLI/library coverage pass. |
| `B-PROJECT-02` | complete | Project build/run/test requests use a deterministic SHA-256 whole-program cache over the compiler-owned loaded source graph, including ordered configured prelude-root provenance, effective target/profile/backend policy, runtime/toolchain identity, and admitted native environment. Verified generated-C++/executable payloads publish atomically under `build/gti/cache/v2`; hits skip frontend/backend/native compilation, corruption rebuilds before replacement, legacy schema entries are ignored, and `--no-cache` bypasses read/write. Requests with declared native sources, native search directories (including dependency-injecting environment paths), opaque native argument vectors, native link operands, or unresolved libraries/frameworks conservatively bypass the cache until their transitive inputs are modeled; pure-GTI checkout moves retain content identity, and direct mode remains uncached. |
| `B-PROJECT-03` | complete | Manifest schema 1 resolves canonical workspace members and recursive source-only path dependencies without network access. Direct aliases feed the compiler-owned source graph, transitive/quoted cross-package access is rejected, cycles/duplicate names/roots/nested workspaces fail before compilation, `--package` selection and shared collision-free outputs are deterministic, schema-9 metadata publishes the graph, and cache identity includes package provenance. Direct mode remains manifest-independent. |
| `B-PROJECT-04` | complete | Exact pinned-revision Git resolution, `gti.lock`, explicit `gti fetch`, `--locked`, and `--offline` use immutable content-verified checkouts and have reproducible CLI and installed-toolchain coverage. Registry ranges, mutable branch/tag selectors, and dependency code execution remain out of scope. |
| `B-PROJECT-05` | ready | Publish a stable read-only driver `ProjectFacts` API and library tests over the implemented workspace graph. This row does not integrate the LSP, fetch, or build. |

Each row must preserve direct mode. Dependency fetching, manifests, cache, and
artifact policy stay in `gti_driver`, never `gti_compiler` or the LSP.

### T-LSP: Language Tooling

| ID | State | Scope and dependency |
| --- | --- | --- |
| `T-LSP-01` | hover/completion/signature help complete; API docs ready | `///` Markdown is retained once on declarations and exact compiler symbols; hover publishes it at declarations and resolved uses across dirty dependency overlays, and completion items and signature help publish the same symbol-attached value under the client's declared documentation format. Generated API docs remain a consumer of the same value. |
| `T-LSP-02` | complete for current snapshots | Current-snapshot references, document highlights, hierarchical document symbols over parser-recorded extents, fail-closed function-local rename, and signature help from the selected overload and argument position are implemented through `LanguageQueries` with protocol tests. Project-wide coverage remains gated on `T-LSP-04/05`. |
| `T-LSP-03` | measured defer | Extract document/snapshot service boundaries when a concrete feature makes the current server class costly to evolve. |
| `T-LSP-04` | blocked | After `B-PROJECT-05`, consume driver-owned `ProjectFacts` without fetching/building and with dirty buffers authoritative. |
| `T-LSP-05` | measured defer | A durable project index only after unopened-file references/rename proves a client. |
| `T-LSP-06` | measured defer | Process-isolated analysis only if in-process crash recovery must guarantee containment of heap corruption. The current guard is state-safe, not process isolation. |

Every new syntax/semantic feature still updates formatter, Tree-sitter, LSP
tokens/queries, shipped-source parsing, and documentation in its own row.

### Q-QUALITY: Diagnostics, Conformance, And Release

| ID | State | Scope and gate |
| --- | --- | --- |
| `Q-DIAG-01` | ready | Add one generated diagnostic table with enum identity, default severity, group, and format contract; migrate one subsystem per bounded sub-slice while preserving current spans/messages unless intentionally changed. |
| `Q-FAIL-01` | hosted consumer, same-thread native-callback firewall, and managed-task handoff/re-raise complete; broader M-FAIL integration active | The versioned C ABI, exact escaped ordinary/emergency reports, artifact/site/outcome validation, status 70, observer firewall, report-I/O fallbacks, terminal arbitration, callback termination adapter, and worker ordinary/`GTI-R0014` containment are implemented and focused tests pass. Embedding, foreign-thread callback containment, and broader checked families remain co-delivered with their M-FAIL/M-BACK slices. |
| `Q-DEPRECATION-01` | blocked | `D-COMPAT-01` is done. After `T-LSP-01`, add one bounded `[[deprecated("message")]]` declaration contract with use-site diagnostics, semantic metadata, formatter, Tree-sitter, hover, completion, and deterministic tests. |
| `Q-FUZZ-01` | bounded compiler campaign complete — `Q-FUZZ-01A` capture/replay, `Q-FUZZ-01B` parser/formatter/pipeline/source-loader libFuzzer harnesses, `Q-FUZZ-01C` resumable bundle reduction, and `Q-FUZZ-01D` real-compiler typed differential/composition validation are implemented; prerequisite `C-MIG-02` is done | Wider seeded exploration remains opt-in. Arbitrary JSON-RPC/LSP fuzzing, a complete generator, and a semantics-aware reducer are explicitly deferred by [the tooling record](archive/compiler-bug-finding-and-triage.md). |

Examples, sanitizers, installed-library consumers, release packaging,
deterministic output, and the local language audit are standing release
invariants rather than a finishable task row. Each new feature adds its
conformance case in its owning row.

## Outstanding Work Classification

This table is the top-level disposition for major open work. Detailed scope is
owned by the rows and domain plans above.

| Area | Disposition | Principal blockers/owner |
| --- | --- | --- |
| Language restriction ledger | **complete and maintained** | `D-LANG-01`; [`language-alignment.md`](language-alignment.md) |
| Concurrency memory model | **adopted durable decision** | `D-MEM-01` and `D-MEM-02` done; ADR 008 |
| Transfer/share facts and concurrent globals | **complete policy substrate** | `I-CAP-01`, `D-MEM-02`, `C-TYPE-01`, and `C-GLOBAL-01` done; bounded public concurrency is systems-ready work |
| Public threads/atomics/mutex | source-defined owned `std::thread`, `std::thread_result<Result>`, ordered scalar `std::atomic<T>`, and scoped `std::mutex<T>`/`std::mutex_guard<T>` **complete** | `C-CALL-01`, `C-THREAD-01`, `C-RUNTIME-01`, `C-ATOM-01`, `C-ORDER-01`, `C-SYNC-01`, and their `C-CONFORM-01` subsets done |
| Evaluation order | **contract adopted; bounded ordinary call/constructor schedule implemented; systems-readiness implementation required** | `D-EXEC-01` and `M-LIFE-01` done; scalar/reference plus eligible class-copy/class-move ordinary-call and concrete ordinary-constructor HIR/MIR order landed; remaining `M-EXEC-01` and matching `M-BACK-01/02` slices remain |
| Runtime failure contract | contract complete; **systems-readiness implementation required** | `D-FAIL-01`, `I-CAP-01`, and `M-LIFE-01` done -> `M-EXEC-01` -> co-delivered `M-FAIL-01`/`Q-FAIL-01` -> complete `M-BACK-02` migration |
| Source text and documentation comments | **hover/completion/signature-help tooling complete; broader consumers remain** | `///` retention, hover, completion, and signature help are complete in `T-LSP-01`; broader library coverage and generated API documentation remain |
| Target/data-layout facts and `sizeof`/`alignof` | **complete bounded systems-readiness substrate** | `S-LAYOUT-01` and `S-LAYOUT-02` done; aggregate/native layout remains client-driven |
| Compiler-private capability visibility | **complete** | `I-CAP-01` done; trusted source roles, exact private type identity, `GTI-S2058`, and compiler-owned LSP filtering |
| Indexed partial moves | **complete bounded-first slice** | `M-OWN-01` and `M-OWN-02` done; dynamic indices remain conservative |
| Temporary/active-drop authority | **complete normal-exit lifecycle substrate** | `M-LIFE-01` done; failure rollback and full ordered materialization remain downstream |
| Stored/escaping mutable dependencies | **bounded-first systems-readiness proof** | `M-OWN-03`; first clients are mutex guards and mutable views |
| Mutable iteration/views | L-RANGE-01/02 and the bounded read-only single-owner L-RANGE-03 release are complete; mutable, nested, and multi-owner views remain | Follow-up must first extend the ownership representation explicitly |
| Native C records/handles/callbacks | bounded records, generated adapter header, pointer-only opaque identity, strings, returned arrays, and exact same-thread callback adapters complete; captures, userdata ownership, foreign-thread entry, and ownership transfer remain systems-readiness breadth | `S-ABI-01/02/03`, `S-FFI-02` F1/F2/F3, and `S-CALL-01` done; complete GLFW 3.4 oracle is the acceptance evidence |
| Owned callables and capture | contract complete; confined boundary, exact value-result, repeatable read/mut invocation, consuming once-callable cardinality, MIR-v10 environments/transport, explicit owned move capture, exact same-type generic return including the bounded repeatable-invoke/final-return combination used by `std::for_each`, one-field generic owner, exact consumed managed-thread `void()` and typed-result tasks, and non-capturing same-thread native adapters implemented; broader owner/extraction/concept work remains systems-readiness work | `D-CALL-01`, `C-CALL-01`, and `S-CALL-01` done -> remaining `L-CALL-01` |
| Allocator/provenance model | **typed public allocation/provenance slice complete; allocator-aware client ready** | `S-ALLOC-01/02` complete through ADR 022 and `<std/memory>`; `S-ALLOC-03` remains |
| Freestanding profile | **later breadth until a target workload requires it** | `S-FREE-01` |
| Payload enums/matching | **in progress:** passive exact construction, exhaustive copied matching, and the cleanup-free class-payload family complete; owning-payload drop transport, move/borrow patterns, generics, and stable layout remain | `L-SUM-01` |
| Integer arithmetic modes | wrapping/saturating and checked-result add/subtract/multiply **complete in 0.111.0**; division/remainder/shift breadth is client-gated | `L-NUM-01` |
| Binary64 | **complete in 0.110.0** | `L-FLOAT-01` |
| Domain operators | **systems-readiness client-gated work** | `L-OP-01`; exact member/capability families only |
| Error propagation syntax | **complete in 0.311.0** | `L-ERR-01` |
| Bounded concepts and requirements | multi-parameter source composition, validity-only trailing `requires` on functions/methods/constructors, constructor-local inferred type generics, closed exact requirement arguments, distinct copy-construction, input/forward/bidirectional/random-access-iterator, sentinel, sized-sentinel, exact-dereference, writable-dereference, and mutable-subscript relations, confined exact-result operations, and exact qualified class specialization are implemented; broader public callable, complete-range, and hash capabilities remain client work | ADR 009 and ADR 020; constructor-local proposal done -> remaining `L-CALL-01` -> remaining `L-RANGE-04`; `L-CONT-02`; general requires-expressions, partial specialization, subsumption, and ranking remain later breadth |
| Wider/value generics, custom lifecycle bodies, block statics, generalized borrow graphs, `static_assert`, and wider integers | **bounded-first or later breadth as classified** | Stable `R-*` entries in the restriction ledger; a demonstrated readiness client may add a row |
| Shared/weak ownership and optional | systems-readiness library work | temporary/drop authority; atomics only for cross-thread shared owners |
| Formatting, text, host services | systems-readiness library work | ranges/views, failure contract, bounded runtime/FFI inputs |
| Container/algorithm foundation | systems-readiness accepted minimum | `L-CONT-01`, `L-RANGE-04` |
| Associative containers | **systems-readiness client/benchmark-gated work** | `L-CONT-02`, exact capabilities, allocator proposal |
| Runtime performance | **exclusive campaign complete** | `O-PERF-00` through `O-PERF-05` and `O-GAME-01` closed in exact order; later work requires a new measured client and bounded row |
| MIR transformations | **bounded local production pipeline** | `O-MIR-01` through `O-MIR-07` provide current transforms/analyses; `O-MIR-08/09` are measured-defer; the closed interprocedural client was subsumed by `O-PERF-02` |
| MIR-backed C++ emission | **ADR 016 complete** | every body in `tests/mir_census_baseline.json` uses the verified-MIR route; every backend consumes the self-contained `LoweredProgram` contract |
| Type interning | **measured defer** | context lifetime and allocation benchmark |
| Natural-loop/range facts | **complete bounded analysis; transform measured-defer** | `O-MIR-06`; `O-MIR-08` reopens only for a source-MIR client with demonstrated native runtime value |
| Place alias/escape facts | **complete bounded analysis** | `O-MIR-07`; first standalone memory transform is gated by `O-MIR-09` |
| Incremental dominance | **measured defer** | add only when a measured transform makes full recomputation material |
| Compiler structural ownership | **exclusive campaign complete** | `R-STRUCT-00` through `R-STRUCT-05`; [archived closure report](archive/compiler-structural-refactor.md) |
| Source/semantic/HIR/MIR compiled migration | **complete foundation** | `C-MIG-02` through `C-MIG-07`; internal decomposition is owned by `R-STRUCT` |
| Deterministic names/concrete instance emission | backend/package preparation | MIR body authority and stable GTI-owned identity |
| Project dependencies/cache | parallel toolchain | tests -> cache -> path/workspace -> Git/lock |
| LSP project index/process isolation | client/risk-gated | project facts or stronger containment requirement |
| Separate compilation/stable GTI ABI | later-breadth proposal | deterministic names, layout/ABI, package model, concrete emission |
| Module vocabulary | Edition 1 include spelling fixed; implementation later | `D-COMPAT-01` done in ADR 011; modules are not required by current readiness workloads |

## Deliberate Stop Conditions

Stop a task and leave its row blocked when any of these occurs:

- a language rule can be explained only by generated C++ or an LLVM type;
- the proposed fact would have two authorities in semantics and MIR;
- a new private capability can be forged by spelling `gti_internal`;
- an ownership transition has no path-sensitive cleanup/verification story;
- a target/layout result comes from host `sizeof` rather than the selected
  target contract;
- a concurrency API relies on a borrow, capture, global, or cleanup capability
  not represented in semantics/HIR/MIR;
- proof, restriction, or framework work has no named user-facing consumer or
  current correctness defect;
- an optimizer wants a pass manager, incremental analysis, or LLVM container
  before the first client and measurement exist;
- a “mechanical” compiled-library move changes behavior; or
- a new executable language feature would add or extend AST/semantic/HIR-only
  C++ emission, or would treat MIR ignored by the production backend as
  feature completion; or
- a row would need a second prompt-sized feature to make its tests meaningful.

## Prompt Handoff Template

Use this shape when starting the next task:

```text
Implement <ROW-ID or BACKEND-RECOVERY-PHASE> from
docs/plans/implementation-sequence.md.

Before editing:
- verify every prerequisite and cite its current evidence;
- read the owning language, architecture, and domain-plan sections;
- name the readiness workload, public API, or user workflow this slice unlocks;
- classify its executable-authority impact and, for new runtime behavior, name
  the verified MIR-emitted body family that will own production execution;
- state the exact files/layers owned and the explicit non-goals.

Deliver the complete declared row or backend-recovery phase, including its
production MIR consumer. Add focused tests, run its exit gate plus the relevant
broader verification matrix, update the canonical docs and status evidence,
then stop. Reviewable commits are encouraged; an IR-only checkpoint is not a
phase exit when the matching production cutover is in scope.
```

`M-BACK-02` and ADR 016 are complete. A future backend task starts from the
immutable `LoweredProgram` consumer contract, exact corpus census, exhaustive
generated graph, and sealed whole-program plan. It must not link frontend
representations, parse generated C++, or recreate the retired no-MIR emitter as
a comparison, fallback, or legacy mode.

### Completed migration campaign: generated-item inventory

Native-callback, structural-operator, callable, lifecycle-cleanup, and concrete-
instance adapters are completed migration clients. Native callback semantic
selection, exact HIR-to-MIR identity, verified failure policy, and per-body MIR roots are copied into
sealed generated-item rows. Structural and callable adapters instead carry
exact function/operator or function/capability payloads and active-declaration
roots. Lifecycle cleanup carries the exact concrete class and destructor
instances, ordinary-versus-specialization form, failure mode, and destructor
declaration root. The planner verifies each census, source, payload, closure,
and order, and `CppEmitter` uses the sealed plan as the sole adapter-existence
authority. Concrete rows name each generic function/constructor declaration,
owner, optimized MIR body, and failure effect. Ordinary C/runtime declarations
already use ABI declaration/body rows, so native callbacks exhaust generated
native wrappers. C++ spelling remains backend-only.

1. Every generated adapter has a stable identity in the sealed whole-program
   plan and an exact census.
2. Each family uses immutable, target-independent representation rows whose
   inputs are semantic identities, concrete HIR
   instances, and verified MIR schedules. Keep C++ spelling and ABI policy in
   the backend representation layer.
3. Each row is structurally verifiable, its sole production consumer reads the
   sealed plan, and the corresponding AST/HIR eligibility query is deleted. GTI
   has no compatibility obligation during this pre-1.0 migration, so no
   fallback or dual-route comparison remains after a family lands.
4. **Exit gate met.** The generated-item census is complete, mutation tests
   reject missing, duplicate, stale, and reordered rows, and native O0/O3
   C++20/C++23 plus installed-toolchain oracles preserve program behavior. That
   sealed inventory becomes the shared input seam for the current C++ backend
   and a future LLVM backend.
