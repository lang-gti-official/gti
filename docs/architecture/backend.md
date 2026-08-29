# Backend And Native Handoff

Status: current architecture.

GTI ships a native C++ source backend. All production backends consume one
compiler-owned, backend-neutral `LoweredProgram`; they do not receive the AST,
`SemanticModel`, `HirProgram`, source MIR, optimizer side tables, or generated
C++. The backend chooses target representation, but it cannot recover missing
language meaning from an upstream representation or from host C++ behavior.

The phase direction is:

```text
source -> AST -> semantics -> HIR -> source MIR -> verified optimization
       -> LoweredProgram -> backend -> BackendArtifact -> driver
```

AST owns written syntax, semantics owns resolved language meaning, HIR owns
concrete instances and typed values, MIR owns executable control flow and
effects, `LoweredProgram` owns the complete backend-neutral program contract,
and each backend owns target representation.

## Backend Contract

`include/gti/backend.h` defines one executable-backend operation:

```cpp
BackendArtifact generate(const LoweredProgram &program);
```

Backend policy is explicit state of the selected backend. For example,
`CppBackend` receives the requested C++20 or C++23 policy in its constructor.
A backend returns an in-memory `BackendArtifact`; file publication, native
compiler invocation, process handling, and final artifact replacement belong
to the reusable driver.

`BackendInput` no longer exists. `CppBackend`, `MirBackend`, and
`NativeHeaderBackend` all verify and consume only `LoweredProgram`.

## Lowered Program Construction

`LoweredProgramBuilder` is the only frontend-aware construction boundary. The
reusable driver invokes it exactly once after semantic analysis, HIR lowering,
MIR lowering, MIR verification, and optimization. Construction checks:

- the selected target and analyzed-program seal;
- semantic/HIR program-initialization and hosted-entry plans;
- source and optimized MIR validity, failure metadata, and exact optimization
  coherence;
- complete MIR body identity and role coverage;
- active declaration order, parent structure, and resolved payloads;
- exact symbol and concrete class/function/constructor/destructor/lambda
  instance inventories; and
- the complete generated-item graph, roots, dependencies, and deterministic
  order.

Only a successful build publishes a `LoweredProgram`. Source MIR and optimizer
records remain construction evidence and are not retained in the consumer
contract. This makes a valid construction seal sufficient evidence for a
backend; no backend reconstructs frontend coherence.

Construction and consumption are intentionally separated. The consumer header
`include/gti/lowered_program.h` includes no AST, semantic, HIR, optimizer, or
builder declaration. `include/gti/lowered_program_builder.h` forward-declares
the upstream representations needed only to construct the value.

The implementation follows the same ownership boundary. `lowered_program.cpp`
owns immutable lookup and enumeration, `lowered_program_builder.cpp` is the
only frontend-aware conversion unit, `lowered_program_closure.cpp` derives the
deterministic body/generated-item closure shared by construction and checking,
and `lowered_program_verifier.cpp` consumes only the completed lowered value,
MIR verification, and failure metadata. A repository boundary test rejects any
AST, HIR, semantic-model, or builder dependency in the model, closure, or
verifier units.

`LoweredProgram` is value-owned and pointer-free. Its fields are private, its
public views are const, and its identities are stable within the value. It
contains:

- target-resolved backend-neutral target and layout facts;
- verified optimized MIR, scalar block parameters/edge arguments, per-instance
  effects, and canonical failure metadata;
- every body identity, role, definition kind, source span, and generated-item
  requirement;
- a flat active declaration tree preserving source order, namespace nesting,
  semantic identity, and resolved declaration payloads;
- resolved symbols, signatures, linkage, generic requirements, storage,
  lifecycle, inheritance, enum, union, C ABI, and borrow facts;
- concrete class, function, constructor, destructor, and lambda instances with
  exact type/value substitutions and source provenance; and
- exhaustive generated-item identities, payloads, roots, and dependencies.

`verifyLoweredProgram` rejects missing, duplicate, stale, reordered, unrooted,
or cyclic records and an invalid construction seal. `LoweredProgramPrinter`
provides deterministic full text for comparison and independent tooling.

## Generated Items

Every generated family has a target-independent contract:

| Family | Lowered authority |
| --- | --- |
| Program initialization | exact `Module/0` source, executable-initialization condition, and hosted-entry dependency |
| Hosted entry | exact entry body, startup contract, initialization dependency, and body root |
| Structural operator adapter | exact function/operator payload and declaration root |
| Callable adapter | exact function/capability payload and declaration root |
| Lifecycle cleanup | exact class/destructor instance, ordinary or concrete-specialization form, failure policy, and destructor root |
| Native interop adapter | exact callback target, signature, source-body roots, order, and containment policy |
| Thread task adapter | exact task and `void` or owned result type, spawn body/instruction, once-invocation contract, result take-or-abandon ownership, synchronization identity, and worker containment firewalls |
| Concrete-instance adapter | exact generic function/constructor declaration, concrete owner/body, failure effect, and declaration root |

Ordinary C/runtime declarations and boundary shells are resolved linkage and
ABI declaration/body rows, not a second generated-wrapper family. Native
callbacks are the generated native interoperability case.

