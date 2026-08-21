# Optimization

Status: Current HIR constant-analysis pass and bounded authoritative MIR
transform.

GTI optimization currently has two entry paths in `OptimizationPipeline`.

## HIR Constant Analysis

`run(const HirProgram&, OptimizationLevel, TargetInfo)` performs constant
folding at `-O1` and above and stores proven replacements by `HirValueId`.
`checked_integer.h`, `binary_float.h`, and the shared constant evaluator define
fixed-width GTI numeric behavior. Private `llvm::APInt` and `llvm::APFloat`
computations implement those GTI-owned contracts. Integer overflow, zero
division/modulo, invalid shifts, out-of-range conversions, or any uncertain
outcome keep the checked operation rather than becoming a host C++ constant.
Explicit checked-result add/subtract/multiply instead fold to a GTI-owned
expected-state constant containing either the exact integer or the selected
out-of-range error. That operation is non-trapping; it does not weaken the
failure effects of ordinary checked operators.
Binary32 and binary64 literals, unary and binary arithmetic, mixed-width and
mixed integer/floating operations, comparisons, and numeric conversions may
fold only to the exact retained `BinaryFloat` bit pattern in the selected
width. Floating exceptions produce their specified IEEE default result rather
than an integer-style failure.

The C++ representation emitter still consumes this replacement table for
non-body source surfaces such as declaration initializers. It does not control
executable body text. Because one source expression can produce several
concrete HIR values, a source replacement is available only when every
instance has the same value.

## Owned MIR Path

`run(OptimizationRequest)` takes ownership of a `MirProgram`, verifies it, and
returns an `OptimizedProgram` containing both the unchanged canonical
`sourceMir` snapshot and the possibly transformed `mir`. At `-O1` and above it
runs one bounded fixpoint pass with three replayable edits:

- an exact scalar `Compute/Identity` chain ending in a literal becomes
  `Compute/Literal` with `IdentityFold` provenance;
- an admitted comparison, logical-not, binary-float conversion, or binary-
  float arithmetic computation over literal operands becomes
  `Compute/Literal` with `ComputeFold` provenance naming the original
  operation and operands; and
- a function-body `Branch` over a proven literal boolean becomes the selected
  `Goto` with `BranchFold` provenance. This CFG edit is confined to bodies with
  no loans, drop obligations, cleanup boundaries, failure records, or frozen
  program-initialization steps.

Every value candidate is compared by `HirValueId` with the HIR constant-
analysis result; disagreements are reported rather than rewritten. `-O0`
schedules no transform and remains byte-identical. The optimized MIR controls
production C++ for every executable source body.

MIR verification re-evaluates compute folds from dominating literal
definitions and requires a folded branch condition to dominate its terminator.
Before production emission, `verifyMirOptimizationCoherence` independently
replays every edit from canonical source MIR, rebuilds value uses and
reachability as required, and exact-compares the complete MIR structures.
Source MIR carrying optimizer provenance is rejected. No unlisted operation,
operand, CFG, instance/header, or metadata change is authorized. The comparison includes the
complete `MirProgramInitializationPlan`, including empty source-unit rows and
their order, plus every `MirBlock::programInitializationStep` tag. A pass may
rewrite an admitted instruction but cannot reorder, retarget, or stale the
program-wide initialization schedule. This coherence result is the
pipeline report's output verification, so unauthorized edits make
`OptimizedProgram::valid()` false before code generation; `CppBackend` repeats
the gate independently at ingress. The canonical source snapshot is a trusted
internal `Frontend`/`OptimizationPipeline` precondition, not an unforgeable
capability exposed by the public C++ structs; the backend still checks that it
belongs to the supplied semantic and HIR snapshot and fails closed when it is
absent or stale.
MIR v20 also introduced each function instance's definition provenance and
`mayRaiseDefinedFailure` bit. MIR v21 serializes the corresponding constructor
and destructor facts and makes the canonical derived effect result cover all
three instance kinds. MIR v22 serializes the merged program-initialization
unit/step plan and each block's exact step tag. The generic verifier accepts
conservative `true` but
permits `false` only when its own bounded analysis proves the claim; the
lowerer's two-pass fixed point records the exact derived result before
optimization begins. The current transform does not change call edges or the
defined-failure dimension.

`MirProgramEditor` is the first controlled mutation boundary. A pass accumulates
expected-operation and expected-instruction guards at a core
`MirBodyAddress` plus `{block,index}` address. Core MIR supplies the exhaustive
deterministic body inventory and const/mutable lookup used by the pass, editor,
and source/optimized coherence check. Applying a batch validates every address
before any mutation, sorts deterministically, rewrites a copied program,
rebuilds dirty value uses, verifies the complete result, and commits atomically.
Instruction, result, and HIR provenance IDs remain stable. Literal edits
invalidate instruction facts and value uses; a branch fold additionally
invalidates control flow, reachability, and dominance and recomputes the
derived indexes before verification. Integer replacements must fit the exact
target domain at the editor boundary; the verifier's contextual allowance for
signed-minimum lexical magnitudes is not a general rewrite permission.

The fold intentionally excludes strings, trapping integer arithmetic and
conversions, and dynamic values. A duplicated string literal may carry
construction/drop effects, while checked operations require proof that their
failure behavior is unchanged. Those families remain later client-driven
slices rather than assumptions hidden in this pass.

`computeMirDominance` is the first bounded MIR analysis. It copies one body CFG
into a private pointer-stable snapshot, runs LLVM's generic dominator
construction, and returns only GTI `MirBlockId` reachability and immediate-
dominator facts. The MIR verifier uses a freshly computed result to reject uses
not dominated by their definitions. No LLVM type, snapshot pointer, or cached
tree crosses the compiled implementation boundary; CFG changes require full
recomputation. Loop analysis and incremental dominator updates are not present.

`optimization/effects.h` provides exhaustive conservative effect tables for
MIR instructions, scalar operations, intrinsics, and exact synchronization
operations. New enum values must add a classification and stable name. A
trusted synchronization record supplies the precise memory footprint for an
atomic load or store and explicit task/runtime effects for spawn, join, and
mutex operations. Every such instruction is synchronizing, non-speculatable,
non-removable, and non-reorderable. Calls without an exact trusted operation
remain conservative unknown-memory/user-code synchronization barriers; that
fallback is not promoted into a happens-before proof. Effects are a safety
boundary for future passes, not a report of what the current C++ compiler
happens to optimize.

## Ownership Rules

- Language semantics and checked arithmetic come from the frontend/evaluator.
- Host `float`, `double`, and C++ constant evaluation are not proofs of a GTI
  binary32 or binary64 replacement.
- New CFG, propagation, reachability, use-def, place, and loan passes should
  operate on MIR.
- MIR mutations require centralized repair and post-pass verification before
  they can control emission.
- Source provenance and resolved identities must survive transformations.
- Native C++/LLVM optimization remains responsible for target instruction
  selection and machine-level work.

General pass management, cached analysis storage, incremental dominator
updates, loop analysis, and broader transform coverage remain plans rather than
prerequisites for production MIR emission. The executable-body cutover is
complete; future optimization work must add a verified MIR rewrite and extend
source-to-optimized coherence before it can affect generated code.
See [`docs/plans/optimization.md`](../plans/optimization.md) and
[`docs/plans/performance-tooling.md`](../plans/performance-tooling.md) for the
supporting transform and observability designs.
