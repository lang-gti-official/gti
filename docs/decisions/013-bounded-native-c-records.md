# 013: Add Passive Layout-Stable Records To The C Boundary

Status: Accepted and implemented

Scope revision (2026-10-11): later C-scalar, recursive-pointer and exact native
callback slices broaden the original field/passage allowlist without changing
passive layout or ownership. Foreign-header import is now implemented through
libclang; it emits ordinary declarations, not a second native type system.
Native ownership transfer remains a distinct boundary. The 2026-10-15
revision below admits passive C ABI unions through the existing union contract.

## Context

GTI's first `extern "C"` boundary deliberately supported calls before
aggregates. It admitted fixed-width scalar values, bounded one-level raw
pointers, and counted text inputs while rejecting every source-defined record.
That was sufficient for operating-system calls and small runtime services, but
not for the C libraries that form much of the systems ecosystem. Graphics,
audio, compression, databases, networking, and platform APIs routinely pass
small records by value or pointer.

GTI now has compiler-owned target scalar facts and source-level
`sizeof(type)`/`alignof(type)`. It can therefore compute a closed record layout
without asking generated C++ to define GTI semantics. The remaining design
question is how to expose that power without accidentally declaring every GTI
class ABI-stable or importing C's unchecked ownership and callback rules.

## Decision

`[[c_abi]] struct` is GTI's explicit passive native-record opt-in:

```gti
[[c_abi]]
struct NativePoint {
  mut float x;
  mut float y;
};

extern "C" {
  NativePoint point_transform(NativePoint value);
  void point_translate(NativePoint* value, float dx, float dy);
}
```

The opt-in is nominal and closed. A C ABI record:

- is a non-generic `struct` with at least one public instance field;
- has no bases, access sections, static fields, methods, constructors,
  destructors, or copy/move policy declarations;
- may contain target-correct C scalars, fixed-width integers, `bool`, `float`, `double`, a
  valid nested C ABI record, positive concrete fixed arrays of any admitted
  field type, exact named native callback aliases, or recursive raw pointers
  ending in `void`, admitted scalars, C ABI records, opaque handles, `c_string`
  or exact named native callbacks;
- may use aliases of an admitted field type;
- cannot give fields GTI initializers; construction policy belongs in an
  ordinary wrapper or native factory, while the ABI declaration remains a
  definition that C and C++ can share exactly; and
- may use `mut` on a field as a source access policy. Mutability does not alter
  representation.

GTI `char`, GTI enums, ordinary classes/structs/interfaces, references,
owners, borrowed-state carriers, expected values, compiler-private types,
symbolic or zero-length arrays, symbolic types, and cleanup-owning values are
excluded.

Fields remain in source order. For each field, semantics rounds the current
offset up to the field's ABI alignment, places the field there, and advances by
its size. Record ABI alignment is the maximum field ABI alignment. Final size
is rounded up to that record alignment. Every addition, padding step, and final
size is checked in the compiler's unsigned 64-bit layout domain. Empty and
recursive by-value records are rejected. A raw pointer is the
explicit way to represent an opaque or linked edge.

The semantic model owns the selected record size, alignment, field offsets,
and field types. HIR and MIR retain those facts; MIR verifies their structural
shape. `sizeof` and `alignof` consume the same facts as frontend constants.
The C++ backend emits a passive struct plus `sizeof`, `alignof`, `offsetof`,
standard-layout, and trivially-copyable assertions. Those assertions audit the
selected native toolchain; they do not become language authority.

Valid C ABI records may be passed to and returned from `extern "C"` functions
by value. Recursive raw pointers to them are also allowed. A pointer-free
record does not by itself make a C call unsafe. A raw pointer in the signature,
including one nested inside a by-value record, requires a lexical `unsafe`
call. Raw pointers still create no owner, retained borrow, null guarantee, or
cleanup obligation. This phase adds representation and passage, not ownership
transfer.

All currently supported targets use the accepted GTI scalar and pointer layout
facts. Synthetic target tests prove deterministic frontend selection across
arm64/x86_64 on macOS, Linux, and Windows. Native release platforms compile a
C oracle that checks `sizeof`, `_Alignof`, `offsetof`, by-value arguments and
returns, nested records, and pointer mutation. Cross-language declaration
agreement remains the binding author's responsibility for handwritten
boundaries; `gti bindgen` now imports supported foreign declarations through
libclang. The native-header backend generates
that agreement for GTI-authored declarations: one artifact contains a C17
branch with deterministic flattened names and a C++20/C++23 branch with exact
source namespaces, plus layout assertions and the external C prototypes. It
does not infer or import a foreign declaration.

