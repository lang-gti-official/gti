# GTI Compiler Roadmap Status

> **Plan status:** Implementation checkpoint and future-work ledger. It does
> not define current language semantics.

Status: implementation checkpoint

Checkpoint version: 0.289.0 plus the completed hard-cutover audit. The
backend-authority campaign's per-release record lives in
[`implementation-sequence.md`](implementation-sequence.md).

This document records where the compiler currently sits against
[`roadmap-to-1.0.md`](roadmap-to-1.0.md). The roadmap remains the durable
capability and release plan; the operational dependency queue lives in
[`implementation-sequence.md`](implementation-sequence.md). This checkpoint is
the shorter evidence ledger that future passes should update when they complete
or materially unblock a milestone. The grammar and semantic specification
remain authoritative for shipped language behavior.

## Current Position

### Outcome-first planning pivot

[ADR 012](../decisions/012-outcome-first-systems-readiness.md) changes the
planning priority without changing current language semantics or discarding
the architecture already built. Compiler work now names a user-facing program,
API, or workflow and prefers the smallest sound vertical slice that makes it
real. Infrastructure remains high priority when it fixes correctness or is the
nearest prerequisite of one of those slices.

The `1.0` label is now a soft, revisable readiness goal. GTI should claim it
only as a full-featured language ready for serious systems programming; it is
not a boundary for postponing essential capabilities. The former version
horizon has been replaced in the maintained ledger by durable-rule,
systems-ready, bounded-first, design-first, and later-breadth roles.

This promoted bounded native records and pointer-only opaque handles (now
complete), the remaining callback boundary, an application-visible allocator and arena/pool client, payload
enums and exhaustive matching,
cleanup-correct error propagation, exact domain operators, one associative
container, and the minimal public concurrency profile into systems-readiness
work. Advanced forms remain gated, and the existing ownership, failure,
evaluation, IR, and backend requirements still apply.

Source loading, parsing, semantic selection, concrete HIR discovery, and MIR
lowering remain directional, and the C++ backend does not decide overloads,
ownership, dispatch, or language validity. Production source-body execution is
no longer split: all 2,487 reviewed identities across 57 examples emit through
the general verified-MIR route. The public no-MIR emitter and executable AST
statement route are removed. An exact census, a two-endpoint native corpus
oracle, structural mutation tests, and focused runtime matrices guard the
cutover.

The remaining backend boundary is generated representation. Hosted entry and
program initialization have explicit thunk contracts; some structural,
callable, lifecycle, native, and concrete-instance adapters still derive shape
from sealed frontend facts. This is a future second-backend concern, not a
source-body compatibility route.

### Historical backend campaign notes

The detailed release checkpoints below record the incremental migration. Their
body counts, family selectors, and compatibility references describe the state
at those releases and are superseded by the hard-cutover position above.

MIR v20 supplied the first function-level failure-effect foundation needed by
that closure. MIR v21 retains that vector and makes
`MirDefinedFailureEffects` cover functions, constructors, and destructors.
Functions, constructors, and destructors carry the bounded
`mayRaiseDefinedFailure` dimension plus applicable definition provenance. The
function component retains the acyclic closed scalar/static-call proof and adds
the exact class-default-cleanup shape. A separate closed proof covers exact
passive-scalar-class constructor initializer stages, their source destructors,
and free-function graphs consumed atomically by the production
`owned-lifecycle-call-v1` family. The lowerer
verifies a provisional all-conservative program, derives the three-kind result,
then lowers and verifies the exact final facts. Generic verification permits
conservative `true` but rejects any `false` that it cannot independently prove.
A static GTI call records `None` exactly when its target is proved false and
`DirectCall` otherwise.

The 0.144.0 backend campaign first completes `class-default-cleanup-v1`, the fourth bounded
production MIR family and the first with construction and normal cleanup. It
admits exact generated-default construction of empty concrete class locals in
one root scope, loads the scalar return value before cleanup, and executes
reverse lexical `Drop` instructions through one normal cleanup boundary. The
matching public source destructors are independently emitted from verified MIR
and limited to exact scalar-literal assignments to mutable top-level globals.
The C++ representation uses strict raw-storage lifetime slots with explicit
construction and destruction, so native RAII cannot hide a missing MIR drop.
`mir_backend_class_default_cleanup` and its runtime companion prove exact
selection, fail-closed HIR/MIR and summary mutations, and O0/O1/O3 execution at
C++20/C++23. Declared constructors, fields or bases, nested scopes, branches,
calls, loans, and failure-capable cleanup remain compatible.

The next 0.144.0 backend phase completes `owned-lifecycle-call-v1`, the fifth bounded
production MIR family and the broader failure-free construction/normal-cleanup
phase. One atomic acyclic free-function graph may construct, move, pass,
transfer, and drop exact passive-scalar-field owners whose source constructor
and destructor schedules are proved by MIR v21. Production uses explicit
lifetime slots and disengages every verified drop before native destruction;
source/MIR schedule coherence and reverse HIR caller closure keep the whole
component either selected or compatible. Dedicated structural mutations reject
initializer, operation, place, CFG, return, call-input, reverse-edge, transfer,
and drop drift. Its runtime gate proves cleanup order without double cleanup at
O0/O1/O3 under C++20/C++23. Checked lifecycle, comma, loops, switches, class
results, and other unsupported connected shapes remain wholly compatible.

The 0.150.0 checkpoint measures the true cutover position and makes it a
tracked gate. Production emits exactly one body from verified MIR across the
57-example corpus. The six completed families select correctly on their own
fixtures but effectively never match shipped code, because each carries a
whole-program selection contract. `CppMirBodyEmitter`'s 1953-of-2429
emitter-ready figure is a precondition rather than emission: the class is a
fail-closed analysis gate with no text-emission step and no production caller.
The `cpp_mir_body_emitter` suite now sweeps the example corpus and asserts that
every example still reaches verified MIR, that no frontend-produced body is
structurally incoherent, that no invalid-shape issue is raised, and that
readiness does not regress below a floor. The first of those matters most: a
change that breaks compilation silently improves every other figure, which
masked two unsound attempts earlier in this campaign.

The 0.154.0 checkpoint adds per-body static calls to `scalar-cfg-v1` and
fixes an HIR receiver-fidelity defect the work exposed. Calls no longer
require the closed-graph scalar-direct-call selection: an eligible call names
a static proved-failure-free source free function through the exact ordered
`CallInput`/`Call` stages already verified by that family, the HIR gate now
consults the MIR failure summary so a may-raise or conservatively-true target
declines gracefully instead of reaching the fail-closed shape gate, and the
coherence layer carries the exact call-input correspondence walk. The
closed-graph family keeps selector priority for the whole components it owns.
Composition now works end to end: a concrete-class member reading its own
field and calling a free helper emits wholly from verified MIR.

The HIR fix: lowering attached an implicit receiver to every unqualified call
made inside a member body whose target was merely non-static, which described
free-function calls as receiver-bearing. The guard now also requires the
resolved target to be a member (`ownerClass != 0`), so a free-function call
inside a member carries no receiver at any layer.

Corpus emission is unchanged at 32 bodies: the examples' free-function calls
target checked-arithmetic functions whose summaries are not provably
failure-free, so no additional example body qualifies yet. The capability is
proven by the focused gates instead, and its yield grows as the failure-free
closure widens.

The 0.153.0 checkpoint adds the `--emit-mir` inspection surface. Direct mode
gains a `MirBackend` selected exactly like the C++ and native-header backends:
the same complete frontend, optimization compatibility check, and MIR
verification run first, then the backend serializes the verified optimized
snapshot in the deterministic versioned `mir-v*` format owned by `MirPrinter`.
The artifact defaults to `<entry>.mir`, honors the loaded-source overwrite
guard, is byte-deterministic across runs, and is mutually exclusive with the
other emission modes. This gives the backend-authority migration a first-class
way to inspect exactly what executable backends consume without reaching for
test probes.

The 0.152.0 checkpoint adds This-rooted scalar field reads to
`scalar-cfg-v1` and produces the first material production emission. A
read-only member of a concrete class may now read scalar fields through
`this`: the HIR gate admits the receiver as a class-typed carrier and
`this.field` member reads whose object is exactly the receiver, the shape
gate admits This-rooted places (the bare carrier plus single-`Field` scalar
projections, each receiver use materializing its own place keyed by source
value), boundary read events may name admitted This keys (reinitialize events
remain binding-only, so no write channel opens), coherence maps each This
place back to its exact `This` or member-access source value, and emission
binds each field place by reference to the live member spelling while
skipping the carrier. Reads of another object's fields, projection chains,
writes, and mutable receivers all stay compatible, with the foreign-object
read covered as an explicit near miss.

Measured effect across the 57-example corpus: production emission rises from
1 to 32 verified-MIR bodies across 20 files, and the differential oracle now
compares 17 sources with behavioral agreement on every one and zero
disagreements. One family defect was found and fixed during the change: the
first canonical-place rule for This roots flagged two bare carriers as
duplicates, which broke a two-field member in the compiler suite; carriers
are per-use places keyed by source value, mirroring temporaries.

The 0.151.0 checkpoint clears the per-declaration member boundary in
`scalar-cfg-v1`. The selector now resolves a member per MIR instance: an
ordinary non-static, non-virtual, non-operator read-only member of one
concrete non-generic class instantiation is emitted from verified MIR through
the ordinary deferred qualified definition, while zero instances, a generic
owner (including a single generic instantiation, whose substituted HIR body no
longer matches the source declaration shape), several instantiations, a
mutable receiver, or any use of `this` decline gracefully to compatibility.
Free functions keep the exact-one-instance fail-closed contract, and owner
drift is still rejected in both directions. The focused gate covers positive
member selection, the `this`-reading near miss, and the single-generic-owner
near miss. Corpus emission is unchanged at one body, because every eligible
concrete-class member in the examples reads fields through `this`; the next
yield boundary is This-rooted scalar field reads, which requires coordinated
HIR-gate, shape-gate, coherence, and emission extensions plus a reference
binding for field places.

