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
  typed-HIR compatibility decisions
  + owned, verified identity MIR snapshot
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
gti_driver
  artifact policy + native toolchain request + process execution
    |
    v
CLI router and presentation
```

The staged design for MIR transformations, pass and analysis ownership, effect
classification, verification, optimization levels, and migration away from
HIR-to-source replacement side tables is specified in
[`docs/optimization-architecture-proposal.md`](optimization-architecture-proposal.md).
The concise current position against the 1.0 dependency plan is maintained in
[`docs/compiler-roadmap-status.md`](compiler-roadmap-status.md).

Compiler declarations and reusable data models remain under `include/gti/`,
while non-template implementations migrate incrementally into the compiled
`gti_compiler` target under `src/compiler/`. The lexer is the first completed
subsystem; MIR integrity, deterministic printing, effect classification, and
the identity optimization entry point are also compiled there. Migration
ordering, header rules, exact-version library contract, and acceptance criteria
are specified in
[the compiler library migration proposal](compiler-library-migration-proposal.md).

`include/gti/driver/` and `src/driver/` form the separately compiled
`gti_driver` layer. An immutable `CompilationRequest` carries one entry source,
standard-library layout, resolved target, optimization level, and C++ backend
standard through the shared frontend and backend pipeline. `NativeCompileRequest`
then carries ordered structured native inputs without exposing process or
artifact policy to `gti_compiler`. `ExecutableBuildRequest` owns the shared
generated-artifact and native invocation sequence used by both direct and
project builds. The manifest parser and project resolver produce an immutable
`ProjectBuildPlan`, which is converted to the same compilation and executable
build requests as direct mode. The plan owns the effective package/profile/
target native inputs selected from the same resolved `TargetInfo`; structured
paths are package-contained, mixed link operands retain semantic order, and
trusted argument escape hatches remain exact argv elements. TOML, discovery,
profile selection, package containment, output layout, and native processes
therefore remain outside `gti_compiler` without creating a second
language-compilation path.

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
from temporary storage before backend entry. A retained borrow creates a stable
semantic loan tied to its owner and carrier bindings. For one unshared carrier
whose uses remain in one straight-line statement region, semantic analysis
chooses the exact statement after which the loan ends. A use crossing an `if`
normally ends at its merge. When a mutation within an arm requires an earlier
end and no later use exists, semantics records one endpoint per path: after the
final use in a used arm, at a reachable nested merge, or at entry to an unused
arm. The planner recurses through nested `if` paths. A terminating arm relies
on its normal cleanup and contributes no state to the reachable merge. A use
crossing an ordinary loop projects a pre-existing unshared carrier to that
loop's shared exit: the loan remains active across condition, body, increment,
`continue`, and backedges, then ends after condition-false and `break` paths
converge. Loans created in a loop body remain per-iteration, while a loan first
created in a `for` initializer uses lexical loop-scope cleanup. Switches,
break-path-local early endings, reborrows, and shared carriers retain the
conservative lexical extent.
Moves transfer the same loan identity rather than creating a second dependency.
While a loan is active, moves or replacements of the owner, mutable receiver
calls, and direct mutating storage operations are rejected.

Nominal class and struct types derive ownership traits recursively from their
state-bearing base and fields after generic substitution. A type containing
compiler-private storage, directly or through another aggregate, is move-only
in the frontend; copy and use-after-move errors are rejected before backend
entry. Binding metadata is the backend contract for deciding whether semantic
immutability may lower to C++ `const` without preventing a validated explicit
move. Flow state applies to any named movable local or by-value parameter
consumed by `std::move`, not only unique owners.

Inheritance and polymorphism are frontend semantics rather than C++ defaults.
`SemanticModel` records each direct public base, whether it is an interface,
abstract and polymorphic class state, exact override roots, and static versus
virtual dispatch for every selected method or operator call. Virtual calls also
retain the concrete class that owned overload lookup, independently of the
override root that controls runtime dispatch. GTI permits one
state-bearing base plus interface bases, rejects diamonds, and performs no
object slicing or implicit call-argument upcasts. A derived value can initialize
or be returned as an explicit base reference. Interfaces are public pure
behavior contracts, and polymorphic destruction is compiler-owned lifecycle
metadata. A backend must consume these facts rather than rediscovering an
override set or dispatch mode from source names.

Native C linkage is also selected before backend entry. `ExternCDecl` retains
the source linkage block, while every enclosed `FunctionDecl` carries
`LanguageLinkage::C`. `FunctionInfo` validates the bodyless free-function shape,
closed scalar/counting-input ABI plus one-level scalar/`void` raw-pointer
allowlist, and one program-global exact C symbol; it records that linkage and
`externalSymbol`. Concrete HIR and MIR function instances copy both fields even
though the current C++ backend still emits from checked AST plus semantic/HIR
data. Calls therefore resolve through ordinary GTI lookup and retain a selected
function identity rather than gaining native behavior from a call-site
spelling. Semantic analysis also marks a selected pointer-bearing C call as an
unsafe operation, while leaving scalar-only and counted-input calls available
in safe code. See [`native-c-interop.md`](native-c-interop.md) for the source
and ABI contract.

`include/gti/hir.h` assigns stable IDs to concrete class, function,
constructor, destructor, binding, statement, and value instances. Executable
bodies retain explicit blocks, branches, loops, switches, declarations,
returns, and control flow. Switch HIR retains its typed subject, grouped arms,
normalized case constants, and arm-local statement IDs. Typed values identify
their operation and operands in evaluation order
while retaining resolved call edges, intrinsic identity, semantic value
metadata, lexical unsafe-operation identity, source-unit identity, and source
provenance. An unsafe source block remains an ordinary HIR block with an
explicit safety marker; dangerous expressions retain their classified
operation instead of relying on source spelling. Constructor initializer,
class field initializer, module, and destructor bodies use the same
representation. `std::move(value)` lowers to a unary HIR `Move` value, and MIR
preserves it as an explicit ownership-transfer instruction rather than
rediscovering transfer from a call name.
Concrete class HIR additionally retains substituted base instances, class kind,
abstract and polymorphic state, virtual method roots, and structured base or
field constructor initializers. HIR call values carry an explicit dispatch mode.
Unqualified member calls carry an explicit synthesized `this` receiver, so no
backend needs to infer member-call context from source spelling.

`include/gti/mir.h` lowers each concrete HIR body to validated basic blocks with
explicit `goto`, branch, switch, return, unreachable, and unit-exit
terminators. Typed places distinguish bindings, symbols, `this`, internal
temporaries, values, and loans, with field, index, and dereference projections.
Instructions make scalar computation, initialization, assignment, mutation,
moves, borrows, resolved calls, construction, lexical drops, and borrow ends
explicit. Raw address formation, pointer arithmetic, pointer difference, and
raw dereference/index/member projections are distinct MIR operations or places;
raw-memory instructions carry conservative effects and do not create semantic
loans. Return loans retain their source place and escape status. Confined
stored-reference classes identify one constructor borrow argument in semantics;
HIR carries that origin and marks reference-field access, while MIR represents
field-stored, every local carrier binding, and returned dependencies as
explicit loans. Class metadata records base instances, polymorphic state,
structured constructor initialization, and reverse field-drop order. MIR call
instructions preserve
static versus virtual dispatch; validation requires every virtual call to have
a resolved function target, receiver, and concrete dispatch owner.

MIR loan verification follows reachable control-flow paths. Every loan has one
producing borrow, call, or construction instruction; each semantic loan has at
most one MIR identity and a unique set of carrier bindings; explicit loan
operands, loan-rooted places, and borrowed bindings require an active loan;
`EndBorrow` requires an active loan; normal exits reject active non-escaping
loans; and CFG joins require identical incoming loan state. Semantic analysis
chooses proven source-level endpoints, HIR carries them on statements and
conditional branch entries, and MIR lowering materializes them as `EndBorrow`.
Verification checks that contract but does not choose last-use points or prove
place aliasing.

A call-result loan that is not retained by a reference or borrowed-state
binding ends at the enclosing full-expression boundary. Conditions end such
loans after producing their scalar condition and before transferring control,
so loop backedges recreate a fresh dynamic borrow instead of carrying one from
the previous iteration. A retained loan with one unshared carrier in a
straight-line statement region ends after its final proven use. Nested `if`
arms may end that loan independently when every reachable path has a proven
endpoint. Semantic use and conflict events retain their arm path, so the
planner can propagate an inactive-loan proof across a nested merge without
emitting a duplicate `EndBorrow`. HIR attaches endpoints through one recursive
statement-lowering wrapper, so unbraced arms and `else if` cannot bypass the
semantic fact. MIR discards terminating predecessors and merges the active-loan
and carrier state of reachable paths. For a loop-carried loan, semantics records
the loop statement as the endpoint, HIR carries that fact in `endedLoans`, and
MIR materializes it only after the loop's natural and `break` exits converge.
The loop header and every backedge therefore agree that the loan is active;
`continue` never ends it. Switch edges, break-path-local early endings,
reborrows, and shared carriers remain conservative.

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

Local structured bindings use the same rule boundary. `SemanticModel` records
one hidden source `BindingInfo` plus ordered field or fixed-index component
facts. HIR retains that owner and each projected binding explicitly. MIR emits
one source initialization and maps the visible symbols to subplaces of that
owner, registering cleanup only for the owner. The C++ backend currently emits
`const auto [names] = expression;`, but that spelling represents a decision
already validated by the frontend and is not the decomposition authority.

Generic class field initializers, functions, constructors, and destructors are
rechecked with concrete substitutions, so move-only arguments are accepted
when the body transfers them and rejected when the body copies them. A
diagnostic in an instantiated function or constructor body includes the
requesting call site.

Generic constraints resolve through namespace-scoped source `concept`
declarations. Public standard concepts and their implication graph live in
`stdlib/prelude.gti`; user concepts compose those declarations with conjunction.
The compiler maps only trusted `gti_internal` prelude declarations to
irreducible `GenericConstraintKind` atoms. Semantic analysis flattens a selected
concept to an exact capability set while checking symbolic bodies, inferred and
explicit arguments, symbolic forwarding, and constrained packs; concrete HIR
reanalysis validates the resulting instance. Concepts intentionally do not
lower to C++ concepts or delegate candidate ranking to C++. Concrete lifecycle
atoms query semantic ownership traits and constructor availability. Concrete
comparison atoms inspect substituted class member candidates for exact public,
read-only `bool` contracts; operator names or C++ expression validity alone
cannot satisfy them. Constrained `T()` construction is retained as an explicit
intrinsic through HIR and MIR so a future backend does not have to rediscover
its meaning from emitted syntax. The full boundary is recorded in
[`concepts.md`](concepts.md).
The transitional C++ backend represents validated comparison members with C++
operator spelling because generic GTI bodies are still emitted as C++
templates. This does not delegate GTI constraint satisfaction or overload
selection to C++; every emitted instantiation and direct comparison target has
already been checked by semantics and concrete HIR analysis.

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
identities, and a future LLVM backend will consume their MIR lowering. Virtual
calls additionally retain the selected contract identity, dispatch mode, and
concrete class that owned overload lookup.
The C++ backend uses native virtual dispatch as its current representation, but
it does not decide whether a call is virtual or whether an override is valid.
It reference-casts a virtual receiver to the recorded lookup owner before member
lookup, preserving frontend overload selection despite C++ name hiding while
still allowing native virtual dispatch to select the final override.

Restricted member operators follow the same boundary. Semantic analysis selects
one exact `operator*`, `operator->`, prefix `operator++`, `operator[]`,
`operator==`, `operator!=`, `operator()`, or contextual `operator bool`
candidate and records its function ID, result type, and reference access.
Callable objects support arbitrary arity, but still require one exact argument
list and cannot add method type parameters. The C++ backend emits a direct call
to that private method identity instead of declaring or invoking a C++
operator. Mutable reference results remain receiver-tied places in the
semantic model, so wrappers do not delegate access or overload rules to C++.

Range-based `for` is parser-owned sugar over this resolved core. Its generated
scope binds one stable range reference, iterator, and sentinel, then uses an
ordinary `ForStmt` with the selected comparison, dereference, and prefix
increment operators. HIR retains `RangeFor` source provenance around those
normal values and calls; MIR receives the existing loop CFG, including the
increment target for `continue`. Generated bindings are reserved and excluded
from semantic occurrences and completion, while diagnostics map back to the
source range colon. This keeps the protocol structural and prevents either
frontend or backend from recognizing public stdlib container names.
The confined read-only stored-reference carrier also supports owner-tied source
iterators. Fixed arrays, owned temporary ranges, mutable owner-tied iterators,
and precise iteration/element loan scopes remain staged in
[`iterator-range-proposal.md`](iterator-range-proposal.md).

Constructor overload resolution is likewise complete in the frontend. Each
class lifecycle record contains declared overloads plus generated or deleted
default construction, copy/move construction, copy/move assignment, and
declared or generated destruction. Construction expressions retain their
selected ordinary constructor ID, generated-default identity, or explicit
copy/move construction kind. Source declarations may independently default or
delete copy and move construction; they do not enter the ordinary overload set
or invoke C++ special-member suppression. A declared destructor
also records the active-drop requirement and makes the class noncopyable. The
C++ backend emits compiler-owned special members explicitly, so adding a source
constructor cannot accidentally suppress movement or another lifecycle
operation through C++ rules. Immutable fields are still rejected on writes by
GTI semantics but are not represented as physical C++ `const`, allowing
validated whole-object lifecycle operations.

Raw-pointer lowering keeps binding access separate from pointee access. An
immutable GTI `T*` binding must not become C++ `const T*`, because that would
silently change the pointee contract; only source `const T*` produces a
read-only C++ pointee. GTI binding immutability remains a frontend fact. The
backend emits raw dereference, indexing, arrow, address, and arithmetic only
after consuming the corresponding validated unsafe-operation metadata.

Derived constructors resolve their one state-bearing base initializer in the
frontend using exact constructor matching. The semantic model and HIR retain
whether each initializer targets a base or field, its selected constructor,
and whether default base construction was generated. This ordering and
selection are not delegated to a C++ initializer list.

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
local lambda bindings back to that closure instance. A generic function may
also record a direct by-value parameter as a non-escaping callable operation or
exact bool predicate. The symbolic body records each required invocation and
its exact void or bool result. Predicate typing is context-owned and limited to
direct bool conditions, explicit initializers or assignments, logical operands,
and returns. Concrete generic reanalysis resolves each site to one exact lambda
signature or class `operator()` target. HIR and MIR retain the callable
parameter, concrete signature and result, target identity, and confined
call-site argument indexes. A declaration-order-independent fixed point also
records edges where one direct generic callable parameter is passed to another
function's proven non-escaping callable parameter. Concrete HIR resolves each
edge to the selected function instance, and MIR preserves that target alongside
the callable contract. A parameter with no such contract remains an ordinary
generic value and cannot receive a forwarded closure.

The C++ backend emits a value-capturing C++ lambda and lowers symbolic callable
invocation through `gti_internal::backend::invoke`. GTI function objects expose
a hidden ADL bridge that forwards to the already selected mangled method; the
bridge is backend representation and cannot participate in GTI lookup or
overload selection. Capture eligibility, mutability, exact call matching, and
non-escape rules remain frontend decisions. Closure environment layout and
general escaping callable representation remain future backend-neutral work.

Conditional expressions are distinct typed HIR values with condition, true,
and false operands. Semantics analyzes each arm from the same incoming value
state, requires one exact result type, and merges moved-state facts afterward.
The result is an owned value rather than a C++ conditional lvalue; copyable
places are materialized and move-only places require explicit `std::move`.
Borrow-carrying result types remain rejected until MIR has a branch-selected
loan representation. MIR lowers the expression to a condition branch, one
initialization block per arm, and a shared result block, preserving lazy
evaluation independently of the backend. The C++ emitter wraps native `?:` in
an immediate `std::remove_cvref_t<decltype(...)>` cast. Native `?:` provides
lazy selection while the outer cast prevents C++ from preserving an lvalue when
both arms are places. This also works in namespace initializers and for generic
or unnamed result types without capture rules leaking into GTI.

Semantic analysis also computes a structural fallthrough summary for every
function and lambda body. Both reachable branches must terminate, literal
boolean conditions remove their unreachable branch, only the selected
target-condition branch contributes, switch-local `break` is consumed by its
switch, and an infinite loop is terminating only when its condition is proven
true and it has no reachable loop-local `break`. A `do`/`while` summary treats
the body as mandatory and a loop-local `continue` as an edge to the post-test
condition; MIR preserves that edge after body-scope cleanup.
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

Host I/O uses the same source-defined-library split. `<std/cstdio>` owns the
public `expected`, `std::io_errc`, `std::unique_ptr<std::FILE>`, and RAII policy.
The prelude declares the exact `gti_rt_*` host symbols in a bounded
`extern "C"` block, then ordinary `gti_internal::runtime` GTI wrappers call
them. Fixed-width scalars cross directly. A `gti_internal::text_view` argument
lowers to the C-compatible `gti_c_string_view { const char *data; uint64_t
length; }` record from `runtime/include/gti/c_abi.h`; it is an immutable,
counted, non-retained input and is not a GTI owner or general C struct surface.
Because the source declaration uses `std::string_view` rather than a raw
pointer, this reviewed conversion remains callable from safe GTI.
The runtime performs one-byte native reads without exposing descriptors to GTI
applications. The legacy `@runtime` declarations remain a closed
compiler-validated compatibility path, not the standard library's only route
to native symbols and not a general FFI.

`<std/tcp>` is the first optional library unit built directly over bounded C
linkage rather than a compiler-recognized runtime binding. Its POSIX `socket`
and `close` declarations remain fixed-width scalar calls; ordinary GTI code
owns move-only socket lifetime, explicit-close errors, and lexical cleanup. The
slice creates only an unconnected IPv4 stream socket. Address layout and
traffic buffers remain deferred even though the C ABI now accepts one-level
scalar/`void` pointers: GTI still has no reviewed socket-address record layout,
mutable slice abstraction, or ownership/retention contract for traffic APIs.

Compiler-private `gti_internal::storage<T>` is a semantic move-only owner, not
a C++ template leaked into the frontend. Its resolved intrinsic calls describe
allocation, variadic exact in-place construction, owner-tied read-only and
mutable borrows, destruction, and movable-element relocation in the semantic
model. Borrowed-state element types are rejected. Capacity and initialized-slot
state remain private safety bookkeeping and cannot be queried by GTI source;
nominal wrappers record their own logical size and capacity. The C++ backend
currently lowers the operations to an aligned RAII storage helper. HIR preserves
the storage-call operands and nested selected element constructor; MIR carries
the same constructor target so it can eventually replace the representation
and lower allocation through an LLVM-oriented runtime boundary.

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

`std::vector<T>` now applies these boundaries as an ordinary source-defined,
move-only class constrained to movable elements. Its initial public slice owns
logical size and capacity, performs checked indexing and growth, constructs
elements in place through a final by-value pack, and supplies conservative
read-only one-owner iteration. The pack avoids an intermediate element but is
not C++ perfect forwarding; copyable arguments may be copied at the vector
method boundary. Mutable iteration, precise invalidation, temporary ranges, and
the complete v1 container API remain staged language and library work.

More generally, `gti_internal` is the backend-neutral capability layer beneath
safe nominal standard-library classes. `std::unique_ptr`, `std::vector`, and
similar APIs own user-facing policy while trusted intrinsic declarations
provide only operations that ordinary GTI cannot yet express. Intrinsics may
enforce their own safety invariants but must not expose wrapper-level size,
capacity, engagement, or policy queries. The compiler binds those declarations
by semantic identity, not by a public wrapper's name. The declarations use
ordinary bodyless GTI function syntax in the implicit prelude; semantic
registration attaches the closed intrinsic kind, resolved calls retain the
selected function identity, and HIR does not enqueue those declarations as
bodyless function instances.
Lexical `unsafe {}` now exposes the bounded raw-pointer operations described in
[`raw-pointers.md`](raw-pointers.md). It does not re-export private
`gti_internal` allocation or storage capabilities, expose C++ representation
details, or make every internal operation public by default.

`include/gti/optimizer.h` exposes both sides of the middle-end transition. The
legacy transforming pass consumes executable typed HIR and records proven
constant replacements by stable `HirValueId`. The transitional C++ emitter maps
a source expression to its HIR values and applies a replacement only when every
concrete instance agrees on the constant.

The MIR entry point takes an owned `OptimizationRequest` and returns an
`OptimizedProgram`. It currently performs no transformations: it verifies and
returns a structurally identical snapshot. The CLI supplies that snapshot to
`BackendInput::mir`; `CppBackend` still ignores it while consuming the legacy
HIR result. `src/compiler/mir.cpp` owns shared reachability repair, value-use
index repair, and structural verification. `MirPrinter` serializes complete MIR
deterministically, and `optimization/effects.h` classifies instruction,
operation, and intrinsic behavior conservatively. Adding one of those enum
kinds without extending its table fails a compile-time coverage check. Calls
into runtime or user code are conservatively marked as possible synchronization
barriers even though GTI does not yet expose threads or atomics.

The initial pass folds:

- grouping and unary `+`, checked `-`, `!`, and `~` on constants;
- proven in-range integer arithmetic, remainder, bitwise operations, and
  defined shifts;
- constant equality and comparisons;
- constant `and`/`&&` and `or`/`||`, including short-circuit results.

`checked_integer.h` is the backend-neutral arithmetic contract used by this
compatibility pass. It evaluates signed-magnitude constants in an explicit
fixed-width domain and returns either a value or a precise failure category.
Only value outcomes become HIR replacements. Overflow, zero division or
modulo, and invalid shifts leave the original checked operation intact. Folded
integer emission carries an explicit GTI type so native literal typing cannot
change overload or conversion behavior. Fixed-array extent evaluation maps its
`uint64_t`-only grammar onto the same contract and translates failures into
extent diagnostics. Optimizations must implement GTI semantics, not inherit
whichever behavior the compiler host or C++ backend happens to use.

`-O0` disables GTI optimization and requests `-O0` from the native compiler.
`-O1`, `-O2`, and `-O3` currently enable the safe GTI folding pass and forward
the matching level to the native compiler. This leaves machine-level work to a
mature optimizer while GTI's own middle end develops.

## Next Optimization Work

Implement optimizations only after their required language rules and analysis
are explicit. The detailed order, capability gates, and acceptance criteria are
defined in
[`docs/optimization-architecture-proposal.md`](optimization-architecture-proposal.md).
Complete the remaining Milestone 1 infrastructure before enabling another
transforming pass:

1. Add optimizer-owned body/program editors with revision tracking and central
   repair after each rewrite.
2. Add explicit pass management plus cached analyses and conservative
   invalidation.
3. Add optional before/after MIR dumps through the performance-tooling
   contract.

After those foundations, the highest-value pass work remains:

1. Port the checked-integer evaluator and existing HIR folding decisions to MIR
   shadow mode, then compare both results before MIR controls emission.
2. Add local constant propagation over MIR values without crossing mutation or
   call boundaries.
3. Add MIR reachability simplification and remove proven unreachable branches
   and blocks.
4. Use MIR value definitions and indexed uses for dead-value elimination, then
   add place-level dataflow for redundant load/store removal.
5. Add range analysis to remove runtime checks only when safety is proven.
6. Add pass statistics and before/after IR dumps so optimization changes are
   measurable and diagnosable.

The staged benchmark harness, compiler phase telemetry, optimization remarks,
safety-operation reporting, IR inspection, and profiler source mapping are
specified in `docs/performance-tooling-proposal.md`.

Compile-time target conditionals already perform source-level branch selection.
Do not duplicate inactive target branches in later representations.

## Path To LLVM

Typed HIR is the first target-independent instance representation, and MIR now
supplies structural control flow, explicit scalar operations, value use-def
information, and ownership lowering. Neither is yet a complete LLVM-facing
representation. The model now classifies values, places, access, ownership,
transferability, lexical drop requirements, and class lifecycle operations,
including declared cleanup, active-drop policy, inheritance graphs, override
roots, and virtual call dispatch. GTI still lacks complete lifetime analysis,
custom copy/move lifecycle bodies, polymorphic object layout, virtual table
layout, complete generic representation, and ABI rules.
Encoding those decisions prematurely in an LLVM-shaped IR would make backend
accidents into language semantics. See `docs/ownership.md` for the ownership
and allocation contract.

Adopt the following layers as those rules mature:

1. **Checked AST:** Syntax-preserving program plus semantic model.
2. **Typed HIR:** Implemented executable statement bodies, value operand graphs,
   stable value and symbol IDs, resolved calls, typed closure bodies, and
   concrete generic and inherited class instances. Calls retain their selected
   target and dispatch mode; concrete callable-parameter instances also retain
   required signatures and non-escaping argument metadata.
   Further syntax desugaring can move here as the C++ emitter stops consuming
   source structure directly.
3. **MIR:** Implemented validated control-flow graphs, body-local typed values,
   explicit scalar and mutation operations, short-circuit lowering, use-def
   indexing, projected places, resolved calls, moves, loans, lexical cleanup,
   class metadata, structured base construction, class field-drop order, and
   non-escaping callable contracts.
   General temporary lifetimes, concrete layouts and virtual tables, calling
   conventions, and target-independent runtime operations remain future work.
4. **Backends:** C++ source emission and LLVM IR emission consume the same MIR.

The C++ backend can move from checked AST to HIR and then MIR incrementally. A
future LLVM backend should be added only when MIR fully describes behavior that
is currently delegated to C++: destruction order, object layout, checked
integer operation realization, runtime calls, and instantiated generic types.

Backend-specific optimization remains valid after this split. The GTI middle
end owns language-aware transformations; the C++ compiler or LLVM owns target
instruction selection, register allocation, vectorization, and final machine
optimization.

## Reviewed Deferred Work

- `extern "C"` now gives bodyless free functions one explicit, exact native C
  symbol and a fixed scalar, counted-input-buffer, and one-level scalar/`void`
  pointer ABI. Pointer-bearing calls require lexical unsafe. Ordinary bodyless
  GTI declarations still do not acquire external linkage, and GTI-defined
  classes, references, owners, generics, or functions do not gain a stable
  binary ABI. Separate GTI compilation, exported GTI symbols, native record
  layouts, pointer-to-pointer and callback types, casts, and ownership transfer
  remain future interop work; they must not be inferred from this bounded call
  surface.
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
- Direct and project builds must enter through `gti_driver` compilation
  requests. Resolve `TargetInfo` before frontend entry and reuse that exact
  value for semantics, optimization, and backend generation.
- Keep native process execution, resource discovery, and artifact lifetime in
  `gti_driver`; `gti_compiler` must not acquire those dependencies.
- A backend accepts only a `FrontendResult` for which `canGenerateCode()` is
  true, including successful HIR and MIR construction.
- The current compatibility pass consumes typed HIR. New transforming passes
  converge on MIR and consume immutable HIR only for concrete-instance and
  interprocedural facts, following the optimization architecture proposal.
- Passes must not erase source provenance needed by diagnostics and tooling.
- Backend limitations must not become parser or semantic restrictions unless
  they are deliberate GTI language rules.
- Preserve C linkage and the exact external symbol from semantics through HIR,
  MIR, backend emission, diagnostics, and tooling; never reconstruct it from a
  namespace-qualified source name or delegate symbol selection to C++.
- New backends implement `Backend`; they do not branch throughout the frontend.