### Follow-on: opaque handle identity

GTI 0.119.0 adds the independent `[[c_opaque]] struct Name;` declaration for a
nominal incomplete C handle. This does not widen ordinary record layout or make
an opaque pointee ABI-stable. Its original one-level pointer boundary has since
been generalized to recursive raw pointers, including pointer fields inside
an admitted `[[c_abi]]` record. The
generated bridge header emits an incomplete C typedef and an exact namespaced
C++ forward declaration, allowing either language to complete its private
implementation while the exported functions retain C linkage. GTI infers no
ownership or cleanup from such a pointer.

### Follow-on: fixed-array fields

GTI admits `T field[N]` when every extent is a positive concrete value and the
ultimate element type is otherwise valid in a C ABI record. Semantics computes
the array layout recursively from target-owned element facts. Native headers
and generated C++ emit direct C array declarators rather than `std::array`, so
the representation is identical in C17 and C++20/C++23. GTI element access
retains its ordinary checked-index contract; the backend's verified-MIR array
helpers accept either native C arrays or ordinary GTI fixed arrays without
changing failure behavior.

`GTI-S2069` owns an invalid extent and points at that extent. `GTI-S2070` owns
an inadmissible ultimate element type and points at the written type. Neither
diagnostic offers a fix-it because choosing a portable replacement is a design
decision.

## Consequences

- GTI can now express a substantial class of real C struct APIs without making
  ordinary GTI types ABI-stable.
- Safe wrappers should keep C ABI records as a narrow representation layer and
  expose ordinary GTI ownership and invariants above it.
- Native records are trivially copyable non-owning values. A pointer copied in
  a record is still only an address.
- Recursive pointer/out-parameter passage, exact callbacks and bounded
  foreign-header import are delivered separate slices. Annotated ownership
  transfer, retained userdata, flexible arrays, bit-fields,
  packing, varargs and direct C++ ABI remain separate capabilities.
- C++ libraries are supported through a generated-header C adapter: ordinary
  C++ classes and RAII stay behind `extern "C"`, and exceptions must be caught
  before they cross the boundary. GTI does not adopt native C++ ABI identity.
- `[[c_abi]]` cannot be combined with concurrency capability attributes. A
  wrapper that needs a nominal transfer/share policy should contain or convert
  the passive record rather than turn the ABI representation into the policy
  type.

## Alternatives

- **Make every plain `struct` C-compatible.** Rejected because ordinary GTI
  structs retain methods, lifecycle, ownership, inheritance, and evolving
  representation freedom.
- **Use `extern "C" struct`.** Rejected because C language linkage belongs to
  symbols, while a declaration attribute states the separate representation
  promise explicitly and composes with namespace placement.
- **Permit arbitrary standard-layout-looking records and trust C++.** Rejected
  because the backend must audit a semantic decision, not define it.
- **Support pointer passage only.** Rejected because small by-value records are
  common C APIs and the native C oracle can prove the bounded calling boundary.
- **Add callbacks and ownership transfer in the same phase.** Rejected because
  they require independent callable lifetime, failure containment,
  initialization, nullability, retention, and cleanup contracts.

## Revision: passive C ABI unions (2026-10-15)

The original struct-only boundary is superseded for passive named unions.
`[[c_abi]] union` uses the same admitted field set, ownership exclusions and
native passage as a C ABI struct, with all offsets zero and maximum member
size rounded to maximum alignment. This unlocks event payloads and native
userdata representations without an opaque wrapper around every union.

The general rule is structural and applies equally to unrelated user-written
unions and imported declarations. Existing union member operations retain
unsafe active-member obligations; complete-value copies preserve representation.
The compiler owns layout, HIR/MIR retain it and native C/C++ assertions audit
it. No hidden tag or allocation is introduced. Importer degradation remains
appropriate for bit-fields, packing, explicit alignment and anonymous promoted
fields until their corresponding place/layout semantics are implemented.
