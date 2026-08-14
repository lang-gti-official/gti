# MIR

Status: Implemented structural CFG, ownership/effect, synchronization,
normal-exit lifecycle, and defined-failure identity foundation; not yet the
sole backend input.

MIR lowers each concrete HIR body into body-local control flow, values, places,
resolved calls, ownership operations, and cleanup. It is the intended
transformation and future-backend IR.

## Representation

`MirProgram` and `MirLowerer` live in `include/gti/mir.h`. Compiled repair,
verification, and deterministic printing live in `src/compiler/mir.cpp` and
`src/compiler/mir_printer.cpp`.

`MirProgram` copies the selected execution profile from `HirProgram` as
immutable program metadata. Body lowering and optimization do not rediscover
it from host/backend flags. The current profile fact constrains frontend
global/static validity. MIR verification also rejects represented
synchronization operations in the single-threaded profile, so a backend or
transform cannot introduce concurrent behavior after semantic checks.

The deterministic serialization is currently `mir-v15`/`mir-body-v15`.
Version 15 adds exact local defined-failure origin sets, snapshot-local source
anchors, and call-like propagation channels. It also extends the ordered-input
schedule to a concretely resolved class `operator()` receiver, using a
`MoveValue` checkpoint for an explicitly moved receiver or exact
trailing-`&&` target. Version 14 added
exact synchronization operation identity and atomic order metadata to call
instructions. It retained version 13's exact ordered-input
schedule for concrete ordinary constructor invocations with supported
arguments. A scheduled constructor has
one exact constructor target, no receiver, source-ordered argument checkpoints,
and a final `Construct` that consumes only those one-use prepared values.
Version 12 added exact global/static borrow-origin places to function summaries
and call instructions. Version 11 added exact class-copy and class-move
parameter checkpoints to the
ordered ordinary-call stage. A copy checkpoint retains the source place; a
move checkpoint retains the already materialized value and transfers any exact
active temporary obligation at that checkpoint rather than at the final call.
It retains version 10's explicit call-input roles and materialization order,
including exact owned-callable
return/field transport, complete declared-field metadata, and constructor
parameter-to-field move evidence introduced alongside that stage. It retains
version 8's ordered closure capture operands, copy/move modes, exact environment
symbols and capture-place projections and version 7's consuming
callable capabilities and full concrete semantic type on lambda-instance
records, so deterministic output exposes enclosing generic identity as well as
physical closure shape. It also retains the version-6 exact
read/mutable invocation requirements, selected and call-site capabilities,
and concrete function receiver mutability; the version-5 explicit callable
boundaries; and the version-4 body-local full-expression identities, exact
ordered cleanup membership, standalone boundary markers, and active-cleanup
metadata.

A `MirBody` owns:

- basic blocks with `goto`, branch, switch, return, unreachable, or exit
  terminators;
- one concrete `PlaceDomain` copied from HIR;
- typed values with one defining instruction and indexed uses;
- places rooted in bindings, symbols, `this`, temporaries, values, or loans,
  with field/index/dereference projections and, where semantics supplied one,
  the corresponding value-owned `PlaceKey`;
- explicit initialize, assign, modify, move, borrow, call, construct, drop, and
  end-borrow instructions, plus carried read/move/reinitialize ownership events;
- for the bounded ordinary-invocation slices, one non-removable,
  non-reorderable `CallInput` checkpoint per receiver and argument. Each
  checkpoint retains the source HIR value, call-site identity, receiver or
  exact argument-index role, selected parameter type, and value/class-copy/
  class-move/read-borrow/mutable-borrow mode. A class-copy checkpoint consumes
  one exact copy operand rooted in the source place. A class-move checkpoint
  consumes one exact materialized value and carries its `TransferOut` event
  when that value has an active drop obligation. Its single result has exactly
  one executable use by the matching `Call` or `Construct`, so the invocation
  consumes only prepared inputs rather than reevaluating a source operand;