Selector relaxation is not the next increment, for a structural reason rather
than the one recorded earlier. Instrumenting the real `selectedMirScalarCfg`
shows first-match declaration rejections of operator 1124, member 944, generic
892, compiler-private 798, `constexpr` 432, non-scalar parameter type 268, and
entry 114, with no function reaching the body-shape gate. The earlier claim
that relaxing those gates yields nothing came from an approximate model that
wrongly excluded `Lifecycle`; the real gate already admits it as a pure
full-expression marker. Admitting members was then attempted and reverted: the
selector requires exactly one MIR instance per source declaration with empty
type arguments, so a member of a generic class makes the backend fail with
"missing the MIR instance for an eligible source function". Emission is keyed
per source declaration, so the next increment needs per-instance body emission. Of those 899, `Lifecycle` is required by all 899, `Call` by 647,
`CallInput` by 420, failure edges by 354, drops by 47, `Construct` by 26, and
loans by 16. `Lifecycle` is already `RepresentedByMir`, so the first emission
increment is `Lifecycle` plus the scalar-CFG set, then `Call`/`CallInput`. The
remaining wiring is a representation-row builder reusing the emitter's naming
authority, the reserved text-emission step, and `CppBackend` integration with
differential comparison against the compatibility emitter.

The 0.149.0 checkpoint admits trivial-commit assignments and establishes that
the remaining cutover debt is representation work rather than eligibility
widening. MIR v29 routes a checked assignment whose destination is trivially
destroyed and which runs no lifecycle event, covering the narrowing compound
forms that keep a closed instruction; a failure writes nothing there, so the
edge needs only ordinary cleanup. This removes 22
`MissingCheckedFailureControlFlow` occurrences with no debt displaced.

Two further eligibility widenings were attempted and reverted on evidence,
and both are now proved to need new representation rather than a predicate
change:

- Operations that produce a loan (132 occurrences, the largest remaining
  exclusion inside supported bodies). Registering the loan into the enclosing
  scope only after the instruction is not sufficient, because the loan record
  and its reborrow parent link already exist; the loan-flow verifier then
  rejects the failure edge for ending a parent before its child, which broke
  16 examples. This needs a success-edge loan on the invoke terminator plus
  loan-flow liveness starting at the normal successor.
- Constructor and field-initializer bodies, reverted in 0.148.0 for the
  partial-construction leak recorded below.

The remaining `MissingPackExpansionMir` debt is likewise upstream: a call whose
argument is a `PackExpansion` returns no HIR call plan, so it also accounts for
part of `MissingCallInputScheduleMir`. Expanding a pack into concrete ordered
elements is semantic work, matching the treatment `PackFold` already receives.

The 0.148.0 checkpoint admits state-preserving reads to the routed family and
records a measured prerequisite for the constructor slice. MIR v28 stops
treating any ownership event as a blanket disqualification: a `Read` that
leaves an available place available on a reachable edge has nothing for a
failure edge to unwind, matching the benign-event rule the defined-failure
effect derivation already applies. All 34 such operations in the example
corpus are exactly that shape; routing them removes 34
`MissingCheckedFailureControlFlow` occurrences and moves two more bodies to
emitter-ready (1951 to 1953) with no debt displaced elsewhere.

Enabling constructor and field-initializer bodies was attempted and reverted
on evidence. The flip passes MIR verification and removes 91 occurrences, but
the resulting MIR is unsound: a constructor `TransferOut`s each completed
subobject into `this`, which removes it from the body's temporary and scope
cleanup sets, so a later checked failure propagates through an empty cleanup
block and leaks it. This was confirmed in the two-initializer `std::string`
constructor of example 20, where obligation 1 is initialized on an earlier
success edge, transferred out by the field-initializer `CallBody`, and then
absent from the failure block of a later checked call. Verification does not
catch it because constructor rollback verification is itself the missing
authority. Partial-construction rollback is therefore a hard prerequisite,
not a parallel refinement, and `supportsMirFailureControlFlow` now records
that reason at the exclusion.

The 0.147.0 checkpoint extends ordered failure edges to construction. MIR v27
routes a failure-capable `Construct` exactly like an ordinary call, and a
full-expression-root construction with a cleanup-owning result now initializes
that result only on its success edge. This removes 28 unrouted checked
constructions from the example corpus. It does not move a body across the
readiness line, because every affected body carries other debts, and it does
not complete constructor-side work: a nested construction, a construction that
produces a loan, and constructor and field-initializer bodies themselves all
remain compatible.

Constructor-side evidence for the next slice is now exact. All 14 checked
constructor bodies in the corpus own cleanup-bearing fields, so
partial-construction rollback is genuinely required rather than vacuous; no
constructor instruction currently carries a `constructorInitializer` stage tag
outside the bounded explicit-scalar-field path, so the stages a rollback would
unwind are not yet represented; and the 58 field-initializer bodies each carry
one checked operation with zero drop obligations, so their own rollback is a
no-op but their failure channel would cross the unconverted constructor
`CallBody` boundary. Representing initializer stages, tracking their
subobject obligations, and closing constructor plus field-initializer channels
together is therefore one coherent slice, not three independent ones.

The 0.146.0 checkpoint lands the ordered compound-update family. MIR v26
lowers a same-domain scalar compound assignment or increment/decrement to an
explicit `Load` read, the exact checked arithmetic `Compute`, and an ordinary
`Assign` commit, replacing the closed `Modify`/compound-`Assign` instruction.
The bounded family requires the read, the operation, and the commit to share
one fixed-width integer domain and requires the decomposed arithmetic to
inherit exactly the origin its checked contract names, so an unchecked
operator-indexed update and a narrowing form that folds a conversion into the
same origin both stay compatible. Across the 57-example corpus this moves 37
more bodies to emitter-ready (1914 to 1951), reduces the ordered-compound debt
from 231 to 26 occurrences, and removes a further 103
`MissingCheckedFailureControlFlow` occurrences because the decomposed
arithmetic routes through the ordinary failure edge.

Two remaining debts are now known to be blocked upstream rather than by MIR
scheduling. `MissingCallInputScheduleMir` (162) covers calls whose HIR value
carries no `callPlan`; the parameter types and input kinds are overload-
resolution facts, so staging them is semantic work, not MIR work. The
remaining `MissingCheckedFailureControlFlow` (430: 333 function, 58 field-
initializer, 39 constructor) is dominated by failure-capable `Construct` and
by initializer and constructor bodies, which need partial-construction
rollback before they can carry a failure edge; that is the next coherent
slice.

The 0.145.0 checkpoint restabilizes the tree after the 0.144.0 landing and
extends M-FAIL-01's representation: the hosted owned-arguments golden verifier
now mirrors the generated Stage-E schedule, defined-failure effect derivation
iterates to its fixed point, internal MIR failures report every distinct
verifier error, and five self-aliasing `SemanticType` assignments in the C++
body-emitter scan and HIR receiver peeling were corrected (the emitter bug
mislabeled six coherent example bodies incoherent under ASan-visible
undefined behavior). MIR v25 then makes checked failure-edge eligibility
position-independent: every eligible checked scalar computation, load, and
ordinary call in any nested value position of a failure-control-flow body
carries the verified Invoke/record/reverse-cleanup/propagation contract, with
prepared owner stages dropped exactly once across failure-capable cleanup
chains. Nested cleanup-owning results deliberately keep their re-homed
initialize lifecycle and stay compatible until the remaining owning-result
materialization lands. Across the 57-example corpus this moves 87 additional
bodies to emitter-ready and removes the largest single
`MissingCheckedFailureControlFlow` share (855 to 533 occurrences); ordered
compound schedules, generated construction schedules, call-input staging for
remaining call forms, and partial-construction rollback remain the next
inventory debts.

The current 0.144.0 backend phase completes `scalar-failure-callgraph-v1`, the sixth
bounded production MIR family and the first hosted failure-capable component.
One no-argument `int32_t` entry and its exact acyclic `int32_t` free-function
graph use a private bool/out-result/record ABI. Checked signed and unsigned
integer operations create exact source records, direct calls preserve the
record, and verified failure drops run before propagation without publishing a
result. Atomic selection rejects every external HIR-body incoming edge or
selected-class representation user, dynamic initialization, native/virtual/
normal-ABI edges, cycles, checked lifecycle bodies, and source/MIR drift. The
single hosted boundary reports once and exits 70; its native-exception
firewall exits 70 without manufacturing a GTI record. Structural and runtime
gates cover every admitted operation and failure outcome at O0/O1/O3 under
C++20/C++23. This does not complete broader M-FAIL integration, callbacks,
embedding, double failure, general initialization, or final backend authority.

The 0.143.0 checkpoint completes `M-BACK-01`/`scalar-leaf-v1`. Production
`CppBackend` independently verifies the exact optimized MIR snapshot and emits
eligible non-entry, non-generic fixed-width-integer leaf functions wholly from
MIR, including source literals and optimizer-created identity-fold literals;
a no-op `void` result is also admitted. MIR v20 retains and verifies version
19's exact source or identity-fold provenance used by this path. Unsupported or
incoherent bodies remain wholly compatible, while invalid, stale, or forged
MIR is rejected instead of reinterpreted. The two first-family gates prove
optimized-instruction control and execute selected plus near-miss bodies at
O0/O1/O3 under C++20/C++23.

