# Raw Pointers And Lexical Unsafe

Status: current bounded language contract

GTI provides one-level raw pointers for native C interoperation and low-level
implementation work. One bounded C-return form may add a second level when a
`[[c_array(count)]]` attribute pairs it with an exact integer out parameter.
Their spelling follows C++, while operations that can violate memory safety
are permitted only inside a Rust-style lexical `unsafe { ... }` block.

Raw pointers are deliberately not owners or checked borrows. They are the
escape hatch beneath safe values, references, and RAII classes, not a
replacement for those abstractions.

## Type And Binding Model

The implemented raw-pointer types are `T*` and `const T*`:

```gti
mut int32_t value = 7;
int32_t* pointer = nullptr;
const int32_t* read_only = nullptr;
```

`const` qualifies the pointee. `mut` continues to qualify the binding, just as
it does for every other GTI value:

| Declaration | Binding | Pointee access |
| --- | --- | --- |
| `T* pointer = nullptr;` | cannot be reseated | writable |
| `mut T* pointer = nullptr;` | may be reseated | writable |
| `const T* pointer = nullptr;` | cannot be reseated | read-only |
| `mut const T* pointer = nullptr;` | may be reseated | read-only |

`const` is accepted in this position only as a raw-pointer pointee qualifier;
GTI has not introduced general C++-style `const` values or `T* const`
declarators. A raw-pointer variable or field must have an explicit initializer.
Use `nullptr` when no address is available yet.

The ordinary type surface has exactly one pointer level. Pointer-to-pointer
locals, parameters, fields, aliases, and unannotated returns, references to raw
pointers, anonymous/general function-pointer declarators, pointer-to-array
types, and implicit
array-to-pointer decay are rejected. The only written `T**` form is an
`extern "C"` return carrying valid `[[c_array(count)]]` metadata. A fixed array
may contain one-level pointer values, but the array itself does not become a
pointer.

