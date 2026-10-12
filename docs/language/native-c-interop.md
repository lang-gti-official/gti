# Native C Interoperation

Status: current bounded language and toolchain contract

GTI can call an existing C ABI function through a C++-familiar linkage block:

```gti
extern "C" {
  int32_t socket(int32_t domain, int32_t type, int32_t protocol);
  int32_t close(int32_t descriptor);
}

int main() {
  int32_t descriptor = socket(2, 1, 0);
  if (descriptor >= 0) {
    [[discard]] close(descriptor);
  }
  return 0;
}
```

This is a bounded C ABI rather than a general foreign-definition facility. It
supports fixed-width and target-correct C scalar values, passive `[[c_abi]]` records, nominal pointer-only
`[[c_opaque]]` handles, recursively nested raw pointers, NUL-terminated
`c_string` values, and annotated native pointer-plus-count returns behind
lexical `unsafe`. It also supports named exact native callback types and
same-thread adapters for eligible GTI free functions, plus the exact registered
worker and once-initializer adapters owned by `<std/cthread>`. It does not
expose native variables, variadic calls, C++ linkage, annotated ownership
transfer, general foreign-thread GTI callback entry, or a stable binary ABI for
ordinary GTI-defined types.

The compiler can emit one native bridge header for this bounded surface. The
header is valid C17 and C++20/C++23: C sees deterministic C record names, while
the default C++ surface exposes familiar GTI source namespaces and record names
under `extern "C"` function linkage. Functions use their selected external
symbol, including when `link_name` makes that differ from the GTI call name. A
consumer may define
`GTI_NATIVE_HEADER_NO_SOURCE_NAMES` before including the header to suppress the
optional C++ record aliases and place C-function declarations only in GTI's
isolated namespace. Opaque handles retain their exact incomplete source
identity because a C++ consumer must be able to complete that type. This is
deliberately a C ABI that C++ code can implement and consume, not an
`extern "C++"` ABI for classes, overloads, exceptions, templates, or native
C++ ownership.

## Target-Correct C Scalar Types

The prelude supplies `c_bool`, `c_char`, `c_schar`, `c_uchar`, `c_short`,
`c_ushort`, `c_int`, `c_uint`, `c_long`, `c_ulong`, `c_longlong`,
`c_ulonglong`, `c_float`, `c_double`, `c_size_t`, `c_ptrdiff_t`,
`c_intptr_t`, `c_uintptr_t`, and the `c_void` alias for `void`.
These describe the selected target's native declaration, not the compiler host.
For example, `c_long` is 64-bit on accepted macOS/Linux targets and 32-bit on
Windows. `c_char` is C's distinct plain `char`: unsigned on Linux arm64 and
signed on the other accepted profiles. GTI's ordinary `char` remains a distinct
code unit and is not a C ABI scalar.

Equal width does not imply equal type: `c_long` and `c_longlong` remain distinct
even on LP64, including overloads, generic arguments and pointer pointees.
Genuine native typedef aliases do canonicalize. For example, `c_int` is the
same type as `int32_t`, and `c_size_t` canonicalizes to `c_ulong` on
macOS/Linux or `c_ulonglong` on Windows. `c_bool` aliases GTI `bool` and has
C `_Bool` / C++ `bool` representation. Numeric C families retain ordinary GTI
checked arithmetic and explicit narrowing conversions rather than C wrapping
or implicit narrowing. Mixed equal-width integer families choose the higher
native rank (`long long` over `long` over `int`) independently of operand order.

```gti
extern "C" {
  c_long labs(c_long value);
  c_size_t strlen(c_string text);
}
```

Declare a platform library function with the C families its own header uses.
A `size_t` parameter or result is `c_size_t`, not `uint64_t`: on macOS
`size_t` is `unsigned long` while `uint64_t` is `unsigned long long`, and the
generated C++ may already see the platform prototype, so a fixed-width spelling
can conflict with it during native compilation (coverage finding A37). The same
applies to `long`, `char*` buffers (`c_char*`), and other C families: equal
width is not enough. The C families make such a binding direct, without a
forwarding C shim; the
[libc region provider](../../examples/libc-region-provider/src/main.gti) binds
`malloc` this way.

The runnable [native scalar example](../../examples/123-native-c-scalars.gti)
uses real C library calls, unrelated generic aggregates and checked numeric
conversion. `gti bindgen` emits these families for the corresponding C
primitives; native header emission preserves exact primitive identity or a
genuine target-native fixed-width typedef alias. Long double and target-specific
extended scalar formats are not admitted by this vocabulary.

## Declaration Contract

The language string must decode to exactly `"C"`. An `extern "C"` block may
appear at namespace scope, including inside a GTI namespace, and may contain
only bodyless free-function declarations ending in `;`. A declaration cannot
be a definition, method, operator, generic, overload, runtime binding, or use
`static`, `virtual`, `override`, a mutable receiver, or a writable return.

By default, each declaration binds the exact source identifier as its native C
symbol. `[[link_name("symbol")]]` instead binds the decoded `symbol` while the
declared function name remains the GTI lookup name:

```gti
namespace cli11 {
[[c_opaque]] struct GtiCli11App;

extern "C" {
  [[link_name("gti_cli11_app_create")]]
  GtiCli11App* app_create(std::string_view description);
}
}
```

GTI code calls `cli11::app_create`, while the object file refers to
`gti_cli11_app_create`.
The C++ backend uses a separate private declaration identity bound to this
unchanged external symbol. Generated support headers therefore cannot capture
or conflict with a GTI C declaration merely because its symbol is also provided
by libc. The exact resolved signature and callback identity remain GTI facts;
this does not remap fixed-width integers to different C scalar types. Prefer
the target-correct `c_size_t` spelling when transcribing a `size_t` prototype.
Native bridge headers and embedding exports keep their selected external names.

The link name must be one portable ASCII C identifier;
the current contract does not admit arbitrary object-format spellings. GTI
does not mangle the selected symbol or prefix it with the containing GTI
namespace. `link_name` is valid only on a bodyless `extern "C"` function, may
occur at most once, and may be stacked with `[[c_array(count)]]`. C symbols are
program-global: the same selected symbol cannot be redeclared, overloaded, or
introduced by a second namespace. A C-linkage
function also cannot share its GTI overload set with an ordinary GTI function.
The symbol `main` is reserved for the GTI entry point, and an external symbol
cannot reuse the name of root-namespace GTI storage. Invalid ABI declarations
use semantic diagnostic `GTI-S2054`.