The same checkpoint completes `M-BACK-02`/`scalar-cfg-v1`. Its whole-body
selector adds fixed-width-integer, `bool`, and `char` scalar computations,
unprojected binding/temporary places, initialization and assignment, branch,
switch, short-circuit, and loop CFG while excluding calls, checked failure,
references, loans, drops, cleanup, and construction.
`mir_backend_scalar_cfg` proves exact selection, compatibility near misses,
optimized-literal control, and source-coherent emitted-CFG control.
`mir_backend_scalar_cfg_runtime` executes the family at O0/O1/O3 under
C++20/C++23. These remain bounded production authority seams, not general MIR
body authority; the later owned-lifecycle phase completed the broader
failure-free closure, while failure-capable and final-authority body families
remain.

The next completed M-BACK-02 phase is `scalar-direct-call-v1`, the third
bounded production MIR family. It extends the scalar-CFG substrate with exact
ordered scalar call-input checkpoints and static calls inside a closed acyclic
graph of source-defined free functions. MIR v20 proves every selected node
`mayRaiseDefinedFailure=false`, and the final calls carry `None` with no stale
failure record or `Invoke` edge. If an independently eligible HIR graph carries
a conservative true summary, production emission rejects the noncanonical
drift rather than silently falling back; header-, body-, dispatch-, and
cycle-ineligible graphs remain compatible. Once the family is selected,
target, input, provenance, graph, or instruction drift likewise fails closed.
Checked/unsupported targets, recursion, members, internal linkage,
`constexpr`, HIR-`for` target graphs, virtual/callable dispatch, ownership,
cleanup, and construction remain outside that family. The dedicated structural
and runtime gates cover fail-closed mutations, IdentityFold evidence, and
O0/O1/O3 at C++20/C++23.

The 0.142.0 checkpoint extends the prepared-argument M-FAIL-01 edge from local
detectors to one ownership-free static direct call with a trivial result. The
nested call receives `Invoke` only when its exact result feeds a later indexed
outer `CallInput` after an owning stage. Its propagation remains un-sited and
forwards the callee record unchanged; failure destroys caller-owned outer stages
in reverse construction order, while the normal path evaluates the nested call
once and continues outer setup. Calls with their own owning parameter or result
state, virtual/callable dispatch, constructors, and backend execution remain
excluded. MIR v18 already contains every required identity, so serialization,
syntax, semantics, formatter, Tree-sitter, LSP, and backend authority do not
change.

The 0.141.0 checkpoint extends M-FAIL-01 through the first nested ordinary-call
argument boundary. An exact local scalar `Compute` or `Load` used as a later
indexed argument after one or more caller-owned parameters have been prepared
now ends in `Invoke`. Its failure edge preserves the fixed record, destroys all
earlier stages exactly once in reverse construction order, and never transfers
them to a callee that did not begin; the normal edge resumes setup and the final
call remains the sole transfer point. MIR verification recomputes eligibility
from exact HIR value identity, the indexed `CallInput`, dominance, and prepared
obligations. No serialized metadata, source syntax, semantic rule, LSP behavior,
or backend authority changed. Nested call propagation and other compound
argument forms remain open.

The 0.140.0 checkpoint completes the bounded M-EXEC-01 parameter/result staging
prerequisite for nested M-FAIL-01 work. MIR v18 gives every eligible class-value
input to an ordinary call one distinct caller-owned prepared-parameter
obligation. Copy initializes that stage, move reparents the exact active source
obligation when present, and the final call transfers each stage exactly once
when the callee begins. An eligible full-expression-root ordinary call with a
cleanup-owning result now initializes its exact result obligation only on the
`Invoke` success edge; failure cleanup cannot drop an object that was not
constructed. Constructors, nested argument detector edges, assignment targets,
borrowed and remaining owning result forms, backend execution, and containment
remain open.

The 0.137.0 checkpoint starts M-FAIL-01's explicit control-flow slice. MIR v17
terminates eligible full-expression-root scalar computations, loads, and calls
with `Invoke`, gives the dedicated failure successor one body-local fixed-record
parameter, performs verified reverse-construction temporary/lexical cleanup,
ends active loans, and exits through `PropagateFailure` without changing the
record. The verifier rejects missing or forged invokes, normal predecessors to
failure blocks, rewritten records, failure-result use, misplaced cleanup, and
cleanup-order drift. This bounded family is intentionally limited to function
and lambda bodies with trivial non-reference results and no destination,
ownership, loan, or lifecycle effect. Nested argument evaluation, staged owner
parameters, constructors, owning/borrowed results, double failure, containment,
runtime records, and backend execution remain open.

The 0.136.0 checkpoint completes M-FAIL-01's deterministic artifact metadata
slice. Source-written include edges retain exact spelling and lexical
occurrence solely for host-path-free external-unit identity. A
backend-independent post-HIR builder coalesces generic definition anchors,
unions and canonically sorts outcomes, assigns one-based `FailureSiteId`
values, serializes the immutable descriptor, and computes its SHA-256 artifact
identity. MIR v16 retains the descriptor and an exact site for every local
detector while propagation remains un-sited; verification rejects descriptor,
digest, assignment, and instruction-site drift. Fixed records, explicit
normal/failure edges, cleanup unwinding, containment, backend execution, and
task-entry handoff remain open.

The 0.135.0 checkpoint starts M-FAIL-01's compiler-owned identity layer and
extends the bounded M-EXEC-01 schedule needed by consumed task callables.
Semantics, HIR, and MIR v15 now carry exact defined-failure codes/details,
multiple snapshot-local source-unit origins, and distinct direct, virtual,
constructor, and callable propagation channels. MIR verification rejects
forged or target-inconsistent records, while optimizer effects make every
classified operation non-speculatable, non-removable, and non-reorderable.
Concretely selected class `operator()` calls now use the ordinary ordered-input
schedule, including a `MoveValue` receiver for an explicit move or exact
trailing-`&&` target and for once-callable fallback to a read/mutable overload.
Artifact-local failure sites, explicit normal/failure edges, cleanup unwinding,
runtime records and containment, hosted generated origins, backend execution,
task-entry transfer, and public `std::jthread` remain open.

The active L-TEXT-01 slice now provides source-defined canonical base-10
`std::print`/`std::println` and owning `std::to_string` for every fixed-width
integer, plus sequential integral `{}` replacement through `std::format`,
`std::try_print`, and `std::try_println`. Ordinary GTI validates `{}`, `{{`,
and `}}`, counts arguments, builds owned text, and returns `format_errc` before
stdout on malformed input. The language contribution is a bounded ordered
call-pack fold and lossless `uint8_t(char)` extraction; no public format name
or grammar is compiler-recognized. Indexed/named/specifier formatting,
floating-point conversion, dynamic view composition, and customization remain
open.

The 0.133.0 checkpoint completes C-MIR-01 without exposing a public
concurrency API. HIR and MIR v14 carry exact thread spawn/join, atomic
load/store/read-modify-write/compare-exchange, and mutex lock/unlock identities
plus operation-specific atomic orders. MIR rejects malformed order pairs,
misplaced synchronization metadata, and every synchronization operation under
the single-threaded profile. Exhaustive effects keep recognized operations
non-speculatable, non-removable, and non-reorderable while ordinary unknown
calls remain conservative barriers. The future `std::jthread` remains an
ordinary source-defined class over identity-bound private capabilities;
task-transfer, worker-failure, runtime, ordered execution, and backend work
remain explicit prerequisites.

The 0.132.0 checkpoint extends M-EXEC-01's bounded ordered-input schedule from
ordinary functions to concrete ordinary constructors with at least one
supported argument. HIR reuses one exact argument-role plan; MIR v13 emits the
same one-use checkpoints followed by a receiver-free `Construct`, moves active
temporary obligations at their parameter checkpoint, and verifies the exact
constructor target and strict source order. Generated/default zero-argument
construction, copy/move special construction, borrowed-state class values,
packs, failure rollback, and production backend consumption remain open.

The 0.131.0 checkpoint admits free-function and static-method reference returns
from one exact namespace-global or non-generic static-field place. The default
single-threaded profile permits `mut T&`; unsafe pointer dereference remains
inside the accessor, safe callers receive symbol-rooted checked loans,
temporary calls end at their full expression, and retained mutable aliases are
lexical and conflict. Semantic summaries flow through HIR and MIR v12, whose
verifier and deterministic serialization bind every call and return to the
same exact place. The existing concurrent-profile `GTI-S2060` rule continues
to reject unsynchronized mutable process-wide storage.

The 0.130.0 checkpoint completes `L-INIT-01`. Braces in an ordinary named
function, method, or constructor call now initialize one exact owned
fixed-array parameter. A bounded inferred-only `uint64_t` value parameter may
name that complete extent, and concrete function, method, and constructor HIR
identity retains the inferred value. The first public client is the ordinary
source-defined `std::vector<int>({1, 2, 3})`; ADR 015 excludes
`std::initializer_list`, common-type inference, list-preferred overloads, CTAD,
and native C++ overload authority. Formatter, Tree-sitter, LSP, stdlib runtime,
examples, and the C++20/C++23 backend use the same exact-array contract.

The 0.128.0 checkpoint extends the bounded M-EXEC-01 ordinary-call schedule to
eligible non-borrowed class-value parameters. HIR distinguishes class-copy and
class-move inputs from scalar values and reference borrows. MIR v11 copies an
lvalue from its exact place or consumes an exact materialized value, transfers
that value's active temporary obligation at its source-ordered checkpoint, and
keeps the final call free of undifferentiated ownership setup. Verifier
mutations reject forged copy/move roles, missing or misplaced transfer, and an
erased exact target. Borrowed-state class values, remaining call forms, result
and target places, and production M-BACK emission remain open; semantic call-
borrow restrictions are therefore unchanged.

