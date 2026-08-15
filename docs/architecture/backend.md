# Backend And Native Handoff

Status: Current transitional C++ backend with `scalar-leaf-v1`,
`scalar-cfg-v1`, `scalar-direct-call-v1`, `class-default-cleanup-v1`, and
`owned-lifecycle-call-v1` plus the hosted `scalar-failure-callgraph-v1`
component emitted from verified MIR.

`include/gti/backend.h` defines a target-independent `Backend` interface.
`BackendInput` carries the checked `Program`, `SemanticModel`, typed HIR,
optimized MIR, HIR compatibility replacements, and selected target.
`BackendArtifact` is source or object content plus its extension.

M-FAIL-01 now supplies compiler-owned `FailureMetadata` through
`BackendInput::mir`: the immutable descriptor, artifact identity, and
detector-to-`FailureSiteId` mapping are built before MIR optimization. Building
it consumes `SourceGraph`, `SourceManager`, and source-loader logical-root
provenance; a backend must not reconstruct those inputs from absolute token
paths. MIR v20 retains the bounded full-expression-root
`Invoke`/cleanup/`PropagateFailure` family, caller-owned ordinary-call parameter
stages, and success-edge initialization for an eligible cleanup-owning call
result added in v18 and the source/identity-fold literal provenance added in
v19. Version 20 adds MIR-owned source/runtime/declaration definition provenance
and a verifier-bounded `mayRaiseDefinedFailure` summary. False means an
acyclic closed scalar/static-call graph was proved from MIR; bodyless
declarations, recursive cycles, and unsupported shapes remain conservatively
true. MIR v21 generalizes the summary to separate function, constructor, and
destructor vectors. The bounded `class-default-cleanup-v1` proof can mark its
exact source destructors and owning function failure-free. A separate bounded
owned-lifecycle effect closure can also prove exact source constructors,
destructors, and free functions over passive scalar-field classes. The
`owned-lifecycle-call-v1` selector consumes that closure together with exact
HIR-to-MIR constructor, destructor, call, move, and cleanup coherence.
Production C++ emission now materializes that verified metadata as an
immutable version-one runtime ABI descriptor. The bounded
`scalar-failure-callgraph-v1` component also executes its selected failure
edges and reaches the hosted runtime terminal primitive. Other compatibility
bodies still cannot execute those edges: a failure edge may not be mixed into
an AST/HIR-emitted caller before its complete closed component reaches
containment.

## C++ Backend

The C++ representation layer is the compiled `gti_cpp_backend` target.
`CppEmitter` is a narrow facade whose implementation is private to
`src/compiler/cpp_emitter.cpp`; `CppBackend` consumes only `BackendInput`.
The compiler frontend and LSP do not link this target. The driver links it
privately, and installed exact-version consumers may link `GTI::cpp_backend`.

`CppBackend` requires `BackendInput::sourceMir`, the canonical verified MIR
snapshot retained before optimization. It first checks that source snapshot
against the supplied semantic and HIR snapshot, then re-verifies
`BackendInput::mir` and requires exact source-to-optimized coherence. It next
builds and plans one sealed representation snapshot for the complete program
before constructing `CppEmitter` or producing output bytes.
The only admitted divergence is the current provenance-bearing identity fold;
operation or operand substitution, branch/CFG rewrites, and all other metadata
drift fail closed. The source pointer is a trusted internal
`Frontend`/`OptimizationPipeline` precondition rather than an unforgeable
public-library capability, but omission, a mixed frontend snapshot, or a stale
source snapshot is still rejected. `CppBackend` also independently verifies
the canonical failure metadata, so descriptor-byte or artifact-digest drift
fails before C++ emission. Six complete body families now use that path:

The backend target also contains the private `CppMirProgramPlan` preflight for
the active final authority cutover. It
accepts an exact owned copy of the verified optimized `MirProgram` plus
pointer-free representation rows, exact-compares the complete MIR structures,
and inventories every core `MirBodyAddress`, declaration/data row, and explicit
generated representation thunk before any output. A coherent plan has one
whole-program outcome: `Complete`, or `UnsupportedSurface` when any valid body,
data fact, or thunk lacks sealed generic-emitter coverage. The temporary named
body-family labels are inventory only and always contribute
`UnsupportedSurface`; they cannot be hand-authored proof of `Complete`.
Missing, duplicate, stale, wrongly classified, or dependency-incoherent facts
produce `Incoherent`; they never authorize partial MIR emission or fallback.
Before canonical sorting or moves, the planner exact-compares every copied MIR,
body, declaration-data, and thunk row against the builder's private full-copy
inventory seal. A caller that removes, injects, or stales rows cannot preserve
coherence by coordinating the remaining facts. Hand-authored snapshots have no
production route for establishing that seal.
Thunk dependencies are closed, acyclic, body-rooted, and ordered before their
users. `HostedEntry` is currently contracted only as ordinal zero for its exact
entry-kind function owner/source, and `ProgramInitialization` only as owner and
ordinal zero for exact `Module/0`. The latter exists exactly when the verified
merged `MirProgramInitializationPlan` contains an `Initializer` step, and
`Module/0` is its sole direct body root. Tagged `ImplicitZero` and `Constant`
storage stages remain `DataOnly` even though Module has places, blocks, and
abstract `Initialize` instructions. An executable legacy generic
`StaticFieldInitializers` body remains an independent unsupported body row and
cannot infer this merged thunk. The planner independently re-derives that
closed graph; every MIR entry has exactly one directly rooted hosted thunk,
which depends on program initialization if and only if the merged Module plan
requires it.
All other thunk kinds remain unsupported inventory. The result retains only
copied identity/representation rows, not the duplicate MIR, AST bodies, HIR
bodies, semantic model, or optimization result.

The private `CppMirRepresentationSnapshot` builder is now the sole production
producer of those rows. It exact-checks active `Program` declaration identity
against `SemanticModel`, HIR, and verified MIR. The frontend seal includes the
Program snapshot, full analyzed `TargetInfo`, exact active-statement preorder,
and ordered source-unit/dependency/prelude provenance, so even a passive-only
separately parsed Program, a different source graph, or a target selecting a
different passive conditional branch fails closed before inventory can be
trusted. Semantic and HIR seals must match each other and the external Program
plus backend target. The shared HIR-plan verifier must also accept the exact
program-initialization and hosted-entry plans, their module bindings/roots, and
semantic constant provenance. The frontend/MIR snapshot gate additionally
requires a valid complete MIR program and exact-compares initialization
unit/step order, storage identity and full place metadata, data provenance and
constant payloads, dynamic statement/initializer/full-expression provenance,
and program-constant substitutions. The builder then enumerates every core MIR body;
derives source-executable, ABI-declaration/runtime-binding, and data-only roles;
and copies enum, namespace/class `constexpr`, ABI/opaque/union declaration, and
otherwise-unused generic class-template facts. A second coarse declaration
inventory retains every active source surface emitted outside executable MIR:
namespace and namespace-alias wrappers, type aliases, classes, callables,
global/static/field storage, access sections, language-linkage blocks, and
empty declarations; any newly admitted scope-level statement receives a
conservative `OtherDeclaration` row until classified. Generic free functions,
member functions, constructors, and operators therefore remain visible even
when no concrete HIR/MIR instance exists. Each row has a stable active-Program
traversal ordinal plus its exact semantic declaration and class-owner identity
where those exist. It also derives the exact hosted-entry and
program-initialization thunk graph. No caller supplies a support bit, trusted
flag, body-family label, data row, or thunk claim.

`CppBackend` immediately plans that built snapshot. `Incoherent` throws before
emission. During this migration tranche, any `UnsupportedSurface` selects one
whole-program compatibility-emitter invocation; there is no planner-level
per-body fallback. A structurally `Complete` declaration/data-only empty
executable surface uses that same representation emitter until the generic MIR
emitter is installed. Every source-executable row and every copied declaration
data/generated adapter remains migration-unsupported, and temporary named
families cannot promote one. The planner validates and orders supplied rows;
the sealed builder, rather than MIR or a public caller, owns semantic-data
exhaustiveness. `Module/0` is `DataOnly` exactly when its verified merged plan
contains no `Initializer` step; other initializer bodies retain the canonical
empty one-block `Exit` distinction. The current per-family selectors
inside the compatibility emitter below remain production authority until the
generic emitter replaces that single whole-program route.

- `scalar-leaf-v1` admits non-entry, non-generic free GTI functions containing
  only fixed-width-integer parameters/results, parameter loads,
  literal/identity values, trivial full-expression boundaries, and one return.
  `void` is admitted only as a result for a no-op leaf.