- typed lexical/value drop obligations and per-instruction initialize, move,
  reparent, replace, transfer-out, and drop lifecycle events;
- resolved call targets, static/virtual dispatch, constructor targets,
  intrinsic identity, C linkage, and external symbols;
- exact defined-failure identity on checked instructions. Each local detector
  origin retains a sorted unique outcome set and a snapshot-local
  `SourceUnitId` plus line/offset anchor. Direct, virtual, constructor,
  callable, and future task-join propagation are separate channels and never
  copy a callee's possible category set. The verifier rejects invalid
  vocabulary, anchors, duplicates, instruction placement, or propagation that
  disagrees with the exact target and dispatch;
- exact backend-independent synchronization records on resolved calls. The
  verifier accepts thread spawn/join and mutex lock/unlock without an atomic
  order; requires a legal operation-specific order for atomic load, store, and
  read-modify-write; and requires a legal success/failure order pair for
  compare-exchange. Release loads, acquire stores, release/acquire-release
  failure orders, and a failure order stronger than success are rejected.
  Synchronization metadata on a non-call instruction is invalid;
- exact confined invocation and confined/owned argument-boundary records. The
  verifier permits this metadata only on calls; requires descriptors to be
  ordered, unique, within the call's operand list, and identical to the
  concrete target contract; and requires a confined invocation boundary if
  and only if the receiver traces to the matching enclosing callable-parameter
  binding. An owned call argument must be the exact moved parameter operand.
  An owned callee must either return that exact moved binding as a single-use
  result transferred out of local cleanup, or return the exact owner
  construction whose declared field and constructor initializer prove the
  corresponding parameter-to-field move. Direct result/field materialization
  is distinguished from a local temporary that requires an explicit
  transfer-out event. Class records retain all declared fields so this proof
  is not inferred from the lexical-drop subset;
- read-, mut-, or once-callable invocation on each concrete callable call,
  plus the required and selected capability for each exact generic signature.
  Mutable invocation requires an exclusive or owned receiver. Once invocation
  requires the receiver value to trace to an exact MIR `Move` carrying the
  matching available-to-moved ownership event. Every SSA value in that move
  and optional identity chain has exactly one use, ending at the verified call
  receiver or forwarding operand, so one move proof cannot authorize two
  consuming calls. Program verification checks the required capability against
  formal parameter access, binds the selected capability and full
  parameter/result signature to an exact lambda or `operator()` target, and
  validates each forwarding target, parameter index, concrete call edge,
  operand provenance, and consuming move back to the recorded source
  callable-parameter binding. Source move-state CFG analysis remains
  authoritative for path cardinality;
- the program-entry kind and exact concrete startup-append target for the owned
  command-line argument form;
- after M-FAIL-01, an explicit compiler-generated hosted-startup operation/body
  whose three local failure origins and `main` anchor lower from HIR rather
  than being synthesized by a backend, ordered before program initialization
  and source-entry parameter transfer by D-EXEC-01;
- lexical scopes, cleanup edges, loans, carrier bindings, and source/HIR
  provenance.

IDs are body-local and start at one.

## Lowering And Verification

For each body, MIR lowering creates an entry block and root scope, seeds
parameter cleanup, lowers prologue/construction values, lowers statements,
synthesizes a legal terminal edge, rebuilds reachability and value uses, then
verifies structure.

A valid HIR layout query lowers to an ordinary unsigned-64
`MirOperation::Literal` containing the retained frontend value. MIR has no
target-layout operation and therefore cannot reinterpret `sizeof` or
`alignof`; the source HIR identity remains available as provenance.

`MirClassInstance` carries the selected `[[c_abi]]` record size, alignment, and
ordered field offsets. Program verification checks that this metadata is
structurally complete and within the record bounds; deterministic printing
exposes it for audits. MIR does not contain a native-layout instruction.