The 0.127.0 checkpoint implements two deliberately separate aggregate
families. A passive native `union` supplies C++-familiar overlapping storage,
but accepts only recursively passive fields, carries frontend size/alignment
facts, and requires `unsafe` for every member read or write. A non-generic
payload `enum class` supplies closed tagged alternatives, exact construction,
immutable copied pattern bindings, and exhaustive `switch` diagnostics.
Semantic identity and exhaustiveness flow through HIR and MIR; C++20/C++23
currently represent the safe family with `std::variant` without making that
library type semantic authority. Generic, borrowed, moved, cleanup-owning, and
stable-layout payloads remain the next bounded `L-SUM-01` work.

The 0.126.0 callable checkpoint implements the first bounded owned transport
between generic call boundaries. A free function's direct immutable by-value
`T` parameter may be explicitly moved through an exact `T` result or into the
exact sole `T` field of a concrete generic owner. Semantics rejects implicit
copies and borrowed/raw capture state; HIR preserves the substituted closure and
destination identities; MIR v10 verifies the caller move, callee return or
construction, declared owner field, constructor parameter move, and final drop
owner. C++20/C++23 runtime evidence proves a move-only capture is cleaned up
exactly once. Inferred lambda results, arbitrary nested owners, extraction,
global/static storage, callable references, and erasure remain closed.

The 0.125.0 callable checkpoint implements the first owned closure environment
without opening general callable escape. `[target = std::move(source)]` moves
one enclosing local or by-value parameter into an immutable capture field;
semantics updates availability left to right, HIR records ordered copy/move
initializers, and MIR v8 verifies exact environment symbols, capture places,
move provenance, and drop state. Moving the completed local closure transfers
that environment, and C++20/C++23 runtime coverage proves a cleanup-owning
capture is destroyed exactly once. General init/reference capture and exact
generic callable return/field escape remain closed.

The 0.124.0 callable checkpoint implements confined once invocation without
opening callable escape. `operator() &&` is the bounded consuming nominal form;
`std::move(operation)()` records an exact once-callable generic requirement and
uses ordinary path-sensitive move state for at-most-once invocation and
forwarding. HIR retains required and selected capabilities, MIR verifies the
receiver's ownership-move provenance and exact target, and the C++ bridge
preserves frontend selection across read, mutable, and consuming overloads.
The current consuming receiver must be structurally cleanup-free until the
backend has a full-expression-owned receiver representation. General
rvalue-reference types and owned escape remain closed; the 0.125.0 checkpoint
above completes the bounded local move-capture environment.

The 0.123.0 callable checkpoint implements repeatable invocation capability
without opening callable escape. An immutable confined parameter requires a
read-callable target; a `mut` confined parameter accepts either a read-callable
target or an exact class `operator() mut`. Semantics records the requirement and
selected capability, HIR/MIR preserve it with the exact target and receiver,
MIR preserves and verifies the contract, and the backend prevents C++ overload resolution
from changing frontend-selected receiver access. Source-defined predicate and
numeric algorithms now accept stateful mutable function objects. Consuming
once-callable invocation is completed by the checkpoint above; environment
lifecycle, capture movement, and owned escape remain closed.

The 0.122.0 callable checkpoint completed the first two L-CALL-01
implementation slices. Semantic, HIR, and MIR callable sites now carry
explicit `Confined` boundary records instead of phase-spanning booleans, and
MIR verifies ordered, unique, in-range confined argument descriptors.
Generic callable calls may return exact non-reference values without tracked
borrowed state or lambda identity when an explicit binding, assignment,
condition, or enclosing return supplies the result type; concrete reanalysis
substitutes symbolic requirements before target validation. `auto` result
inference, borrowed results, and owned escape remain closed. Ordinary GTI uses
this capability for
operation-based `std::accumulate` and `std::inner_product` plus unary
`std::transform_reduce`.

The 0.121.0 integrity checkpoint hardens recently landed public capabilities
without adding a competing language layer. Native-facing identifiers and
namespaces now reject C/header collisions before emission; opaque C handles are
address-only through raw pointers; and editor hover presents that compiler-owned
contract. The transitional C++ backend isolates ordinary GTI names from host
macros/namespaces and dependency-orders complete type definitions while
preserving source-order global initialization. Managed build publication rejects
leaf and root symlink escapes, workspace discovery rejects redirected manifests,
and the whole-program cache conservatively bypasses native sources, search
paths (including dependency-injecting environment paths), opaque argument
vectors, link operands, and unresolved libraries until their transitive native
inputs can be modeled.

The 0.118.0 checkpoint completes B-PROJECT-03. Manifest schema version 1 now
resolves canonical workspace members and recursive source-only path
dependencies without network access. Direct package aliases feed the
compiler-owned source graph; transitive and quoted cross-package access is
rejected; cycles, duplicate names/roots, nested workspaces, and missing source
roots fail before compilation. `--package` provides deterministic selection,
workspace artifacts are collision-free under one managed root, metadata schema
7 publishes the package graph, and cache identity includes package provenance.
Direct mode remains manifest-independent. Exact Git/lockfile work and a stable
read-only driver project-facts API are now ready as separate next rows.

The 0.117.0 checkpoint completes M-LIFE-01. Semantics selects exact AST
full-expression roots and rejects cleanup-owning global/static storage with
`GTI-S2061`; HIR retains typed lexical/value obligations and concrete cleanup
identities; and MIR verifies initialization, transfer, path-conditional state,
LIFO full-expression cleanup, and reverse lexical cleanup on every normal
edge. Boundary tables, construction ranks, exact ownership-consuming
call/constructor targets, lambda capture projections, cleanup-relevant class
shape, and adversarial mutations make those facts verifier-owned without
claiming the ordered evaluation schedule that remains M-EXEC-01 work.

The 0.126.0 checkpoint starts the first bounded M-EXEC-01 implementation slice.
Concrete non-intrinsic ordinary calls with scalar/reference parameters retain
exact HIR receiver and argument roles; MIR makes each input a one-use schedule
checkpoint and verifies receiver, source-ordered arguments, then invocation.
Adversarial wrong-site, duplicate/bypass, type-drift, and reorder mutations are
rejected. The same checkpoint starts C-MIG-02 by moving SourceLoader algorithms
behind the existing compiled-library interface; parser extraction remains the
next mechanical slice. Neither change makes MIR the production backend or
relaxes conservative call-borrow semantics.

The 0.115.0 checkpoint completes B-PROJECT-02. Project build, run, and test
requests derive a SHA-256 whole-program identity from the compiler-owned loaded
source graph, effective target/profile/backend policy, runtime and native
compiler identity, and admitted toolchain environment. The current hardened
eligibility policy bypasses declared native sources, native search directories,
opaque native argument vectors, native link operands, and unresolved
library/framework inputs, plus dependency-injecting environment search paths,
until their transitive dependencies can be modeled.
Verified generated C++ and executable payloads
live under `build/gti/cache/v2`; the v2 identity includes ordered configured
prelude-root provenance before canonicalized source/dependency facts. Hits skip parsing through native linking,
corruption is diagnosed and rebuilt before replacement, and `--no-cache`
provides a clean verification path. Pure-GTI checkout moves retain content
identity while native/external path-semantic inputs remain explicit. Direct
compilation and frontend-only `check` remain uncached.

The 0.113.0 checkpoint completes B-PROJECT-01. Manifest schema version 1 now
accepts executable and test target kinds; the driver resolves all or one named
test into deterministic immutable build plans; and `gti test` builds each root
as an independent whole program. Runtime failures are reported by target while
later tests continue, the first failing process status is propagated, metadata
schema 6 publishes target kinds, and direct compilation plus source-tree and
installed CLI/library workflows remain covered. This row unblocked the
whole-program cache completed at 0.115.0.

The 0.98.0 checkpoint adds multi-parameter source concepts and bounded,
validity-only trailing `requires` conjunctions. Exact input-iterator,
iterator/sentinel, and homogeneous accumulation relationships now support a
source-defined `std::accumulate`; concrete generic reanalysis retains the
selected operator identities through HIR, and formatter, Tree-sitter, LSP, and
editor surfaces share the frontend representation. General
requires-expressions, specialization, subsumption, and constraint-based
overload ranking remain outside this checkpoint under ADR 009.

The 0.98.0 checkpoint extends managed project-native compilation to declared,
package-contained `.cpp`, `.cc`, and `.cxx` sources. They use the already
resolved C++ compiler, project standard and optimization, shared include paths,
and ordered native `compile-args`; atomically published C++ objects follow C
objects and precede runtime/manifest operands in the final link. Metadata schema
4 reports both source categories, while failed C++ source compilation preserves
the prior object and executable.

The 0.102.0 checkpoint completes design-only D-EXEC-01. Execution Section 4.2
and ADR 010 select strict left-to-right operands, receivers, parameters,
captures, and initialization; target-first one-time assignment-place
evaluation; direct destination materialization; LIFO full-expression
obligations; reverse partial cleanup; ordered owned-entry setup; and a lexical
dependency-first program-initialization walk. This changes no current emitted
behavior. The semantic/HIR/MIR facts and closed production-backend migrations
remain the systems-readiness implementation gap.

The 0.97.0 checkpoint completes I-CAP-01. The source graph distinguishes
application, implicit prelude, and physical configured standard-library roles
without allowing an override-only path to mint trust. Root `gti_internal`
declarations, references, and alias targets are application errors under
`GTI-S2058`. Private owner, storage, and text-view types bind through their
exact trusted-prelude class identities; aliases and signatures retain privacy,
the C++ backend consumes semantic capability facts, and shared language queries
filter completion, hover, definition, and semantic classification for
application documents. Public `std` wrappers remain ordinary GTI source.