- `scalar-cfg-v1` admits non-entry, non-generic free functions, and ordinary
  non-static, non-virtual, non-operator read-only members of one concrete
  non-generic class instantiation, over
  fixed-width integers, `bool`, and `char`, with `void` also permitted as a
  result; unprojected scalar binding and temporary places; the verified
  literal, identity, logical-not, bitwise,
  equality, and ordering computations in its selector; load, initialization,
  plain assignment, and trivial full-expression boundaries; and
  `Goto`/`Branch`/`Switch`/`Return`/`Unreachable` control flow, including loop
  backedges. It excludes calls, checked-failure arithmetic, references,
  pointers, loans, drops, cleanup, construction, and failure edges. A member
  is resolved per instance rather than per declaration: zero instances, a
  generic owner, several instantiations, or a mutable receiver keep the
  member wholly on compatibility emission, and emitted member definitions use
  the ordinary deferred qualified form. A read-only member may additionally
  read scalar fields through `this`: the bare receiver is a class-typed
  projection carrier that is never referenced, each single-`Field` projected
  place binds by reference to the live member spelling, and reads of another
  object's fields, projection chains, and every write remain outside the
  family.
- `scalar-direct-call-v1` adds exact ordered scalar `CallInput`/`Call` stages
  to the scalar-CFG substrate. Every reachable node must be an eligible
  source-defined free function in one closed acyclic static-call graph, every
  target must retain exact semantic/HIR/MIR identity, and every selected call
  carries `None` under MIR v20's proved defined-failure summary. Checked or
  unknown targets, recursion, bodyless/runtime declarations, members,
  internal or `constexpr` functions, virtual/callable dispatch, ownership,
  construction, drops, and cleanup remain wholly compatible. A false
  `mayRaiseDefinedFailure` fact is necessary but not sufficient for this
  narrower production family: generic MIR effect analysis may validly prove
  concrete `constexpr` or internal bodies false without making them eligible
  for MIR C++ emission.
- `class-default-cleanup-v1` admits a non-entry, non-generic, zero-parameter
  source free function with a non-void scalar result and one straight-line root
  scope. Each local is an exact empty, base-free, field-free, non-polymorphic
  concrete class created by semantic generated-default `{}` construction with
  no constructor target. Its public source destructor is restricted to exact
  scalar-literal assignments to mutable top-level scalar globals. The verified
  schedule is `Construct`, `Initialize`/`Reparent`, and one full-expression
  boundary per local; a return-global `Load` and boundary; reverse lexical
  `Drop` instructions; one normal cleanup boundary; and `Return`. Declared
  constructors, fields or bases, nested scopes, branches, calls, loans,
  failure edges, and failure-capable destructors remain compatible.
- `owned-lifecycle-call-v1` admits one atomic acyclic graph of non-entry,
  non-generic, source-defined free functions over exact scalar values,
  variables, expression statements, blocks, `if`, and `return`, plus exact
  owned values of concrete, base-free, non-polymorphic classes. Each
  admitted class has only passive scalar instance fields, one exact ordinary
  scalar-field constructor, and one exact failure-free scalar-CFG destructor;
  custom copy/move policy, declaration field initializers, generic instances,
  references, raw pointers, callable state, and checked lifecycle bodies stay
  outside the family. The graph proves exact constructor-initializer stages,
  `Construct`, `Move`, `CallInput`, `Call`, `TransferOut`, and reachable
  `Drop` schedules plus every failure-free reverse caller. Source-eligible MIR
  drift fails closed; checked or otherwise source-ineligible lifecycle classes
  and functions remain wholly compatible. Comma, logical/conditional
  expression sub-CFG, loops, and switches are not admitted by this bounded
  family.
- `scalar-failure-callgraph-v1` admits one unique no-argument `int32_t` hosted
  entry and its exact closed, acyclic graph of source-defined GTI free
  functions with `int32_t` parameters/results. The body domain reuses the
  proved owned-lifecycle scalar/class subset and adds checked integer add,
  subtract, multiply, divide, remainder, shifts, negation, and dynamic integer
  conversion. Every detector and propagating call retains its verified
  `Invoke`/`PropagateFailure` record and failure-cleanup edge. The component is
  selected atomically only when no compatibility function, constructor,
  destructor, lambda, module initializer, or field/static initializer calls
  into it or uses one of its selected lifecycle representations. Program-wide
  initialization is restricted to inert scalar literals and empty static-field
  initializer bodies for this first hosted slice. Native/C-linkage edges,
  virtual dispatch, recursion, failure-free normal-ABI helpers, checked
  constructor/destructor bodies, borrowed/raw/callable state, broader
  signatures, and dynamic initialization keep the whole component compatible.
  Representation closure follows value-owning semantic containment through
  fixed arrays, unique/shared owners, private storage, expected/unexpected
  payloads, and lambda captures, and separately inspects every instantiated
  class field and lifecycle body. A compatibility body cannot hide selected
  storage behind a generic owner. A raw pointer, reference, function signature,
  or noncapturing type context alone is nonowning and does not demote an
  otherwise closed component.

