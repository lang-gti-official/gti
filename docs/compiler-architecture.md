# Compiler Architecture And Optimization

GTI separates language analysis from target code generation. The current
pipeline is:

```text
source files
    |
    v
Frontend
  SourceLoader -> SourceGraph -> Parser (per source unit)
                              -> SemanticVisitor
                              -> HirLowerer
                              -> MirLowerer
    |
    v
FrontendResult
  Program + SourceGraph + SemanticModel + HirProgram + MirProgram
          + SourceManager + diagnostics
    |
    v
OptimizationPipeline
  typed-HIR optimization decisions
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
contextual-conversion, and resolved-construction semantics, typed HIR,
structural MIR, source map, source-unit dependency graph, and diagnostics.
`SemanticModel` also owns a source-unit occurrence database populated during
real semantic analysis. `SemanticTypePrinter`, `SignaturePrinter`, and
`LanguageQueries` consume that snapshot to answer backend-neutral tooling
queries. Completion performs a separate frontend analysis with an internal
cursor marker so semantic analysis can capture the live scope, receiver,
visibility, and overload candidates. The LSP retains the complete
`FrontendResult` and only translates positions and protocol data. It does not
resolve GTI names or reconstruct signatures.
Expression metadata includes
value category, access, ownership, transferability, and drop requirements while
preserving the existing type query API. `canGenerateCode()` is true only when
source loading, parsing, semantic analysis, HIR lowering, and MIR lowering all
succeeded. The LSP may request semantic analysis of a recovered parse;
backends must not run for that result.

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
`const` without preventing a validated explicit move. Flow state applies to any
named movable local or by-value parameter consumed by `std::move`, not only
unique owners.

`include/gti/hir.h` assigns stable IDs to concrete class, function,
constructor, destructor, binding, statement, and value instances. Executable
bodies retain explicit blocks, branches, loops, switches, declarations,
returns, and control flow. Switch HIR retains its typed subject, grouped arms,
normalized case constants, and arm-local statement IDs. Typed values identify
their operation and operands in evaluation order
while retaining resolved call edges, intrinsic identity, semantic value
metadata, source-unit identity, and source provenance. Constructor initializer,
class field initializer, module, and destructor bodies use the same
representation. `std::move(value)` lowers to a unary HIR `Move` value, and MIR
preserves it as an explicit ownership-transfer instruction rather than
rediscovering transfer from a call name.

`include/gti/mir.h` lowers each concrete HIR body to validated basic blocks with
explicit `goto`, branch, switch, return, unreachable, and unit-exit
terminators. Typed places distinguish bindings, symbols, `this`, internal
temporaries, values, and loans, with field, index, and dereference projections.
Instructions make scalar computation, initialization, assignment, mutation,
moves, borrows, resolved calls, construction, lexical drops, and borrow ends
explicit. Return loans retain their source place and escape status, and class
metadata records reverse field-drop order.

Computed results use body-local `MirValueId` identities. Every value records
one defining block and instruction, and each body indexes instruction,
terminator, value-root, and projected-index uses. A closed `MirOperation` enum
represents literals, aggregates, conversions, arithmetic, comparisons,
bitwise operations, checked indexing, mutation, and expected engagement without
requiring a backend to interpret `HirValueKind` or source tokens. `HirValueId`
is retained only as source provenance. Logical `and`/`or` lowers to branch
control flow and an internal boolean temporary, so the right operand remains
lazy. Contextual class-to-bool conversion lowers to the exact selected operator
call, while built-in expected truth testing lowers to `ExpectedHasValue`.

MIR still does not define object layout, ABI, general temporary lifetimes, or
the exact runtime realization of primitive arithmetic edge checks. The C++
backend therefore continues to consume the checked AST and typed HIR alongside
MIR while emission migrates incrementally. A new backend must not infer those
remaining GTI semantics from C++ behavior.

Scoped enums are resolved as nominal frontend types rather than integer
aliases. `SemanticModel` records each enum ID, source unit, fixed backing type,
and evaluated enumerator values; qualified enumerator expressions retain their
resolved owner and value. HIR carries the same declaration and expression
metadata, so a backend can choose its own enum representation without parsing
source names or relying on C++ conversion rules. The C++ backend currently
emits a fixed-backing `enum class` and never supplies GTI with implicit enum
conversions.

Local `auto` is resolved during semantic analysis rather than delegated to a
backend. Its `BindingInfo` records the exact initializer type, access mode, and
ownership traits, and concrete generic reanalysis carries those facts into each
HIR function instance. The C++ backend may preserve `auto` spelling as a
representation convenience, particularly for closure types, but it does not
own GTI inference or copy eligibility.

Generic class field initializers, functions, constructors, and destructors are
rechecked with concrete substitutions, so move-only arguments are accepted
when the body transfers them and rejected when the body copies them. A
diagnostic in an instantiated function or constructor body includes the
requesting call site.

Standard generic constraints are frontend capabilities attached to type
parameters. Semantic analysis uses them while checking symbolic bodies,
inferred and explicit arguments, symbolic forwarding, and constrained packs;
concrete HIR reanalysis validates the resulting instance. They intentionally do
not lower to C++ concepts or delegate candidate ranking to C++.

`include/gti/backend.h` defines target-independent backend input and output.
Backends receive validated MIR and typed HIR together with the checked source
program, semantic model, selected target, and optimization result. The source
program, semantic model, and direct HIR access remain transitional inputs while
the C++ emitter migrates incrementally toward MIR consumption.

`include/gti/source_graph.h` models canonical source units and explicit include
or prelude dependency edges. `SourceLoader` removes include directives while
retaining one token stream per unit. The frontend parses those streams
independently, records each unit's declaration range, and assembles a
dependency-ordered whole-program AST for the current backend. Semantic
registries publish declarations only to the declaring unit, its direct
consumers, and prelude consumers. The graph therefore prevents accidental
transitive and sibling visibility even though C++ emission still uses one
translation unit. Namespace scope and source-unit scope remain independent.

Function overload resolution is complete before backend entry. The semantic
model assigns each declaration a per-program function ID and maps each valid
call to its unique selected declaration and instantiated signature. The C++
backend currently turns those IDs into private generated names, so the native
C++ compiler never chooses a GTI overload. Typed HIR consumes the same
identities, and a future LLVM backend will consume their MIR lowering.

Restricted member operators follow the same boundary. Semantic analysis selects
one exact `operator*`, `operator->`, `operator[]`, `operator==`, `operator!=`,
`operator()`, or contextual `operator bool` candidate and records its function
ID, result type, and reference access. Callable objects support arbitrary arity,
but still require one exact argument list and cannot add method type parameters.
The C++ backend emits a direct call to that private method identity instead of
declaring or invoking a C++ operator. Mutable reference results remain
receiver-tied places in the semantic model, so wrappers do not delegate access
or overload rules to C++.

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

A class binding may use `Type name{arguments};` to avoid repeating its declared
type. The parser retains this as a distinct direct-initializer expression;
semantics supplies the declared class type and records the same exact
constructor identity used by `Type(arguments)`. HIR carries the direct
initializer and constructor edge. The C++ backend emits an explicit
`Type(arguments)` temporary at the declaration, rather than delegating GTI
selection to C++ list-initialization. Primitive, array, enum, reference, and
`auto` declarations do not gain brace semantics through this feature.

Declared cleanup is non-throwing and executes once for the active value before
reverse-order field destruction. The C++ backend currently represents this with
a private active flag and cleanup helper. Generated move construction transfers
the flag; generated move assignment first cleans the active target, moves its
fields, and transfers the flag. This is backend lowering for the frontend drop
contract, not a C++ ABI commitment. MIR records lexical drop points and
reverse field-drop order; explicit active-state transitions remain deferred
until custom lifecycle bodies are designed.

Fixed array declarations normalize C++-style declarator extents into semantic
`Array(element, length)` types. Length participates in exact type identity and
is not runtime storage. Literal arithmetic extents are checked and recorded in
`SemanticModel` as one uint64_t value, which the backend consumes without
re-evaluating source syntax. A generic class may carry a whole symbolic uint64_t
extent; typed HIR substitutes the concrete value before backend entry and
includes value arguments in class-instance identity. Indexed expressions
retain place/access metadata, so element mutation follows the containing
binding. Constant bounds failures are frontend diagnostics; dynamic access
lowers through a checked backend operation that later range analysis may remove
when safety is proven.

Lambda expressions receive semantic closure IDs, concrete parameter and return
types, immutable value-capture metadata, and structural ownership traits. HIR
stores each closure body as a `HirLambda` and resolves calls through copied
local lambda bindings back to that closure instance. The C++ backend currently
emits a value-capturing C++ lambda, but capture eligibility, mutability, exact
call matching, and non-escape rules are all frontend decisions. MIR lowers
each closure body and resolved call while leaving closure environment layout to
a future backend-neutral representation.

Semantic analysis also computes a structural fallthrough summary for every
function and lambda body. Both reachable branches must terminate, literal
boolean conditions remove their unreachable branch, only the selected
target-condition branch contributes, switch-local `break` is consumed by its
switch, and an infinite loop is terminating only when its condition is proven
true and it has no reachable loop-local `break`.
This prevents missing non-`void` returns from reaching C++ as undefined
behavior. Top-level `main` deliberately preserves the defined implicit zero
return familiar from C++. The current entry-point contract is a definition with
signature `int main()`; argument handling remains deferred until GTI has a
type-safe command-line argument representation.

Compiler-managed imports such as `<std/array>` resolve beneath the installed
GTI standard-library root during source loading. They produce ordinary direct
source-graph edges and never consult native C++ include paths. A standard unit
retains its logical import name so visibility diagnostics and the LSP can refer
to `<std/...>` rather than exposing installation-relative filesystem paths.

Compiler-private `gti_internal::storage<T>` is a semantic move-only owner, not
a C++ template leaked into the frontend. Its resolved intrinsic calls describe
allocation, construction, owner-tied read-only and mutable borrows, destruction,
and relocation in the semantic model. Capacity and initialized-slot state remain
private safety bookkeeping and cannot be queried by GTI source; nominal wrappers
record their own logical size and capacity. The C++ backend currently lowers the
operations to an aligned RAII storage helper. HIR preserves the same operation
identities; MIR can replace the representation and lower allocation through an
LLVM-oriented runtime boundary.

`std::string` applies that split to text. It is ordinary GTI source imported
from `<std/string>`, and its move-only lifecycle is derived from a private
`storage<char>` field. The frontend knows storage operations and borrow access,
but not the public string class name. Allocating duplication remains the
source-defined `clone()` operation; an owner-backed `std::string_view` is
deferred until its lifetime can be represented independently of the C++
backend.

Unique ownership follows the same split. `std::unique_ptr<T>` is an ordinary
nominal class from the GTI prelude, while its private
`gti_internal::unique_owner<T>` field and allocation, borrow, and null-state
operations are semantic capabilities. The C++ backend maps only that internal
handle to C++ RAII; public operators and lifecycle behavior are emitted from
the resolved GTI class declarations. `std::make_unique` is resolved and
instantiated as an ordinary variadic GTI function, not recognized by the
compiler or replaced by a backend allocation call.

Concrete HIR represents a variadic parameter pack with its ordered element
types instead of reconstructing them from call-shape metadata. A pack containing
any move-only element is one tracked ownership unit and is consumed by its first
whole-pack expansion. This supports ordinary source-defined forwarding helpers
without introducing C++ forwarding-reference deduction or pretending that HIR
can already address and move individual pack elements.

More generally, `gti_internal` is the backend-neutral capability layer beneath
safe nominal standard-library classes. `std::unique_ptr`, `std::vector`, and
similar APIs should own user-facing policy while trusted intrinsic declarations
provide only operations that ordinary GTI cannot yet express. Intrinsics may
enforce their own safety invariants but must not expose wrapper-level size,
capacity, engagement, or policy queries. The compiler binds those declarations
by semantic identity, not by a public wrapper's name.
A future explicitly unsafe API may re-export selected capabilities for
low-level development, but that must not expose C++ representation details or
make every internal operation public by default.

`include/gti/optimizer.h` is the first middle-end stage. Passes consume
executable typed HIR and record proven constant replacements by stable
`HirValueId`; they neither walk nor mutate parser-owned nodes. This keeps source
structure and diagnostic locations stable and makes every backend consume the
same optimization decisions. The transitional C++ emitter maps a source
expression to its HIR values and applies a replacement only when every concrete
instance agrees on the constant.

The initial pass folds:

- grouping and unary `+`, `-`, and `!` on constants;
- constant equality and comparisons;
- constant `and`/`&&` and `or`/`||`, including short-circuit results.

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
2. Add local constant propagation over MIR values without crossing mutation or
   call boundaries.
3. Add MIR reachability simplification and remove proven unreachable branches
   and blocks.
4. Use MIR value definitions and indexed uses for dead-value elimination, then
   add place-level dataflow for redundant load/store removal.
5. Add range analysis to remove runtime checks only when safety is proven.
6. Add pass statistics and before/after IR dumps so optimization changes are
   measurable and diagnosable.

Compile-time target conditionals already perform source-level branch selection.
Do not duplicate inactive target branches in later representations.

## Path To LLVM

Typed HIR is the first target-independent instance representation, and MIR now
supplies structural control flow, explicit scalar operations, value use-def
information, and ownership lowering. Neither is yet a complete LLVM-facing
representation. The model now classifies values, places, access, ownership,
transferability, lexical drop requirements, and class lifecycle operations,
including declared cleanup and active-drop policy. GTI still lacks complete
lifetime analysis, custom copy/move lifecycle bodies, object layout, complete
generic representation, and ABI rules.
Encoding those decisions prematurely in an LLVM-shaped IR would make backend
accidents into language semantics. See `docs/ownership.md` for the ownership
and allocation contract.

Adopt the following layers as those rules mature:

1. **Checked AST:** Syntax-preserving program plus semantic model.
2. **Typed HIR:** Implemented executable statement bodies, value operand graphs,
   stable value and symbol IDs, resolved calls, typed closure bodies, and
   concrete generic instances.
   Further syntax desugaring can move here as the C++ emitter stops consuming
   source structure directly.
3. **MIR:** Implemented validated control-flow graphs, body-local typed values,
   explicit scalar and mutation operations, short-circuit lowering, use-def
   indexing, projected places, resolved calls, moves, loans, lexical cleanup,
   and class field-drop order. General temporary lifetimes, concrete layouts,
   calling conventions, and target-independent runtime operations remain future
   work.
4. **Backends:** C++ source emission and LLVM IR emission consume the same MIR.

The C++ backend can move from checked AST to HIR and then MIR incrementally. A
future LLVM backend should be added only when MIR fully describes behavior that
is currently delegated to C++: destruction order, object layout, integer edge
cases, runtime calls, and instantiated generic types.

Backend-specific optimization remains valid after this split. The GTI middle
end owns language-aware transformations; the C++ compiler or LLVM owns target
instruction selection, register allocation, vectorization, and final machine
optimization.

## Reviewed Deferred Work

- The grammar permits ordinary bodyless function and method declarations, but
  only `@runtime` declarations currently have a compiler-defined external ABI.
  Defining how ordinary declarations bind across separately compiled GTI
  modules belongs with the module and linkage design; assigning ad hoc native
  symbols in the C++ emitter would make that future ABI accidental.
- Reference-returning method and operator calls are already classified as
  writable or read-only places in semantics, but parser-owned assignment nodes
  still cover only names, fields, indexes, and dereferences. Direct
  `object.borrow() = value` therefore remains unsupported. The correct next
  layer is one assignment/store representation whose target is any
  semantically checked place; adding a call-specific assignment node would
  duplicate the existing transitional AST design and is intentionally deferred.
- The formatter retains a lightweight token model so it can format incomplete
  editor buffers, but it must stay conservative where `&` is ambiguous. It no
  longer assumes capitalization implies a type. A future syntax-aware
  formatter should consume parser or Tree-sitter roles while retaining the
  malformed-buffer fallback instead of growing a second full grammar.

## Architecture Rules

- The CLI and LSP must enter analysis through `Frontend` so phase ordering and
  diagnostics cannot drift.
- A backend accepts only a `FrontendResult` for which `canGenerateCode()` is
  true, including successful HIR and MIR construction.
- Optimization passes consume typed HIR and selected target information.
- Passes must not erase source provenance needed by diagnostics and tooling.
- Backend limitations must not become parser or semantic restrictions unless
  they are deliberate GTI language rules.
- New backends implement `Backend`; they do not branch throughout the frontend.
