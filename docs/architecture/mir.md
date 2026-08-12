# MIR

Status: Implemented structural CFG and ownership/effect foundation; not yet the
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
global/static validity; future synchronization and task operations will
consume it only in their owning rows.

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
- resolved call targets, static/virtual dispatch, constructor targets,
  intrinsic identity, C linkage, and external symbols;
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

Each defined integer operation lowers as an ordinary call with one of six
exact intrinsic identities. Its effect-table entry is speculatable, removable
when unused, reorderable, non-trapping, and free of memory, ownership, and
user-code effects. These facts apply only to explicit wrapping/saturating
operations; checked built-in arithmetic keeps its failure effect. MIR records
and verifies the selection but does not derive arithmetic mode from the public
function name.

`verifyMirProgram` checks identity ranges, definitions and uses, terminators,
call/constructor metadata, native-linkage invariants, program-entry adapter
metadata, value availability, and reachable loan state. It rejects an adapter
identity on an ordinary or no-argument function, a malformed owned-argument
entry shape, and multiple MIR entry points. A value use in its defining block
must follow its defining instruction. A reachable cross-block use must be
dominated by the definition; this includes values referenced through place
roots and index projections. Current MIR has no block-parameter values: every
`MirValue` has one instruction definition, while constants, parameter
bindings, and entry loans use their own representations.

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

`MirPrinter` version 2 includes each body domain, carried place key, constant
or dynamic index metadata, and ownership event, so deterministic snapshots
observe the same facts the verifier consumes. It normalizes every nonzero
process-local snapshot generation to `1`; body/revision and all structural key
data remain exact.

`MirCanonicalPlace` remains for the older loan/carrier normalization and
checked-dereference verifier paths. Its index comparison now retains constant
and dynamic-selection metadata, but replacing that normalization with a
single key-producing loan transform is outside the bounded fixed-array slice.
Complete path-conditional drop obligations and partial-construction rollback
remain M-LIFE-01 work.

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
parent/child transitions, and use-def relationships. It does not yet completely
define general temporary lifetimes, partial initialization, every active-drop
transition, object/vtable layout, calling conventions, a general ABI, or the
runtime realization of every checked operation.

It also does not yet implement
[Execution Section 4.2](../language/execution.md#42-evaluation-order). The
lowerer recursively visits many operands left to right and gives logical and
conditional expressions explicit CFG, but that traversal is not a verified
full-expression schedule and does not control production emission. Calls remain
ordinary instructions after inline operand lowering; target-place formation,
parameter/result materialization, and full-expression cleanup are incomplete.
Module initializers and each class's static-field initializer body are separate
rather than one source-graph-derived hosted sequence.

M-LIFE-01 must add body-local temporary identity, lifetime start, transfer or
reparenting, active drop, and LIFO full-expression obligations. M-EXEC-01 must
then decompose receivers, parameters, target places, operators, branches, and
program initialization into ordered instructions/CFG, with an explicit
`FullExpressionId`/boundary and `ProgramInitializationStepId` where applicable.
The verifier must reject use before materialization, duplicate target
evaluation, invocation before parameter setup, cleanup-state mismatch at an
edge, and a boundary with a live untransferred obligation. A structural edit
that changes those regions invalidates and rebuilds their schedule and cleanup
facts.

MIR also does not yet carry the failure records, possible-outcome sets,
failure successors, caller propagation, or containment edges required by
[Execution §4.10](../language/execution.md#410-defined-runtime-failure). The existing
`mayTrap` effect is conservative scheduling information; it cannot distinguish
a defined checked failure from unsafe/native behavior and does not identify a
category or source site. Retaining an HIR value ID is useful provenance but is
not sufficient authority for a MIR-only backend.

After M-LIFE-01 establishes temporary and active-drop facts and M-EXEC-01
decomposes the relevant calls, construction, and checked expressions,
M-FAIL-01 must add one `Invoke`-style terminator with normal and failure
successors. An origin form carries the exact local outcome set plus an
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
