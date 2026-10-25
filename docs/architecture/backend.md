# Backend And Native Handoff

Status: current architecture. LLVM is the primary development backend. C++ is
deprecated as the primary backend and maintained with bug fixes only through
v1.0 as a comparison oracle. It remains the shipped default until LLVM passes
O-LLVM-16; a future C++ catch-up requires a separate owner decision. See
[ADR 031's backend policy](../decisions/031-native-llvm-backend-foundation.md#backend-development-policy-2026-10-25).

GTI ships a C++ source backend and a selectable native LLVM backend, which
already emits the documented scalar, places/text and ordinary class/lifecycle
profiles. Unsupported required LLVM shapes reject before artifact publication;
full-language coverage remains in progress. Both consume one compiler-owned,
backend-neutral `LoweredProgram`;
they do not receive the AST,
`SemanticModel`, `HirProgram`, source MIR, optimizer side tables, or generated
C++. The backend chooses target representation, but it cannot recover missing
language meaning from an upstream representation or from host C++ behavior.

Typed macro results reach this boundary only as verified ordinary MIR
initialization, lifecycle, cleanup and join transport. The C++ backend never
executes a HIR result region or interprets a macro-local return as a native
function return. Written-expression metadata is normalized to a static text
literal before MIR; a leaked source-query capability is rejected, not emitted
as a runtime function or evaluated argument.

Body-template presentation substitutes the family label and indentation with
two append-only passes, in that order. It does not admit operations or consult
MIR/semantic state. This avoids repeatedly shifting a large cleanup body for
every indentation marker while preserving the established output bytes.

Body-text queries reuse instruction-ID and referenced-place indexes within the
existing representation scope for one immutable verified lowered program.
These indexes contain existing MIR records, not cached permission to emit an
operation. Copies, foreign bodies and unscoped mutation probes use direct
queries; nested consumers of a different snapshot cannot reuse the outer
indexes. Scoped/unscoped
plan comparisons pin byte-identical output and nested-snapshot isolation.

Whole owning-array movement into an adjacent fresh binding uses the shared
failure-free relocation proof, not the narrower passive move/assignment proof.
Generated transfer may include guarded source destructors: it transfers their
active cleanup state without running them. The exact typed source, vacant
destination, single result consumer and lifecycle reparenting remain mandatory;
this does not authorize replacement of live objects.

Hosted argument marshalling consumes the verified startup drop recipe when
argument construction or append fails. A source-owned vector may have
failure-capable destruction: its secondary-failure Invoke/terminal envelope is
checked rather than projected away as failure-free behavior. Emission runs that
exact cleanup under the original failure context before terminal containment.
No hidden native unwinding replaces that MIR schedule. Owning payload-enum
returns use the same exact unpublished-result recipe, including a secondary
failure during rollback.

The phase direction is:

```text
source -> AST -> semantics -> HIR -> source MIR -> verified optimization
       -> LoweredProgram -> backend -> BackendArtifact -> driver
```

AST owns written syntax, semantics owns resolved language meaning, HIR owns
concrete instances and typed values, MIR owns executable control flow and
effects, `LoweredProgram` owns the complete backend-neutral program contract,
and each backend owns target representation.

Payload enum representation rows carry the complete semantic enum type, not
only an `EnumId`. Definition naming, dependency scheduling, construction,
matching and lifecycle lookup distinguish every exact instance. Verification
rejects missing or conflicting concrete rows; generic primaries cannot serve as
a fallback representation. Passive aggregate enum results use verified folded
traits, while an owning enum argument produced by a call reuses that call's
proved success-only result slot and ordinary ordered transfer/failure cleanup.
No AST/HIR-only enum execution route is introduced.

Guarded enum reference inventories include abstract referent paths that are
provenance, not unconditional reads of native fields in possibly inactive
alternatives. The backend omits a path's physical alias only after checking its
exact typed enum-field/dereference chain and absence of runtime storage/address
uses. Stored loans carry no native pointer; selected-reference extraction,
reference-returning calls and exact return-pointer transfers supply their own
runtime addresses. Generic Borrow or entry-address uses cannot hide behind this
classification. The same rule admits a place rooted at a field-less Stored
loan, whose borrow-carrying local or class value holds its references
internally. Such a place names the carried external owner, for example as the
source of a reference selected from that carrier's payload. It is never spelled,
so any runtime use of it rejects the body. A Stored dependency inherited from a
reference binding names that binding's referent, as a parameter entry does.
Fresh owning enum returns may cross their verified erased Stored-to-Return
borrow publication and EndBorrow markers without inventing a second result
slot; all actual cleanup and success-only initialization remains in the
original MIR order.

Payload alternatives construct in place from their already selected MIR
operands. The native enum's in-place alternative constructor avoids temporary
alternative and whole-enum transfers. List initialization preserves operand
evaluation order. An owning fresh alternative followed immediately by its
constructor-field `Initialize` may use the final field storage directly: the
existing publication proof establishes the observation-free empty field shell,
unique result consumer, result activation, field rollback obligation and exact
`Reparent`. Adjacency excludes intervening failures or observations. Non-adjacent
and returned/moved inputs retain their original staged publication. MIR cleanup
and lifetime events are not removed or rescheduled by this representation choice.

A switch over a payload-enum place observes that place's discriminant even
when all payloads are passive. It does not first copy the complete enum. Arm
bindings independently retain their selected copy, borrow or consuming-transfer
semantics; an ordinary integral enum still uses the scalar switch path.

A staged call receiver can cross the existing scalar conditional-argument
control flow when its exact origin remains unchanged. The receiver place may be
a bare local, the exact full-expression slot, or a binding-rooted path of field
selections and reference dereferences, such as a container field inside a
`mut { }` group reached through a `mut` reference parameter: references are
never reseated and field offsets are constant, so re-spelling that place at the
consuming call denotes the object staged before the diamond. Index, raw-memory
and pack projections remain outside this proof. Checked numeric alias
and concrete type-parameter conversions use MIR's numeric-intrinsic `Call`
representation; they receive the same staging admission as `Compute/Convert`.
The conversion still executes at its own instruction, with its original Invoke
and failure cleanup before later arguments. This does not admit arbitrary
intervening calls, receiver mutation, raw memory operations or new loan shapes.

Signature-pattern primaries retain both their resolved public argument shapes
and binder projections in sealed lowered declarations. Native template carriers
spell a primary with the public arity and one patterned definition; GTI has
already checked all applications and selected their concrete identities.
The C++ spelling does not introduce competing specialization selection or
source-level deduction. Concrete representations use the same resolved
bindings, and executable fields, constructors, and methods remain MIR-owned.

The C++ representation preserves GTI's distinct unsigned eight-bit `char` as
`char8_t`, while `uint8_t` uses `std::uint8_t`. Collapsing both to the latter
would merge distinct generic-owner member specializations in one translation
unit. Character literals, checked string-view reads, and character storage use
the same representation; text views inspect its bytes through C++ `char`.
This requires no new source meaning or C ABI: GTI already distinguishes these
types and excludes `char` from the public C ABI.

Scalar unique allocation uses a status-returning native allocation helper
selected from the verified MIR instruction. It performs the exact zero/one
scalar initialization and publishes only on success. The instruction's exact
allocation outcome produces a bounded record and calls the allocation-free
abort entry point; exhaustion has no cleanup/containment edge. Its native
nothrow new-expression permits process allocation/release elision under ADR
022's revised policy. This does not permit deleting application resource
calls. The helper contains no constructor dispatch or source-level selection.
Selected ordinary class construction uses MIR's separate `AllocateObject`
resource and exact constructor call. Raw storage has no live pointee cleanup;
its MIR failure Drop only frees bytes. Prepared owning class arguments occupy
lifetime slots before allocation; exhaustion aborts without destroying them.
A successful
constructor publishes the owner, while a failed constructor retains its
original record and follows the explicit raw-storage cleanup edge. The
existing constructor rollback proof remains mandatory. Targetless generated
default/copy/move construction uses the same explicit storage protocol; a
generated default requiring a field/base schedule must retain its exact
constructor target. There is no fallback native `make_unique` execution path.
The allocation intrinsic consumes its exact prepared borrows through the same
pending-input/success-chain proof as ordinary calls. An abort-only allocation
can share a block with subsequent owner publication; its already consumed
inputs must not remain pending across that publication. No arbitrary intervening
effects or new argument lifetimes are admitted by this bookkeeping.

## Opt-in Source Debug Locations

Source-level debugging is a statement-line first slice, not a GTI debugger or
a complete source-level type/variable description. `CompilationRequest` can
ask the frontend-aware `LoweredProgramBuilder` to capture parser-recorded
statement spans against the **optimized** MIR's retained HIR identities.
Each `LoweredBody::sourceLocations` row owns its source path/span and exact
block/instruction identity; instruction zero identifies a terminator. These
optional rows are verified and sealed with the lowered contract (current printer
version 24; debug rows were introduced in version 13), survive destruction of
AST/HIR, and are never executable,
ownership, type, or optimization authority. The default contract has no rows.

The C++ backend consumes only those lowered rows. Body/template planning emits
escaped standard `#line` directives around admitted source operations and
returns completed text to the formatting-only final writer. Every physical
line in a mapped multi-line expansion repeats its statement-start line;
standalone generated ownership/failure transport, cleanup, adapters, and
unmapped operations reset to `<gti-generated>` rather than inventing GTI source
lines.
Nested lambda and retained generic body plans consume the same owned metadata.
Native Clang/GCC debug information, not a GTI-specific DWARF writer, maps the
resulting machine instructions to those locations.

File-and-line breakpoints and source display are the supported outcome at
`-O0`. Native stepping uses these mappings but can still stop in generated
glue. Initializer-only expressions without recorded statement extents may
remain unmapped. Optimizations can fold, merge, inline, or remove statements.
Several instructions may share one line; columns, native frames,
local names/types, expression evaluation, generated prologue/cleanup stepping,
and GTI function-name breakpoints still reflect the C++ representation. There
is no promise that every source line is executable. Precise expression spans,
GTI-named variables/types, relocatable source paths, and synthetic-frame hiding
remain later slices; this metadata does not bypass MIR verification.

## Backend Contract

Terminal and caller-tracking contracts are sealed declaration facts, also
checked against the embedded MIR. The current lowered-program inspection format
is version 24 and retains both flags and optional source debug locations.
The C++ representation
passes an explicit artifact-local caller-site parameter through supported
tracked signatures and calls. An
untracked caller supplies the verified call-site constant; a tracked caller
forwards its incoming parameter. The backend does not consult source spellings
or recover caller policy from a C++ function name.

The private panic instruction produces a version-two failure record and copies
its bounded text payload before following MIR cleanup. Its successful path is
unreachable; ordinary failure propagation, hosted containment and managed-thread
handoff retain the complete record. The artifact descriptor remains version one,
and the separately preserved runtime version-one interfaces are not silently
reinterpreted as larger records.

`include/gti/backend.h` defines one executable-backend operation:

```cpp
BackendArtifact generate(LoweredProgram program);
```

Backend policy is explicit state of the selected backend. For example,
`CppBackend` receives the requested C++20 or C++23 policy in its constructor.
A backend returns an in-memory `BackendArtifact`; file publication, native
compiler invocation, process handling, and final artifact replacement belong
to the reusable driver.

`BackendInput` no longer exists. `CppBackend`, `MirBackend`, and
`NativeHeaderBackend` all verify and consume only `LoweredProgram`.

Callable and failure-boundary adapters transfer their already-owned by-value
arguments into the selected MIR-backed function with native moves; reference
arguments remain references. Internal transport slots are not made const merely
because the source parameter is read-only. This representation glue must not
introduce an extra copy or delegate argument cleanup policy to native C++.

Failure-form functions, methods and lambdas can publish raw-pointer results
through an exact typed out-parameter, just like other passive values. The
result retains its pointee identity and access; a native call from one opaque
handle type to another does not require those types to coincide. Null returns
and forwarded pointer results use the same channel. Failure publishes no result
and follows the caller's verified cleanup edge. This does not extend the native
ABI or permit converting between distinct opaque handle types.

### Raw Address Representation

An `AddressOf` result with no uses in the verified body's use table needs no
SSA storage. The C++ backend still evaluates its address expression and emits
every checked projection and failure successor in MIR order, but discards the
result rather than assigning it to a fabricated carrier. Nested native-array
indexing uses this rule: later element places already contain the complete
projection path, so an unused C row address must not be stored as a pointer to
an ordinary GTI `std::array`. This is a representation-only repair, not dead
instruction removal or a native-array cast. Used address values retain their
existing typed storage, and checked element borrows retain their loans.

Ordinary GTI raw-pointer values use an artifact-private union
`raw_address<T>` with one live native `T*` member. Its implicit union
copy/move operations copy the object representation, rather than evaluating
that member ([C++ union copy construction](https://eel.is/c++draft/class.copy.ctor#16)).
Qualification, erasure/restoration, native-field capture and assignment use
reference-based byte transfer; equality reads address bits by byte copy.
These operations remain inert after a pointee expires. Native pointer values
are evaluated only for verified unsafe access/arithmetic or an explicit
foreign boundary, as a typed load of the live `T*` member. An integer round
trip through the address bits would hide that pointer's provenance and type
from native alias analysis; it kept a checked element loop over a source
sequence from vectorizing. Optional provenance auditing does not make lifetime proof
mandatory for unsafe code. Passing one runtime test is not the justification
for representation-preserving transport.

Verified raw access/arithmetic and native-call boundaries recover a native
pointer. The carrier does not add source integer conversions, lifetime checks,
ownership, or aliasing permission. The supported representation requires a
flat 64-bit little-endian object-pointer ABI with all-zero null representation
on the supported hosted arm64/x86_64 targets. Architecture, size and alignment
guards reject other native compilation profiles; capability-pointer targets
are explicitly rejected. Equal pointer/integer size alone is not the authority
for treating arbitrary C++ pointer representations as integer addresses.

Native declarations, callbacks and C-compatible fields retain native pointer
types. Addressing an ordinary pointer carrier addresses its actual native
member, so a nested raw address can be stored, forwarded, returned and passed
to C without contextual out-parameter staging. Repeated addresses retain real
slot identity and writes are immediately visible through all aliases.
Foreign retention is permitted only while the unsafe caller maintains the
slot's lifetime and access contract.

Ordinary fixed arrays of raw pointers use a private union around a real
`std::array<T*, N>`. This provides actual adjacent pointer objects for C/GTI
raw indexing and arithmetic; adjacent members of separate carrier unions
would not constitute one native pointer array. Union transport preserves bytes
for the whole array without evaluating possibly expired elements. MIR still
owns source bounds checks, place identity and array initialization/cleanup.
A checked reference to a raw pointer names a `raw_address<T>` slot. Semantics
rejects one to a native `T*` slot (a fixed pointer-array element or a native
record or union field), so the backend never aliases a native `T*` as a
`raw_address<T>&`.

Failure-free passive C-compatible call results share one consumer proof for
bare assignment, a single enum payload field, discard, and unchanged return.
The exact record/type/signature
must agree, no result cleanup obligation may be invented, and return permits
only the existing passive cleanup tail. A return writes caller-provided result
storage at its Call; discard still executes the foreign call. Assignment and
discard use byte-preserving representation transfer rather than native
memberwise pointer reads. Neither source default construction nor owning-result
transport is inferred from this passive representation rule.
The payload consumer additionally proves the exact typed alternative/field,
sole SSA use without value-rooted storage, and producer dominance/order. The
call executes into its passive representation at its original MIR position;
later payload operands cannot reorder it. Missing or duplicated uses, changed
tags/types, and newly fallible payload publication fail representation admission.

A consuming payload extraction, a `Move` of a switch-owned subject binding,
spells its field as an rvalue at the binding's construction. The extraction
that carries the subject's `TransferOut` then ends the drained enum's C++
lifetime without GTI cleanup; its moved-from fields are already disarmed. MIR's
operand kind and lifecycle event select both spellings; the backend does not
infer consumption from the subject's representation.

A cleanup-free value whose folded traits forbid copying, such as a payload enum
whose alternative holds a move-only class without a destructor, has no lifetime
slot: it lives in a plain local. When MIR records exactly one executable use of
that SSA value, the use consumes it, and the backend spells the local as an
rvalue there. The use may be a by-value call stage, the invocation operand that
receives the stage, an aggregate element, a store, a join argument or a
failure-form result publication. A plain `return` of a local of the result type
keeps the plain spelling, which C++ already moves and may elide. With more than
one use the local keeps its plain spelling, which the native compiler rejects
rather than copying. The rule comes from MIR's value traits and use list, not
from the representation, and copyable passive values keep their copy spelling.

An observing switch whose subject is an enum held by a fixed-array element
read-borrows that element in place. The element may belong to a local, a
global or an array field, possibly through nested arrays, or the enum may be a
field of an element at a constant index. MIR first finishes the element's
checked selection at an `AddressOf` whose `Invoke` reports the bound, and
program verification requires the check of every index in the subject place
to dominate the switch. The backend spells the discriminant, every borrowed
payload binding and every copied payload field over that one element
subscription, without copying the enum or checking the index again. Admission
and the text step accept these places through one shared predicate. A field
of an element at a runtime index, as in `records[i].state`, is spelled once
MIR has checked that element's selection at its own `AddressOf`, which program
verification requires to dominate every access through the field.

The same predicate admits any read or store of a field below fixed-array
elements, such as `points[i].x` or `cells[i][j].start.y`, when every index is
a compile-time in-bounds constant or a run-time index whose checked selection
MIR completed at an `AddressOf` over the place prefix ending at that index.
The text spells plain subscripts and the field chain without a second check;
removing the checked selection refuses the body.

Ordinary field chains projected from a checked class-payload extraction share
that extraction's address. Admission checks every typed field projection;
local declaration planning and operand emission use the same view predicate.
No field gets a second default-constructed object, even when its class has a
default constructor: that would invent construction, state and cleanup absent
from MIR. Nested fields and non-default-constructible owning fields follow the
same rule.

Results from consuming source members use the ordinary owning-call result
staging and replacement proof. The exact `Once` receiver must resolve to its
selected consuming member and materialized receiver place. Receiver cleanup
remains at the recorded full-expression/failure Drop; it is not an argument
transfer at Call. A class receiver moved out of a place without a Value
obligation of its own, that is a cleanup-free class, is the exception. MIR
stages it as a prepared parameter on a mutable temporary: the receiver's
`MoveValue` CallInput activates that obligation and the Call transfers it to
the callee, exactly like an owning argument's. The receiver proof names the
prepared slot as the consumed object, whatever the access of the moved-from
binding, and the call spells the moved source in its place as it does for a
staged by-value argument. When a later argument can fail, as in
`std::move(maker).make(1 / d)`, MIR's failure edge drops that prepared
obligation. The receiver then occupies its prepared lifetime slot from the
stage onward, like a by-value argument before a fallible one: the call
consumes `std::move(<slot>)` and retires the slot, and the failure edge
destroys the still-engaged slot once, without running the member. The receiver
proof (`preparedReceiverStage`) admits exactly those failure-cleanup Drops as
the slot's other users, so each result proof below covers this form unchanged.
The value-rooted result slot of a discarded,
replacing or field-read result already counts every prepared input. Other
result proofs that count prepared argument transfers count this handoff too
where their schedule admits it: the returned-result slot and
the conditional temporary of a result made in a conditional arm or
short-circuit operand, whether a failure-free or checked call constructs it
there for field views or publishes it into a conditional join. Forwarding a
cleanup-free result unchanged to the Return, as direct placement into the
caller's storage does, does not admit it; such a call keeps its staged result
slot. Only a successful result activates the replacement stage, so
receiver failure cannot overwrite the old destination, and failed replacement
cleanup destroys the unpublished result without committing it.

A construction whose only use is the Return publishes inline at the Return
unless the failure form can fail in between. When an Invoke or a
defined-failure source lies on a path from the construction to that Return,
as fallible scope-exit destructors do after a failure-free `expected`
construction (RT-12), the failure form builds the result in the out-parameter
at its MIR position, as it does for a result with a `ReturnResult`
obligation. Its staged inputs are then consumed where MIR consumes them, so a
later failure finds no input slot still live; a result with no obligation is
passive and needs no cleanup on that path.

A member or closure receiver moved out of a named binding materializes at its
`Move`: the backend constructs the receiver's lifetime slot from the binding
there, and the Receiver `MoveValue` input and the call only alias that slot.
Local declaration planning gives neither SSA identity storage of its own. The
receiver stage is not a copy stage. A consuming member is invoked on the slot
as an rvalue; a selected read-only or mutable fallback is invoked on it as an
lvalue. When the full expression has no internal merge, the slot is the moved
value's own Value-rooted place. Inside one arm of a conditional expression or a
short-circuit operand, MIR instead names as the Move's destination the hidden
temporary that carries the
receiver's full-expression obligation through the merge, and the Drops after
the merge are conditional on the arm that ran. That temporary is the receiver's
slot only when the Move activates its unique Value obligation, the input and
invocation identities match exactly, nothing else writes or reads it, and only
Drops of that obligation end it, at least one of them on the ordinary path. A
conditional Drop destroys the slot only if its arm constructed it. A failing
receiver destructor leaves the published result and the remaining cleanup to
MIR's failure edge.

Pointer-bearing passive C-compatible records use byte-preserving construction
and assignment internally. Verified source calls snapshot value arguments
before passing them by internal reference; result publication uses a typed
output channel. These are representation choices, not new source borrows or
failure edges. External C signatures remain unchanged and retain their unsafe
caller obligations. Retained generic signatures use their exact MIR instances
to select the corresponding transport; they must not revert a concrete native
record to an ordinary C++ by-value copy merely because its projected spelling
is a type parameter.

For a retained nonvirtual class member, a class-bound type parameter (or fixed
array of it) selects the internal reference transport through the same private
byte-transport trait as its field representation. This does not interfere with
function-template argument deduction. Class-owned packs apply the same choice
per element; checked-reference elements retain their exact access-qualified
reference type and do not instantiate the object-pointer trait on a reference.
Ordinary versus native-record result
families have constrained signatures; each emitted body still comes from an
exact MIR instance group whose projected bodies agree. The status-returning
failure channel remains separate from this result representation.

A retained class-bound virtual result uses one coherent result-channel
convention across its resolved virtual-slot family. The representation query
follows lowered virtual-root identities, including nongeneric overrides of a
generic base. An explicit internal output parameter is present throughout the
family: native-record results return `void` and publish bytes to that parameter;
ordinary scalar and owning results retain their original return and cleanup
convention, passing `nullptr` for the unused channel. Calls take this choice
from the sealed representation snapshot; they do not rely on C++ default
arguments, including when a generic parameter pack becomes concrete. The C++
return spelling is conditional only while the owner type is
symbolic. Virtual methods therefore need no constrained overloads or native
by-value fallback. Verified concrete MIR still selects every emitted body;
the extra parameter is backend calling-convention glue, not a source argument
or new failure edge.

Generated field, capture and container representations preserve the same
property without changing MIR's active-alternative or cleanup schedule. The
native-record container matrix covers payload-enum alternatives, native arrays,
owning alternatives, captures and generic relays at C++20/C++23 O0/O1/O3,
disabled elision and ASan/UBSan. The broad completion and installed evidence is
recorded in [verification](verification.md#low-level-memory-phase-1).
Unsupported representation families must fail closed, never silently fall back
to memberwise expired-pointer copying. The ordinary address carrier alone does
not discharge those composite transport requirements.

The address representation guarantee is distinct from the unsafe obligation to
pass a valid pointer to a callee that accesses it. The optional raw auditor can
prove inert transport through direct/lambda targets and bounded virtual
families; an owning erased interface whose generic target set is unknown can
remain incomplete even when one initializer looks inert. Warn reports that
uncertainty without blocking compilation; Strict promotes audit findings to
errors. The default Off policy requires no such interprocedural raw-validity
proof. Native ABI tests use live payloads, and explicit audit negatives cover
expired payloads. Improving erased-target auditing requires general effect/
target tracking through ownership, replacement and cloning, not a backend or
public-library-name permission. Mandatory checked loans, type/access rules and
unsafe caller obligations apply in every audit mode.

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

Generic requirement leaves have explicit `Concept`, `Member`, `Expression`,
or `Predicate` kinds. A concept keeps its declaration identity; an exact member
keeps its actual name and receiver-access facts. A general expression instead
owns ordered synthetic-parameter types, its exact owned result type and any
explicit expression type arguments, with a checked result index separating
those groups. Predicate leaves retain their resolved type arguments and source
provenance, not a fabricated member name. These are static declaration facts:
the lowered value contains no source expression or AST pointer, and a backend
must not evaluate predicates or reconstruct expression validity. Semantics
checks admissibility and concrete instantiation before this boundary.

`verifyLoweredProgram` rejects missing, duplicate, stale, reordered, unrooted,
or cyclic records and an invalid construction seal. `LoweredProgramPrinter`
provides deterministic full text for comparison and independent tooling.

The builder runs every inventory check and seals the value only if they all
pass; it never re-checks a seal it has just computed. Each consumer verifies
the complete inventory and seal once. `verifiedLoweredProgram` returns a
`VerifiedLoweredProgram`, owning evidence about that exact immutable value.
A seal match alone is insufficient: the deterministic inspection print is not
an exhaustive serialization of every verified invariant. For example, changing
the independent program kind without its embedding plan must be rejected even
if the print is unchanged. The C++, MIR, and native-header backends all use
this admission boundary. `CppBackend` hands the
evidence to the emitter, emission planner, and representation snapshot, so no
stage checks again. The `LoweredProgram` overloads of those stages still check
first, so a direct or test caller cannot bypass rejection. `verifyLoweredProgram`
is also available for independent tooling and tests. The historical PR #176 audit
therefore retains streamed hashing and shared evidence, but rejects its
seal-only admission shortcut.

Evidence owns a const snapshot, shared between consumer stages. Acquiring it
copies an lvalue program or moves an rvalue program. `Backend::generate`
accepts a value, and the driver moves its finished program into this boundary;
C++ emission moves that value into the proof without another full-program
copy. Callers that need to retain their input can still pass an lvalue. Later
assignment, movement or destruction of the caller's original value cannot
detach the proof from the checked contents. Merely holding a const pointer to
a caller-owned program would not establish this, because `LoweredProgram`
itself remains an assignable value.

The seal is a 64-bit hash of the deterministic print. Both printers write to a
stream, and the seal hashes the text as it arrives, eight bytes at a time,
without materializing it; a large program's print is around 100 MB. Seals
are compared only with seals of the same compiler, so the hash is not a stable
external format.

Lookups by class, function, constructor, storage, symbol, enum, or generic
parameter identity use row hints built at construction. A hint is used only
after its row is confirmed to still carry that identity; otherwise the lookup
scans, so mutated test values keep their exact behavior. The hints are derived
data and are not part of the printed contract.

Several C++ representation facts depend on the whole program rather than on
the type being spelled:

- the closure of classes that keep a native template representation;
- whether any lexical closure forces the shared template carrier;
- whether a class supports direct representation, which depends on its
  member rows;
- the conditional class-return arms of a MIR body.

`CppRepresentationScope` is opened by the emission planner and the
representation snapshot once they hold verification evidence. While it is
open on a thread, queries about that exact program compute each such fact
once. A nested stage of the same consumer reuses the open scope. Queries about
any other value, and every query outside a scope, compute the same answers
directly.

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

For a MIR polymorphic unique-owner cleanup leaf, C++ uses a private virtual
status-returning hook. Successful typed allocation binds the exact allocated
MIR class instance's complete cleanup recipe before publishing its owner;
upcasting preserves that selection. Each represented polymorphic class
subobject carries a function-pointer cleanup binding. This metadata belongs to
the allocation identity, not the GTI value: native copy/move construction starts
unbound, while assignment preserves the destination binding. The allocation
path binds newly constructed complete objects, including the default-constructor
allocation helper as well as explicit MIR constructor allocation. Static base/field destruction
continues to use the statically selected MIR recipe. The dynamic recipe drains
remaining cleanup after a first failure and retains both failure records when
cleanup fails during unwinding; native destruction does not choose that policy.

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
selected C++ standard and exact storage, managed-thread, C-thread,
atomic, mutex, SIMD, defined-failure, runtime-binding, and C-ABI text-view
needs. The preamble and
failure-support writers receive those explicit values and an output sink;
they do not query program meaning. Storage and defined-failure support remain
universal only because that is the current artifact contract, not because the
writers infer their use.

Capability support has two levels. A lowered class declaration whose
compiler capability kind is shared owner, atomic storage, mutex storage, or
mutex lock token selects only a forward declaration of the backend class
template that represents it. Declarations retained without an instance, such
as the generic library classes left by an unused `#include`, still emit
uninstantiated C++ class templates whose members spell those qualified
template names, and C++ resolves a qualified template name where the
template is defined. The complete definitions and their native headers
(`<atomic>`, `<mutex>`) require a concrete MIR capability instance or a
verified capability operation. The managed-thread handle representation is
not a template, so a retained thread-handle declaration selects its complete
support. `dormant_capability_runtime` builds and runs each family's
include-only program natively.

Scoped groups select the same managed-thread support from the verified
`ThreadScope` operation. The body plan retains its ordered adapter identities,
consumed task operands and task-join failure channel. Generated closure roots
and detached-program verification match each adapter to its exact group and
operand, not merely the enclosing function. This permits several heterogeneous
lambda tasks in one concrete generic body without reusing the owned-spawn
single-task template path. The preamble implements stack task slots, join-all
and ordinary verified cleanup as described in the
[runtime contract](standard-library-and-runtime.md#scoped-worker-execution).
Provenance-only projected pack places retain exact type/field rows in admission
but do not form unused C++ accesses to private carrier fields.

### Executable Bodies

Owned replacement recognizes an implicit receiver's complete field path:
`field.child` (Symbol plus projections) is normalized to the same checked
representation path as `this.field.child`. The root must resolve to a field of
the concrete enclosing instance, and every suffix field retains its exact
identity, mutable access and representation row. Static/module symbols and
non-field projections gain no receiver privileges. Constructor rollback and
RHS staging/transfer/cleanup schedules are unchanged; normalization introduces
no runtime operation or additional allocation.

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

An ordinary by-value parameter is already an owned incoming value. Its exact
MIR binding traits select native movement when it is movable but noncopyable,
including cleanup-free wrappers and borrowed carriers. This transport does not
create another semantic move, loan or cleanup obligation. Reference formals
and byte-preserving indirect native-record arguments retain their separate
representation contracts; no public owner or range name selects the prologue.

Admission queries reject structurally inapplicable values before performing
body-wide ownership searches or instruction lookups. In particular, producer
probes require the existing class/category/lifetime conditions, and
payload-enum transport metadata requires enum-typed storage. These are
necessary conditions of the existing exact proofs, not replacement proofs or
cached admission decisions. This keeps unrelated scalar aggregate elements
from repeatedly triggering whole-body searches; applicable values still pass
the complete lifetime, publication, use, and failure-edge checks. Neither MIR
semantics nor emitted C++ changes as a result of these early rejections.

The same necessary-condition filtering applies to direct class temporary
borrows: scalar values cannot be their producers, and slot candidates must
match the existing mutable class slot's exact source-value identity and type
before call, materialization, activation and cleanup searches run. A terminal
subscript boundary query first requires an existing source-defined GTI
subscript member with an owner and one index parameter; an unrelated function
cannot be selected by the existing whole-program subscript proof. These
filters do not memoize admission, discharge a safety check, or change the
complete proof for any potentially applicable candidate.

Admission and text planning must also cost time proportional to a body's
length, not its square. Verified MIR is immutable for the duration of one
`CppMirBodyEmitter` operation, so each public operation opens a body lookup
scope. Within it, each body is indexed once, on first use: instructions by
id, dominance, the instructions and terminators that name each place or drop
obligation, Invoke terminators by paired instruction, instructions by result
or receiver value, value-rooted places and drop obligations, and, for a
recognizer that publishes values into places, the values grouped by the place
each one targets. Recognizer queries consult those indexes instead of
rescanning the body for every value, place or instruction they examine. An
index is derived only from the body it describes and narrows candidates
without deciding admission: every candidate still passes the complete
original proof, and each answer equals the scan it replaces. A recognizer
reentered while its own index is still being built scans instead, and code
outside a scope keeps scanning. A single `main` of about 1,100 formatting
statements, which previously needed more than 18 CPU-minutes, now plans in
about the time the same statements take when split into small functions;
`single_body_compile_scaling` guards that growth by comparing the compiler's
CPU time for one repeated statement block at two body lengths.

Two recognizers whose complete proof is itself a body walk also remember their
answers in the same per-body index, because admission, plan construction and
text emission ask them about the same values and drops. `returnMoveDefinition`
records, per queried value, the Move that may publish a class-valued return
directly; its reachability and dominance walks look blocks up by position,
which verified MIR makes exact (`verifyMirBody` requires `blocks[i].id == i +
1`), and one walk from the Move's block serves both the same-block and the
cross-block form. `failureDestructorTarget` records, per drop obligation,
whether the drop's recipe equals `mirUniqueCleanupType` of its type, a
derivation that walks the program's class instances. Both answers depend only
on the immutable verified body and program, so each equals the computation it
replaces; a body outside a representation scope still computes every query.
On `tests/fixtures/stdlib_surface_json.gti`, whose single body emits about 34
MB of C++, these two recognizers had been about three quarters of the whole
`--emit-cpp` compile.

Each body plan owns its readiness and issues, canonical plain and failure text
templates, the deduced-callable variants where target family spelling differs,
boundary-declaration decision, initializer schedule and field spellings, and
hosted-startup kind. The plan is pointer-free; it outlives both the MIR program
and copied representation map used to build it. Placeholder replacement for
family labels and indentation is therefore formatting, not a second
admission path.

Whole-program admission validates MIR and the copied representation map once
per `analyzeProgram`/`buildProgramPlan` operation. The separate
`analyzeProgramStructure` operation batches the same structural results and
canonical body order as standalone `analyze(address)`, including hosted startup,
with one fresh whole-program validation. Structural readiness is not complete
text admission: field-initializer bodies can be structurally ready while outside
the text vocabulary. Production preflight continues to use `analyzeProgram`,
and production emission continues to require a sealed `buildProgramPlan`.
Global MIR/map failure returns no independently ready body results; missing
body-local representation rows retain their exact structural issues.
Initializer vocabulary probes
and plan assembly reuse that operation's body analysis; nested base-constructor
probes use a private body-local vocabulary helper, not the standalone validating
entry point. Invalid global input stops before body plans are constructed.
Standalone `analyze`, `buildPlan`, and `initializerSchedule` still validate their
inputs on every call. No validation cache survives an operation or is keyed only
by a program address. Optional caller-owned metrics count actual whole-program
verifications for deterministic regression checks without changing admission.
The emitter corpus uses one structural batch for its readiness sweep and one
immutable program plan for its production/general-text agreement sweep rather
than revalidating the whole program separately for each body. Exact standalone
result/text comparisons and global/body-local forgery tests guard this reuse;
there is no persistent certificate or change to the admission predicate.

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

Slot cleanup reuses one private captureless native adapter per body emission
for each equal complete `MirDropType` recipe and sealed native slot-type
spelling. The recipe, including alternative/subobject identities and order,
remains MIR-owned; equal rendered expressions alone do not justify sharing.
Adapters are collected only by the existing admitted Drop routes and inserted
once into the body prelude before all labels. Each original Drop retains its
slot, failure record, conditional and unwinding arguments, status and Invoke;
all failure and second-failure termination edges remain distinct. This shares
native template identities, not GTI callable values, lifetimes or control flow.
The inventory resets for every body and plain/failure/deduced representation
variant and never escapes its immutable program/map snapshot. Pointer-free
body plans retain the finished declarations and calls as ordinary text.

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

The native destructor's engaged semantic-slot check remains mandatory. Its
missing-`Drop` diagnostic and cleanup-emergency handoff query live in a cold
out-of-line helper, keeping that exceptional path out of ordinary wrapper
inlining cost. Empty-slot destruction, explicit retirement/access checks and
representation-only destructor behavior are unchanged; this is not permission
to omit a MIR cleanup operation or infer that a checked failure cannot occur.

Small source functions and constructors receive an early native inline hint
in optimized GNU/Clang builds. The structural policy admits at most 64 reachable
instructions and 64 blocks per body, 256 instructions across the exact expanded
invocation graph, and 16 invocation levels. It rejects CFG/call cycles,
allocation, synchronization, native/dynamic invocation boundaries, virtual
methods and program entries. A shared generic native definition is hinted only
when every represented concrete instance qualifies. This is a bounded native
optimization preference, not a source guarantee or permission to elide checks;
larger bodies retain the native optimizer's normal discretion. It applies to
ordinary user-defined wrappers without recognizing a library name or return
type, and makes no change to GTI failure, lifetime or cleanup authority.

Grouping a class value does not construct another object. HIR records the
grouping source as a non-executing alias of the exact producer value, preserving
its type, source identity and lifetime rather than introducing another value or
drop obligation. The alias verifier rejects erased producers, extra executable
grouping values and inconsistent source/type identities before MIR lowering.
MIR and the backend consume the original constructor, call or conditional-result
move;
parentheses do not create class `Identity` instructions or another publication.

Separately, the backend's exact MIR `Identity`-link publication classifier can
follow an existing nonborrowed class value to one binding destination slot.
This representation proof is not the lowering rule for source grouping.
Each link must have the same complete expression facts, a unique executable
consumer, and either no value obligations or the exact same-type `Reparent`.
Links are adjacent instructions or the first instruction on a unique Invoke
success edge; the last link is adjacent to binding initialization. MIR source
identities, failure edges, and lifecycle events remain present and execute in
order. This binding-publication proof does not by itself cover borrowed carriers
or other consumer shapes.

The same identity-link proof also composes with the existing prepared-argument
slot protocol. It distinguishes the original call/constructor producer from the
terminal identity-linked value consumed by `CallInput`, validates every
intervening drop transfer back to the producer's activation, and requires
adjacency to the argument stage. Admission and computation emission use the same
publication query. The prepared argument remains live through later argument
evaluation; its existing transfer and failure-cleanup schedule is unchanged.
This does not admit arbitrary moves as constructor producers. An exact nonborrowed temporary
Move may instead publish into its prepared argument slot: its source and target
are distinct typed places, intervening identities are adjacent, and either the
source's exact obligation reaches the prepared parameter by reparenting or both
source and payload are cleanup-free. The original native shell is retired after
the move. The materialized-input fallback does not construct the argument again,
and incomplete temporary transfer schedules fail closed instead of falling
back to deferred copying at the call.

Local class-valued conditional publication uses one shared uninitialized
temporary followed by the MIR move into its binding. The exact two-arm CFG,
unique storage use, publication order and terminal representation teardown are
shared between cleanup-owning and passive classes. Owning values must retain
their exact activation/reparent obligations. Passive values instead require a
generated passive destructor, no borrowed state and a generated move proved
defined-failure-free by MIR type facts; no value or temporary cleanup obligation
is synthesized. Both modes reject additional temporary aliases, readers or
writers. The native moved-from shell is retired after the binding construction,
including when the same CFG executes again through a loop.
Transparent identity links between the conditional move and binding publication
retain the same storage and exact drop-reparent chain. Moved-from shell retirement
traces that chain back to the original move's obligation; comparing only the
final binding's immediate source obligation would miss retirement through
those links and leave the slot engaged on loop re-entry. This retirement uses
the already-validated identity links and does not execute an additional GTI
destructor.
The same conditional join may end in the exact prepared argument stage. A
receiver staged before that diamond keeps its stable address while the arms
construct and transfer their argument. The cross-branch proof admits only the
matched arm publications, move/identity chain and argument stage, with primitive
constructor inputs. A later source call producing a primitive argument is also
permitted when its exact result stage feeds that same consuming call. Raw
memory, pointers, borrowed carriers and unrelated stores are not admitted by
this extension. Calls and their failure edges retain their original MIR order.
The passive proof uses the class's generated destructor and active-cleanup
facts, not `DropKind::Trivial`: a nominal class can retain a lexical lifetime
without requiring an active cleanup operation. A constructor `Invoke` may
separate an arm from publication only when its unique success block contains
that publication followed only by verified `EndBorrow` instructions. These
loan endpoints let a fallible source call retire its borrowed inputs after
publishing the arm's class result, without computing another value or altering
the result. The selected status constructor writes the slot at the original
Invoke and engages it only on success; its original failure edge and cleanup
remain intact. Ordinary constructor-boundary admission still verifies
the selected constructor. The shared class-publication query is used by both
transformed constructor admission and emission; recognizing storage in the
prologue alone is insufficient.
A passive arm may also be a static source call, publishing plainly or on its
Invoke success edge, a callable value invocation (a closure literal, a stored
closure or a callable parameter), which publishes on its Invoke success edge
like a checked source call, a copy of a named place, or the move out of a
nested passive join. A member call arm keeps the receiver identity that ordinary call
admission proves: its exact read-only or mutable borrow
(`exactMemberBorrowReceiver`), or a consumed receiver
(`exactMemberConsumingReceiver`), where a cleanup-free receiver moved out of a
place is staged as a prepared input. A call arm may also consume prepared
inputs, as in `Pod t = c ? std::move(a).pod() : make(std::move(b));`. The
passive result has no obligation, so the arm call's lifecycle must be exactly
one unconditional `TransferOut` per prepared input, the schedule
`exactPreparedInputTransfers` proves, and no activation of its own
(`onlyPreparedInputTransfers`). A construction arm may likewise hand by-value
arguments to its constructor, as in `(c ? seg(pt(1)) : seg(pt(2))).flat`; its
lifecycle is then exactly those transfers. Virtual calls, and constructions
with any other lifecycle event, stay outside this form. An arm may also move
a whole named local out,
as in `Pod t = c ? std::move(a) : std::move(b);`. The passive class has no
obligation to hand over, so MIR gives that `Move` no lifecycle event, only the
ownership event that takes the local's key to `Moved`, and the local keeps its
passive binding drop. That drop ends the moved-from shell after the join on
the path that moved it, and the live object on the other. A nested join is
proved on its own, and the nested arm's region starts at that inner join's
branch block. A passive arm may also evaluate its inputs before that entry, as
in `c ? f(a + 1) : f(b + 2)` with overflow-checked additions, a call that can
fail, a `?` operand, or scalar control flow inside an argument. Without an
exact diamond, the branch is the
nearest common dominator of the two arm entries, and its two successors start
the arms' regions (`conditionalArmRegionBranch`). Each region is proved exactly
(`exactConditionalArmRegion`): the branch enters it once and nothing else
enters it or its arm's entry; its blocks are ordinary code dominated by the
region's start; and their ordinary edges stay inside or reach the entry. An
Invoke's failure edge leaves through its own failure record's cleanup and a
`Return` leaves the body (a propagated `?` residual), both before the arm
publishes anything. An owning join whose arms evaluate inputs first keeps the
ordinary owned-temporary publication it already has. The outer arm's move
out of a nested join is that join's single move, and the nested join's
representation ends right after it, so a loop that evaluates the conditional
again finds both storages empty. Slot retirement always clears engagement and
preserves the empty-slot guards. For a trivially destructible C++ representation,
retirement need not invoke a destructor: its physical lifetime ends on storage
reuse or release. This avoids an unnecessary native lifetime-end barrier while
MIR still forbids subsequent use of the retired value. Nontrivial representations
still run their destructor at retirement, including cleanup-bearing source
objects and representation-only shells. When a passive join's single move reaches
neither the binding nor the prepared argument terminal, the moved value is
staged in its own representation-only slot and the join's representation ends
right after. That staged slot then serves a return, field read, assignment or
field initializer exactly as a fresh result's slot does. In the failure form
too, a Return of that staged value publishes the caller's result from the
slot; the staging Move does not publish it.
A member call on the joined value itself, as in `(c ? a() : b()).read()`,
has a different MIR shape. MIR gives the join temporary a full-expression
value obligation, stages the temporary as the call's receiver (a read-only
or mutable temporary borrow; a consuming member's receiver is staged as the
mutable one), and drops it at the end of the full expression. The join's Move is then only the temporary's materialization and
has no use. Each arm's `Initialize` activates the temporary's obligation
directly, because a cleanup-free arm result has none of its own. A
construction arm already constructed the temporary at that `Initialize`.
`classValueDestinationSlot` also names the temporary for a call or ordinary
constructor arm with no lifecycle event beyond its exact prepared input
transfers, for a copied place, and for a whole named local moved out by the
same ownership and passive-drop facts as the arm above, when the `Initialize`
follows the producer directly or opens its Invoke success block. Each
constructs the temporary directly, a call that can fail only on success.
Constructor admission requires its complete input schedule; input transfers
do not substitute for the temporary's real full-expression drop, which stays
inactive if construction fails. The receiver borrows it as
`directTemporaryReceiver` borrows a fresh result's slot. An arm may also move
out of a nested cleanup-free join, whose storage has no obligation either.
That move is the nested join's single move, and its writer ends the nested
storage right after constructing the temporary.
An owning join's synthetic temporary, once a move has emptied it, ends its
native shell where MIR hands its obligation away. In a plain body that staged
the returned value, that point is the return's transfer-out. Otherwise it is
the consuming `Initialize` that reparents the obligation into a field. A
temporary that MIR later drops keeps that drop.
An owning arm may also move out of a nested owning join, or move a named
local whose obligation MIR first moves into the arm value; the arm's
`Initialize` reparents that obligation into the join's. A call or construction
arm activates its result's own obligation, at the producer when it is
failure-free and on its Invoke success edge otherwise, and the arm's
`Initialize` reparents that obligation into the join's. A producer that
consumes prepared inputs (moved by-value arguments, or a cleanup-free receiver
moved out of a place) also transfers each one out at the call, as in
`Quiet t = c ? std::move(a).make() : make(std::move(b));`. The publication
proof admits exactly one unconditional `TransferOut` per prepared input beside
that activation, the schedule `exactPreparedInputTransfers` proves; any other
producer event rejects the arm. When the join's single
move is read only through field views, or consumed by one assignment whose
`Replace` takes the join's obligation, the moved value denotes the join
temporary. Views read `temporary.get().field`, the assignment move-assigns from
`temporary.get()`, and MIR's later Drop of the temporary ends the live object
or its moved-from shell. A returned owning join whose move leaves a pending
`ReturnResult` on the temporary, because later cleanup such as the moved
local's Drop can fail, keeps the result there until `Return`. Each arm
initializes the temporary once, the arms alone enter the move's block, and no
arm is reachable again after the move. `Return` publishes it with `take()`,
and a failure-cleanup edge destroys the unpublished temporary through MIR's
ordinary Drop.

A copy-loaded class result whose only executable use is `Return` also avoids an
invented default-constructed SSA object. The plain C++ form returns the exact
source place by value when the sealed representation row proves copy
construction; the transformed failure form placement-constructs that same
source into its out result. Multiple uses, a mismatched operand/place type, or
an absent copy-construction row reject body admission rather than selecting a
native fallback.

Passive generated-copy/move class assignments can publish into an exact field
chain rooted in a binding or the receiver, as well as a bare binding. The
emitter checks each concrete field identity and type, the final mutable field,
generated special members, single consumer, instruction order, and absence of
borrowed state or active cleanup. Copy loads, explicit moves, fresh constructors
and transformed call results retain their distinct MIR ownership proofs; field
assignment does not manufacture an intermediate owning lifetime. Index,
reference, union and base projections do not enter this field-chain transport.

A passive generated default used by a constructor field initializer preserves
its verified declaration defaults at the adjacent publication instruction,
using the existing private empty field shell and generated assignment protocol.
This includes a passive union with its exact overlapping layout, trivial fields,
and generated default/copy/move/destructor witnesses. Its targetless empty
construction and adjacent field publication need no owning cleanup token;
the containing constructor and fixed-array construction retain their original
MIR Invoke, success activation, transfer, and rollback schedules. A union with
active cleanup, user lifecycle, or mismatched field/layout metadata does not
enter this representation proof.
Read- and mutable-borrow `CallInput` results are representation aliases, not
native SSA objects. Ordinary calls and constructor publication resolve them
through the same exact MIR source-place spelling, including when another
argument transfers an owning or borrowed aggregate through prepared storage.

An immediately consumed by-value `CallInput` may copy a cleanup-free fixed-array
aggregate from its exact value-rooted slot. Generated element copying is
observation-free, while the source retains its original full-expression drop;
this is not a consuming transfer. Exact source/stage/parameter identities,
cleanup-free copyability, and same-block adjacent consumption authorize the
representation alias. Owning or borrowed elements, intervening argument work,
and a transfer of the retained source obligation do not enter this proof.
Targetless `Construct` and `Call` default producers can also publish into the
same exact prepared-parameter slot: omitted brace-default fields in designated
construction retain CallInput activation, consuming handoff and failure cleanup.
These are representation proofs over resolved MIR, not source-AST replay or
standard-library type exceptions.

Generated declaration-field schedules retain selected concrete constructor
bodies and the verified `generatedDefaultRequiresSchedule` obligation. A native
primary template may represent several such concrete owners: it declares its
ordinary default constructor once and a distinct status overload for each
selected MIR constructor. Out-of-line specializations execute those bodies;
an inline native default or `noexcept` body must not replace them. Direct
concrete representations retain their single exact-owner declaration.
The retained primary likewise declares a source-generic constructor's identical
native parameter list only once across concrete owners. This shares only the
declaration: each selected owner retains its own verified-MIR definition and
every fallible instance retains its distinct status-tag overload.

An otherwise unused reference-call SSA result identifies a referent; its paired
CallResult loan already supplies the physical pointer. The declaration loop
therefore emits no extra value object, irrespective of the referent type.
This includes payload-enum referents: default-constructing an
unused native value could either fail to compile or introduce a second object's
constructor/destructor effects. Selected source return types distinguish actual
references from by-value borrowed carriers; synthesized operator destinations
may carry their enclosing assignment's contextual value category. Intrinsic
reference results retain their place category and verified producer/loan shape.
Actual by-value carriers keep their existing storage and cleanup representation.

The passive default-body proof recurses through resolved, zero-argument generated
class defaults with cycle detection. It does not admit source calls, selected
user constructors, borrowed state, active cleanup, failure edges, or union/base
layouts. Thus a nested record's declaration defaults remain executable without
turning arbitrary constructor bodies into supposedly passive native initializers.
The same proof permits a targetless default producer in representation-only
assignment-result storage; construction still happens at its MIR instruction.

Source-constructor failure-tag calls placement-construct in the originating
member's lexical context using global placement `new`. They do not delegate
access checking to `std::construct_at`: that library template cannot call a
private constructor even when the originating GTI member can. The selected
MIR constructor, staged arguments, destination, success activation, and
rollback remain unchanged; source visibility is never widened.

Legacy empty-field admission for a containing owner of the sealed optional slot retains
the exact selected private constructor's instruction-free, nonborrowed void
`Return` witness. A recursive fields-only aggregate proof cannot substitute the
slot's shape for that witness after the selected constructor has been corrupted.
The promoted standard optional no longer instantiates this capability: its
state is an ordinary payload enum and uses the general constructor, active-tag,
assignment and cleanup proofs. Legacy capability implementation code is retained
but is not declared by `<std/optional>`.

The declaration-bound `fail_empty_owner` terminal operation takes no arguments,
never returns, and selects the existing `EmptyOwnerAccess`/`MemberAccess`
failure. Semantics establishes those facts; HIR preserves them and MIR checks
arity, termination, caller tracking and the exact failure identity independently.
The backend publishes that verified failure using ordinary failure cleanup. It
does not recognize `std::optional`, inspect its tag or grant access to its value.

Owning enum replacement is admitted from an exact paired cleanup recipe,
source slot and lifecycle transfer. It uses payload move construction even
when payload assignment is unavailable; borrowed-state assignment rules do not
change. After checked replacement cleanup, the backend retires the drained
native alternative and leaves a vacant shell until commit or lexical drop.
An enum nested in a containing class also retires its drained alternative on a
first cleanup failure, preventing the containing failure edge from repeating
child cleanup. Secondary-failure abandonment does not retire incompletely
drained storage. Failure-free movement and raw relocation eligibility remain
independent proofs, including for address-stable mutex storage.

The existing MIR double-failure contract also has an inherited dynamic component:
an ordinary source destructor or helper called by a failure-cleanup `Drop` cannot
treat its next failure as a fresh primary and run callee-local cleanup first.
Each failure-form body with primary cleanup reserves one backend-private scope
and activates it at each unique primary-record entry. MIR verification proves
that the complete
cleanup chain preserves that record until propagation or emergency termination;
it cannot rejoin ordinary execution. The invocation-local scope therefore spans
the chain rather than repeating TLS activation and native RAII cleanup at every
block. Every emitted
`Invoke` failure edge checks the inherited context after producing its exact
secondary record, before entering its local cleanup successor. Terminal source
constructor/callback/call/join adapters preserve the same check. No public type
spelling, reconstructed expression, allocation, or native source exception selects
this behavior; verified MIR active-failure and edge identities own the schedule.

Each native body invocation reserves at most one secondary cleanup record,
independent of the number of alternative cleanup paths. Every attempted
fallible Drop under an active primary zeroes that record in place before calling
its selected cleanup recipe. A failed attempt immediately checks the inherited
context and takes the verified emergency terminal path; no later Drop can
overwrite its secondary record. Successful attempts do not modify the primary.
Recursion and nested calls have separate invocation-local storage, and the fixed
failure ABI is unchanged. The terminal envelope is constructed in a private
out-of-line helper frame, not in one caller-frame local per terminal block.
GNU-compatible compilers use cold/noinline hints; MSVC uses noinline. The helper
is not noexcept because managed-worker emergency handoff can unwind through it.

Every emitted `Invoke` failure alternate is cold for native block placement
and call-site frequencies. The emitter spells it `[[unlikely]]`, except for a
failure-form call whose callee carries the always-inline small-source hint
(`cppMirSmallSourceBody`, the same predicate that spells the declaration). That
alternate begins with the cold, noinline incoming-cleanup check, and once the
callee inlines, its own failure path ends in a cold publication or check call.
A weight on the success flag would instead survive inlining as a claim about
the data condition that success reduces to, such as a payload tag; native jump
threading then moved it onto the unrelated branch that selected the tag, and
with the trailing payload-enum tag (RT-11) a `has_value()`/`value()` loop ran
about twice as slowly. Calls the native inliner may decline keep the weight,
which informs that decision: dropping it everywhere cost about 10% on a
string-producing payload-enum loop.

A detector's failure record and its incoming-cleanup check are one out-of-line
call to `mir_publish_failure_v1`. It writes exactly the record the inline
sequence wrote, taking the artifact identity from the program's descriptor,
keeps a propagated inner record, and then performs the same check. A panic
keeps its inline record because it also captures its message and may forward
the caller's site. The helper carries the same guarded cold/noinline hint and
is not noexcept, for the same handoff reason. Success paths no longer carry the
record's construction, so native inlining sees a failure-capable body at
roughly its success-path cost (RT-1).

Each admitted fallible Drop assigns its status at the producer before its paired
Invoke tests it. These native scalar carriers have no redundant entry-block
false initializer: that dead definition forced native SSA promotion to compute
large dominance-frontier/phi sets for every alternative cleanup status. This
does not change any GTI value's initialization, lifecycle slot, constructor
status protocol, failure record, or cleanup edge.

Aggregate, array, source-defined sequence, optional, shared/unique, closure and polymorphic cleanup
recipes inherit that scope. A cleanup sequence arms its own scope after its first
failure so later child calls stop before local cleanup on a second failure; the
fixed-array leaf traversal retains the same rule. Nested scopes never replace the
earliest primary, and successful exit or the runtime's existing managed-worker
emergency handoff restores the thread-local state. The managed task/capture cleanup
adapter copies its pending v2 primary and immutable descriptor into its own frame
through the internal `copy_managed_task_cleanup_primary_v2` query, rather than
borrowing mutable worker state. Neither records nor stack contexts escape to
another thread. Runtime-site records may legitimately retain a null descriptor.
During managed task cleanup, an emergency may complete the already stored primary
only when both that complete v2 record and its descriptor exactly match; replacing
the primary or extending an existing two-record envelope remains terminal.
Publishing that cleanup-kind outcome clears the inactive ordinary payload; the
exact primary lives only in the emergency envelope. Join-result validation
continues to reject contradictory active and inactive payloads.

A moved class return is constructed at its MIR `Move`, before the cleanup
continuation, but is not published until Return. The Move must dominate the
Return, and later uses of its source remain restricted to moved-from drops.
The return classifier can traverse live destructor `Invoke` blocks only with
the verified `ReturnResult` obligation: failure cleans the initialized out
result, while success transfers it to the caller. Direct transformed calls
and explicit construction use the same pending-result destination. Failure
cleanup calls a fallible result's exact status cleanup recipe before native
teardown; an infallible result uses native teardown directly.
Source-defined buffer/sequence results use the ordinary class-result lifetime
and field-cleanup schedule; there is no private prefix-storage result carrier.
This does not admit arbitrary executable work or reinitialization
between a class result's construction and Return.

In transformed constructors with owned-parameter fields, passive scalar and
plain-enum parameter reads remain in the MIR body schedule. They are not
extracted into a competing C++ member-initializer schedule ahead of the
body-ordered owning-field publication.

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

Named places fuse a copy or a fresh construction the same way, without a
bounds site. A global's storage, a lambda's by-value capture, and the referent
of a by-reference capture are lvalues that the shared place spelling names
directly, exactly like a bare binding: each is a destination (a captured
reference only when it grants mutable access) and each can be the copy's
source. A mutable loan referent, as an overloaded subscript's `mut T&` result
yields, receives the copy through the loan, as it receives a fresh value; only
that loan's pure two-phase activation may separate the copy from the store. A
read-only loan or captured reference is no destination, so its copy keeps no
declaration and the body is refused.

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
Source container checks use the general scalar bounds detector: its exact
outcome must be discharged before the backend can omit that detector. Raw
element access remains a separate explicitly unsafe MIR operation, not a
private checked/unchecked container-helper selection. A fully discharged
`Invoke` follows its verified success edge; any retained local or callee failure
keeps the checked representation and failure transfer.

These choices inspect only MIR operation identities and exact discharge
metadata. They do not recognize `std` wrappers, source declaration names, or
generated C++ shape. Unsupported operations remain on their checked backend
route. Sparse storage is ordinary source code: its slot-state guards and
general object-lifetime calls do not acquire an unchecked sparse helper or an
initialized-slot discharge from the public container's identity. The retired
sparse-state optimizer pass and proof family have no backend equivalent.

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
mutability from a C++ rvalue or reselect the native member overload. A
virtual member's call keeps its MIR dispatch: the slot is presented as the
member's owner class (the temporary's own class or one of its bases) and the
table selects the override, through the transformed status ABI when the family
is failure-capable. The emitter allocates no storage for a by-value receiver
here.

For a verified read-only temporary argument input, the body emitter uses the
same exact materialization, the value-rooted lifetime slot or the CFG-promoted
hidden slot of a call in a conditional arm or `&&`/`||` operand, and passes
that slot to the selected reference parameter. The hidden slot's MIR drop
after the merge is emitted as its conditional `destroy_if_engaged()`. A
payload-enum temporary in such a slot, cleanup-free or owning, is constructed
there as well. Its `PayloadConstruct`, retargeted to the slot by the merge,
constructs the alternative in place. An owning enum returned by a call
publishes through the slot's construction address; a call that cannot fail is
retargeted to the slot and a raising one arms the slot's obligation on its
`Invoke` success edge. A linear payload-enum argument keeps its value-rooted
SSA object. A borrow of a hidden slot that no lifetime-slot recognizer
constructs in place is refused: its producer would define only its SSA value
and leave the borrowed slot unwritten. Its
`CallArgument` loan is representation-erased only
after preflight proves the matching `BorrowRead`, call input, immediate
loan-end schedule, and full-expression drop. Failure successors use the MIR
cleanup order. The emitter neither binds an arbitrary C++ prvalue nor infers
eligibility from the generated signature.

Either borrowed temporary may itself consume an owning constructor argument.
The argument's exact `Construct`, success-only activation, and single
`MoveValue` input reparent ownership into a caller-owned prepared-parameter
slot. The outer constructor transfers that obligation at invocation and takes
the prepared object, retiring only its moved-from representation. Later
argument failure instead drains the still-prepared object through MIR cleanup.
The outer result may publish into a named
binding or directly into its verified temporary-borrow slot; the latter has
no binding `Initialize`. Its borrow stage must be on the construction's
success successor, with the existing materialization and cleanup proof.
Missing activation, transfer, materialization, or scheduled cleanup rejects
emission. This does not widen the separate constructor-field rollback proof.

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

Carrier references preserve a physical runtime address independently of their
`storedDependency` owner loan. The latter is lifetime metadata, not another
pointer to the carrier's storage. A copied shared-carrier value return also
needs no extra runtime loan pointer: the returned class value already contains
its reference fields. Representation admission recognizes the exact readonly
abstract entry and earlier `Load`/`Copy` SSA producer. Unlike an immediate
transfer, its entry can retire later during scope cleanup. This does not relax
lifetime verification: `verifyLoweredProgram` runs the complete MIR verifier
before representation admission, requiring child-first retirement on success
and failure. The backend neither reconstructs owner origins nor invents loan
endpoints from C++ reference fields.

For an exact cleanup-owning class value consumed by an ordinary by-value call,
the body emitter uses the caller-owned `PreparedParameter` slot named by MIR.
A direct `Construct` or admitted cleanup-owning call result publishes once into
that slot; its source drop remains authoritative until the matching `CallInput`
reparents it, and the consuming `Call` transfers the prepared drop exactly
once. Unsupported, aliased, duplicated, projected, or mismatched
shapes remain rejected rather than receiving backend-invented storage or
cleanup.

A complete fresh owning class result can also transfer directly
from its success-activated value slot into one exact static source-GTI function
parameter. An owning class constructor result uses the same proof, with exact
publication either on its checked success edge or its infallible instruction
activation. These consumers share the same slot-use and CFG proof:
one typed by-value operand, one matching unconditional `TransferOut`, producer
success dominating the consumer, and exact cleanup on intervening argument
failures. The function declaration's parameter and return identities must match;
member receivers, native calls and dynamic dispatch are not inferred from this
route. The value is taken from its sealed slot only at the proved handoff.

The prepared extraction may precede another checked argument, including another
owning extraction. A dominating, acyclic CFG proof requires every normal path
to reach the sole consuming call without reading or changing the slot, and each
intervening failure path to destroy its exact prepared obligation once before
propagation. A fallible destructor's paired cleanup-failure edge retains the
original and secondary failure records. This is the same MIR parameter lifetime,
not native argument evaluation or special recognition of `make_pair`.

When an exact temporary-place extraction moves into prepared parameter storage,
its MIR Reparent ends the source temporary's lifetime. The native lifetime-slot
carrier is retired after movement so a loop can execute the same temporary
again; generated movement has already disarmed source user cleanup. Named
binding carriers do not take this early-retirement route. A cleanup-free
temporary may instead keep a passive obligation of its own, as the hidden
branch-only temporary of a `?` operand in a conditional arm or on the right of
`||` does. It moves into a directly activated prepared slot, and its moved-from
shell ends where MIR drops it rather than at the move. A stable unprojected
local receiver may remain staged across a propagation branch. The branch is
the failure-free `has_value` call of a `?` through `std::tryable`. Its success
edge continues with the `take_value` call, which MIR names with the
propagation's own HIR value as it does the branch, and only the ends of the
test's own input loans may follow the test in its block.

A cleanup-free direct result may forward into caller storage without inventing
a result-drop obligation when it has one exact return use and only inert
boundary/EndBorrow records intervene. Ordinary source-call results
similarly permit their completed input loans and unrelated passive local
lifetimes to end before return, including the exact call's Invoke success
edge. Prepared argument handoff uses the same exact CallInput/TransferOut
proof as constructor publication. No default constructor or result-drop
obligation is invented. These representation choices preserve the verified
Invoke and cleanup schedule.

The failure-free `StringViewSize` and `StringViewEmpty` observers admit either
a borrowed view place or an ordinary SSA view value, such as the result of a
call returning a view. A counted view is a by-value descriptor, so the observer
reads only that descriptor. Any owner dependency belongs to the producing
call's verified MIR result loan, which spans every use of the value; the
backend does not extend, end or reinterpret it. The admission keys on the
verified intrinsic, the `Value` operand category with no place or loan, an
exact receiver value type with the `StringView` representation, and a
producer other than a staged `CallInput`; observers carrying failure sites or
a defined failure are not admitted. MIR constant evaluation already observes
either operand category. When `std::string_view` becomes a source-defined
class under the separate source-view proposal (historical PR #109), these would
become ordinary member calls on a class value and subsume this intrinsic route.
That proposal is not the current primitive-view implementation.

A moved nominal callable receiver uses a different contract from a by-value
argument. Its exact MIR Move initializes a Value-rooted lifetime slot, the
receiver CallInput aliases that slot, and invocation retains its obligation.
The same slot remains active through later siblings until the MIR normal or
failure cleanup. The backend requires one executable stage use and one exact
lifetime root rather than interpreting the lifetime root as another invocation.
Consuming method field aliases use the shared `receiverAllowsMutation` rule,
so moving a `mut` field does not accidentally become a native const-copy.
Public wrapper identity plays no part in either representation decision.
An unfused copyable lambda `Load` consumed by return materializes its SSA copy
at that exact verified instruction. It is not erased as an invocation-receiver
alias: the later return publishes the retained value instead of reading an
undeclared SSA name or re-reading its source place. Generic relay signatures
remain deduced native representations of the exact semantic lambda type;
copyable returns remain copies and noncopyable returns retain their movement
and environment-drop contracts.
The same proof admits an owned lambda Move with one direct invocation receiver
use and one exact lifetime root. Its slot uses the existing concrete closure
representation, whether produced locally or received as a concrete generic
parameter. The Move constructs the slot; both ordinary and failure-ABI calls
alias it, and the MIR environment drop controls capture destruction. No
host-language temporary is allowed to shorten that lifetime to the invocation.
Pack-expanded invocations retain the same slot through an ordered receiver
stage. A cleanup-free moved local environment instead aliases its already
materialized immutable storage, but only after proving the exact single-use
Move/optional value-stage/invocation chain. A cleanup-free moved concrete
callable parameter or receiver field spells its place through the same
single-use Move/value-stage chain; the stage itself has no representation. Its available-to-moved event remains
in MIR; this representation erasure does not permit source reuse.

Nested callable invocations consume only their own prepared receiver and
argument stages, just as ordinary calls do. A receiver already prepared for
an enclosing call may remain pending through the nested invocation, subject
to the same instruction-window, unique success-path handoff, and block-end
consumption proofs. A local frozen lambda inside a generic function may use
the existing fused-closure representation without a native type row, but only
when its exact initialization and complete use chain satisfy that proof.
Neither change reconstructs a call or its evaluation order from source syntax.

A lambda local's storage follows the instruction that produces its value.
`constructedLambdaDestination` names the exact binding or verified temporary
result slot that a lambda value publishes into, unless an admitted fused
closure chain carries that value. Such
a producer can be a materialized Closure, a copy `Load`, a `Move` or a
closure-returning call. The declaration (`lifetimeSlotPlace`), the producer's
construction, the destination `Initialize` and the vocabulary admission all
consult this one proof. So `F g = f;` in a deduced-callable body declares and
constructs one lifetime slot even though the body creates no literal of type
`F`. A passive closure's local-binding slot is representation-only. A joined
result retains its explicit `Value` obligation even when capture cleanup is
trivial: its exact `Initialize`/`Reparent` publication, later `Move` and `Drop`
remain the lifetime authority. Nested results reparent the exact moved-from
temporary's `Value` obligation, not a fabricated SSA-rooted lifetime. Several
forcing edges share an environment
factory only for the same exact semantic lambda type and the same independently
verified concrete lambda instance/body/layout; each edge constructs from its
own capture operands at that edge's `Closure` step. Distinct concrete generic
instances do not share an environment by source span alone.
When `Reparent` hands off a hidden result's obligation, the destination is
already constructed by its producing `Move`; the publication retires only
the disarmed source representation, including passive environments.
Before each representation-only construction,
its producer retires any previous representation, as a representation-only
class publication does, so a loop can rebuild the local without a semantic
Drop. A fused chain's carriers still allocate no storage. When neither a
template overlay row nor a materialized closure can name a constructed local's
type, the body is refused instead of emitting an undeclared slot.

An exact typed lambda invocation stages a by-value class argument through the
same ordered `CallInput` plan as a function call. When a later argument can
fail, a MIR failure edge drops that argument's prepared-parameter obligation.
The argument then occupies its prepared lifetime slot from the stage onward,
exactly as for a source function call: the invocation moves from the slot and
retires it, and the failure edge destroys the still-engaged slot. The proof
binds the lambda's exact parameter type in place of the function target.
Without that failure cleanup, the invocation keeps the direct by-value
spelling and adds no prepared slot or extra move.

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

The owning fixed-array admission gate also retains these exact sequenced
`Move` arguments and verified direct return moves. The former constructs at
the move instruction; the latter publishes into the caller's result slot.
Neither route requires an independently default-constructed SSA array, even
when its elements delete default construction. Arrays without an exact
argument, result, or lifetime-slot publication proof remain rejected rather
than falling back to native value initialization.

A fixed-array element that MIR stages by copying or moving one whole named
binding, such as a by-value constructor parameter or a local, is spelled
inside the array's braced initializer instead: `Polygon(Point a, Point b) :
corners({a, std::move(b)})` constructs the field once as
`std::array<Point, 2>{a, std::move(b)}`, so each element is copied or moved
once, directly into the array's storage. A braced initializer evaluates its
elements left to right, so the elements keep their operand order; what moves
is the point of each copy or move, from its `Load` or `Move` to the aggregate.
The proof (`inlineFixedArrayElements`) admits that only when nothing in
between can tell. Producer and aggregate share one block, and every
instruction between them is failure-free. None of them writes the binding (or,
for a move, names it at all), and the body neither lends it mutably nor takes
its address, so nothing writes it unnamed. An
instruction in between that is not itself an inlined element later in the
same aggregate may run user code, so the element's transfer must then be
observation-free: the generated copy of a passive generated-copy class, or a
generated move. A copy also needs a copy constructor on the element's leaf
class; semantics currently lets a class without one appear as a copied array
element, and the body is then refused rather than spelled. A moved class
element carries a value obligation that the `Move` takes from the binding and
the aggregate transfers out, so it needs no storage. A move with no lifecycle
event, as MIR records for a cleanup-free array, is admitted only for a
cleanup-free element, because the binding's own obligation stays in force.
The rule names representations, not types: rows of a two-dimensional array
(`cells({top, std::move(bottom)})`), elements of nested rows
(`{{a, b}, {c, std::move(d)}}`) and moves of a class with a source destructor
use it alike. The inline spelling applies only where the aggregate constructs
its final storage: a passive staged binding, or the owning route's binding,
constructor field, nested element slot or argument slot. That owning route
accepts obligation-free elements beside transferred ones, so an inlined copy,
or a cleanup-free copy in its sequenced local, can sit next to a fresh
construction (`corners({a, Point(5, 4)})`). An element outside the proof keeps
its staged route. A plain constructor stages a by-value array parameter of
passive generated-copy classes, like `top`, in a lifetime slot just as it
stages one such class: the array moves and is destroyed element-wise by
generated members, which run no user code.

For a verified `MoveBorrowedTemporary` ordinary call or constructor input, the body emitter
publishes the exact producer call result into its MIR value-rooted lifetime
slot, moves that value into the selected parameter, and ends the
moved-from slot representation after the consuming invocation. MIR remains
authoritative for producer success, ownership transfer, call/constructor
failure cleanup, external-owner loan endpoints, and the resulting value's
one drop.

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

A cleanup-free class result with one exact value-rooted field chain and one
scalar `Load` uses the same uninitialized lifetime-slot representation.
An admitted GTI call or ordinary `Construct` publishes once into the slot, as
does a failure-free `Move` of a whole object out of a place, as in
`std::move(a).x`, which constructs the slot from its source, and the load
spells through its verified field chain. The chain may be nested, as in
`outer(inner(1)).inner.value`. MIR builds such a read from its shorter chains
and records each of them as its own place rooted at the same value; that
prefix record is admitted only when no instruction, terminator, loan, or drop
touches it, and it declares and spells nothing. The copy `Load` that MIR uses
for a class assignment's result, as in `(p = q).x`, copy-constructs the slot
from the committed destination the same way; it requires a copyable class with
a generated destructor and no active drop state or borrowed state, takes no
other consumer, and keeps every other copy-load route unchanged. Whole uses of
that result need no backend rule of their own: an initialization, return or
constructor field initializer spells the copy `Load` as any copy load, a
receiver borrows the hidden temporary as a joined temporary, and a by-value
argument is an ordinary copied argument staged before any later argument
runs. The constructor form also requires exact target, owner, result,
parameter, and operand metadata; a trivial scalar copy load; and no active
cleanup, lifecycle, borrowed-state, or drop obligation. A text view field
(`std::string_view` or `c_string`) is read the same way: the view is a passive
descriptor whose text is never the temporary's own storage. A construction that
publishes a local owner's text (`L-OWN-02`) holds borrowed state only as that
verified `Stored` loan, so it too may take this one field read, though no
transfer consumer. A text size or emptiness observer whose borrowed receiver is
the projected view is the read itself, as a copying `Load` is; the slot retires
right after it. The owned-result field view below admits the same observer.
Additional consumers, places outside the read chain, non-field projections,
cleanup-owning temporaries, and incoherent type rows reject this route rather
than falling back to native temporary behavior.
A cleanup-owning class result keeps MIR's value-rooted slot and its
full-expression Drop, and may instead be read through field views of that
slot. The result may come from a call, as in `make_tracked(7).number`, from
an ordinary `Construct`, as in `tracked(7).number`, or from a `Move` out of an
available place, as in `std::move(a).number` on a local or by-value parameter.
A failure-free constructor
constructs the slot in place and arms the result's obligation with its own
`Initialize`. A raising one places the object at the slot's construction
address and arms the obligation on its Invoke success edge. A `Move` with no
failure of its own move-constructs the slot from its source; its one lifecycle
event transfers the source's obligation to the result (or arms it when the
source had none), and the source's later Drop ends only the moved-from shell.
Either way the
construction's or move's only other uses must be field views. Views cover nested fields
and a field lent to one member call as in `make_named().text.size()`, in
function bodies as well as field defaults, member initializers and base
argument preparations, whose carrier holds the slot. A view is a pure field
projection of the result's value root with exact field rows. It carries no
loan or drop and is touched only by copying `Load`s, by text size or emptiness
observers, or by read-only borrow stages consumed by one call. It declares no
local and spells
`slot.get().field`, so a field class without a default constructor is never
default-constructed. MIR's verified initialization bounds every read, and the
result is destroyed exactly once by MIR's normal-path or failure-cleanup Drop.
Inside a short-circuit operand or a conditional arm, the producer, a call, a
construction or a `Move` out of a place (as in `c ? std::move(a).number : 0`),
instead constructs the result in a conditional temporary that
holds the result's own obligation. A failure-free producer does so at its
destination; a transformed one arms the temporary on its Invoke success edge;
a `Move` names the temporary as its destination and arms it with its one
lifecycle event.
Either may also transfer out the prepared inputs it consumes, as the moved
receiver in `c ? std::move(p).loud().id : 0` is; the proof admits exactly
those transfers beside the result's activation.
When the result is read only through field views, that temporary is its
storage: the views spell `temporary.get().field`, the result declares no local,
and MIR's conditional Drop destroys the temporary exactly once if its operand
ran, on the normal path or on a failure-cleanup edge.
Reading a trivial scalar does not transfer its owner: fresh passive immovable
results use this same proof and storage without requiring a copy or move
constructor. They cannot carry borrowed state or active drop state through
this representation-only slot. Constructors construct in place; status-returning
source calls publish only on their MIR success edge.
The field load retires the passive shell before slot reuse, including in loops.
Transfer consumers still require movability independently.

A constructed or returned owning value may replace a field reached through a
checked mutable reference parameter. Its leading dereference must resolve to
the exact reference carrier and mutable loan already admitted for whole-referent
assignment. The remaining path uses the ordinary concrete field rows, checking
every containing field's mutability. Such borrowed referents may be initially
available on function entry and acquire no second drop obligation; replacement
still destroys the old value and transfers the staged value exactly once.
Read-only carriers do not gain this permission. Raw pointers use their separate
verified raw-place contract: the already-evaluated pointer (and optional index)
must have mutable pointee access, and every subsequent field must resolve through
the same concrete mutable field rows. This also admits owning replacement through
a returned pointer or nested raw field path, without manufacturing a checked
loan or a second drop obligation. MIR retains the unsafe store and raw-memory
effects; construction, old-value cleanup, publication and staged-value failure
cleanup use the existing replacement schedule unchanged.

A failure-free source call has no Invoke edge: MIR activates its result's
value obligation with the call's own final `Initialize` event, after any
by-value input it consumed has transferred out. Every result slot that a
transformed call fills through its construction address on the success edge,
a failure-free call fills by constructing the slot from its returned prvalue
(`construct_from`). That covers class staging slots for a replaced binding, a
by-value call or constructor argument, one field read and a Return, as well as
value-rooted owning-array, owning class and owning payload-enum result slots, a
short-circuit conditional temporary that the call names as its direct
destination, and a callable binding. A plain owning-array result has no
declarable local, so the plain form admits such a call only with one of those
slots, each of which requires the call's own activation. When nothing executable
separates the call from its Return, a plain body spells `return <call>` and a
failure form constructs the caller's result storage from the call directly. The
staged arguments that call consumed already transferred out at the call, so in a
plain body their slots end the moved-from representation at C++ scope exit,
after the returned object is initialized. A targetless default construction
never takes that route; it spells `return T{}` itself.

Generated native destructors test their class's initialized active-drop flag
before calling its cleanup adapter. This is the same guard the adapter already
checks, not a new assumption that moved-from cleanup is harmless. MIR-directed
move/drop operations disarm the flag; a false flag therefore bypasses only an
effect-free adapter path (emergency-state observation and an empty failure
record, with no source cleanup). A true flag still enters the unchanged adapter,
including emergency handling, failure propagation and exact source destructor
order. Native base/member destructors still run after either branch. Exposing
the guard at this boundary lets native optimization remove retired-shell calls
without inlining the cold defined-failure machinery or weakening GTI cleanup.

When cleanup such as the drop of a moved-from parameter runs between the call
and the Return, the result stays in its value-rooted slot and the Return takes
it out. A fallible cleanup in between reparents the result to its return-result
obligation, and the failure path destroys that slot rather than the caller's
never-constructed storage. The call may be a consuming (`&&`) source member
whose `Once` receiver passes the shared consuming-receiver proof, as in
`return std::move(box).release();`. A receiver that owns cleanup is not
transferred at the call, so its full-expression drop is one of those later
cleanups. A cleanup-free receiver moved out of a place is a prepared input that
the call transfers like an owning argument, as in
`return std::move(maker).make();`; only the moved-from source's drop then
follows the call. No other invocation capability is admitted. A failure-free
call returning a reference binds its call-result loan by address in either body
form. A plain body returning a moved class, callable or owning array value
stages it at the Move, before any later drop of the moved-from source, and
returns it from that slot. A moved owning payload enum already occupies its
value-rooted slot. A
copied class whose source is dropped or overwritten before the Return is copied
at the Load into a staging slot, while a source still live at the Return is
copied there directly.

An ordinary cleanup-free construction may instead feed one exact by-value
parameter of a selected static/free source GTI call. The same uninitialized
slot preserves construction at its MIR position rather than moving it into
the native argument list. Exact target/type/operand identities, sole use, and
dominance from successful construction to consumption are required; no
value-root place, active cleanup, borrowed state, or semantic drop may be
invented for this representation-only shell. A later argument failure still
uses the existing MIR cleanup of preceding owning arguments. Unrelated
user-defined constructors exercise both infallible and failing construction,
ordered subsequent arguments, and early failure before call entry.

A class result of a callable value invocation is published like a source
call's result (`callableClassResultInvocation`). Verified MIR binds the
invocation's result and parameters to its exact lambda instance, and the
invocation completes the result on its Invoke success edge under callable
propagation, as a transformed source call does. A cleanup-free result therefore
takes the representation-only SSA slot (`classSsaLifetimeSlot`) for one field
read, by-value call argument, assignment, constructor field or discard. An
owning result takes the value-rooted slot (`valueRootedClassCallResultSlot`)
that its field views or one consuming call read, and a by-value argument with a
prepared parameter receives the result directly in that parameter's slot
(`stagedClassResultForSource`). The invocation writer passes the selected
slot's construction address and marks it constructed on success. No class
result of an invocation becomes a default-constructed C++ local.

A cleanup-free class construction whose constructor can fail, consumed by one
adjacent `Assign` into an existing binding, uses the same representation-only
slot as a call result that replaces a binding. That covers `h = holder(6)`,
whatever its arguments are. The failure-tag constructor publishes into the
slot on its Invoke success edge. The `Assign` then move-assigns the slot's
value into the binding, and the generated representation retires. A failed
construction leaves the binding untouched. The class needs a generated move
constructor and move assignment, no borrowed state and no active drop state.
The construction's only lifecycle events may hand each by-value argument to
the constructor once (`preparedArgumentTransfer`): an owning argument's own
value drop, or the
prepared-parameter slot of a class argument, a named copy or move staged there
or a fresh result already published into it, as in `h = holder(q)`. A
failure-free construction, or any construction the fused assignment already
spells, keeps that fused route. A cleanup-free class call result may likewise
replace the exact mutable referent returned by a source call, as in
`*cursor = make()`. The referent's loan, not the synthesized place's
contextual access, owns the permission, and only that loan's canonical
two-phase activation may sit between the publication and the `Assign`.

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

Signature metadata includes its GTI receiver qualifier in the canonical native
identity. Native type spelling uses an unqualified function type for read-only
metadata, a ref-qualified `R(Args...) &` type for mutable metadata, and
`R(Args...) &&` for consuming metadata. These are distinct non-object types,
not native callable values or pointers. GTI-selected operators and MIR receiver
access still define invocation; native ref qualification performs no selection.

A generated-default class prvalue may enter an adjacent failure-capable
constructor even when it has a source destructor. The representation proof
checks failure-free field initialization independently of destruction, then
requires the existing exact fresh-value cleanup obligation and its single
transfer into the consuming parameter. It does not omit cleanup or admit
effectful/fallible field initialization as an inline default value.

A selected, nameable application of an eligible top-level generic class
similarly emits as an ordinary `__gti_type_<digest>_<source-name>` class. The
digest owns the nominal primary, source-unit identity, generic parameter shape,
and exact type/value arguments. Fields, bases, initializers, lifecycle status,
and constructors come from the matching sealed MIR class instance. A deleted
status spells `= delete`, including a default constructor whose requirements
the application fails. The representation snapshot's class row takes its
boundary, copy-construction and copy-assignment proofs from the same instance
statuses. The generic declaration's statuses describe only its open primary, so
`maybe<Tracked>` over a move-only payload is not copy-constructible even though
`maybe<T>` generates a copy constructor. A class type without exactly one
sealed instance keeps its declaration's statuses. Compiler-capability and
passive C ABI types keep their unconditional boundary proofs. An ordinary
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
The lexical-carrier census includes constructor instances, even when no free
or member function carries the closure type. An unnameable constructor parameter
uses the admitted dependent-template overlay and is not also published as a
concrete native specialization with an invented parameter spelling.
A direct generic class definition is ordered by its exact concrete application
and exact substituted by-value fields, not by one collapsed source-family
node. Finite alternating nests such as `box<cell<box<int>>>` therefore form an
acyclic concrete layout graph even though both source families mention one
another across the program. A genuine cycle in that exact graph remains a
fail-closed backend invariant violation; ordinary source-level recursive
by-value fields are rejected earlier by semantics.
For retained native generic carriers, merely mentioning a class as a type
argument does not impose its complete layout. The planner follows the exact
MIR instance's fields and bases, stopping at pointer/owner/storage indirection.
This permits mutually owning node types while preserving true by-value
dependencies such as `box<T>` storing a `T` field. Symbolic template spellings
without a concrete instance retain conservative argument dependencies.
Private unique-owner carriers spell generated default/move/destruction with
explicit non-constexpr bodies and fixed `noexcept` signatures. This avoids
C++23 constexpr special-member instantiation requiring an incomplete pointee's
deleter while another recursive class is being defined. The move spelling
uses the same resolved field/base order and lifecycle guard policy; it does
not add a GTI constructor or source cleanup body.
A selected generic method on a retained carrier keeps its stable
`__gti_member_` name, but the backend emits the required owner-specialization
declaration before any use. These carriers consume identities and validity
already chosen by GTI; they cannot perform lookup, overload selection,
constraint checking, ownership decisions, failure analysis, or execution
outside verified MIR. Native overload resolution and template deduction
therefore provide physical spelling only for the named carrier cases, never
language semantics.

Retained generic carriers also close their dependent declaration-type graph:
generic classes appearing in their dependent fields, bases, aliases, enum
payloads, or method and constructor signatures use the same template-carrier representation
transitively. Otherwise a symbolic `Wrapper<T>` field or result would instantiate
a different C++ class from the digest-named `Wrapper<int>` addressed by concrete
MIR. Fully concrete declaration references do not force this propagation.
This is representation coherence only; executable methods still come from the
selected concrete verified MIR bodies.

Type-only aliases also require forward declarations for any direct concrete
class names in their resolved type argument graph, even when no executable
MIR class instance uses them. Declaration planning uses the same concrete-name
authority as runtime uses and emits each forward identity once before aliases.
This includes the exact member-alias catalog of each concrete MIR class
instance: a dependent declaration-level alias alone cannot name its resolved
type/value argument application. Repeated targets share one forward declaration.
It does not instantiate layouts, constructors, or methods merely for an alias,
and does not recursively expand the alias members of a referenced generic
class. Dependent aliases retain their existing native-template spelling. This
keeps recursive alias families finite and unused aliases ABI-consistent with
any later executable use.

Closure-typed (unnameable) free-function instances retain separate artifact-local
C++ symbols keyed by their exact lowered/MIR instance identity. Each symbol's
native template carries only the unnameable physical types and emits that one
verified MIR body. Declaration planning and body-call rows use the same naming
authority; the projected symbolic signature must substitute to the exact MIR
signature. Different selected constructor tags, pack arities, or cleanup schedules
therefore do not force unrelated instances into a shared source-template body.
These private symbols are not stable external ABI identities. Nameable generic
instances retain their existing concrete digest names.

Closure-typed member instances emit through a deduced-callable template whose
body text is canonical per exact nameable signature and parameter-cleanup
shape. Signature selectors compare the projected result and parameter types,
including flattened final packs. Every variant uses the same nameable
dimensions within its exact pack arity; an unnameable dimension is omitted for
that whole arity family. Indexed closure-identity predicates are formed only
inside a discarded `if constexpr` branch that first checks the pack length, so
a shorter instance never forms an out-of-range native tuple element type.
Calls to retained-owner member templates spell MIR's exact method type
arguments explicitly when a runtime pack precedes transformed result/failure
parameters; native deduction cannot recover such a nonterminal pack. The
cleanup shape records which declared parameters carry a lexical drop obligation
in that instance's verified MIR. Instances sharing both keys must prove
byte-identical overlaid body text. When concrete callable
environments produce different shapes — a scalar-capturing lambda beside a
class-capturing one — the one emitted template reunites the canonical variant
bodies behind exact signature guards and an instantiation-time `std::is_trivially_destructible_v`
dispatch over each varying parameter, which must be declared directly as a
template type parameter (or be the exact thread-task parameter). Any variance
the dispatch cannot express fails closed rather than merging inconsistent
bodies. There is no unconditional final-body fallback: an unrepresented
signature fails at native instantiation instead of selecting a different MIR
instance's body.

For final value packs containing closures, the callable signature projection
compares expanded concrete parameters against MIR rather than requiring one
native argument per source formal. Unnameable element types use exact pack-index
type aliases, and the deduced body reads pack arguments through tuple views.
Prepared closure arguments occupy MIR lifetime slots before invocation and are
retired after transfer. Transformed calls spell their exact type arguments
because the trailing failure/result parameters prevent native pack deduction.
A passive closure passed to such a call keeps one materialized closure type
even when a normal non-pack call could fuse its literal. Its immutable Load
aliases that slot at the call; emitting a second literal would create a
different native type than the explicit template argument. Active captured
values retain their existing prepared lifetime and transfer schedule.
Fresh owning closure literals can publish directly into an exact prepared
argument slot at the Closure step. The following CallInput reparents the
source's lifetime metadata and the selected call transfers the prepared owner;
it does not create another closure or defer capture evaluation to native call
argument order. The proof retains exact capture moves, a sole executable use,
matching drop identities and consumer index, and no additional source alias.
These rules are representation choices over verified MIR; they do not select
overloads or infer ownership. Free-function instances with different pack
arities or cleanup shapes have distinct symbols and bodies. Member variants
remain subject to the fail-closed canonical-body check, not general pack-shape
dispatch.

An owning closure that remains live across a conditional invocation can instead
occupy its exact value-rooted full-expression slot. Its Closure instruction
initializes that obligation once; read-only invocations do not transfer it.
Receiver CallInput records alias the environment without copying it, retaining
the exact call-site identity. This permits eager captures followed by an
error-only mapper call, with the same MIR-scheduled cleanup on either branch.
Trivial eager closures retain an exact value place without a cleanup
obligation; repeated evaluation resets only their representation storage.
Class captures retain their lexical lifetime obligation even when destruction
is passive. Thus scalar moves and class copies occur before the branch too.
Other frozen passive closures still use the existing checked fused representation.
An immediate passive scalar-copy closure may also fuse into one same-block
invocation when only its exact receiver staging separates construction and
use. This preserves the MIR capture-time snapshot even if later instructions
write the source, including a later loop iteration. Any intervening computation,
argument evaluation, write, call, cleanup or failure boundary blocks that proof;
delayed or repeated uses retain the existing frozen-source requirement. Owning
move and class captures retain their distinct lifetime/consumption proofs.
Taking a checked local reference prevents that fusion: the closure factory
materializes its environment once, and the ordinary MIR loan/pointer carrier
aliases it. The body-local closure type also names reference carriers and
their dereference places. Invocations through these places reuse that exact
environment; they neither repeat captures nor acquire a cleanup obligation.
Borrow endpoints and owner cleanup remain the verified MIR schedule, including
reborrows and failure cleanup.
Exact lambdas with owned class parameters use the shared parameter-slot prelude;
class results publish into the same verified destination slots as function
results. Prepared argument shells are retired after invocation, including its
failure path. None of these representation rules recognize `transform_error`
by spelling or recover language rules from HIR.

Closure-dependent allocation also retains explicitly supplied generic type
arguments, including an allocated model type that does not appear as a direct
value parameter. Type rows distinguish body-scoped template overlays from
program-wide spellings. MIR body emission composes nested class, unique-owner,
pointer, and reference types from those overlays or a local closure alias;
it must not reuse a program-wide placeholder for an unnameable closure.
The same representation is used for allocation storage, result carriers, and
borrowed receiver pointers. An exact moved closure parameter may cross the
transformed failure-call boundary without requiring a local closure definition.
These are native naming choices; selected instances and ownership still come
from verified MIR. Direct return of an owning factory call across remaining
fallible cleanup uses the explicit unpublished-result lifetime described above.
Dynamic cleanup through the interface owner uses the complete object's bound
MIR cleanup recipe; upcasting preserves that binding.

Closure-bearing nominal carrier arguments retain ordinary MIR `CallInput`
staging and explicit move provenance. Generic-owner virtual failure contracts
also inspect the exact template-overlaid implementation; a program-wide
placeholder type must not make a valid override disappear. Unnameable owners
emit only their template body, not a second concrete specialization with a
placeholder closure type. The ordinary native vtable bridge uses those same
exact signature selectors when a family contains multiple result/pack shapes.
A symbolic result instantiated as `void` retains an unused `void*` result slot
where its declaration or connected virtual contract requires one; concrete
callers pass a typed null pointer. This is native ABI metadata, not a runtime
MIR void value. Representation validation rejects disagreement between virtual
roots and overrides.
An instantiated abstract root retains its status-ABI declaration even if no
concrete override is instantiated. Existing overrides must still have admitted
failure bodies. An empty checked owner therefore compiles and reaches its MIR
null-owner failure before virtual dispatch, without inventing a native body for
the abstract method.

A moved return can cross cleanup of other already-transferred parameters or
locals when an unconditional MIR transfer dominates each drop and no later
activation is possible. This is not permission to skip live cleanup or publish
an owning result across arbitrary fallible work. Bare unique-owner bindings
use their empty-initialized handle representation for non-failing drops,
resetting it at the MIR instruction even on a failure exit rather than treating
it as a class lifetime slot.

For a callable member template on a generic owner, that canonical comparison
is partitioned by the exact owner instance, matching the owner-specialized C++
definition that will consume it. Its projected signature substitutes every
owner type/value parameter while retaining only the member template's own
parameters as dependent C++ names. Thus two owners such as `container<int32_t>`
and `container<uint8_t>` neither masquerade as one canonical MIR body nor leak
an out-of-scope owner `T` into an explicit owner specialization. Identity,
substitution completeness, and the fully concrete MIR signature are checked
before the projection is admitted; incompatible rows still fail closed.

Constructor template overlays include constructor-local type arguments, not
only the enclosing class arguments. The lowered constructor identity, owner,
substitution and expanded concrete parameter signature must agree. Status
constructor declarations retain their constructor template parameters, and
the body is emitted from the same verified MIR under those local type names.
Constructor-local lifetimes use ordinary cleanup slots. Replacing a completed
owning field retains its already-active construction rollback obligation;
failure still drains locals and the replacement according to MIR.
When equivalent constructor instances share one source-template body, only the
representation comparison omits instance markers. The emitted body retains
every admitted instance's exact marker, including separate cleanup-shape
variants, so coalescing cannot silently remove an instance from corpus evidence.
When lexical closure parameters select different verified constructor bodies
with the same cleanup shape, the template uses their exact artifact-local MIR
lambda identities and nameable owner identity as compile-time selectors.
Distinct bodies require distinct selectors; unrepresented combinations fail
at instantiation rather than choosing a fallback body. The selectors carry no
runtime state and do not perform GTI overload resolution. Initializer-list
representations must still agree before bodies can share this template.

Within one immutable emission plan, constructor and callable-template body
plans are cached by concrete instance identity together with their exact
overlaid representation map. Repeated declaration/definition requests reuse
that validated pair; the cache is cleared for every new plan. Initial MIR
verification and canonical comparison between distinct instances are retained.

Call-result slots also cover virtual status calls returning owned objects.
Their exact virtual contract, prepared receiver, result activation, move into
the destination and cleanup remain checked. Native virtual boundary bridges
placement-construct class results in uninitialized storage instead of first
default-constructing an object and overwriting it.

An unused cleanup-free nominal result also receives
uninitialized output storage, from a transformed or a failure-free source call,
such as `[[discard]] make_value()` or a comma's discarded left operand.
Selection requires no SSA uses, value-rooted places, semantic result-drop
obligation, borrowed state, or active cleanup; the class must have generated
destruction. A failure-free call with a MIR destination is not discarded even
though its value has no uses: MIR rehomed it into that place (for example a
branch-only receiver temporary on the right of `||`, `&&` or `?:`), so it
constructs there and the place's own drop retires it. A transformed call marks that representation on success and a
failure-free call constructs it from its returned value; either retires it
immediately, permitting repeated calls in a loop.
Failure leaves the storage uninitialized and follows the existing MIR failure
edge. This is not a substitute for the MIR cleanup schedule of an owning or
destructor-bearing result.

Exact lambda invocation consumes borrow-staged arguments as ordinary calls do.
The stage must match the call site, physical argument index and parameter type,
and remain pending in the current block or its verified invoke-success chain.
It spells the existing place, not a copied referent. This includes projected
read-only reference pack elements in source-defined erased callback models.

Materialized closures use a local environment structure. Fields retain
the verified capture types and order; its call operator emits the exact lambda
MIR body. A private status-returning cleanup member consumes the MIR recipe's
capture projections, using the same cleanup-record cursor as aggregate classes.
Direct closure drops and owning-class field drops can therefore drain remaining
captures and caller obligations after one failure, or preserve primary and
secondary records on failure during cleanup. Native object destruction ends
the disarmed representation lifetimes; it does not select capture cleanup or
replace the MIR failure edge. Passive materialized environments retain trivial
cleanup. Re-entered passive local bindings and passive value-root slots retire
only their representation before reconstructing the next loop iteration's
environment. Owning closures and prepared argument temporaries keep their
verified explicit lifetime protocol. Fused non-escaping closures can still
use native lambdas. A fused literal is spelled only where its chain is
consumed: at an invocation, a deduced callable argument or a comma discard,
each of which selects the literal's calling convention. A chain with no such
consumer spells no lambda body, so the vocabulary requires neither convention
of that body. A lambda whose body has no plain form, for example because it
passes a move-only or destructor-bearing temporary by value into a class
constructor, can therefore be declared and never invoked.
Each materialized environment carries a private type alias for its exact
artifact-local MIR lambda identity. Source-template member families use this
identity alongside signature and cleanup selectors when distinct closure
instances need different concrete callees. Non-closure substitutions have a
zero selector. This is representation metadata, not GTI reflection or a new
callable conversion; body selection still comes from concrete verified MIR,
and an unrepresented variant fails closed.

Lowered declarations retain an explicit default-constructor or destructor
specifier for source/coherence and tooling inventory. Because that policy has
no source body, the emitter does not publish a separate member definition; it
uses the class lifecycle row's `Generated` status to spell the structural
operation. A policy declaration never becomes an empty C++ body: semantic
analysis has already decided structural availability, and the lifecycle status
remains the backend authority for that result.
Generated native moves preserve reference-field bindings and move ordinary
value fields. Applying native `std::move` to a reference field would move its
referent instead of transferring the binding, so the lowered resolved field
type selects direct binding initialization. Semantic lifecycle facts still
decide whether copying, movement and assignment are available; this does not
introduce reference reseating.

A targetless generated move construction consumes its staged input through one
leading `TransferOut` of that input's value obligation, followed by the
result's own activation. When the moved binding feeds the construction
directly, the backend spells `std::move` over the binding's slot and never
materializes the staging input, so the transfer is a native no-op: the
generated move disarms the source's active cleanup and its ordinary scope drop
retires the shell. Only a materialized value-rooted staging slot is destroyed
explicitly after publication. In both forms, a provenance-only `EndBorrow`
retiring the source's lifecycle-bound stored loan may sit between the
construction and its publication when the old and new loans have identical
sources, access, ancestry and dependency sets.

A generated default constructor of a non-generic class with no declared
instance fields has a selected HIR constructor instance when
its state-bearing non-generic base has an exact zero-parameter source
constructor. Semantics records that base declaration identity; HIR discovers
the base even when no expression constructs it directly. The generated
instance has declaration identity zero and no AST constructor. Its ordinary
constructor MIR body owns the exact base initialization, success activation,
and failure propagation, so a failed base cleans its completed state and then
the caller's live locals. Methods and static fields do not change eligibility.
The MIR `Source` definition category means an executable GTI constructor body
in this family; it does not assert that an AST constructor declaration exists.
HIR and lowered-program checks independently bind the generated body to its
owner's generated lifecycle status and the selected base schedule.

The C++ plan emits this body's admitted status constructor instead of a native
`= default` operation that would call a missing ordinary base entry. Trivial
and cleanup-owning bases use the same `InitializeBase` publication contract;
the rollback record retains the base's exact cleanup requirement. No terminal
adapter or native exception supplies the failure path. A base cleanup that
cannot raise a defined failure is emitted with a null failure-record
argument. Only a fallible base rollback consumes the body's secondary cleanup
record and its paired failure edge; a non-failing rollback neither allocates
nor references that optional record. A source destructor
does not prevent this base-only generated schedule. Derived instance fields,
generic owners/bases, constructor default arguments and
generated-base chains remain outside this generated-body slice and retain
their existing structural construction representation or the separately
verified declaration-field schedule described above.

An abstract or polymorphic class with a declared destructor uses the same
per-class construction guard as a non-polymorphic class. Its exact declared
destructor identity, active-drop state and reverse declared-field schedule
remain mandatory. The guard is disarmed until that class's constructor
success; a failed base therefore does not execute its source destructor, while
a completed base is cleaned exactly once if the derived construction fails.
This fields-only witness does not authorize default construction of a
state-bearing base or an abstract complete object. The selected base status
constructor, its success activation and exact rollback obligation are still
verified independently. Heap ownership, upcasting and stack construction use
the same rule, with no new runtime metadata or successful-path allocation.
Observation-free string-view literals may likewise feed
the exact native base initializer. They retain the same MIR type, single-use
argument, full-expression and borrow verification as other passive inputs;
effectful producers still require a separately verified pre-base schedule.

Checked/effectful scalar base arguments now execute through a pre-native-base
partition of the same verified constructor MIR. The immutable body plan retains
its exact CFG, stage, operands and scalar values/places crossing the boundary;
the text writer does not reconstruct source expressions. A private delegation
carrier forwards the selected arguments and original failure record, then the
remaining constructor schedule resumes after that prefix. Global effects are
not copied or restored. Prefix failure disables the selected base gate, leaving
its passive native shell unpublished and suppressing the source base body and
declared destructor. Native base success remains the existing MIR activation and
rollback witness.

A scalar base argument may be computed through class temporaries of the base
initializer's own full expression: a constructed or returned value used as a
method receiver, bound to a reference parameter, or read through a scalar
field, as in `Base(Feet(30).third())`. MIR drops such a temporary after
`InitializeBase`, at the end of that full expression, so it must outlive the
delegation. A temporary with a value obligation keeps its exact Value-rooted
slot, and the partition places that slot in the carrier as a lifetime slot.
The carrier's constructor runs the preparation, and its prvalue initializes the
delegated constructor's parameter directly, so the slot keeps one address from
its construction in the prefix to its MIR `Drop` after the native base. The
carrier is defined beside the constructor definitions, where each slot's class
is complete. A cleanup-free value read only through its fields has no slot and
ends inside the preparation, as it would in any body. The partition admits only
the temporaries' own producers, borrow stages, fresh-argument loan ends, drops
and field reads. After the stage only a slot's own `Drop` may name it, and no
later edge may re-enter the prefix, whose instructions the resumed body skips.

This preparation family requires scalar derived parameters and scalar base
arguments, with a passive non-borrowing base whose declared cleanup is guarded
and whose representation has no state-bearing ancestor. The derived class and
the selected base may be generic instances, such as `Building<int>` built over
`Foundation<int>`: the MIR proofs treat a generic instance like any class. The
carrier and the gate constructor, however, are members of each class's own C++
definition. The representation snapshot therefore marks a generic application
that is emitted as its own concrete class (`concreteGenericClass`), and the
emitter admits a preparation or gate only for such a class or a non-generic
one. A generic class retained as a native class template, such as one
instantiated with a lexical closure type, would need template specialization of
those members and remains refused. Owned or borrowed derived parameters and
base arguments, retained generic carrier templates and recursively inactive
owning/nested bases need the frame, transfer/loan and shell prerequisites
recorded in the implementation sequence. They are not accepted through native
C++ delegation shortcuts.

When a field or static initializer selects a failure-capable source
constructor, the private C++ representation snapshot derives one exact
contained-constructor row from the lowered initializer schedule and constructor
record. The row supplies backend-private tag and state spellings. Its generated
overload delegates to the exact failure form and terminates with the unchanged
failure record when construction fails. The emitter rejects missing,
duplicated, or mismatched rows; no C++ spelling or containment policy enters
`LoweredProgram`.

A containment wrapper delegates to the same admitted body plan as its status
constructor definition. For a generic constructor with an unnamed closure
parameter, this is the cached, independently verified dependent-type overlay,
not the ordinary map that cannot spell that closure. The wrapper checks the
exact constructor body identity and failure-form admission; it does not
introduce a second execution schedule or treat template spelling as proof.

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
events, constructs the selected tagged-carrier alternative in an enum lifetime
slot, and publishes that slot only through the matching enum lifecycle event.
Whole-enum move and tag-only switch observation use the exact MIR places and
obligations. The exact adjacent Move -> Replace -> Drop schedule also admits
whole-enum move assignment: native assignment replaces the destination's
active alternative, then MIR destroys only the moved-from staging shell.
Generated C++ names and native tagged storage are representation choices; neither
selects ownership behavior or identifies a source enum. Owning enum
call/return is admitted only through an exact verified-MIR transport shape.
Named and fresh arguments are moved from their one lifetime slot into the
native by-value parameter, parameter entry moves into its lexical slot, and a
transformed function, method, or exact lambda placement-constructs its result
in the caller's value-rooted slot. An exact captureless lambda invocation
stages named and fresh arguments through the same ordered `CallInput` proof
as a function call. The paired success edge arms that slot;
failure leaves it empty and retains MIR's cleanup order. Representation-only
moved-from shells are destroyed immediately after their verified transfer.
A call result returned unchanged instead constructs directly in the calling
body's own result storage. It shares the class direct-return placement proof:
the success edge arms the result obligation and the return path transfers it
out exactly once. The callee must also satisfy the call-result slot proof's
exact source-function or captureless-lambda boundary. The value-rooted place
names that storage and declares no slot. When a fallible drop, such as a
moved-from parameter's, precedes the return, MIR's unpublished-result
reparent keeps the obligation, and its rollback destroys the caller storage.
An active payload-enum return must be published by a fresh `PayloadConstruct`,
a verified `Move`, or that direct call. A failure-free call's owning result,
and a fresh variant whose publication cannot be fused because later cleanup
such as another local's fallible drop intervenes, instead wait in their
value-rooted slot; at the Return the caller's result is constructed from that
slot, and a failure on the way destroys the still-unpublished value. Any other
return fails admission rather than assigning into uninitialized caller
storage. A cleanup-free enum's value-rooted place, which a call borrows for a
fresh temporary, is the SSA value itself and has no separate local. A borrowed
payload extraction's value is not an object even when its type is such an
enum: its place, including the address a reference binding retains, names the
active payload inside the subject.
Constructor arguments use the same proof, including fresh unit variants.
A fallible ordinary source call may also construct its owning enum result
directly in a binding when its sole executable value use is the first
instruction on the Invoke success edge: an exact unconditional Initialize
reparent from the result obligation to the initially inactive binding
obligation. The original value-rooted MIR storage and drop identities remain
verified metadata. Emission marks the binding's native representation engaged
only on call success and erases only that matching Initialize's redundant
representation move/retirement. Non-immediate or non-unique publication keeps
the ordinary value-rooted representation. The admission and emission paths
share the same publication query; no source lifetime or cleanup is removed.
The analogous exact `Move` value-to-binding publication preserves the original
source object, its move, and its scheduled moved-from-shell cleanup. Only the
intermediate value-rooted native object is omitted: its sole use must be an
adjacent Initialize in the same block, with distinct source/value/binding drop
identities and an identical cleanup type. Temporary destinations, intervening
instructions, multiple consumers, and incomplete transfer evidence retain the
existing representation path.
A fresh cleanup-free class construction that is one alternative field fuses
into the variant braces only when it is adjacent to its `PayloadConstruct`.
Otherwise, when its constructor ends the block at an Invoke or sibling field
arguments are prepared after it, the construction runs at its MIR position in
representation-only storage. Its success edge must dominate the sole
`PayloadConstruct` use, the class must move without defined failure, and MIR
must record no value-rooted place or drop for it. The alternative moves the
completed value, and the moved-from shell is destroyed immediately afterward.

The same exact transfer proof stages owning array and result operands
into their value-rooted slots. Empty-slot relocation must be defined-failure
free; this is distinct from assignment into a live destination, whose old
value may have a fallible destructor. Consuming `PayloadExtract` initializes
its one pattern binding directly for every admitted value family, including
nested enums, instead of reading an uninitialized intermediate slot.
Recursive enum cleanup emits distinct local sequence names at each depth:
an inner sequence's initializer must read the outer record/unwinding state,
not a shadowing, not-yet-initialized local. Both ordinary and secondary failure
reports are checked by the composition runtime matrix.

Payload enum rows use backend-private `payload_variant`: a recursive native
union followed by a compact active tag, with direct tag-dispatched native
retirement. Native alternative `k` is verified MIR variant `k`, unshifted, and
the backend-private empty shell is the final native alternative, so the live
discriminants of a two-alternative enum are 0 and 1. Construction,
`payload_get`, switch cases and cleanup recipes all spell the MIR ordinal
directly. A small passive carrier often travels whole, as one native scalar,
through call results, matches and `?` propagation. A leading tag, or ordinals
shifted past a private alternative zero, kept native optimizers from folding a
later tag test back into the branch that selected the alternative; the
trailing tag and unshifted ordinals are the layout Clang folds. GCC 15 instead
loses 6–10% on scalar tag loops with the unshifted ordinals; the RT-11 row in
the optimization plan records that open tradeoff. Constrained-default special members preserve trivial copy/move/assignment and
destruction for passive members, including inert byte transport. Ordinary
integer enums are unchanged. Payload enums are accessed through
`payload_get<Index>` selected from the sealed variant inventory, under the
existing MIR active-tag proof. This is not a public layout/ABI guarantee or
a source-visible unchecked access operation. It removes standard-library
visitor-table dispatch from retirement without changing which GTI cleanup
obligations run. Native copy/move/assignment availability follows the member
representations; it cannot admit an operation absent from GTI's sealed traits.

For private passive SSA and binding transport, a construction-only member
such as a class containing a reference still needs native copy transport.
When every alternative is trivially destructible and nothrow copy-constructible,
but at least one is not copy-assignable, the carrier's private native copy
assignment reconstructs the selected alternative instead. Self-copy is inert;
the source tag, including the private vacant state, is preserved. This does not
enable GTI assignment: semantic traits and MIR admission still reject assignment
of a construction-only source type. Throwing-copy or nontrivially destructible
alternatives do not receive this passive fallback.

The carrier starts with only its private empty (final) alternative active. Construction
starts the selected union-member lifetime, then publishes its tag; an exception
before publication leaves no complete alternative to destroy. Recursive empty
union tails contain no resource. Native moves retain a valid moved-from member
until MIR's scheduled shell retirement. Cross-alternative assignment retires
the old member before constructing and publishing the replacement; same-tag
assignment retains its member lifetime where member assignment is available;
construction-only transport uses the guarded reconstruction above. Self-assignment
is inert. A failed
cross-alternative native construction leaves a private vacant tag, which can
only be retired or reconstructed, not exposed as a GTI value. GTI defined
failure remains governed by MIR's explicit success/rollback edges, not native
exceptions. Independent emitted-carrier tests cover empty/unit alternatives,
equal payload types at different indices, alignment, nested carriers/arrays,
native construction/move/assignment failures and exact retirement counts;
source runtime tests retain failure-cleanup and sanitizer coverage.

Cleanup-free enum fields may fuse a producer with its exact adjacent constructor
`Initialize`. The source constructor, declared field, root value, unique
initializer, destination and following full-expression boundary must agree.
The enum must have only scalar payload fields, be borrow-free, trivial to drop
and move-assignable, with no other runtime use or value-rooted place. The native
carrier independently asserts trivial move assignment. Assigning the fresh
alternative directly into the private initialized empty shell preserves operand
order and the source publication point without a redundant staged enum or native
object-lifetime restart. This is a representation operation, not a source
assignment. Substituted fields, duplicated roots, borrowed state, effects and
mismatched boundaries retain the ordinary staged path or fail admission.

A bounded closed-tag-domain proof exposes an exhaustive switch's impossible
default edge to the native optimizer. It independently matches the exact
payload enum/type/traits and copied dense alternative inventory, each unique
typed enumerator case, and an empty `Unreachable` default block. The source
must be an initialized entry reference parameter dereference or an exact
immediate enum field of an ordinary read-only source-method receiver. Only
inert full-expression/read metadata may precede it, and entry cannot have a
backedge. The receiver rule relies on verified call-boundary availability of
the complete object, not merely the method's read-only declaration. Semantics
rejects calls after a projected field move; MIR independently checks receiver
operands against its solved projected-place availability. Constructor bodies
begin after required fields are initialized, and destructor bodies precede
automatic field retirement. The focused moved-enum-field/observer regression
retains that invariant alongside the general projected-field ownership tests.
An exact passive transfer extends that entry proof to a consuming source
method: a direct, still-available enum field moves into an unavailable local
match subject through adjacent `Move`, `Initialize` and full-expression
instructions. Producer/result identity, source field/type/traits, destination,
available-to-moved ownership metadata and the matching final boundary must all
agree. The enum carries neither active cleanup nor borrowed state. No other
instruction may precede the switch, and the original no-backedge and complete
alternative checks still apply. The destination inherits a valid discriminant
from that exact transfer; a local declaration or an exhaustive switch alone
establishes nothing.
Constructor/destructor bodies, unrelated local/value roots, mutable receivers,
previously moved state, intervening effects and incomplete/substituted rows do
not qualify. This distinction excludes the carrier's private empty/vacant
shells; an exhaustive-case count alone is never sufficient. The proof emits
`mir_closed_enum_unreachable_v1` only on that switch edge. Ordinary MIR
`Unreachable`, explicit defaults and defined-failure paths retain their existing
abort/diagnostic behavior. Mutation tests cover omitted, duplicate, substituted
and mistyped cases, unavailable sources, wrong fields/formals, effectful entry,
changed defaults and backedges.
Transfer mutations additionally cover altered producers, initialization targets,
availability, ownership endpoints and intervening cleanup/raw effects.

Concrete owning pack parameters also enter explicit lifetime slots. Each
verified entry-active `PackElement` binding obligation selects one flattened
native argument and moves it into its canonical MIR place. Expansions and
folds access that slot, not the moved native parameter shell. Copy stages
leave it live; move stages and reverse normal/failure drops follow the exact
MIR lifecycle events. In particular, an unused owning pack cannot delegate
destructor failure to native parameter scope and skip the caller's cleanup.
Constructor-local packs use that same concrete layout and entry obligation
proof when forwarding into a field constructor. A failure cleanup accepts only
the exact element ordinal, flattened parameter type, source binding and active
Binding drop; it does not reinterpret the pack root as one owning parameter.
Completed fields and remaining arguments retain their existing reverse MIR
rollback schedule. This applies equally to unrelated generic envelopes and
source-defined list/deque/queue nodes, without recognizing container names.

An admitted direct class/struct field uses the class initializer schedule's
exact payload construction, `This.field` publication, value-rooted reparenting,
full-expression endpoint, and construction-rollback row. A transformed
failure-capable constructor may require a temporarily disengaged native shell;
the C++ representation therefore reserves a private `std::monostate` as the
final alternative. It is neither a source variant nor observable tag state. Field
publication retires only that proved empty shell and placement-move-constructs
the enum from its exact value slot. Its generated transfer must be failure-free;
neither publication nor rollback reconstruction requires enum assignment. An
enum with a construction-only class payload therefore remains nonassignable
while supporting ordinary owning-field construction. The joint class/enum
definition scheduler emits every payload class before its enum
and that enum before a class embedding it, rejecting a recursive by-value
cycle. Concrete generic owners/callables use the same exact lifecycle proofs;
generic arguments are not a reason to bypass or reject those proofs. The
declaration-bound inline optional, uninitialized and mutex storage capabilities
contribute their payload as a native layout dependency, while pointer-backed
storage does not. This also orders retained generic-owner member specializations:
a mutex embedding a vector needs the complete vector representation before a
concrete mutex constructor declaration can instantiate that owner. Exact static
consuming methods retain their receiver capability alongside enum result
placement. If a later argument fails, a prepared enum obligation still drops
at its verified MIR point even when direct native parameter construction
erases the prepared representation slot. The same exact transport proof maps
that obligation to its backing object; a named source retains its disarmed
shell for its own separately scheduled drop. Polymorphic enum dispatch, native layout, and direct enum captures
remain outside this vocabulary and are rejected semantically before MIR.

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
Cleanup-free constructed fields use the same body-ordered publication route
when a nested class argument needs a prepared parameter slot. Proving the
consuming constructor failure-free removes its failure-propagation marker,
not the prepared input handoff: the shared transfer-only proof checks a
bijection between its `TransferOut` events and prepared inputs, independently
of the status-boundary proof. The field still requires the exact selected
constructor, a proved non-failing target, adjacent `Initialize`, and its
construction-only empty representation. No result activation or drop is
invented for the cleanup-free value. Native member-initializer fusion remains
an optional equivalent representation, not a prerequisite for this composition.
A payload enum's private inactive alternative contains no active payload or
borrow, including when its alternatives mix owning values and references.
Construction-only empty-state admission may therefore recurse through such an
enum field. It does not make direct reference fields default-constructible or
authorize reassignment of borrowed objects. Publication still proves the exact
producer, field `Initialize`, value-to-field reparent and rollback schedule.
Borrow-carrying producers additionally retain their complete dependency set:
nonempty sets require the corresponding checked field-loan publication, while
empty sets require explicit selected-alternative evidence. The native move
carries existing reference addresses; it never invents a pointer to an inactive
payload.
Ordinary native constructor overloads that publish fields from the same MIR
body use the same proved private empty state as their failure-form overloads.
They must not implicitly call a source default constructor before MIR field
publication; that constructor may be deleted or have observable behavior.
Body-ordered owned-parameter fields compose with an independently verified
native base initializer. The base follows its exact selected status entry;
the destination fields retain their private empty shells until MIR reaches
their own `Move` and `Initialize`. An input parameter's original obligation
therefore remains armed if base construction fails. Only successful field
publication reparents it into field rollback. The field transfer, parameter
cleanup and base success/rollback proofs are all required; this composition
does not hoist an owning move across fallible base construction.
An independently verified empty interface base contributes dispatch identity,
not source storage, initialization or cleanup. Its structural proof remains
shared between the private empty-field and empty-shell admission paths.
That whole-shell proof still excludes state-bearing bases and non-generated
interface lifecycle operations; it is distinct from the per-class destructor
guard and independently selected state-bearing base schedule described above.
A cleanup-free class field may have lexical lifetime without an active cleanup
obligation. Such a field can accompany a guarded owner only when its concrete
class has generated, observation-free destruction, no active cleanup state,
and either the same recursively proved private empty representation or the
exact native bound-field publication described below. The exact
reverse field-drop schedule remains mandatory; no user destructor or active
child may be hidden behind the passive field marker.
A completed cleanup-bound field containing native references cannot be reset
to an unbound empty object. For the bounded exact owned-parameter field route,
rollback instead runs its verified cleanup recipe in place: the source
destructor guard is disarmed and owning children are drained to inert states,
while reference bindings retain their native object shell. Enclosing native
destruction later destroys that shell without repeating GTI cleanup. Admission
requires a unique concrete non-polymorphic class with no bases, an exact cleanup
recipe, guarded source-destructor composition when present, and recursively
emptyable or retained-cleanup children. This is not a default-construction or
empty-state witness. The exact Move/Initialize and value-to-field rollback
identity must already have been proved; arbitrary initializer reordering is
not admitted. The consumed source formal's later MIR drop can be represented
as inert only under that same exact transfer proof. Destructor failure still
uses MIR's original primary/secondary failure record and emergency endpoint.
The recursive proof requires a concrete nonzero array extent and exact element
type. A concrete polymorphic element may participate only through independently
verified stateless interface bases with generated default construction and
cleanup: those bases add dispatch identity but no source initialization or
owned storage. Abstract elements, state-bearing bases, stale base identities,
borrowed state, and source-defined interface initialization remain excluded.
An owning array aggregate may publish directly into its constructor's exact
This-rooted field. The proof binds the adjacent aggregate/Initialize pair to
the selected initializer, full-expression, and rollback obligation. The writer
retires only the private empty array, placement-constructs its elements from
the already-owned MIR slots in operand order, and retires the moved shells.
The following Initialize retains publication authority; no default-constructed
SSA array, duplicate source construction, or implicit element copy is added.
When an explicit class-field construction remains in an ordinary constructor
body, the exact `ConstructorFieldResultSlot` schedule constructs the selected
class prvalue in its final field when the status constructor's success edge
immediately reparents its one result obligation into that field's rollback
obligation. The backend retires the proved disarmed empty representation and
constructs at the field address. Failure leaves a disarmed native shell for
enclosing representation cleanup; success publishes the field through the
existing MIR stage. This requires no element copy or move, and preserves an
immovable element's address from its constructor onward. Class-call results
and non-adjacent publication retain the value-rooted lifetime-slot route and
its separate transfer proof. The ordinary C++ route uses factory construction
so it never names an undeclared SSA value.
A class call result can initialize a constructor field as its selected member
initializer whether or not the callee can fail. A cleanup-free result has no
obligation. A failure-free call constructs it in representation-only storage
immediately before the field `Initialize`, and a transformed call does so on
the Invoke success edge leading to it. The field's empty representation is
then replaced by the moved value. A call that binds a temporary to a reference
parameter, as in `lap(convert(Feet(30)))`, ends that fresh-argument
(`CallArgument`) loan right after the invocation, so its `EndBorrow` precedes
the field `Initialize`. Only the producer's own argument-loan ends may come
between them: they carry no representation, and any other instruction still
breaks adjacency. An owning result from a failure-free call
arms its value obligation with the call's own `Initialize` event, as a
failure-free constructor does, and the field `Initialize` reparents it into the
field's rollback. The value then moves from its lifetime slot into the field.
Either kind of call may be a source member whose receiver passes the shared
receiver proofs: a typed borrow, as in `kept(source.make())` or through a
reference parameter, or a consuming (`&&`) member's `Once` receiver, as in
`kept(std::move(box).release())` or `kept(Single(v).release())`. Neither
receiver is transferred at the call. A consumed receiver's full-expression
`Drop` follows the field `Initialize`, and MIR's failure edge from that drop
rolls the published field back; the backend emits that schedule unchanged.
Field declaration defaults such as `Tracked kept = Source(3).make();` run as
the same constructor-initializer stages. A cleanup-free receiver moved out of
a binding is instead a prepared parameter that the call transfers like an
owning argument, as in `kept(std::move(source).take())` where `source`'s class
needs no cleanup. Both producers count that transfer among the call's
prepared inputs (`preparedReceiverStage`), so no receiver drop follows the
field `Initialize`. A later fallible argument, as in
`kept(std::move(source).take(1 / d))`, materializes that receiver in its
prepared slot, and a moved or copied argument such as
`kept(build(std::move(source), 1 / d))` in its own. The failure constructor
boundary admits their failure-edge Drops as ordinary prepared-argument
cleanup, as it does for a fresh argument result, rather than as rollback of an
object field. A virtual member call, as in `kept(maker.make())` through
a `Maker &` or `kept(factory.build(d))` through a `mut` interface reference,
takes the transformed route through its exact virtual failure contract: MIR
gives it `VirtualCall` propagation and an `Invoke` edge whose success edge
activates the result. The proof requires the status callee that call emission
selects (`virtualStatusCallee`), which may be a pure-virtual or interface
contract root, and the same receiver proofs and input transfers, including a
consuming virtual member. MIR's failure effects never prove a virtual method
returning an owning class failure-free, so such a call never takes the plain
virtual route, and the failure-free producer keeps requiring static dispatch.
If a constructor has no reachable normal `Return`, its initialized fields never
transfer to a completed object. The field-slot proof permits zero final object
publications only after recomputing CFG reachability and proving that absence;
it does not trust the stored reachable flag alone. Field initialization,
reparenting, rollback Drops, and emergency termination remain unchanged. A
constructor with a normal completion still requires its exact publication
event; a terminal constructor does not invent one to satisfy native lowering.
Nested receiver-field access is emitted only when every projection resolves
through an exact class field and representation row. Missing or additional
lifecycle uses reject the body rather than changing source evaluation order.
A read through a receiver's stored class-reference field uses the exact
`Field, Dereference, Field...` MIR projection path: the reference field must
have its copied reference-type row, and every trailing field must resolve from
the referent type through an exact field row. The C++ alias relies only on the
language's existing implicit reference-member dereference; unsupported
projections fail body admission.

Nested checked-owner arrow chains use those same exact field rows. MIR keeps
each projected receiver rooted in its preceding call-result loan, whose
referent type comes from the exact target. The backend does not reinterpret a
dependency owner as a returned pointee or synthesize missing dereferences.

A fixed-array drop with a concrete MIR schedule uses the same lifetime-slot
boundary for its aggregate storage. The C++ representation recursively visits
the recorded extents in reverse order, invokes the exact leaf failure-form
destructor, and destroys each completed leaf once. Ordinary cleanup preserves
the first defined failure while completing the remaining leaves; cleanup while
another failure is active retains the existing double-failure termination
rule. The emitter rejects arrays whose schedule, leaf class, or destructor is
not already verified by MIR.

An owned field may follow exact scalar parameter-to-field copies: each prefix
is one passive parameter Load, its exact This-field Initialize, and matching full-expression
boundary. The scalar stores still execute in the MIR body; structural moves
cannot observe them or raise. Calls, computations, borrowed/owning loads,
foreign fields, mismatched initializer metadata, and mismatched boundaries are
not this proof. This admits a generic callback owner with a scalar identifier
before its callable field without accepting arbitrary initializer reordering.
An exact stored-reference prefix is also admitted: each parameter Load, Stored
Borrow and full-expression boundary must match the existing reference-binding
proof and immediately precede the owned Move/Initialize pair. This preserves
the mutex guard's reference-plus-token construction without accepting arbitrary
borrow-producing expressions or moving a field ahead of observable work.

The sealed native mutex-storage constructor also admits an adjacent prepared
input for a cleanup-free payload. Its source-parameter Move, CallInput carrier,
inactive PreparedParameter obligation, exact Initialize, and constructor
TransferOut must agree in type, place, order, and identity. The carrier adds no
observable cleanup; its initialization and handoff are still mandatory MIR
events. This proof does not generalize to active-cleanup payloads or arbitrary
constructor argument expressions.
Ordinary source constructors may use the same prepared-input schedule for a
passive generated class (see below) with generated copy, move, and
destruction, no state-bearing base, and no active cleanup. Every prepared
input has a distinct obligation and exactly one matching constructor handoff;
scalar sibling inputs retain their argument positions. A conservatively
failure-capable target is eligible only when its complete verified body is
accounted for by the exact passive parameter-to-field moves, lexical parameter
drops, and inert boundaries.
The failure summary remains unchanged; unrelated constructor work cannot be
hidden behind the native initializer spelling.

An all-owned-parameter initializer list may stay in native member-initializer
form only when every declared initializer has one exact source-parameter move
and the complete field order is proven. Cleanup-free fields require no
invented drop record. An active-cleanup field additionally requires its value
drop, construction rollback reparent, field drop, and one unconditional
transfer-out in the verified constructor prologue. Mixed passive and active
fields are admitted only when each field independently satisfies its matching
schedule and no other lifecycle event mentions those obligations.
Several active fields may share a standalone transfer instruction; the proof
matches each field's distinct obligation, not the instruction's event count.
A passive class parameter can retain its lexical shell drop without inventing
a field activation. The prologue still permits only the recorded field moves
in declaration order, followed by their exact parameter-cleanup schedule.
A borrowed-carrier parameter such as a text view enters the body carrying its
caller's dependency as an entry loan, and the prologue ends that loan after
the parameter's move. The end record has no producer, never escapes, carries
no lifecycle, failure or boundary metadata, and spells as a comment, so it is
admitted beside the field moves rather than forcing the generated constructor
of a designated or positional struct out of the C++ text vocabulary.
MIR independently proves when a moved parameter's shell retirement cannot
invoke a source destructor; the backend does not infer this from C++ moves.

When earlier initializer work can fail, a later owned parameter move stays in
the verified constructor body instead of being pulled into native initializer
order. The same body-staged publication proof accepts cleanup-free moves of a
passive generated class (defined below): the exact parameter Move, field
Initialize, and full-expression boundary remain mandatory, while value and
construction-rollback obligations must be absent. Its inert parameter shell
drop remains recorded. Custom lifecycle, borrowed state, and active cleanup
do not use this passive route. A trivially dropped, borrow-free scalar, enum
(including a payload enum whose payloads are trivially dropped), raw pointer
or fixed array of such values has no parameter shell drop either: no
obligation names its formal, its moved value or its field, and its `Move` and
`Initialize` carry no lifecycle event. The proof requires that complete
absence. The field then starts from its passive empty state in the
member-initializer list, and the exact `Move` and `Initialize` publish it in
MIR order, as in `Gauge(int32_t tag) : ledger(Ledger(tag, divisor)),
odd(tag % 2 == 1), tag(std::move(tag))`.

A passive generated class (`passiveGeneratedClass`) has generated
destruction, plus generated copy construction where a route copies it, no
active cleanup or drop state, no state-bearing base, and only passive fields.
A passive field copies bitwise and its destruction runs no source cleanup: a
scalar, an enum, a raw or null pointer (a `c_string` is a raw pointer), a text
view, a function item with its function identity, a fixed array of passive
fields, or a passive generated class. The rule names representations, not
library types. A function item is a stateless identity tag
(`function_item<id, R>` natively): MIR calls the named function directly,
never through the stored object, so a generic `Op<F>` instantiated with a
function name copies and moves like any passive class. A text view's
dependency on its owner, when it has one, exists only as a MIR loan, which has
no representation, so it changes neither the copy nor the destruction. A class
that only holds text views is therefore passive and not a borrowed carrier.
Semantics keeps ownerless text in such a by-value field of another class, and
MIR records no loan, value or rollback obligation for moving it. So
`shelf(Name name) : v(std::vector<int32_t>()), name(std::move(name))`, where
`Name` holds one `std::string_view`, publishes in body order exactly like a
class of scalar fields, and `name(name)` uses the native parameter copy below.
Every route that consults the predicate sees this same representation fact:
field assignment, conditional class joins, prepared call inputs, closure
captures, parameter shells and constructor publication. Each route still
checks borrowed state, traits and its own MIR schedule. A semantically
dependent value, such as a local `Name` viewing a named owner, reaches only the
local copy, move and by-value call routes that semantics admits, and those
copy its bits.

A moved `std::string_view`, `c_string` or function item uses the same
drop-free route without an `Initialize`. MIR stages none for these types: the
constructor record publishes the formal's exact `Move` under the shared
`mirRecordPublishedConstructorInitializer` rule, and that value has no other
consumer. The body emits the `Move` and its field store in MIR order, and the
field keeps its passive native default state until then, as in
`shelf(std::string_view name) : v(std::vector<int32_t>()),
name(std::move(name))`. A text view or C string also carries its caller's text
as the formal's entry loan. A by-value field may hold only ownerless text
(semantics reports `GTI-S2045` otherwise), so the move publishes no dependency
and MIR records no `Stored` loan for that field. The proof admits borrowed
state only in exactly that shape: the formal carries it through one entry loan
with no producer, escape, stored field, lifecycle binding, two-phase
reservation or extra dependency. No loan derives from that entry loan, every
record naming it is its comment-only `EndBorrow`, and no loan publishes into
the field or is produced by the moved value. A user-defined borrowed carrier,
such as a class with a `T&` field, is a different family. Its field retains
the dependency through a `Stored` publication, and its native class has no
empty representation, because its references must be bound when the object is
constructed. Such a field uses deferred field storage, described next, unless
it is its class's only carrier and its first field, which the first-field
prefix routes below publish.

A field whose type is a passive bound shell (a class or struct carrying
references, with generated, observation-free copy, move and destruction, no
active cleanup, user destructor, base or capability) and no native empty
state can follow other initialization work whenever it is not the first
field or a state-bearing base precedes it. The C++ class then declares that
field, and every other bound shell of the same class, in deferred storage: an
anonymous union whose other member is an inert byte with a default member
initializer, `union { unsigned char __gti_deferred_cursor{}; Cursor cursor; };`.
The field keeps its name, type, size and alignment, and every access spells
it as before. The inert byte is active until a constructor publishes the
field. A native member initializer still publishes the field where an exact
native route proves it, such as the first-field prefix, a constructed
sequence, or a parameter copy; otherwise the field is left out of the
member-initializer list and the verified body publishes it at its MIR point.
A moved or copied formal is constructed into the storage with
`std::construct_at`, and a carrier constructor builds in place there through
its ordinary or status-tag overload, with no prior `std::destroy_at`, since no
field value exists yet. The `Stored` loan that follows the publication remains
a proof-only record. A moved formal keeps its lexical shell drop, and the
body-staged move proof requires the exact cleanup-free schedule: Move and
`Initialize` without lifecycle events, no value or construction-rollback
obligation, and the field's one `Stored` loan published from the moved value
right after the `Initialize`, sourced at the formal, whose input loan ends at
once. A failure before the publication leaves the inert byte, and the caller
ends the unpublished object; a failure after it leaves a bound shell whose
ending runs no source code. Neither needs a rollback, so the failure form's
per-field rollback proof accepts deferred storage as it stands, and owning
siblings follow their own verified rollback. The union never records which
member is active, so the class asserts at native compile time that the shell
is trivially destructible. Its generated copy and move are trivial copies of
the bound references and scalars, so the enclosing class's special members
copy and move the published field unchanged. Deferred storage is therefore
restricted to shells whose every leaf is a reference, scalar, text view,
pointer, function item, plain enum, fixed array of these, or another such
shell; a payload enum leaf stays outside. A generic owner may share one native
class template across instances, so deferred storage applies only to
non-generic classes for now.

This keeps GTI's construction order for every producer semantics admits for a
carrier field: the move or copy of an exact carrier formal, or a construction
from exact formals, including a carrier constructor that checks its input or
prints, as in `Built(int32_t& value, int32_t divisor) : ledger(Ledger(3,
divisor)), cursor(Checked(value, divisor))`. If the ledger fails, the checking
constructor never runs. A copy of a formal that nothing changes is still
published by the native parameter-copy route, ahead of the earlier work, as
for passive classes: the copy reads the formal's entry value, runs no source
code and cannot fail, and the published shell needs no rollback. Remaining
stages are a class containing a class with deferred fields (an empty state for
the containing class), carriers with active cleanup, and generic owners.

A borrowed payload enum, such as `enum Slot { ready(int32_t& value), empty }`,
also retains its caller's dependency in the field, but its native value has a
passive empty state. Moving the formal into the field initializes it and then
hands the formal's guarded entry group to the field's escaping `Stored` loan
through one `Borrow` from that group, followed directly by the group's
`EndBorrow`. The enum value already holds the reference, so that publication
is proof-only (`storedFieldValueLoanProducer`), as for an in-place payload
construction, and the move takes the drop-free route above in MIR order: after
work that can fail, as in `Gauge(Slot slot, int32_t divisor) :
ledger(Ledger(1, divisor)), slot(std::move(slot))`, or after an earlier read of
the formal. The proof (`formalEntryGroupPayloadMove`) admits borrowed state in
only that shape: the transfer of the exact moved value is the field's only
publication, no other loan derives from or protects the formal's group, and
every other record naming the group is a comment-only `EndBorrow`, such as the
one on a failure route. A copy, `slot(slot)`, uses the native copy route
below.

Such a move is itself failure-free (see the MIR failure summary), so a
constructor whose other work cannot fail, as in
`Gauge(int32_t tag) : odd((tag & 1) == 1), flagged(is_odd(tag)),
tag(std::move(tag))`, uses the ordinary form. When the native owned-field
proof cannot hoist every owned move, `cppMirOrdinaryBodyStagedParameterMoves`
admits the same body-ordered publication there for drop-free moves only:
the constructor's summary must be failure-free, its fields must have the
empty state, no reference may bind natively, and natively copied fields must
form the complete first-field prefix. Body admission and the planner's
initializer list consult that one proof. For a constructor that either form
can body-stage, passive scalar parameter copies stay in the MIR body schedule
(`cppMirCopyParameterFieldBindings`), whichever form is selected.
In the failure form, a native copy outside that first-field prefix may still
publish beside body-staged moves when it copies a passive generated class
(`failureConstructorHoistablePassiveCopy`). The copy query already refuses a
formal that earlier initializer work may write, and the copy itself runs no
source code and cannot fail, so publishing it ahead of the body schedule is
unobservable; the copy-only failure form already publishes it that way. Its
records must still be MIR's adjacent `Load`, `Initialize` and full-expression
boundary in one reachable block, and every initializer that copies a formal
must have its exact copy witness, so no copy is hoisted past a reordered or
malformed one. In
`Shelved(Pair moved, Pair copied) : ledger(Ledger(31, divisor)),
moved(std::move(moved)), copied(copied)`, `moved` starts empty and is moved
after the ledger in MIR order, while `copied(__gti_mir_arg_1)` publishes in
the member-initializer list, in either declaration order. A borrowed carrier
keeps the prefix rule, because it publishes a `Stored` loan. The ordinary form
keeps it too: a class formal's lexical drop keeps its constructor
conservatively failure-capable, so no class copy reaches that form today.
The native owned-field proof also refuses a passive scalar sibling that copies
the very formal a later initializer moves, as in `Copied(int32_t tag) :
first(tag), second(std::move(tag))`: the native list would move the formal
before the body copy ran, and the formal's shell is never declared. That
constructor publishes in body order in both forms.

An exact by-value parameter copy may also publish one field through the native
member-initializer list when the copied class is a passive generated class
with generated copy/destruction: only recursively passive scalar, enum,
pointer, text-view, array or generated-record state, no state-bearing base,
and no active cleanup.
This MIR shape carries one reachable parameter `Load`, one matching initializer
metadata row, an exact field `Initialize`, and one full-expression boundary;
the copied parameter's native scope destruction represents its trivial MIR
drop. The scalar copy route, including passive raw-pointer value copies,
requires the same explicit `Load`/`Initialize` pair. Copying a raw pointer
introduces no dereference, loan, or ownership authority. Custom lifecycle,
extra uses, forged ownership and ambiguous fields fail closed. Several such
copies may precede body-staged owned fields when their exact Load/Initialize/
boundary triples form a contiguous declaration-ordered entry prefix. Borrowed
carriers retain their additional publication/loan-end witness. The prefix may
not cross base construction, computations, later owned publication or failure
edges. Native initialization realizes only that proved prefix; subsequent owned
fields remain empty until their original MIR publication and rollback execute.

The native member-initializer list reads a formal's entry value: the C++
argument, or the prepared value published before the base. MIR reads the
formal at its copy `Load`, after every instruction on a path into it. A copy
therefore publishes natively only when none of that earlier code can change
the formal's own storage. Copies and read-only borrows leave it intact. An
assignment, initialization or drop of one of its places, a move, an address,
a mutable borrow (a `mut` reference argument or receiver, or a `[mut &x]`
capture) or a mutable loan can change it, there or later through the borrow.
A place reached through a dereference belongs to another object.
`Pair(mut int32_t seed) : first(bump(seed)), second(seed)` used to copy the
unwritten argument this way. The owned-parameter move proof applies the same
rule, and so does a record-published closure copy, which then has no route at
all because a closure has no empty state. A copy kept out of the native list
stays in MIR order. A scalar, enum, raw pointer or fixed array field starts
from its passive empty state, and the exact `Load` and `Initialize` publish
it. A cleanup-free class copy, whose `Load` and `Initialize` carry no
lifecycle event and whose value owns no drop obligation or place, declares a
transient at its `Load`; the `Initialize` moves the transient into the field,
which starts from its native empty state. A class without that empty state
stays outside the route. The same body-ordered class copy serves any copied
place, such as the referent in `Holder(Point& point) : point(point)`.

A closure copied from its by-value constructor parameter into a field, as in
`Holder(F f) : callback(f)` instantiated with a lambda type, uses the same copy
route. MIR stages no `Initialize` for a closure-typed field: the constructor
record publishes the parameter's copy `Load` under the shared
`mirRecordPublishedConstructorInitializer` rule, which optimization also
preserves. The copy proof accepts that record publication in place of the
explicit `Initialize` when the field has no active cleanup, the loaded value
has no other use, and the closure is copyable and borrow-free with only scalar,
payload-free enum or passive generated-copy class captures. Copying the native
closure object then replays the GTI copy exactly, and destroying it runs no
source code. A closure has no native default state or assignment, so the field
is copy-constructed in the member-initializer list and the body aliases the
formal instead of copying it again. A class capture leaves a lexical drop on
the formal; it stays in MIR and emits as a trivial drop. The body may use the
formal again, as in `F spare = f;` or `call(f)`, and MIR can record that later
read as another bare place of the formal's binding. The copy proof therefore
names the formal by its binding: every bare place of it, for a class formal as
for a closure, spells the one alias, and none declares a lifetime slot that
nothing constructs. When the body can fail, the caller ends the native object
after the failure, so the failure-form constructor accounts for every lexical
field. A closure field has no native empty state, but the record-published
copy constructs it in the member-initializer list and the copy proof has
established that ending it runs no source code. A struct-capturing closure
field is therefore retained as a passive shell, wherever it sits in the field
order, exactly as a trivially dropped closure field needs no rollback. Every
closure instance
of a class shares one native constructor template, so a record-published
closure copy keeps its sibling fields body-staged, as the owned-parameter form
does. All instances then have one member-initializer list, and their bodies
differ only behind the existing destructibility dispatch. Owned-parameter
fields around the copy still move natively: the copy's exact `Load` and
boundary form an effect-free prefix of a later owned move, like a passive
scalar sibling, and the formal's unconditional scope-end `Drop` is one of the
complete owned prologue's lexical parameter drop records. Closures capturing
fixed arrays, payload enums or other closures remain outside this route. A
closure-typed class constructed as another class's member initializer uses
the constructed-field family below.

A closure moved from its formal, as in `Keeper(F f) : callback(std::move(f))`,
publishes through the constructor record as well, and the owned-parameter
proof binds the formal's exact `Move` to the field. The same record form
serves a payload enum moved into a field. Such a move may precede work that
can fail, in the constructor body or in a later sibling initializer. The proof
still binds it when it publishes ahead of every failure edge: only the exact
effect-free records of earlier initializers precede the `Move` (passive scalar
siblings, record-published closure copies, or stored-reference bindings), and
its own full-expression boundary follows it. The native member-initializer list
then moves the formal exactly where MIR moves it, in the failure form as in the
ordinary one, and the body only records that publication. A body with no
failure record cannot abort before the move, so the proof also binds a move
recorded after other initializers, which then run in the body after the native
list has published the field. That reordering is exact only while no other
record names the formal, apart from its lexical shell `Drop`. An earlier
initializer that reads the formal, as in `Sized(std::string_view name) :
length(name.size()), name(std::move(name))`, would read a parameter the list
has already consumed; such a move keeps the read and the move in the failure
form's body, in MIR order, through the drop-free route above. Whether the failure
form may keep the published field when a later edge fails is a separate,
per-field proof. A trivially dropped field needs nothing. A struct-capturing
closure field is retained as a passive shell under the copy route's capture
rule, because ending it with the caller's object runs no source code. The
moved-from formal keeps its lexical drop on both exits; it retires only the
native parameter's moved-from shell. A move recorded after any other
initializer work, such as a fallible sibling declared before the closure
field, is never hoisted across that work and stays outside this route. So does
a struct-capturing moved closure that is not copyable, whose retained shell
would need a destruction-only capture proof.

A shared-reference carrier with generated, observation-free destruction may
retain its bound native shell during constructor rollback. This is not an
empty-state capability: its references must already be bound by the exact
contiguous declaration-ordered parameter-copy prefix (Load, Initialize, dependency publication,
source-loan retirement, full-expression boundary), or an independently proven
native constructed-field publication. The shell proof recursively excludes
exclusive references on the copying route, active cleanup, user destructors, capability classes,
unions and bases. Later constructor work and failure cleanup remain in MIR;
neither reference binding nor user cleanup is reconstructed by the backend.

A nullable raw-pointer field default uses ordinary typed initialization: the
source SSA value stays `nullptr_t`, and the exact destination retains its
pointer type. The native initializer proof admits only that null literal with
its explicit field publication; unrelated pointer types or integer addresses
are not implicit initialization conversions.

An explicit class-typed field initializer whose value is one ordinary
`Construct` of the field's own type may publish through the same native
member-initializer list. C++17 guaranteed elision makes
`field(Type(arguments...))` the exact representation of MIR's
construct-then-publish schedule: the field is constructed once, in place, with
no intermediate object and no invented copy or move. Because C++ initializes
members in declaration order, the proof is whole-constructor: normally every
declared field must have its own explicit initializer in declaration order, each
construction argument must be either an exact copy-load of one scalar
constructor parameter or an exact source scalar literal, and the successful
instruction sequence must contain nothing but the named instructions and
metadata boundaries. Conservative constructor `Invoke` edges may split that
sequence only when an acyclic, exact-target MIR proof excludes every local
failure and unknown propagation in the called constructor chain. Each alternate
must propagate its matching failure record, either directly or after only an
exact cleanup-free generated parameter-shell Drop/Failure-boundary schedule;
active or user cleanup, alternate control flow, mismatched identities and
unaccounted blocks fail closed. The original MIR edges remain intact.
Parameter-shell retirement proves generated, observation-free destruction,
not copyability: a deleted copy constructor cannot invalidate a Drop that
performs no copy. Native parameter-copy initialization retains its separate
generated-copy requirement.
Native member initialization is
equivalent because those alternates cannot execute and the complete field,
argument, publication and cleanup order is still checked. Terminal containment
of a checked operation alone is not sufficient for this stronger proof.
A failure-free scalar parameter-to-field constructor is also a valid ordinary
target: its complete Load/Initialize/full-expression schedule proves the native
overload, even when its conservative failure summary is set. Floating and
plain enum fields, like integer fields, must have explicit MIR Initialize
stages; constructor metadata alone is not a substitute for that publication.
The explicit operand use prevents dead-computation removal from discarding a
grouped or forwarded enum initializer.

A construction argument may also be a passive closure formal, as in
`Outer(F f) : inner(Holder<F>(f))` for a lambda type `F`. MIR stages the
closure in an exact prepared argument slot, and the construction transfers that
slot out. A copy reads the formal place directly in one copy `CallInput` whose
Read leaves the formal available. A move uses the exact `Move` and move
`CallInput` of a moved passive class. The closure's native copy or move replays
the GTI one (the shared closure-copy rule). A class capture gives the copied
formal a lexical drop: its unconditional scope-end `Drop` and normal boundary,
and its failure-route `Drop` after an impossible child failure, are natively
trivial and stay in MIR. The child constructor is an ordinary target when its
closure copy or move publication, including that formal drop, is its complete
schedule. A closure-dependent field type has no program-wide spelling, so the
initializer names the field's own declared type,
`field(std::remove_cv_t<decltype(field)>(arguments...))`. That spelling is exact
in every instance of the retained constructor template.

The family composes with a selected base initializer and with owned-parameter
fields in the same native member-initializer list. The base keeps its own exact
native binding: one failure-free selected constructor whose scalar arguments are
the only instructions before its `InitializeBase` stage. That stage arms the
base's construction rollback, and the shared transfer retires it with the field
rollbacks. Any other base must be a stateless interface. An owned-parameter
field keeps its exact `Move`. Either its adjacent `Initialize` hands the
formal's drop to the field's rollback, or a cleanup-free carrier, such as a
moved closure, publishes from the constructor record. The consumed formal keeps
one unconditional scope-end `Drop` of its moved-from shell. The owned-parameter
proof accepts a move that this schedule claims as its complete prefix, even
where an impossible child failure splits the prologue into blocks. That
failure's route may retire the native base after the published fields, and the
formals that the native list consumed before the body ran. Such a retirement
may also be a fallible destructor whose failure only terminates cleanup
failure. The route cannot execute, and C++ performs each of these retirements
itself. Passive closure shells are natively trivial on this route too. In the
status form, a native constructed field with no active cleanup needs no
rollback beside any sibling, because ending the native object destroys it
without observation. A computed or prepared base argument, a fallible base
constructor and a child constructor that stages further fields in its body
remain outside this family.

A fieldless class with an exactly empty returning constructor body also has
that ordinary overload, including concrete implementations of stateless
interfaces. The proof requires an empty initialization list and passive base
and field surfaces; a conservative failure summary alone does not invalidate
the otherwise empty schedule.

A first field may instead be a cleanup-bound borrowed child constructed from
exact read-only reference or passive generated-copy formals. This bounded
prefix requires no bases and nothing before construction except those exact
argument stages. Every later field must have a separately proved private empty
representation and remains initialized by its original body schedule. Read
ownership metadata must name the same formal and leave it available. A passive
class copy additionally preserves its exact prepared-parameter activation and
transfer into the child; custom copies or active argument cleanup are excluded.
The success edge activates the child value's exact
drop; the adjacent field `Initialize` reparents it into the field's rollback
obligation, followed by the exact stored-dependency publication and source-loan
retirement. Only those argument/construction/publication instructions disappear
into native member initialization. Later body work, including failure, remains
in MIR order. Rollback consumes the same proved field drop and drains the
retained reference-bearing shell without default construction or a second
destructor call. Normal cleanup, original failure propagation and secondary
cleanup failure use the existing verified schedule. A selected non-failing child
can use its ordinary native overload. A fallible child instead uses its exact
status-tag constructor and the parent's existing failure-record out parameter.
On entering the parent body, its status is captured before the parent's result
is reset; the original MIR Invoke branches on that status. Child failure cannot
be turned into a known-success edge, publish the parent, or arm the child's
field rollback obligation. The child's unpublished shell must independently
support observation-free retirement. Multiple fallible native prefixes and
nonempty unclaimed siblings remain excluded;
this bounded route alone is not complete allocator/container validation.

Several borrowed fields may form one native prefix, such as two owners leasing
from one binding, only when every child is proved genuinely non-failing. A
member-initializer list cannot stop after a failed child. The no-failure proof
reads each constructor's success sequence, and the failure route of an Invoke
whose exact callee is itself proved non-failing must stay a passive, exact
schedule. One extension applies because that route cannot execute. It may roll
back fields this constructor already published, each through the exact
construction-rollback obligation armed on the success path, before the
parameter shells. A rollback whose destructor can fail is its block's Invoke,
whose failure edge reaches an empty block that only terminates cleanup failure.
A parameter shell MIR proves definitely moved out need not be passive, because
it is retired without its destructor. Every other drop, a malformed boundary,
an unknown failure or an unarmed rollback still fails closed. The emitted
constructor keeps its complete verified failure graph. A child uses
its ordinary native overload only when that overload is published, which for a
constructed-field schedule means no field still reports a status. Otherwise the
call site uses the child's status-tag overload, so it never names an ordinary
constructor that plain-body admission did not define.

The same exact first-field schedule also admits a passive reference-bearing
child with generated, observation-free destruction. This branch
requires no child value drop, success activation, field rollback obligation or
reparenting; it still checks the selected constructor, original Invoke failure
identity, adjacent field/dependency publication and both source-loan endpoints.
A cleanup-bearing child cannot use that passive branch. Exclusive fields require
the exact construction/movement proof below, never the generated-copy route.
The bound native shell is not an empty-state witness, and later field/body work
is not hoisted. This supports ordinary nested view construction and copied
field returns without inventing a second cleanup schedule.

Nested constructor inputs extend that prefix through a typed, pointer-free
argument tree. Each row names either an exact enclosing formal or an actual
selected ordinary constructor, never a reconstructed source expression. Every
claimed producer and prepared input has one matching SSA consumer; the normal
block sequence must match the complete postorder tree before field publication.
Direct and nested inputs use that same proof and rollback graph matcher rather
than separate parameter-list exceptions.
Reference leaves preserve the formal's access mode, including the exact entry
Parameter loan for an exclusive formal. Passive shared carriers use generated
copies. A whole owning formal may instead use its explicit Move, Binding-to-Value
handoff, prepared-slot reparenting and callee TransferOut witness. Such a formal
cannot also acquire a body-local owner or be cleaned as an unmoved value.
A cleanup-free formal, such as a destructor-free class that only carries a
borrow, has no unique cleanup recipe and therefore no Value handoff. It uses
the same exact Move ownership event and formal binding when its Binding
obligation and prepared slot are both passive and destructor-free, the staged
input initializes that slot directly, and the type is a passive bound shell.
The row records only the moved formal; no value, rollback or cleanup
obligation is invented, and its failure-edge Binding drop retires a passive
moved-from shell. Passive exclusive carriers are admitted only on exact
construction/movement paths; this does not grant copying or empty reference
shells.

Every inner constructor is independently proved genuinely non-failing before
its Invoke can be fused. Braced native argument lists retain GTI's left-to-right
evaluation even for sibling constructions. The final field producer keeps its
ordinary or status-tag overload, original failure record and outer Invoke;
no native exception or unchecked success assumption replaces that boundary.
Rollback replays the exact failed-child and intermediate loan retirement,
previous field drops, formal cleanup, cleanup boundary and secondary-failure
terminators. A consumed owning formal's generated moved-from shell is inert,
but its MIR retirement and failure graph remain verified. No runtime loan
metadata, extra allocation or default-constructed reference member is added.

Genuinely fallible inner producers still require a staged pre-constructor
execution facility: it must evaluate the prepared tree before native member
binding, retain failure records and exactly-once ownership cleanup, and pass
the same ordering/rollback matrix. Arbitrary factories, argument packs and
non-formal computations are not admitted by this bounded native argument proof.
This is a remaining backend goal, not permission to guess a constructor origin
or convert a real failure edge into success.

Reference fields may also bind an exact shared reference formal whose referent
itself carries a borrow. The native binding remains that formal, while the
verified entry Parameter loan and separate Stored dependency retain physical
shell versus external-owner identity. The backend requires the matching source,
parameter type and field Borrow; it never substitutes the external referent.
A returned constructed adapter preserves the same physical source and external
dependency in its exact Construct/Stored/Return handoff. Its class value carries
the reference, so publication creates no independent pointer alias. These
representation proofs do not change escape or endpoint verification.

The sealed native `OptionalStorage` capability has one narrower empty-prefix
case: a cleanup-free storage field may use the native initializer list when its
exact empty construction, field publication, and full-expression boundary are
the first three constructor instructions, with one declared field and no bases.
The remaining source constructor body, including failure edges, stays in MIR
order. For cleanup-owning storage, the existing value/rollback obligations and
success-edge reparenting instead remain explicit. Its independently proved empty
native constructor uses plain in-place construction, not a source-constructor
status-tag overload; only that empty-body proof permits a known-success result.
Capability identity, exact owner/target types, and the original publication
proof govern both cases, never the public wrapper's spelling.

Private empty-state construction is distinct from move assignment, recursively
through containing classes. Constructor placement and rollback reconstruction
may use a nonassignable field's observation-free empty representation; paths
that emit an assignment still require the stronger assignable-field proof.
Neither query bypasses the selected native capability constructor witness.
When a complete native initializer binding claims a constructed value, that
binding takes precedence over its local placement shape. Snapshot planning and
plain-constructor admission use the same per-value decision; a partial prefix
cannot hide placement required by an unclaimed later value. A claimed
construction's conservative Invoke follows its proven success edge, not an
unset body-local status flag. Its original failure and cleanup metadata remains
intact for verification.
A cleanup-free class field can also publish at an adjacent, exact
`Construct`/`Initialize` pair in the body, using placement construction rather
than assignment. This requires a proved non-failing target and carries no
invented cleanup obligations. The route also applies to assignable fields when
the complete prologue cannot use native initializers, such as a passive field
before a fallible owning-field call. Active fields retain their success-edge
activation, rollback reparenting, and normal transfer-out identities.

An adjacent class `Move`/discarded `Assign` can also consume a cleanup-free
temporary directly into one mutable binding or exact receiver field. The
shared move-assignment fusion query retains the fixed-array staging case and
adds this representation-only case only for a concrete, non-borrowing class
with generated move construction, assignment and destruction. Its source must
have the existing exact uninitialized publication-slot proof and no semantic
drop obligation; the destination must retain its resolved type and mutable
field identity. The writer neither default-constructs an SSA class nor invents
cleanup authority.
The same resolved mutable-field predicate supports an adjacent parameter
`Load`/discarded `Assign` copy when the record has generated copy construction,
assignment and destruction with only passive scalar, enum, pointer or
text-view fields. Its
source must carry an exact available, reachable Read event; borrowed state,
replacement cleanup and custom special members are not admitted by this route.
An exact non-failing `Construct` followed by its discarded field assignment
uses that same field predicate and the existing fresh-assignment proof. Its
constructor arguments retain their MIR evaluation order, and the original
conservative Invoke may bridge only to the unique immediate assignment. The
generated passive value has no independent drop obligation; arbitrary fallible
construction or owning replacement does not use this fusion.

Constructor-body replacement of a completed owning field accepts both an
unqualified instance-field symbol and `this.field`. Both resolve through the
same concrete owner and mutable field representation rows; module/static
symbols are not receiver fields. The existing construction-rollback obligation
continues to own the replacement, whether it was active on entry or activated
by an earlier MIR initializer. MIR verification remains responsible for
availability and lifecycle ordering before the assignment.

A sibling field may share the schedule through the exact parameter-copy proof,
a source scalar literal, or the substituted scalar default-construction
intrinsic. Each retains the same explicit field publication and full-expression
boundary, in declaration order. A non-borrowing cleanup-free class
field carries no drop obligation but still publishes through one explicit
MIR `Initialize` into its exact This-rooted field. The native initializer list
consumes that instruction together with its producer and full-expression
boundary; missing publication fails verification. This also covers a selected
generated default whose field-initializer body is passive and whose recursive
empty-state proof excludes observable construction or failure. An active-cleanup
field additionally requires the produced value's `Value` obligation, its
`Reparent` into the field's `ConstructionRollback` obligation, and one
unconditional transfer-out at normal completion. A failure-capable target
constructor is admitted only when its verified MIR body can never report a
failure status and that ordinary body is itself published by this route, so
the plain C++ overload exists at the call site. Zero-argument concrete default
construction may omit the argument call-plan identity because there are no
inputs to stage; the selected constructor and exact field type still match.
A targetless explicit generated-default `Construct` can instead publish in
the constructor body directly into the final field. Its default initializer
body must be passive and its private pre-publication shell independently
inert; the adjacent Initialize must reparent the exact active value obligation
into the selected field's rollback obligation. This uses construction-only
representation authority, not move assignment, and retains normal field
retirement and later failure cleanup unchanged.
A declaration-ordered explicit prefix may leave a trailing generated empty
owner at its declaration default. That exception requires a targetless,
argument-free default construction, a passive generated field-initializer
body, the recursive empty-state lifecycle proof, and the exact initially
active rollback obligation and normal transfer. Source calls and observable
defaults fail this proof. Folded or synthesized literals,
other computations, call results, borrowed sources, concrete generic targets,
base initializers, and partially claimed schedules stay in the MIR body
schedule.

Declaration-time owner initialization may retain a cleanup-only failure branch
for an earlier completed field. Once the initializer schedule proves that its
producer succeeds or contains failure, that branch is unreachable. The exact
drop target, ownership transfer, Invoke/secondary-failure routing and cleanup
boundary still require validation, but a pointee destructor's ability to fail
does not make the dead branch executable. This permits multiple null owners of
resources with fallible destructors without weakening live cleanup rules.

For owners without a generated-default construction route, concrete source
constructors select declaration defaults alongside explicit initializers in
field declaration order. Their ordinary MIR Call/Invoke, publication, and
construction rollback schedule owns execution, including fallible class-value
calls such as `std::make_unique<T>()`. The backend suppresses native declaration
defaults only after proving every concrete source constructor covers each
default exactly once. Explicit overrides suppress the corresponding default;
failure unwinds completed fields before constructor parameters and caller
values, without beginning the enclosing object's lifetime.

Native field-prefix proofs consume this selected constructor schedule, not
the passivity of the unselected catalog defaults. Exact field identities and
declaration order, child input preparation, original Invoke outcomes, and
publication/rollback identities still require proof. An observable or panicking
default that the selected constructor overrides must never execute.

The complete owned-parameter field prefix may include scalar/enum parameter
copies beside moved fields. A copied sibling must retain its exact
declaration-ordered parameter `Load`, field `Initialize`, and full-expression
boundary, with no hidden value use, failure, cleanup or borrowed state. Those
copies remain executable MIR statements rather than being hoisted into native
initializers. The complete witness preserves every moved parameter's unique
source, loan/drop handoff and publication; a scalar sibling must not make a
later moved field lose its otherwise valid prefix proof.

Range endpoint permission markers have no native borrowing representation.
The writer admits `ParkRangeCursor` and `RestoreRangeCursor` only with canonical
neutral instruction shape and one exact verified range-inventory membership.
They erase to comments, without a cursor copy, allocation, native reborrow or
change to begin/end evaluation order. The backend never infers permission from
the public range or cursor type.

A cleanup-free, zero-input, single-field constructor can retain a child's
status-tag native construction when that child has no ordinary constructor
representation. The proof covers the complete parent body: one selected
Construct/Invoke, immediate success-field Initialize and full-expression end,
Return, and exact failure propagation, with no loans or drop obligations. It
independently proves the entire selected child non-failing, including an exact
InitializeBase target and success activation; neither abstract-base spelling
nor a conservative effect summary supplies that proof. Fallible children,
multiple fields, owning cleanup, and additional body work are not admitted by
this bounded matcher and require their existing independently proved routes.

An owning field-result call can itself consume prepared owning arguments.
Calls and constructors share the same exact input-transfer proof: every
unconditional `TransferOut` names one operand's prepared-parameter obligation,
including a fresh result reparented into its argument slot. Input handoff is
distinct from result publication. The result activates only on its Invoke
success edge, then its explicit field `Initialize` reparents that exact value
into construction rollback. Later argument, callee, field and body failures
retain their original reverse-order cleanup. This covers source-defined
collection builders and unrelated owning functions without recognizing public
library names or relaxing the field's private empty-state/movement proof.

A type-only concrete instance with no selected constructor and no generated
default-construction route has no executable declaration-default consumer.
Its catalog defaults are suppressed as well, so an unused generic sibling
cannot impose conflicting native defaults on the same retained template.
Every selected constructor still requires the full field-order, exact-type,
and once-only coverage proof; absence of a constructor is not permission to
omit a default from an executable construction.

A status constructor may override an effectful declaration default, including
an implicitly selected nominal default. The declaration catalog is not a second
executable schedule: this route requires native catalog suppression and exact
selected field order/types, each MIR producer and `Initialize` publication, and
its full-expression endpoint. Stored reference fields retain the existing exact
parameter/loan binding proof. Each endpoint must precede the next field's
producer; missing, duplicate or forged coverage is rejected. This same proof
replaces initializer-list-only coverage for status base constructors. Per-field
empty-shell, active-drop, reference and failure-rollback checks remain separate
and mandatory; selected-schedule coverage does not authorize an empty native
representation by itself. An overridden default is neither evaluated nor
replayed by native field initialization.

Nullable raw-pointer declaration defaults retain MIR's `NullPtr` literal value
and the field's distinct typed pointer destination. The selected-schedule proof
admits only the exact null literal conversion, with matching producer/value
identity, publication operand and destination; arbitrary mismatched values do
not qualify. A sealed optional-storage construction independently proves its
selected private constructor's instruction-free, unborrowed void-return body on
every aggregate/status admission route. Field order or an empty native shell
alone cannot replace that witness.

Generated/defaulted construction with `generatedDefaultRequiresSchedule`
retains its selected concrete constructor MIR and can suppress the separately
retained declaration catalog after the complete coverage proof. It does not
universally lack a constructor failure channel. The backend still has a
classified `GTI-B0003` boundary for a fallible source class-value call left in
an unscheduled declaration-time initializer. It cannot silently redirect that
call through a terminal wrapper. For such a rejected case, an assignable nullable
owner can start null and be allocated in an explicit initialization method
after construction. Remaining schedule/routing profiles need focused evidence;
the classification is not a blanket prohibition on generated defaults.

Obligation-free class values in a move capture remain concrete MIR values
rather than backend-recognized library types. The C++ backend may materialize
the exact closure environment only after proving unique use,
matching concrete type, passive lifecycle, and the recorded full-expression
boundary. Active cleanup and additional projections stay on their existing
lifetime-slot paths.

Move-only class replacement of a mutable owned nominal field path has three
additional exact representations. A transformed source call may initialize one
value-rooted obligation, publish it through an unconditional `Replace` into
the field, and then drop only the moved-from slot shell. A move from one field
through a unique mutable `CallResult` loan may initialize the same kind of
slot only when the move and receiver-field assignment are adjacent (or
separated by their exact MIR replacement preparation and success edge), the
success path ends that loan before the slot drop, and no alias or extra
lifecycle event exists. A targetless concrete default construction may use the
same replace/drop schedule when its own unconditional `Initialize` is the
single activation. Each route requires an exact noncopyable, move-assignable
class representation and one resolved active-cleanup terminal field. Every
intermediate projection must resolve to a mutable nominal field and its exact
native representation row. An indexed owning receiver may retire an exact
chain of mutable call-result loans, child before parent, with the same source;
the moved-from RHS shell is destroyed only after those recorded endings. The
emitter does not recognize the owning wrapper or container name. Apart from
the checked-array and checked-reference replacement extensions below, non-field projections,
conditional lifecycle events, incomplete
borrow endings, or additional consumers fail closed.

For a constructed or moved RHS replacing a field through a returned mutable
reference, the exact receiver stage may be an ordinary non-failing call or a
checked call with an Invoke success edge. Both preserve the MIR order: RHS
construction, destination evaluation, old-value cleanup, publication, loan
retirement, then staging-shell destruction. A getter does not acquire a
synthetic failure edge just to satisfy the representation schedule.

For passive result trees, raw and native function pointers, C-string addresses,
and null pointers use the same ordinary by-value boundary as scalars and views.
This is representation transport, not pointee validity or ownership authority;
semantic lifetime checks and unsafe dereference obligations remain unchanged.

For complete represented cleanup families, `CleanupBeforeAssign` drains the
old contents through the MIR-selected helper before the paired `Assign` moves
the held replacement. Lifetime-slot values are accessed as live objects, not
as slot wrappers. Failure keeps the RHS obligation active: class stages use
their explicit lifetime slots, and native storage-handle SSA stages drain their
held handle before the empty native carrier leaves scope. No C++ assignment
decides the old value's failure behavior. The exact instruction pair is a
cleanup barrier even when its selected destructors cannot fail.
For checked array-element replacement, preparation first emits a bounds guard
for each dimension from the exact MIR place and writes the original fixed-array
failure site before returning false. Only validated subscripts reach cleanup;
the paired commit has no detector and only moves the held value. Fresh call
results, direct constructed objects, and named moved RHS values retain their
own lifetime slots until commit or failure cleanup. Bounds-only preparation
still has a success/failure edge when old-value cleanup is non-failing.

An immediately consumed, targetless generated empty class argument can also
feed a status constructor without an extra native local. This requires the
exact MIR initialization/transfer/constructor chain, a generated default
constructor, no source destructor, a proved empty field representation, and
literal-only field initialization ending in MIR `Exit`. Source-defined or
potentially failing default construction cannot use this inline representation.

The field may belong to `this`, a mutable local binding, or an exact
non-escaping mutable call-result loan. The enclosing owner retains cleanup
responsibility; no independent field drop is invented. A checked destination
call keeps the staged RHS active until the call succeeds and drops it if the
call fails. A moved source may additionally be a direct field of `this` or a
local owner; that field move initializes the staging obligation rather than
consuming the enclosing object's obligation. These are representation choices
over MIR's verified ownership events, not permissions to consume arbitrary
borrowed referents.

For a source-defined writable dereference operator, synthesized destination
value information may inherit the surrounding assignment's read-only category.
That contextual information is not access authority. Admission consumes the
exact selected mutable-reference declaration, its matching mutable `CallResult`
loan and referent type, and the existing replacement/cleanup proof. It does not
create another owning drop for the referent or widen a read-only returned
reference. Unrelated cursor fixtures exercise repeated owning replacement and
reject independently forged declarations, access, origins and cleanup pairs.

Move-assignment from an existing class binding into a mutable class binding has
its own exact representation. The source may be immutable because GTI
immutability prevents restoration rather than consumption; the backend does
not spell such storage as physical C++ `const`. For an immutable class source,
the representation additionally requires the exact generated move constructor
and assignment recorded by the concrete MIR class row. MIR stages the moved
value in a value-rooted obligation, so the backend constructs that staging slot
from the source binding, move-assigns the destination binding from the slot,
and then drops only the moved-from staging shell. The route requires an
adjacent `Move`/`Assign`/`Drop` triple in one block, a `Move` lifecycle event
transferring the source binding obligation into the staging drop, an
unconditional `Replace` from that drop into the destination binding's own
obligation, and a single unconditional `Drop` of the slot. Aliased source and
destination bindings, places outside the owned-field families above, failure
sites on either instruction, extra uses, custom or incoherent immutable-source
class rows, and any additional lifecycle event on the staging obligation fail
closed.

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
Prepared constructor arguments may independently transfer their obligations
at invocation. The backend matches every such unconditional `TransferOut` to
its exact argument index, call site, type, prepared slot and obligation, once
each. Those input handoffs neither activate the result early nor discharge its
replacement-failure cleanup.

A borrowed by-value constructor input may instead retain a temporary stored
dependency produced by an ordinary call. Its exact ordered argument stage,
single consumer, type, read-only nonescaping dependency and producer identity
tie that loan to the construction. After publication, the backend requires
every such input dependency to retire exactly once before the moved-from
staging shell is dropped. A same-access nonescaping `CallResult` parent chain,
such as a checked element followed by a borrowed view, retires child-to-parent
using the same structural rule as destination reborrows. Destination-loan
retirement remains independent.
No unrelated instruction or loan ending is skipped; stored carriers, escaping
or lifecycle-bound dependencies and extra argument uses are not admitted by
this schedule. MIR remains authoritative for the complete loan interval and
all constructor/replacement failure edges. This preserves ordinary owning
construction from views without naming a library constructor or view type.

A later terminal argument can prevent the enclosing invocation entirely.
The backend reuses MIR's full-expression/CFG proof for a zero-use prepared
input, then independently checks that every intervening failure edge destroys
the exact prepared slot once. The producer still constructs before later
argument evaluation; no callee handoff or `TransferOut` is invented. Both
arms of a conditional terminal argument must satisfy this rule. The impossible
normal continuation is not permission to omit failure cleanup.

The publication has five destination families, distinguished by which
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
`Drop`. An exact mutable owned nominal field uses the same representation-backed
field-path proof as moved-value/call-result assignment. Its containing object
already owns cleanup, so publication names no destination obligation; the
paired replacement preparation, temporary activation and shell drop remain
explicit. This also covers restoring a moved-out receiver field after concrete
`if constexpr` selection. No library spelling or compile-time branch marker is
consulted by the backend.

A direct dereference of a named checked-reference carrier is the fifth family.
The exact unprojected sibling must have reference type with the same referent
type, mutable reference access, and a retained mutable loan naming that carrier.
The carrier binding's own access does not determine referent mutability. The
same structural carrier query supplies lvalue spelling and assignment admission;
no standard-library type is privileged. Cleanup-free fresh/copy/move/call-result
replacement and owning fresh/move/call-result replacement use their existing
producer, single-use, staging, and commit proofs. The referent owns no new drop
obligation. An owning `Replace` therefore has target zero, while its paired
`CleanupBeforeAssign` and the held RHS failure-cleanup obligation remain intact.
Unlike an ephemeral call-result loan, the named reference is not ended at commit:
subsequent uses retain the original MIR-verified loan interval. This representation
does not authorize movement out of a borrowed referent.

An owning class call result may also replace an exact unprojected mutable
call-result loan returned by an ordinary source accessor. The result-slot
admission and assignment emitter share one witness requiring the exact GTI
source declaration, mutable reference result, producer/result identity and
loan access. The referent has no separate owning obligation. MIR's existing
paired replacement preparation destroys the old target only after the RHS
successfully exists; its `Replace` retains target zero and the staging shell
is dropped once. The backend neither invents a destination owner nor repeats
the accessor call. Read-only, detached or independently owned loans and missing
replacement/cleanup pairs fail closed. The unrelated owning-accessor matrix
checks left-before-right evaluation, both pre-construction and partially built
RHS failures, the old target's preserved contents, and exactly-once destruction.

Body-local MIR verification requires exactly one `Replace` from each consumed
active-cleanup RHS value obligation; validating only present lifecycle events
is insufficient, because a missing transfer could leave the published source
scheduled for cleanup. This requirement is independent of the destination's
representation or whether it owns a body-local drop obligation.

Owning payload-enum assignment uses the same two referent witnesses. Its
value slot (a fresh payload construction, call result or moved binding)
replaces either a place this body tracks, through that place's `Reinitialize`
ownership event, or the complete referent of a checked mutable reference
carrier or source mutable call-result loan, where MIR records no local
ownership event and the `Replace` keeps target zero. The paired
`CleanupBeforeAssign` drains the old alternative and retires its native shell;
the commit move-assigns the slot into the same referent spelling and destroys
only the moved-from slot shell. A borrowed form naming a local `Replace`
target fails closed.

Requiring a destination obligation for an element, owned field or checked referent
would retire the same storage twice, so either publication fails closed when
it names one. Every other destination, a bare-binding, field or loan publication with
a failure site, an incomplete two-phase loan schedule, extra uses, and any
additional lifecycle event on the staging obligation also fail closed.

A cleanup-free ordinary-constructor result used as an exact by-value argument
retains representation-only storage at its MIR construction point, for both
free functions and borrowed-receiver methods/operators. The receiver proof
accepts either an exact `CallInput` borrow or the direct typed borrow retained
by operator MIR; it still verifies owner identity, access and receiver capability.
No owning lifetime or receiver consumption is inferred from native syntax.

For the adjacent-assignment proof, a cleanup-free class prvalue has no staging
obligation: no place or drop obligation owns the constructed value and the
discarded assignment result is metadata only, so construction spells directly in
the value position of
its one adjacent assignment — into a checked fixed-array element, a bare
mutable binding, or the referent of a mutable borrow, which is what an
overloaded subscript yields. A loan referent's storage belongs to whatever
the loan points into, so like an element it carries no obligation of its own
and, like a bare binding, its write carries no failure site. Materializing an
intermediate C++ object there would invent construction and destruction that
MIR does not contain.
The adjacent assignment may sit across the constructor's unique Invoke success
edge only when the complete ordinary-constructor/status proof succeeds. No
other work or incoming edge may intervene, apart from the exact boundary- and
effect-free two-phase loan activation. A genuinely fallible or status-only
constructor cannot use this representation. The MIR failure edge is preserved;
no cleanup or failure semantics are inferred from the generated C++.

Fixed-array access through a receiver uses the same field-chain/index-chain
representation as local and module records. An exact sibling field place proves
the array type, sealed field rows prove the nested path, and each dynamic
dimension retains its checked failure operation. Reads, writes and replacements
share this representation; the former single-field/single-index receiver-only
route is removed. Borrowed record parameters also admit one leading checked
reference `Dereference` before the field chain; an exact root reference and
sibling array place prove the path, without copying the owner or admitting a
raw-pointer dereference through that checked-reference proof. A separate exact
raw-pointer SSA prefix (`RawDereference` or `RawIndex`) may select the same
field-array sibling: the raw operation retains its unsafe access metadata,
while every subsequent fixed-array dimension retains ordinary bounds checks.
Read-only and mutable access still come from verified MIR and source loan
analysis. Constant in-bounds array-element field chains use the same type/field
rows for binding, receiver and raw-pointer roots; no extra object is materialized.

Structured fixed-array decomposition retains its semantically chosen ordinal
as a MIR constant index as well as the evaluated index value. A fixed-array
projection whose index is a literal must agree with that literal; verification
rejects a contradictory constant ordinal instead of trusting an unchecked
backend shortcut. A passive class array copy with one adjacent binding
initialization constructs that destination
directly from its exact source array. The source's Read/Available ownership
event, generated copy/move/destruction traits and destination activation/drop
identity must all match. There is no default-initialized array SSA temporary,
no extra ordinary constructor invocation, and no change to the hidden owned
copy or its immutable element projections. Active cleanup/borrowed-state
arrays retain their separate protocols rather than entering this fusion.
A by-value argument copied from a named passive class array is a copy Load
immediately consumed by its argument CallInput. The CallInput owns
uninitialized representation storage and copy-constructs it from the exact
source in MIR argument order; the consuming source call or constructor copies
that completed stage into its parameter. The same Read/Available, adjacency,
sole-use and source-callee facts are required, and neither SSA value records
storage or a drop. A constructor's exact copy of such a parameter into a field
uses the member-initializer copy proof shared with passive record parameters.
Any remaining fixed-array SSA value whose element class has no default
constructor fails body planning instead of being value-initialized, so a lost
publication proof cannot invent element default construction.
An owning whole-array Move followed by its one adjacent lexical Initialize
constructs the final binding directly only with an exact source ownership
event, source/value drop Move, value/binding Reparent, and structurally
failure-free element movement. All original source and destination cleanup
instructions remain scheduled by MIR; no representation-only fusion may
invent a source Drop or erase an owning result obligation.

Raw address formation evaluates a selected array place without loading its
referent. MIR lowering places the selected Index's semantic fixed-array bounds
origin on AddressOf; verification rejects missing or mismatched bounds facts.
The native detector checks every dimension before forming the address, and its
paired Invoke carries the canonical source failure record through ordinary
cleanup. Unsafe raw-pointer validity obligations remain distinct from these
checked fixed-array selections. Forming an address creates no checked loan.

Generated passive record copies may contain positive concrete
fixed arrays of passive fields, including C-ABI records. Exact
generated copy/destruction traits, field drop facts, and the absence of active
cleanup or state-bearing bases remain required.

A moved value staged through plain call inputs into one construction in the
move's own block, with no record between them touching the source place, is a
moved-place chain (`movedPlaceChainSource`). Neither the `Move` nor its call
inputs materialize a local, so every spelling of that construction passes
`std::move` over the source place, including a construction published at a
plain or failure-form return, as in `return Holder(std::move(value));` and
`unexpected(code)` for a scalar error.

### Declarations And ABI

Checked references, retained loan pointers and failure-result pointer channels
share value-object qualification. In particular, a read-only `c_string&`
borrows the pointer slot as `const char* const &`, not by adding another const
to its character pointee. The same top-level qualifier is retained when that
reference crosses nested returns, callable adapters or stored-reference fields.
This is representation policy only; semantic owner dependencies and access
permissions remain unchanged.

Generated nominal and instance names hash logical source identities rather
than host filesystem paths. The source descriptor table carried with failure
metadata covers every loaded unit, including those without failure sites:
`<prelude>`, standard-library include names, root/package-relative paths, and
content-plus-include-route identities for external units. Lowered declaration
source-unit references must resolve in that table before emission. Declaration
spans retain physical paths for diagnostics and opt-in debug locations; those
paths do not participate in generated identifier hashes. This preserves names
across checkout and toolchain relocation without conflating same-basename
external units. Failure-site serialization and runtime descriptor IDs retain
their existing contract; the additional source rows are compiler metadata.
Private module implementation roots retain their registering interface as
loading provenance. Their external identity route extends that interface's
route with the implementation's package-relative path, also covering files
included only by the implementation. This provenance is separate from
dependency/visibility edges and grants no access to private declarations.

Interface-owned static functions use the ordinary static declaration and direct
call path. Purity comes only from the verified function's `pureVirtual` fact,
never from the enclosing interface kind. Lowered declaration and MIR instance
inventories must agree on static, virtual/pure/override roots and receiver access;
contradictory metadata is rejected before representation planning. Static methods
add no dispatch-table slots or implicit receivers.

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
Retaining a native carrier (including the lexical-callable template overlay)
must retain full definitions of its exact specializations as well as their
forward declarations. The source and direct-concrete definition schedules are
exclusive for each selected instance; a primary application and an exact
application in the same program cannot silently select different carrier ABIs.
Lifecycle members follow the same exclusivity. A directly represented concrete
instance of a generic active-drop class defines its cleanup adapter and
destructor failure entries in its own deferred concrete row; the primary's
lifecycle pass leaves that instance alone and emits explicit specializations
only for instances that remain specializations of the primary.

Dependent generic enum fields and signatures in these native carriers use a
closed selector catalog. Its key retains the GTI enum family, expanded type
arguments, and separate checked uint64 value arguments; trailing packs and
namespace-distinct families remain distinct. Each specialization names an
already-verified concrete enum representation. The native compiler does not
instantiate payload semantics or invent a layout. An unused dependent family
may have only an undefined selector primary; an executable concrete application
must have its exact lowered/MIR instance. Missing concrete instances never
become `void` fields. The catalog is emitted only for dependently referenced
families, and rejects textual duplicate keys. Full native specializations also
reject typedef-equivalent duplicate keys as redefinitions, without quadratic
pairwise comparisons or silently selecting another GTI identity. Such a native
representation collision remains a backend limitation, not a source legality
rule; exact-only applications do not demand a selector catalog.
Enum declaration parameter metadata is serialized and validated for exact
argument kind/count and consistency across the family's concrete rows.

Generated constructors have no source member declaration row. For a retained
native primary, their exact ordinary and status-form specialization declarations
are nevertheless emitted alongside source-member specializations, after the
concrete field dependencies are complete and before executable consumers.
The declarations and later definitions use the same verified constructor
instance and admitted signature; native implicit instantiation cannot select a
different body or precede the explicit specialization.

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
Exact function-item fields use the passive constructor-field representation:
the source/destination identity and absence of active cleanup are verified,
not inferred from a public callback wrapper name. A field invocation remains
a direct namespace call; MIR has already scheduled callee-expression effects.

### Defined Failure

Source-owned prefix results use ordinary class-result lifetime slots at a
status-returning call boundary. A successful MIR Return moves the exact owner
through the out-parameter. If destination replacement later fails, the activated
result's MIR Drop executes its source cleanup before the representation leaves
scope. Missing representation rows or success activation fail admission.
Owned class results
from source member calls use the existing value-rooted replacement slot only
when the receiver has its exact unique staged borrow and matching owner and
mutability; result activation, input transfers, replacement, and shell cleanup
retain the same proof as a free-function result. Container swaps are ordinary
clients of these representations, not backend-recognized operations.

Source-defined sparse owners use the ordinary class-result and field-cleanup
representations. Their two region leases, live-slot metadata and reverse
cleanup loops are emitted from verified MIR; there is no native sparse-storage
handle, adoption primitive or hidden live-slot bitmap. The backend lowers
general object construction, destruction and relocation using the selected
constructor and exact cleanup recipe carried by MIR. It does not reconstruct
the source owner's initialization state or release policy.

MIR carries canonical failure metadata, detector sites, failure edges, cleanup
schedules, propagation, and publication behavior. The C++ backend materializes
that contract as runtime descriptors and transformed failure siblings where
the native ABI requires them. It does not infer failure from emitted C++ or use
C++ exceptions to reconstruct GTI semantics.

The descriptor representation shares identical counted logical-source byte
arrays and identical ordered outcome arrays within an artifact. The earliest
canonical site supplies each array's deterministic name. Site IDs, locations,
outcome order/counts, canonical descriptor bytes and artifact identity are
unchanged; different source bytes or outcome shapes do not share storage.
This is immutable-data interning, not failure-site elimination or new authority
to remove a check. The same representation is used at O0 and optimized levels.

The generated incoming-cleanup failure check is outlined with guarded
`cold`/`noinline` native hints on GNU-compatible compilers. Its only production
call sites are failure edges. Keeping its emergency-record construction and
thread-local lookup outside native inlining reduces successful-path costs in
source-storage clients without removing any operation. Other native compilers
use the identical unannotated helper. It is neither `noexcept` nor terminal:
it returns when no primary cleanup failure is active, and a managed-worker
double-failure report can perform the existing internal exception handoff.
Cleanup scopes, activation, record construction and ordinary source functions
receive no blanket cold, inline or exception annotation. MIR, replay evidence,
failure identity and cleanup order are unchanged.

A transformed body omits its invocation-local cleanup scope when every verified
active-failure region only propagates the record: inert loan ends and lifecycle
boundary markers, then direct propagation or an argument-free continuation.
Lifecycle transfers and every executable cleanup operation (including any
Drop, call, native/resource release or unknown terminator) retain the scope.
This is representation-only; it neither removes checks nor changes MIR or its
failure summary. In particular, the incoming-secondary-failure check remains
at failure producers in propagation-only helpers: they may be called from an
ancestor's destructor while that ancestor owns the primary record. Ordinary
user accessors and forwarding functions use the same rule as library helpers.

Integer overflow detection has one representation shared by every checked
family. The `mir_checked_add/subtract/multiply_v1` failure helpers and the
terminating `add`/`subtract`/`multiply` helpers use it. So do the
`gti_internal::integer_*_overflows` intrinsics, which the source `checked_*`
functions test before returning an `expected`, and the `saturating_*`
intrinsics.
The preamble's `integer_add_overflows`, `integer_subtract_overflows` and
`integer_multiply_overflows` are exact predicates over operands already
converted to the result type. Under GCC and Clang they lower to
`__builtin_*_overflow`, which the native compiler maps to the target's
overflow flags. Other compilers, or a build defining
`GTI_PORTABLE_OVERFLOW_CHECKS`, use the portable limit comparisons, which
compute the same predicate. Each caller stores its result only on success, so
an overflowing operation publishes nothing and reports the same code and
detail. `checked_integer_overflow_runtime` compares both paths at O0 and O3
against exact arithmetic. MIR operations, safety discharges and failure
metadata are unchanged; this is a backend spelling of the same checked
operation.

Unique-owner borrowing uses the failure ABI. At the intrinsic instruction the
backend tests the represented owner for emptiness and selects only the exact
code/detail carried by MIR. Its `Invoke` routes failure through cleanup; the
success path publishes the pointee pointer at the paired reference return.
The loan keeps that owner stable between the check and publication. This path
does not call the legacy terminal `owner_access` helper, and a missing failure
edge or plain-body representation is rejected rather than bypassing caller
cleanup. No public smart-pointer name is recognized.

Ordinary generated GTI functions, classes and program storage live in
`__gti_program` inside an anonymous C++ namespace. Their translation-unit-local
linkage reflects that they are implementation details of one complete lowered
program, not native exported APIs. This allows the native compiler to discard
unused source helpers and avoids collisions between independent generated
program identities. It does not remove MIR bodies, checks, initialization,
cleanup or observable resource calls. Hosted entry adapters and explicit native
ABI boundaries remain outside that ordinary-program identity contract; public
opaque native declarations retain their original scope. No public library name
or optimization level controls linkage.

C-linkage declarations use the deterministic private C++ spelling
`__gti_c_fn_<FunctionId>`, independently of the semantic external symbol.
The declaration writer retains the exact native signature and binds that
private identifier through the selected target's object-format link label:
macOS (Mach-O) labels include the ABI's leading underscore, Linux (ELF) labels
retain the symbol directly, and Windows (COFF) uses an alternative-name linker
directive. The choice comes from the verified target, not host preprocessor
checks, so the lowered program contains no unresolved target branch. Typed MIR calls,
indirect-function values and hosted C-thread boundaries consume the same sealed
private declaration identity. Runtime definitions, native bridge headers and
embedding export symbols are not renamed. This is representation isolation,
not a libc-name exception or a type conversion.

Each native callback item produces one typed `extern "C" noexcept` thunk. A
non-failing target is called directly. A failure-capable target uses its
verified failure sibling and forwards the original record to the runtime
terminal boundary. Native exceptions are contained and terminate; the backend
cannot select another target or weaken that policy.

Concurrent thunks consume the verified `CThreadCallbackKind` to select manual
worker or synchronous once entry/leave hooks. The public once state machine
remains ordinary MIR-emitted GTI and atomic operations. Module initialization
uses the canonical program-storage root even when a copied argument retains
another MIR view. Data-only constexpr aggregates are not runtime lifetime slots.
Their native materialization factories are selected by the constant's exact
concrete semantic type, not just its class declaration ID. A constant
`pair<int, int>` therefore does not emit a constexpr factory for an unrelated
runtime-only `pair<string, int>` instance. This avoids imposing C++ literal-type
requirements on ordinary runtime owners, including under C++20.
Passive generated class copies in `CallInput` retain a representation slot at
their written evaluation point, including across a later argument's failure
edge; native argument ordering cannot delay the copy. Such slots own no
source destructor effects. The legacy unstaged `Load` copy of an owner-tied
iterator uses the same outer representation storage when its sole by-value
consumer lies in another block. The copy still executes at `Load`, verified
SSA dominance preserves ordering, and MIR retains the owner dependencies;
the consumer moves from the slot and retires its passive representation.
Unused passive constructor tags retain their exact
lexical cleanup without preventing other fields' verified initialization.

A borrowed call input may remain representation-erased across a conditional
scalar or text-view argument when it names a bare local, a binding-rooted path
of field selections and reference dereferences, the exact full-expression slot
of a temporary, or a field view inside an owned result's slot. Its stage must
dominate the unique consuming call, and every normal path through the acyclic
argument region must reach that call. Scalar and text-view computations, loads,
input stages and expression temporaries may intervene. A nested function or
method call may run wholly inside the region, for example as the conditional's
test, one arm's value, or a direct scalar argument. Its inputs are scalar or
text-view values, borrows staged for it alone, and class arguments prepared in
its own parameter slots: the receiver and arguments may borrow binding-rooted
places through fields and reference dereferences, and a mutable borrow never
names the borrowed place's root. Its scalar or text-view result has no loan,
its only lifecycle effect is the handoff of its prepared arguments, and any
region instruction may consume it: a branch, a computation, a temporary or an
input stage. The call keeps its MIR position and its own Invoke failure edge,
which leaves the region before the outer call. The proof also covers a nested
read-receiver or free function call, or an exact constructor, with scalar or
text-view inputs and a cleanup-free nominal result whose sole consumer is an
exact prepared argument slot for the outer call or a nested call. Its failure
edge precedes publication; result preparation does not change the borrowed
local's address. An owning class argument of either call may also be prepared
in the region, and one of the outer call before the conditional. It is either
an exact constructor with scalar or text-view inputs, activated on its checked
Invoke success edge or at a construction that cannot fail, or another local
moved or copied into its exact prepared argument slot. The outer and nested
calls share one prepared-slot proof: the slot's activation, its sole
TransferOut to the consuming call, and a Drop of the slot on every failure edge
between preparation and that call. No prepared slot is the borrowed place. A
nested call's receiver or read-only reference argument may also be a class
object created in the region, such as `make(2)` in
`use(make(1), skip || make(2).ready())`. A function or constructor call
materializes it in its own full-expression slot: the value-rooted lifetime slot
or, in a conditional arm or `&&`/`||` operand, the CFG-promoted hidden slot.
The direct-temporary proof that spells such a borrow owns the slot's
activation, the stage borrowing it, and its drop at the end of the full
expression, after the outer call and conditional when the operand may be
skipped. The producer keeps its MIR position and its own Invoke failure edge
unless it cannot fail. Its only lifecycle effect is that slot's activation, its
inputs are those of a nested call without owning arguments, and its receiver,
if any, is read-borrowed. An argument borrow's call-argument loan may end
inside the region, but the staged input's own loan may not. No such slot is the
borrowed place.
An ordinary safe source method may also consume a separately rooted local
receiver and publish an owned class result into that exact prepared argument.
The bounded zero-argument method shape reuses the moved receiver's full-expression
slot proof and the result's exact activation/reparent/sole-handoff proof. Only
the linked move, receiver stage, method call and result stage are admitted;
neither receiver nor result carries borrowed state. Both checked Invoke-success
publication and a failure-free producer's unconditional activation retain their
existing cleanup schedules. This rule depends on storage and call facts, not a
library result spelling.
Whether the receiver is a method object or a local lambda does not change
this proof. Other user-call shapes, other owning transfers, raw memory, other
loans, and arbitrary stores remain outside this proof. Thus the deferred local
address denotes the same object that was selected before argument evaluation;
the backend does not reorder an observable address computation across a branch.

A cleanup-free source class call result assigned into a fixed-array element
uses its existing representation-only result slot until the adjacent assignment.
Admission requires the exact mutable array/field element, generated failure-free
move/assignment, and the checked write's exact bounds origin and status edge (or
its independently verified discharge). The native checked-array write consumes
the staged result only after producer success; bounds failure follows the MIR
failure edge and cannot publish to the array. Its moved-from passive shell is
retired without inventing a source cleanup obligation. A failed producer leaves
the destination untouched, and unrelated active owners retain normal MIR cleanup.
The MIR routing rule also covers bounds-only class assignments with no lifecycle
transition: lexical-drop type traits alone do not suppress that required edge.
Owning replacement still checks bounds at its separate `CleanupBeforeAssign`
operation; this routing rule does not authorize an unproved owning transfer.

The same cleanup-free result slot can feed one exact payload-enum field.
Constructors and ordinary source calls share the proof: the selected variant's
field type and sole operand use match, there is no semantic value-root/drop,
and the producer's successful continuation dominates publication. A generated
failure-free move transfers the completed value into the payload, then retires
the passive representation shell. Failed production cannot publish the enum;
borrowed or owning results retain their separate checked loan/drop transport.

An owned class result returned directly from an erased virtual call or a stored
lambda invocation may use the caller's placement output. Admission requires the
exact resolved signature and receiver capability, a separately admitted failure
body, a sole executable return use, and the paired success activation and final
transfer of the result obligation. The backend does not introduce an intermediate
copy or infer ownership from the public callback wrapper's name. This direct
return proof does not admit arbitrary local class-valued lambda result bindings.
When the class result is passive and has no semantic drop, both ordinary calls
and exact lambda calls share the same inert-success-tail proof. Only lifetime
or loan boundaries and exact passive binding drops may precede Return; the
absence of a result obligation does not permit early publication across active
cleanup or other executable work.

A resolved nominal `operator()` uses the same exact class-result placement
proof as another function-target call, both for argument preparation and local
binding initialization. Callable receiver capability remains independently
verified; no public wrapper name participates in this classification.
Receiver field paths used only as Stored or CallResult provenance are not
eagerly evaluated into native lvalue aliases. Exact field/type rows remain part
of admission, and CallResult provenance requires one admitted executable call
to materialize its pointer. A Return loan can share that provenance without
evaluating it again when the return is independently erased into its class
value, or its sole transfer copies an existing checked pointer carrier without
an address-place operand. Instruction operands and destinations, return or edge
arguments that actually use the place, drop obligations, range carriers, other
loan families and array bases prevent this erasure. Thus a public accessor can
preserve a private nested origin without generating a source-level private field access in its
caller; actual field reads and writes keep their normal representation.
For concrete, non-template owners and members, generated callable ADL adapters
are friend declarations inside the class, with inline forwarding definitions
after all type layouts. Their by-value parameters and results are then complete
without adding callable signatures as layout dependencies. This also permits a
callable to return a class that contains the callable by value. Retained native
primary templates retain inline hidden friends for per-instantiation ADL.
Their result and transported parameter spellings use an owner-dependent
exact-type identity (`std::conditional_t<true, T, Owner>`), deferring native
complete-type checking until instantiation without changing the resolved GTI
signature, selected member, receiver capability, or argument transport.

Declared destructors of generic owners containing lexical closure types retain
an explicit `RetainedPrimary` cleanup form rather than inventing a nameable
concrete owner (such as `Owner<void>`). Each instance keeps its own verified
cleanup item; the lowered verifier and C++ plan independently check the form.
Exact owner argument overlays permit MIR body emission with dependent type
spellings. A shared primary-template destructor adapter is emitted only when
the complete admitted source-body text and failure convention agree across
instances. Different bodies remain an unsupported representation boundary.
The instance-specific aggregate cleanup branches remain distinct: sharing the
source destructor body never substitutes one closure's capture cleanup for
another instance's field or nested-owner cleanup. Constructor comparison also
ignores only the two known no-op publication comments, while retaining exact
executable body, initializer-list, and cleanup-discriminator comparisons.

A call resolved to a concrete override initially remains virtual in MIR; the
C++ backend does not infer devirtualization. The bounded optimizer
`exact-receiver-dispatch` proof may refine it to a static source override,
which backend admission accepts and emits with explicit qualification from
the sealed MIR target. A closed-world parameter refinement keeps the
interface-typed receiver place. `isMirExactReceiverPresentation` admits it
only when `dispatchOwner` names the target's owner and that owner directly
implements the receiver's interface. The backend then spells
`static_cast<const Owner &>(receiver)` before the qualified call. It preserves the existing failure/status contract,
argument staging, and cleanup; native type analysis is not proof authority.
For a conservatively failure-capable virtual
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
receiver; C++ overload resolution is not authority. Its failure call uses the same
`cppFunctionHasFailureResultChannel` query as ordinary source calls: a
class-bound generic return keeps its uniform output parameter even when the
selected instance returns `void`, so that adapter passes a null result slot.
Concrete non-generic `void` targets retain their no-result-channel signature.
This is backend ABI agreement, not a different GTI result or task contract.
The backend boxes the concrete task only at that boundary and passes an opaque context plus
entry/destroy callbacks to the version-one runtime thread ABI. Creation
failure destroys the box on the spawning thread exactly once; success invokes
it once and destroys it on the worker. A typed adapter additionally constructs
one result box and supplies exact take and abandon callbacks: successful
explicit join transfers that box once, while automatic join destroys it once.
The entry catches only the runtime's in-process failure-handoff sentinel after
MIR cleanup completes or an explicit cleanup-emergency terminal edge stops
further cleanup, and terminates on any unrelated native exception. Neither the
opaque runtime state nor a C++ `std::thread` type appears in public GTI source
or `LoweredProgram`.

Private handle intrinsics lower from their exact verified MIR identities.
Spawn and join retain their synchronization records; explicit and automatic
join translate native status separately from an ordinary worker record or
cleanup-emergency envelope. The latter two are reintroduced into the joining
body's existing MIR failure channel with their descriptor pointers unchanged.
Task-object/capture destruction uses exact failure-aware cleanup helpers
inside the same reference-capturing adapter firewall. An ordinary failure can
publish the first failure or a second record of an emergency envelope. An
internal MIR cleanup-emergency terminator always takes its terminal runtime
edge: it never returns a fabricated success from a destructor helper. The
cleanup helpers themselves permit the private handoff to reach the adapter;
native `noexcept` destructor wrappers remain representation backstops, not
another source Drop schedule.

During that exact emergency handoff, a worker-local marker prevents native
stack retirement from running source lifecycle bodies, payload storage loops,
shared releases, unlocks, or joins omitted by the verified emergency path.
Native variant/array/closure/unique shells recursively reach guarded
GTI payload/carrier boundaries; their scalar storage deallocation does not
authorize additional source cleanup. Live thread states are quarantined and
locked mutex representations are left undestroyed. The generated and defensive
runtime adapter catches clear the marker before separately gated task-box
handling. Ordinary failure, an unrelated native exception, and a merely stored
pending failure do not authorize this abandonment. A recoverable explicit join
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
Malformed lowered values are rejected before header emission. A C-linkage
function's lowered `externalSymbol` is authoritative even when its GTI source
name differs through `[[link_name]]`; emitted prototypes and MIR call targets
use the external symbol, while source lookup has already completed in the
frontend.

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

## Native LLVM Backend Foundation

`LlvmBackend` is an independent sibling of `CppBackend`. It receives
`LoweredProgram` plus GTI-owned LLVM code-generation policy, verifies the
sealed value, restricts this first stage to the configured host target,
and builds an immutable admission plan before LLVM initialization. Pending
requirements report `GTI-B0003` with their exact pending family. Native target
and assembly-printer initialization remains backend-owned and idempotent.
The admitted scalar, places/text and class/lifecycle profiles below emit real native objects;
unsupported shapes never fall back to C++ or a dummy program.

The public backend header has no LLVM include or type. LLVM context, target
registry, triple, and initialization APIs are confined to the private
`src/compiler/llvm/` implementation. The dedicated `gti_llvm_backend` library owns
the `Core`/`nativecodegen`/`Passes` component roots; `gti_compiler` retains the narrow
support-library surface from ADR 006. Source and link-graph gates reject
frontend, optimizer-private, C++ representation, and unowned LLVM component
dependencies.

On Linux, static LLVM component symbols are localized at the final ELF link,
including for installed static-library consumers. A distribution's `libclang`
may load its own shared `libLLVM`; exposing GTI's static LLVM registries to that
DSO causes symbol interposition, duplicate initialization and shutdown
corruption. Only the named LLVM archives are isolated (`--exclude-libs`), not
GTI/runtime/native API symbols. This neither adds a monolithic LLVM dependency
nor changes the opaque libclang C API boundary. macOS uses its existing
two-level linkage and does not receive the ELF option.

Backend selection is immutable `CompilationRequest` policy. The generic
`compileToSelectedBackend` entry follows the same frontend, optimized-MIR,
lowered construction, verification, diagnostic, and progress path as
`compileToCpp`. Direct CLI compilation exposes `--backend cpp|llvm`; omitting it
continues to select C++. C++-only `--std` and `--keep-cpp` policy cannot be
combined with LLVM selection.

`BackendArtifactKind::Object` is the neutral native handoff. Object
contents remain opaque bytes; the driver, not the backend, owns atomic
publication, runtime/native inputs, linking, caches, diagnostics, and final
executable publication. A dedicated `NativeLinkRequest` composes an object
artifact with declared native objects, ordered link inputs, and the GTI runtime
without applying C++ standard, preprocessing, strict-floating-point, or source
optimization flags. Object-backend cache entries remain conservatively
bypassed until their binary metadata format is implemented. LLVM returns this
artifact kind for admitted programs; C++ remains the default backend.

LLVM lowering preserves this boundary. It must not link
frontend representations, receive source MIR or optimizer tables, parse
generated C++, depend on C++ spelling helpers, or treat HIR as executable
authority. If a backend-neutral fact is missing, the compiler extends and
verifies `LoweredProgram`; the backend may not recover that fact from an
upstream phase or native behavior.

Compatibility with the deleted multi-representation route is not a project
constraint. A new backend is an independent client of the lowered contract,
not another branch in the C++ emitter.

See [ADR 031](../decisions/031-native-llvm-backend-foundation.md) for the
dependency, initialization, packaging, status, and first-lowering decision.
The campaign's [shared cross-milestone contracts](../plans/implementation-sequence.md#shared-cross-milestone-contracts)
and [parallel assignment matrix](../plans/implementation-sequence.md#agent-assignment-and-evidence)
govern remaining work. Those accepted implementation constraints do not expand
the currently admitted execution profiles described below.

### LLVM Target, Layout And Execution ABI

O-LLVM-03 provides the private machinery shared by all native lowering profiles.
`Session` owns the exact
`VerifiedLoweredProgram`, context, native target machine, module, unique layout
and ABI planners, diagnostic collector and emission state. Planner construction
is private to the session, so clients cannot create competing per-program maps.
The only process-global LLVM state is once-only
target/assembly-printer registration. No module, type cache, diagnostic buffer,
or pass manager is shared between programs. The initial targets are native
macOS arm64 and Linux x86_64. Foreign targets and unimplemented debug metadata
are rejected, not silently approximated.

The machine selects generic CPU features and PIC relocations, verifies the
sealed scalar allocation sizes, ABI/preferred alignment, pointer width and
endianness, then supplies the module's triple/data layout. Later verification
rejects mutations of either. O0/O1/O2/O3 map explicitly to LLVM's corresponding
module pipeline and target code-generation level. Floating contraction is
disabled and no fast-math assumptions are supplied. The module is verified
before and after optimization; LLVM error diagnostics prevent publication and
warnings remain session-owned. Emission is single-use and returns binary
`BackendArtifactKind::Object` bytes only after successful target emission.

`Layouts` allocates deterministic named opaque types for concrete class
identities before completing record bodies. It is the shared authority for
storage sizes, field offsets, alignment, array element strides and address-only
transport. Boolean values are `i1`, storage is `i8`; references and raw/native
function pointers are addresses. Arrays use exact element storage, checked
extents and target padding; zero-size storage retains its alignment. Plain
non-polymorphic records are backend-owned, with one byte for an empty record.
Native C records and unions must match every sealed size/alignment/offset.
Pointer recursion does not recursively instantiate its pointee; by-value
cycles and unresolved types reject. Ordinary class drop-state is described
below; private ownership capabilities, inheritance/dispatch and payload-enum
representations retain their later milestones. No public ordinary-class
`sizeof` contract is invented.

`Abi` centralizes signatures for definitions, calls and future adapters:

| Private GTI ABI v1 shape | Transport |
| --- | --- |
| Failure-free scalar/reference result | Direct scalar/selected address result. |
| Address-only result | Explicit result-storage pointer; native `sret` is not assumed. |
| Failure-capable function | `i1` success status; optional result destination, then failure destination. Result storage is published only on success. |
| Failure destination | `{ gti_failure_record_v2, artifact-descriptor pointer }`; copied owned message, pinned immutable descriptor. It is written only on failure. |
| Instance receiver | Explicit receiver address after result/failure destinations. |
| Caller attribution | Exact site index and pinned artifact pointer, after receiver. This is attribution, not permission to originate a failure. |
| Ordinary arguments | Source order, scalars/addresses directly and address-only values through already-staged storage. MIR owns moves, loans, cleanup and publication. |

This table describes the **implemented** convention, not a final long-term
failure-transport decision. The reviewed
[C04 evaluation](../plans/implementation-sequence.md#c04-failure-transport-evaluation)
compares current status propagation with native unwinding before dependent
callable/worker ABIs are extended. No transport change is implemented by that
approval; definitions and calls continue to agree on the convention above.

All GTI functions use deterministic `__gti_native_v1_f<instance-id>` names.
Declaration planning retains source spans as private metadata and records
definition obligations before body emission, enabling recursion without name
lookup or upstream access. Unlowered required definitions cannot be emitted as
an object. ABI version 1 is an internal invalidation identity, not a stable
public GTI binary ABI. No `nsw`, `nuw`, `inbounds`, `noalias`, `nonnull`,
`noundef`, lifetime or dereferenceability promises follow from these plans.

Native C planning is separate: exact scalar/pointer signatures and required
narrow signed/unsigned extension attributes are applied identically to calls
and declarations. Aggregate arguments/results reject until O-LLVM-11 provides
target-specific classification and bidirectional oracles. Correct field
offsets alone never authorize passing a C record in registers.

The runtime bridge declares only actual, versioned C services: TLS begin/drain,
failure validation, terminal ordinary/cleanup reporting and allocation abort.
It models text views and all required v2 failure/v1 descriptor records and
checks them against native C layout in tests. Initial containment uses the
existing `gti_rt_failure_terminate_v2`, not C++ exception transport. The new
`runtime_execution.h` begin/drain C hooks preserve startup/shutdown operations
when present in the sealed plan; O-LLVM-06 adds typed TLS storage and the C
registration bridge described below. Backend policy cannot remove a verified startup operation merely
because it considers that operation unnecessary.

`runtime_abi_inventory` asks Clang to inventory **every active C declaration**
in all runtime C headers, with exact signature, canonical parameter types,
symbol/version, header ownership, resource/failure/callback family and attached
contract comments. Its target-specific JSON is retained in the build as
`gti_runtime_abi_inventory.json`; new unclassified headers or missing services
fail the gate. This tooling does not enter the backend. The five legacy
`@runtime` bindings accepted by semantics remain explicit adapter obligations:

| Binding / GTI signature | Real C service / remaining adaptation |
| --- | --- |
| `stdout.write`: `void(text_view)` | `int32_t gti_rt_write_stdout(gti_c_string_view)`; C aggregate classification and checked write-failure conversion. |
| `stdin.read_byte`: `int32_t()` | `gti_rt_read_stdin_byte`; direct scalar status/value. |
| `file.open_read`: `int64_t(text_view)` | `gti_rt_open_file_read(gti_c_string_view)`; C aggregate classification. |
| `file.read_byte`: `int32_t(int64_t)` | `gti_rt_read_file_byte`; direct scalar status/value. |
| `file.close`: `int32_t(int64_t)` | `gti_rt_close_file`; direct scalar status/value. |

Those legacy unversioned C services are recorded as such, not relabeled v1.
Most current library operations already call source-defined wrappers over C
services. C++ inline/template helpers in `runtime.hpp`, `runtime_thread.hpp`
and the C++ preamble are **not** linkable C entries; typed lifetime, ownership,
thread handoff, synchronization and helper-only mechanisms still require their
named later milestone. Inventory is not lowering admission.

The object test emits data and minimal C-ABI identity/call oracles, atomically
publishes and rereads the actual bytes, then links a separately compiled C
consumer through `NativeLinkRequest` at all four optimization levels. These
are infrastructure oracles, not a fake GTI `main`, instruction lowering, or
proof that a GTI program executes with LLVM. Separate O-LLVM-04 execution gates
now exercise real GTI scalar programs through the public backend.

### LLVM Scalar Execution

The native backend emits verified MIR scalar bodies directly, without C++
representation helpers or a second semantic analysis. `body.cpp` owns body-local
SSA/place materialization, edge arguments, direct calls, return publication and
MIR-selected failure continuations. `scalar.cpp` owns primitive representations
and instructions; `failure.cpp` owns immutable source descriptors and the
versioned failure channel. All are private consumers of the same session.

The admitted profile includes fixed-width integers, bool/char, binary32/64,
integral scoped enums, concrete generic scalar functions, ordinary recursion,
local scalar and staging slots, branches/short-circuit flow, switch and loops.
PHIs are declared before predecessor emission, and bodies emit in reverse
postorder so dominating SSA definitions are available. Edges carrying arguments
use transfer blocks, preserving distinct edge values even when a branch or
switch shares both endpoints; place-copy arguments execute only on that edge.
Scalar namespace/static
storage and ordered scalar initialization are implemented as a prerequisite of
the ordinary numeric and panic wrappers; no class layout or lifecycle support
is inferred from that overlap with O-LLVM-05/06.

Native functions use the centralized private ABI. A failure-capable direct call
returns status, never unwinds through LLVM/C++, and loads/publishes its result
only on its success path. Checked arithmetic guards originate only an exact
authorized, nondischarged MIR outcome and branch to its selected Invoke failure
edge. Signed and unsigned overflow are checked; division/conversion/shift guards
dominate potentially undefined LLVM operations. Signed minimum modulo -1 selects
a safe divisor and produces zero. Wrapping, saturation and overflow predicates
use separate intrinsic modes. No `nsw`/`nuw` or fast-math promises are invented.
Floating operations use ordinary IEEE LLVM operations under GTI's fixed nearest-
ties-even/gradual-underflow environment, with no reassociation or contraction;
NaN comparisons and truncation-before-range-check conversion are explicit.

Failure descriptors retain canonical bytes, artifact identity and source/outcome
tables. Channels carry a copied v2 record and its pinned descriptor through
propagation and shared continuations. Containment uses the runtime C reporting
boundary; class cleanup uses the same channels and emergency envelope as
described below. Plain `std::panic(string_view)` is
source-defined and forwards caller attribution through ordinary direct calls.
The scalar-ABI `gti_rt_failure_capture_panic_bytes_v2` is a thin form of the
existing bounded UTF-8-aware capture operation, not a foreign aggregate ABI
guess. Formatted panic and the currently formatting-based `std::assert` wrappers
still require their later class/storage/text families; scalar conditional
assertions calling plain panic need no new intrinsic.

Hosted `main` is emitted from the sealed NoArguments startup body and operations,
not manufactured from a special source example. Its program-initialization call,
entry call, exit value and containment are ordinary MIR. TLS begin/drain execute
when present in that plan (the current producer includes them when TLS storage
exists). The places/text profile below adds passive projections, references and
arrays; the class profile adds ordinary owning class arguments, cleanup and
TLS. Private ownership capabilities, indirect/virtual dispatch, general native
calls and native debug information remain explicit limitations. No mixed-backend
fallback is used. C++ stays the default.

Hosted returns preserve GTI's portable status policy: values from 0 through 255
are returned unchanged and every other `int32_t` result becomes status 1. This
normalization applies only to the sealed startup `ReturnEntry`, never to an
ordinary GTI function return. The four source-location literal operations read
their sealed caller metadata directly; text retains its UTF-8 byte extent and
line/column retain their exact unsigned values. The source-defined owning
`source_location` wrapper composes these operations with independently admitted
string storage and checked references; its runtime case is tested separately
from isolated literals. Neither case establishes full hosted-library closure.

An unsafe source call's repeated permission marker on a scalar materialization
can be admitted only through the exact checked `CallResult` loan. The current
witness requires an unprojected loan place and a unique earlier same-block
source GTI call with matching HIR identity, result type, reference access and
unsafe contract. The call and callee body remain independently admitted; raw,
foreign and intrinsic operations do not inherit this witness. MIR loan-flow
verification still establishes active access. Possible owner origins never
substitute for the returned runtime address.
The same producer proof admits a direct same-block `CallResult`-to-`Return`
loan transfer when its complete dependencies, access, type and HIR identity
are unchanged and the return terminator consumes that exact escaping loan.
It does not authorize constructing a reference from a possible owner address.

Raw primitive references may use an ordinary, independently admitted native
class reference as their lifetime witness. The centralized
`rawBorrowResultLoan` verifier query supplies the exact parent, origin and access
contract. The evaluated raw pointer supplies the physical address, even when
the witness owns a different address. This does not prove pointer membership,
extend the supported pointee layouts, or add runtime checks or alias promises;
the unsafe caller retains its alignment, initialization and lifetime obligations.

### LLVM Places And Counted Text

`places.cpp` owns body-local storage, selected loan addresses and evaluated
projections. Addresses are computed at their MIR stage, never hoisted or
recomputed from possible owner origins. Reference results use the ABI planner's
reference type, including pointer result destinations on checked-call success.
Verified permission/availability operations erase; no runtime loan manager or
LLVM alias attribute is inferred from source mutability.

The current added profile admits passive fixed arrays of primitive values,
their multidimensional projections, by-value snapshots, primitive/array
references and constrained fixed-array range operations. Array bounds compare
the evaluated index before address selection or access. Constant global arrays
consume sealed recursive constants; dynamic initialization keeps MIR order.

Read-only and mutable references to integral enums, including reference returns,
use the exact enum identity in the sealed integral inventory and its underlying
storage layout. This selected-address rule does not broaden enum-array or raw
memory admission. An integral-enum payload field uses the same reference rule,
but its enclosing payload storage remains independently gated by O-LLVM-07;
combined payload-field execution evidence is still required.

Counted byte views retain embedded NUL, UTF-8 bytes and their byte count. Text
literals, storage, PHIs and ABI values share the session's one text-view type.
Size/empty/index operations and byte equality require no heap allocation;
equality checks counts and never touches data for an empty view. The text loop
and array accesses add no `inbounds`, `noalias` or overflow promises.

Raw primitive/nested addresses, null state, casts through `void*`, pointer-slot
references, raw indexing, arithmetic and signed element differences use the
evaluated MIR operands. Integer-plus-pointer preserves source evaluation order.
Checked address assertions retain verified witness loans while returning the
selected physical pointer, never the witness's address. Checked erasure uses
the exact `ErasedObject` capability declaration identity; ordinary classes with
similar names gain no representation support. O-LLVM-09's represented-erasure
generalization includes cleanup-free integral enums: mutable restoration writes
the original enum place, read-only restoration preserves its access, and an
unrelated enum value remains untouched. This supersedes the earlier native
rejection of that shape; it does not admit a forged erased capability or relax
the source escape, overlap, or read-only rules. The opaque referent has no
by-value object representation. Restoration/return publication retains exact
source, producer, type and access facts. Ordinary unsafe GTI functions remain
calls to their inventoried source bodies, not intrinsic substitutions.

Literal storage includes a final NUL byte without changing counted length.
The C-string call-input adaptation requires a complete NUL-free source literal
or an immutable local alias traced to that literal. Dynamic views and embedded
NUL values reject before artifact publication. Current frontend call matching
is broader than the documented C-string rule; the native backend deliberately
does not copy the C++ backend's unchecked `.data()` conversion. Resolving that
frontend/MIR contract discrepancy is a separate prerequisite for broader
C-string argument admission, not authorization for an implicit conversion.

The registered profile and installed-consumer gates cover macOS arm64 and
Linux x86_64. A self-checking 256-element/1,048,576-round update workload on
macOS measures C++23/O3 at 85.19 ms and LLVM O3 at 77.27 ms (5 warmups,
20 samples); executable sizes are 33,440/33,512 bytes, direct builds
0.203/0.120 seconds, and peak compiler RSS 73.2/98.8 MB. The workload has no
GTI heap-allocation operations; process-startup allocation was not measured.
This is one bounded workload, not a general backend speedup claim.

Integral-enum/nonprimitive array/reference composition and fixed-array value
equality remain explicit preflight limitations. Scalar equality does not imply
an array element comparison implementation. Ordinary class execution is described
below; private owning storage, payloads and native calls remain later milestones. Source restrictions
are unchanged: explicit array-reference declarators and conditional lvalue
initialization are not added by backend work. Constrained range references to
fixed arrays are the implemented source entry point for whole-array borrowing.

### LLVM Classes, Lifecycles And Storage

The O-LLVM-06 execution profile admits exact concrete, non-polymorphic,
classes, including immovable and concrete generic classes. `layout.cpp`
owns target-native field offsets and an additional active-state byte only when
the sealed class requires source-destructor state. Class values travel as
addresses through the private GTI ABI, not native C aggregate registers.
`places.cpp` projects fields by exact symbol identity. Constructor entry Stored
loans publish reference-field addresses rather than copying referent contents.

`body.cpp` emits selected constructors, direct methods, field/static initializer
bodies and explicit lifecycle transitions. `placement.cpp` plans a single
physical destination for each fresh construction/call result, following exact
MIR Initialize, Return, prepared-argument publication and aggregate-element edges.
Caller result destinations propagate through nested fresh returns; class fields
and fixed-array elements are constructed at their final address. This is not
optional named-return elision: a Copy/Move operand remains an explicit transfer,
and no named local is aliased to the caller's destination. Conflicting fresh
destinations fail closed. Class-containing arrays carry addresses, not LLVM SSA
aggregate snapshots. Their projections use the same typed address engine as
classes; a raw-pointer value must still be materialized as pointer storage when
its MIR place selects that value rather than its pointee. Copy operands
snapshot their values at the MIR stage, before a later argument can mutate the
source. Already-staged class parameters retain their prepared storage address.
Every `CallInput` with a destination publishes into that exact MIR place before
activating its prepared-parameter obligation. Address-only class results then
refer to that storage, so failure of a later argument and successful transfer
to the callee observe the same object. Merely activating the obligation without
populating its destination is invalid, including for moves from named locals.
Ordinary, direct/braced/designated construction, generated copy/move transport,
fresh immovable returns, returned movable values, replacement cleanup and fresh fixed arrays of immovable class
objects use the same operations; no native representation destructor runs GTI
cleanup. In-place scalar assignments use the existing promoted arithmetic and
checked conversion implementation before publishing the updated value.

`lifetimes.cpp` caches native move-state and cleanup helpers by exact layout or
sealed drop recipe. It follows the explicit source destructor and reverse field
or array-element cleanup order. Source state and body-local obligations retire
before invoking cleanup. A first ordinary cleanup failure drains remaining
subobjects; another failure terminates through the existing emergency protocol.
During unwinding the first new failure returns to the owning MIR failure edge.
Partial-construction rollback executes only the completed obligations; cleanup
recipes also add their selected destructors to native reachability.
The prepared ReturnResult obligation retains cleanup authority over a completed
caller-destination object until remaining local cleanup succeeds. Failure drops
that object before propagating, without relocating it. Passive fixed-array drop
recipes may have no active element recipe and produce no element-cleanup walk.
Targetless generated default construction is admitted for exact fieldless,
baseless objects with no required initializer schedule; it arms their own
destructor state without inventing a constructor call.
Prepared class/array arguments use the same physical storage as their sealed
parameter-drop obligation. Fresh objects construct there before publication;
explicit copies/moves publish their snapshot before arming cleanup. A later
argument failure therefore destroys that exact object, not an uninitialized or
relocated stand-in.

Passive namespace/static class objects consume complete typed object constants
or the existing ordered initialization plan. Owning process globals remain a
source-language rejection, not a backend limitation to work around. TLS uses
its separate sealed storage and initializer inventory. Literal slots are native
TLS data; dynamic slots have explicit uninitialized/initializing/live/retired
state. A live-slot check precedes the cold initialization path. The cold path
checks recursive initialization, runtime attachment and shutdown, resets state
on failure, and registers exactly once after successful publication. Cleanup
marks the slot retired and follows its exact drop recipe. Already-live slots
remain accessible during drain; new initialization and resurrection reject.

`runtime_execution.h` now exposes the standard-layout C
`gti_thread_local_cleanup_entry_v1` record plus registration/attached/closing
services alongside begin/drain. The existing C++ helper name aliases that C
record; its intrusive registry and drain algorithm are unchanged. LLVM models
the C layout and callback signature directly, with no C++ cleanup-entry API,
exception transport, hidden allocation or native TLS destructor. A C17 oracle
and target-layout assertions check this boundary independently.

Stored reference fields consume either their entry capability or the address
published by their non-entry MIR Borrow. The latter executes at its actual
initializer stage, including after a reference-return call's success edge.
`Places::selected` preserves the evaluated referent address even when the
protected borrow origin is an enclosing object. Field publication stores that
address, not a scalar load and not the enclosing origin's address. The backend
does not change permission, escape or loan endpoints.

Exact brace arrays now initialize fresh immovable elements without requiring
movement. Both backends preserve final addresses and MIR's per-element cleanup.
The C++ consumer uses external `mir_lifetime_slot` views into the final array;
the existing byte storage starts implicit-lifetime array/aggregate shells,
while constructors start each class element's lifetime. Publication releases
the element views without destruction or relocation and activates the aggregate
obligation. TLS initializer arrays use their exact ABI result storage, including
nested-element lifetime views. Cleanup-free aggregates without element lifetime
records retain the existing pure-construction/staged-value protocols; exact
constructor prvalues in that pure path need no move capability.
For array fields, the existing empty-state proof permits retiring
each disarmed native shell immediately before that element's construction.
No source effects are hoisted and no automatic native destructor owns rollback.
Internal lifetime slots retain their original storage size; only external views
carry an address and engagement state. Immovable by-value
arguments, named-local returns and conditional-value merges also keep their
existing semantic rejection. Bases/dispatch, union/payload storage and private ownership
capabilities retain their later milestones. Function-local TLS with compound
assignment and moving owner-bearing array fields out of a containing class are
tested natively; the C++ comparator currently rejects those specific
representation shapes. This task does not relax its gates. Recursive source
move-state clearing covers class arrays as well as class subobjects, while
passive arrays/classes emit no state-clear walk.

`llvm_placement_execution` checks deleted-copy/deleted-move objects, fresh return
chains, branch-selected returns, constructor/default field publication, TLS,
class-array copies/movement and retained semantic rejections at O0-O3. Fresh
direct/nested/default/field arrays compare C++20/C++23 with native execution.
The C++ comparator still relocates nested/default scalar fields in
address-sensitive probes, and rejects the class-array argument/copy/move and
side-effecting reference-initializer fixtures in its representation vocabulary.
These cases have explicit native-only
oracles, not claimed C++ parity; supported comparators still run C++20/C++23.
`llvm_target_layout_abi` observes exact post-failure cleanup traces after real
checked field, array and prepared-return calls, not just terminal exit codes.

A self-checking eight-million-step class-field/method workload on macOS arm64
measured C++23/O3 at 15.60 ms and LLVM O3 at 14.36 ms (five warmups, twenty
samples). Executables were 16,848/17,016 bytes; direct builds took 0.213/0.096 s.
The program contains no GTI heap-allocation operation; process-startup
allocations were not measured. This bounded probe shows no material regression,
not a general class-backend speedup claim.

### LLVM Unions And Payloads

The native payload plan belongs to `Layouts`, keyed by exact concrete sealed
enum identity in one session. Safe sums use a 32-bit alternative index followed
by aligned overlapping storage; every alternative has its exact field storage
types, including byte-backed booleans. All consumers query that plan for tag and
field addresses. The reserved `UINT32_MAX` encoding is representation-only
inactive/moved state: the plan verifies that every source alternative index is
strictly smaller. No niche optimization or source-visible empty case is inferred.

Fresh payload arguments share destination publication with classes and arrays.
The complete tag is written only after the operands have completed. Cleanup
consumes the exact MIR alternative/field recipes, retires the active tag before
running children, and preserves reverse cleanup and primary/secondary failure
handling. Consuming extraction selects the original payload field rather than
moving a whole enum before each extraction. Borrowed extractions retain the
evaluated field address and upstream loan permissions. Native unions instead
use the sealed overlapping layout with no hidden tag or active-member guarantee.

Verified consuming patterns extract every active field exactly once, with no
intervening user operation or failure, and transfer out the shell's cleanup
obligation after the last extraction. The drained shell is never dropped or
read again. A scalar owner's old payload bits therefore need no separate
clearing merely to prevent a second parent drop; MIR transfers that cleanup
authority. This is distinct from ordinary moves out of reusable owner storage.

Dependency groups and active-alternative conditions remain verifier-owned
permissions. Their native witness requires concrete native source places and
exact alternative identities; it does not select the first possible owner as
the physical address. `MirReferenceSelection` uses its evaluated MIR value,
and reference-binding payload extractions retain the field or stored referent
address. A place-category `Load` is still a value, including a loaded raw
pointer; source category alone never decides this physical distinction.

Static payload constants preserve the same canonical offsets, extent and
alignment. A packed initializer with explicit padding and typed field fragments
represents the selected alternative, including pointer/text relocations. Arrays
and containing classes preserve their canonical strides and field offsets;
global storage receives explicit canonical alignment. This creates no startup
evaluation or allocation and does not introduce another constexpr evaluator.
Tracked inactive-state failures use sealed caller-location metadata just like
panic, retaining source attribution through source-defined wrappers.

Concrete variadic parameters remain flattened in the existing private ABI.
Their MIR pack binding is metadata: each projected element aliases its own
incoming argument cell, preserving final addresses without a tuple allocation
or a second aggregate copy. `parameter_packs` is the single shared planner for
ABI lanes, body entry, place storage and admission. It proves exact owner
parameter types/bindings and coalesces compatible MIR views by their verified
identity, never advancing an argument cursor once per view. Even an empty pack
retains its source formal binding; the text dump's omission is not a different
owner contract. Absent empty bindings and unproduced zero-continuation forms
are rejected. Complete scalar pack views need not carry an ownership PlaceKey:
exact formal binding, constant index and verified element type identify their
physical lane. Any supplied key must agree; missing keys are never fabricated
and this mapping grants no loan or access permission. TypePack descriptors erase
only for exact function/constructor
formals, while runtime pack SSA values stay forbidden. Symbolic, nested and
non-trailing native packs remain unsupported. Direct consuming methods similarly use
their exact selected source target and verified receiver transfer; this does
not admit erased callables or introduce a separate invocation ABI.

O-LLVM-07 remains under implementation. Its owning matrix covers nested and
generic alternatives, static/global/thread-local storage, ordinary source
optional/expected wrappers, stored references, source propagation, replacement,
consuming cleanup, partial-input failure and sibling cleanup after a destructor
fails. A C++ vocabulary gap for an owning conditional-result body is explicitly
recorded; native execution checks its values and exact destructor trace rather
than extending the deprecated comparator. Full platform/cost acceptance and
slot-capability composition remain obligations. The current optional library
uses ordinary enums, not the legacy `OptionalStorage` capability. The live
`PolymorphicStorage` family requires concrete polymorphic object layout from
O-LLVM-10; heap-owned compositions also require O-LLVM-08. Neither dependency is
silently granted by the existence of a payload layout.
### LLVM Function Values And Closure Environments

O-LLVM-09 lowers exact function items, invocation-pointer capabilities and
concrete lambda instances from verified MIR. `callables.cpp` resolves only
sealed body identities and signatures. A function item is physically stateless;
a function-pointer capability stores a typed internal adapter address; a lambda
stores its captures in target-layout order. None of these paths recognizes a
public callable-wrapper name or falls back to generated C++.

Lambda bodies use the private GTI ABI planned by `Abi`. A capturing lambda has
its environment as the planned receiver; a captureless lambda has no mandatory
environment channel. Direct calls retain their exact ABI. Invocation pointers
use a checked adapter built from the same `FunctionAbi`, so failure-free targets
can participate without a function-type cast and failure-capable targets retain
the original failure record. This is the implemented status-based C04 baseline,
not a decision that freezes the campaign's open failure-transport evaluation.
Native C callback adapters remain the separate O-LLVM-11 boundary.

Capture construction consumes the MIR-owned capture types, order and copy/move/
borrow modes. Borrowed captures store only the selected referent address and do
not acquire cleanup. Owned captures use the ordinary typed storage, move-state
and reverse cleanup machinery. Returned closures, stored closure fields,
prepared consuming receivers and later-argument or later-field failure therefore
share the same final-destination publication and rollback rules as ordinary
classes. An ordinary closure environment stays in its caller-owned object
storage; the backend does not allocate merely to form or invoke it. Ownership
capabilities whose storage policy itself requires allocation still compose with
O-LLVM-08, and polymorphic callable wrappers still compose with O-LLVM-10.

Several instantiated lambda bodies may have the same fully concrete lambda type,
for example when a default field initializer is retained both as its own body
and through generated construction. They may share one physical environment
layout only when every instance agrees on ordered capture storage types, capture
modes and active-cleanup state. This representation check never selects a body:
formation and invocation retain their exact `lambdaTarget`, while cleanup
resolves the exact `MirDropType::lambdaInstance` before checking its capture
selectors and nested recipes. Body-local bindings, effects, borrow contracts and
source identity are therefore not canonicalized with storage.

Checked erased-reference transport is admitted from the verifier's exact
referent type, access, origin and loan facts. The LLVM backend accepts any
already-represented complete object, plus concrete closure and invocation-
pointer storage, and carries only the selected address through erase/restore.
It does not recognize `std::function_ref` or any other public wrapper name;
wrapper construction and invocation compose from their ordinary source bodies.
An unrelated source helper can relay that address only when the central MIR
verifier proves its finite exact call chain reaches the trusted restore
intrinsic with the same opaque witness, referent type, complete field path,
access and argument origin. A complete scalar referent legitimately has an
empty field path; a summary flag, tombstoned producer, changed origin/access or
cyclic helper chain without the intrinsic endpoint is not authority.
Abstract, opaque, symbolic and unsupported capability storage remains rejected
even when a similarly spelled source wrapper exists.

An unstaged constructor-field record is also a distinct pending admission
requirement. One shared plan proves the exact constructor owner, field, SSA
result and unique record producer before the body may use the field as that
producer's final destination. A staged `Initialize` sibling suppresses this
route. The requirement is witnessed only by an independently admitted scalar
transport or concrete storage shape, so adding an owner representation does not
silently claim constructor publication for it.

That shared plan has two explicit transports. `ExactValue` retains the original
same-type scalar/class/callable route. `NullUniqueOwner` accepts only a real
source `nullptr` literal with `NullPtr` value type and no operands, call/effect,
borrow, ownership, lifecycle, unsafe or target metadata, targeting exactly one
complete `UniqueOwner<T>` field. Admission additionally requires the target's
implemented `NativeUniqueOwnership` shape; the callable component therefore
fails closed without O-LLVM-08. Native emission keeps the source null SSA value
but writes a target-typed null pointer directly to the owner field exactly once.
It never treats the source `NullPtr` as owner placement storage, invents drop
state or recognizes the source-defined `std::unique_ptr` name.

A constructor `Stored` loan is not itself a representation decision. The
shared publication plan resolves the exact declared field and initializer. A
reference field publishes its selected referent address once; a class or lambda
field carrying nested borrowed state is provenance-only because its complete
by-value object was already published by the verified move/copy stage. Native
emission skips both a second field write and runtime loan-slot materialization
for that provenance-only handoff. Missing, ambiguous or type-inconsistent
field facts fail closed rather than treating every Stored loan as a pointer.

Concrete trailing parameter packs have no runtime pack object. One exact
concrete-parameter plan serves admission, body lowering and place storage. It
accepts the verified MIR forms currently emitted: the source trailing
`TypePack`, or expanded parameter types retaining one trailing source-level
pack binding. A per-lane binding vector with zero-valued continuations has no
sealed producer and is rejected. An empty pack has no physical lane, but its
trailing source binding remains in the owner's verified binding vector; a
body-local available pack root is not a substitute for that identity. Repeated
MIR views with the same verified binding, constant element index and type
coalesce into one logical pack or element root and never consume another ABI
lane. A projected view need not carry an ownership `PlaceKey`; every key that
is present must exactly extend the pack root with that element index.
Conflicting keys, types or formal identities fail closed. The body maps each
used projected `PackElement` to its typed ABI lane. Concrete packs embedded in
a lambda's enclosing substitution identity are inventory metadata rather than
executable storage.
Symbolic, nested, non-trailing and mismatched mappings remain unsupported.

`llvm_callable_execution` compares direct/indirect calls, read/mutable/once
invocation, returned/stored and nested generic closures, borrowed-carrier field
storage, checked erased source wrappers, failure propagation and cleanup with
the production backend where that backend has a matching representation.
Native-only wrapper controls exercise final-field placement and rollback after
a later initializer. Borrow rejection remains a frontend result.
The optimized owned-closure probe also audits the linked executable for GTI
allocation-provider symbols; performance and code-size observations are bounded
workload evidence, not a general speedup claim.

The 2026-10-25 macOS arm64 checkpoint uses 400-million-iteration workloads so
each measured run exceeds 0.09 seconds. Five-sample medians for C++23/O3 versus
LLVM/O3 were 100.606/100.519 ms for direct calls, 102.091/101.377 ms for a
borrowed-capture closure, 94.638/649.676 ms for an invocation pointer, and
99.422/654.378 ms for the mixed closure/pointer path. Direct and closure paths
are within about 1%; the roughly sevenfold pointer overhead is stable and
material.
The pointer executables were 107,048/122,344 bytes; direct executables were
106,744/105,688 bytes and closure executables 106,744/105,640 bytes. The
separate owned-closure executable was 105,768 bytes and retained no GTI
allocation-provider symbol. This isolates a remaining indirect-adapter/inlining
cost; it is not final O-LLVM-09 performance acceptance.

### LLVM Coverage And Admission

`llvm_admission.h` exposes `llvmCoverageLedger()` and
`planLlvmAdmission(VerifiedLoweredProgram, LlvmBackendOptions)`. The plan shares
ownership of the exact immutable verified snapshot and retains optimization and
debug policy; source identities and target come from that snapshot. It cannot
be constructed from a frontend, mutable MIR, an inspection print or an unverified
program. `src/compiler/llvm/` owns the private inventory and vocabulary.

`coverage.def` assigns every current type/capability, MIR instruction/operation/
terminator, place/projection/operand/loan/lifecycle, intrinsic, failure,
synchronization, native-runtime, declaration/generated-item and startup enum to
a named O-LLVM milestone. Entries distinguish pending lowering, implemented
lowering and verified nonexecuting facts. Scalar execution entries name the
`llvm_scalar_execution` gate; implemented places/text shapes name
`llvm_place_execution`; class/TLS shapes name `llvm_class_execution`.
A nonexecuting permission/provenance fact can be inventoried without a
runtime operation; a source-only query marked `MustBeAbsent` cannot silently
be erased if it appears as executable input. Neither classification grants LLVM
attributes or permits removing a runtime check.

A pending vocabulary family can have a narrower implemented shape. Each use
retains its original ledger row and an optional inventory-produced structural
implementation witness. Concrete primitive arrays satisfy `PassiveArrayValue`,
not the whole `Aggregate` family; admitted ordinary class arrays use the separate
`NativeClassStorage` witness. Private capability/payload arrays remain rejected. Distinct
shapes have distinct inventory keys, so a supported occurrence cannot hide an
unsupported occurrence of the same enum in the same body. `MustBeAbsent` rules
are never overridden, and callers cannot supply witnesses. These facts live in
the backend admission plan, not in `LoweredProgram`.

The compiled switches reject new unassigned enum members, including enums with
no count sentinel. `input_fields.def` checks the decomposition of the central
input records, so new fields require coverage/admission review too. Literal and
declaration/generated payload variants have corresponding compile-time guards.
An executable implementation claim is written at its ledger row as
`LLVM_COVERAGE_IMPLEMENTED(test, profile)`. Bare `Implemented` is not a valid
claim and there is no milestone-wide scalar fallback. Pending rows carry no
execution acceptance. Verified nonexecuting rows instead name the structural
ledger assertion which verifies their rule. An already implemented row with no
matching executable fixture uses `LLVM_COVERAGE_IMPLEMENTED_GAP(reason)`; that
preserves the implementation classification but blocks promotion rather than
manufacturing evidence.

`llvm_coverage_evidence` reconciles those claims with the configured CTest
inventory and JSON evidence manifests derived from the execution tests' actual
runnable case dictionaries. Manifest schema
`gti.llvm.execution-evidence/v1` binds a test to named profiles, each profile to
stable source IDs, and each source ID to its repository/inline origin and the
SHA-256 of the exact UTF-8 text. Unknown fields, stale hashes and dangling
source IDs reject. Evidence producers register through the CMake-owned
`gti_register_llvm_evidence_manifest` service. Its deferred writer emits one
generated registry after the tests directory has declared every producer, so
the audit does not maintain a second hard-coded test list. Malformed, duplicate
or non-CTest registrations, missing scripts and scripts without the manifest
contract reject. The registered CTest command must execute that exact script;
naming an unrelated test is not evidence. A test-only admission probe lowers
each named source and
requires its plan to contain the exact row, including structural implementation
witnesses. This establishes that the test source reaches the claimed coverage
vocabulary. It does not establish that an unrun test passed, that cleanup and
failure behavior were correct, or that another platform ran the case. The
report therefore records exact source state, build configuration,
host/compiler/LLVM identity and separate `passed`, `failed`, `unrun` or
`unavailable` observation states. The compiled source identity must agree with
the live repository HEAD and cleanliness, preventing an older helper from
certifying changed audit code. Raw ledger reports start unrun; dirty source is
explicitly non-exact. Structural completeness and observed execution are
reported separately. Production promotion requires both on an exact source;
an all-implemented ledger with unrun tests is never promotion-ready.

The payload, callable and memory execution gates participate in that same
manifest registry. Their profiles are derived from the case dictionaries the
CTest commands actually execute, including payload pack/access failures,
public checked arithmetic, callable transfer/cleanup and the memory/lifetime
matrix. A checked source profile is therefore not a hand-maintained alias for
some other test. Admission inventories each verified callable argument
descriptor separately from the callable receiver's invocation boundary. The
MIR verifier binds those descriptors to an exact target parameter, boundary
and owned-transfer stage, so an owned carrier call cannot be inferred from an
adjacent closure or forged by a backend caller. Explicit raw-pointer conversion
reaches verified MIR as an effect-free `Compute/Convert`, not as an executable
intrinsic call. Admission reuses the central `rawPointerConversion` verifier
before attaching the `NativeObjectAddress` witness: both operands remain exact
raw-pointer values, the conversion cannot add mutable access, concrete
pointees must match unless one side is `void`, and each non-void pointee must
have a supported native representation. Numeric conversions and malformed
pointer conversions cannot borrow that witness. These are coverage
classification corrections for existing language behavior; they do not add a
callable form or cast, or widen either source rule.

The inventory records types and nested arguments, access and ownership modes,
exact instructions and their failure/cleanup/publication shapes, generated
dependencies, native signatures, program initialization and thread-local data,
initializer and lifetime operations. Repeated requirements coalesce by body and
context, retaining a count and the first source witness. Required executable
definitions remain separate from ABI/data-only bodies and uninstantiated source
declarations. The latter are inventoried as metadata without demanding that
their symbolic types have runtime storage.

`SealedEntryDependencies` roots the module and hosted startup plus source C
exports and TLS initializer bodies. It follows selected direct functions,
constructors, lambda/address-taken function identities, explicit body calls and
the sealed generated-item dependency graph. Required bodies retain inventory
order. All bodies are still scanned; dormant uses are recorded as non-required,
not erased from coverage. Reachable values independently require their concrete
types. Passive data without a use is metadata; ordered executable initialization
is never omitted. Unknown indirect/virtual/callback/task dispatch,
declaration-backed generated dependencies, embedding exports and rootless
libraries conservatively select `AllExecutableBodies` and retain all generated
items. Those shapes reject until their families are admitted. This is a bounded
omission proof, not speculative whole-program devirtualization.

Admission scans the full inventory before returning a result. The first
unsupported requirement is deterministic, preferring an instruction with source
provenance, then the first remaining required fact. Its diagnostic names the
ledger key, milestone, body and available block/instruction. Debug source rows
provide an instruction span when available; otherwise the owning body span or
driver entry fallback is used. The generic `BackendUnsupportedError` carries
that optional span through the existing driver diagnostic path. A forged
`LoweredProgram` still fails verification as an invariant error before admission.
No artifact is emitted or published on a limitation, and an existing output
file is preserved.

The configured dependency gate uses CMake's generated final target source list,
including sources introduced by included build files. The source-only lint
also checks literal root declarations. It discovers all public
`llvm_*.h` headers and private `src/compiler/llvm/` helpers. It follows private
project includes, rejecting frontend construction, optimizer and C++ helpers
even behind an innocently named local header. The sealed contract headers stop
that authority walk because they still cohabit with legacy value declarations;
public LLVM/Clang exposure is checked through their transitive includes too.
Mutation gates exercise added helpers, transitive forbidden includes, hidden
source ownership, public exposure, new enum members and new aggregate fields.

The behavioral corpus compiles/runs through `tests/backend_execution.py` with
explicit backend and optimization selection. C++ endpoints retain C++20/C++23
and MIR marker/ownership assertions. LLVM endpoints use native artifacts and
observable exit/stdout/stderr comparison without C++ representation options.
Current LLVM corpus runs must fail until their requirements are implemented;
unsupported builds are never counted as successful execution.

Readiness reporting keeps three different claims separate. The coverage ledger
classifies the complete compiler vocabulary, so its global `Pending` count
remains visible. Admission is use-specific: a required use of a pending family
is supported only when that exact `LlvmCoverageUse` carries an implemented
shape selected by the inventory. The test-only coverage probe serializes that
binding directly from the admission plan in
`gti.llvm.required-uses/v1`; seeing the base and shape keys elsewhere is not a
witness. Production admission and `firstUnsupported()` remain authoritative.
The report validator preserves the plan's diagnostic selection: the first
unsupported source-located instruction takes precedence, with the earliest
unsupported record retained as fallback. Malformed base rows, witnesses and
location records reject before that selection is evaluated.

`tests/llvm_readiness_baseline.json` is the versioned durable readiness
contract. It pins the frozen-baseline provenance, canonical source providers,
supported-family obligations, shipped target/profile identities and complete
O-LLVM-16 gate names without copying body/block/instruction inventories. Exact
required-use inventories, counts and digests are run artifacts tied to the
source, compiler, build and target that produced them. Receipts for a different,
truncated or stale artifact do not apply. The normal focused evidence gate
emits `gti.llvm.readiness-run/v1` with an explicit
`focused-schema-control` scope and therefore cannot make a candidate promotion
ready.

Baseline structural readiness means every member of every canonical provider
has an exact admitted use artifact for each required target/profile, including
the actual implemented witness on each pending required use. It is still only a
prerequisite. General language-family review, registered positive and negative
execution, cross-family interactions, both shipped platforms, optimized cost,
sanitizers and installed-package gates remain independent requirements for
campaign promotion. Missing providers, unrepresented supported families,
unrun gates and unavailable platforms are reported as unresolved; they are not
converted into a count of missing features. The additive
`gti.llvm.coverage-readiness/v2` summary retains the preceding global-row fields
for consumers of the unversioned report while adding the distinct baseline and
campaign results.

`tests/llvm_readiness_collect.py` provides opt-in `plan`, `collect` and
cross-host `reconcile` operations. Collection reuses the census, registered
producer APIs and validated stdlib surface manifest, preserving declared case
context. Missing role, loading context or target/profile applicability remains
unresolved; filenames and fixture registration do not establish a positive
standalone program. The current census and execution-manifest vocabulary lack
reusable case-context metadata. Stdlib cases retain their declared context,
but missing target applicability still prevents a complete collection claim.
An explicit frontend-rejection control can satisfy only its declared control;
it never establishes executable support.

Collection artifacts use `gti.llvm.readiness-collection/v1` and
`gti.llvm.readiness-member-receipt/v1`. Receipts bind actual bounded helper
output and required-use artifacts to the source, provider/baseline revision,
compiler binary/build and target/profile. Validation rechecks the actual
artifact and admission selection before trusting a receipt's status or digest.
Independent optimized platform builds may reconcile for the same source and
provider revision; reuse within a target requires the exact binary/build.
Attempts form append-only history: interrupted work is distinguishable from
failure, and retries retain the original logs and outcomes. Output directories
must be specific paths outside the source tree; collection never deletes them.
Structural observations always retain execution, cleanup, performance and
campaign signoff as unrun. The focused `llvm_readiness_collection_controls`
uses real scalar/callable/rejection probes and explicitly scoped miniature
recovery controls, never a full-provider or platform execution claim.

The remaining architecture obligations have explicit owners:

| Obligation | Producer / representation owner | Verification and next gate |
| --- | --- | --- |
| Native layout, private function ABI and failure-result transport | Sealed type/call/failure facts; LLVM target/layout/ABI session | Target/layout/signature and status-publication tests, O-LLVM-03/04. No LLVM facts in the lowered contract. |
| Exact emission roots for a partially supported program | Sealed program kind, declarations, bodies and generated dependencies; backend closure planner | Dormant-prelude, global-init, export, indirect/virtual/callback controls before omitting a body, O-LLVM-04. |
| Versioned host/runtime bindings | Exact runtime declarations plus C ABI implementations; runtime/LLVM binding owner | Link/signature/status/cleanup tests, O-LLVM-03/06/12/13. Existing C++ inline/template services need explicit native bindings. |
| Native C aggregate calling convention | Sealed native declarations/layouts; target ABI classifier | Bidirectional argument/result/callback oracles, O-LLVM-03/11. Field offsets alone are insufficient. |
| Complete virtual dispatch facts | Semantics/HIR/MIR publish exact bases, roots and targets; LLVM chooses representation | Dispatch-plan audit and verifier mutations, O-LLVM-10. Extend the upstream contract first if these facts prove insufficient. |
| Worker/TLS cleanup and atomic wait ABI | Verified task/storage/synchronization schedules; C runtime adapters | Source failure, attachment/drain, memory-order and cleanup matrices, O-LLVM-06/13. C++ unwinding must not cross generated LLVM frames. |

These are implementation prerequisites, not claims that runtime migration or
GTI executable emission is complete. O-LLVM-03 supplies the bounded session,
layout/signature and object machinery described above. The operational exit gates
remain in the [LLVM sequence](../plans/implementation-sequence.md#llvm-production-backend-campaign).

### LLVM Readiness Audit (2026-10-24)

Initial source baseline for the references below:
`a31de551ee796766d38dfd15986257f610f01345` (0.472.0),
including the independent backend foundation and source-defined concept policies.
This is a structural review, not evidence of executable LLVM support. The
foundation's previous integration recorded 759/855 tests passing before the
owner stopped that broad run; it did not complete the full suite. Its focused
backend and installed-consumer checks passed. Neither result certifies future
LLVM-generated programs.

Reconciled main `3cc3b3dc1b39b9cf55f04d1ddae7d533bde2b86d` (0.473.0)
adds thread-local storage. The reviewed contract delta includes
`MirThreadLocalStorage`, initializer bodies, explicit ensure/begin/drain
operations, a generated-storage item and new defined-failure outcomes. These
are part of the campaign's final coverage baseline. The runtime cleanup-entry
and lifetime APIs in `runtime_thread.hpp` are currently C++-only, adding another
concrete C-ABI prerequisite; ordinary class/storage work owns TLS representation,
and the concurrency bucket completes multi-thread entry/drain composition.
O-LLVM-02 subsequently implements the vocabulary, admission and growing-source
dependency gates described above; the table's fixed-two-file observation below
remains the original audit finding, not the current gate behavior.

**Assessment:** retain the sealed input, backend interface, distinct library,
and driver-owned artifact boundary. No replacement IR or pipeline refactor is
justified by this audit. The foundation is a sound starting boundary, but native
execution contracts are still prerequisites, not completed architecture.

| Finding and concrete evidence | Consequence and owner |
| --- | --- |
| `LlvmBackend::generate` verifies the owned input, checks the host, initializes LLVM, and rejects execution (`src/compiler/llvm_backend.cpp:61`). `LoweredProgram` owns MIR and concrete/generated inventories (`include/gti/lowered_program.h:738`). | Correct foundation. Keep one verified immutable input alive for the entire generation session. Shared value types named `SemanticType` or IDs named `Hir*` in that contract are not permission to consult semantic/HIR objects. |
| `LlvmBackendOptions` contains only optimization/debug switches; there is no target machine, module, layout or function ABI planner (`include/gti/llvm_backend.h:18`). Native C/union layout rows exist, but ordinary class layout is not a published C++ ABI (`include/gti/mir.h:1269`, `include/gti/lowered_program.h:241`). | Fix before real lowering: a private target session, recursive type/layout map, symbol/function ABI plans, and explicit failure-result convention. Ordinary object representation remains backend policy; missing language facts must be added upstream with verifier coverage. |
| `LoweredFunctionDeclaration` records linkage and exact external symbols, but not the platform's register/indirect classification of a C call (`include/gti/lowered_program.h:318`). The C record oracle checks physical offsets (`tests/native_record_c_oracle_test.py`). | C record layout alone cannot establish a working foreign call. Native ABI classification needs bidirectional compiled C oracles for arguments, results and callbacks on each shipped target. It must be separate from the private GTI calling convention. |
| `runtime/include/gti/runtime.hpp` adapts C++ text views to C records; `runtime/include/gti/runtime_thread.hpp:12` uses a private C++ exception for worker failure handoff. `src/compiler/cpp_support_preamble.cpp` emits templated lifetime/owner helpers. | Runtime migration is required. Reuse versioned C services, lower type-dependent mechanisms from the verified program, and add narrow C status/callback entry points where needed. Calling a C++ helper or throwing through LLVM frames is not a portable runtime ABI. |
| The LLVM-specific source gate lists exactly two files and its C++ include pattern targets `gti/...` headers (`tests/backend_boundary_test.py:82`). | Strengthen before adding implementation files: discover the backend's complete source/header ownership, inspect local and transitive private dependencies, and add negative mutation controls. Current passing gates do not protect an unlisted future `llvm_*.cpp`. |
| Object publication uses a binary stream, although the helper is named `publishTextAtomically` (`src/driver/build.cpp:1967`). Linking distinguishes source and object (`src/driver/build.cpp:3554`), but object caching is bypassed and project selection remains C++. | Keep the existing byte artifact and publisher. Add real-object execution tests, project/cache identity, target policy, debug artifacts and installed-toolchain coverage before making LLVM the default. A helper name alone is not grounds for a refactor. |

The highest early correctness risks are unchecked LLVM poison/undefined behavior,
success-only publication of call results, partial-construction cleanup, native
ABI mismatch, and thread/callback containment. These cross language buckets and
must be covered from their first use. In particular, GTI `Invoke` expresses a
verified success/failure split; it does not require LLVM exception-handling
`invoke` or a host personality function.

The accepted production direction and complete prerequisite/acceptance sequence
are in [ADR 031](../decisions/031-native-llvm-backend-foundation.md) and
[O-LLVM](../plans/implementation-sequence.md#llvm-production-backend-campaign).

### LLVM Execution Audit (2026-10-25)

Baseline `237129c6c0ab2974babaeb6c4b9eb226c321e344` includes O-LLVM-05/06
places, ordinary class lifecycles and TLS. The earlier readiness audit above is
historical; its missing session, dependency gates and TLS C registry are now
implemented. The sealed-input/session/layout/ABI separation remains sound; this
audit does not justify another IR or a backend-wide refactor.

The three existing native execution suites passed, but a new differential probe
found an admitted-program bug: a moved named class argument activated its
prepared drop without storing the object in that place. A later argument panic
then destroyed uninitialized storage. `body.cpp` now publishes the `CallInput`
destination before lifecycle activation and uses that same address for class
transport. `llvm_class_execution` checks reverse cleanup of staged arguments,
constructor-argument failure before invocation, and exactly-once cleanup after
callee failure, against C++20/23 and native O0/O1/O2/O3.

This is bounded execution evidence, not whole-language conformance. Native C
aggregate classification, private ownership capabilities, virtual dispatch,
worker/callback containment, object caching and default-backend promotion retain
their campaign prerequisites. No pending admission rule is weakened by this fix.
The sequence owns future implementation instructions. The sections above remain
the authority for what is implemented today.
