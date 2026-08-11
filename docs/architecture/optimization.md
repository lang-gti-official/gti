# Optimization

Status: Current compatibility pipeline and MIR Stage A boundary.

GTI optimization currently has two entry paths in `OptimizationPipeline`.

## HIR Compatibility Pass

`run(const HirProgram&, OptimizationLevel, TargetInfo)` performs constant
folding at `-O1` and above and stores proven replacements by `HirValueId`.
`checked_integer.h` and the shared constant evaluator define fixed-width GTI
integer behavior. Overflow, zero division/modulo, invalid shifts, or any
uncertain outcome keep the checked operation rather than becoming a host C++
constant.

`CppEmitter` still consumes this replacement table. Because one source
expression can produce several concrete HIR values, a source replacement is
available only when every instance has the same value.

## Owned MIR Path

`run(OptimizationRequest)` takes ownership of a `MirProgram`, verifies it, and
returns an `OptimizedProgram`. This path currently performs no transformation:
verified input is returned unchanged and the output verification record equals
the input record.

`optimization/effects.h` provides exhaustive conservative effect tables for
MIR instructions, scalar operations, and intrinsics. New enum values must add a
classification. Effects are a safety boundary for future passes, not a report
of what the current C++ compiler happens to optimize.

## Ownership Rules

- Language semantics and checked arithmetic come from the frontend/evaluator.
- New CFG, propagation, reachability, use-def, place, and loan passes should
  operate on MIR.
- MIR mutations require centralized repair and post-pass verification before
  they can control emission.
- Source provenance and resolved identities must survive transformations.
- Native C++/LLVM optimization remains responsible for target instruction
  selection and machine-level work.

Pass management, controlled MIR editors, analysis invalidation, shadow folding,
and MIR-controlled emission are plans, not current infrastructure. See
[`docs/plans/optimization.md`](../plans/optimization.md) and
[`docs/plans/performance-tooling.md`](../plans/performance-tooling.md).