Every body outside those exact families remains wholly on the compatibility
path, which traverses the checked AST and consults semantic facts, HIR,
compatibility replacements, and the target. The generated C++ is a
representation artifact, not GTI semantics.

GTI currently has no downstream compatibility obligation to accidental
compatibility-emitter behavior or to the textual shape of generated C++. Once
a complete body family emits from verified MIR and passes its production exit
gate, the production selector must choose that MIR family and fail closed on
incoherence rather than falling back to AST/HIR execution. Reusable
compatibility-emitter code may still serve body families that have not migrated
and the explicit public direct-`CppEmitter` compatibility API; its presence
does not authorize fallback for a selected production family. This policy does
not waive an explicitly adopted source-language, runtime ABI, or
native-interoperability contract.

`CppEmitter` requires references to the matching `SemanticModel` and
`HirProgram` at construction. There is no fact-free emission mode: callers
must run the same `Program` through semantics and HIR before requesting C++.
Those models are borrowed lvalues; temporary models are rejected so the
emitter cannot retain dangling authority references.
Individual lookup results remain optional where the semantic or HIR model says
that no such fact applies, but their owning models cannot be omitted to make
the emitter reconstruct meaning from source spellings.

The public direct `CppEmitter` constructors remain an explicit
compatibility-only API for exact-version library clients. Production
`CppBackend` alone can supply verified MIR through its private construction
path. That production path includes `gti/runtime_failure.h` and emits one
internal-linkage, constant-initialized `gti_failure_artifact_descriptor_v1`.
Its backing tables preserve the exact 32-byte artifact identity, canonical
descriptor bytes, one-based site order, counted logical-source bytes,
unsigned-64 line and byte spans, and each site's canonical allowed outcomes.
An empty artifact uses a null site pointer and a zero site count while still
retaining its nonempty canonical descriptor and nonzero identity. The public
compatibility-only construction path has no MIR metadata authority and emits
neither this header nor a descriptor.

Before selecting a MIR body, the emitter checks its snapshot-local HIR
instance, place-domain identity, source values, parameter bindings,
full-expression records, construction and drop obligations, cleanup schedule,
definition provenance, failure-effect summary, and declaration/signature
coherence. Generic MIR verification deliberately permits
a conservative true summary. If an otherwise eligible
`scalar-direct-call-v1` graph carries that conservative value, production
emission rejects the noncanonical drift rather than silently returning it to
compatibility. Header-, body-, dispatch-, and cycle-ineligible graphs remain
wholly compatible. `CppBackend` also rejects snapshot/header or definition-kind
incoherence before selection; after a HIR candidate plus a proved-false summary
selects the MIR family, graph, identity, provenance, or instruction drift fails
closed rather than falling back. Direct literals
carry verified MIR `Source` provenance; an optimized literal is selectable only
with verified MIR `IdentityFold` provenance retaining its exact dominating
source value. The HIR compatibility replacement table does not authorize this
path. A selected body has no per-expression fallback: unsupported instructions
make the whole body ineligible, and malformed MIR is rejected before selection
rather than silently reinterpreted.

The `class-default-cleanup-v1`, `owned-lifecycle-call-v1`, and
`scalar-failure-callgraph-v1` representations use a compiler-private
`mir_lifetime_slot<T>` with raw aligned storage, explicit
`std::construct_at`/`std::destroy_at`, and an aborting guard if native scope exit
finds a slot still engaged. The owned family also stages moved parameters and
prepared call inputs in slots: `Construct` creates an object, reparenting or
transfer explicitly disengages its source, and every verified `Drop`
disengages the slot before invoking the MIR-selected destructor. Generated
native destructors therefore cannot observe a second active cleanup, and
native RAII cannot silently repair an omitted or reordered MIR `Drop`.

Each selected failure-capable body has a private
`bool(..., result-out, failure-record)` ABI. A local checked operation writes
the exact immutable artifact/site/code/detail record only on its failure edge;
a call forwards the same record pointer unchanged. MIR `Return` publishes the
result immediately before `true`, while every failure cleanup runs before
`false` and never publishes the result. The one generated native `main`
contains the component, calls `gti_rt_failure_terminate_v1` exactly once after
GTI cleanup when the record is returned, and firewalls an escaping native
exception with immediate status-70 exit without manufacturing a GTI record.
No hidden body calls the terminal primitive.