An executable `Module/0` initializer does not imply that a hosted entry exists.
When one is present, its verified startup body calls `Module/0` exactly once
and the hosted-entry item depends on the program-initialization item. A
translation unit without `main` instead has no hosted-startup body call; the
C++ representation invokes the same verified module body through one private
static initialization guard. Data-only namespace storage needs neither route.
The program-plan verifier checks the hosted call only when a hosted startup is
present and never invents an entry to satisfy the C++ representation.

When the selected executable `Module/0` route must construct cleanup-free
owning program storage at runtime, its copied C++ storage row records a
lifetime-slot representation. This currently covers class values and concrete
fixed arrays — including nested multidimensional chains — whose leaf element
is a class and whose MIR aggregate schedule proves ordered element
construction, rollback, transfer, and publication. A nested element is itself
an owning aggregate: it constructs its own value-rooted slot and the
enclosing aggregate moves from it in operand order. Global and static
declarations, the module construction carrier, and later symbol-rooted reads
or writes all consume that same row. An access first exposes the engaged value
and then follows the exact index or field projection rows; missing slot or
projection facts fail body admission. This is only a target representation of
the existing MIR storage and initialization order. It does not admit
cleanup-owning program storage or make C++ static lifetime rules an execution
authority.

For a local fixed array whose exact leaf class is polymorphic, aggregate
transfer is admitted only when the concrete class is movable by its generated
move, every declared field move is failure-free, every base is an exact
structural base, and no base carries state. Interface bases therefore preserve
the same complete-element proof as a base-less class; state-bearing bases,
custom moves, abstract classes, and unions remain outside this vocabulary. A
source destructor uses the verified fixed-array drop schedule only under the
same base/field restrictions, preserving per-element construction rollback and
reverse destruction rather than relying on native array lifetime.

Backends choose helper names, monomorphization mechanics, ABI spelling, and
target syntax. They cannot rediscover whether an item exists from source
declarations or HIR use sites.

## C++ Backend

`CppBackend::generate` verifies the lowered value and constructs
`CppEmitter(program, standard)`. The emitter then:

1. adapts the lowered tables into a C++-private representation snapshot;
2. seals a complete body, declaration, data, and generated-item inventory;
3. admits every executable body into a pointer-free
   `CppMirBodyEmissionPlan`;
4. resolves all remaining declarations, support features, adapters, and
   target spellings into an ordered pointer-free `CppProgramEmissionPlan`;
   and
5. passes that completed plan to formatting-only writers.

The private snapshot and plan are target-specific derived forms, not compiler
meaning and not backend API. Their builders accept only `LoweredProgram`.
Frontend-backed snapshot overloads and comparison oracles were removed with
the final cutover.

`CppEmitter::Impl` stores only the completed program-emission plan. Lowered
lookup, representation lookup, feature selection, body admission, and output
ordering all finish in `CppProgramEmissionPlanner`; the final program writer
has no `LoweredProgram` or MIR dependency. Its fragments have one explicit
role: support preamble, failure/lifecycle support, native/thread adapters,
declaration/type/member lifecycle text, initializer/module adapters, or hosted
entry adapters. Tests construct plans in an inner scope, destroy every
frontend and lowered input, and then require repeated output to remain
identical.

Generated support is likewise a compiled backend component rather than a
mutable block inside the coordinator. `CppSupportPreambleFeatures` records the
selected C++ standard and exact Expected, storage, managed-thread,
atomic, defined-failure, runtime-binding, and C-ABI text-view needs. The preamble and
failure-support writers receive those explicit values and an output sink;
they do not query program meaning. Storage and defined-failure support remain
universal only because that is the current artifact contract, not because the
writers infer their use.

### Executable Bodies

`CppMirBodyEmitter` is the admission and plan-building authority for source
executable bodies. It receives verified optimized MIR plus copied
target-representation rows and produces an immutable
`CppMirBodyEmissionPlan`. `CppMirBodyTextWriter` formats only that admitted
plan; it cannot inspect MIR or rerun a capability predicate.
Functions, constructors, destructors, lambdas, field/static initializers,
module initialization, hosted startup, and boundary shells all use this route.
Unsupported operations, places, lifetimes, failure edges, or representation
facts reject the complete program before output; there is no AST/HIR body
fallback.

Each body plan owns its readiness and issues, canonical plain and failure text
templates, the deduced-callable variants where target family spelling differs,
boundary-declaration decision, initializer schedule and field spellings, and
hosted-startup kind. The plan is pointer-free; it outlives both the MIR program
and copied representation map used to build it. Placeholder replacement for
family labels and indentation is therefore formatting, not a second
admission path.

Before spelling a body, the backend derives a pointer-free
`CppMirControlFlowPlan` solely from that body's sealed optimized MIR. The plan
lists every block in canonical order, gives it a stable function-local C++
label, and lists every successor by its canonical edge slot: zero for a
`Goto`, switch default, or invoke success; one for branch/invoke failure; and
one-based case slots for a `Switch`. A separate verifier exact-compares the
plan with the MIR block and edge inventory. Missing, duplicate, reordered,
relabelled, or retargeted rows fail emission; the backend has no dispatcher or
other control-flow fallback.