Floating literals lower as ordinary `MirOperation::Literal` values tagged by
their semantic `float` or `double` type. The verifier requires the retained
`BinaryFloat` format to match that type, and deterministic printing uses
`f32:0x........` or `f64:0x................`. MIR contains no LLVM floating
representation.

Payload-enum construction and pattern extraction lower as distinct typed HIR
values and `MirOperation::PayloadConstruct`/`PayloadExtract` instructions with
the semantic enum, variant, field index, and ordered operands retained. An
exhaustive payload switch carries that semantic fact in HIR; MIR terminates its
otherwise-required unmatched CFG target with `Unreachable` instead of
inventing a path to the switch exit. The current extracted fields are passive
immutable copies, so this is not yet a move/borrow projection model.

Valid passive unions retain their checked size, ABI alignment, and ordered
field layout on semantic, HIR, and MIR class records. MIR does not infer an
active field or add a union operation: member access remains an ordinary place
operation carrying the frontend's unsafe classification.

Each defined integer operation lowers as an ordinary call with one of nine
exact intrinsic identities. Its effect-table entry is speculatable, removable
when unused, reorderable, non-trapping, and free of memory, ownership, and
user-code effects. Checked-result overflow is an ordinary error-state value,
not a MIR failure edge. These facts apply only to the explicit standard-library
operations; checked built-in arithmetic keeps its failure effect. MIR records
and verifies the selection but does not derive arithmetic mode from the public
function name.

`verifyMirProgram` checks identity ranges, definitions and uses, terminators,
call/constructor and synchronization metadata, native-linkage invariants,
program-entry adapter
metadata, value availability, reachable loan state, and lifecycle state. It
also requires each lambda instance's exact type to reproduce its declaration,
result, parameters, and captures, and requires callable targets to match that
full type. Same-shaped closures from distinct enclosing generic instances are
therefore not interchangeable. It rejects an adapter
identity on an ordinary or no-argument function, a malformed owned-argument
entry shape, and multiple MIR entry points. A value use in its defining block
must follow its defining instruction. A reachable cross-block use must be
dominated by the definition; this includes values referenced through place
roots and index projections. Current MIR has no block-parameter values: every
`MirValue` has one instruction definition, while constants, parameter
bindings, and entry loans use their own representations.

The program verifier additionally rejects every synchronization operation
when `MirProgram` selects the default single-threaded profile. The concurrent
profile permits the records but does not by itself make a public concurrency
API available. Public wrapper semantics, trusted capability selection,
task-transfer proof, runtime support, and backend lowering remain separate
prerequisites.

The owned-entry append target is a module-root call edge for reachability and
dead-code decisions even though it is not represented by an instruction in the
user function body. A pass may rewrite that edge only while preserving the
verified owner, return, parameter, linkage, and entry-kind contract.

`computeMirDominance` exposes an immutable GTI-owned result in block IDs. Its
compiled implementation validates the CFG, copies it into a private snapshot
with stable node addresses, runs LLVM's generic dominator calculation, and
copies only reachability and immediate-dominator IDs into the result. It is a
fresh full computation: neither the result nor the verifier retains pointers
into `MirBody` or the private snapshot.

The snapshot adapter has a private `printAsOperand(llvm::raw_ostream&)` hook
because LLVM's generic dominator implementation requires it for diagnostic
printing. This does not make `raw_ostream` GTI's MIR-printing abstraction;
public headers remain LLVM-free and `MirPrinter` remains GTI-owned.