Two residual abort spellings in a selected artifact are compiler-integrity
guards, not language failure routes: `mir_lifetime_slot` rejects a wrong-state
access or a native scope exit with an escaped active slot, and the generated
CFG switch default rejects a block-state value that is not any verifier-
approved MIR block ID. Exact selection and verified lifetime/CFG coherence make
both unreachable for a valid program. They neither create a defined-failure
record nor participate in the native-exception firewall. All legacy
message-plus-abort checked helpers remain outside the closed hosted component.

The emitter is responsible for choices such as:

- mapping the root GTI namespace `std` to the compiler-reserved C++ namespace
  `__gti_std`, while leaving an ordinary user namespace named `gti_std`
  distinct;
- placing ordinary GTI-source declarations under the compiler-owned
  `::__gti_program` namespace and undefining source identifiers that host
  headers define as macros. Before emitting its own helper preamble, the
  backend separately maintains a fixed set of macro-expandable tokens used by
  its helper preamble and undefines that set both before and after generated
  includes; numeric-limit `min`/`max` calls also use the macro-resistant
  parenthesized C++ spelling. This is a maintained representation boundary,
  not a promise to sandbox arbitrary native macros. A separate semantic-symbol
  cleanup prevents valid root names such as `FILE`, `size_t`, `NULL`, or `EOF`
  from colliding with the C++ implementation;
- dependency-ordering enum and class definitions before the emitted class
  bodies that require complete types, while leaving global definitions and
  their initialization order in source order. Class member bodies are emitted
  out of class after all type definitions so mutually referring method bodies
  do not inherit C++ source-order restrictions. Namespace globals receive
  declaration-only spellings where C++ permits them. A namespace `constexpr`
  binding is forward-declared as `extern const` before its later `constexpr`
  definition, while internal-linkage globals use compiler-private holders.
  Frontend constants may replace value-category expressions, but never
  place-category expressions whose storage identity is observable through an
  address or reference;
- emitting already-selected mangled calls, C-linkage symbols, dispatch, and
  lifecycle operations;
- representing `HirValueKind::PackFold`/`MirOperation::PackFold` as one C++
  comma fold over the exact generic function identity selected by semantics.
  The emitted pattern names that fixed target directly and substitutes only
  the source pack element; it does not emit an overload set, use ADL, or ask
  C++ template deduction to choose a GTI declaration. Native comma-fold order
  realizes the retained left-to-right element sequence, including the empty
  no-op case;
- representing a validated lexical closure as a C++ lambda while preserving
  the frontend's ordered bare-copy or explicit
  `[target = std::move(source)]` capture spelling. C++ closure traits are not
  authority: semantic capture records and HIR/MIR move/drop facts decide source
  validity and lifecycle before emission;
- representing fixed arrays, unique ownership, storage, classes, and virtual
  dispatch in C++. Every array initializer, including a contextual call
  argument, is emitted as an explicit `std::array<Element, N>{...}` value so
  native initializer-list overload resolution cannot select a different
  target. Inferred source `uint64_t` extents use the target-equivalent
  `std::size_t` non-type template parameter required by `std::array` deduction;
  GTI's supported targets define both as 64-bit unsigned domains;
- realizing checked arithmetic, conversion, indexing, pointer, and runtime
  operations;
- isolating the hosted native `argc`/`char**` boundary in an adapter for the
  semantic owned-argument entry kind, copying each argument into the exact
  GTI string/vector types and invoking the already-resolved append callable;
- emitting every GTI `float` or `double` literal and proven replacement from
  its exact binary32 or binary64 bits with `std::bit_cast`, and rejecting a
  host without matching IEEE-754 `float` and `double` representations;
- realizing the selected fixed-width wrapping operations through unsigned
  modulo arithmetic and the selected saturating operations through guarded
  bounds checks, and constructing checked-result expected values through the
  same guarded arithmetic, without executing signed native overflow;
- emitting a source layout query as its retained unsigned-64 frontend constant,
  never as native C++ `sizeof` or `alignof`;
- emitting a `[[c_abi]]` record as a passive standard-layout C++ struct and
  asserting its semantic size, alignment, and every field offset with native
  `static_assert`s rather than accepting native layout as language authority;
- emitting the same native-record definition through `NativeHeaderBackend` as
  a dual C17/C++20-or-C++23 header. The C++ branch preserves exact source
  qualification through default public aliases while defining passive records
  and C-linkage declarations in `::__gti_program`; consumers may suppress the
  optional aliases. The C branch uses deterministic flattened names where
  namespaces cannot be represented. Both consume semantic field types/layouts
  and never reconstruct ABI facts from generated C++;
