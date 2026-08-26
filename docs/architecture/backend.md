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
| Thread task adapter | exact task type, spawn body/instruction, once-invocation contract, synchronization identity, and worker containment firewalls |
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
fixed arrays of class values whose MIR aggregate schedule proves ordered
element construction, rollback, transfer, and publication. Global and static
declarations, the module construction carrier, and later symbol-rooted reads
or writes all consume that same row. An access first exposes the engaged value
and then follows the exact index or field projection rows; missing slot or
projection facts fail body admission. This is only a target representation of
the existing MIR storage and initialization order. It does not admit
cleanup-owning program storage or make C++ static lifetime rules an execution
authority.

Backends choose helper names, monomorphization mechanics, ABI spelling, and
target syntax. They cannot rediscover whether an item exists from source
declarations or HIR use sites.

## C++ Backend

`CppBackend::generate` verifies the lowered value and constructs
`CppEmitter(program, standard)`. The emitter then:

1. adapts the lowered tables into a C++-private representation snapshot;
2. seals a complete body, declaration, data, and generated-item inventory;
3. builds the complete `CppMirProgramPlan`;
4. builds target-spelled MIR body rows; and
5. emits only when the whole-program route is complete and `VerifiedMir`.

The private snapshot and plan are target-specific derived forms, not compiler
meaning and not backend API. Their builders accept only `LoweredProgram`.
Frontend-backed snapshot overloads and comparison oracles were removed with
the final cutover.

### Executable Bodies

`CppMirBodyEmitter` is the text authority for source executable bodies. It
receives verified optimized MIR plus copied target-representation rows.
Functions, constructors, destructors, lambdas, field/static initializers,
module initialization, hosted startup, and boundary shells all use this route.
Unsupported operations, places, lifetimes, failure edges, or representation
facts reject the complete program before output; there is no AST/HIR body
fallback.

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
The call constructs the slot, and the load spells through its verified field
chain. Additional consumers, non-field projections, active cleanup, or
incoherent type rows reject body emission rather than falling back to native
temporary behavior.

Constructor initializer lists and ABI-shaped wrappers may be assembled by
`CppEmitter`, but their executable operations, ordering, cleanup, and failure
effects come from the lowered MIR and generated-item contracts. Generic source
surfaces may be represented as C++ templates and concrete specializations;
specialization eligibility comes from concrete-instance items, and executable
specialization text still comes from MIR.

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

A failure-form constructor may publish owned fields from its MIR body instead
of the native C++ member-initializer list when native initialization would
reorder an earlier operation after a later parameter move. This route requires
an exact move, obligation reparenting, full-expression, recursive private
empty-state, and construction-rollback proof. A failure-capable class temporary
passed to another constructor likewise remains in its verified value-rooted
lifetime slot until the consuming construction finishes. Only fields named by
the explicit constructor-initializer schedule are replaced with private empty
state; unrelated in-class initializers retain their source-defined value.
Nested receiver-field access is emitted only when every projection resolves
through an exact class field and representation row. Missing or additional
lifecycle uses reject the body rather than changing source evaluation order.

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

### Declarations And ABI

`CppEmitter` walks `LoweredDeclaration` rows to assemble namespaces, aliases,
classes, enums, access sections, templates, callable signatures, global/static
storage, fields, native linkage, opaque ABI types, compiler-provided
representations, and generated adapters. It uses lowered symbols and instances
for resolved identity and C++-private helpers for spelling.

An exact specialized class arrives with a distinct lowered class identity, its
generic primary identity, canonical concrete argument key, and canonical
namespace. The C++ backend emits that already-selected identity as an explicit
full specialization, forward-declares it before instantiating uses, and orders
its definition after the primary. It does not match source arguments or choose
between the primary and specialization.

C++ type syntax, include selection, helper names, failure ABI syntax, and
representation choices remain inside the C++ backend. C++ source fragments do
not belong in `LoweredProgram`.

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

Each thread-task item produces one private type-specific adapter rooted by its
exact verified spawn instruction and exact concrete function-or-lambda entry.
For an overloaded nominal callable the adapter invokes the already selected
generated member spelling with its verified read, mutable, or consuming
receiver; C++ overload resolution is not authority. The backend boxes the
concrete task only at that boundary and passes an opaque context plus
entry/destroy callbacks to the version-one runtime thread ABI. Creation
failure destroys the box on the spawning thread exactly once; success invokes
it once and destroys it on the worker. The entry catches only the runtime's
in-process failure-handoff sentinel after MIR cleanup and terminates on any
unrelated native exception. Neither the opaque runtime state nor a C++
`std::thread` type appears in public GTI source or `LoweredProgram`.

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
The backend does not infer this route from `std::jthread`, a destructor name,
or native C++ ownership behavior.

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