M-OWN-02 maps each carried semantic/HIR key to body-local `MirPlaceId` metadata
and preserves constant index values independently of the SSA value that
computed an ordinary index. Its verifier runs a sparse
available/moved/uninitialized state-set fixed point over reachable CFG edges.
Local binding storage begins uninitialized, parameters begin available,
initialization activates a local lifetime, and drop returns the root to
uninitialized for roots that participate in an explicit move. The sparse
fixed point does not impose lifecycle state on unrelated legacy MIR places.
Moves require an exact available key, element assignment restores that key,
ancestor uses observe moved descendants, and different constant elements are
disjoint while dynamic selections remain may-alias. Branch and loop joins use
state-set union. A mismatched domain, forged move key, missing restoration, or
unavailable operand makes the MIR candidate invalid rather than producing a
late source diagnostic or relying on generated C++ behavior.

The current `MirPrinter` schema includes each body domain, carried place key,
constant or dynamic index metadata, ownership event, complete cleanup-relevant
class lifecycle shape,
exact class/lambda cleanup descriptor, typed drop obligation, call parameter
roles, full-expression and lexical cleanup-boundary tables, and lifecycle
event, so deterministic snapshots observe the same facts the verifier consumes.
It normalizes every nonzero
process-local snapshot generation to `1`; body/revision and all structural key
data remain exact.

`MirCanonicalPlace` remains for the older loan/carrier normalization and
checked-dereference verifier paths. Its index comparison now retains constant
and dynamic-selection metadata, but replacing that normalization with a
single key-producing loan transform is outside the bounded fixed-array slice.
Partial-construction rollback on a defined failure edge remains M-FAIL-01 work.

M-LIFE-01 maps each HIR obligation to an exact MIR place and runs a separate
available/moved/uninitialized fixed point over lifecycle events. Parameters
begin active; value and lexical initialization activate obligations; move,
reparent, replacement, and transfer-out preserve exactly one owner; and normal
return/exit edges must retain no active obligation. Full-expression cleanup is
LIFO, lexical cleanup is reverse construction order, and a temporary created
only by a logical or conditional branch retains a path-conditional obligation
through the merge and is dropped at the enclosing full-expression boundary.
Its cleanup place is body-local rather than a branch-local SSA root. MIR
publishes only reached HIR boundaries, retains their source identities and
ordered obligation membership, and emits one standalone boundary marker after
the reverse-order cleanup sequence. Verification rejects an absent, duplicate,
misplaced, or forged marker; cleanup before a completed root; active state at a
boundary; a swapped cleanup sequence; double or missing drops; forged cleanup
metadata; use of a non-consuming operand as an ownership transfer; and
predecessor states that cannot justify an event's conditionality. Lifecycle
instructions and boundary markers are observable effects and cannot be erased
merely because they produce no SSA result.

Each drop obligation retains a stable HIR construction ordinal. Reached
full-expression drops are a contiguous suffix immediately before their sole
boundary marker in reverse ordinal order. Each emitted lexical cleanup sequence
has its own table entry and marker, and every binding-kind `Drop` instruction is
covered by exactly one such sequence. Resolved `Call` and `Construct`
instructions retain their exact parameter types; only a non-reference input
role can justify an ownership-consuming lifecycle event, and program
verification cross-checks those roles against the concrete target signature.
Ownership-consuming ordinary calls and construction retain that exact target
identity. A confined callable forwarding edge retains the exact source
parameter binding and target contract. When that contract requires `Once`
directly or through another forwarding edge, every matching concrete edge must
be rooted in an ordinary MIR ownership move; a copied source value cannot
satisfy the cardinality proof. Closure construction names one concrete lambda
instance and retains one ordered operand, capture mode, exact type, and
environment symbol for every capture. A copy operand must name an exact
readable place; an owned-move operand must be the single use of an ordinary MIR
`Move` with matching ownership state. A cleanup-bearing moved value may retain
one non-executable value-root place only when that place is the exact value-drop
obligation; an alias place or executable use through it invalidates the move
proof. Lambda-body symbol places identify their exact capture index, and
program verification ties that projection to the concrete instance. Closure
movement and drop use the ordinary lambda lifecycle obligation, including
recursively cleanup-owning captured values. Class
lifecycle metadata contains every lexically dropped field in
declaration order; its independently retained drop projection must be the exact
reverse order. Trivial fields remain structural HIR/layout facts rather than
lifecycle-verifier authority.

