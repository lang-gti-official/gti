# Compiler Architecture And Optimization

GTI separates language analysis from target code generation. The current
pipeline is:

```text
source files
    |
    v
Frontend
  SourceLoader -> Parser -> SemanticVisitor
    |
    v
FrontendResult
  Program + SemanticModel + SourceManager + diagnostics
    |
    v
OptimizationPipeline
  checked-program optimization decisions
    |
    v
Backend
  CppBackend today; LLVM backend later
    |
    v
BackendArtifact
  C++ source today; object code later
    |
    v
CLI toolchain driver
```

## Current Boundaries

`include/gti/frontend.h` is the reusable frontend entry point used by both the
CLI and LSP. A `FrontendResult` owns the recovered AST, retained expression,
binding, function, and resolved-call semantics, source map, and diagnostics.
Expression metadata includes
value category, access, ownership, transferability, and drop requirements while
preserving the existing type query API. `canGenerateCode()` is true only when
source loading, parsing, and semantic analysis all succeeded. The LSP may
request semantic analysis of a recovered parse; backends must not run for that
result.

Nominal class and struct types derive ownership traits recursively from their
fields after generic substitution. A type containing compiler-private storage,
directly or through another aggregate, is move-only in the frontend; copy and
use-after-move errors are rejected before backend entry. Binding metadata is the
backend contract for deciding whether semantic immutability may lower to C++
`const` without preventing a validated ownership transfer.

`include/gti/backend.h` defines target-independent backend input and output.
Backends receive a checked `Program`, its `SemanticModel`, the selected target,
and an `OptimizationResult`. `CppBackend` implements this contract without
making C++ representation choices part of the language frontend.

Function overload resolution is complete before backend entry. The semantic
model assigns each declaration a per-program function ID and maps each valid
call to its unique selected declaration and instantiated signature. The C++
backend currently turns those IDs into private generated names, so the native
C++ compiler never chooses a GTI overload. A future HIR or LLVM backend consumes
the same identities.

Fixed array declarations normalize C++-style declarator extents into semantic
`Array(element, length)` types. Length participates in exact type identity and
is not runtime storage. Indexed expressions retain place/access metadata, so
element mutation follows the containing binding. Constant bounds failures are
frontend diagnostics; dynamic access lowers through a checked backend
operation that later range analysis may remove when safety is proven.

Compiler-private `gti_internal::storage<T>` is a semantic move-only owner, not
a C++ template leaked into the frontend. Its resolved intrinsic calls describe
allocation, capacity, construction, copied reads, destruction, and relocation
in the semantic model. The C++ backend currently lowers those operations to an
aligned RAII storage helper. A future HIR/MIR can preserve the same operation
identities while replacing the representation and lowering allocation through
an LLVM-oriented runtime boundary.

`include/gti/optimizer.h` is the first middle-end stage. It records proven
constant replacements against AST expression identities rather than mutating
parser-owned nodes. This keeps source structure and diagnostic locations stable
and makes every backend consume the same optimization decisions.

The initial pass folds:

- grouping and unary `+`, `-`, and `!` on constants;
- constant equality and comparisons;
- constant `and` and `or`, including short-circuit results.

It intentionally does not fold integer arithmetic. GTI must define signed
overflow and related arithmetic edge cases before compile-time evaluation can
soundly replace runtime behavior. Optimizations must implement GTI semantics,
not inherit whichever behavior the compiler host or C++ backend happens to use.

`-O0` disables GTI optimization and requests `-O0` from the native compiler.
`-O1`, `-O2`, and `-O3` currently enable the safe GTI folding pass and forward
the matching level to the native compiler. This leaves machine-level work to a
mature optimizer while GTI's own middle end develops.

## Next Optimization Work

Implement optimizations only after their required language rules and analysis
are explicit. The highest-value next steps are:

1. Define integer overflow behavior, then add typed constant arithmetic.
2. Add local constant propagation without crossing mutation or call boundaries.
3. Build control-flow graphs and remove proven unreachable branches and blocks.
4. Add use-def information for dead local elimination and redundant load/store
   removal.
5. Add range analysis to remove runtime checks only when safety is proven.
6. Add pass statistics and before/after IR dumps so optimization changes are
   measurable and diagnosable.

Compile-time target conditionals already perform source-level branch selection.
Do not duplicate inactive target branches in later representations.

## Path To LLVM

The checked AST and side-table semantic model are sufficient for the current
C++ backend, but they are not the final LLVM-facing representation. The model
now classifies values, places, access, ownership, transferability, and lexical
drop requirements. GTI still lacks complete lifetime analysis, object layout,
generic instantiation, and ABI rules. Encoding those decisions prematurely in
an LLVM-shaped IR would make backend accidents into language semantics. See
`docs/ownership.md` for the ownership and allocation contract.

Adopt the following layers as those rules mature:

1. **Checked AST:** Current syntax-preserving program plus semantic model.
2. **Typed HIR:** Stable value and symbol IDs, desugared constructs, resolved
   calls, and monomorphized generic instances.
3. **MIR:** Explicit control-flow graphs, temporaries, ownership operations,
   concrete layouts, calling conventions, and target-independent primitive
   operations.
4. **Backends:** C++ source emission and LLVM IR emission consume the same MIR.

The C++ backend can move from checked AST to HIR and then MIR incrementally. A
future LLVM backend should be added only when MIR fully describes behavior that
is currently delegated to C++: destruction order, object layout, integer edge
cases, runtime calls, and instantiated generic types.

Backend-specific optimization remains valid after this split. The GTI middle
end owns language-aware transformations; the C++ compiler or LLVM owns target
instruction selection, register allocation, vectorization, and final machine
optimization.

## Architecture Rules

- The CLI and LSP must enter analysis through `Frontend` so phase ordering and
  diagnostics cannot drift.
- A backend accepts only a `FrontendResult` for which `canGenerateCode()` is
  true.
- Optimization passes consume checked types and selected target information.
- Passes must not erase source provenance needed by diagnostics and tooling.
- Backend limitations must not become parser or semantic restrictions unless
  they are deliberate GTI language rules.
- New backends implement `Backend`; they do not branch throughout the frontend.
