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

## Safe Operations

The following operations do not inspect or derive memory and are valid outside
an unsafe block:

- initialize, copy, move, pass, return, or assign a compatible raw pointer;
- initialize or assign a raw pointer from `nullptr`;
- compare compatible raw pointers with `==` or `!=`; and
- add pointee qualification by converting `T*` to `const T*`.

Pointee qualification cannot be removed implicitly. GTI provides no implicit
conversion between a typed pointer and `void*`, no integer-to-pointer or
pointer-to-integer conversion, and no raw-pointer truthiness. Compare with
`nullptr` explicitly.

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

- address formation with `&place`;
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
pointer. Taking the address of an array element is valid inside `unsafe`.

## `void*`

`void*` and `const void*` are opaque address carriers. They may be initialized,
copied, compared, passed, and returned under the same rules as other raw
pointers. They cannot be dereferenced, indexed, used for member access, or used
in pointer arithmetic.

GTI does not implicitly convert between `void*` and a typed pointer. This keeps
type erasure visible at a native wrapper boundary until an audited cast surface
is designed.

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

## Functions With A Caller Obligation

Use `unsafe` when a namespace-scope GTI function requires the caller to
establish a precondition that its types and implementation cannot enforce:

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

This bounded surface admits namespace-scope GTI free functions, including
`static`, `constexpr`, and generic declarations. It excludes methods,
constructors, destructors, operators, runtime bindings, `extern "C"`
declarations, and hosted `main`. Unsafe callable types are not implemented:
an unsafe function cannot convert to a native callback or a safe function item,
including an algorithm argument or managed-thread task, even inside `unsafe`.
A safe wrapper is appropriate only when it establishes the precondition itself.

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
- typed-pointer/`void*` conversions or raw casts;
- general C arrays, unions, bit-fields, packing, or platform-layout imports;
- direct C++ class ABI calls or exception passage;
- source-level `new`, `delete`, placement construction, or manual object
  lifetime;
- ownership inference or automatic destruction for raw pointers; or
- a stable ABI for GTI-defined types.

Each omission requires its own semantic, lifetime, ABI, IR, diagnostic, and
tooling design rather than being inherited from the C++ backend.