## Controlled Optimization Edits

`optimization/rewrite.h` exposes a deliberately narrow `MirProgramEditor`.
Bodies are identified by GTI body kind plus owning instance ID; instructions
are addressed by block ID and zero-based instruction index with an expected
instruction-ID and operation guard. The editor never exposes mutable program
vectors or retains instruction pointers across an edit.

The first supported edit replaces a verified computation with a literal while
preserving instruction/result IDs, type information, unsafe classification,
and HIR/source provenance. All queued patches are checked before mutation and
applied to a copy in deterministic body/block/index order. Touched bodies have
their value-use indexes rebuilt, then the complete candidate program is
verified before the copy is committed. Invalid, stale, duplicate, or malformed
patch sets therefore leave the input byte-identical.

This replacement cannot change CFG, reachability, place/loan topology, or
dominance. Its edit result records those facts as preserved while marking
instruction facts and value uses invalidated and the latter repaired. There is
no general pass manager, analysis cache, incremental dominance update,
insertion/erasure API, or ID compaction yet; each is added only with a concrete
transform that needs it.

Semantic analysis chooses proven borrow endpoints; HIR carries them; MIR emits
and verifies them. Verification is an integrity gate, not an alias or last-use
analysis that invents missing semantics.

A global-origin function summary and every matching call instruction retain
one `BorrowOriginPlace`. Lowering materializes a symbol-rooted MIR place with
the summarized projections and creates the call-result loan from it. The
verifier rejects missing, forged, mismatched, receiver/argument-attached, or
wrong-return places. Unretained loans must still reach their selected
full-expression `EndBorrow`; retained mutable loans follow the semantic lexical
endpoint. The MIR profile remains metadata: mutable process-wide storage is
rejected by semantics in the concurrent profile rather than reinterpreted by
lowering.

One MIR loan may name multiple carrier bindings for a shared read-only
semantic loan. It still has exactly one producer and one path-sensitive loan
state; a shared loan uses its active and inactive states. Ending it invalidates
every carrier simultaneously, and verification rejects any subsequent carrier
use or inconsistent state at a join.

A bounded exclusive reborrow instead lowers as a distinct mutable or read-only
child loan linked to one mutable parent. Producing the child suspends the
parent; `EndBorrow` for a child reactivates it only when no other active child
remains. Nested chains apply the same transition recursively. Known-disjoint
sibling-field children may coexist, and a suspended parent may still be used
through a known-disjoint projection. Verification rejects direct or
overlapping use of a suspended parent, a read-only-to-mutable transition, or
inconsistent active/suspended state at a CFG join. The precise place model for
this slice is limited to stable roots with named-field and checked-dereference
projections; it does not claim indexed, raw, or opaque provenance.

`MirPrinter` must remain deterministic and address-free so tests and future
tooling can compare snapshots.

## Current Completeness Boundary

MIR currently represents CFG, scalar operations, places, calls, moves, loans,
raw-memory operations, drops, construction metadata, exclusive-reborrow
parent/child transitions, full-expression/lexical drop obligations, normal-exit
lifecycle transitions, and use-def relationships. It does not yet completely
define ordered receiver/argument/result materialization, partial constructor
initialization, failure-edge rollback, object/vtable layout, calling
conventions, a general ABI, or the runtime realization of every checked
operation.

