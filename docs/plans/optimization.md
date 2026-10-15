# GTI Optimization Architecture Proposal

## Runtime Performance Policy Revision (2026-10-09)

[ADR 012](../decisions/012-outcome-first-systems-readiness.md#runtime-performance-requirement-2026-10-09)
supersedes this plan's earlier benchmark-first entry restrictions. Runtime
performance is required; architectural rationale and a defined cost model can
justify general analyses, transforms and representation work without a measured
regression, proxy or minimum predicted speedup. Historical measured-defer
records remain evidence, not permission barriers to reopening the architecture.
This revision does not claim the proposed facilities are implemented or waive
their semantic proofs, independent verification and performance acceptance.

## Closed-world call-site facts (2026-10-09)

`optimization/closed_world` now provides the closed-world contract that
interprocedural rows need (see
[the architecture](../architecture/optimization.md#closed-world-call-site-facts)).
Its first fact, exact referent classes of reference parameters, devirtualizes
interface calls through forwarded parameters. Later clients should extend
this fact family rather than add a second notion of a closed function:

- argument scalar ranges for RT-2;
- exact `this` classes for member helpers;
- exact owned-pointee classes from allocation origins, for owning callback
  models;
- the callee-set inventory that RT-10 inlining and specialization need.

The existing closed-call safety analysis keeps its own root and rejection
rules. Reconciling the two inventories is follow-up work.

## PR #166 audit disposition (2026-10-09)

The reviewed proposal is `1636c1df`, not an implementation authority. Its
screen is useful for finding clients, but minimum-of-seven timings under
competing builds, non-interleaved attribution samples, and unsafe check-removal
ceilings do not establish a production speedup. The original runner also failed
to forward the selected C++ compiler/standard to the GTI build; PR reconciliation
corrects that plumbing with a two-mode protocol test. It remains a historical
screening harness, not the canonical measurement path. Reuse the existing benchmark and
frozen-storage comparison harnesses with complete-state oracles, exact source
and tool identities, warmups and interleaved samples.

The immediately justified slice is **RT-1: one cold failure-only helper**.
An independent current-head storage proxy improves runtime without removing
checks; broad cold scope/activation/destructor annotations instead regress it.
Production code adopts only the incoming-cleanup check's guarded cold/noinline
hint. Frozen production comparisons, runtime/compile/size/memory controls,
all 570 CTest gates and staged consumers now pass; the proxy alone was not
treated as completion evidence. See
[performance-tooling.md](performance-tooling.md#rt-1-source-storage-failure-path-cost).

RT-1's second slice (2026-10-12) emits every `Invoke` failure alternate as an
`[[unlikely]]` branch and publishes each detector's record and incoming-cleanup
check through one cold helper. Against the same tree it makes `strings` 36%,
`sort` 15% and `hashmap` about 4% faster, and leaves the other probes within 2%
once loop placement is controlled. On the default layout, `particles` and
`vector-push` lose about 10% from where their unchanged hot loops land; the
owner accepted that. See
[performance-tooling.md](performance-tooling.md#rt-1-cold-failure-paths-2026-10-12).

The rest is future work, not implicitly implemented by this change. The rows
below retain their technical prerequisites under the revised admission policy:

| Finding | Disposition, prerequisite and acceptance |
| --- | --- |
| RT-1 broader outlining/inline policy | Unlikely failure edges and cold record publication are delivered (2026-10-12). The remaining headroom is `hashmap`, at 101 ms against an 80 ms threshold-2000 ceiling, for an inline decision from MIR success-path size. Native loop alignment (1 to 3% on several probes) is a separate, unadopted lever. Keep managed-worker handoff exception-transparent; blanket `noexcept`, forced inlining and higher global thresholds are not justified by this revision. |
| RT-2 contextual narrowing | Prepare a bounded post-fixpoint refinement, re-establishing conservative coverage of **every** reachable call context. Current closed-call contexts widen but do not narrow; simply replacing widened inputs with attractive return ranges is not a proof. Require independent replay, conflicting callers/recursion/budget negatives, and loop-carried performance validation at acceptance. |
| RT-3 conversions/division/shifts | A general proof family, beginning with exact integral conversion domains. Each failure outcome needs its own rule and replay: zero and signed-minimum/-1 for division, and all GTI shift preconditions, not merely a bounded shift count. Require boundary negatives and representative byte/text performance validation; a prior measured gain is not required to broaden the family. |
| RT-4 refreshed infallibility | Structural failure-freedom of canonical MIR and the backend's plain-call vocabulary are implemented; measured neutral at run time. The remaining slice requires effect derivation on optimized MIR plus an independently replayed Invoke-to-Call edit, including CFG/identity repair. Native, recursive, synchronizing and cleanup boundaries remain conservative. No backend inference from apparently empty failure blocks. |
| RT-5 relational index/length facts | The body-local slice is implemented (2026-10-13): strict orders from dominating comparisons discharge public logical-size checks in the same body, with opaque-call/mutation negatives and failure parity. It changes no corpus program or probe on its own. Crossing an accessor still requires an admitted precondition/specialization or bounded inlining; a container's public name is not authority. The measured ceiling of crossing `vector::operator[]` is about 20% on a store loop with a hoisted bound and little elsewhere, because Clang already removes the checks in plain read and copy loops; see [RT-5](#rt-5-no-relational-facts-between-indices-and-lengths). |
| RT-6 affine loop versioning | Requires a non-failing, side-effect-free preflight and an unchanged checked fallback. Hoisting may not change the first failure, partial stores, evaluation order or floating-point ordering. The fallback's presence alone does not prove the preflight safe. Validate representative clients and preflight costs as part of acceptance. |
| RT-7 iterator counters/reductions | Prove length fits the **counter's** domain; allocation capacity alone does not prove a `ptrdiff_t` counter cannot overflow. Speculative reductions additionally require non-observable, exactly restorable state and ordered first-failure replay. Do not apply a wrapping accumulator to ordinary effectful loops. |
| RT-8 slot/drop self-checks | Earlier measurement found no demonstrated value; that is not an architecture admission gate. Before omitting an assertion, identify the exact verifier invariant and every representation caller that establishes it. Do not assume all backend checks are redundant or introduce an unchecked release mode. |
| RT-9 library algorithms/layout | General source-library architecture, not nominal compiler shortcuts. Sort changes require comparator failure/order and worst-case tests; flat maps require explicit iterator/reference/address and allocation contracts. Design representations to match their invariants and validate workload time/cache/allocation costs before acceptance. The sort slice is delivered in 0.410.1 as bounded iterative introsort; see [RT-9](#rt-9-library-algorithm-and-data-structure-choices). |
| RT-10 bounded MIR inlining | General optimization architecture may justify this work without a prior workload speedup or proxy. Needs source provenance, argument order, loan/drop/cleanup composition, recursive/native/virtual boundaries, cost limits and independent replay. Validate representative call chains and code-size/compile-time tradeoffs. |
| RT-11 payload-enum construction and layout | Measured 2026-10-15 with `std::experimental::optional` against `std::optional` at `-O2`: a 200-million-iteration `int` lookup loop takes 0.27 s against 0.17 s, and an allocating `string` loop 1.63 s against 1.44 s, with identical output and binaries within 1% in size. Both loops inline the same calls, so the difference is representation. An engaged value is built in a value slot and then move-constructed into the field once its empty shell is retired, so the payload moves two or three more times than through the compiler-private optional storage, each move followed by a moved-from destruction. For a trivially copyable payload, the whole-object copies and moves let LLVM fuse the `std::variant` tag and payload into one integer, so tag tests after a dominating `has_value()` are not folded. A tag-only switch over a passive enum also copies its subject first, which a hand-edited probe put at about 7% of the `int` loop. Prerequisite for promoting the experimental optional: construct a fresh alternative directly in its destination field or result storage when MIR proves the destination's empty shell, and switch on the place without a copy, preserving the verified Move/Replace/Drop schedule. A GTI-owned tagged layout is the further option if in-place construction does not close the gap. The owner accepted the gap for now (2026-10-15): RT-11 follows the remaining `?` language work, so its scope covers every payload-enum client. |

Closed-world executable lowering from #176 is already integrated; the old
whole-stdlib code-size observations must be remeasured, not used as current
evidence. None of the above authorizes removal of GTI-observable checks without
canonical proof or an exactly equivalent, independently verified fallback.

> **Plan status:** Non-canonical staged design. The implemented optimizer
> boundary is documented in
> [`docs/architecture/optimization.md`](../architecture/optimization.md).

Status: accepted; Milestones 1 through 5 are implemented for their bounded
local families. Milestone 6 has verified effects, bounded contextual safety
summaries and closed-world exact-reference-receiver dispatch refinement.
General MIR inlining/cloning, owned-pointee/`this` specialization and broader
memory/loop transforms remain incomplete. The completed
exclusive runtime-performance campaign and every follow-on optimizer row are
sequenced only in
[`implementation-sequence.md`](implementation-sequence.md).

Historical audit (2026-10-08): a measured
[runtime optimization audit](#runtime-optimization-audit-2026-10-08) records
findings RT-1 through RT-10 and the release-check boundary. Their current
disposition and prerequisites are recorded in the
[2026-10-09 revision](#pr-166-audit-disposition-2026-10-09), not the original
audit's scheduling statements.

Implementation update (2026-10-01): the exhaustive optimizer language-growth
gate is reconciled at `72a8087a`. The measured follow-up implements one bounded
body-local exact-receiver dispatch refinement, with canonical replay and
conservative loan/effect/cleanup boundaries. This does not implement general
devirtualization, MIR inlining, interprocedural receiver specialization, or
MemorySSA. `O-MEASURE-02` is now complete for this facility at measured clean
implementation `2c8d4561`: paired local-interface runtime falls from 223.175 to
120.008 ms, controls show no material regression, and broad/focused functional
gates pass. Exact evidence and the larger, still-unimplemented callback/helper
proxy ceilings are recorded in [performance-tooling.md](performance-tooling.md).
Publication was held at that historical checkpoint; the bounded facility is
now integrated. It is not completion of every devirtualization family.

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
backed by canonical body-local proofs. O-MIR-03 and the first O-MIR-04 slice have their architectural prerequisites
in place.

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
[`implementation-sequence.md`](implementation-sequence.md). The current analysis cache exists
for scalar promotion, folding, unreachable cleanup, and dead computation
removal, SCCP, safety analysis, and alias-aware safety invalidation. Loop/range
and alias facts remain available for further performance work, but they do not
dictate a pass. General loop transforms, MemorySSA, broader contextual/owned-pointee
specialization, an LLVM backend and broader optimizer frameworks remain
unimplemented. The bounded local and closed-world reference-parameter
exact-receiver clients are implemented. Their design and scheduling may follow architectural need under
ADR 012 without waiting for a measured bottleneck; the operational sequence
still owns task ordering and each facility retains its technical prerequisites.

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

## Runtime Optimization Audit (2026-10-08)

Status: historical audit of compiler `f0ba8383`, not a claim about the current
implementation. The original findings and suggested first slices below retain
their dated context. Current status, corrected prerequisites, and scheduling
policy are governed by the [2026-10-09 disposition](#pr-166-audit-disposition-2026-10-09)
and [runtime-performance policy](#runtime-performance-policy-revision-2026-10-09).
That revision supersedes the original stabilization pause and benchmark-first
priority restrictions; the implementation sequence remains the scheduling
authority. Each finding still names the GTI rule it must preserve and the
evidence an implementation needs.

The audit asked which established compiler techniques could make ordinary GTI
programs faster without weakening GTI safety or semantics, including removing
checks in release builds where a provable boundary allows it. Unlike the
earlier campaigns, which tuned named workloads to parity, it screened 14 new
everyday probes against idiomatic C++ twins. It then attributed each gap with
native inlining remarks, sampled profiles, disassembly, GTI optimization
remarks, and measurement-only ceiling variants of the emitted C++. Probes and
the reproducible screen live in
[`benchmarks/probes/runtime-audit`](../../benchmarks/probes/runtime-audit/README.md);
method, machine, and raw medians are recorded in [performance
tooling](performance-tooling.md#rt-audit-01-runtime-optimization-screen-2026-10-08).

### Safety boundary for "removing checks in release"

GTI has two different kinds of runtime check, and only one of them may differ
between build profiles:

1. **GTI-observable checks** are language semantics: integer overflow, bounds,
   conversion range, division, shift count, owner access, expected access,
   storage state, and the other defined failures. Such a check may disappear
   only through a replayable proof that it cannot fail, or through a
   restructuring whose fallback reproduces the exact first failure, its order,
   cleanup, and partial effects (for example loop versioning). An optimization
   level, release profile, or user flag must never remove one unproved. An
   unchecked release mode would change program meaning and is not recommended.
2. **Backend representation self-checks** assert invariants that verified MIR
   has already proved. Examples are the lifetime-slot `engaged_` assertions
   ("verified MIR lifetime slot was read while empty"), double-construction
   checks, and cleanup-scope consistency aborts in the generated preamble.
   They are not GTI semantics. A release profile may omit them where the MIR
   verifier is the proof authority, keeping them in debug and verification
   builds. This is the only category that "dropping checks in release" can
   safely mean.

### Screen summary

Medians, GTI `-O2` against idiomatic C++ `-O2` under the same strict
floating-point policy (`GTI/C++` ratio; see the performance record for
samples):

| Probe | Ratio | Dominant cause found |
| --- | ---: | --- |
| `sort` | 8.66x | heapsort algorithm (RT-9) plus bounds checks in non-inlined helpers (RT-1, RT-5) |
| `vector-push` | 6.11x | `push_back` never inlined (RT-1, RT-9); checked sum blocks vectorization (RT-7) |
| `matrix` | 4.01x | two retained bounds checks per inner iteration block vectorization (RT-5, RT-6) |
| `lambda` | 3.58x | checked `count_if` counter blocks vectorization (RT-7) |
| `conversions` | 2.98x | conversion family never discharged (RT-3); checked accumulator (RT-7) |
| `strings` | 2.29x | `push_back`/`find` not inlined (RT-1); redundant bounds check (RT-5) |
| `hashmap` | 2.27x | lookup helpers not inlined (RT-1); invariant checks in node chains (RT-9) |
| `division` | 1.40x | division family never discharged (RT-3); division cost dominates |
| `calls` | 1.31x | loop-carried context loses the proof (RT-2) |
| `expected` | 1.09x | near parity |
| `particles` | 1.04x | near parity |
| `dispatch` | 1.04x | parity |
| `shifts` | 1.03x | parity; literal and masked counts already cheap |

`sort-heap` runs the same GTI program against a C++ twin of GTI's exact
heapsort: GTI is 3.13x that twin, while the heapsort twin itself is 2.63x
libc++'s introsort (539.2 versus 204.9 ms). The algorithm and the compiler
overhead are separate costs.

### Findings in recommended order

The order weighs measured headroom against risk and size. RT-1 and RT-9 are
representation and library changes with no new proof obligations; RT-2 to
RT-4 extend existing analyses; RT-5 to RT-7 are the larger analysis and
transform investments. RT-8 defines the release boundary but showed no
measurable runtime value in this screen. RT-10 is enabling infrastructure.

#### RT-1: generated failure-ABI functions defeat native inlining

**Evidence.** Raising the native inline threshold on unchanged emitted C++
recovers most of several gaps. Minimum of seven samples:

| Probe | Default | Raised threshold | Reference |
| --- | ---: | ---: | ---: |
| `vector-push` | 255.9 ms | 94.1 ms | 40.2 ms C++ |
| `strings` | 107.0 ms | 33.1 ms | 81.0 ms C++ |
| `hashmap` | 437.8 ms | 233.2 ms | 212.2 ms C++ |
| `sort-heap` | 1421.9 ms | 666.4 ms | 539.2 ms heapsort twin |
| `calls` | 1233.7 ms | 922.8 ms | 934.3 ms C++ |

Clang's remarks show why: the two-line `vector::push_back` is costed at 370
against the default threshold of 225, `ensure_capacity_for_one` at 530, and
`string::push_back`, whose growth path is written inline, at 720. Every
failure-capable body is a `bool` failure sibling with an out-pointer result and
failure-record pointer. Its cold blocks construct
`mir_failure_cleanup_scope_v1` objects, read a `thread_local` context, call
`std::abort` paths, and assign a 568-byte `gti_failure_record_v2` (including
its 512-byte message buffer) at each site. None of these paths are marked
unlikely or cold, so the inliner charges them at full weight. The sampled
`sort` profile is dominated by the out-of-line `sort_less_at`, `swap_elements`,
and `sort` siblings. The `vector-push` push loop is one `bl
push_back__gti_mir_failure` per element.

Measurement-only experiments on the emitted C++ bound the fixes:

- marking the failure-only helpers `cold, noinline` recovers part of the gap
  (`vector-push` 255.9 to 189.5 ms, `strings` 107.0 to 76.4 ms, `hashmap`
  437.8 to 268.4 ms, `sort-heap` 1421.9 to 1159.8 ms; no gain for `calls`);
- outlining failure-record construction adds nothing beyond that;
- `noexcept` lowers inline cost (`ensure_capacity_for_one` 530 to 420,
  `push_back` 370 to 335) but not below the threshold;
- forcing `always_inline` on short siblings shrinks binaries but is not
  uniformly faster. A blanket native hint is not a substitute for a GTI-owned
  decision.

**Theory.** Hot/cold splitting of exceptional paths, and inlining decisions
made where the compiler knows which edges are exceptional. GTI's MIR already
identifies every failure edge exactly, but that knowledge is lost when it
reaches the native cost model.

**Safety.** Representation only. The same checks, failure records, cleanup
order, and propagation remain.

**First slice.**

- Emit failure-edge branches as unlikely and keep failure-record construction,
  incoming-cleanup checks, and cleanup scopes in outlined cold code. Delivered
  for failure edges, records and incoming checks (2026-10-12); cleanup scopes
  stay in line because broad cold scope annotations regressed the storage
  proxy.
- Compute an inline hint, or a GTI-side inline decision, from the MIR
  success-path size, excluding failure edges. Not yet needed except for
  `hashmap`; outlining or an `inline` hint without unlikely edges made
  `strings` 1.38x slower.

Measure with the `attribute` screen and code size. A full MIR inliner is
RT-10. `noexcept` is a separate, gated sub-item: the managed-thread runtime
unwinds a worker's native frames with `throw worker_failure_handoff{}`
(`runtime/src/thread.cpp`), so generated frames must stay
exception-transparent. Only bodies proved unreachable from a managed worker
entry may be `noexcept`, unless that terminal handoff moves onto the existing
status channel.

#### RT-2: interprocedural contexts lack narrowing

**Evidence.** In `calls`, `step(x, k)` returns `(x * 3 + k) % 1000003`. One
direct call discharges both checks in `step`. The same call inside a loop that
feeds its result back retains both ("canonical MIR facts do not prove
arithmetic-overflow safety"), although the remainder bounds every return to
±1000002 whatever `x` is. The cause is in
`analyzeMirClosedCallSafetyChecks` (`src/compiler/optimization/safety.cpp`).
Contexts only grow through `mergeContext`, are widened after four updates,
and reject the body after eight. No descending pass follows convergence, so
the widened argument context is never re-tightened from the converged return
summary. The arithmetic ceiling variant puts this probe's remaining gap at
1233.7 to 940.1 ms, against 934.3 ms for C++.

**Theory.** Abstract interpretation's narrowing (descending) iterations after
a widened post-fixpoint (Cousot & Cousot, POPL 1977), or widening with
thresholds taken from callee return summaries.

**Safety.** Proof precision only; the existing proof and replay rules apply
unchanged.

**First slice.**

- After convergence, rerun bounded context rounds that recompute each context
  from converged summaries without widening, keeping the eight-update budget
  as a fail-closed limit.
- Report "rejected after context budget" separately from the generic
  missed-proof remark.

#### RT-3: missing discharge families for conversion, division, and shift

**Evidence.** Remarks show `check retained: unsupported failure discharge
family numeric_conversion_out_of_range/numeric_cast` for every conversion,
including `uint8_t(narrow & 255)`, whose interval the existing mask transfer
already proves to be `[0, 255]`. The architecture lists division,
dynamic-divisor remainder, and shifts as always checked. The conversion ceiling
variant shows `conversions` falling from 201.6 to 163.5 ms on its own, and to
59.2 ms with every ceiling combined (C++: 81.5 ms).

**Theory.** These use the same interval facts already used for add, subtract,
multiply, and remainder:

- **conversion:** the source interval lies inside the destination domain;
- **division:** the divisor interval excludes zero, and either the dividend
  excludes the signed minimum or the divisor excludes `-1`;
- **shift:** the count interval lies in `[0, width)`.

**Safety.** Each removes one exact failure outcome by proof. Division keeps
GTI's defined helpers wherever native C++ has undefined behavior.

**First slice.** Conversion first: it is the most common in byte and text
code and needs no new representation. Division and shift follow with
near-miss tests at every domain edge.

#### RT-4: infallibility is not refreshed after discharge

**Evidence.** `MirFunctionInstance::mayRaiseDefinedFailure` is computed once
during MIR lowering (`src/compiler/mir_lowering.cpp`) from pre-optimization
semantic effects and never recomputed. With every check in `step`
discharged, the emitted body is still a failure sibling with an out-pointer
result. Its dead failure blocks remain, and every caller keeps an `Invoke`,
a success test, and its failure edge. Failure edges also block GTI's own
scalar promotion ("not promoted: body has failure-sensitive control flow").

**Theory.** Recompute the least defined-failure summary on optimized MIR, the
same monotone call-graph fixpoint as `deriveMirProgramEffectSummaries`, then
demote callers' invokes of now-infallible targets to plain calls.

**Safety.** The demotion is a proof that no failure outcome remains in the
callee. It must be replayed independently, like other transformation records.

**First slice.** An analysis plus one editor transform, `invoke-to-call`, with
replay. It also feeds RT-1 and helps O-MIR-08's scalar-promotion prerequisite.

**Status (2026-10-11).** The canonical derivation now proves structural
failure-freedom ([MIR](../architecture/mir.md)): a body with no live failure
source other than exact static calls to proved targets is failure-free
whatever its types, so lowering already gives its callers plain calls. The C++
backend's plain-call vocabulary was completed for every result family those
calls produce. Measured at `-O2` against `01ce4297`, this step is neutral at
run time: every runtime-audit probe, and the S-ALLOC-07 storage workload from
#169 (process 37.3 ms against 37.3 ms, resource 64.1 ms against 64.3 ms), is
within noise, and the probes' text sections are unchanged, because clang
already deletes failure paths it can prove dead after inlining. The probes' hot
callees (`step`'s checked multiply and remainder, the accessors' bounds checks)
still carry failure sources in canonical MIR and become failure-free only after
the optimizer discharges them. The runtime benefit therefore still depends on
the refresh above: effect derivation on optimized MIR with an independently
replayed Invoke-to-Call edit.

**Reach of the refresh (2026-10-12, main `94470419`).** On the `-O2`
optimized MIR of every runtime-audit probe, a function qualifies when every
local failure outcome carries a discharge and every propagating call targets a
function that already qualifies. Four functions qualify: three small helpers in
`matrix` and one in `expected`. The other probes' hot callees keep an
undischarged overflow check (`calls`, which is RT-2's loop-carried context) or
call stdlib paths that can genuinely fail, such as allocation. The refresh also
needs real failure-route retirement. `rewriteMirUnreachableBlock` refuses any
body with failure records, drops, loans or cleanup boundaries, and the
verifier requires one Invoke per producer record. So the refresh is an
invariant-heavy MIR change with little current reach. Its value grows as RT-2,
RT-3 and RT-5 discharge more checks, and it should follow them.

#### RT-5: no relational facts between indices and lengths

**Evidence.**

- In `matrix`, the `i * n + j` arithmetic is fully discharged, but the public
  `index_bounds_check(index, live)` inside `vector::operator[]` is retained.
  The inner loop keeps two `cmp`/`csel`/`tst`/`b.ne` checks per element and is
  not vectorized. The bounds ceiling variant shows 85.8 to 24.5 ms against 22.7
  ms for C++: the retained bounds checks are the entire gap.
  Revisited for 0.397.2 and 0.397.4. The probe's `n` is a constant, so
  `main`'s closed-call contexts prove both accessor checks with intervals once
  each vector's live length reaches `multiply`. 0.397.2 keeps the length
  through floating construction and stores; 0.397.4's capture analysis keeps
  it through the opaque allocation behind the later vectors. The inner loop
  now has no bounds or overflow check, and the probe runs 82.0 to 40.1 ms
  against 17.4 ms for C++. The remaining gap is the accessor's
  failure-capable call shape: its calls stay Invokes after the check is gone,
  which defeats native inlining (RT-1, RT-4).
  Revisited in October 2026: raising clang's inline threshold changes nothing
  for this probe (54 ms against 23.5 ms for C++), so call shape is no longer
  the gap. `raw_address<T>` is a union over `T*` whose reads go through
  `memcpy`, so clang treats the vectors' data pointers as possibly aliased,
  reloads them on every iteration and does not vectorize the inner loop. A
  representation or alias fact for that address carrier is the next
  prerequisite here.
  A runtime `n` would still need this relational domain.
- `string::find(char)` once iterated `index < this.count` and indexed with
  `this[index]`, whose check is the same comparison. The standard library's
  loops of this shape (`find`, `equals`, `compare_bytes`) now read through
  `read_unchecked` under `unsafe`, so no such check remains in them.
- The heapsort's `root < end / 2` and `child + 1 < end` guarantee its index
  checks.

The interval domain cannot express `index < length` for a runtime length, and
the check sits in a callee.

**Theory.**

- Weakly relational domains: Pentagons, which add strict upper bounds `x < y`
  to intervals (Logozzo & Fähndrich, SAC 2008); or octagons on small variable
  packs (Miné, HOSC 2006).
- Demand-driven inequality-graph elimination on SSA with branch-refined names
  (ABCD; Bodík, Gupta & Sarkar, PLDI 2000).

GTI MIR already has dominance, branch predicates, and exact length-changing
effects, so a loop condition `i < v.size()` can dominate `v[i]` while MIR
effects prove the length invariant. Crossing the accessor needs either RT-10
inlining, or a callee *check precondition* summary (`index < receiver.length`)
discharged at the call site and paired with a specialized unchecked body.

**Safety.** Proof-carrying discharge with the existing replay; mutation,
aliasing, and opaque calls invalidate relations exactly as they invalidate
intervals today.

**First slice.** A body-local strict-upper-bound relation between scalar
places and prefix-storage length, consumed by the existing public
logical-size-check discharge.

**Status (2026-10-13).** The first slice is implemented, with orders between
immutable MIR values rather than between places. A comparison of two values of
one integer domain records the order its taken edge implies. A public
logical-size check in the same body is discharged when its operands denote the
ordered values: the same value, loads of one place that no possibly aliasing
write or barrier has retired, or one still-current owned field. Writes never
retire an order; redefining a value in a later loop iteration does, and joins
keep only orders established on every path. See the
[admitted operations](../architecture/optimization.md).

On its own the slice changes no program. A differential over the 1,367
corpus programs that build, and over the runtime-audit probes, discharges no
additional check, and every probe binary keeps its size; see
[performance-tooling.md](performance-tooling.md#rt-5-body-local-indexlength-orders-2026-10-13).
The checks that remain are in accessors. A ceiling measurement of crossing
`vector::operator[]` found Clang already removing the checks in plain read and
copy loops once it inlines the accessor. The one shape that gains is an
in-place update whose bound is hoisted into a local, about 20%. Every store
loop is limited more by aliasing between element stores and the vector's own
fields, which forces reloads on each iteration and prevents vectorization, the
same cause as the `matrix` gap above. Crossing therefore waits on either RT-10
inlining or a precondition specialization with a client that measures, and
that aliasing representation is the larger lever for store loops.

#### RT-6: loop versioning for affine accesses

**Evidence.** In the `matrix` inner loop, both indices are affine in `j` over
loop-invariant vectors whose length is unknown statically. Relational facts
alone cannot prove them when the sizes are unrelated to `n`.

**Theory.** Range-check elimination by loop versioning, or check hoisting.
Before the loop, check the extreme affine index against the loop-invariant
length. Run an unchecked copy when it passes and the original checked loop
otherwise (Gupta, LOPLAS 1993; Würthinger, Wimmer & Mössenböck, PPPJ 2007).

**Safety.** The fallback is the unchanged checked loop, so failure origin,
order, cleanup, and partial effects match exactly. This is the
"side-effect-free preflight with an exactly equivalent fallback" that
Milestone 5 already permits. The preflight itself may not fail, allocate, or
observe anything.

**First slice.** One reducible single-latch loop, with affine indices from
O-MIR-06 induction facts and a length-invariance proof from MIR effects.

#### RT-7: counters and reductions in iterator loops

**Evidence.** `count_if`'s `result++` is a checked `ptrdiff_t` increment, a
compare against `INT64_MAX` plus a branch per taken element. That stops the
vectorization the C++ twin gets; the arithmetic ceiling variant shows 51.3 ms
against 63.5 ms for C++, from a 227.2 ms screen median. `vector-push`'s
`checksum += uint64_t(value)` and the `conversions` accumulator behave the same
way. The implemented accumulator family needs a literal integer loop bound, and
iterator loops have none.

**Theory.**

- **Trip-count bounds:** a loop over a container executes at most `length`
  iterations, and prefix-storage length is bounded by its allocation, so a
  counter incremented at most once per iteration from zero cannot overflow.
- **Wide speculation with exact replay:** for sums whose element envelope is
  unknown, compute in a wider or wrapping accumulator with a sticky overflow
  flag. On overflow, discard it and rerun the original checked loop from the
  saved entry state.

**Safety.** The replay variant is admissible only when the body's effects are
confined to non-escaping locals that the replay restores. The failure is then
reported at the identical iteration and site, and no partial effect is
observable. This is the general form of the guardrail in Milestone 5. It must
not be applied to a body with stores, calls, or cleanup.

**First slice.** The counter rule on range-`for` loops, which needs no
speculation.

#### RT-8: representation self-checks and dynamic drop flags

**Evidence.** Every generated class-typed local is a `mir_lifetime_slot` with a
dynamic `engaged_` byte. Each `get()` checks it and calls `storage_error`;
construction and drop check it too. The `vector-push` push loop reloads the
slot byte on every iteration. The slots ceiling variant shows no change
distinguishable from noise in any probe (for example `vector-push` 255.9
against 267.5 ms, `strings` 107.0 against 103.2 ms).

**Theory.** Static drop elaboration in the style of Rust MIR: where verified
MIR knows a place's initialization state at every point, emit a static drop
and no flag. Keep dynamic flags only for conditionally initialized places.

**Safety.** Category 2 above. These invariants are already proved by MIR
verification, so the release profile may omit them while debug and
verification builds keep them. GTI-observable behavior is unchanged.

**Priority.** Low until measured. The screen shows no runtime value at its
noise level, so this finding defines the release boundary rather than a
demonstrated speedup. Measure it first on cleanup-heavy code (many class-typed
locals and early exits) before scheduling.

**First slice.** Omit the read-while-empty assertion in optimized builds
where MIR verification proves definite initialization. Then elaborate drops
statically for unconditionally initialized locals.

#### RT-9: library algorithm and data-structure choices

These are library changes with ordinary tests, not compiler work, but the
screen shows they are among the largest single wins.

- **Sort (delivered in 0.410.1; see the status below).** `std::sort` was
  iterative heapsort. On identical C++ the heapsort
  costs 2.63x libc++'s introsort. Introsort (or pattern-defeating
  quicksort) keeps the contract: in place, unstable, O(n log n) worst case,
  strict weak order required. An inconsistent comparator still cannot cause
  out-of-bounds access, because every access stays checked or proved.
- **Growth fast path.** `vector::push_back` calls `ensure_capacity_for_one`,
  whose growth path (`reserve`, allocation, relocation) inflates the hot
  function. The append then rechecks capacity that the caller just ensured.
  Split growth into a separate, cold function, as C++ libraries do, and let
  the closed-call prefix-capacity facts discharge the recheck. `string`
  follows the same pattern.
- **Hash map nodes.** Each `unordered_map` probe step pays a vector bounds
  check, a checked `node - 1`, a non-empty owner check, and a remainder zero
  check. Those follow from container invariants that no local analysis sees.
  An open-addressing or flat layout would remove the indirection and most
  checks. The existing ledger already requires allocation, cache, and address
  evidence before any representation change, and this screen supplies the
  first such evidence.

**Status (2026-10-12).** The sort item is delivered in 0.410.1. `std::sort` is
bounded iterative introsort: a pseudo-median-of-nine pivot, a bounds-guarded
Hoare partition, insertion sort for short ranges, and a heapsort fallback that
keeps the worst case O(n log n). Both overloads share it through small phase
functions that borrow the comparator. On the Mac mini at `-O2`, sort-only time
is 1.76x faster on the `sort` probe, 2.09x on 200k strings, and 1.03x on a
lambda-comparator integer sort; the probe is now 3.67x its C++ twin instead of
6.37x. Keeping the phase functions small matters: a first single-function
version stopped clang inlining the failure-ABI `operator[]` and regressed the
comparator workload, which is RT-1's inlining cost in a library shape. The
growth fast path and hash-map items are unchanged. Measurements are in
[performance-tooling.md](performance-tooling.md#rt-9-introsort-for-stdsort-2026-10-12).

#### RT-10: MIR inlining as the enabling transform

Milestone 6 lists inlining as unimplemented. RT-1 shows the native compiler
will not reliably inline GTI's small accessors. RT-5 shows the checks that
matter live inside those accessors (`operator[]`, `sort_less_at`,
`swap_elements`). A bounded MIR inliner for small, non-recursive, exact
static targets would let GTI's existing proofs see caller facts. It is the
largest investment here and requires the gates the plan already names:
provenance composition, cleanup and temporary lifetime correctness, failure
edge splicing, a cost model, and replay. A measured precursor is RT-1's
emission-level hint, which captures much of the native benefit without the
proof benefit.

### Not recommended

- **An unchecked release mode for GTI-observable checks.** It changes
  program meaning. Category 1 checks leave only by proof or exact fallback.
- **A higher global native inline threshold or blanket `always_inline`.** The
  threshold experiment is a measurement ceiling (code size grows about 12%),
  and forced inlining was not uniformly faster. GTI should decide from MIR
  facts (RT-1, RT-10).
- **`-fno-exceptions` or blanket `noexcept`.** The backend contains native
  exceptions with `try`/`catch`, and worker handoff unwinds by exception;
  see RT-1.
- **Fast-math or contraction.** GTI floating-point semantics forbid
  reassociation and contraction.

### Code size note

Each probe executable is about 400 KB against about 35 KB for its C++ twin,
because the emitted program carries the whole reachable stdlib and support
preamble (about 40,000 C++ lines). This is not a measured runtime cost here.
Closed-world MIR reachability for executables would reduce instruction-cache
footprint and native compile time; the plan already treats reachability as
removal authority as a gate.

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
9. Add infrastructure for a defined general optimization contract, operation
   family or production client, with bounded analyses and verification. Backend
   cutover is delivered; neither a prior bottleneck nor a proxy speedup is an
   admission requirement under ADR 012.
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

HIR constant replacements remain only as a transitional shadow comparison for
the existing fold. They are not a backend or non-body representation input.
The shadow result cannot authorize a new CFG or dataflow transformation;
canonical semantic/lowered facts supply declaration constants. Remaining work
is retiring the bounded shadow comparison, not split executable-body authority.

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
for the existing MIR fold's shadow comparison. It is not an emitter input or
the API for new passes.

### MIR

MIR is the implemented optimization authority for executable bodies. It owns:

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

MIR is not yet ready for every optimization. Body-local alias/escape,
verified effects, exact closed-world reference facts, authoritative
lifetime/cleanup and bounded target/layout facts are available. Memory
versioning, broader interprocedural pointer identity, transform-specific
lifetime composition, target/ABI representation and additional proof families
still need explicit contracts. The capability gates below
make those gaps explicit instead of allowing passes to guess.

## Proposed Pipeline

The implemented steady-state authority path is:

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
optimization and backend generation. Reuse compiler-owned target/layout facts
when a transform needs them; extend a backend-neutral layout/ABI query only
for facts the current contract cannot express. Do not introduce a parallel
target authority or query a C++ compiler from a pass.

The LSP continues to stop at `FrontendResult`; it does not need optimized MIR
for semantic language queries. Developer IR views may explicitly request the
optimization pipeline.

## Optimizer Ownership Model

The current `MirProgram` exposes only const accessors and grants mutation to
`MirLowerer`. Preserve that encapsulation. Do not make every program vector
public to enable passes.

The implemented optimizer-owned mutation boundary keeps canonical source and
candidate MIR separately. Its public request/result shape is:

```cpp
struct OptimizationRequest {
  const HirProgram &hir;
  MirProgram mir;
  OptimizationLevel level;
  TargetInfo target;
  OptimizationOptions options;
};

struct OptimizedProgram {
  MirProgram sourceMir;
  MirProgram mir;
  OptimizationReport report;
};

class MirEditor;
```

The optimizer takes MIR by value or unique ownership and returns a new owned
result. The unoptimized `FrontendResult::mir` stays available for debugging and
differential tests. The driver passes an owned MIR value to the pipeline; shared mutable MIR is
forbidden. Controlled editor queues and core MIR repair utilities are the
existing mutation seam, not a missing extraction task.

Editors provide operations such as replacing an operand, replacing a
terminator, splitting or removing a block, replacing a value with a constant,
and erasing a proven removable instruction. Editors are responsible for
revision tracking and delegate ID repair, reachability, use indexing, and
validation to shared MIR utilities.

Passes must not update `valueUses`, definition locations, block reachability,
or IDs ad hoc. A central repair/canonicalization step either incrementally
maintains those facts or rebuilds them after a pass. Start with rebuilding for
simplicity; introduce incremental maintenance when its invalidation and cost
model are coherent, then measure representative compile/memory behavior.

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

Declarations remain under `include/gti/`; optimization implementations are
already compiled under `src/compiler/optimization/` and
`src/compiler/optimizer.cpp`. The [library migration record](archive/compiler-library-migration.md)
is historical, not a prerequisite to start a new optimizer task.
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
  virtual PassOutcome run(MirPassContext &, MirEditor &) = 0;
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

Do not adopt an analysis merely because a mature compiler has one. A general
operation-family/optimization contract can justify infrastructure before its
first transformation; test its invariants, consumers and invalidation and
measure costs at acceptance.

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

Use the implemented checked effect summaries for admitted exact calls;
unknown or unproved virtual calls retain conservative defaults. Intrinsics
receive explicit summaries by semantic
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
- preserve the implemented exact floating-point contract: NaN, signed zero,
  rounding, comparison, no reassociation and no contraction;
- preserve target-conditional selection and use one target throughout;
- never derive signed overflow, shift, division, remainder, conversion, or
  indexing semantics from the optimizer host language.

## MIR Evolution Guardrail

The compiler-owned exhaustive classification and runtime vocabulary gate are
now implemented in `optimization/coverage.cpp`; see the canonical
[conformance contract](../architecture/optimization.md#language-growth-conformance-gate).
The gate names current consumers/barriers without duplicating `mir_effects`.
Compile-failure growth probes supplement the existing effect-table census.
Registering an enum member is necessary but does not replace the semantic,
transfer-function, verifier, backend, or equivalence obligations below.

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
| copy propagation | unimplemented general transform | MIR values and places | dominance; exact type/access preservation; move and loan barriers; bounded replay and cost model |
| redundant load/store removal | analysis gates partly complete; transform absent | MIR places | place alias/escape; exact call effects; memory versions or a proved local substitute; lifetime boundaries |
| bounds/check elimination | bounded read/storage/arithmetic families implemented | MIR checked operations | integer semantics; range/predicate analysis; explicit check identity; proof reporting |
| loop optimization | analysis gate complete; transform absent | MIR loops and values | natural-loop forest; invariants/inductions; transform-specific legality and replay; cost evidence |
| devirtualization | bounded local and closed-world reference-parameter exact-receiver slices implemented; owned-pointee, `this`, pointer and specialization forms deferred | verified MIR calls, local loans, closed-world call-site facts, concrete instances and override identities | exact confined or closed-world receiver, initialization dominance or exact caller facts, authoritative effects, failure/cleanup compatibility, independent replay |
| inlining/specialization | general transform absent; RT-10 defines the remaining goal | HIR call graph plus MIR bodies | conservative effects and callee-set facts; recursion handling; cost limits; provenance and ordered-input composition; cleanup/lifetime correctness; independent replay and optimized performance evidence |

Do not implement a pass when one of its gates is represented only by C++
emission behavior. Add the missing backend-neutral fact first.

The implemented local sequence is:

1. MIR constant folding;
2. conditional and switch simplification;
3. unreachable block removal;
4. dead non-trapping value removal;
5. sparse propagation and another CFG cleanup; and
6. range-based check elimination.

Loop/range and place alias/escape analyses are implemented foundations, not
blanket loop/memory transformation authority. O-MIR-08/09 and standalone
O-MIR-10 retain historical measured-defer/parked records. Reopening those goals
follows ADR 012 and the missing proof contracts; O-PERF-02's historical choice
is not a permanent bar on general inlining or memory architecture. The maintained sequence in
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
General infrastructure may grow from architectural requirements under ADR 012;
identify its production consumers and validation, not a prior speedup threshold.

- Maintain deterministic MIR printing, public validation utilities, effect
  traits and the bounded controlled editor. Broader editor/analysis management
  needs a defined optimization contract, consumers and validation, not merely
  resemblance to another compiler's framework.
- Return an `OptimizedProgram`; keep `-O0` unchanged while changed passes carry
  explicit reports and fresh verification.
- Historical Stage A preserved the HIR replacement/emitter bridge. That bridge
  is now retired from production emission; only the bounded fold shadow
  comparison remains.
- Verify that `-O0` is structurally identical and every queued edit is atomic.

### Stage B: prove the shadow path

Status: complete for the compatibility fold. Primitive identities plus admitted
comparison, logical, floating arithmetic, and conversion computations are
implemented and cross-checked. New passes do not use shadow authority.

- Retain or retire the bounded fold cross-check as test evidence; its body
  family already consumes optimized MIR. It cannot authorize new transforms.
- Port another compatibility fold when general optimization architecture or an
  active migration phase requires it; validate its production consumer.
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
HIR compatibility replacements remain solely for the existing fold's shadow
comparison. Declaration constants use canonical semantic/lowered facts.

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
- Preserve deterministic applied/missed remarks in the implemented reporting
  surface; add exact new-family coverage.
- Test pass idempotence where the pass claims canonical output.
- Add randomized or property-driven equivalence testing only after deterministic
  printers and failure classification exist.

Measurements validate performance acceptance and inform cost/priority; they
are not permission to design general architecture and never prove correctness.
CI should not use wall-clock pass thresholds. Follow the performance tooling proposal for
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

These questions belong in code review and the repository's
`implement-language-feature` and `verify-change` workflows. If a question cannot be answered from GTI IR,
identify and implement the missing architectural fact before admitting the
optimization; C++ behavior cannot supply its proof.

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

Status: complete; broader infrastructure needs a defined contract and evidence,
not a benchmark-first admission gate

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

**Implementation status:** bounded fixed-array bounds, arithmetic overflow,
contextual source-prefix/logical-size and indexed-content proof families are
implemented at O2/O3 with canonical analysis, typed metadata, controlled edits,
independent replay, reporting and near-miss/differential coverage. The original
sparse/private-prefix capability discharge engines are retired with their
storage migration; their historical evidence is not a current pass inventory.
Conversion/division/shift proof families, relational runtime lengths, cyclic
promotion and broader memory/loop transforms remain work. Alias facts preserve
proven disjoint state; they do not themselves authorize check removal. The
[current architecture](../architecture/optimization.md) owns the exact admitted
operations and current report schema.

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
- `examples/012-fixed-arrays.gti` already discharges the canonical short loop,
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
widening/transfer functions. Reconsider cyclic promotion as general SSA/loop infrastructure when its
cost model and correctness obligations justify it, even before a benchmark
regression. A transforming O-MIR-08 client still needs a replayable fact that
reaches real source MIR and optimized performance acceptance. The safety client must consume only loop, dominance, alias,
effect, and fixed-array facts, record near misses, invalidate safety analysis
after edits, replay coherently, use `MirEditor`, preserve O0 identity
and all checked failure behavior, and remain backend-neutral.

### Milestone 6: interprocedural optimization

Status: the first bounded contextual proof-summary client is complete under
`O-PERF-02`; broader interprocedural transformation remains deferred.

- Conservative per-instance call effects and exact closed static-summary
  queries are implemented and independently verified.
- One deterministic whole-program analysis transports exact scalar arguments,
  returns, mutable object fields, structurally admitted source-prefix state
  and indexed-element envelopes through concrete
  source-defined static calls and constructors. It uses bounded rounds and
  per-context updates, stores values separately from effects, and rejects
  recursion, native or virtual calls, owning transfers, synchronization,
  incomplete call families, and unsupported cleanup/lifetime shapes.
- The analysis is snapshot-scoped. Any semantic body edit invalidates its
  program-wide cache and all derived body safety facts; independently replayed
  metadata attachment explicitly preserves the immutable result. Replay
  validates all claims against one source and one candidate snapshot rather
  than recomputing the whole program per claim.
- Closed-world executable artifact lowering is integrated; full analysis and
  reached compiler-trusted body selection remain distinct contracts. Body
  cloning and general MIR inlining are not implemented.
- Local exact-receiver dispatch and the closed-world exact-reference-parameter
  refinement are implemented. Broader owned-pointee/`this`/specialization
  clients require exact call/root, lifetime, cleanup, layout and cost contracts;
  closed-world facts must be established, never assumed.

Acceptance criteria:

- unknown or virtual calls remain conservative without exact proof;
- provenance spans inlined or specialized bodies coherently;
- benchmarks show value across workload categories;
- a future backend consumes the same transformed calls.

## Unresolved Gates

Resolve these before the pass families that depend on them:

- memory versioning for transformations that must distinguish intervening
  writes; introduce MemorySSA when existing place and effect facts cannot
  express the required general reasoning soundly and maintainably, without a
  benchmark-first entry gate;
- transform-specific loop legality, failure-order proof, and cost evidence;
- transform-specific composition of the existing temporary/lifetime/drop
  authority, including ordered arguments and new cloned/inlined failure edges;
- backend-neutral object layout, data layout, virtual tables, and calling
  conventions;
- the closed-world boundary once separate compilation or dynamic linkage exists;
- cost models and size policy based on representative benchmark data.

Each gate must become a language contract or backend-neutral IR fact. Resolving
one inside a pass or `CppEmitter` is architectural drift.

## Documentation And Release Effects

Apply current release policy per change: observable optimization behavior,
CLI/report surface or a user-visible fix may require a VERSION change; internal
refactors, authority moves, tests and plan updates do not. For later milestones,
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
