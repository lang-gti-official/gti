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

Optimizer tombstones are verified inert proof slots and emit no operation.
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