Persistent places, value slots, loan carriers, failure state, and lifecycle
state are declared before the first label. Emission begins with a direct jump
to the planned entry label, and every MIR block is a label followed by its own
compound scope. Static transfers are direct `goto` statements. Branches and
invokes use C++ conditionals whose arms publish the selected MIR edge and then
jump; a MIR `Switch` may use an ordinary C++ `switch`, but each selected arm
also jumps directly. Returns, exits, propagation, unreachable blocks, and
cleanup-failure termination keep their existing terminal spellings. This
scope shape prevents a jump from entering past a C++ initialization while
preserving the old per-block scope exit. GTI destruction and failure cleanup
remain explicit MIR instructions and terminators rather than native RAII.

Direct labels and gotos are the complete production CFG representation,
including reducible and irreducible graphs. The backend does not structure
loops from source syntax or names, and it does not require a mutable block
number plus `for (;;)`/dispatch `switch`. Native optimization is free to
recover natural loops from the direct backedges, while retained checked exits
and call boundaries remain visible consequences of MIR behavior.

A trivially-droppable value whose native representation is not default
constructible may use a representation-only lifetime slot without acquiring a
semantic drop obligation. When an exact producer-to-`Initialize` publication
can execute again through arbitrary verified control flow, the C++ emitter
ends any prior representation immediately before constructing that same
canonical destination. This rearming is permitted only by the existing exact
publication classifier and the proof that no MIR `Drop` governs the slot. It
does not reconstruct loops, alter MIR lifetime semantics, or let an ordinary
cleanup-owning slot replace or conceal a missing `Drop`.

A copy-loaded class result whose only executable use is `Return` also avoids an
invented default-constructed SSA object. The plain C++ form returns the exact
source place by value when the sealed representation row proves copy
construction; the transformed failure form placement-constructs that same
source into its out result. Multiple uses, a mismatched operand/place type, or
an absent copy-construction row reject body admission rather than selecting a
native fallback.

A checked fixed-array whole-element class assignment may likewise erase one
adjacent staging value, but only for the emitter's existing binding-array,
binding-field-array, and receiver-field-array element place families. A named
copy requires one bare-binding `Load(Copy)` source and a copyable destination;
a fresh construction requires a generated cleanup-free class destructor and
no value-rooted place, drop obligation, or lifecycle event. Both forms require
one exact fixed-array bounds origin paired with the assignment's site, the
same source and destination type, one consumer, adjacency, and mutable access
through the sibling array storage. The checked-write helper's tagged lvalue
overload performs copy assignment without constructing a class temporary. Its
by-value route still stages an explicit move or fresh prvalue before the bounds
check, then move-assigns without a second materialization. Projected sources,
unmatched failure metadata, cleanup-owning temporaries, and every other
projected destination remain outside this fusion vocabulary. The fresh-value
form serves one further unprojected destination — a bare mutable binding,
described with the prvalue assignment routes below — where the assignment
carries no bounds site at all.

A fixed-array element place may carry several `Index` projections, and MIR
describes that whole chained access with one instruction and one bounds site.
The C++ text therefore owns a bound for every dimension rather than only the
last. A nonterminal index that is a compile-time in-bounds constant spells an
ordinary subscript. A dynamic one spells either the routed guard — one
`mir_checked_array_bound_v1` status assignment opening the success branch the
terminal checked read or write runs inside, so the first violation reaches the
instruction's failure record — or, in the plain text form and every other
element spelling, the contained `array_at` accessor that already carries the
terminal index's bound there. The fully discharged spelling drops both, which
is why the safety proof for such an access must cover every dimension.

Optimizer tombstones are verified inert proof slots and emit no operation.
An instruction safety discharge is likewise consumed only as sealed optimized
MIR, never as a backend analysis result. When all local overflow outcomes for a
checked add, subtract, multiply, or negate are discharged, the C++ backend may
emit the direct arithmetic representation. When all local outcomes for an
admitted fixed-array read are discharged, it may emit a direct indexed read.
For a private sparse-storage read or mutable read, it may select the
backend-private unchecked helper only when both the exact bounds and
initialized-slot outcomes are discharged. A fully discharged `Invoke` follows
its verified success edge; any retained local or callee failure keeps the
checked representation and failure transfer.

These choices inspect only MIR operation identities and exact discharge
metadata. They do not recognize `std` wrappers, source declaration names, or
generated C++ shape. Prefix storage and every unsupported operation remain on
their checked backend route.

For a scalar block parameter, the C++ MIR body emitter owns one backend-private
edge slot. Every `Goto`, `Branch`, `Invoke`, or `Switch` predecessor
publishes its verified arguments to those slots before transferring control;
the destination block reads the corresponding MIR values. Preflight scans the
same edge operands and rejects missing type/representation rows. This is a
representation of MIR value merging, not reconstruction of a source local or
an independent C++-level phi decision.

The backend receives neither canonical source MIR nor optimizer records.
`LoweredProgramBuilder` has already replayed those untrusted records and
sealed exact source-to-optimized coherence. Consequently another executable
backend can consume the same block parameters, effects, and transformed MIR
without rerunning a GTI optimization decision.

For a verified temporary receiver input, the body emitter publishes the exact
producer result into its MIR value-rooted lifetime slot or exact CFG-promoted
hidden slot and emits the selected method through the staged `BorrowRead` or
`BorrowWrite` input. Verification and emission use the same MIR
materialization query. The corresponding MIR drop remains the only destruction
authority on normal and defined-failure paths. The backend does not infer
mutability from a C++ rvalue or reselect the native member overload.

