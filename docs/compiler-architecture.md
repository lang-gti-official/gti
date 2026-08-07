# Compiler Architecture And Optimization

GTI separates language analysis from target code generation. The current
pipeline is:

```text
source files
    |
    v
Frontend
  SourceLoader -> Parser -> SemanticVisitor
                         -> HirLowerer
    |
    v
FrontendResult
  Program + SemanticModel + HirProgram + SourceManager + diagnostics
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
binding, function, class lifecycle, resolved-call, resolved-operator,
contextual-conversion, and resolved-construction semantics, typed HIR, source
map, and diagnostics.
Expression metadata includes
value category, access, ownership, transferability, and drop requirements while
preserving the existing type query API. `canGenerateCode()` is true only when
source loading, parsing, and semantic analysis all succeeded. The LSP may
request semantic analysis of a recovered parse; backends must not run for that
result.

Resolved calls also retain borrow origin independently of backend
representation. A read-only method `T&` result is tied to its receiver, while
an internal storage read is tied to its storage argument. The expression is an
addressable read-only place, and semantic analysis rejects a retained borrow
from temporary storage before backend entry. A retained borrow marks a
move-only root binding conservatively for the remainder of the function;
subsequent moves, replacements, mutable receiver calls, and direct mutating
storage operations are rejected.

Nominal class and struct types derive ownership traits recursively from their
fields after generic substitution. A type containing compiler-private storage,
directly or through another aggregate, is move-only in the frontend; copy and
use-after-move errors are rejected before backend entry. Binding metadata is the
backend contract for deciding whether semantic immutability may lower to C++
`const` without preventing a validated ownership transfer.

`include/gti/hir.h` assigns stable IDs to concrete class, function,
constructor, binding, and value instances. Each callable instance retains its
substituted signature, source declaration, resolved call edges, intrinsic
identity, and source provenance. Fixed generic function and constructor bodies
are rechecked with concrete substitutions, so move-only arguments are accepted
when the body transfers them and rejected when the body copies them. A
diagnostic in an instantiated body includes the requesting call site.

`include/gti/backend.h` defines target-independent backend input and output.
Backends receive the typed HIR together with the checked source program,
semantic model, selected target, and optimization result. The source program
and semantic model remain transitional inputs while the C++ emitter migrates
incrementally from syntax-oriented emission to HIR consumption.

Function overload resolution is complete before backend entry. The semantic
model assigns each declaration a per-program function ID and maps each valid
call to its unique selected declaration and instantiated signature. The C++
backend currently turns those IDs into private generated names, so the native
C++ compiler never chooses a GTI overload. Typed HIR consumes the same
identities, and a future LLVM backend will consume their MIR lowering.

Restricted member operators follow the same boundary. Semantic analysis selects
one exact `operator*`, `operator->`, `operator[]`, `operator==`, `operator!=`,
or contextual `operator bool` candidate and records its function ID, result
type, and reference access. The C++ backend emits a direct call to that private
method identity instead of declaring or invoking a C++ operator. Mutable
reference results remain receiver-tied places in the semantic model, so pointer
and container wrappers do not delegate access or overload rules to C++.

Constructor overload resolution is likewise complete in the frontend. Each
class lifecycle record contains declared overloads plus generated or deleted
default construction, copy/move construction, copy/move assignment, and
declared or generated destruction. Construction expressions retain their
selected constructor ID or generated-default identity. A declared destructor
also records the active-drop requirement and makes the class noncopyable. The
C++ backend emits compiler-owned special members explicitly, so adding a source
constructor cannot accidentally suppress movement or another lifecycle
operation through C++ rules. Immutable fields are still rejected on writes by
GTI semantics but are not represented as physical C++ `const`, allowing
validated whole-object lifecycle operations.

Declared cleanup is non-throwing and executes once for the active value before
reverse-order field destruction. The C++ backend currently represents this with
a private active flag and cleanup helper. Generated move construction transfers
the flag; generated move assignment first cleans the active target, moves its
fields, and transfers the flag. This is backend lowering for the frontend drop
contract, not a C++ ABI commitment. MIR will eventually represent the same
conditional drop directly.

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
aligned RAII storage helper. HIR preserves the same operation identities; MIR
can replace the representation and lower allocation through an LLVM-oriented
runtime boundary.

Unique ownership follows the same split. `std::unique_ptr<T>` is an ordinary
nominal class from the GTI prelude, while its private
`gti_internal::unique_owner<T>` field and allocation, borrow, and null-check
operations are semantic capabilities. The C++ backend maps only that internal
handle to C++ RAII; public operators and lifecycle behavior are emitted from
the resolved GTI class declarations.

More generally, `gti_internal` is the backend-neutral capability layer beneath
safe nominal standard-library classes. `std::unique_ptr`, `std::vector`, and
similar APIs should own user-facing policy while trusted intrinsic declarations
provide only operations that ordinary GTI cannot yet express. The compiler
binds those declarations by semantic identity, not by a public wrapper's name.
A future explicitly unsafe API may re-export selected capabilities for
low-level development, but that must not expose C++ representation details or
make every internal operation public by default.

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

The typed HIR is now the first target-independent instance representation, but
it is not the final LLVM-facing representation. The model
now classifies values, places, access, ownership, transferability, lexical drop
requirements, and class lifecycle operations, including declared cleanup and
active-drop policy. GTI still lacks complete lifetime analysis, custom
copy/move lifecycle bodies, object layout, generic instantiation, and ABI rules.
Encoding those decisions prematurely in an LLVM-shaped IR would make backend
accidents into language semantics. See `docs/ownership.md` for the ownership
and allocation contract.

Adopt the following layers as those rules mature:

1. **Checked AST:** Syntax-preserving program plus semantic model.
2. **Typed HIR:** Implemented stable value and symbol IDs, resolved calls, and
   concrete generic instances. Further syntax desugaring can move here as the
   C++ emitter stops consuming source structure directly.
3. **MIR:** Future explicit control-flow graphs, temporaries, ownership operations,
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
  true, including successful HIR construction.
- Optimization passes consume checked types and selected target information.
- Passes must not erase source provenance needed by diagnostics and tooling.
- Backend limitations must not become parser or semantic restrictions unless
  they are deliberate GTI language rules.
- New backends implement `Backend`; they do not branch throughout the frontend.
