# Optimization

Status: Current compatibility pipeline and MIR Stage A boundary.

GTI optimization currently has two entry paths in `OptimizationPipeline`.

## HIR Compatibility Pass

`run(const HirProgram&, OptimizationLevel, TargetInfo)` performs constant
folding at `-O1` and above and stores proven replacements by `HirValueId`.
`checked_integer.h`, `binary_float.h`, and the shared constant evaluator define
fixed-width GTI numeric behavior. Private `llvm::APInt` and `llvm::APFloat`
computations implement those GTI-owned contracts. Integer overflow, zero
division/modulo, invalid shifts, out-of-range conversions, or any uncertain
outcome keep the checked operation rather than becoming a host C++ constant.
Binary32 literals, unary and binary arithmetic, mixed integer/float operations,
comparisons, and numeric conversions may fold only to the exact retained
`BinaryFloat` bit pattern. Floating exceptions produce their specified IEEE
default result rather than an integer-style failure.

`CppEmitter` still consumes this replacement table. Because one source
expression can produce several concrete HIR values, a source replacement is
available only when every instance has the same value.

## Owned MIR Path

`run(OptimizationRequest)` takes ownership of a `MirProgram`, verifies it, and
returns an `OptimizedProgram`. This path currently performs no transformation:
verified input is returned unchanged and the output verification record equals
the input record.

`computeMirDominance` is the first bounded MIR analysis. It copies one body CFG
into a private pointer-stable snapshot, runs LLVM's generic dominator
construction, and returns only GTI `MirBlockId` reachability and immediate-
dominator facts. The MIR verifier uses a freshly computed result to reject uses
not dominated by their definitions. No LLVM type, snapshot pointer, or cached
tree crosses the compiled implementation boundary; CFG changes require full
recomputation. Loop analysis and incremental dominator updates are not present.

`optimization/effects.h` provides exhaustive conservative effect tables for
MIR instructions, scalar operations, and intrinsics. New enum values must add a
classification. Effects are a safety boundary for future passes, not a report
of what the current C++ compiler happens to optimize.

## Ownership Rules

- Language semantics and checked arithmetic come from the frontend/evaluator.
- Host `float`, `double`, and C++ constant evaluation are not proofs of a GTI
  binary32 replacement.
- New CFG, propagation, reachability, use-def, place, and loan passes should
  operate on MIR.
- MIR mutations require centralized repair and post-pass verification before
  they can control emission.
- Source provenance and resolved identities must survive transformations.
- Native C++/LLVM optimization remains responsible for target instruction
  selection and machine-level work.

Pass management, controlled MIR editors, cached analysis invalidation,
incremental dominator updates, loop analysis, shadow folding, and
MIR-controlled emission are plans, not current infrastructure. See
[`docs/plans/optimization.md`](../plans/optimization.md) and
[`docs/plans/performance-tooling.md`](../plans/performance-tooling.md).