For a verified read-only temporary argument input, the body emitter uses the
same exact value-rooted lifetime slot and passes that slot to the selected
reference parameter. Its `CallArgument` loan is representation-erased only
after preflight proves the matching `BorrowRead`, call input, immediate
loan-end schedule, and full-expression drop. Failure successors use the MIR
cleanup order. The emitter neither binds an arbitrary C++ prvalue nor infers
eligibility from the generated signature.

For a loan-carrying reference argument, the operand may name both a loan and
an exact referent-typed runtime place. The place is the argument lvalue and
the loan is provenance: a borrow-carrying local (an owner-tied iterator, for
example) records its owner loan on the staged operand, and that loan may be
representation-erased. Whenever the operand records a place, the body emitter
spells that place at the call site; only a place-less loan operand spells the
dereferenced loan pointer carrier, which must therefore name a declared
carrier such as a reference-parameter entry loan. MIR verification also
requires a staged call-input place to equal the ownership event's exact
semantic place key, in addition to resolving to the loan's provenance source.
Two same-typed iterator values over one owner are therefore not interchangeable
runtime carriers merely because they share owner provenance.

For an exact cleanup-owning class value consumed by an ordinary by-value call,
the body emitter uses the caller-owned `PreparedParameter` slot named by MIR.
A direct `Construct` or admitted cleanup-owning call result publishes once into
that slot; its source drop remains authoritative until the matching `CallInput`
reparents it, and the consuming `Call` transfers the prepared drop exactly
once. An owning `Expected` extraction may use its value-rooted source place as
representation and drop metadata only when it is the sole unprojected use in
that exact reparent chain, with matching type, access, drop, call-site,
ordering, and success/failure facts. That metadata place emits no C++ local or
default construction. Failure before extraction success never arms the
prepared slot, and unsupported, aliased, duplicated, projected, or mismatched
shapes remain rejected rather than receiving backend-invented storage or
cleanup.

A cleanup-free class argument staged by one `Move` or one failure-free copy
`Load` whose single record is one call operand in the same block declares as a
sequenced `auto` local at its producing instruction, and the consuming
argument moves that local. The sequenced local preserves MIR's
move-or-copy-before-call order, which an inline place expression in the
argument list would race under C++'s unspecified evaluation order. The copy
form requires the type's copy-construction representation row and admits only
class-typed values, so lambda closures and unique owners keep their fused and
prepared spellings; nothing between the producer and the call may touch the
source place.

For a verified `MoveBorrowedTemporary` constructor input, the body emitter
publishes the exact producer call result into its MIR value-rooted lifetime
slot, moves that value into the selected constructor parameter, and ends the
moved-from slot representation after the consuming construction. MIR remains
authoritative for producer success, ownership transfer, constructor failure
cleanup, and the resulting value's one drop.

When a generic source member has only a failure-form C++ entry point but a
particular concrete MIR body is proved unable to fail, `CppEmitter` publishes a
private failure-free boundary wrapper before its first use, but only when a
verified body actually consumes that exact member as an inline class-valued
producer. The wrapper invokes the existing failure sibling, returns its
constructed result, and terminates if the verified impossible failure is
observed. This representation adapter does not erase failure from compiler
meaning, manufacture ordinary entry points for unrelated members, or make
native exception behavior a lifetime authority; it exists only so an exact
nested call can participate in an ordinary constructor full expression.

A cleanup-free class result with one exact value-rooted field projection and
one scalar `Load` uses the same uninitialized lifetime-slot representation.
An admitted GTI call or ordinary `Construct` publishes once into the slot, and
the load spells through its verified field chain. The constructor form also
requires exact target, owner, result, parameter, and operand metadata; one
single-field projection; a trivial scalar copy load; and no active cleanup,
lifecycle, borrowed-state, or drop obligation. Additional consumers, nested or
non-field projections, cleanup-owning temporaries, and incoherent type rows
reject body emission rather than falling back to native temporary behavior.

Constructor initializer lists and ABI-shaped wrappers may be assembled by
`CppEmitter`, but their executable operations, ordering, cleanup, and failure
effects come from the lowered MIR and generated-item contracts. A nameable
source-defined generic free-function instance emits as one ordinary C++
function, not a native template specialization. Its backend-private
`__gti_inst_<digest>_<source-name>` symbol hashes a versioned serialization of
the lowered qualified declaration identity, generic parameter shape, concrete
type/value arguments, and MIR return/parameter signature. The identity contains
no discovery-order `FunctionId`, and the representation snapshot copies that
exact symbol into every normal or failure-form call row.

A selected, nameable application of an eligible top-level generic class
similarly emits as an ordinary `__gti_type_<digest>_<source-name>` class. The
digest owns the nominal primary, source-unit identity, generic parameter shape,
and exact type/value arguments. Fields, bases, initializers, lifecycle status,
and constructors come from the matching sealed MIR class instance. An ordinary
method on that class, and a nameable generic method on an ordinary class, uses
`__gti_member_<digest>_<source-name>` unless C++ requires the source spelling
for an operator or virtual override family. Nameable generic constructors on
ordinary classes publish concrete overloads by their exact signatures. Exact
class specializations selected by GTI take the same direct-class route; C++
does not select a specialization. Every member definition and call uses the
spelling copied into the representation snapshot, while executable text still
comes only from verified MIR.

