# GTI Compiler Internals

Use this reference before changing compiler implementation, semantic metadata,
HIR, MIR, optimization, or backend lowering. It documents the code paths and
authority boundaries that are expensive to reconstruct from the large
transitional implementation. Declarations remain under `include/gti/`; compiled
subsystem implementations move to `src/compiler/` according to
[the compiler library migration proposal](../../../../docs/compiler-library-migration-proposal.md).

Treat the symbols and ordering below as navigation anchors rather than frozen
line numbers. Confirm them with `rg` because the compiler is evolving.

## Contents

- [Fast Source Orientation](#fast-source-orientation)
- [End-To-End Execution](#end-to-end-execution)
- [Data Ownership And Authority](#data-ownership-and-authority)
- [Semantic Analysis Internals](#semantic-analysis-internals)
- [HIR Internals](#hir-internals)
- [MIR Internals](#mir-internals)
- [Optimization And Backend Transition](#optimization-and-backend-transition)
- [Worked Trace: Binary Expression](#worked-trace-binary-expression)
- [Worked Trace: Resolved Call](#worked-trace-resolved-call)
- [Test Navigation](#test-navigation)
- [Maintenance Contract](#maintenance-contract)

## Fast Source Orientation

Start with these searches instead of scanning the 12,000-line semantic analyzer
or every lowering file from the beginning:

```sh
rg -n "class Lexer|Lexer::" include/gti/lexer.h src/compiler/lexer.cpp
rg -n "FrontendResult|Frontend::|CompilationRequest|compileToCpp" \
  include/gti/frontend.h include/gti/driver src/driver
rg -n "bool check\(const Program|registerNamespaces|resolveClassInheritance|resolveInheritedMembers|recordClassLifecycles" \
  include/gti/semantic_analyzer.h
rg -n "class SemanticModel|struct (ExpressionInfo|BindingInfo|FunctionInfo)|Resolved.*Info|CallDispatch" \
  include/gti/semantic_analyzer.h
rg -n "class HirLowerer|seedDeclarations|processPendingInstances|lowerExpression" \
  include/gti/hir.h
rg -n "class MirLowerer|class MirBodyLowerer|rebuildMir|verifyMir" \
  include/gti/mir.h src/compiler/mir.cpp
rg -n "OptimizationPipeline|OptimizationRequest|BackendInput|CppBackend|class CppEmitter" \
  include/gti src/compiler/optimizer.cpp src/driver/compilation.cpp
rg -n "MirEffectTraits|instructionEffects|operationEffects|intrinsicEffects" \
  include/gti/optimization src/compiler/optimization
rg -n "^void test[A-Za-z0-9_]+\(\)" tests/compiler_tests.cpp
```

For one syntax construct, trace its AST class name across the compiler:

```sh
rg -n "visitCallExpr|HirValueKind::Call|MirInstructionKind::Call" include/gti
rg -n "visitBinaryExpr|HirValueKind::Binary|MirOperation::" include/gti
```

## End-To-End Execution

`Frontend::analyze()` in `include/gti/frontend.h` is the reusable compiler
entry point. The CLI and LSP must not recreate its ordering.

```text
SourceLoader::load
  - canonicalize paths and build SourceGraph
  - lex each source unit
  - remove include directives and retain dependency edges
        |
        v
Parser::parse once per SourceGraph::compilationOrder unit
  - retain each unit's declaration range
  - assemble one dependency-ordered transitional Program
        |
        v
SemanticVisitor::check
        |
        v
HirLowerer::lower
        |
        v
MirLowerer::lower
        |
        v
FrontendResult
```

After a successful frontend, `lang::driver::compileToCpp` runs the legacy
`OptimizationPipeline::run(frontend.hir, level, target)` and the owned
`OptimizationPipeline::run(OptimizationRequest)`. The first produces the HIR
constant replacements still consumed by `CppEmitter`; the second verifies and
returns an unchanged MIR snapshot supplied through `BackendInput`. The CLI only
constructs the request and presents its structured result. Optimization
therefore executes after MIR construction. No MIR transformation controls
emission yet.

### Frontend gates

| Stage | Result flag | Stops when | Exception or special behavior |
| --- | --- | --- | --- |
| Source loading | `sourceValid` | any loader or include error | none |
| Parsing | `syntaxValid` | any unit has a parse error | `analyzeRecoveredProgram` lets the LSP continue; semantics and, when otherwise valid, HIR/MIR may still be built for the recovered program, but code generation remains disabled |
| Semantics | `semanticValid` | any semantic diagnostic | completion analysis returns after semantics even when valid |
| HIR | `hirValid` | concrete-instance or lowering diagnostics | instance diagnostics may attach the requesting call site |
| MIR | `mirValid` | structural validation fails | frontend emits internal `GTI-B0001` |

`FrontendResult::canGenerateCode()` requires all five flags. Never invoke a
backend merely because parsing or semantics succeeded.

Keep the same `TargetInfo` for semantics, optimization, and backend selection.
Target conditionals are parsed in every branch, but semantics and emission
select one active branch.

## Data Ownership And Authority

Use this table to decide where a fact must be produced and where consumers must
read it.

| Question | Authoritative representation | Important lifetime or identity rule |
| --- | --- | --- |
| What source structure was written? | `Program` and AST nodes in `ast.h` | AST children are owning pointers; syntax is not resolved meaning |
| Which source unit owns or can see a declaration? | `SourceGraph` plus semantic visibility registries | direct includes and prelude only; the combined `Program` must not imply global visibility |
| What type/category/access/ownership does an expression have? | `SemanticModel::ExpressionInfo` | keyed by AST address; valid while the owning `Program` lives |
| What traits and symbol belong to a binding? | `BindingInfo` in `SemanticModel` | do not reconstruct copyability, mutability, or physical movability from spelling |
| Which call/operator/constructor and dispatch mode were selected? | `ResolvedCallInfo`, `ResolvedOperatorInfo`, `ResolvedConstructionInfo` | semantic IDs, declarations, static/virtual dispatch, and dispatch owner are selected before backend entry |
| What concrete generic and class instances exist? | `HirProgram` | instances are discovered through a growing worklist; class instances retain bases, abstract/polymorphic state, virtual roots, and structured constructor initialization |
| What executable typed values exist across instances? | `HirValue` and `HirStatement` | HIR IDs start at 1 and are stable within one `HirProgram`; zero means no identity |
| What is the body-local CFG and value/place behavior? | `MirBody` | block, instruction, value, place, loan, and temporary IDs are body-local and start at 1 |
| Where are moves, loans, drops, and use-def relationships explicit? | MIR instructions, places, loans, scopes, and `valueUses` | do not rediscover them from AST call spelling |
| What optimization was proven? | `OptimizationResult` keyed by `HirValueId` | a source expression may map to multiple HIR values across instances |
| How is a fact represented in C++ today? | `CppEmitter` | representation only; never make it the semantic source of truth |
| What semantic facts may IDE queries expose? | `SemanticDatabase` inside `SemanticModel` | `SymbolId` values are snapshot-scoped and invalid across analyses |

`FrontendResult` owns `Program` before `SemanticModel`, HIR, and MIR, keeping
AST-address side tables valid for the snapshot lifetime. Do not persist raw AST
pointers, `SymbolId`, or per-program IDs across frontend results.

## Semantic Analysis Internals

`SemanticVisitor::check(const Program&)` is a staged analysis, not one ordinary
visitor walk. Preserve this order when adding a declaration category or
predeclared fact:

1. `registerNamespaces`
2. `registerNamespaceAliases`
3. `registerTypeAliases`
4. `registerEnums`
5. `registerClasses`
6. `resolveTypeAliases`
7. `resolveClassInheritance`
8. `registerFunctionGenericParameters`
9. `registerNamespaceSymbols`
10. `collectClassMembers`
11. `resolveInheritedMembers`
12. `validateStoredReferenceContracts`
13. `recordClassTypes`
14. `recordClassLifecycles`
15. enter the root scope and `analyze` declarations
16. `SemanticModel::finalizeCallableForwardings`
17. `SemanticModel::finalizeCallableArguments`
18. `SemanticModel::finalizeOccurrences`

The ordering deliberately makes namespaces, aliases, nominal types, callable
signatures, members, and lifecycle facts available before body analysis.
Changing it can silently introduce declaration-order dependence. A new
declaration kind may need registration, publication to source consumers,
tooling-symbol creation, and analysis; adding only a visitor method is often
insufficient.

### SemanticModel records

The model is a set of side tables over the checked AST. Important records are:

- `ExpressionInfo`: `SemanticType`, `ValueCategory`, `AccessMode`, and
  `SemanticTypeTraits`.
- `BindingInfo`: exact type, access, ownership/drop/copy/move traits, explicit
  movement, source symbol, static storage, and internal linkage.
- `FunctionInfo`, `ClassTypeInfo`, `EnumTypeInfo`, and `TypeAliasInfo`:
  normalized declaration identities and resolved signatures/types. Function
  records retain virtual/pure/override state and virtual roots; class records
  retain kind, resolved bases, abstract/polymorphic state, and the confined
  direct stored-reference field when present.
- `ClassLifecycleInfo`: compiler-owned construction, assignment, destruction,
  active-drop, structural trait decisions, and any source-declared defaulted or
  deleted copy/move construction policy.
- `ResolvedCallInfo`: exact callable, substituted parameter and return types,
  intrinsic identity, borrow origin/access, static/virtual `CallDispatch`, and
  the overload lookup or dispatch owner.
- `ResolvedOperatorInfo`: exact member operator and result access.
- Parser-owned range-for core expressions use the same resolved call and
  operator records as ordinary source expressions. Their generated tokens are
  source-mapped to the range colon and excluded from semantic occurrences.
- `ResolvedConstructionInfo`: exact ordinary constructor, generated default, or
  copy/move lifecycle identity plus any stored-borrow argument/access;
  constructor records preserve ordered
  base/field initializer targets rather than flattening them into expressions.
- switch constants, array extents, contextual conversions, lambda records, and
  source-facing semantic occurrences.

Record a fact once and make HIR, MIR, the backend, and language queries consume
it. Do not add parallel name matching or type inference downstream.

### State and lookup

`SemanticVisitor` owns nested value scopes, namespace registries, source-unit
visibility maps, class and enum registries, generic parameter scopes,
substitutions, receiver context, control-flow depth, and flow-sensitive value
state. When changing lookup or visibility, inspect both the global registry and
the `visible*` maps populated for source units.

Inheritance is resolved before member bodies. `resolveClassInheritance` builds
and validates base relationships; `resolveInheritedMembers` establishes
inherited overload sets, override roots, and the owner needed for later virtual
dispatch. Do not make call resolution or a backend reconstruct those relations
from base names.

Use `publishNamespace`, `publishTypeAlias`, `publishClass`, `publishEnum`, and
namespace-symbol publication helpers so direct consumers receive declarations
without leaking them transitively.

### Concrete generic reanalysis

Generic bodies are first checked symbolically. HIR requests concrete checking
through these public semantic entry points:

- `analyzeFunctionInstance`
- `analyzeConstructorInstance`
- `analyzeDestructorInstance`
- `analyzeClassFieldInitializers`

Each clones the base `SemanticVisitor`, calls `prepareInstanceAnalysis`, installs
class/function type and value substitutions, analyzes the relevant declaration,
and returns a `SemanticInstanceAnalysis` with its own model and diagnostics.
Extend these paths when a feature's validity depends on concrete ownership,
pack contents, class value arguments, or substituted fields.

Concept declarations are registered and resolved by source identity before
generic declarations are analyzed. `GenericConstraintKind` contains only
irreducible compiler facts, and `GenericConstraintSet` is the flattened result
of source composition. Only `@compiler_constraint` declarations in the trusted
prelude's `gti_internal` namespace may introduce one atom. Public standard
concepts, aliases, and implications belong in `stdlib/prelude.gti`.
`firstUnsatisfiedConstraint` owns concrete validation. Lifecycle atoms query
semantic type traits and public default-constructor availability; comparison
atoms inspect substituted class overload sets for exact public, read-only
`bool` member contracts. Keep these checks independent of emitted C++ concepts
or expression validity. `DefaultTypeParameterConstruction` records a validated
constrained `T()` through semantic call metadata, HIR, MIR, and the effect
table.

After symbolic body analysis, `SemanticModel::finalizeCallableForwardings`
computes non-escaping callable forwarding contracts to a fixed point. It marks
only direct generic-parameter edges whose selected target parameter already has
a proven callable contract. Concrete reanalysis then rejects any forwarded
lambda outside that graph, while HIR and MIR retain the selected forwarding
instance and target parameter index.

## HIR Internals

HIR is the backend-independent concrete instance graph plus executable typed
values. It is not merely a pretty AST.

`HirLowerer::lower` performs two top-level operations:

1. `seedDeclarations` registers enums, seeds non-generic declarations and
   module storage, and enqueues initial class/function instances.
2. `processPendingInstances` drains dynamically growing class, function,
   constructor, and destructor vectors until no discovered call, construction,
   field type, or nested instance adds more work.

Never replace this with one fixed pass over the initial vectors. Processing one
instance may enqueue another instance through a field, call, selected operator,
constructor, destructor, lambda, return type, or parameter type.

For a concrete generic instance, `processClass`, `processFunction`,
`processConstructor`, and `processDestructor` select the appropriate
`SemanticInstanceAnalysis`, append instance diagnostics, and lower against that
instance model rather than the base symbolic model.

`HirClassInstance` retains kind, `HirBaseInstance` entries, abstract and
polymorphic state, fields, and lifecycle information. `HirFunctionInstance`
retains virtual/pure/override state and virtual roots. Constructors use ordered
`HirConstructorInitializer` records with a base-or-field target, target type,
selected constructor, arguments, and generated-default state. Preserve this
structure so later phases never infer base construction from source spelling.

### HIR bodies and values

`HirBody` owns bindings, values, statements, and root statement IDs. Module,
field-initializer, static-field-initializer, function, constructor, destructor,
and lambda bodies share the same representation.

`HirStatementKind::RangeFor` retains source provenance around the implemented
lowered core block. That block contains ordinary resolved calls and one normal
`For` statement, so MIR currently consumes the existing loop CFG. A confined
stored-reference iterator carries one owner origin independently of this sugar;
do not infer precise per-iteration or last-use loan scopes that MIR does not yet
represent.

`HirStatementKind::StructuredBinding` retains one hidden source binding and
ordered field or array-element projections. MIR initializes and drops only the
hidden owner; each visible binding ID maps to a projected place rooted at that
owner. Do not lower the names as independently initialized locals or infer the
shape again from the emitted C++ structured binding.

`lowerExpression` currently uses explicit AST-class dispatch to choose a
`HirValueKind`, recursively lower operands, copy `ExpressionInfo`, and attach
resolved symbols, operations, literals, calls, constructors, operators,
intrinsics, borrow origins, and lambda targets. When adding an AST expression,
add an explicit HIR branch; otherwise it can silently retain the default
`Literal` kind.

`HirProgram::sourceValueIds` maps one AST expression to every concrete
`HirValueId` produced for it. Optimizations that project a result back onto
syntax must require agreement across all those instances; see
`OptimizationResult::replacement(const HirProgram&, const Expr&)`.

## MIR Internals

`MirLowerer` mirrors every HIR module/class/function/constructor/destructor/
lambda instance and invokes `MirBodyLowerer` for each body. Constructor
initializer values are emitted as prologue values, while ordered
`MirConstructorInitializer` records preserve their base/field target and exact
constructor. Class instances mirror kind, bases, abstract/polymorphic state,
and reverse field-drop order. Function instances mirror virtual roots and
virtual/pure/override flags. Call instructions retain `CallDispatch` and their
dispatch owner instead of asking a backend to infer virtual behavior.

`MirBodyLowerer::lower` follows this sequence:

1. append the entry block and establish the current block;
2. create the root lexical scope;
3. `seedParameterDrops`;
4. emit constructor or other prologue values and materialize any escaping
   stored-reference field loan;
5. `lowerStatements(source.roots)`;
6. synthesize a legal exit, implicit `main` zero return, void return, or
   unreachable terminator when the body did not terminate;
7. `rebuildMirReachability`;
8. `rebuildMirValueUses`;
9. validate HIR provenance and call `verifyMirBody` on the completed body.

### Structural model

- `MirPlace` separates roots (`Binding`, `Symbol`, `This`, `Temporary`, `Value`,
  `Loan`) from field, index, and dereference projections.
- `MirOperand` distinguishes values, constants, copies, moves, read/write
  borrows, and existing loans.
- `MirInstruction` owns computation, load, initialize, assign, modify, move,
  borrow, call, construct, drop, and end-borrow effects.
- `MirTerminator` owns CFG transfer: goto, branch, switch, return, unreachable,
  or exit.
- `MirValue` has one definition block/instruction. `valueUses` indexes every
  instruction operand/receiver, terminator, place root, and projected index.
- lexical scopes retain drops and loans. `Local`, `CallResult`, `Stored`, and
  `Return` loan kinds preserve whether a dependency is bound locally, carried
  by a value, stored into a field, or escapes through a checked return. Branch,
  break, continue, return, and normal exit must emit cleanup for exactly the
  scopes they leave.

The closed `MirOperation` enum is the typed scalar vocabulary for later passes
and backends. Add or map an operation there when a new HIR value carries
backend-independent scalar meaning. Extend `verifyMirBody` with every new
instruction, terminator, operation, operand, or identity invariant. Instruction,
operation, and intrinsic `Count` sentinels size deterministic name/effect tables;
adding an enum member without adding its classification fails the compiled
middle-end build.

`src/compiler/mir.cpp` owns reusable reachability and value-use repair plus
structural body/program verification. `MirPrinter` in
`src/compiler/mir_printer.cpp` serializes stored program/body order and every
identity-bearing MIR record without raw addresses. Optimizer rewrites must use
these utilities rather than maintaining derived facts ad hoc.

Verification also propagates active loan sets through reachable CFG edges. A
loan must have one producing `Borrow`, `Call`, or `Construct`; one semantic loan
identity may map to at most one MIR loan; every historical carrier binding is
recorded once; explicit loan and borrowed-binding uses require it to be active;
`EndBorrow` cannot repeat; normal returns and module exits cannot retain a
non-escaping loan; and predecessor states must agree at joins. This remains an
integrity check over existing MIR, not a last-use or alias analysis. Semantic
analysis chooses source borrow endpoints, HIR carries them on statements, and
MIR lowering emits the corresponding `EndBorrow` instructions.

`MirBodyLowerer::endFullExpressionLoans` ends newly created, non-escaping loans
that were not retained by a binding. Expression statements, initializers,
conditions, loop increments, and switch subjects invoke it after their result
has been materialized. Retained reference and borrowed-state carriers instead
use semantic loan identities. `MirBodyLowerer::endSemanticLoans` consumes the
HIR endpoint after the complete statement, then removes the active loan and
carrier mappings. Only one unshared carrier confined to a straight-line
statement region receives an early endpoint today; branches, loops, reborrows,
and shared carriers remain conservative.

MIR does not yet define object layout, ABI, general temporary lifetime, exact
runtime realization of primitive checks, or every active-drop transition. Do
not pretend its current completeness is sufficient for LLVM emission.

## Optimization And Backend Transition

`OptimizationPipeline` currently has two compatibility-era entry points:

- the existing HIR overload runs one constant-folding pass at `O1` and above,
  evaluates fixed-width integer operations through `checked_integer.h`, and
  stores only proven value replacements by `HirValueId`;
- the `OptimizationRequest` overload owns a MIR copy, verifies it, performs no
  passes, and returns an `OptimizedProgram` with an empty deterministic report.

`optimization/effects.h` is the centralized conservative effect contract for
all current MIR instructions, scalar operations, and intrinsics. Unresolved
per-instruction arithmetic failures, calls, allocation, storage operations,
moves, loans, and drops are not speculatable or removable. A constant operation
may fold only when the checked evaluator returns a value; a failure outcome
retains the original operation. Do not weaken those classifications from
emitted C++ behavior. Calls that invoke runtime or user code carry
`maySynchronize`; future concurrency intrinsics must receive an explicit effect
summary before optimization can inspect or move them.

For the proposed steady-state ownership model, pass manager, MIR mutation API,
effect classification, capability gates, backend migration, and verification
milestones, read
[the optimization architecture proposal](../../../../docs/optimization-architecture-proposal.md).
The proposal marks which parts of Milestone 1 are implemented. Controlled
editors, pass management, analysis caching/invalidation, and dump CLI options
are still proposals and must not be treated as available infrastructure.

Implement syntax-preserving improvements against HIR only when HIR contains the
needed facts. Implement new CFG, propagation, reachability, use-def, place, and
loan analyses against MIR rather than re-walking AST.

`BackendInput` contains checked AST, `SemanticModel`, HIR, MIR, optimizations,
and target. The current `CppBackend`, however, constructs `CppEmitter` with AST,
semantics, HIR, optimizations, and target; it does not consume `input.mir` yet.
This is a documented transition point:

- add language validity and selected identities to semantics;
- preserve concrete instances and typed values in HIR;
- preserve body-local operations and effects in MIR;
- add only C++ representation choices to `CppEmitter`;
- migrate existing emitter decisions toward HIR/MIR without dropping source
  provenance or changing GTI behavior.

## Worked Trace: Binary Expression

For `left + right`:

1. `Lexer` produces `TokenKind::PLUS`; logical word/symbol aliases are already
   normalized at this boundary.
2. `Parser` creates `Binary` (or `Logical` for short-circuit operators) with
   precedence encoded by its expression parser.
3. `SemanticVisitor::visitBinaryExpr` analyzes operands, selects built-in or
   member-operator behavior, rejects invalid domains, and records
   `ExpressionInfo` plus any `ResolvedOperatorInfo`.
4. `HirLowerer::lowerExpression` creates `HirValueKind::Binary`, stores the
   operation token, evaluation-order operand IDs, semantic info, and any exact
   operator target.
5. `MirBodyLowerer` maps the HIR token to a closed `MirOperation`, emits values
   and either a scalar instruction or selected call, and indexes all uses.
6. `OptimizationPipeline` may record a HIR replacement only for operations whose
   GTI edge behavior is defined and implemented by the pass.
7. The transitional `CppEmitter::visitBinaryExpr` consults semantic resolution
   and HIR-keyed optimization results, then emits either a direct selected
   method call, a checked helper, or a C++ representation of the built-in GTI
   operation.

## Worked Trace: Resolved Call

For `callee(arguments)`:

1. `Parser` creates `Call` with the callee, explicit type arguments, ordered
   value arguments, and closing-paren source token.
2. `SemanticVisitor::visitCallExpr` distinguishes ordinary functions, methods,
   callable operators, lambdas, constructors/conversions, and trusted
   intrinsics. Exact overload resolution records `ResolvedCallInfo`,
   `ResolvedOperatorInfo`, `ResolvedLambdaCallInfo`, or
   `ResolvedConstructionInfo`.
3. The semantic record owns return and parameter types, substituted type
   arguments, callable identity, intrinsic kind, borrow origin, static/virtual
   dispatch, and dispatch owner. The backend must not repeat selection from
   names or a receiver's apparent C++ type.
4. HIR lowers the callee and arguments in evaluation order, copies the semantic
   record, and enqueues the selected concrete function or constructor instance.
   Discovery can grow the pending-instance worklist.
5. MIR emits a `Call` or `Construct` instruction with exact HIR instance targets,
   receiver/operands, intrinsic identity, dispatch mode and owner, result value,
   place, and loan effects.
6. The C++ emitter reads the semantic identity and emits a mangled direct call
   or trusted helper. C++ overload resolution is not part of GTI semantics.

## Test Navigation

`tests/compiler_tests.cpp` is one executable with feature-group functions rather
than individually selectable test cases. Locate the nearest group before adding
coverage:

```sh
rg -n "^void test[A-Za-z0-9_]+\(\)" tests/compiler_tests.cpp
```

High-value compiler-internal groups include:

- `testFrontendBackendAndOptimizationPipeline`
- `testMirControlFlowAndOwnershipEffects`
- `testSourceUnitDependencyGraph`
- `testOwnershipSemanticFoundation`
- `testTypedHirGenericInstances`
- `testInheritanceAndInterfaces`
- `testCompletePipeline`
- `testParserRecovery`
- `testSemanticDiagnostics`
- `testLanguageQueries`

Feature tests such as constructors, operators, generics, arrays, lambdas,
expected values, aliases, conditionals, enums, and formatting live in matching
`test...` functions in the same file. Put the most focused assertion with its
feature group; add a separate group only when it creates a real subsystem
boundary.

CTest exposes the complete in-process executable as `compiler_pipeline`.
MIR verification, deterministic printing, identity optimization, and effect
classification have the focused `optimizer_foundation` target.
The small exact-version static-library link check is
`compiler_library_boundary`. The separately installed driver archive is checked
by `driver_library_boundary`, while request, target propagation, native command,
resource, and artifact behavior is covered by `driver_pipeline`.
Manifest discovery, schema validation, target/profile resolution, and path
containment are covered by `project_model`. Direct CLI-native compilation is
`cli_workflow`, and uncached project builds from root and nested directories
are `project_cli_workflow`; LSP protocol coverage is `lsp_protocol` when
`json-c` is available. See the change guide for focused and broad commands.

## Maintenance Contract

Update this reference whenever any of these change:

- `Frontend::analyze` phase order, options, result flags, or early-return gates;
- the registration sequence in `SemanticVisitor::check`;
- inheritance resolution, inherited-member ownership, or dispatch metadata;
- a primary `SemanticModel` record or identity lifetime;
- HIR instance discovery, class/base/constructor structure, generic reanalysis,
  ID rules, virtual roots, or expression dispatch;
- MIR body-lowering order, dispatch, structured construction, operations,
  cleanup, use indexing, or validation;
- `OptimizationContext`, `BackendInput`, or what `CppBackend` actually consumes;
- the declaration/compiled-source boundary or `gti_compiler` target shape;
- compiler test grouping or CTest target names.

When one of those symbols disappears, search for its replacement and update the
documentation in the same change. Do not preserve a stale name merely to keep
this reference text stable.