- preserving `[[c_opaque]]` types as declarations only: generated GTI C++ and
  the bridge header's C++ branch emit the exact namespaced forward declaration,
  while the C branch emits a deterministic incomplete `typedef struct`. The
  native implementation may complete that type privately in C or C++, but the
  backend never asks for its layout or emits a GTI definition;
- emitting a valid passive GTI `union` as a native C++ union and asserting the
  frontend-selected size, alignment, and trivial-copy contract. The backend
  does not infer an active member and a GTI union does not acquire C ABI status
  merely because its storage representation is native;
- emitting a payload enum as a closed generated wrapper over `std::variant`.
  Semantic variant identity, exact payload construction, exhaustiveness, and
  pattern bindings are already fixed before emission; `std::variant` is a
  replaceable backend representation rather than a source-level dependency;
- selecting C++20 versus C++23 expected support. C++20 references the vendored
  compatibility namespace as absolute `::nonstd`; a source `nonstd` remains
  isolated inside `::__gti_program`.

GTI constant evaluation remains authoritative for checked-result constants.
C++23 may emit their representation as native `constexpr std::expected`.
Because the vendored C++20 expected representation is not a literal type, the
C++20 backend emits an immutable `const` object after the frontend has already
resolved every constant observer; native C++ constant evaluation is not used
as a substitute proof.

It must not perform GTI lookup, overload resolution, constraint checking,
ownership validation, or infer an intrinsic from spelling.

For `[[c_abi]]` only, `CppEmitter` uses canonical resolved field spellings and
does not emit GTI lifecycle policy members into the record definition. This
keeps its class definition token-equivalent to the generated header's C++
definition across translation units. Semantics still decides which GTI
construction operations are available. Initializers are prohibited on the ABI
record itself; safe wrappers and native factory functions own construction
policy.

Restricted non-virtual methods other than native comparison implementations
retain selected function-identity names. Public iterator-protocol declarations
additionally emit backend-private hidden-friend adapters for dereference,
prefix increment, and sentinel inequality. A symbolic generic body calls the
adapter through unqualified argument-dependent lookup. This makes inherited
adapters available without C++ member hiding or public `using` declarations and
lets each adapter forward to its declaration's already-selected implementation
identity.

The adapters use stable representation names such as
`__gti_operator_dereference` and `__gti_operator_pre_increment`; these are not
source names or a GTI ABI. They are emitted only for the public, exact shapes
admitted by the bounded `input_iterator` and `sentinel_for` contracts. Private
operators, constrained operators, invalid protocol shapes, and inactive
conditional declarations do not expose adapters. Abstract/interface
declarations emit the same adapter, whose forwarding call retains virtual
dispatch.

Sentinel inequality adapters carry an additional backend-private
`exact_type<S>` tag, and a symbolic call derives that tag from the exact
sentinel argument type. The tag prevents native conversions from making a
different base/derived sentinel overload viable while argument-dependent
lookup still discovers adapters inherited from iterator base classes. The tag
is GTI-owned rather than a standard-library type so its associated namespace
does not expand argument-dependent lookup into `std`.

Read-only symbolic calls pass the receiver through a generated const-view
helper, so a mutable source binding cannot cause native overload resolution to
prefer a mutable operator after GTI selected a read-only operation. Concrete
restricted-method calls continue to name their resolved declaration directly.
Deferred calls through confined generic callable parameters use the same rule:
the emitter carries the semantic parameter types plus the required read,
mutable, or once capability in a backend-private `exact_call` tag. Return types
are not part of dispatch because GTI does not overload on return type. Hidden
friend adapters carry their own exact receiver capability. The helper probes
only the order permitted by semantics (`Once -> Mutable -> Read`,
`Mutable -> Read`, or `Read`), while a direct concrete moved call carries its
already-selected capability. Parameter tags retain whether the selected target
takes a value, read reference, or mutable reference. Call-site tags carry the
set of bindings permitted by the argument's checked value category and access;
their constrained conversion admits exactly the same binding forms as semantic
analysis. The underlying value type remains exact. When a generic expression
type is deferred in the AST, the tag uses an unevaluated dependent `decltype`
of that argument so C++ template instantiation materializes the concrete value
type selected in HIR. Thus an `int32_t` call cannot reach a same-arity `bool`
adapter through a native conversion, and native overload resolution does not
repeat GTI semantic analysis. Raw-pointer tag compatibility is limited to the
same two conversions semantics admits for calls: `null` to a raw pointer and
mutable-to-read-only pointee access. Operators excluded by compiler-private
visibility or the bounded trailing-requirements model do not receive adapters.

