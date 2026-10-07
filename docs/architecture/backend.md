# Backend And Native Handoff

Status: current architecture.

Passive class error returns use representation-only SSA storage: ordinary
construction or checked copy/move extraction executes at its MIR instruction,
before surrounding scope cleanup. An exact sole `Unexpected` consumer may
publish after a verified normal cleanup spine, including fallible destructor
Invokes. Only wrapper construction is deferred; payload construction, failure
identity and all Drops stay in MIR order. Payloads must have no active cleanup,
borrowed state or address escape, and copy extraction additionally requires
generated copy construction. A failed cleanup therefore discards no hidden
owning result. Shared constructor/extraction admission follows the same
cleanup-tail proof; the runtime does not invent unwinding behavior.

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
scalar initialization, publishes only on success, and routes exhaustion through
the instruction's failure edge. A direct allocation-function call prevents
C++ new-expression allocation elision from deleting this observable detector.
The helper contains no constructor dispatch or source-level type selection.
Selected ordinary class construction uses MIR's separate `AllocateObject`
resource and exact constructor call. Raw storage has no live pointee cleanup;
its MIR failure Drop only frees bytes. Prepared owning class arguments occupy
lifetime slots before allocation, so exhaustion can destroy them. A successful
constructor publishes the owner, while a failed constructor retains its
original record and follows the explicit raw-storage cleanup edge. The
existing bounded constructor rollback proof remains mandatory; targetless
generated construction and broader subobject rollback remain migration work.

## Opt-in Source Debug Locations

Source-level debugging is a statement-line first slice, not a GTI debugger or
a complete source-level type/variable description. `CompilationRequest` can
ask the frontend-aware `LoweredProgramBuilder` to capture parser-recorded
statement spans against the **optimized** MIR's retained HIR identities.
Each `LoweredBody::sourceLocations` row owns its source path/span and exact
block/instruction identity; instruction zero identifies a terminator. These
optional rows are verified and sealed with the lowered contract (printer
version 11), survive destruction of AST/HIR, and are never executable,
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
checked against the embedded MIR. Lowered-program serialization version 11
includes both flags and optional source debug locations. The C++ representation
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
BackendArtifact generate(const LoweredProgram &program);
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

