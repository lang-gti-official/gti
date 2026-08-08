# GTI Compiler Internals

Use this reference before changing compiler implementation, semantic metadata,
HIR, MIR, optimization, or backend lowering. It documents the code paths and
authority boundaries that are expensive to reconstruct from the large
header-only implementation.

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
rg -n "FrontendResult|Frontend::|Frontend \{" include/gti/frontend.h src/cli/main.cpp
rg -n "bool check\(const Program|registerNamespaces|resolveClassInheritance|resolveInheritedMembers|recordClassLifecycles" \
  include/gti/semantic_analyzer.h
rg -n "class SemanticModel|struct (ExpressionInfo|BindingInfo|FunctionInfo)|Resolved.*Info|CallDispatch" \
  include/gti/semantic_analyzer.h
rg -n "class HirLowerer|seedDeclarations|processPendingInstances|lowerExpression" \
  include/gti/hir.h
rg -n "class MirLowerer|class MirBodyLowerer|indexValueUses|validate\(\)" \
  include/gti/mir.h
rg -n "OptimizationPipeline|BackendInput|CppBackend|class CppEmitter" \
  include/gti src/cli/main.cpp
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

After a successful frontend, the direct CLI runs
`OptimizationPipeline::run(frontend.hir, level, target)`, constructs a
`BackendInput` from the complete `FrontendResult`, and invokes `CppBackend`.
Optimization therefore executes after MIR construction in the current driver,
even though the implemented optimization pass consumes HIR rather than MIR.

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
12. `recordClassTypes`
13. `recordClassLifecycles`
14. enter the root scope and `analyze` declarations
15. `SemanticModel::finalizeOccurrences`

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
  retain kind, resolved bases, and abstract/polymorphic state.
- `ClassLifecycleInfo`: compiler-owned construction, assignment, destruction,
  active-drop, and structural trait decisions.
- `ResolvedCallInfo`: exact callable, substituted parameter and return types,
  intrinsic identity, borrow origin, static/virtual `CallDispatch`, and the
  overload lookup or dispatch owner.
- `ResolvedOperatorInfo`: exact member operator and result access.
- `ResolvedConstructionInfo`: exact constructor or generated default identity;
  constructor records preserve ordered base/field initializer targets rather
  than flattening them into expressions.
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
4. emit constructor or other prologue values;
5. `lowerStatements(source.roots)`;
6. synthesize a legal exit, implicit `main` zero return, void return, or
   unreachable terminator when the body did not terminate;
7. `markReachableBlocks`;
8. `indexValueUses`;
9. `validate` the completed body.

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
- lexical scopes retain drops and loans. Branch, break, continue, return, and
  normal exit must emit cleanup for exactly the scopes they leave.

The closed `MirOperation` enum is the typed scalar vocabulary for later passes
and backends. Add or map an operation there when a new HIR value carries
backend-independent scalar meaning. Extend `validate()` with every new
instruction, terminator, operation, operand, or identity invariant.

MIR does not yet define object layout, ABI, general temporary lifetime, exact
runtime realization of primitive checks, or every active-drop transition. Do
not pretend its current completeness is sufficient for LLVM emission.

## Optimization And Backend Transition

`OptimizationPipeline` currently runs one constant-folding pass over typed HIR
at `O1` and above. `O0` returns an empty result. The pass visits module, field,
function, constructor, destructor, and lambda HIR bodies and stores replacements
by `HirValueId`.

For the proposed steady-state ownership model, pass manager, MIR mutation API,
effect classification, capability gates, backend migration, and verification
milestones, read
[the optimization architecture proposal](../../../../docs/optimization-architecture-proposal.md).
This section records the implementation that exists today; the proposal must
not be mistaken for implemented behavior.

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
CLI-native compilation is `cli_workflow`; LSP protocol coverage is
`lsp_protocol` when `json-c` is available. See the change guide for focused and
broad commands.

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
- compiler test grouping or CTest target names.

When one of those symbols disappears, search for its replacement and update the
documentation in the same change. Do not preserve a stale name merely to keep
this reference text stable.