Native-facing names must also remain valid after the compiler places them in
the generated C17/C++ bridge header. For a function this validation applies to
the selected link name; without `link_name`, that is the source identifier.
Semantics rejects a C17 keyword where a spelling is emitted as an exact C name,
an implementation-reserved spelling such as `__name` or `_Name` on either
emitted language surface, and a leading-underscore root type or C symbol
because that spelling is reserved at C file scope. It also rejects actual
collisions with the
`<stddef.h>`, `<stdint.h>`, and `<gti/c_abi.h>` surface included by the header:
object-like macros where the name is emitted, function-like macros when an
external function name is followed by `(`, and root ordinary identifiers that
conflict with support typedefs, `gti_c_string_view`, or the compiler-reserved
`gti_cabi_` record-name prefix. The generated-header control macro
`GTI_NATIVE_HEADER_NO_SOURCE_NAMES` is reserved anywhere its expansion could
replace a native-facing identifier token, including a namespace component,
record or opaque-handle name, record field, C function, or parameter.

Every namespace component containing a public native record, opaque handle, or
C-linkage declaration is part of the default C++ source-name surface too.
Those components reject implementation-reserved spellings and object-like
support macros at any depth. A root component also rejects ordinary names
already supplied by the generated C++ environment, including the fixed-width
and size typedefs and `FILE`; for example, `namespace FILE` cannot contain an
opaque handle. `EOF` and the other C17 stdio object macros are rejected even in
a nested native namespace because macro expansion is not scope-aware.
Ordinary namespaces that contain no native surface remain unrestricted.

This validation follows the generated token context rather than banning every
support-header spelling everywhere. A field or parameter may shadow a typedef
such as `size_t`, and `offsetof` or `INT32_C` may be a type, field, or parameter
name because a function-like macro is not invoked without a following `(`.
Likewise, a lower-case leading underscore remains valid for a field, parameter,
or namespaced type that is not emitted at C file scope. `GTI-S2054` owns these
conflicts on C function declarations, `GTI-S2064` owns them on native records,
and `GTI-S2065` owns them on opaque handles. A link-name failure points at its
string literal; other failures point at the exact source identifier before
header generation.

The current ABI allowlist is based on the resolved type, so a transparent alias
follows the same rule as its canonical allowed type:

- returns: `void`, the C scalar families above, `bool`, `int8_t`, `int16_t`, `int32_t`, `int64_t`, `uint8_t`,
  `uint16_t`, `uint32_t`, `uint64_t`, `float`, `double`, valid `[[c_abi]]`
  records, `c_string`, exact named native function types, and recursive raw
  pointers whose terminal pointee is `void`, one
  of those scalar types, `c_string`, a valid C ABI record, or a
  `[[c_opaque]]` handle or an exact named native callback type.
  `[[c_array(count)]]` supplies count metadata independently of pointer depth;
- parameters: the same scalar types, passed immutably by value,
  valid C ABI records passed immutably by value, recursive raw pointers with
  immutable bindings and the same permitted pointees, plus
  `std::string_view` as the counted input-buffer case and `c_string` as the
  NUL-terminated input-pointer case and exact named native function types; and
- compatibility spellings `int`, `uint`, `int8` through `int64`, and `uint8`
  through `uint64` resolve to their documented fixed-width types and therefore
  follow the corresponding scalar rule.

A pointer pointee may be qualified with `const`. `T*` and `const T*` describe
native writable and read-only pointee access respectively; leading declaration
`mut` is not permitted on a C parameter and would only describe reseating the
local parameter binding, not the C ABI.

GTI `char` is not a C ABI scalar; use `c_char` for native plain `char`.
Enums, ordinary complete classes/structs/interfaces, `expected`, owners,
references, arrays, mutable parameters, packs, string-view returns,
anonymous or general function-pointer
declarators, and pointers to non-ABI pointees are also rejected. The bounded
pointer rules apply equally to `c_char**`, `OpaqueHandle**`, deeper pointers,
record fields, aliases and named native callback signatures. A mutable slot's
address can be captured with `auto` or an explicit pointer type, stored,
forwarded and returned. A C call may replace its value without conferring
ownership or cleanup policy. Retaining the slot address requires the unsafe
caller to preserve its lifetime; there is no call-local staging buffer.
`T* const*` makes the slot read-only; `const T**` instead makes the final
object read-only. These are distinct types. The allowlist does
not define array parameters, captured callbacks, opaque ownership transfer, or
direct C++ linkage.

Every C ABI call is conservatively effectful. A successful declaration says
only how GTI calls the symbol; it does not make the native implementation safe,
portable, available on every target, or linked into the executable.

## Importing Existing C Headers

`gti bindgen <header.h> [-o <out.gti>] [--namespace <name>]
[--target <triple>] [--clang <path>] [-I <directory>]... [-D
<macro[=value]>]... [-U <macro>]...` generates the GTI declarations for a C
header's bounded-ABI-compatible surface. Clang preprocesses the C17
translation unit, so includes, macros, conditionals, ordered header search
paths, and explicit definitions have their native meaning before declaration
import. The host target is used by default; an explicit supported 64-bit
target is forwarded to Clang and selects the C data model, including LP64
`long` on macOS/Linux and LLP64 `long` on Windows. Complete
passive records become `[[c_abi]]` structs, incomplete or inexpressible
records become `[[c_opaque]]` handles so pointer-based APIs stay usable,
enums become aliases to their native-compatible C integer family plus exact
`constexpr` constants, a bare anonymous enum contributes those constants alone, function-pointer
typedefs become named callback aliases, and prototypes fill one `extern "C"`
block. A configured function rename emits its GTI source spelling plus
`[[link_name("original_symbol")]]`; a portable assembler label recorded by
Clang uses the same representation. Record import covers the documented field
set, including callback fields and fixed arrays with multiple extents or record,
pointer, callback, and scalar elements; `restrict` qualifiers are read and
dropped because they do not change the caller's contract. Field positions the
record surface
rejects — for example pointers to whole arrays — degrade the record to an
opaque handle with the reason recorded, so generated source always satisfies
this contract's own validation. Everything
the bounded surface cannot express is reported with its
reason and listed in the generated file's skip report rather than silently
dropped. Native preprocessor errors retain Clang's source diagnostics. The
declaration reader is itself libclang (ADR 021), so a declaration that does
not parse as C is skipped with Clang's error message rather than imported
through error recovery. Reader/planner notes now preserve original-source
provenance through preprocessing line markers; internal spelling coordinates
still identify native-error overlap without laundering recovered declarations. The
generated source has no special status: this language contract remains the
sole ABI authority when it is compiled. The tool's architecture and growth
phases are recorded in
[the native-bindings-import plan](../plans/native-bindings-import.md).