It also does not yet completely implement
[Execution Section 4.2](../language/execution.md#42-evaluation-order). For the
bounded ordinary-invocation and concrete `operator()` slices, the verifier
requires a function receiver when applicable, argument inputs in exact
source/parameter order, and the final `Call` or `Construct` to form a strict
dominance/order chain. A read or mutable callable receiver uses its exact
borrow capability; an explicitly moved receiver or exact once-callable target
requires a `MoveValue` receiver checkpoint and ownership transfer. This
preserves an enclosing once-callable contract when exact selection falls back
to a read or mutable call operator. A scheduled constructor must have
no receiver and must retain its exact ordinary constructor target.
Verification rejects a direct unprepared operand, wrong
call-site, duplicate or abandoned checkpoint, swapped argument index,
type/role drift, and invocation before setup. Class-copy inputs additionally
require the exact copyable, non-borrowed source place. Class-move inputs require
the exact movable, non-borrowed materialized value and transfer its unique
active obligation at the checkpoint. Borrow-source, callable-source, ownership,
and loan-flow checks trace through the checkpoint's one underlying operand.

Other calls and expression families still rely on recursive lowering order,
which is not a verified materialization schedule and does not control
production emission. Borrowed-state class parameter construction,
generated/default and copy/move special construction, target-place formation,
result storage, operators other than concrete `operator()`, packs, unresolved
callables, conditional families, and failure rollback are incomplete. Module
initializers and each class's static-field initializer body remain separate
rather than one source-graph-derived hosted sequence.

M-LIFE-01 supplies body-local temporary identity, lifetime start, transfer or
reparenting, active drop, and LIFO full-expression obligations for the current
failure-free place slice. Later M-EXEC-01 slices must extend the ordered input
representation to borrowed-state class values, remaining call forms, result and
target places, operators, branches, and program initialization, with a
`ProgramInitializationStepId` where applicable.
The verifier must reject use before materialization, duplicate target
evaluation, invocation before parameter setup, cleanup-state mismatch at an
edge, and a boundary with a live untransferred obligation. A structural edit
that changes those regions invalidates and rebuilds their schedule and cleanup
facts.

MIR now carries exact local possible-outcome sets, snapshot-local origin
anchors, and direct/virtual/constructor/callable propagation identity required
by the first M-FAIL-01 slice. Any such operation projects conservatively to
`mayTrap`, making it non-speculatable, non-removable, and non-reorderable, but
the structured identity remains the authority. MIR does not yet carry
artifact-local site IDs, fixed failure records, failure successors, cleanup
unwinding, caller-control-flow propagation, or containment edges required by
[Execution §4.10](../language/execution.md#410-defined-runtime-failure).

After the completed M-LIFE-01 temporary and active-drop facts and after
M-EXEC-01 decomposes the relevant calls, construction, and checked expressions,
The next M-FAIL-01 slice must add one `Invoke`-style terminator with normal and
failure successors. An origin form carries the exact local outcome set plus an
artifact-local `FailureSiteId`; a call/constructor/virtual form carries only a
`mayPropagateFailure` channel and preserves a callee record byte-for-byte. The
normal successor receives an optional typed result block parameter and the
failure successor receives a fixed failure-record block parameter. This extends
the current instruction-only `MirValue` definition rule; a checked operation
cannot branch from the middle of a basic block or smuggle its result through an
unverified native exception.

Cleanup blocks forward the same record through supported initialized and
partially initialized shapes to the hosted-program boundary. A second origin
while the primary record is in cleanup constructs the fixed emergency envelope.
The same representation supplies reusable boundary primitives that later task
and callback rows plus E-EMBED-01 can integrate without changing the failure
effect. The verifier must reject missing/forged sites, origin-incompatible
categories, a propagating edge that re-sites or rewrites the record, a normal
result used on the failure edge, and cleanup/control-flow joins with mismatched
record state. Optimizers preserve the first observable origin, site, cleanup,
and prior effects.

One primitive scalar literal-identity family can now be transformed in
verified shadow MIR at `-O1` and above. It still does not control emitted code.

Consequently `CppBackend` still emits from AST plus semantic/HIR data and does
not consume MIR bodies. Do not treat that transition as permission to add
semantic inference to the emitter. The migration plan is in
[`docs/plans/optimization.md`](../plans/optimization.md).