A named native callback alias such as `using Callback = (int32_t) -> void;`
is a separate bounded C-interoperation type. It has pointer representation but
does not add another raw-pointer spelling, dereference operation, cast, or
general function-value model. See
[`native-c-interop.md`](native-c-interop.md#native-callback-types).

Raw pointers are nullable, trivially copyable and movable, and non-owning. A
raw-pointer value does not keep its pointee alive, arrange destruction, or
create a semantic loan. Copying a pointer copies only its address.

## Compiler guarantees and unsafe obligations

Ordinary compilation checks types, pointee constness, access permission,
lexical `unsafe`, checked ownership, checked loans, and reference escape rules.
An unsafe operation additionally asserts programmer-owned obligations:
validity, alignment, initialization, aliasing and the promised owner/lifetime
relationship. Inability to prove those obligations is not a language error.
Unsafe does not make undefined behavior valid or turn a raw address into an
owner. Mandatory HIR/MIR verification and defined-failure cleanup are unchanged.

Optional `--raw-pointer-audit=warn` analysis checks available provenance and
effects. It is **off by default**, and off bypasses origin traversal,
interprocedural queries and audit-only loan observations rather than merely
hiding their diagnostics. `strict` runs the same analysis and promotes its
findings to errors for CI. It is an audit policy, not a different runtime mode
or a soundness certificate; warning suppression cannot hide strict errors.
Project profiles may set `raw-pointer-audit` to `"off"`, `"warn"` or `"strict"`.

Findings distinguish observed contradictions (`GTI-S2101`), possible lifetime
or alias violations (`GTI-S2102`), and incomplete analysis (`GTI-S2103`). A
conservative mutable-call invalidation is a possible violation, not proof that
the call destroyed the object. Recursion or a resource limit is incomplete
analysis, not proof of invalid memory. No finding is a substitute for reviewing
the unsafe contract; absence of findings is not proof of correctness.
Path joins, weak writes, loops and call summaries retain uncertainty even when
only one known origin remains. An unknown alternative is not evidence that
the known alternative must be the actual address.

## Optional raw-pointer audit

When enabled, the compiler preserves known local address origins through
pointer bindings, raw-pointer fields, aggregate copies/movement, designated
construction, fixed-array elements, reseating, qualifications, arithmetic and
branch joins.
This metadata does **not** keep the object alive. Dereference, indexing and
member access report an origin whose scope has ended or whose owner is known
to be moved on a possible path. Replacing or reinitializing a
consumed object does not revive addresses into its previous lifetime. Access
also respects overlapping
checked loans using their ordinary last-use endpoints. Array-element addresses
conservatively retain the array origin because later raw indexing can select
another element. Unknown foreign addresses still require unsafe proof.

Loops retain possible origins carried to later iterations, including through
local pointer aliases. Reseating a pointer before accessing it replaces the
earlier origins; merely copying or comparing an expired address remains inert
and does not become an access error at a backedge.
The same loop transfer follows source setters through aliases that first name
a destination on a later iteration, and includes raw globals.

Ordinary calls without an exact raw-result summary conservatively retain the
possible origins of raw input values/fields and addressable reference or
receiver inputs. The same rule applies to raw fields in constructed or returned
aggregates. Mutable reference/receiver calls merge possible incoming origins
into their raw fields rather than claiming an unknown call cleared them.

Source function and constructor results also retain origins produced inside
the callee, independently of declaration order and for concrete generic
instances. Returning `&local` does not keep the local alive: copying/comparing
the returned pointer is inert, but later accessing it or asserting a checked
borrow produces a finding. Returning a global's address preserves its actual
identity for witness and loan checks. Returned aggregate fields retain their own origin
sets; cached getters of global raw pointers observe the current pointer facts,
not the first call's value. Default arguments and constructor field
initializers use the same rule.
Inherited raw fields and raw-bearing expected success/error or payload-enum
alternatives retain these facts too. Tagged alternatives are conservatively
joined, not proved separately; `.value()`, `.error()`, `.value_or()`, and enum
pattern extraction preserve the carried addresses rather than treating the
carrier as their owner. Raw-bearing expected mapper outputs currently retain
an unresolved-source fact; inert transport remains permitted.

Source setters also preserve known pointer facts written into globals and
checked/raw formal destinations, including constructor writes. Merely checking
an uncalled function, constructor, destructor, or lambda does not execute its
global writes. An exact unconditional ordinary-function setter/reset can replace
old facts; constructor, conditional, or ambiguous write effects conservatively
retain their union. Global writes inside caller branches and loops are also
weak, including short-circuit/conditional expressions; even a definite reseat
within such a region may conservatively retain older facts. An unconditional
exact reset after the region remains supported. Unsupported
recursive write summaries, lambda/deferred writes, and possible source-destructor
global writes remain unresolved at unsafe memory uses, rather than becoming
foreign unknown addresses. Destructor effects are conservative across temporary
cleanup and scope exits. Captured and stored mutable raw aliases retain their
known destinations: unsupported callable/cleanup effects mark those possible
destinations unresolved, not unrelated local storage. Precise
captured-alias/destructor write inference is not implemented.

This source-result analysis is deliberately bounded. Recursive or dynamically dispatched
calls, lambda/deferred-callable raw results, raw-bearing variadic source
results, constructor-relative self addresses, unresolved stored-reference raw
fields, and exhausted/failed analysis retain an unresolved-source fact rather
than becoming foreign unknown addresses. An unsafe memory use or checked-borrow
assertion reports incomplete analysis (`GTI-S2103`); inert copies and comparisons
remain valid. Concrete retained references resolve to their external storage first.
Ordinary nonvariadic concrete generic calls are supported. The implementation
limits are 32 active bodies and 2,048 distinct queries per independent
top-level demand. Nested outcomes are private to that demand; only completed
top-level outcomes may be reused across demands. Up to 2,048 root outcomes are
cached; cache capacity exhaustion disables reuse, not acceptance. Lightweight
failure evidence is interned separately. Definition order cannot warm an
otherwise over-budget chain into apparent completeness.
Abstract source methods are unresolved dispatch, not foreign bodyless calls.
Unknown receiver effects also follow stored mutable raw aliases through a
readonly receiver; readonly access to the carrier does not make its pointees
immutable.

Source helpers, constructors, and overloaded operators also preserve raw-access
requirements. A known expired pointer, or a pointer conflicting with a live
checked loan, remains visible to the audit when passed to a helper that reads
or writes through it. Diagnostics point to the caller invocation. Source
copy/equality-only helpers remain inert. Concrete lambdas may likewise prove
that their bodies perform no raw access; unknown foreign calls do not establish
that proof.
Identity-bound restoring transfer also permits inert address transport when
the concrete type is structurally relocatable and needs no active cleanup. Types
whose cleanup may run source code do not gain this access-free proof.

Virtual calls can prove inertness when a checked place identifies an exact
owning receiver, or when every target in the sealed nongeneric family has a
complete access-free source summary. Otherwise calls conservatively check
their possible raw inputs. Open generic/erased callable target families are
not assumed inert merely because one currently observed target compares an
address. Passing a known expired address through such an unresolved invocation
can therefore produce a conservative finding even if that particular
implementation only copies it; copying or comparing the address directly
remains permitted. This access
proof does not make dynamically returned raw addresses or write effects precise.

Mutable calls also conservatively invalidate existing raw addresses into their
class, array, or compiler-owned storage arguments/receiver. This preserves
known lifetime loss through ordinary `destroy`, `clear`, release and relocation
wrappers without recognizing library names. A source method returning a checked
borrow is not exempt merely for its return type: it can replace storage before
returning. Exact compiler-owned borrow-only operations preserve existing
addresses. A bounded source-effect proof also admits a nonvirtual GTI helper
with a validated explicit borrow origin whose entire body is a direct unsafe
`return *pointer_parameter;` (optional grouping/nested blocks), and whose value
parameters require no cleanup. No calls, writes, local objects or other body
operations are admitted by that proof; parameter destructors also prevent it.
Consequently
an innocuous aggregate mutator can produce a conservative audit finding;
scalar mutation does not end its object's lifetime and is unaffected. Stale
addresses may still be copied or compared without an audit finding. Access,
reborrowing and arithmetic inspect their optional facts. Loop-backedge
substitution retains these possible lifetime-ending events rather than reviving
a later iteration's stale address.
Consequently, a function that actually clears an address or returns unrelated
storage may be conservatively flagged when an input has contradictory known
facts. This is a bounded provenance analysis, not a complete interprocedural
alias proof or a claim that every invalid unsafe program is diagnosed. Storing
an address in an object's pointer field does not establish ownership of its
pointee. Broader memory-campaign work remains tracked under `S-ALLOC-04`.

## Safe Operations

The following operations do not inspect or derive memory and are valid outside
an unsafe block:

- form `&place` for an admitted addressable place;
- initialize, copy, move, pass, return, or assign a compatible raw pointer;
- initialize or assign a raw pointer from `nullptr`;
- compare compatible raw pointers with `==` or `!=`; and
- add pointee qualification by converting `T*` to `const T*`.

Pointee qualification cannot be removed implicitly. GTI provides no implicit
conversion between a typed pointer and `void*`, no integer-to-pointer or
pointer-to-integer conversion, and no raw-pointer truthiness. Compare with
`nullptr` explicitly.

Address formation preserves the actual access of its operand: an ordinary
read-only `int value` produces `const int*`, while `mut int value` permits
`int*`. This applies through checked references and fields as well. It creates
no long-lived loan and does not extend the operand's lifetime. Computing the
operand still performs its ordinary access and initialization checks. Checked
array indexing retains bounds checks and evaluates its index once. Raw
projections beneath `&`, such as `&pointer[index]`, `&pointer->field` and
`&*pointer`, still require `unsafe`; the outer operation suppresses none of
their obligations.

## Unsafe Operations

An unsafe block is an ordinary lexical block whose contents may perform the
following otherwise-rejected operations:

```gti
unsafe {
  int32_t* pointer = &value;
  *pointer = 9;
  int32_t observed = pointer[0];
}
```

The gated operations are:

- dereference reads and writes with `*pointer`;
- raw indexing with `pointer[index]`;
- raw member access with `pointer->member`;
- pointer arithmetic, difference, increment, decrement, and `+=`/`-=`;
- a call to an `extern "C"` function whose return type or any parameter type is
  a raw pointer; and
- a call to a GTI function declared `unsafe`.

Unsafe blocks may nest and otherwise follow ordinary block scope and cleanup
rules. Permission does not escape the closing brace. A nested lambda starts a
new safety context, so dangerous operations in its body require their own
`unsafe` block even when the lambda is declared inside one.

`unsafe` does not disable type checking, access checking, initialization
checking, ownership checking, or any unrelated diagnostic. It states only that
the programmer accepts the proof obligations of the gated operation.

The pointee must still provide the representation required by the operation.
In particular, `void*` and a pointer to a `[[c_opaque]]` handle remain
address-only even inside `unsafe`: neither can be dereferenced, indexed, used
for member access, or advanced. Opaque-handle pointers may still be copied,
assigned, compared, passed, and returned as described in
[`native-c-interop.md`](native-c-interop.md#opaque-native-handles).

Pointer arithmetic accepts integer offsets. Adding or subtracting an integer
produces the same pointer type; subtracting two identical non-`void` pointer
types produces `int64_t` (the canonical type behind `std::ptrdiff_t`). Ordering
comparisons are not part of this surface. Pointer difference is the native
unsafe pointer operation, not checked integer subtraction merely because its
result type is `int64_t`; its same-allocation and representability conditions
remain programmer proof obligations.

Address formation requires a supported addressable place. GTI does not form a
pointer to a temporary, function, fixed array as a whole, or another raw
pointer. Taking the address of a supported checked array element does not
itself require `unsafe`.

## Explicit Owner-Witness Borrowing

`<std/memory>` provides unsafe free functions:

```gti
unsafe T& assume_ref<T, Owner>(const T* address, Owner& owner);
unsafe mut T& assume_mut<T, Owner>(T* address, mut Owner& owner);
```

Generic arguments may be inferred. The caller asserts that the address names
an aligned, initialized, live `T` whose lifetime is covered by the witness,
with the requested access. The result is an ordinary checked reference tied
to the witness. Its live loan prevents incompatible witness access, movement
and escape. A stored pointer does not independently prove that its enclosing
object owns the pointee. The mutable operation cannot remove pointee constness.

These helpers are ordinary GTI definitions using a public function contract:

```gti
[[borrowed_from(owner)]]
unsafe T& assume_ref<T, Owner>(const T* address, Owner& owner) {
  unsafe { return *address; }
}
```

An application can define the same pattern without access to compiler-private
declarations. `[[borrowed_from(parameter)]]` resolves one checked-reference
parameter; `[[borrowed_from(this)]]` selects the non-static receiver. The
parameter must grant sufficient access for the returned reference. Unambiguous
existing return-origin inference remains available without an annotation.
Safe returns must prove the selected origin; the annotation alone cannot
relabel a local, unrelated parameter, or temporary. A direct `return *address`
inside lexical `unsafe` explicitly asserts the raw pointer's relationship to
that owner. Declaring the function `unsafe` does not authorize its body.
The initial raw-assertion syntax is direct dereference (optional grouping), not
arbitrary local reference construction or general lifetime parameters.

The contract is preserved through concrete generic analysis, HIR and MIR. A
safe reference contract can be used on a virtual/interface declaration.
Optional raw assertion auditing is incomplete across polymorphic bodies;
the declared checked reference contract is mandatory in every implementation.

The unsafe programmer must establish that the address belongs to the witness
and remains valid for the complete promised lifetime. Optional auditing reports
contradictory or unresolved evidence; it does not grant that validity.
Source-defined wrappers propagate audit observations through formal parameters,
including pointer aliases; checking
does not depend on the public helper's spelling or declaration order. Loop
backedge origins are checked as well as the first iteration's origin.
Raw inputs stored in aggregate fields and receivers conservatively retain
their possible origins. Checked reference fields resolve to their retained
external owner rather than the carrier shell. Writes through known raw aliases
update the original storage's raw-field facts; multiple possible destinations
and dynamic indices weakly retain both old and incoming origins.

These helpers assert a checked lifetime relationship; they do not allocate,
initialize or release memory. They retain the bounded interprocedural analysis
described above, rather than promising arbitrary source-call provenance
inference. Typed allocation and object-lifetime operations are provided
separately by `std::allocator` and `std::storage<T>`.

## `void*`

`void*` and `const void*` are opaque address carriers. They may be initialized,
copied, compared, passed, and returned under the same rules as other raw
pointers. They cannot be dereferenced, indexed, used for member access, or used
in pointer arithmetic.

GTI does not implicitly convert between `void*` and a typed pointer. Explicit
type-call conversion accepts a namespace- or class-scoped pointer alias:

```gti
using Erased = void*;
using ItemAddress = Item*;
Erased erased = Erased(address);
ItemAddress restored = ItemAddress(erased);
```

The source and destination must both be raw pointers. They must have the same
pointee type, or at least one must point to `void`. The conversion may add
pointee constness but never remove it. Integer/pointer conversions and direct
unrelated typed-pointer reinterpretation remain rejected. A dependent target
type call with a raw-pointer input is checked again in its concrete instance;
it does not establish a general unconstrained conversion capability.

`<std/memory>` supplies ordinary source helpers:

```gti
void* erase_address<T>(T* address);
const void* erase_address<T>(const T* address);
T* typed_address<T>(void* address);
const T* typed_address<T>(const void* address);
```

The erasure helper infers `T`; restoration spells `typed_address<Item>(erased)`.
These are safe inert address operations, not accesses. They establish no
initialization, alignment, lifetime, ownership or write permission and create
no checked loan. Existing origin records survive erasure and restoration,
including expired/replaced origins and known pointee-type contradictions.
Known contradictory restoration is an optional audit finding; the conversion
alone still grants no access permission. Actual dereference and checked
owner-witness borrowing retain their own obligations. Pointer conversions do not extend constant evaluation or
promise stable object layout. Example 113 demonstrates an unrelated user type.

## Programmer Proof Obligations

Before each unsafe operation, the programmer must establish every applicable
condition:

- the pointer is non-null when the operation accesses a pointee;
- the pointed-to object or array element is alive for the complete operation;
- the address has the required alignment and denotes the declared pointee type;
- a read observes initialized storage;
- a write targets writable storage and does not pass through `const T*`;
- indexing and arithmetic remain within one valid object or array allocation,
  allowing a one-past value only for arithmetic or comparison and never for
  access, and a pointer difference is representable as `std::ptrdiff_t` and
  GTI `int64_t`;
- aliases, native retention, and concurrent access satisfy the called API's
  contract and the data-race/happens-before rules in
  [Execution section 4.9](execution.md#49-concurrency-boundary); and
- ownership and destruction occur exactly once outside the raw pointer itself.

Violating one of these obligations, including manufacturing a data race through
the address, has undefined behaviour. The unsafe block
does not insert a dynamic null, bounds, lifetime, provenance, or alignment
check and does not make an invalid operation sound.

Safe functions and classes may contain reviewed unsafe blocks, but they must
meet these obligations for every input accepted by their safe public API. A
safe abstraction must not require its caller to know about an undocumented raw
pointer precondition.

### Explicit resource binding

The in-progress custom-resource slice in `<std/resource>` makes the trust
boundary explicit with `unsafe std::bind_resource(resource)`. The result is a
reusable checked shared borrow, not ownership of the provider. The unsafe
obligation applies to every later allocation through that binding or any copy:
successful nonempty regions must be writable, sufficiently large and aligned,
disjoint from all outstanding allocations, and free of preexisting object
lifetimes that adoption would overwrite. The provider must keep each region
and its descriptor valid until matching release, and release exactly that
region without accessing already-destroyed elements. Independent raw aliases,
resets or concurrent use must preserve these promises too.

The structural `memory_resource` concept checks callable signatures and unsafe
effects. It does not prove allocator correctness. A `raw_allocation` is passive
address/extent/alignment/identity metadata: copying it creates neither a loan
nor another right to release the region. Direct provider calls remain unsafe.

The compiler checks the ordinary provider borrow and cleanup/transfer schedule.
The private typed-storage adoption operation additionally checks its empty
destination, count arithmetic, reported extent, non-null address for nonempty
storage and numeric alignment. These checks cannot prove actual allocation
extent, address provenance, non-overlap, writability or synchronization. A
malformed provider is already outside the binding contract even when one
contradiction is detected dynamically; an error return is not a general
containment guarantee for arbitrary unsafe code.

The source lease is declared before typed storage so reverse field cleanup
destroys live elements before returning the region. This is ordinary checked
lifecycle composition, not recognition of a public standard-library name.
End-to-end resource-owner/container integration is still being completed; see
[custom resources](standard-library.md#custom-resource-protocol-in-progress)
for its current boundary and the example arena's bookkeeping/thread limits.

## Functions With A Caller Obligation

Use `unsafe` when a GTI function or named non-polymorphic method requires the
caller to establish a precondition that its types and implementation cannot enforce:

```gti
// Caller: pointer must address a live, initialized int32_t for this call.
unsafe int32_t read_raw(const int32_t* pointer) {
  unsafe { return *pointer; }
}
```

Every call to this function requires a lexical `unsafe` block, even when its
body is empty or its signature contains no pointer. Overload resolution first
selects the ordinary exact declaration; its unsafe requirement is then checked.
The declaration modifier does not change overload ranking, return types,
ownership, checked failure, or cleanup. It does not make the function body
unsafe: raw operations and calls to other unsafe functions still need explicit
blocks.
The implementation must document the actual obligation; the compiler checks
the caller's acknowledgement, not the truth of a lifetime or aliasing claim.

This surface admits namespace-scope GTI free functions and ordinary instance
or static methods, including `constexpr` and generic declarations and methods
of generic classes. Instance methods retain their ordinary read-only, mutable,
or consuming receiver rules. It excludes virtual/interface/override methods,
constructors, destructors, operators, runtime bindings, `extern "C"`
declarations, and hosted `main`. Unsafe callable types are not implemented:
an unsafe function cannot convert to a native callback or a safe function item,
including an algorithm argument or managed-thread task, even inside `unsafe`.
A safe wrapper is appropriate only when it establishes the precondition itself.

An exact structural member requirement acknowledges this effect explicitly:
`requires(T& value) { unsafe { value.inspect() } -> int; }`. The query is
unevaluated; its keyword grants no permission to a generic body. Calls through
that requirement still require a lexical unsafe block. A safe implementation
can satisfy it, but an unsafe implementation cannot satisfy a requirement
without the keyword. See [concepts](concepts.md#exact-member-signature-requirements)
and runnable example 117.

The canonical modifier order is `unsafe static constexpr` before the return
type, omitting modifiers that do not apply. Function contract attributes such
as `[[noreturn]]` precede these modifiers. The former `[[unsafe]]` spelling is
rejected with `GTI-P0001`; a contiguous attribute has a spelling-only migration
fix, including a separating space when the following type/modifier is adjacent.
It is not a compatibility alias. Attributes containing comments receive
the migration diagnostic without an edit that would delete those comments.

`GTI-S2100` (group `unused-unsafe`) warns when a checked, non-generic application
block discharges no unsafe operation. It points to the keyword and offers a
fix removing only that keyword, preserving braces, scope, and cleanup. An inner
unsafe block discharges its own operations; a lambda starts a new safety
context and cannot consume its enclosing block's permission. Symbolic generic
bodies, deferred compile-time branches, invalid programs, and compiler-trusted
implementation blocks are not given speculative removal advice. The warning
does not change program validity and can be disabled with `-Wno-unused-unsafe`
or a project's existing `disabled-warnings` profile setting.

`std::thrd_create` is the current C-style case that retains a raw context. When
creation succeeds, the unsafe caller keeps that context and everything
reachable through it live and appropriately synchronized until successful
join, not merely until `thrd_create` returns. The `unsafe` modifier does
not itself enable concurrent native callbacks, runtime attachment, or any
C-thread operation outside the exact registered `<std/cthread>` contract.

## Native C ABI

An `extern "C"` declaration may use a one-level raw pointer when its pointee is
one of:

- `void`;
- a fixed-width signed or unsigned integer;
- `float` or `double`;
- a valid passive `[[c_abi]]` record; or
- a nominal `[[c_opaque]]` handle.

The pointer may have a read-only pointee with `const`. Pointer parameters are
immutable bindings. Pointers to ordinary GTI classes, enums, arrays,
references, generic instances, owners, or `expected` values do not acquire a C
ABI. Transparent aliases are validated after resolution. A `[[c_abi]]` record
has checked public representation; a `[[c_opaque]]` handle deliberately has no
pointee representation and therefore retains the address-only restriction
above.

An annotated `[[c_array(count)]]` return may be one admitted pointer or the
exact two-level form whose inner pointer is admitted. The named count is an
immutable one-level pointer to a writable fixed-width integer. This metadata
does not make the native storage owned, non-null, bounded in the raw type, or
safe to retain; calls and element access stay lexical `unsafe`. See
[`native-c-interop.md`](native-c-interop.md#native-pointer-plus-count-arrays)
for the complete pair contract.

A bodyless `extern "C"` function may also take one exact
`OpaqueHandle**` parameter. This is an out-parameter boundary form, not a
general pointer-to-pointer type. Its call argument must be the direct
`&binding` expression for a mutable binding of exact type `OpaqueHandle*`, and
the call must be inside `unsafe`. The contextual address cannot be retained,
returned, forwarded, or used outside that full expression. The native
function may replace the pointer value; GTI infers no non-null, ownership,
lifetime, allocation, or cleanup guarantee from that write.
The foreign function must not retain the address of the output slot. The C++
backend stages internal address carriers into native pointer slots for this
call and copies updated values back before evaluating subsequent source code.
Repeated output arguments naming the same binding share one native slot;
native declarations and C-compatible fields retain their actual pointer ABI.

A declaration is not itself an unsafe operation. Calling a pointer-bearing C
function is unsafe because the declaration cannot express native bounds,
retention, nullability, ownership transfer, or aliasing requirements. A C call
whose source-level signature contains only the existing fixed-width scalars,
`float`, `double`, `void`, or the special non-retained `std::string_view` input
remains valid in safe code.

See [`native-c-interop.md`](native-c-interop.md) for symbol, ABI, and linking
rules.

## Safe RAII Wrappers

The intended pattern is a small unsafe implementation boundary beneath an
ordinary move-only RAII class:

```gti
[[c_opaque]] struct NativeWidget;

extern "C" {
  NativeWidget* widget_create();
  void widget_destroy(NativeWidget* widget);
}

class Widget {
  mut NativeWidget* handle = nullptr;

public:
  Widget() {
    unsafe {
      this.handle = widget_create();
    }
  }

  Widget(Widget& other) = delete;
  Widget(Widget&& other) = default;

  ~Widget() {
    if (this.handle != nullptr) {
      unsafe {
        widget_destroy(this.handle);
      }
    }
  }
};
```

The wrapper, not `NativeWidget*`, owns the native resource. `[[c_opaque]]`
preserves nominal C/C++ adapter identity without giving the pointee a GTI
layout or ownership rule. A production wrapper must also define creation
failure, prevent accidental duplicate destruction, preserve exactly-once
cleanup across movement, and keep the raw handle out of safe APIs unless
exposing it has a separately documented contract.

## Deliberate Omissions

This feature does not add:

- general pointer-to-pointer types beyond the bounded annotated C-array return
  and direct opaque-handle C out parameter;
- anonymous/general function pointers, captured callbacks, or foreign-thread
  GTI callback entry beyond the named exact native callback family;
- implicit fixed-array decay;
- implicit typed-pointer/`void*` conversion, integer/pointer casts, or unrelated
  typed-pointer reinterpretation;
- general C arrays, unions, bit-fields, packing, or platform-layout imports;
- direct C++ class ABI calls or exception passage;
- source-level `new`, `delete`, placement construction, or manual object
  lifetime;
- ownership inference or automatic destruction for raw pointers; or
- a stable ABI for GTI-defined types.

Each omission requires its own semantic, lifetime, ABI, IR, diagnostic, and
tooling design rather than being inherited from the C++ backend.