The direct-class route deliberately retains a native physical carrier for an
unused family, a family containing nested type declarations, a compiler or C
ABI capability, a generic owner whose storage directly contains the private
unique-owner capability, a program containing an otherwise-unnameable lexical
closure, or a dependent generic family named by one of those carriers. The
planner closes that last relation transitively across bases, fields, aliases,
enumerator payloads, and member/constructor signatures before it emits any
forward declaration. The ownership exception preserves C++'s delayed incomplete-
pointee instantiation for recursive shapes such as
`Node -> std::unique_ptr<Node>`. The closure exception keeps dependent class
graphs on one ABI until the representation snapshot records an explicit
carrier closure. A direct class's field-initializer schedule always uses its
concrete representation row, never a declaration-level `T` or value parameter.
A direct generic class definition is ordered by its exact concrete application
and exact substituted by-value fields, not by one collapsed source-family
node. Finite alternating nests such as `box<cell<box<int>>>` therefore form an
acyclic concrete layout graph even though both source families mention one
another across the program. A genuine cycle in that exact graph remains a
fail-closed backend invariant violation; ordinary source-level recursive
by-value fields are rejected earlier by semantics.
A selected generic method on a retained carrier keeps its stable
`__gti_member_` name, but the backend emits the required owner-specialization
declaration before any use. These carriers consume identities and validity
already chosen by GTI; they cannot perform lookup, overload selection,
constraint checking, ownership decisions, failure analysis, or execution
outside verified MIR. Native overload resolution and template deduction
therefore provide physical spelling only for the named carrier cases, never
language semantics.

Closure-typed (unnameable) instances of one generic declaration emit through
a single deduced-callable template whose body text is canonical per
parameter-cleanup shape: the shape records which declared parameters carry a
lexical drop obligation in that instance's verified MIR. Instances sharing a
shape must prove byte-identical overlaid body text. When concrete callable
environments produce different shapes — a scalar-capturing lambda beside a
class-capturing one — the one emitted template reunites the canonical variant
bodies behind an instantiation-time `std::is_trivially_destructible_v`
dispatch over each varying parameter, which must be declared directly as a
template type parameter (or be the exact thread-task parameter). Any variance
the dispatch cannot express fails closed rather than merging inconsistent
bodies.

For a callable member template on a generic owner, that canonical comparison
is partitioned by the exact owner instance, matching the owner-specialized C++
definition that will consume it. Its projected signature substitutes every
owner type/value parameter while retaining only the member template's own
parameters as dependent C++ names. Thus two owners such as `container<int32_t>`
and `container<uint8_t>` neither masquerade as one canonical MIR body nor leak
an out-of-scope owner `T` into an explicit owner specialization. Identity,
substitution completeness, and the fully concrete MIR signature are checked
before the projection is admitted; incompatible rows still fail closed.

Lowered declarations retain an explicit default-constructor or destructor
specifier for source/coherence and tooling inventory. Because that policy has
no source body, the emitter does not publish a separate member definition; it
uses the class lifecycle row's `Generated` status to spell the structural
operation. A policy declaration never becomes an empty C++ body: semantic
analysis has already decided structural availability, and the lifecycle status
remains the backend authority for that result.

When a field or static initializer selects a failure-capable source
constructor, the private C++ representation snapshot derives one exact
contained-constructor row from the lowered initializer schedule and constructor
record. The row supplies backend-private tag and state spellings. Its generated
overload delegates to the exact failure form and terminates with the unchanged
failure record when construction fails. The emitter rejects missing,
duplicated, or mismatched rows; no C++ spelling or containment policy enters
`LoweredProgram`.

The passive initializer schedule keeps a closed expression vocabulary.
Transparent `Identity` values preserve one exact staged spelling, `Convert`
uses the copied target-type row and retains either the unchecked cast or the
terminally contained checked-conversion edge, and `PayloadConstruct` uses the
exact copied enum variant and ordered field types. Each form requires its
producer-before-consumer order, exact single-use chain, matching types, and
trivial cleanup state. Any operation outside the listed vocabulary still
rejects body admission; initializer emission never falls back to AST/HIR or
general C++ expression text.

An active payload-enum body uses the same verified-MIR route, not the passive
initializer shortcut. The representation snapshot copies the exact MIR enum
traits, enum-wide active-cleanup bit, dense variants, field types, and
per-field cleanup facts. Admission exact-compares every row with MIR.
`PayloadConstruct` consumes only operands carrying the matching `TransferOut`
events, constructs the selected `std::variant` alternative in an enum lifetime
slot, and publishes that slot only through the matching enum lifecycle event.
Whole-enum move and tag-only switch observation use the exact MIR places and
obligations. The exact adjacent Move -> Replace -> Drop schedule also admits
whole-enum move assignment: native assignment replaces the destination's
active alternative, then MIR destroys only the moved-from staging shell.
Generated C++ names and `std::variant` are representation choices; neither
selects ownership behavior or identifies a source enum. Owning enum
call/return and aggregate representation are not backend vocabulary in this
slice and are rejected semantically before MIR.