### Composable typedef imports

The importer preserves every native pointer level. For
`typedef int (*Callback)(int)`, `Callback` is a callback value and
`Callback*` is a pointer to a slot containing that value, including in records,
results and nested pointers. `const Callback*` makes the slot read-only, not
the function. A bare function typedef such as `typedef int Operation(int)`
also produces a named callback signature: native `Operation*` and a C
function-typed parameter use that signature after the native pointer/parameter
adjustment. The generated declarations use existing GTI types, not new compiler
privileges. Hidden or system callback names get deterministic synthesized aliases.

Reusable `const char*` typedefs use `const c_char*` storage, so their record and
nested-pointer uses remain ABI-faithful. Direct call-boundary uses retain the
existing `c_string` convenience. Fixed-array aliases compose through other
array aliases and record fields; C array parameters decay to element pointers.
Zero-length array extensions, pointers to whole arrays, variadic callbacks and
prototype-less callbacks are reported rather than approximated.

See the runnable [callback-slot project](../../examples/native-callback-slots/README.md).
Native multidimensional array fields support checked element reads and writes;
each index keeps its bounds check and evaluation order, including failure before
evaluating a later index. This repairs the backend row-address representation
defect tracked in [#175](https://git.internal.howetotech.dev/gti-lang/gti/issues/175)
without adding whole-array pointer or reference support.

### Imported enum representation

The reader records Clang's compatible integer type, signedness, width, native
size/alignment agreement, and evaluated enumerator values. The planner admits
8-, 16-, 32- and 64-bit C integer representations whose layout matches the enum.
Signed minima and the full unsigned 64-bit range are preserved. The writer uses
ordinary aliases, explicit integer conversions and `constexpr` expressions;
this is not a new GTI enum kind or an unchecked conversion facility.

For example, on the LP64 import profile, `enum Flags { HIGH = 0x80000000U };`
becomes `using Flags = c_uint;` and
`constexpr Flags HIGH = Flags(2147483648);`. The same representation applies
through typedefs, native function/callback signatures, record fields, arrays,
and qualified nested pointers. An excluded or system-provenance enum's uses
resolve to the compatible integer without referencing an ungenerated alias.
An incompatible layout (such as independent over-alignment), unsupported
integer type, or invalid emitted name receives a skip reason; dependent uses
are rejected consistently and containing records can degrade to opaque handles.

Enum values and representation are C-target facts, not portable GTI promises.
The current declaration reader selects canonical LP64 Unix or LLP64
MSVC-compatible Clang profiles from the requested target's data model. In
particular, MSVC-compatible ordinary enum rules can evaluate wide constants
differently from Unix rules. Regenerate per target and match the native
library's ABI; enum-changing compiler flags such as `-fshort-enums` are not
currently forwarded as declaration-reader options. Fixed-underlying enum
extensions accepted by Clang retain their explicit integer family. This slice
does not expand the separate integer-macro evaluator's range.

See the runnable [native enum bindings project](../../examples/native-enum-bindings/README.md).

## Native Callback Types

A native callback type is named by a namespace-scope alias:

```gti
using ErrorCallback = (int32_t, c_string) -> void;
using Resolver = (void*, c_string) -> ErrorCallback;

extern "C" {
  ErrorCallback set_error_callback(ErrorCallback callback);
  Resolver set_resolver(Resolver resolver);
}
```

The anonymous `(parameters...) -> result` spelling is accepted only as the
direct target of that `using` declaration. Every later use names the alias.
Its parameters are immutable and its complete shape must recursively use the
bounded C ABI family described above. A callback alias may be stored in a
passive `[[c_abi]]` record or used as an `extern "C"` parameter or result. It
is nullable: initialization, assignment, and comparison with `nullptr` are
valid. A callback binding requires an explicit initializer.

This is a native C callable facility, distinct from GTI function items and
capturing closures. Native callback values cannot be arithmetically manipulated, cast, or formed
from lambdas, methods, generic functions, `unsafe` functions, declarations
without bodies, or inexact overloads. In an exact callback context, one non-generic
namespace-scope GTI function with a body may convert to the alias:

Direct adaptation of a bodyless `extern "C"` function item is not in that
conversion family and reports `GTI-S2076`. A C function may instead return a
value of the exact named native function-pointer type; GTI can transport and
invoke that address under the native invocation contract below. Private C++
declaration isolation does not change this eligibility boundary, the address
transported by C, or public bridge-header/export symbol names.

```gti
void report_error(int32_t code, c_string message) {}

unsafe {
  [[discard]] set_error_callback(report_error);
  [[discard]] set_error_callback(nullptr);
}
```

Semantics selects the function identity and exact signature. HIR and MIR carry
one stable adapter row, and the backend emits an `extern "C" noexcept` thunk.
The thunk has process lifetime, so a same-thread C library may retain its
address. It captures no GTI state and provides no implicit userdata lifetime.
If the GTI target reports a defined failure, the thunk completes the target's
verified cleanup and terminates through the runtime failure boundary; a native
exception is caught and terminates rather than crossing C.

General callbacks remain single-threaded. In the concurrent profile, GTI
adapters support the exact `(void*) -> int32_t` C-thread entry and `() -> void`
once initializer, with target
thread support and `<std/cthread>` included. The adapter requires once-only
attachment on its exact runtime-registered worker; ordinary initial-thread or
foreign-thread invocation of the worker adapter terminates before entering GTI.
The once adapter instead requires its exact synchronous invocation on an
existing initial, managed, or entered manual GTI execution thread. This is not general
foreign-thread attachment. See the [C-style thread contract](concurrency.md#c-style-manual-create-and-join)
for context lifetime, synchronization, hosted shutdown, and failure containment.
Concurrent code may still describe and transport other callback values received
from C without exposing a GTI target. `GTI-S2076` owns invalid callback type
shapes and conversions; `GTI-S2084` owns unavailable thread capability.

### Invoking Native Function Pointers

A named native function-pointer value can be called with ordinary call syntax
from a lexical `unsafe` block, including a local binding, a record field, or a
function result:

```gti
using Transform = (int32_t) -> int32_t;
extern "C" { Transform obtain_transform(); }

int32_t apply(Transform transform, int32_t input) {
  unsafe { return transform(input); }
}
```

The callee is evaluated once and copied before arguments are evaluated in
source order. Changing its source binding during argument evaluation therefore
does not change the function address being invoked. Parameters and results
must match the exact native signature; native pointers do not accept explicit
generic arguments. They can participate in ordinary concrete generic callable
contracts, without acquiring a GTI function-body identity.

Even a scalar-only call requires unsafe: the caller must ensure that the
address is non-null, executable, uses the declared C signature, and satisfies
the native implementation's preconditions. Null transport and comparison are
safe; calling null is not checked or given a defined GTI failure. Native calls
remain conservative foreign effects and have no GTI defined-failure return
channel. Calling a GTI-generated adapter retains its existing terminating
failure firewall and thread-entry restrictions. This does not admit capturing
callbacks or broader foreign-thread entry. See runnable example
[`122-native-function-invocation.gti`](../../examples/122-native-function-invocation.gti).

## NUL-Terminated C Strings

`<std/string>` supports explicit inbound copies. `unsafe string(c_string)`
copies through the first NUL into independent owned storage; null causes a
defined panic and allocation exhaustion follows ordinary string allocation
policy (an allocation-failure report and process abort). The unsafe static
factory `std::string::try_from_c_string(c_string)` instead returns
`expected<std::string, std::c_string_errc>`, with `null_pointer`, `size_overflow`
and `resource_exhausted` errors. It uses fallible allocation directly. Empty
input allocates nothing; nonempty input allocates one `length + 1` byte region.
No UTF-8 validation or implicit conversion is introduced.

Both entry points require a lexical `unsafe` block. The caller must establish
that the non-null input remains readable and stable through its first NUL for
the whole scan and copy. Dangling addresses, unreadable memory and missing
terminators are unsafe precondition violations, not recoverable error values.
Neither result retains the input address or its owner. Example
[151](../../examples/151-c-string-import.gti) demonstrates both APIs.

`unsafe const uint8_t* std::c_string_bytes(c_string)` in `<std/memory>` exposes
the same non-owning byte address, preserving null without scanning, allocating,
reading or extending storage lifetime. It establishes no checked reference or
owner witness. Subsequent pointer reads remain unsafe. This general bridge also
lets user-defined byte consumers operate without constructing a string.

`c_string` is a compiler-owned boundary type with the native representation
`const char*`. It is not `std::string`, does not own storage, and exposes no
derefencing or indexing operations. A value returned by C may be null; GTI can
compare it with `nullptr`, copy it as the same exact type, initialize it from
`nullptr`, and pass its address as `c_string*` inside the ordinary raw-pointer
`unsafe` boundary.

The only implicit production conversion in the current bounded slice is from
`std::string_view` to a `c_string` **call parameter**. Only a complete,
statically known literal-backed view qualifies; ordinary dynamic views and
subviews do not acquire an implicit termination guarantee. The literal backing
array includes a terminator, so MIR verifies the exact source/parameter pair and the backend
passes the literal's `.data()` pointer. Exact overloads remain preferred: a
literal selects a `std::string_view` overload over a `c_string` overload.
This conversion is not a general class conversion and does not permit a
`c_string` local to be initialized from a string view.

`std::string::c_string()` returns a non-null NUL-terminated view of the
string's characters. Allocated character storage maintains a
representation-private NUL byte at its logical `size()`; a zero-capacity empty
string uses an immutable process-lifetime NUL sentinel. The terminator is
maintained after reserve, append, resize, pop, clear, relocation, and capacity
changes. Embedded NUL bytes remain part of the logical string; ordinary C
string functions therefore observe only the prefix before the first embedded
NUL, matching the familiar C++ behavior.

`std::string::try_c_string()` provides the checked alternative. It returns
`nullptr` when any logical character is NUL and otherwise returns the same
terminated storage view. Both methods produce a compiler-tracked read-only
dependency on the source string. Mutation, movement, destruction, and escape
from a local owner are rejected while a derived pointer remains live; the loan
ends after the pointer's last source-level use. The checked form is
conservatively owner-dependent even on a path where its value is null.

Obtaining either pointer is not an `unsafe` operation: the pointer cannot be
indexed or dereferenced in safe GTI. The compiler also cannot infer whether an
arbitrary native function secretly retains a pointer after a call. A normal C
parameter is therefore treated as borrowed for the call; a binding for a
retaining API must express and uphold that library-specific lifetime contract.
No backend cast silently extends the source string's lifetime.

## Native Pointer-Plus-Count Arrays

`[[c_array(count)]]` describes the common C contract in which a function
returns borrowed array storage and writes its element count through an exact
out parameter:

```gti
[[c_opaque]] struct NativeHandle;
[[c_abi]] struct NativePoint { mut int32_t x; mut int32_t y; };

extern "C" {
  [[c_array(count)]] const NativePoint* native_points(uint32_t* count);
  [[c_array(count)]] NativeHandle** native_handles(int32_t* count);
  [[c_array(count)]] c_string* native_names(uint32_t* count);
}
```

The attribute is valid only on a bodyless `extern "C"` function. Its result
must be an admitted recursive C raw pointer. The named count must be an immutable
parameter binding of type `I*`, where `I` is a fixed-width integer and the
pointee is writable. Consequently the parameter is written as `int32_t* count`,
not `mut int32_t* count`: leading `mut` would make the local pointer binding
reseatable and is prohibited on C parameters.

The attribute is boundary metadata, not a GTI container type. It does not
infer ownership, storage duration, aliasing, nullability, or a cleanup
function. The native result may be null, especially when the count is zero,
and the library's own contract determines how long the elements remain valid.
An API whose returned storage must be freed belongs to a separate ownership
transfer family.

Calling the pointer-bearing function and indexing the returned storage require
`unsafe`. Results can use `auto` or their exact explicit pointer type.
The annotation does not restrict ordinary nested-pointer composition:

```gti
mut int32_t count = 0;
unsafe {
  auto handles = native_handles(&count);
  if (handles != nullptr && count > 0) {
    NativeHandle* first = handles[0];
  }
}
```

Semantics resolves the count name to its exact parameter and HIR/MIR retain
that parameter index. MIR verification rejects malformed pair metadata before
backend entry. The annotation is optional for ordinary recursive pointer
passage; a nested return does not require a count when its native contract is
not a counted array. The generated C/C++ bridge header erases the attribute
and emits the exact C declarator. `GTI-S2073` owns an invalid attribute/count
pair. The former `GTI-S2074` nested-pointer position restriction is retired;
pointer depth is no longer limited to these two boundary positions.

## Passive Native Records

A source record opts into the bounded C representation contract with
`[[c_abi]]`:

```gti
[[c_abi]]
struct NativePoint {
  mut float x;
  mut float y;
};

[[c_abi]]
struct NativeEvent {
  mut uint32_t kind;
  mut NativePoint position;
  mut void* userdata;
};

extern "C" {
  NativePoint point_transform(NativePoint value);
  void event_update(NativeEvent* event);
}
```

The opt-in applies only to a non-generic `struct` with at least one public
instance field. It cannot declare bases, access sections, static fields,
methods, constructors, a destructor, or copy/move policies. It also cannot be
combined with the transfer/share capability attributes. A native record is a
passive representation value; ordinary GTI classes remain free to evolve their
layout and should be used as the safe policy wrapper.

An admitted field is one of:

- a target-correct C scalar, fixed-width integer, `bool`, `float`, or `double`;
- another valid `[[c_abi]]` record by value; or
- a named exact native callback alias; or
- a positive concrete fixed array whose ultimate element is itself an
  admitted field type; or
- a recursive raw pointer ending in `void`, an admitted scalar, a valid C ABI
  record, a `[[c_opaque]]` handle, `c_string` or a named native callback,
  with independent qualification at each layer.

Transparent aliases follow the resolved type. GTI `char`, enums, ordinary
nominal types, references, owners, borrowed values, `expected`, symbolic or
zero-length arrays, symbolic types, and cleanup-owning values are rejected.
`c_string` is a function- and callback-boundary type, not a field type.
Native fixed-array fields emit direct C array declarators in both header
branches and generated C++, never `std::array`; their ordinary GTI indexing
remains bounds checked. Multiple concrete extents nest in declarator order,
and an array element takes its layout facts recursively, including by-value
record elements. Empty records and recursive by-value records — directly or
through an array element — are rejected; pointers provide linked/opaque edges.

Fields remain in source order. Starting at offset zero, semantics rounds each
field offset up to that field's ABI alignment, advances by its size, records
the maximum field alignment as the record ABI alignment, and rounds final size
up to that alignment. Every operation is checked in the compiler's unsigned
64-bit layout domain. `sizeof(NativePoint)` and `alignof(NativePoint)` consume
these frontend facts just like the existing scalar and fixed-array queries.
`GTI-S2064` owns invalid native-record declarations and non-array fields;
`GTI-S2069` owns a fixed-array field with a non-concrete or zero extent;
`GTI-S2070` owns an inadmissible array element type. `GTI-S2063` continues to
own an unsupported standalone layout query.

A field cannot have a GTI initializer. A C ABI record is representation only:
default construction policy belongs in an ordinary safe GTI wrapper or in a
native factory function. A record can be received from native code, copied,
stored, inspected, passed back, or initialized from another record value while
its generated default constructor remains unavailable. Copying includes direct
by-value return of a record parameter or local, and a record-returning native
call may be returned directly after its exact arguments are evaluated. Keeping
initialization policy out of the record also ensures that the
compiler-generated C++ header and generated program contain the same C++ type
definition. `mut` controls source write access to the field and has no
representation effect.

The semantic model owns size, ABI alignment, and every field offset. HIR and
MIR retain that metadata. Generated C++ uses a passive struct and compile-time
standard-layout, trivially-copyable, `sizeof`, `alignof`, and `offsetof`
assertions to audit the native compiler. Native C++ layout never substitutes
for the frontend calculation.

C ABI records may cross `extern "C"` by value or through recursive pointers. Generate
the checked declarations with:

```sh
gti binding.gti --emit-native-header -o binding.native.h
```

The header contains the selected program's non-private `[[c_abi]]` records,
their checked size/alignment/offset assertions, and its source `extern "C"`
prototypes. Root-namespace record names remain readable in C. A namespaced GTI
record receives a deterministic encoded C name, recorded beside its qualified
source name in a comment, because C has no namespace facility. The C++ branch
defines passive records in the compiler-owned `::__gti_program` namespace and
exposes familiar source-qualified record aliases by default. C-linkage
functions are declared directly in their source namespace rather than imported
from the private namespace with a C++ `using`-declaration; this gives GCC and
Clang the same definition scope and unqualified lookup behavior for an ordinary
native implementation. Defining `GTI_NATIVE_HEADER_NO_SOURCE_NAMES` suppresses
the optional record aliases and emits function declarations only in
`::__gti_program` when embedding the header beside an existing C++ source-name
surface. Opaque handles are different: their public source-qualified incomplete
struct is the actual identity that native C++ must be able to complete, so it
is never hidden by that opt-out. Nested by-value records are defined
dependency-first and pointer edges use forward declarations.

The header is compiler output and should be regenerated when the GTI boundary
changes rather than edited. It does not import a foreign header, infer a C
declaration, or make arbitrary native types layout-stable.

## Opaque Native Handles

An incomplete native identity uses the dedicated pointer-only declaration:

```gti
[[c_opaque]] struct NativeDatabase;

namespace graphics {
[[c_opaque]] struct Renderer;
}

extern "C" {
  NativeDatabase* database_open(std::string_view path);
  int32_t database_open_out(c_string path, NativeDatabase** out_database);
  void database_close(NativeDatabase* database);
  int32_t database_version(const NativeDatabase* database);
}
```

`[[c_opaque]]` applies only to a nongeneric, baseless, incomplete `struct`
declaration ending in `;`. Ordinary forward declarations are not a second
incomplete-type system: omitting the attribute is `GTI-S2065`. An opaque
handle has nominal type identity but no public fields, size, alignment,
construction, destruction, inheritance, or concurrency policy. It may appear
as the terminal pointee of a recursive raw pointer, including ordinary
`NativeDatabase**` slot values and native array results. Passing it by
value, defining a GTI body, using it as a base or generic argument, or querying
the pointee layout is invalid. `sizeof(NativeDatabase*)` and
`alignof(NativeDatabase*)` remain valid pointer queries.

An opaque-handle pointer is an address-only value. It may be initialized,
copied, assigned, compared with a compatible pointer or `nullptr`, passed, and
returned. The hidden pointee cannot be dereferenced, indexed, reached with
`->`, or used for pointer arithmetic, difference, increment, or decrement.
Those operations require a complete pointee representation, so `unsafe` does
not enable them; semantics rejects them with `GTI-S2065` and relates the use to
the `[[c_opaque]]` declaration.

A `[[c_abi]]` field may contain `NativeDatabase*` or
`const NativeDatabase*`. The record contains only the address; the opaque
pointee contributes no record-layout fact. An annotated `[[c_array(count)]]`
function may return `NativeDatabase**`. A bodyless C function may instead take
`NativeDatabase**` to replace one mutable handle-pointer binding. These are
distinct native contract uses, not restrictions on where a nested-pointer
value may be stored. General recursive pointer values remain ordinary inert
addresses; accessing a slot or pointee retains its unsafe/type obligations.

The generated bridge header gives each handle a deterministic dual surface:

```c
/* C17 branch */
typedef struct NativeDatabase NativeDatabase;
```

```cpp
// C++20/C++23 branch
struct NativeDatabase;
```

The C implementation may privately complete the C struct. The C++
implementation may privately complete the exact namespaced struct around
classes, templates, containers, and RAII state. Only pointers and the declared
`extern "C"` functions cross the boundary; this is C++ adapter compatibility,
not a promise to call the native C++ object ABI.

Raw handle pointers own nothing in GTI. A factory returning `NativeDatabase*`
does not automatically create an owner, a non-null guarantee, or a cleanup
obligation. A safe binding places the pointer behind an ordinary GTI class,
keeps its unsafe native operations private, rejects or represents a null
factory result, deletes copy, and invokes the matching destroy function from
its deterministic destructor. Until explicit native ownership-transfer
annotations exist, those rules are wrapper policy rather than inferred FFI
semantics.

## C++ Adapter Compatibility

A C++ source may include the generated header and implement an exported
function with ordinary C++ internals:

```cpp
#include "binding.native.h"

class NativeEngine {
public:
  NativePoint transform(NativePoint value) const;
};

extern "C" NativePoint native_transform(NativePoint value) {
  try {
    return NativeEngine{}.transform(value);
  } catch (...) {
    return NativePoint{}; // map failure to the declared C boundary policy
  }
}
```

The implementation may use classes, templates, overloads, and RAII behind the
shim. Only the declared C ABI types cross into GTI. An opaque handle may retain
the adapter's exact namespaced identity while hiding all of its
representation. A C++ exception must never escape through an `extern "C"`
function; the shim must catch it and translate it to a result allowed by the
declared boundary. General C++ ABI calls remain outside the language contract.

### Objective-C++ Apple API bridges

On macOS, the same boundary can be implemented in a `.mm` Objective-C++
translation unit. Declare it in the existing `cpp-sources` field and name its
frameworks explicitly:

```toml
[[package.native.platforms]]
os = "macos"
cpp-sources = ["native/apple_bridge.mm"]
frameworks = ["Foundation"]
```

The selected C++ compiler must support Objective-C++ (Apple Clang does).
GTI compiles the bridge separately with the selected C++ standard and then
links its object into the generated C++ executable. Other targets ignore this
platform fragment; selected `.mm` inputs on a non-macOS target are rejected.
Prebuilt bridge objects may instead use existing `link-files`. No Objective-C
syntax, class representation, message dispatch, or object ownership enters
GTI's type system, and `gti bindgen` does not import Objective-C APIs.

Keep `NSString`, `NSObject`, autorelease pools and retained handles behind
the bridge. Catch Objective-C exceptions with `@try`/`@catch` and any C++
exceptions with `try`/`catch`, translating failures to the declared C result;
neither may unwind through GTI. Do not return an `UTF8String` pointer after
its owner or autorelease pool has expired. Prefer copying bytes into bounded
caller-owned storage, or define an explicit owned opaque-handle/free contract.
GTI cannot verify a foreign implementation's retention or bounds promises.
Apple API thread-affinity requirements also remain the bridge/caller's
responsibility; compiling a bridge does not make a main-thread-only API safe
to call from arbitrary threads.

The runnable [Foundation bridge example](../../examples/objective-cpp-bridge/)
copies a deterministic Foundation string into a live GTI byte array, checks
the returned length, then constructs an owning GTI string. The bridge retains
no GTI address; its `@autoreleasepool` releases native temporary objects before
return. Run `gti run` from that example directory on macOS; it prints
`psych/foundation`. It works both with Clang's default manual Objective-C
ownership and an explicit application `compile-args = ["-fobjc-arc"]`.
That flag is shared with ordinary C++ compilation, where Clang accepts it
without enabling Objective-C parsing. Opaque flags deliberately bypass the
whole-program cache; dependency packages still cannot inject arbitrary flags.

## Pointer-Bearing Calls And Unsafe

A declaration containing an allowed raw pointer is not itself unsafe. This
includes an opaque-handle pointer and a raw pointer nested inside a by-value
`[[c_abi]]` record. Calling it
requires a lexical unsafe block because GTI cannot infer the function's
nullability, bounds, retention, aliasing, initialization, or ownership rules:

```gti
extern "C" {
  int64_t read_bytes(int32_t descriptor, uint8_t* output, uint64_t length);
}

int64_t read_one(int32_t descriptor, mut uint8_t& output) {
  unsafe {
    return read_bytes(descriptor, &output, uint64_t(1));
  }
}
```

The wrapper must prove both the general raw-pointer obligations and the native
function's documented contract. In the example, that includes proving that
`output` remains live and writable for at least one byte for the duration of
the call and that the callee does not retain the address.

A C function whose source signature contains only the C ABI scalar families
above, pointer-free C ABI records, `float`, `double`, `void`, or the special
non-retained `std::string_view` parameter remains callable from safe code. The
private pointer inside the `gti_c_string_view` lowering does not turn the
source-level call into a raw pointer operation.

Raw C pointers own nothing and create no GTI semantic loan, including when
they are copied inside a native record. A source-defined wrapper must represent
ownership, lifetime, and exactly-once cleanup through ordinary GTI values and
RAII. See [`raw-pointers.md`](raw-pointers.md) for the complete type,
unsafe-operation, proof-obligation, and wrapper contract.

## Counted Text Input

An immutable by-value `std::string_view` parameter lowers to the public C record
installed as `<gti/c_abi.h>`:

```c
typedef struct gti_c_string_view {
  const char *data;
  uint64_t length;
} gti_c_string_view;
```

The matching native declaration must use `gti_c_string_view` at that parameter
position. The record carries bytes and an explicit length: the data need not be
NUL-terminated and may contain zero bytes. Native code must not read beyond
`length`, write through `data`, or retain either the pointer or a derived view
after the call returns. GTI guarantees this conversion only as a non-retained
input argument; string-view returns and ownership transfer are not supported.

For example:

```gti
extern "C" {
  int32_t consume_text(std::string_view value);
}
```

has this C-facing prototype:

```c
#include <gti/c_abi.h>

int32_t consume_text(gti_c_string_view value);
```

The C header is the ABI source of truth. Do not reproduce the record with a
platform-dependent length type such as `size_t`.

## Linking And Targets

An `extern "C"` declaration does not locate a header or library. The GTI source
contains the prototype; the native link step must still provide the symbol.
Direct compiler mode forwards accepted arguments after `--` to the selected
native C++ compiler, so a library can be linked explicitly. Arguments that
would replace driver-owned output, executable mode, standard, optimization,
target, sysroot, or data layout are rejected. Raw compiler-driver/cc1 escapes
whose payload could replace those facts are rejected as a family:

```sh
gti main.gti -o main -- -lfoo
gti main.gti -o main -- -L/path/to/foo/lib -lfoo
```

`--emit-native-header` is an artifact-only direct mode, like `--emit-cpp`.
It cannot be combined with `--keep-cpp`, another emission mode, or trailing
native compiler arguments. If `-o` is omitted, the output is
`<source-stem>.native.h` beside the entry source.

System C library symbols may be available without an extra option, depending on
the target toolchain. Use target conditionals around platform-specific linkage
blocks and declarations. Every conditional branch must parse, while only the
selected branch is analyzed and emitted.

Project manifests provide the structured equivalent. Native inputs may appear
at package, profile, or executable-target scope:

```toml
manifest-version = 1

[package]
name = "native-demo"
version = "0.1.0"

[package.native]
include-dirs = ["native/include"]
c-sources = ["native/helper.c"]
cpp-sources = ["native/support.cpp"]
c-standard = "c17"
c-compile-args = ["-DHELPER_API=1"]
library-dirs = ["native/lib"]
link-files = ["native/lib/helper.o"]
libraries = ["helper_support"]
compile-args = ["-pthread"]
link-args = ["-pthread"]

[[package.native.platforms]]
os = "macos"
frameworks = ["CoreFoundation"]

[targets.native-demo]
kind = "executable"
root = "src/main.gti"

[targets.native-demo.native]
libraries = ["target_support"]

[profiles.release.native]
compile-args = ["-DNATIVE_RELEASE=1"]
```

The fields accepted by each `native` table and matching platform fragment are:

| Field | Meaning |
| --- | --- |
| `include-dirs` | Existing native header search directories |
| `c-sources` | Existing package-contained `.c` files compiled before linking |
| `c-standard` | C language mode: `c11`, `c17` (default), or `c23` |
| `c-compile-args` | Ordered arguments used only while compiling declared C sources |
| `cpp-sources` | Existing package-contained `.cpp`, `.cc`, `.cxx`, or macOS-only Objective-C++ `.mm` files compiled before linking |
| `library-dirs` | Existing native library search directories |
| `link-files` | Existing regular object, archive, or other exact native link inputs |
| `libraries` | Names emitted through the native toolchain's library option |
| `frameworks` | macOS framework names; invalid for another selected OS |
| `compile-args` | Ordered C++ arguments for declared C++ sources and the generated GTI C++ input |
| `link-args` | Ordered linker arguments after structured link operands |
| `raw-args` | Trusted exact arguments appended after the driver-owned output |

`[[...native.platforms]]` entries require at least one non-empty `os`, `vendor`,
or `arch` selector. Every supplied selector must exactly match the resolved
`TargetInfo`; unmatched fragments contribute nothing and their host-specific
paths need not exist. Duplicate selectors in one native table are rejected.
Frameworks are valid only when the effective target OS is `macos`.

Structured paths are relative to `gti.toml`, canonicalized, and confined to the
package even through symbolic links. Selected include/library paths must be
existing directories; selected C sources, C++ sources, and link files must be
existing regular files. C source paths must use the exact `.c` extension; C++
source paths must use `.cpp`, `.cc`, `.cxx`, or `.mm` (macOS only). A dependency
contributes paths confined to its own declared package root; native fields do
not grant arbitrary filesystem discovery.

Lists concatenate rather than replacing lower scopes. C sources, C++ sources,
search paths, and link operands resolve from target to profile to package so
specific inputs precede broader dependencies. Within one base or platform
fragment, operand categories have the fixed order `link-files`, then
`libraries`, then `frameworks`; array order and duplicates are preserved, but
TOML field order does not interleave categories. C, C++, linker, and raw
arguments resolve from package to profile to target so specific flags occur
later. A matching platform fragment precedes its base for sources, search
paths, and operands, and follows its base for arguments. `c-standard` is
accepted only on a base native table; the most-specific target, profile, then
package declaration wins, with `c17` as the default. Declared C++ sources use
the selected profile or CLI `cpp-standard`.

`c-compile-args`, `compile-args`, `link-args`, and `raw-args` are trusted exact
argv elements. GTI does not shell-split, interpolate, execute, or interpret
embedded paths in these fields, so their paths are not package-containment
checked; that trust extends only to the package being built directly, never to
its dependencies. Use `c-sources`,
`cpp-sources`, `include-dirs`, `library-dirs`, and `link-files` when GTI should
validate package-relative inputs. Reserved standard, optimization,
target/data-layout, sysroot, output, language-mode, response-file, and
non-executable-mode options are rejected. Raw compiler-driver/cc1 escapes and
unjoined forwarded-linker escapes are rejected because the following payload
cannot be classified independently. `raw-args` are placed after the
build-owned output arguments. A dependency package's package-scope native
inputs compose into dependent builds as isolated per-package groups:
structured contained inputs compose, only validated `-D<name>[=<value>]` and
`-U<name>` macro definitions compose from its compile-argument vectors, and
any other compiler argument or any linker or raw argument on a dependency is
rejected with `GTI-B1606`. Each group's include directories and macros apply
only to that package's own declared native sources, and its link operands
append after the dependent's in dependents-before-dependencies order.

For every selected C source, `gti build` invokes the resolved C compiler with
the C standard, target profile optimization, GTI's runtime include directory,
shared manifest include directories, and effective C-only arguments. This lets
C definitions include `<gti/c_abi.h>` without redeclaring the toolchain path.
Compiler discovery uses `--cc`, `GTI_CC`, `CC`, then `cc`. The staged object
atomically replaces its prior intermediate and is placed before the runtime and
declared libraries in the final C++ link.

For every selected C++ source, `gti build` invokes the resolved C++ compiler
with the project C++ standard and optimization, GTI's runtime include directory
(plus the C++20 compatibility include when required), shared manifest include
directories, and effective `compile-args`. Compiler discovery uses `--cxx`,
`GTI_CXX`, `CXX`, then `c++`; the same compiler performs the final link. C
objects are linked first, followed by C++ objects, the runtime, and declared
native operands. C++ source objects use the same atomic publication and failure
preservation contract as C objects. GTI does not parse either native language
or verify that a definition matches its GTI prototype; that cross-language ABI
agreement remains the programmer's responsibility. Declared native sources,
including Objective-C++ bridges, use the compiler's preprocessing output and
dependency report for whole-program cache identity. An unchanged eligible
build can reuse its executable; unavailable identities conservatively bypass
the cache. The exact cache boundaries are described in
[build and driver architecture](../architecture/build-and-driver.md#whole-program-project-cache).

`gti build` and `gti run` pass the effective inputs through the same
`ExecutableBuildRequest` as direct mode. `gti check` validates the selected
native configuration, including selected C and C++ source path existence, but
remains frontend-only and does not discover or invoke a native compiler. The
`gti metadata` schema version 12 reports every target kind, declared execution
profile, effective GTI configuration flags, native vector, C standard, C
source, and C++ source for each target/profile plan without creating the build
tree.
Arguments after `gti run --` remain the executed program's arguments.

Source-level native includes and linker flags are not GTI language syntax.
They belong to the toolchain so platform selection, ordering, diagnostics, and
cache policy remain explicit. Declared native sources, native include and
library search directories, content-complete exact link files, and `-l` names
that resolve to exactly one content-complete file inside a declared search
directory join whole-program cache identity through compiler dependency
discovery and full-tree content identity; a dependency package's composed
native group joins on the same terms. Opaque argument vectors, frameworks,
`-l` names that resolve outside the declared search directories or ambiguously
within one, non-content-complete link inputs, and dependency-injecting native
environment search paths bypass the cache because their complete
compiler/linker-discovered dependencies are not modeled. A forced header,
linker script, or thin archive can name transitive inputs outside the declared
tree, file, or environment value.

## Runtime Relationship

The process byte provider in `<std/resource>` is ordinary GTI over target-selected
native calls. Its remaining `<gti/memory.h>` adapters expose native `size_t`
limits, POSIX alignment/error constants and minimal allocation forwarding.
Fixed-width `uint64_t` is not declared as if it were native `size_t`; source
checks the limit before the adapter narrows. Native `c_size_t` and POSIX
`void**` are expressible directly. The uniquely named allocation/release
forwarders avoid monopolizing `posix_memalign` and `free` while compatible
repeated C declarations are unsupported; Windows reserved CRT symbol spellings
also require adapters. Applications can declare either ordinary C symbol alongside
`<std/resource>`. None of these functions confers
ownership, initialization or safety merely by name.

The runnable [`libc-region-provider`](../../examples/libc-region-provider/)
project demonstrates a separate application byte owner with direct
`malloc(c_size_t)`/`free` bindings, source-owned empty/exhaustion policy and move-only
exactly-once cleanup. It does not pretend that raw bytes are initialized GTI
objects. Run `gti run` in the project directory.

The standard prelude now declares its output and file host calls through the
same bounded `extern "C"` syntax. Their native definitions live behind
`runtime/include/gti/runtime.h` and use `gti_c_string_view` for text inputs.
The ordinary GTI wrappers in `gti_internal::runtime` keep those implementation
symbols out of public library APIs. Only the trusted prelude and physical
standard-library units can resolve that namespace; application declarations,
references, and namespace aliases are rejected with `GTI-S2058`, and shared
language queries filter the same private identities from application tooling.

The legacy scalar/file entries below remain supported examples of that
boundary, not an exhaustive runtime catalogue. Current buffered I/O, byte and
host-service entries are declared in the installed runtime headers:

```c
int32_t gti_rt_write_stdout(gti_c_string_view value);
int32_t gti_rt_read_stdin_byte(void);
int64_t gti_rt_open_file_read(gti_c_string_view path);
int32_t gti_rt_read_file_byte(int64_t descriptor);
int32_t gti_rt_close_file(int64_t descriptor);
```

The legacy compiler-owned `@runtime("...")` attribute remains accepted only
for its closed, validated binding set. It is not required for a user C symbol
and must not be expanded into a second general FFI. New native C declarations
should use the explicit linkage block and satisfy this allowlist. A new runtime
entry whose GTI declaration contains a raw pointer must be called inside an
unsafe block or hidden behind an ordinary safe wrapper that proves the same
obligations for every accepted input.
