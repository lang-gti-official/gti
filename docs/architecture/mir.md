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
- lexical scopes, cleanup edges, loans, carrier bindings, and source/HIR
  provenance.

IDs are body-local and start at one.

## Lowering And Verification

For each body, MIR lowering creates an entry block and root scope, seeds
parameter cleanup, lowers prologue/construction values, lowers statements,
synthesizes a legal terminal edge, rebuilds reachability and value uses, then
verifies structure.

`verifyMirProgram` checks identity ranges, definitions and uses, terminators,
call/constructor metadata, native-linkage invariants, and reachable loan state.
Semantic analysis chooses proven borrow endpoints; HIR carries them; MIR emits
and verifies them. Verification is an integrity gate, not an alias or last-use
analysis that invents missing semantics.

`MirPrinter` must remain deterministic and address-free so tests and future
tooling can compare snapshots.

## Current Completeness Boundary

MIR currently represents CFG, scalar operations, places, calls, moves, loans,
raw-memory operations, drops, construction metadata, and use-def relationships.
It does not yet completely define general temporary lifetimes, partial
initialization, every active-drop transition, object/vtable layout, calling
conventions, a general ABI, or the runtime realization of every checked
operation.

Consequently `CppBackend` still emits from AST plus semantic/HIR data and does
not consume MIR bodies. Do not treat that transition as permission to add
semantic inference to the emitter. The migration plan is in
[`docs/plans/optimization.md`](../plans/optimization.md).