A class-typed fixed-array field uses that schedule only when MIR proves every
element construction, aggregate transfer, array-result obligation, and
reparent into the field's `ConstructionRollback` obligation. Passive classes,
generated moves of cleanup-owning classes, and exact concrete polymorphic
classes with interface-only bases share this route; state-bearing bases,
custom moves, and ambiguous class instances do not. A retained rollback
destructor `Invoke` is representation-only only when its exact source
destructor body is status-proven unable to fail and its cleanup-failure block
matches the verified MIR record. Constant element-field projections consume
the sibling array place and concrete field identities. Dynamic checked
indices remain on their existing MIR operation route rather than being
reconstructed from a native spelling.

A failure-form constructor may publish owned fields from its MIR body instead
of the native C++ member-initializer list when native initialization would
reorder an earlier operation after a later parameter move. This route requires
an exact move, obligation reparenting, full-expression, recursive private
empty-state, and construction-rollback proof. A failure-capable class temporary
passed to another constructor likewise remains in its verified value-rooted
lifetime slot until the consuming construction finishes. Only fields named by
the explicit constructor-initializer schedule are replaced with private empty
state; unrelated in-class initializers retain their source-defined value.
Fixed-array empty state is built recursively element by element, so a nested
array of concrete generic class elements uses each element's private tag
constructor and never assumes that the public class has a zero-argument
constructor. This remains a backend-only rollback representation and does not
add a GTI constructor or observable initialized value.
When an explicit class-field construction remains in an ordinary constructor
body, the exact `ConstructorFieldResultSlot` schedule constructs the selected
class prvalue directly into its value-rooted lifetime slot, moves it into the
field, and destroys only the moved-from shell. The ordinary C++ route uses the
slot's factory construction so it never names an undeclared SSA value; the
failure route retains its existing placement construction and success edge.
Nested receiver-field access is emitted only when every projection resolves
through an exact class field and representation row. Missing or additional
lifecycle uses reject the body rather than changing source evaluation order.
A read through a receiver's stored class-reference field uses the exact
`Field, Dereference, Field...` MIR projection path: the reference field must
have its copied reference-type row, and every trailing field must resolve from
the referent type through an exact field row. The C++ alias relies only on the
language's existing implicit reference-member dereference; unsupported
projections fail body admission.

A fixed-array drop with a concrete MIR schedule uses the same lifetime-slot
boundary for its aggregate storage. The C++ representation recursively visits
the recorded extents in reverse order, invokes the exact leaf failure-form
destructor, and destroys each completed leaf once. Ordinary cleanup preserves
the first defined failure while completing the remaining leaves; cleanup while
another failure is active retains the existing double-failure termination
rule. The emitter rejects arrays whose schedule, leaf class, or destructor is
not already verified by MIR.

An all-owned-parameter initializer list may stay in native member-initializer
form only when every declared initializer has one exact source-parameter move
and the complete field order is proven. Cleanup-free fields require no
invented drop record. An active-cleanup field additionally requires its value
drop, construction rollback reparent, field drop, and one unconditional
transfer-out in the verified constructor prologue. Mixed passive and active
fields are admitted only when each field independently satisfies its matching
schedule and no other lifecycle event mentions those obligations.

An exact by-value parameter copy may also publish one field through the native
member-initializer list when the copied class has generated copy/destruction,
only passive scalar or enum state, no state-bearing base, and no active cleanup.
This MIR shape carries one reachable parameter `Load`, one matching initializer
metadata row, and one full-expression boundary but no field `Initialize`; the
copied parameter's native scope destruction represents its trivial MIR drop.
The scalar copy route continues to require its explicit `Load`/`Initialize`
pair. Custom lifecycle, extra uses, forged ownership, ambiguous fields, and
mixed initializer schedules fail closed.

An explicit class-typed field initializer whose value is one ordinary
`Construct` of the field's own type may publish through the same native
member-initializer list. C++17 guaranteed elision makes
`field(Type(arguments...))` the exact representation of MIR's
construct-then-publish schedule: the field is constructed once, in place, with
no intermediate object and no invented copy or move. Because C++ initializes
members in declaration order, the proof is whole-constructor: every declared
field must have its own explicit initializer in declaration order, each
construction argument must be either an exact copy-load of one scalar
constructor parameter or an exact source scalar literal, and the entry block
must contain nothing but the named instructions and metadata boundaries.
A sibling field may share the schedule only through the exact parameter-copy
proof, which publishes natively as well. A cleanup-free field carries no drop
obligation and publishes through initializer metadata alone; an active-cleanup
field additionally requires the produced value's `Value` obligation, its
`Reparent` into the field's `ConstructionRollback` obligation, and one
unconditional transfer-out at normal completion. A failure-capable target
constructor is admitted only when its verified MIR body can never report a
failure status and that ordinary body is itself published by this route, so
the plain C++ overload exists at the call site. Folded or synthesized literals,
other computations, call results, borrowed sources, concrete generic targets,
base initializers, and partially claimed schedules stay in the MIR body
schedule.

Obligation-free class values in a move capture or an owning `Expected` payload
remain concrete MIR values rather than backend-recognized library types. The
C++ backend may materialize the exact closure environment, direct payload
return slot, or unprojected extraction place only after proving unique use,
matching concrete type, passive lifecycle, and the recorded full-expression
boundary. Active cleanup and additional projections stay on their existing
lifetime-slot paths.