The 0.96.0 checkpoint makes `interface` the complete source-level abstraction
for pure behavior contracts. Interface methods are declaration-only signatures
ending in `;`; the semantic model supplies virtual/pure identity, and the C++
backend lowers that identity to `virtual ... = 0;`. The redundant C++ pure
specifier is now diagnosed inside an interface, while ordinary classes retain
their existing `virtual`, `override`, and `= 0;` syntax.

The 0.95.0 checkpoint extends project-native inputs with declared,
package-contained C sources. Project builds resolve the selected C compiler,
standard, include paths, and C-only arguments; compile each source to a staged,
atomically published object; and place those objects before the runtime and
manifest libraries in the existing final C++ link. Metadata schema 3 reports
the effective C inputs, while `check` remains compiler-free and output-free.

The 0.94.0 checkpoint adds the first transforming owned-MIR slice without
changing emitted behavior. At `-O1+`, exact primitive literals flowing only
through grouping identities are rewritten in shadow MIR and compared with the
existing HIR constant result. A narrow GTI-owned editor validates guarded
body/`{block,index}` patches as one batch, repairs value uses, records precise
invalidation, preserves instruction/result/provenance identity and CFG
dominance, and verifies a copied program before atomic commit. Strings,
arithmetic, conversions, dynamic values, general pass management, analysis
caches, and MIR-controlled emission remain outside this slice.

The 0.93.0 checkpoint adds the owned hosted entry form
`int main(int, std::vector<std::string>)` alongside `int main()`. Semantic
analysis validates canonical standard-library type identities and resolves the
source-defined append operation once; HIR and MIR preserve the entry kind and
that exact callable identity, and MIR verification guards them against drift.
The C++ backend emits a private native `argc`/`char**` adapter that copies every
argument into owned GTI values before invoking the source entry. This does not
add pointer-to-pointer source types, borrowed native storage, environment
access, or general compiler-provided vector behavior.

