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

A `MirBody` owns:

- basic blocks with `goto`, branch, switch, return, unreachable, or exit
  terminators;
- typed values with one defining instruction and indexed uses;
- places rooted in bindings, symbols, `this`, temporaries, values, or loans,
  with field/index/dereference projections;
- explicit initialize, assign, modify, move, borrow, call, construct, drop, and
  end-borrow instructions;
- resolved call targets, static/virtual dispatch, constructor targets,
  intrinsic identity, C linkage, and external symbols;
- the program-entry kind and exact concrete startup-append target for the owned
  command-line argument form;
- lexical scopes, cleanup edges, loans, carrier bindings, and source/HIR
  provenance.

IDs are body-local and start at one.

## Lowering And Verification

For each body, MIR lowering creates an entry block and root scope, seeds
parameter cleanup, lowers prologue/construction values, lowers statements,
synthesizes a legal terminal edge, rebuilds reachability and value uses, then
verifies structure.

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

One primitive scalar literal-identity family can now be transformed in
verified shadow MIR at `-O1` and above. It still does not control emitted code.

Consequently `CppBackend` still emits from AST plus semantic/HIR data and does
not consume MIR bodies. Do not treat that transition as permission to add
semantic inference to the emitter. The migration plan is in
[`docs/plans/optimization.md`](../plans/optimization.md).