Move-only class replacement of one direct mutable receiver field has three
additional exact representations. A transformed source call may initialize one
value-rooted obligation, publish it through an unconditional `Replace` into
the field, and then drop only the moved-from slot shell. A move from one field
through a unique mutable `CallResult` loan may initialize the same kind of
slot only when the move and receiver-field assignment are adjacent, the
success path ends that loan before the slot drop, and no alias or extra
lifecycle event exists. A targetless concrete default construction may use the
same replace/drop schedule when its own unconditional `Initialize` is the
single activation. Each route requires an exact noncopyable, move-assignable
class representation and one resolved active-cleanup field row. The emitter
does not recognize the owning wrapper or container name; projected
destinations, conditional lifecycle events, incomplete borrow endings, or
additional consumers fail closed.

Move-assignment between two existing mutable class bindings has its own exact
representation. MIR stages the moved value in a value-rooted obligation, so
the backend constructs that staging slot from the source binding, move-assigns
the destination binding from the slot, and then drops only the moved-from
staging shell. The route requires an adjacent `Move`/`Assign`/`Drop` triple in
one block, a `Move` lifecycle event transferring the source binding obligation
into the staging drop, an unconditional `Replace` from that drop into the
destination binding's own obligation, and a single unconditional `Drop` of the
slot. Aliased source and destination bindings, projected or non-binding places,
failure sites on either instruction, extra uses, and any additional lifecycle
event on the staging obligation fail closed.

Assigning a freshly constructed class prvalue into an existing mutable binding
has its own exact representation, and MIR represents it in two shapes rather
than one. A cleanup-owning prvalue is staged in a value-rooted obligation: the
backend constructs that staging slot from the `Construct`, move-assigns the
destination binding from the slot, and then drops only the moved-from staging
shell. A non-raising constructor arms the staging obligation with its own
unconditional `Initialize` and sits immediately before the publication; a
raising one arms it on its invoke success edge instead, so the `Construct`
ends the invoking block and the `Assign` opens that edge's successor. Either
way the route requires an unconditional `Replace` from the staging drop, an
adjacent single unconditional `Drop` of the slot, and exactly one activation.

The publication has three destination families, distinguished by which
obligation owns the storage being replaced. A bare mutable binding owns its
own cleanup obligation, so the `Replace` transfers into that obligation and
the write carries no failure site. A checked fixed-array element does not:
the array binding's obligation already covers every element, so the `Replace`
names no destination obligation and the write carries the element's bounds
failure site — the same element shape the cleanup-free fusion route admits,
validated by the same authority. A mutable loan referent also has no separate
destination obligation because its aggregate owner already covers the object,
and writing through the produced loan has no new failure site. That route
requires the exact reserved-loan activation immediately before `Assign`, the
matching `EndBorrow` immediately after it, and only then the staging-slot
`Drop`. Requiring a destination obligation for an element or loan referent
would retire the same storage twice, so either publication fails closed when
it names one. Every other destination, a bare-binding or loan publication with
a failure site, an incomplete two-phase loan schedule, extra uses, and any
additional lifecycle event on the staging obligation also fail closed.

A cleanup-free class prvalue has no staging obligation at all: no place or
drop obligation owns the constructed value and the discarded assignment result
is metadata only, so the construction spells directly in the value position of
its one adjacent assignment — into a checked fixed-array element, a bare
mutable binding, or the referent of a mutable borrow, which is what an
overloaded subscript yields. A loan referent's storage belongs to whatever
the loan points into, so like an element it carries no obligation of its own
and, like a bare binding, its write carries no failure site. Materializing an
intermediate C++ object there would invent construction and destruction that
MIR does not contain.

### Declarations And ABI

`CppEmitter` walks `LoweredDeclaration` rows to assemble namespaces, aliases,
classes, enums, access sections, templates, callable signatures, global/static
storage, fields, native linkage, opaque ABI types, compiler-provided
representations, and generated adapters. It uses lowered symbols and instances
for resolved identity and C++-private helpers for spelling.

An exact specialized class arrives with a distinct lowered class identity, its
generic primary identity, canonical concrete argument key, and canonical
namespace. When the family is eligible for direct representation, the C++
backend emits that already-selected identity as an ordinary digest-named class
and does not emit a native primary/specialization pair. A retained physical
carrier instead receives the already-selected full specialization and orders
it after its primary. Neither route matches source arguments or chooses between
the primary and specialization.

C++ type syntax, include selection, helper names, failure ABI syntax, and
representation choices remain inside the C++ backend. C++ source fragments do
not belong in `LoweredProgram`.

A verified `SemanticType::FunctionItem` receives the private zero-state C++
representation `gti_internal::backend::function_item<FunctionId>`. This is a
nominal tag, not a C++ function pointer, overload set, or ABI address. The body
plan admits `MirOperation::FunctionItem` only with its exact copied target row;
invocation spells a direct call to that already selected GTI function. Missing
or mismatched representation, target, nominal identity, or signature fails
body admission rather than asking C++ to resolve the name.

### Defined Failure

MIR carries canonical failure metadata, detector sites, failure edges, cleanup
schedules, propagation, and publication behavior. The C++ backend materializes
that contract as runtime descriptors and transformed failure siblings where
the native ABI requires them. It does not infer failure from emitted C++ or use
C++ exceptions to reconstruct GTI semantics.