The checkpoint also completes D-MEM-01 and D-MEM-02. The
[concurrency and memory-model proposal](concurrency-memory-model.md) is retained
as design evidence, while
[ADR 008](../decisions/008-safe-concurrency-memory-model.md),
[Execution §4.9](../language/execution.md#49-concurrency-boundary), and
[ownership semantics](../language/ownership-and-lifetimes.md#concurrency-transfer-and-sharing)
now adopt safe data-race freedom, structural transfer/share facts, explicit
single-threaded/concurrent profiles, an owned-only automatic-join first thread
boundary, sequentially consistent scalar atomics, concurrent-global policy,
contained worker failure, and explicit synchronization effects. That decision
did not itself implement public concurrency. ADR 012 now makes the bounded
owned-task/thread, SC-atomic, mutex-guard, and conformance profile a systems-
readiness outcome while preserving every prerequisite in ADR 008.

The 0.103.0 checkpoint completes C-TYPE-01 without exposing public threads.
Structural transfer/share derivation covers concrete generics,
recursive owners, alternatives, arrays, and callable captures; borrowed state,
raw pointers, cleanup, and explicit native affinity deny capabilities unless a
nominal unsafe assertion says otherwise. Public compiler-bound concepts,
interface requirements, deterministic `GTI-S2059` diagnostics, HIR retention,
formatter/Tree-sitter syntax, and LSP presentation share that semantic
authority.

The 0.104.0 checkpoint completes C-GLOBAL-01 without adding a public
concurrency API. Direct and project builds resolve the exact
`single-threaded`/`concurrent` execution-profile fact before semantics; the
default is unchanged. `GTI-S2060` rejects mutable or non-share-capable
namespace globals and static fields only under concurrent selection, including
aliases, concrete generics, raw/borrowed state, nominal native-affinity
opt-outs, declared cleanup, and internal linkage. `SemanticModel`, HIR, and MIR
retain the selection; native flags and backend traits never infer it.

The 0.105.0 checkpoint completes M-OWN-01 and the bounded M-OWN-02 client.
Semantics, HIR, and MIR now share value-owned place domains, constant/dynamic
fixed-array projections, and ownership events. In-range constant elements of
directly owned fixed arrays can move and be restored independently; branch and
loop state is checked before backend entry, and MIR replays the reachable
available/moved fixed point. Dynamic selections remain may-alias.

The current M-LIFE-01 checkpoint makes the supported normal-exit lifecycle
slice authoritative. Semantics selects AST full-expression roots and HIR maps
them to typed lexical/value drop obligations with exact concrete cleanup
descriptors. MIR records initialize, move, reparent, replace, transfer-out, and
drop events and verifies their available/moved/uninitialized state on every
normal edge. Branch-only logical and conditional temporaries retain
path-conditional obligations through their merge and clean up at the enclosing
full-expression boundary, while semantics rejects recursively cleanup-owning
namespace globals and static fields with `GTI-S2061`.

The 0.106.0 checkpoint completes S-LAYOUT-01. Exact `os`, `vendor`, and `arch`
properties and structured triple failures now select one LLVM-free,
GTI-owned scalar data-layout value. Every supported arm64/x86_64 macOS, Linux,
or Windows target is 64-bit little-endian with deterministic byte size and
ABI/preferred alignment facts; installed compiler-library probes compare the
host selection with the native ABI. `GTI-S2062` stops an unsupported selected
layout before parsing, semantic target selection, or backend entry. General
cross-toolchain selection, aggregate/class layout, and stable native records
were still separate work at that checkpoint; S-ABI-01/02 subsequently closed
the passive native-record subset.

The 0.107.0 checkpoint starts P-MEASURE-01 with a hermetic,
standard-library-only benchmark runner and the first checked-vector workload.
Strict descriptors, correctness-first execution, controlled output paths,
exact compiler commands and identities, emitted C++ evidence, deterministic
raw samples, and threshold-free CI smoke now make optimization and
representation claims reproducible. This checkpoint deliberately changes no
language check or storage semantics: integer, fixed-array, dispatch, compiler,
LSP, and project-driver workload breadth remains before P-MEASURE-01 is done,
and public prefix-storage work remains blocked on defined-failure and
MIR-backed-emission prerequisites.

The 0.108.0 checkpoint completes the bounded source projection of those
facts. Reserved, type-only `sizeof(type)` and `alignof(type)` produce exact
`uint64_t` frontend constants for primitives, one-level raw pointers,
transparent aliases, and recursively supported fixed arrays with positive
concrete extents. Semantics reports `GTI-S2063` for unsupported categories,
symbolic or zero extents, and checked size overflow. HIR preserves query
provenance, MIR lowers the result to a literal, and the C++ backend emits the
retained number rather than a native layout operator. Synthetic supported
targets, native ABI probes, and the installed compiler-library consumer cover
the boundary. Expression operands, direct query expressions in array extents,
ordinary record layout and general layout control remain separate work; the
bounded passive `[[c_abi]]` record family is now implemented and verified
against an independent C oracle.

The 0.109.0 checkpoint completes the first outcome-selected L-NUM-01 slice for
renderer/game and low-level systems arithmetic. `<std/numeric>` now exposes
exact fixed-width wrapping and saturating add/subtract/multiply across all
eight integer domains. Semantics and constexpr evaluation share the private
APInt authority, HIR and MIR retain six distinct intrinsic identities, MIR
classifies them as non-failing and memory-free, and the backend avoids signed
native overflow. Focused boundaries plus O0/O3 and C++20/C++23 runtime evidence
agree. Ordinary operators remain checked; explicit checked-result arithmetic
was deferred to the later 0.111.0 L-NUM-01 sub-slice.

The 0.110.0 checkpoint completes L-FLOAT-01. `double` is an exact IEEE-754
binary64 type, `d`/`D` selects binary64 literals, and existing unsuffixed
decimal literals remain binary32. The GTI-owned width-tagged bit record reaches
semantics, HIR, MIR, optimization, and bit-exact C++ emission; private
`APFloat` computes both widths. Mixed arithmetic promotes to binary64,
`float`-to-`double` widening is implicit, and narrowing is explicit. Focused
frontend, MIR, formatter, Tree-sitter, LSP, layout, generic numeric, and
O0/O3 × C++20/C++23 native tests prove parity.

The 0.111.0 checkpoint completes L-NUM-01. `<std/numeric>` now adds exact
fixed-width `checked_add`, `checked_sub`, and `checked_mul` overloads returning
ordinary `expected<T, std::arithmetic_errc>` values. In-range results carry the
integer and out-of-domain results carry `result_out_of_range`; construction is
failure-free and leaves ordinary checked operators unchanged. The private
`APInt` authority, semantic constants, HIR/MIR identities, non-trapping effect
table, guarded native helpers, bounded constexpr observers, LSP source facts,
and O0/O3 × C++20/C++23 runtime matrix agree across all eight integer domains.

The 0.114.0 checkpoint completes S-ABI-01 and S-ABI-02. `[[c_abi]] struct`
opts a passive non-owning record into frontend-owned source-order layout,
bounded `sizeof`/`alignof`, and by-value or one-level-pointer `extern "C"`
passage. Semantics retains size, ABI alignment, and field offsets; HIR and MIR
carry and verify those facts; the backend audits its representation with
standard-layout, trivial-copy, size, alignment, and offset assertions. A C
translation-unit oracle proves nested records, by-value arguments/returns, and
pointer mutation at O0/O3 and C++20/C++23. Callbacks, pointer-to-pointer out
parameters, and annotated opaque ownership transfer remain separate capability
rows.
The same checkpoint also supplies the previously declared mutable
`std::array::at` bodies, restoring the shipped array example without adding a
compiler special case.

The 0.116.0 checkpoint completes S-ABI-03. Direct
`--emit-native-header` and the installed `NativeHeaderBackend` now produce one
deterministic header from checked native records and `extern "C"`
declarations. Its C17 branch uses strict C prototypes and deterministic names;
its C++20/C++23 branch preserves exact GTI namespaces and type identities while
keeping C linkage at the function boundary. Separate C and C++ translation
units—including a private C++ class adapter—link with GTI at O0/O3. Native
records are now representation-only declarations: field initializers are
rejected, and emitted GTI C++ uses the same canonical policy-free definition as
the public header. General C++ ABI, foreign-header import, callbacks,
pointer-to-pointer out parameters, and ownership transfer remain separate
capability rows.

The 0.119.0 checkpoint completes the pointer-only opaque-handle sub-slice of
S-FFI-02. `[[c_opaque]] struct Name;` creates one incomplete nominal identity
that may cross only behind a one-level raw pointer; it has no GTI layout,
lifecycle, ownership, or concurrency policy. Native records may contain such
pointers. The generated header emits an incomplete C typedef or exact
namespaced C++ forward declaration, and the mixed native oracle privately
completes one handle in C and another around C++ class/RAII state. Source
wrappers remain responsible for null handling and exactly-once destroy calls.
Callbacks still wait for M-LIFE-01, M-FAIL-01, and matching M-BACK-02 execution;
pointer-to-pointer output and annotated ownership transfer remain client-gated
S-FFI-02 sub-slices.

M-OWN-01 and the bounded M-OWN-02 implementation are complete in
[`place-and-ownership-state.md`](place-and-ownership-state.md). It selects one
snapshot/body-scoped value key for program, body, formal, receiver, temporary,
materialized, raw, and opaque roots; exact field/constant-index and
conservative dynamic/raw/opaque projections; and one exhaustive equal,
directional-prefix, disjoint, or may-alias relation. Semantics retains source
validity and diagnostics, HIR carries concrete keys/events, and MIR computes
and verifies the finite ownership-state CFG fixed point. The implemented slice
supports moves and restoration of constant elements in directly owned fixed
arrays, including fields containing arrays, while whole-owner, branch, loop,
and dynamic-index uses observe exact or conservative partial state. M-LIFE-01
now consumes that ownership foundation for explicit temporary and active-drop
obligations.

Design-only D-CALL-01 is complete in the accepted
[callable ownership and escape contract](callable-ownership-and-escape.md).
Lexical closures, nominal callable objects, and future exact function items
share one GTI-owned concrete identity/signature model; read-callable,
mut-callable, and once-callable distinguish receiver access and invocation
count independently from copy/move/drop and the implemented transfer/share
facts. The
contract defines immutable-copy and explicit-owned-move capture, confined
versus exact generic owned transport, lifecycle/escape diagnostics, and one
cross-phase vocabulary for algorithms, consumed tasks, and native callbacks.
L-CALL-01 now implements that vocabulary for confined boundaries, exact
context-supplied confined-safe value results, repeatable read/mut invocation,
consuming once-callable cardinality, local copy/move closure environments,
exact same-type generic return, and the bounded one-field generic owner.
Broader owned wrappers and extraction remain behind demonstrated clients,
while C-CALL-01 and S-CALL-01 keep their failure, concurrency, and ABI gates.

Design-only D-FAIL-01 is complete in
[Execution §4.10](../language/execution.md#410-defined-runtime-failure), with
rationale in [ADR 007](../decisions/007-defined-runtime-failure.md). Defined failure now has
stable categories and artifact-qualified source sites, cleanup-preserving
non-resumable propagation, an exact hosted report and status 70, a constrained
observer, explicit program/embedding/task/callback boundaries, and a precise
`expected`/infallible split. ADR 008 incorporates that contained worker
failure and requires explicit or automatic join to re-raise the original
record. The landed M-FAIL-01 slices now assign exact semantic/HIR/MIR
local-origin and propagation identity, deterministic artifact sites, and
bounded full-expression-root and exact local-detector or ownership-free static
direct-call prepared-argument `Invoke`/cleanup/propagation edges. They do
not yet change current execution. The Q-FAIL runtime substrate now supplies a
fixed version-one C record/artifact/site ABI with per-site outcome validation,
the descriptor-free runtime sentinel, allocation-free Unicode-15.1 ordinary
and emergency reports, observer re-entry/exception/mutation protection,
partial/`EINTR`-safe I/O, one terminal winner, and status 70. C/C++ layout,
Unicode-boundary, and subprocess tests own those facts. The emitter still
aborts without cleanup/location/category preservation and wrong-state expected
access still inherits native behavior until ordered nested parameter/result
state, generated hosted containment, descriptor emission, and complete
failure-capable M-BACK-02 body migration land; M-LIFE-01 is complete.

D-LANG-01 is now complete in the maintained
[language restriction ledger](language-alignment.md). It classifies every
external language-audit finding, original alignment question, explicit
language-specification gap, and backend-visible restriction with one reason,
readiness role, user-facing client, owner, and evidence gate. ADR 012
supersedes the former version split. Bounded layout queries are complete;
bounded public concurrency, native callbacks and out-parameter families, an arena/pool allocation
path, payload sums, propagation syntax, exact domain operators, and one
associative container are systems-readiness work.

The 0.92.0 checkpoint closes the floating-point Milestone 0 contract with an
exact GTI-owned IEEE-754 binary32 representation. Decimal literals, constexpr
and optimizer arithmetic, comparisons, and numeric conversions use private
LLVM `APFloat` computation with explicit rounding; emitted constants preserve
their exact bits, and the native driver enforces the matching strict floating
flags. It also adds a private, full-recomputation LLVM generic-dominator
adapter whose GTI block-ID result lets MIR verification reject same-block
use-before-definition and reachable uses not dominated by their definitions.
Neither LLVM representation crosses a public header or becomes cross-phase
authority. Type interning, loop analysis, and incremental dominance remain
deferred until ownership, clients, and measurements justify them.

The 0.90.0 checkpoint consolidates compiler-engineering support onto one
mandatory LLVM-backed build. A compatible system LLVM and the pinned bundled
LLVM are dependency-acquisition choices, not separate implementations. The
portable checked-integer and HIR hash implementations displaced by the LLVM
versions remain temporarily under `archive/` as non-built reference material.
This checkpoint does not adopt LLVM as a backend or transfer GTI-specific
language semantics, HIR, MIR, diagnostics, or code generation to LLVM. It also
records that the current LSP crash guard is best-effort and still needs a
state-safe isolation boundary.

The 0.89.0 checkpoint adds bounded local exclusive reborrows without adding
syntax. A mutable loan may produce a distinct mutable or read-only child over
a stable symbol/receiver root with named-field and checked-dereference
projections. The parent is suspended while any overlapping child remains active
and fully reactivates only after its final active child reaches a frontend-
selected endpoint. Nested chains compose, while known-disjoint named-field
children may coexist and leave disjoint projected parent access available.
Indexed, raw, and opaque sources, mutable stored-reference fields, any local
child escape (mutable or read-only, direct or through a stored carrier), and
mutable owner-tied range-for iteration remain outside this slice.

The 0.88.0 checkpoint extends precise retained-loan endings to shared
read-only aliases. Every alias remains a carrier of one semantic loan; uses
from every carrier feed the existing path-aware planner, and HIR/MIR preserve
one frontend-selected endpoint set and active-state identity. Straight-line,
conditional, loop, switch, and proven break-path shapes therefore permit owner
mutation after all reachable alias uses. That checkpoint still rejected
mutable aliasing because it lacked the explicit child-loan and suspension
transitions added in 0.89.0.

The 0.87.0 checkpoint extends bounded compile-time programming with
non-generic constexpr free functions and static methods, scalar locals,
mutation, structured control flow, recursion, and nested constexpr calls.
`if constexpr` is selected by semantics and only its chosen branch reaches HIR,
MIR, and C++ emission. One compiler-owned evaluator enforces checked primitive
operations, a shared 4096-step budget, and a 64-call-depth limit. Generic and
instance-function execution, class values, references, floating point,
allocation, and runtime/C/intrinsic calls remain explicit later slices.

The 0.86.0 checkpoint introduced compiler-evaluated `constexpr` scalar
bindings and `static constexpr` class fields. Computed values remain in
semantic bindings and HIR, supply concrete fixed-array extents and uint64_t
value-generic arguments, and are serialized by the C++ backend even at `-O0`.

The 0.85.0 checkpoint carries one read-only owner relationship
through resolved calls, concrete generic carrier instances, explicit moves,
checked returns, and drops. Instance methods derive that relationship from the
receiver; free functions and static methods may derive it from one eligible
read-only parameter. This unblocks ordinary helper/factory APIs for read-only
cursors and views without introducing explicit lifetime syntax. Mutable or
exclusive reborrows, more than one or nested origin, global/captured/storage
escape, and dependency-changing assignment remain outside that slice.

The 0.84.0 checkpoint extends retained-loan flow for one unshared carrier across
bounded switch exits and same-path invalidations immediately followed by a
matching `break`. Semantic analysis selects the endpoint, HIR preserves it, and
MIR normalizes each relevant outgoing edge before the verifier checks
predecessor agreement. General mutable reborrow/exclusive-loan graphs remain
explicitly deferred.

The 0.83.0 completeness pass hardened feature composition rather than adding a
new language surface. It preserves resolved inherited-generic owners into HIR,
adds fallthrough- and backedge-aware value-state checks, aligns raw-pointer
qualification ranking through variadic construction, and adds shipped-source
parser and position-sensitive editor-query gates. Larger MIR dataflow and
verification work remains explicitly staged rather than being folded into
these corrections.

The compiler is transitional rather than backend-independent:

- semantics owns language validity, HIR owns concrete instances, and MIR owns
  its implemented body-local CFG, value, place, call, move, loan, drop,
  lifecycle, ordered-input, and bounded failure-edge families;
- MIR still lacks complete receiver/argument/result materialization,
  partial-constructor rollback, containment, and program initialization;
- constant folding still controls C++ emission through the compatibility HIR
  replacement table;
- the C++ emitter still walks checked AST and HIR side data rather than
  emitting complete bodies from optimized MIR.

The immediate compiler critical path is therefore **backend-authority
recovery**, not additional shadow MIR breadth. M-LIFE-01's normal-exit
temporary/drop substrate and several ordered/failure families are already
verified; the next credibility result is to execute the largest sound body set
from those facts. Remaining lifetime, ordering, rollback, and containment work
is co-delivered by the production migration phase that consumes it.

Compiler operations that ordinary GTI cannot yet express now enter semantics
through trusted bodyless declarations in the implicit prelude. Calls bind the
selected declaration and `FunctionId`; namespace aliases preserve that
identity, while an untrusted declaration with the same spelling remains an
ordinary function. This removes call-site name recognition without adding a
source keyword, attribute, or public compiler-known wrapper type. The same
identity rule now covers private type declarations, while source roles and
semantic publication prevent application access to the surrounding namespace.

## Layer Assessment

| Layer | Position | Concrete boundary |
| --- | --- | --- |
| Source graph and parser | Implemented foundation | Per-unit parsing, direct visibility, recovery, source provenance, explicit application/prelude/physical-standard-library roles, target directives, reserved type-only layout-query operators, and the bounded comma-pack-fold expression are shared by CLI and LSP. Override-only paths do not acquire compiler trust. Dependency-first `compilationOrder()` remains parse/assembly order, not the accepted runtime initialization plan. The external Tree-sitter grammar has a CI gate that parses every shipped standard-library and example source in addition to focused corpus fixtures. |
| Semantic analysis | Broad but transitional | Exact types, overloads, concepts, lifecycle, ownership, dispatch, bounded constexpr values/functions/branches, target-owned layout-query constants, AST full-expression roots, and current borrow restrictions are authoritative. It now also owns passive-union validity/layout and unsafe access, plus payload-enum case identity, exact construction, immutable copied bindings, and switch exhaustiveness. Constexpr evaluation is compiler-owned, checked, step/depth bounded, and recorded independently of C++ emission. Layout queries resolve aliases and recursively derive supported positive-array facts without consulting native C++; `GTI-S2063` rejects unsupported or non-concrete operands before lowering. One-level raw-pointer operations and pointer-bearing C calls are classified against lexical unsafe context before lowering; raw pointers create no semantic loans. Trusted intrinsics and compiler-private types bind by declaration identity; `GTI-S2058` rejects application access to root `gti_internal`, and source publication prevents aliases or exposed signature types from leaking its symbols. Variadic storage construction selects exact element constructors, bounded C linkage retains exact external symbols, and the owned hosted-entry signature records its canonical argument types plus resolved startup append callable. The bounded comma-pack fold selects one symbolic free-function target and records each exact read-only element specialization without per-element overload resolution. Named-field move state is path-sensitive and checked on reachable loop backedges. Borrowed-return summaries select one read-only receiver or parameter origin and concrete generic carrier instances preserve it through calls, moves, returns, and drops. Retained local loans have owner/carrier provenance and frontend-selected straight-line, nested conditional, loop-exit, switch-exit, and proven break-path endings. Every carrier of a shared read-only loan contributes to that same path-aware plan. Bounded exclusive reborrows create distinct mutable or read-only child loans over stable root/field/checked-dereference places, suspend the mutable parent, validate prefix-overlap conflicts, permit known-disjoint sibling children and projected access, and fully reactivate the parent only after its final active child endpoint. General indexed, raw, opaque, stored, or escaping exclusive-loan graphs remain deferred. |
| Typed HIR | Concrete-instance authority | Owns concrete generic, class, callable, and entry instances; resolved calls; typed values and places; constants; ownership, borrow, lifecycle, cleanup, failure, native-linkage, payload, storage, pack, and ordered-input facts; and the target-independent program-initialization plan. HIR remains immutable and does not own backend spelling. Some expression families still need more uniform destination/materialization schedules, but that gap cannot move executable authority back from MIR. |
| MIR | Sole executable-body authority with bounded verifier gaps | Owns every body CFG and identity, values, places, calls, moves, loans, raw-memory and synchronization operations, construction/drop/cleanup schedules, failure metadata and propagation, program initialization, hosted startup, and source/optimized provenance. Verification rejects malformed ownership, effects, schedules, and rewrites. Remaining work includes a uniform materialization model for all operation families, double failure, and target-independent generated-item/ABI contracts; it is not a source-body migration. |
| Optimizer | Bounded verified production transforms | Backend-neutral constant evaluation, HIR constant analysis, MIR dominance, controlled atomic editing, repair/invalidation, and source-to-optimized coherence are implemented. Every executable body consumes optimized MIR; only transformations replayed and accepted by the coherence verifier can change emitted behavior. General pass management, cached analyses, loop infrastructure, and broader folds remain client-gated. |
| C++ backend | Source-body hard cutover complete; generated representation still transitional | `CppBackend` requires coherent source and optimized MIR, builds and seals a complete body/data/thunk representation inventory, and accepts only the `VerifiedMir` whole-program route. All 2,487 reviewed body identities emit through `CppMirBodyEmitter`; executable AST visitors and the public no-MIR emitter are removed. AST, semantics, and HIR still supply declaration, layout, ABI, template, and generated-adapter representation facts. Hosted entry and program initialization are explicitly planned; several other generated adapter kinds remain the next backend-separation boundary. |
| Compiler library boundary | Migration complete | Frontend, semantic model/analysis, HIR/MIR lowering and queries, optimization, formatting/language queries, and support algorithms compile in `gti_compiler`; C++ emission compiles in `gti_cpp_backend`, and native/project orchestration compiles in `gti_driver`. Public headers retain records, templates, `constexpr` values, trivial accessors, and exact-version facades. |
| Build and tooling | Parallel foundations | Direct and manifest workflows share driver requests; `build`, `check`, `run`, `test`, `clean`, and schema-7 `metadata` are implemented. Package/profile/target native inputs are target-selected, package-contained, ordered, and passed through the shared native request; declared C and C++ sources compile atomically before the final C++ link. Test targets build and execute independently in deterministic order. Project build/run/test requests use a verified content-addressed whole-program cache. Canonical workspaces and source-only path dependencies now provide deterministic package selection, direct package aliases, graph diagnostics, shared outputs, and cache provenance without network access. Git/registry dependencies, lockfiles, native dependency composition, and LSP project-fact consumption remain staged. LSP queries share frontend snapshots and compiler-owned private-presentation checks for semantic tokens, completion, hover, and definition; broader project awareness and symbol operations remain incomplete. |

## Roadmap Milestones

### Milestone 0: design boundaries - partial

Implemented:

- fixed-width integer domains, checked arithmetic failures, shifts, modulo,
  conversions, and backend-neutral constant evaluation;
- IEEE-754 binary32 and binary64 literals, arithmetic, comparisons,
  conversions, signed-zero/NaN behavior, no-contraction execution, and
  compiler-owned constant evaluation through exact stored bits;
- centralized MIR instruction, operation, intrinsic, and synchronization
  effect tables;
- trusted declaration-bound intrinsic registration with no call-site spelling
  recognition;
- trusted source roles, exact compiler-private type identity, application
  `gti_internal` rejection, private-signature publication, and shared LSP
  presentation filtering;
- target selection, compiler-owned target conditionals, one GTI-owned scalar
  data layout, and bounded type-only `sizeof`/`alignof` constants over
  primitives, raw pointers, aliases, and positive concrete fixed arrays;
- an adopted concurrency and memory-model boundary covering safe data-race
  freedom, transfer/share capability derivation, explicit execution profiles,
  first-profile ownership and globals, atomic and automatic-join thread
  lifecycle semantics, native entry, and staged compiler/runtime verification;
- an accepted defined-failure contract covering stable category/site records,
  cleanup and double failure, hosted reporting/status, observation, embedding,
  worker containment, and recoverable API boundaries;
- an accepted evaluation/full-expression contract covering strict left-to-right
  execution, target-first assignment, destination materialization, transient
  obligation cleanup, partial construction, return publication, and
  program-wide initialization;
- an accepted Edition 1 compatibility contract covering 0.x draft release
  policy, 1.x meaning preservation, permanent Edition 1 defaulting, hard
  unknown-selector failure, corrections, deprecation/removal, and stable
  non-textual direct-visibility includes;
- a maintained restriction ledger distinguishing durable rules from proof,
  lowering, library, and undecided-choice work with explicit readiness roles,
  clients, and owners;
- documented ownership, range, optimizer, build, and runtime boundaries.

Still required:

- ordered MIR materialization facts plus closed C++ production migrations that
  cannot inherit host argument ordering; normal-exit temporary/drop authority
  is complete;
- the bounded public concurrency outcome built on the implemented
  concurrent-global policy;
- the readiness-selected source-text work; all selected add/subtract/multiply
  integer modes and binary64 are implemented;

### Milestone 1: lifetimes, places, and ownership flow - foundation complete;
remaining families co-deliver with backend recovery

Implemented foundation:

- non-null read-only and mutable references;
- receiver/argument borrow origins through semantics, HIR, and MIR;
- local, call-result, stored, and escaping return loan identities;
- single-origin read-only owner dependencies selected from a method receiver
  or one eligible free/static parameter and preserved through ordinary calls,
  concrete generic carrier relays, moves, returns, and drops;
- places with field, index, and dereference projections;
- explicit moves, lexical drops, cleanup edges, and `EndBorrow` instructions;
- typed lexical/value obligations, exact concrete cleanup descriptors,
  initialize/move/reparent/replace/transfer/drop events, and normal-edge
  exactly-once verification;
- LIFO full-expression cleanup for supported materialized values, including
  branch-local logical/conditional temporaries, return transfer, and reverse
  lexical destruction;
- full-expression endings for non-retained call-result loans;
- semantic loan identities with owner, origin, carrier, access, and storage
  protection metadata;
- move transfer of one retained loan identity between borrowed-state carriers;
- read-only alias attachment to one retained loan identity, with loan-wide use
  collection and one endpoint plan across all carriers;
- distinct mutable or read-only child loans derived from a mutable local loan,
  including nested and known-disjoint sibling chains, parent suspension, and
  full reactivation after the final active child's frontend-selected endpoint;
- exact prefix-overlap conflict validation for stable symbol/receiver roots
  with named-field and checked-dereference projections, conservative unknown
  divergence, and independent known sibling fields;
- exact last-use endings for one supported local loan whose uses remain in a
  single straight-line statement region;
- path-specific endings for one loan across linear `if` arms,
  including branch-entry endings for paths with no carrier use;
- recursive path-specific endings through nested `if` trees, including
  reachable nested merges and ordinary cleanup on terminating arms;
- loop-carried last-use projection for a pre-existing local loan
  across `while`, body-first `do`/`while`, and classic `for`, with one endpoint
  after condition-false and `break` paths converge and no endpoint on a
  backedge or `continue`;
- unified switch-exit endpoints for one pre-existing local loan,
  without claiming general nested switch/loop flow;
- proven same-path early endings after a carrier's final use before an
  invalidation immediately followed by the matching `break`, with MIR
  normalization on every relevant outgoing edge before the shared join;
- per-iteration conditional endings for a carrier created inside a loop body,
  while loans first created in a `for` initializer retain lexical loop-scope
  cleanup;
- path-sensitive MIR verification of one loan producer, represented active
  uses, balanced normal exits, and equal incoming loan state at CFG joins,
  including the newly normalized switch and immediate-break edges;
- explicit movement of named writable fields rooted in local values,
  parameters, checked owner dereferences, or mutable `this`, with
  flow-sensitive use checks and definite receiver reinitialization;
- fallthrough-aware move-state joins for terminating `if` arms and unreachable
  tails, short-circuit move-state joins, and explicit `break`/`continue` value
  snapshots; and
- conservative whole-value and projected-field availability validation on
  every reachable `while`, `do`/`while`, and classic `for` backedge.

Still required:

- generalized exclusive-loan graphs for indexed, raw, opaque, stored, or
  escaping provenance beyond the bounded local stable-place slice;
- dynamic-index partial movement and generalized place aliasing beyond the
  bounded constant-index ownership slice;
- a general fixed-point transfer authority for repeated loop headers and
  arbitrary CFG joins, replacing the current bounded semantic snapshots;
- ordered child/parameter/result materialization, partial-constructor rollback,
  and unwind-free defined-failure cleanup semantics; and
- owner-dependency graphs beyond the implemented direct read-only
  single-origin case, including stored or escaping mutable dependencies,
  multiple or nested origins, and dependency-changing assignment.

The MIR verifier remains a guardrail rather than the authority that chooses
loan endpoints. Semantic analysis chooses the implemented straight-line,
conditional, nested-arm, unified loop/switch-exit, or proven same-path
immediate-break endpoint; HIR carries that decision; and MIR materializes
and normalizes it on the relevant outgoing edges. The verifier does not infer
endpoints or place aliasing, but it validates exclusive child/parent identity,
suspension and reactivation, and requires predecessor loan states to agree at
every reachable join.

### Milestone 2: containers, iterators, and ranges - first container slice

Structural range `for`, source-defined read-only iterators, and one confined
stored-reference owner dependency exist. The first ordinary source-defined
`std::vector<T>` now uses private checked storage for default/size construction,
observation, reserve, clear, push/pop, checked access, variadic exact in-place
`emplace_back`, indexed insertion/erasure, movement, and conservative read-only
iteration. Its element type must be movable and cannot contain borrowed state.
The vector name has no compiler privilege.

This does not complete the milestone. Free/static factories can now return one
direct read-only owner-tied cursor or view, including through a concrete
generic carrier relay, but fixed-array range iteration, owned temporary ranges,
per-iteration element loans, mutable iteration, nested/multi-owner views, and
precise invalidation effects remain incomplete. Bounded local exclusive
reborrows provide a prerequisite for mutable access, but they do not create the
range-level or per-iteration loan protocol by themselves.

### Milestone 3: callables and generic capabilities - confined invocation

Typed lexical lambdas, direct confined generic callable parameters,
declaration-order-independent confined forwarding, named concepts, lifecycle
and comparison capabilities, value generics, and restricted packs are
implemented. Concepts now include multi-parameter source composition, bounded
validity-only trailing `requires`, and exact input-iterator, iterator/sentinel,
and accumulation-referent structural capabilities used by source-defined
numeric algorithms. Confined calls support exact `void`, `bool`, and
context-supplied non-reference value results without tracked borrowed state or
lambda identity; operation-based `std::accumulate`, `std::inner_product`, and
unary `std::transform_reduce` are implemented.
Restricted packs now also have one non-consuming comma call fold. It selects
one free/static generic `void` declaration symbolically and records exact
left-to-right element instances; it is sufficient for integral formatting but
does not add pack indexing, per-element overload selection, arbitrary fold
operators, or mutable/moved element access.
Immutable confined parameters require read-callable targets. Mutable confined
parameters retain one local callable and accept read-callable or mut-callable
targets for zero-or-more invocation; the implemented algorithms use this form.
An explicitly moved confined parameter requires once-callable invocation,
accepts reusable or consuming nominal targets, and is checked by ordinary
path-sensitive availability plus MIR move provenance.
Local lambda environments now retain ordered copy/move capture initialization,
exact capture places, closure movement, and cleanup. The bounded
`[target = std::move(source)]` form admits move-only owned captures without
opening general callable escape. Exact same-type generic return and one exact
generic field owner also admit explicit ownership movement without inferred
closure results or erasure.
Bounded scalar constexpr bindings, free functions, static methods, recursion,
structured control flow, and frontend-selected `if constexpr` are also
implemented. Callable result inference, broader owned escape/extraction,
complete-range/callable/hash capabilities,
heterogeneous accumulation, and generic or aggregate constexpr evaluation
remain.

### Milestones 4 and 5 - selective groundwork

Several safe C++-familiar additions are complete, including owned conditional
expressions, arithmetic compound assignments, `do`/`while`, and the bounded
`extern "C"` call layer. The latter owns exact C symbols, a fixed-width scalar
allowlist, passive layout-stable records, nominal pointer-only opaque handles,
non-retained counted text inputs, and one-level scalar/record/handle/`void`
pointers whose calls are lexically unsafe when the signature contains a
pointer. Out-parameter families, callbacks, and annotated ownership transfer
remain unimplemented systems-readiness lanes; unrestricted casts and ABI
breadth remain later.
Project manifests can now provide structured target-aware native link inputs
and automatically compile declared package-contained C and C++ sources. The
public standard library has initial utility, ownership, array, string, vector,
view, math, and I/O foundations plus a bounded POSIX `std::tcp::socket` owner.
Owned process arguments are available through the typed hosted entry form;
environment access remains deferred. The library cannot yet claim connected
networking or the complete systems-ready container/view/algorithm surface
because the address/buffer ABI and Milestone 1
lifetime work are incomplete.

## Parallel Tracks

- **Optimizer/backend:** the bounded Stage A editor client and first Stage B
  shadow proof are sufficient. The immediate work is the active production MIR
  body emitter and cutover; broader shadow folds, per-function effects, and
  general pass management wait unless the selected migration family directly
  requires them. No new optimization may extend the HIR replacement bridge.
- **Build system:** immutable compiler/driver requests, executable/test
  manifest targets, and `build`, `check`, `run`, `test`, `clean`, and
  `metadata` are complete. Structured package/profile/target native inputs and
  declared C/C++ source compilation, deterministic whole-program caching, and
  canonical source-only workspace/path dependency graphs are also complete.
  Exact Git resolution/lockfiles and the read-only project-facts bridge are
  the next independent project-system rows.
- **Quality/tooling:** deterministic diagnostics, formatting, Tree-sitter,
  semantic tokens, completion, hover, and definition have foundations.
  Shipped GTI sources are parsed by Tree-sitter in CI; interface/pack signature
  presentation and current rainbow-delimiter nodes have position-sensitive
  regressions. Full symbol operations, project-aware analysis, fuzzing, and
  performance observability remain open.

## Operational Queue

The sole maintained work queue is
[`implementation-sequence.md`](implementation-sequence.md).
`M-BACK-01`/`scalar-leaf-v1`, `M-BACK-02`/`scalar-cfg-v1`, and
`M-BACK-02`/`scalar-direct-call-v1` plus
`M-BACK-02`/`class-default-cleanup-v1` and
`M-BACK-02`/`owned-lifecycle-call-v1` plus hosted
`M-BACK-02`/`scalar-failure-callgraph-v1` are complete. The active task is the
final authority cutover: inventory and migrate every remaining executable body
and initialization family, then remove compatibility-emitter body execution,
the HIR replacement bridge, and legacy native-failure helpers when their last
users are gone. Completion of all remaining `M-EXEC-01` work is not a global
prerequisite. Unrelated
executable language and optimizer breadth do not pre-empt that campaign.

This checkpoint records evidence rather than duplicating that queue. Every
future pass should update its row in the implementation sequence, this file's
implemented evidence, and the affected canonical architecture/language
documents in the same change.