A cleanup-free direct `std::move(value)(arguments)` whose selected exact target
is reusable mutable invocation still consumes the source in GTI, but the
bridge stabilizes that receiver as an lvalue for the selected C++ `&` member.
A selected consuming target stays an rvalue and reaches the generated `&&`
member. The bridge deliberately does not materialize a by-value helper local:
that would clean up before sibling operands rather than at GTI's enclosing
full-expression boundary. Semantics therefore rejects consuming invocation of
a receiver that structurally requires active cleanup until the backend has an
owned full-expression receiver representation.

Exact owned-callable generic return and field transport needs no erased native
wrapper. Once semantics, HIR, and MIR have proved the exact closure identity,
caller move, result/field destination, constructor move, and cleanup owner, the
transitional backend lowers the existing generic function/class and
`std::move` expression through C++ templates and `auto`. Native closure traits
or template behavior never establish that ownership contract.

Native comparison methods are an existing transitional exception: they retain
C++ `operator...` spellings, although the emitter pins their semantically
selected dispatch owner and receiver mutability to prevent derived member
hiding from changing the target. This comparison representation should not be
treated as the semantic model or extended to new generic protocols.

The compatibility C++ emitter still cannot encode every concrete HIR target into a
separate instantiation of one emitted template. The hidden-friend boundary is a
narrow bridge for the implemented structural contracts; it is not permission
for the backend or native C++ to define new GTI overload semantics.