Each native callback item produces one typed `extern "C" noexcept` thunk. A
non-failing target is called directly. A failure-capable target uses its
verified failure sibling and forwards the original record to the runtime
terminal boundary. Native exceptions are contained and terminate; the backend
cannot select another target or weaken that policy.

A call resolved to a concrete override remains virtual in MIR; the C++ backend
does not infer devirtualization. For a conservatively failure-capable virtual
call, the backend validates the exact contract roots and candidate owners, then
classifies only the concrete dispatch owner's subtree. A complete subtree of
failure-free source definitions uses the ordinary virtual call. If any valid
descendant can fail, the call and its `Invoke` edge retain the transformed
status ABI. Valid family members outside that subtree cannot make a direct
concrete call incomplete, while missing, unrelated, or forged owner/root/target
metadata fails closed.

Each thread-task item produces one private type-specific adapter rooted by its
exact verified spawn instruction, exact concrete function-or-lambda entry, and
exact `void` or owned result type.
For a function-item task the adapter directly invokes the selected namespace
function and never boxes a pointer value. Explicit specialization declarations
may precede the GTI program namespace, but definitions are emitted only after
the complete program declaration/definition region so the direct target is
declared before adapter text.
For an overloaded nominal callable the adapter invokes the already selected
generated member spelling with its verified read, mutable, or consuming
receiver; C++ overload resolution is not authority. The backend boxes the
concrete task only at that boundary and passes an opaque context plus
entry/destroy callbacks to the version-one runtime thread ABI. Creation
failure destroys the box on the spawning thread exactly once; success invokes
it once and destroys it on the worker. A typed adapter additionally constructs
one result box and supplies exact take and abandon callbacks: successful
explicit join transfers that box once, while automatic join destroys it once.
The entry catches only the runtime's in-process failure-handoff sentinel after
MIR cleanup and terminates on any unrelated native exception. Neither the
opaque runtime state nor a C++ `std::thread` type appears in public GTI source
or `LoweredProgram`.

Private handle intrinsics lower from their exact verified MIR identities.
Spawn and join retain their synchronization records; explicit and automatic
join translate native status separately from an ordinary worker record or
cleanup-emergency envelope. The latter two are reintroduced into the joining
body's existing MIR failure channel with their descriptor pointers unchanged.
Task-object/capture destruction remains inside the worker boundary and may
publish the first failure or the second record of an emergency envelope without
throwing through a native `noexcept` destructor. A recoverable explicit join
failure retains the handle for retry. The same status during automatic join
has no return channel: the backend transfers the still-coherent native state
to a private terminal quarantine before propagating `GTI-R0012`, preventing a
live representation destructor while the hosted terminal path completes.
The backend does not infer this route from `std::thread`, a destructor name,
or native C++ ownership behavior.

Private atomic intrinsics lower only from their exact verified operation and
memory-order records. Representation planning maps compiler-owned
`atomic_storage<T>` to the conditional native atomic support type; body
preflight independently validates storage, scalar operands, result shape, and
the complete operation-specific order matrix. Text emission then selects the
native `relaxed`, `acquire`, `release`, `acq_rel`, or `seq_cst` spelling from
that record. It never inspects a public `std::atomic` method, order-tag source
expression, or native overload to reconstruct semantics. Strong CAS emits both
verified success and failure orders, including the exact nine legal pairs.

## Native Header Backend

`NativeHeaderBackend` derives public records, opaque handles, callback aliases,
external signatures, namespace scopes, field names, and target-resolved record
layouts from lowered declarations and symbols. It has no frontend collector.
Malformed lowered values are rejected before header emission.

## Boundary Enforcement

The architecture is guarded by four direct forms of evidence in addition to
the normal compiler/runtime matrices:

- an independent translation unit includes only `lowered_program.h`, verifies
  the value, inventories every top-level table and generated family, and prints
  it deterministically;
- verifier and backend mutation tests reject missing, duplicate, stale,
  reordered, unrooted, cyclic, and seal-invalid values;
- a complete shipped-example census records declaration, body, generated-item,
  and per-family counts; and
- a structural source gate rejects frontend includes, frontend representation
  identifiers, old `BackendInput` signatures, and builder leakage in production
  backend files.

See [`verification.md`](verification.md) for the broader C++20/C++23, installed
toolchain, standard-library, FFI, runtime, and LSP matrices.

## Driver Handoff

The reusable driver owns construction order, backend selection, atomic artifact
publication, native compilation, GTI-facing translation of native diagnostics,
and output publication. Backend code must not assume a command-line working
directory or write final artifacts directly. Runtime and standard-library
locations come from the driver/toolchain installation contract.

## Future Backends

A future LLVM or other native backend receives `LoweredProgram` plus its own
explicit target policy. It must not link frontend representations, receive
source MIR or optimizer tables, parse generated C++, depend on C++ spelling
helpers, or treat HIR as executable authority. If a backend-neutral fact is
missing, the compiler extends and verifies the lowered contract; a backend may
not recover that fact from an upstream phase or native behavior.

Compatibility with the deleted multi-representation route is not a project
constraint. A new backend is an independent client of the lowered contract,
not another branch in the C++ emitter.