Ordinary GTI raw-pointer values use an artifact-private union
`raw_address<T>` with one live native `T*` member. Its implicit union
copy/move operations copy the object representation, rather than evaluating
that member ([C++ union copy construction](https://eel.is/c++draft/class.copy.ctor#16)).
Qualification, erasure/restoration, native-field capture and assignment use
reference-based byte transfer; equality reads address bits by byte copy.
These operations remain inert after a pointee expires. Native pointer values
are reconstructed only for verified unsafe access/arithmetic or an explicit
foreign boundary. Optional provenance auditing does not make lifetime proof
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
Checked references to raw pointers remain outside the current language slice;
the backend must not alias a native `T*` slot as a `raw_address<T>&`.

Failure-free passive C-compatible call results share one consumer proof for
bare assignment, discard, and unchanged return. The exact record/type/signature
must agree, no result cleanup obligation may be invented, and return permits
only the existing passive cleanup tail. A return writes caller-provided result
storage at its Call; discard still executes the foreign call. Assignment and
discard use byte-preserving representation transfer rather than native
memberwise pointer reads. Neither source default construction nor owning-result
transport is inferred from this passive representation rule.

Checked class-payload extraction can construct a function's exact return result
at its existing MIR Call, without an intermediate object or a second source
move. Cleanup-free moved/copy receivers share the inert-return-tail proof;
owning receivers retain their real moved or fresh-call lifetime slot on both
success and wrong-state edges. The owning return consumer requires the exact
success activation, transfer-out, and, when cleanup can fail, unpublished
return-result reparent/rollback schedule. A non-owning placement adapter reuses
the checked extraction helper against caller storage; it supplies neither
implicit construction nor native unwinding. The state check still precedes
construction, and MIR alone decides whether to destroy or publish the result.

Results from consuming source members use the ordinary owning-call result
staging and replacement proof. The exact `Once` receiver must resolve to its
selected consuming member and materialized receiver place. Receiver cleanup
remains at the recorded full-expression/failure Drop; it is not an argument
transfer at Call. Only a successful result activates the replacement stage, so
receiver failure cannot overwrite the old destination, and failed replacement
cleanup destroys the unpublished result without committing it.

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
generic base. A defaulted internal output parameter is present throughout the
family: native-record results return `void` and publish bytes to that parameter;
ordinary scalar and owning results retain their original return and cleanup
convention. The C++ return spelling is conditional only while the owner type is
symbolic. Virtual methods therefore need no constrained overloads or native
by-value fallback. Verified concrete MIR still selects every emitted body;
the extra parameter is backend calling-convention glue, not a source argument
or new failure edge.

Generated field, capture and container representations preserve the same
property without changing MIR's active-alternative or cleanup schedule. The
native-record container matrix covers Expected and payload-enum alternatives,
native arrays, owning alternatives, captures and generic relays at C++20/C++23
O0/O1/O3, disabled elision and ASan/UBSan. The broad completion and installed
evidence is recorded in [verification](verification.md#low-level-memory-phase-1).
Unsupported representation families
must fail closed, never silently fall back to memberwise expired-pointer
copying. The ordinary address carrier alone does not discharge those composite
transport requirements.

The address representation guarantee is distinct from permission to invoke an
unknown callee with a known-expired pointer. Such a callee may access the
pointee. Semantics admits inert transport through proved direct/lambda targets
and bounded virtual families; an owning erased interface whose generic target
set is not known remains conservative, even if one particular source initializer
looks inert. Its native-record ABI is tested with live payloads, alongside an
expired-payload rejection test. Supporting arbitrary erased targets would need
semantic target/effect tracking through ownership, replacement and cloning,
not a backend or public-library-name exception.

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

Admission queries reject structurally inapplicable values before performing
body-wide ownership searches or instruction lookups. In particular, Expected
class-extraction receiver searches require an Expected-typed receiver, their
producer probes require the existing class/category/lifetime conditions, and
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

Each body plan owns its readiness and issues, canonical plain and failure text
templates, the deduced-callable variants where target family spelling differs,
boundary-declaration decision, initializer schedule and field spellings, and
hosted-startup kind. The plan is pointer-free; it outlives both the MIR program
and copied representation map used to build it. Placeholder replacement for
family labels and indentation is therefore formatting, not a second
admission path.

Whole-program admission validates MIR and the copied representation map once
per `analyzeProgram`/`buildProgramPlan` operation. Initializer vocabulary probes
and plan assembly reuse that operation's body analysis; nested base-constructor
probes use a private body-local vocabulary helper, not the standalone validating
entry point. Invalid global input stops before body plans are constructed.
Standalone `analyze`, `buildPlan`, and `initializerSchedule` still validate their
inputs on every call. No validation cache survives an operation or is keyed only
by a program address. Optional caller-owned metrics count actual whole-program
verifications for deterministic regression checks without changing admission.

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
Targetless `Construct` and `Call` default producers can also publish into the
same exact prepared-parameter slot: omitted brace-default fields in designated
construction retain CallInput activation, consuming handoff and failure cleanup.
These are representation proofs over resolved MIR, not source-AST replay or
standard-library type exceptions.

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

A moved class return is constructed at its MIR `Move`, before the cleanup
continuation, but is not published until Return. The Move must dominate the
Return, and later uses of its source remain restricted to moved-from drops.
The return classifier can traverse live destructor `Invoke` blocks only with
the verified `ReturnResult` obligation: failure cleans the initialized out
result, while success transfers it to the caller. Direct transformed calls
and explicit construction use the same pending-result destination. Failure
cleanup calls a fallible result's exact status cleanup recipe before native
teardown; an infallible result uses native teardown directly. A wrapped
`expected` value cleans its payload before tearing down the envelope.
Private prefix-storage handles keep their initialized result in an SSA carrier
until the successful Return; rollback drains that carrier on the explicit MIR
failure edge. This does not admit arbitrary executable work or reinitialization
between a class result's construction and Return.

When the result is the class payload of an `expected` return, cleanup may
similarly cross destructor Invoke blocks. The payload's Value obligation must
be reparented exactly once to ReturnResult, followed by cleanup-only success
blocks and one final publication. Until that Return, the payload remains in
its local lifetime slot: failure destroys that slot, not the caller's still
uninitialized `expected` output. The envelope is constructed only at successful
publication. Extra work, a cyclic continuation, or a changed activation,
reparent, or handoff fails the representation proof.
The producer may activate its Value obligation directly after a failure-free
constructor, or on the success edge of a fallible call. Both forms keep the
payload in its local slot while later parameter cleanup runs; a failure-free
constructor does not justify publishing early across a fallible destructor.

Owning error alternatives use the same placement-result boundary as owning
success alternatives, including `expected<int, Error>` and
`expected<void, Error>`. Scalar/void success with a cleanup-free class error
keeps the ordinary default-initialized Expected carrier: move-only does not
by itself imply a MIR cleanup obligation or owning-error lifetime slot.
An `Unexpected` owns real local storage after its
payload transfer; fallible return cleanup cannot publish it early or destroy
the caller's uninitialized output. Checked consuming payload extraction may
feed a subsequent Expected return through the existing local payload slot.
State-checked native observers use unchecked dereference only after GTI's
guard or verified state proof, avoiding the host `std::expected::value()`
requirement that an error type be copyable.

`expected<T&, E>` does not use native `std::expected`, which forbids reference
payloads. Its private C++20/C++23 carrier stores either `const T*` or `E`; GTI
source still observes a non-null read-only reference. MIR supplies every
pointer from an admitted loan. Direct successful returns and local
initializations dereference that loan when constructing the carrier, while a
forwarded complete carrier is published as a value so its active error state
is preserved. Checked `value()` extraction binds a child CallResult loan to the
stored pointer after the ordinary wrong-state edge succeeds. The backend does
not infer the owner from the pointer or extend its lifetime.

Expected placement also uses MIR's recursive generated-relocation proof for
aggregate owners containing guarded resource fields. Moving the aggregate
invokes each field move; each guarded leaf disarms its own source. This is
construction into empty storage, not a proof that replacement assignment or
user-defined moves cannot fail. Consuming observers can prepare constructor
arguments under the same exact slot, activation, handoff, and later-argument
failure-cleanup proof as ordinary function arguments. Cleanup may traverse
other owners' destructor Invokes without losing the prepared argument's
already-destroyed state. An Expected returned by a resolved member call can
likewise occupy the single value-rooted slot consumed by `?`; its receiver
schedule remains independently verified.

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
An admitted prefix-storage read may select its backend-private unchecked
helper only when its exact bounds outcome is discharged. A fully discharged
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
mutability from a C++ rvalue or reselect the native member overload.

For a verified read-only temporary argument input, the body emitter uses the
same exact value-rooted lifetime slot and passes that slot to the selected
reference parameter. Its `CallArgument` loan is representation-erased only
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
once. An owning `Expected` extraction may use its value-rooted source place as
representation and drop metadata only when it is the sole unprojected use in
that exact reparent chain, with matching type, access, drop, call-site,
ordering, and success/failure facts. That metadata place emits no C++ local or
default construction. Failure before extraction success never arms the
prepared slot, and unsupported, aliased, duplicated, projected, or mismatched
shapes remain rejected rather than receiving backend-invented storage or
cleanup.

A complete fresh owning Expected or class result can also transfer directly
from its success-activated value slot into one exact static source-GTI function
parameter; Expected additionally admits the proved constructor consumer.
An owning class constructor result uses the same proof, with exact publication
either on its checked success edge or its infallible instruction activation.
These consumers share the same slot-use and CFG proof:
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
binding carriers do not take this early-retirement route. A stable unprojected
local receiver may remain staged across an expected propagation branch only
when the branch is the exact `ExpectedHasValue` observation and its unique
success edge proves extraction of that same carrier.

A cleanup-free direct result may forward into caller storage without inventing
a result-drop obligation when it has one exact return use and only inert
boundary/EndBorrow records intervene. Passive expected payload construction
and ordinary source-call results similarly permit their completed input loans
and unrelated passive local lifetimes to end before return, including the
exact call's Invoke success edge. Prepared argument handoff uses the same exact
CallInput/TransferOut proof as constructor publication. The
payload is constructed in a local slot; only the unique Return wraps it in
the Expected envelope. No default constructor or result-drop obligation is
invented. These representation choices preserve the verified Invoke and
cleanup schedule.

An exact moved-Expected return likewise publishes directly into the caller's
result storage. This applies when its class payload is cleanup-free but lacks
a default constructor: the unused SSA identity must not declare a second,
default-constructed Expected. The same unique Move/Return and cleanup-tail
proof owns that erasure; neither the result type alone nor a public name does.

A checked `value()` or `error()` on a borrowed Expected may copy a cleanup-free,
generated-copy class alternative directly into its sole binding's lifetime
slot. The receiver remains borrowed and unchanged. This representation requires
the exact success destination and its immediate, unconditional Initialize;
borrow-carrying payloads and user-defined copy constructors are not folded into
this route. The wrong-state check precedes construction, so failure never
activates the destination. Consuming observers retain their separate verified
movement and cleanup rules.

A one-use moved Expected with a cleanup-free source binding may fuse its
receiver move into either a direct class extraction or a trivial scalar/enum
observer. Both use the same proof: exact source-binding/drop/type identity,
no move lifecycle effects, no active cleanup, and exactly one receiver use.
The scalar path retains the checked wrong-state failure edge and reads that
binding directly instead of default-constructing a second Expected. Owning
receivers requiring cleanup continue to require their explicit lifetime slot.
Their existing value-rooted moved-Expected slot admits either its one state
test or a sole checked scalar observer, preserving the same source-to-value
drop transfer and full-expression destruction. Trivial scalar-only Expected
SSA receivers can be read directly; neither path introduces a loan or bypasses
the checked inactive-alternative failure.

The cleanup-free class extraction route also admits an exact prepared argument
slot, not only a named binding. It reuses the class-result staging proof for
the sole call/constructor consumer, parameter identity, successful Invoke edge,
slot activation, dominance, failure cleanup, and ownership transfer. The
checked extraction constructs that slot before later arguments execute; a
wrong-state failure leaves it inactive. Source MIR still owns consumption of
the original Expected. No native default construction, public-name exception,
or delayed extraction at the final C++ call is introduced.

A temporary expected class result observed only through `has_value()` uses
the same proved lifetime slot as propagation, without requiring payload
extraction. Short-circuit evaluation may instead give it a conditional
temporary place. That route requires exact producer/result-drop/source-value
identity, initially unavailable storage, success-edge activation, one state
test, and the verified conditional full-expression drop. It does not admit
arbitrary temporary storage or infer lifetime from the native Expected type.

A moved nominal callable receiver uses a different contract from a by-value
argument. Its exact MIR Move initializes a Value-rooted lifetime slot, the
receiver CallInput aliases that slot, and invocation retains its obligation.
The same slot remains active through later siblings until the MIR normal or
failure cleanup. The backend requires one executable stage use and one exact
lifetime root rather than interpreting the lifetime root as another invocation.
Consuming method field aliases use the shared `receiverAllowsMutation` rule,
so moving a `mut` field does not accidentally become a native const-copy.
Public wrapper identity plays no part in either representation decision.
The same proof admits an owned lambda Move with one direct invocation receiver
use and one exact lifetime root. Its slot uses the existing concrete closure
representation, whether produced locally or received as a concrete generic
parameter. The Move constructs the slot; both ordinary and failure-ABI calls
alias it, and the MIR environment drop controls capture destruction. No
host-language temporary is allowed to shorten that lifetime to the invocation.
Pack-expanded invocations retain the same slot through an ordered receiver
stage. A cleanup-free moved local environment instead aliases its already
materialized immutable storage, but only after proving the exact single-use
Move/optional value-stage/invocation chain. Its available-to-moved event remains
in MIR; this representation erasure does not permit source reuse.

Nested callable invocations consume only their own prepared receiver and
argument stages, just as ordinary calls do. A receiver already prepared for
an enclosing call may remain pending through the nested invocation, subject
to the same instruction-window, unique success-path handoff, and block-end
consumption proofs. A local frozen lambda inside a generic function may use
the existing fused-closure representation without a native type row, but only
when its exact initialization and complete use chain satisfy that proof.
Neither change reconstructs a call or its evaluation order from source syntax.

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

A cleanup-free class result with one exact value-rooted field projection and
one scalar `Load` uses the same uninitialized lifetime-slot representation.
An admitted GTI call or ordinary `Construct` publishes once into the slot, and
the load spells through its verified field chain. The constructor form also
requires exact target, owner, result, parameter, and operand metadata; one
single-field projection; a trivial scalar copy load; and no active cleanup,
lifecycle, borrowed-state, or drop obligation. Additional consumers, nested or
non-field projections, cleanup-owning temporaries, and incoherent type rows
reject body emission rather than falling back to native temporary behavior.

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

An unused cleanup-free nominal result also receives uninitialized output
storage for a transformed call. Selection requires no SSA uses, value-rooted
places, semantic result-drop obligation, borrowed state, or active cleanup;
the class must have generated destruction. On success the emitter marks and
immediately retires that representation, permitting repeated calls in a loop.
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
cleanup. Fused non-escaping closures can still use native lambdas.
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
instance fields or destructor has a selected HIR constructor instance when
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
adapter or native exception supplies the failure path. Derived instance fields,
source destructors, generic owners/bases, constructor default arguments and
generated-base chains remain outside this generated-body slice and retain
their existing structural construction representation.

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
call/return is admitted only through an exact verified-MIR transport shape.
Named and fresh arguments are moved from their one lifetime slot into the
native by-value parameter, parameter entry moves into its lexical slot, and a
transformed function, method, or exact lambda placement-constructs its result
in the caller's value-rooted slot. The paired success edge arms that slot;
failure leaves it empty and retains MIR's cleanup order. Representation-only
moved-from shells are destroyed immediately after their verified transfer.
Constructor arguments use the same proof, including fresh unit variants.

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
the C++ representation therefore reserves a private `std::monostate`
alternative. It is neither a source variant nor observable tag state. Field
publication retires only that proved empty shell and placement-move-constructs
the enum from its exact value slot. Its generated transfer must be failure-free;
neither publication nor rollback reconstruction requires enum assignment. An
enum with a construction-only class payload therefore remains nonassignable
while supporting ordinary owning-field construction. The joint class/enum
definition scheduler emits every payload class before its enum
and that enum before a class embedding it, rejecting a recursive by-value
cycle. Concrete generic owners/callables use the same exact lifecycle proofs;
generic arguments are not a reason to bypass or reject those proofs. The
declaration-bound inline optional capability contributes its payload as a
native layout dependency, while pointer-backed storage does not. Exact static
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
Ordinary native constructor overloads that publish fields from the same MIR
body use the same proved private empty state as their failure-form overloads.
They must not implicitly call a source default constructor before MIR field
publication; that constructor may be deleted or have observable behavior.
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
class with generated copy, move, and destruction, scalar/enum fields, no
state-bearing base, and no active cleanup. Every prepared input has a distinct
obligation and exactly one matching constructor handoff; scalar sibling inputs
retain their argument positions. A conservatively failure-capable target is
eligible only when its complete verified body is accounted for by the exact
passive parameter-to-field moves, lexical parameter drops, and inert boundaries.
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
MIR independently proves when a moved parameter's shell retirement cannot
invoke a source destructor; the backend does not infer this from C++ moves.

When earlier initializer work can fail, a later owned parameter move stays in
the verified constructor body instead of being pulled into native initializer
order. The same body-staged publication proof accepts cleanup-free generated
class moves with passive scalar/enum fields: the exact parameter Move, field
Initialize, and full-expression boundary remain mandatory, while value and
construction-rollback obligations must be absent. Its inert parameter shell
drop remains recorded. Custom lifecycle, borrowed state, and active cleanup
do not use this passive route.

An exact by-value parameter copy may also publish one field through the native
member-initializer list when the copied class has generated copy/destruction,
only passive scalar or enum state, no state-bearing base, and no active cleanup.
This MIR shape carries one reachable parameter `Load`, one matching initializer
metadata row, an exact field `Initialize`, and one full-expression boundary;
the copied parameter's native scope destruction represents its trivial MIR
drop. The scalar copy route, including passive raw-pointer value copies,
requires the same explicit `Load`/`Initialize` pair. Copying a raw pointer
introduces no dereference, loan, or ownership authority. Custom lifecycle,
extra uses, forged ownership, ambiguous fields, and mixed initializer schedules
fail closed.

A shared-reference carrier with generated, observation-free destruction may
retain its bound native shell during constructor rollback. This is not an
empty-state capability: its references must already be bound by the exact
first-field parameter-copy prefix (Load, Initialize, dependency publication,
source-loan retirement, full-expression boundary), or an independently proven
native constructed-field publication. The shell proof recursively excludes
exclusive references, active cleanup, user destructors, capability classes,
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
support observation-free retirement. Multiple fallible native prefixes,
mutable-reference inputs and nonempty unclaimed siblings remain excluded;
this bounded route alone is not complete allocator/container validation.

The same exact first-field schedule also admits a passive shared-reference
child with generated, observation-free destruction. This separate branch
requires no child value drop, success activation, field rollback obligation or
reparenting; it still checks the selected constructor, original Invoke failure
identity, adjacent field/dependency publication and both source-loan endpoints.
A cleanup-bearing or exclusive-reference child cannot use that passive branch.
The bound native shell is not an empty-state witness, and later field/body work
is not hoisted. This supports ordinary nested view construction and copied
field returns without inventing a second cleanup schedule.

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
cleanup authority. This permits an `expected` record extraction to reach a
field without requiring the record to have a default constructor.
The same resolved mutable-field predicate supports an adjacent parameter
`Load`/discarded `Assign` copy when the record has generated copy construction,
assignment and destruction with only passive scalar/enum/pointer fields. Its
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

A type-only concrete instance with no selected constructor and no generated
default-construction route has no executable declaration-default consumer.
Its catalog defaults are suppressed as well, so an unused generic sibling
cannot impose conflicting native defaults on the same retained template.
Every selected constructor still requires the full field-order, exact-type,
and once-only coverage proof; absence of a constructor is not permission to
omit a default from an executable construction.

Generated/defaulted construction still uses the separate declaration schedule.
Fallible ordinary class-value calls on that route remain `GTI-B0003`: generated
construction has no concrete constructor body to carry propagation to callers.
It is not silently redirected through a terminal wrapper. For this remaining
case, an assignable nullable owner can start null and be allocated in an explicit
initialization method after construction. This gap remains tracked under
M-FAIL/M-BACK in the implementation queue.

Obligation-free class values in a move capture or an owning `Expected` payload
remain concrete MIR values rather than backend-recognized library types. The
C++ backend may materialize the exact closure environment, direct payload
return slot, or unprojected extraction place only after proving unique use,
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
the checked-array replacement extension below, non-field projections,
conditional lifecycle events, incomplete
borrow endings, or additional consumers fail closed.

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

A later terminal argument can prevent the enclosing invocation entirely.
The backend reuses MIR's full-expression/CFG proof for a zero-use prepared
input, then independently checks that every intervening failure edge destroys
the exact prepared slot once. The producer still constructs before later
argument evaluation; no callee handoff or `TransferOut` is invented. Both
arms of a conditional terminal argument must satisfy this rule. The impossible
normal continuation is not permission to omit failure cleanup.

The publication has four destination families, distinguished by which
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
Requiring a destination obligation for an element, owned field or loan referent
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
A passive error record immediately wrapped in `Unexpected` and returned uses
the same storage when construction has an Invoke edge. Only its unique success
continuation may wrap and return it; intervening executable work, additional
uses, borrowed state and active cleanup remain outside this passive proof.
The constructor executes before the wrapper, and failure leaves the caller's
Expected result uninitialized.

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
raw-pointer dereference. Read-only and mutable access still come from verified
MIR and source loan analysis.

Generated passive record copies may contain positive concrete
fixed arrays of passive scalar/enum fields, including C-ABI records. Exact
generated copy/destruction traits, field drop facts, and the absence of active
cleanup or state-bearing bases remain required.

A cleanup-free moved error returned through `unexpected` uses the existing
representation-only class SSA slot: movement occurs at its MIR instruction,
before the source shell ends, and the single wrapper/return consumes the slot.
The proof requires generated failure-free movement, no active cleanup or
borrowed state, one adjacent wrapper and only inert lifecycle/source-shell
cleanup before return. It does not delay reading a moved source until return
or invent a semantic destructor obligation. A consuming checked Expected
extraction feeding that same single return consumer reuses the cleanup-free
receiver proof and publishes into the slot only after the state check succeeds;
the original carrier shell remains live until its MIR-authored cleanup.

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

Private prefix-storage results use a default-empty native handle at a
status-returning call boundary. A successful MIR Return moves the exact handle
through the out-parameter; it is not copied or default-constructed as an element
value. If destination replacement later fails, the activated result's MIR Drop
drains the held storage before the empty native carrier leaves scope. Missing
representation rows or success activation fail admission. Owned class results
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

Unique-owner borrowing uses the failure ABI. At the intrinsic instruction the
backend tests the represented owner for emptiness and selects only the exact
code/detail carried by MIR. Its `Invoke` routes failure through cleanup; the
success path publishes the pointee pointer at the paired reference return.
The loan keeps that owner stable between the check and publication. This path
does not call the legacy terminal `owner_access` helper, and a missing failure
edge or plain-body representation is rejected rather than bypassing caller
cleanup. No public smart-pointer name is recognized.

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

A borrowed bare-local call input may remain representation-erased across a
conditional scalar argument. Its stage must dominate the unique consuming call,
and every normal path through the acyclic argument region must reach that call.
Scalar computations, loads, input stages, and scalar expression-temporary
initialization may intervene. The proof also covers a nested read-receiver or
free function call, or an exact constructor, with scalar inputs and a cleanup-free nominal result whose
sole consumer is an exact prepared argument slot for the outer call. Its
failure edge precedes publication; result preparation does not change the
borrowed local's address. Other user-call shapes, owning result transfers, raw
memory, loans, and arbitrary stores remain outside this proof. Thus the deferred local
address denotes the same object that was selected before argument evaluation;
the backend does not reorder an observable address computation across a branch.

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
the sealed MIR target. It preserves the existing failure/status contract,
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
receiver; C++ overload resolution is not authority. The backend boxes the
concrete task only at that boundary and passes an opaque context plus
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
Native expected/variant/array/closure/unique shells recursively reach guarded
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