Outside the MIR-emitted `scalar-leaf-v1`, `scalar-cfg-v1`,
`scalar-direct-call-v1`, `class-default-cleanup-v1`,
`owned-lifecycle-call-v1`, and `scalar-failure-callgraph-v1` families, the
emitter does not yet implement the accepted
[evaluation/full-expression contract](../language/execution.md#42-evaluation-order).
It emits ordinary calls, constructors, checked helpers, and several operator
operands inline in native C++ argument lists; emits target places directly;
relies on native temporary rules; and leaves module/static initialization to
native static initialization. The owned-entry adapter currently constructs its
argument vector before performing the count conversion, contrary to the
accepted startup sequence. Although some native constructs such as `&&`, `||`,
and `?:` happen to preserve the selected source control flow, that is not a
complete or verified GTI schedule. Emitter-local IIFEs or statement hoisting
must not become a second lifetime authority.

M-LIFE-01 now provides verified temporary obligations, and landed M-EXEC-01
slices provide ordered MIR for selected families. Completion of the whole
M-EXEC row is not a prerequisite for backend migration: any complete selected
family may move immediately through a matching M-BACK closed-body cutover.
The compatibility emitter remains conservative, and semantics must not relax
the both-argument transient-loan restriction for an operation family until its
production path consumes that MIR schedule.

The bounded pack-fold family is narrower than that general gap. Its semantic
contract admits only named fixed places and read-only pack-element access, HIR
and MIR retain the exact ordered element calls, and the compatibility emitter's
single selected C++ comma fold has the same left-to-right sequence. This does
not make inline native argument lists conforming for any other call family or
make pack-fold bodies MIR-emitted.

Outside `scalar-failure-callgraph-v1`, the compatibility emitter still chooses
failure messages in seven generated helper families and terminates with
`std::abort()`. Wrong-state `expected` observers are emitted as native C++
calls. These paths predate the
[defined-failure contract](../language/execution.md#410-defined-runtime-failure)
and are known nonconformities: they erase the stable GTI category/source
record, skip failure cleanup, expose signal-derived status, and import native
exception/assertion behavior. Once the applicable closed call-graph family
migrates through M-EXEC-01/M-FAIL-01/M-BACK-02, a conforming backend must lower
MIR-owned failure successors and pass the completed record to the selected
program, embedding, or task boundary. The current compatibility emitter remains
explicitly nonconforming until that migration; it shall not invent a second
partial failure authority. The descriptor and hosted terminal path are active
only for the exact selected failure component; descriptor presence alone does
not make a compatibility helper conforming. The backend shall not
synthesize categories or use C++ exception unwinding to emulate the contract.

A failure-capable MIR-emission slice must be closed across every GTI caller,
callee, constructor edge, and virtual override between a local failure origin
and its containment boundary. A legacy AST/HIR caller cannot erase a MIR
callee's record or skip cleanup, and a migrated caller cannot treat a legacy
aborting callee as propagation. M-BACK-01 therefore started with and now
implements the genuinely failure-free `scalar-leaf-v1` family without waiting
on unrelated M-EXEC-01 families. M-BACK-02 has also completed the
`scalar-cfg-v1` call-free, failure-free, cleanup-free CFG expansion and the
`scalar-direct-call-v1` acyclic failure-free static-call expansion. It now
also completes `class-default-cleanup-v1`, the first bounded construction and
normal-cleanup production slice, `owned-lifecycle-call-v1`, and the first
closed hosted failure-capable `scalar-failure-callgraph-v1` component. The
active campaign now proceeds to final whole-program authority cutover plus
removal of compatibility helpers; broader execution families remain separate
work rather than implicit extensions of this bounded component.

Both accepted entry signatures lower their source function to a private name
inside `::__gti_program`. A separate global native C++ `main` performs
the checked count conversion and owned startup copy, then moves the resulting
vector into the source function. `ProgramEntryKind` and the append
declaration are semantic facts; HIR concretizes the append target and MIR
retains that concrete identity for verification. The native adapter is a
representation choice and never exposes `char**` to GTI source.

Today the emitter itself also synthesizes the negative-count check, checked
GTI count conversion, and argument copy. The semantic and HIR hosted-entry plans
now own the exact `main` anchor, vector/string constructor and append targets,
and the two adapter-local origins for negative-count validation and checked
count conversion. Allocation failures remain on those exact source-defined
constructor/append callee records; the adapter has no separate
`allocation_failure/hosted_arguments` detector. Under M-FAIL-01/M-EXEC-01, a
compiler-generated hosted-startup HIR/MIR body must stage those calls and
preserve their records. The backend then only realizes that verified schedule;
it does not select targets, failure categories, or source sites.

MIR now owns and verifies one source-graph-derived merged Module schedule for
non-generic namespace and static storage. The current compatibility emitter may
still realize that storage through native C++ static initialization rather than
executing the verified plan. A conforming hosted backend instead enters the GTI
containment boundary first and emits the exact plan inside the generated
adapter. Generic static storage, HostedStartup containment, and that production
cutover remain separate work; native pre-`main` failure is not an alternate
containment policy.

## Driver Handoff

`lang::driver::compileToCpp` in `src/driver/compilation.cpp` runs the frontend,
HIR compatibility optimization, owned-MIR pipeline, and backend. At `-O1+` the
MIR pipeline may produce its verified primitive literal-identity
rewrite. Eligible `scalar-leaf-v1`, `scalar-cfg-v1`,
`scalar-direct-call-v1`, `owned-lifecycle-call-v1`, and
`scalar-failure-callgraph-v1` bodies consume that optimized MIR result, and
`class-default-cleanup-v1` consumes the same verified MIR snapshot without
using the current identity-fold transform. Other bodies still emit from the
HIR compatibility path.
The driver refuses backend generation unless every frontend validity gate and
MIR verification succeeds, and `CppBackend` independently rejects an invalid
MIR, a missing/stale canonical source MIR, unauthorized optimized drift, or an
incoherent failure-metadata snapshot.

The reusable `compileWithBackend` boundary also contains exceptions thrown by
`Backend::generate`. It translates both standard and non-standard exceptions
into a `CompilationStatus::BackendFailure` with an entry-anchored `GTI-B0001`
diagnostic, retains the analyzed source snapshot and prior diagnostics, and
publishes no artifact. Direct C++ emission, native-header emission, and project
builds all use that boundary; individual CLI modes do not install divergent
backend exception policy.

The resulting C++ artifact is handed to `gti_driver`, which owns temporary
files, native tool discovery, exact argument vectors, process execution, and
atomic artifact publication. Those concerns do not belong in `gti_compiler` or
the backend semantic contract.

The native driver makes the portable contract authoritative for its supported
GNU-style toolchain interface by appending `-fno-fast-math` and
`-ffp-contract=off` after forwarded compiler arguments. It then defines
`__gti_strict_ieee754=1`, an opt-in marker required by a generated C++
`static_assert`. Library consumers compiling a `BackendArtifact` themselves
must impose the same no-reassociation/no-contraction policy and define that
marker; otherwise the artifact does not compile. The marker records the
consumer's assertion rather than trying to infer arbitrary native compiler
flags. These controls align runtime operations with the frontend's one-rounding
step `llvm::APFloat` evaluation; they do not make C++ the source of the rule.

## Future Backends

A new backend should implement `Backend` rather than adding target branches to
frontend layers. LLVM emission remains premature until MIR owns the missing
temporary, lifecycle, layout, ABI, and runtime rules described in
[`mir.md`](mir.md) and [`docs/plans/optimization.md`](../plans/optimization.md).
