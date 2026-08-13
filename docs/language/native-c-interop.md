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
supports fixed-width values, passive `[[c_abi]]` records, and bounded one-level
raw pointers behind lexical `unsafe`. It does not expose native variables,
callbacks, variadic calls, C++ linkage, ownership transfer, or a stable binary
ABI for ordinary GTI-defined types.

The compiler can emit one native bridge header for this bounded surface. The
header is valid C17 and C++20/C++23: C sees deterministic C record names, while
C++ sees the exact GTI source namespaces and record identities under
`extern "C"` function linkage. This is deliberately a C ABI that C++ code can
implement and consume, not an `extern "C++"` ABI for classes, overloads,
exceptions, templates, or native C++ ownership.

## Declaration Contract

The language string must decode to exactly `"C"`. An `extern "C"` block may
appear at namespace scope, including inside a GTI namespace, and may contain
only bodyless free-function declarations ending in `;`. A declaration cannot
be a definition, method, operator, generic, overload, runtime binding, or use
`static`, `virtual`, `override`, a mutable receiver, or a writable return.

Each declaration binds the exact source identifier as its native C symbol.
GTI does not mangle that symbol or prefix it with the containing GTI namespace.
The namespace still controls source lookup, so a declaration inside
`namespace posix` is called as `posix::socket`, but its native symbol remains
exactly `socket`. C symbols are program-global: the same symbol cannot be
redeclared, overloaded, or introduced by a second namespace. A C-linkage
function also cannot share its GTI overload set with an ordinary GTI function.
The symbol `main` is reserved for the GTI entry point, and an external symbol
cannot reuse the name of root-namespace GTI storage. Invalid ABI declarations
use semantic diagnostic `GTI-S2054`.

The current ABI allowlist is based on the resolved type, so a transparent alias
follows the same rule as its canonical allowed type:

- returns: `void`, `int8_t`, `int16_t`, `int32_t`, `int64_t`, `uint8_t`,
  `uint16_t`, `uint32_t`, `uint64_t`, `float`, `double`, valid `[[c_abi]]`
  records, and one-level raw pointers whose pointee is `void`, one of those
  scalar types, or a valid C ABI record;
- parameters: the same fixed-width scalar types, passed immutably by value,
  valid C ABI records passed immutably by value, one-level raw pointers with
  immutable bindings and the same permitted pointees, plus
  `std::string_view` as the counted input-buffer case; and
- compatibility spellings `int`, `uint`, `int8` through `int64`, and `uint8`
  through `uint64` resolve to their documented fixed-width types and therefore
  follow the corresponding scalar rule.

A pointer pointee may be qualified with `const`. `T*` and `const T*` describe
native writable and read-only pointee access respectively; leading declaration
`mut` is not permitted on a C parameter and would only describe reseating the
local parameter binding, not the C ABI.

`bool` and `char` are intentionally not C ABI scalars in this contract because
their source meaning should not inherit platform C representation choices.
Enums, ordinary classes/structs/interfaces, `expected`, owners, references,
arrays, mutable parameters, packs, string-view returns, pointer-to-pointer
types, function pointers, and pointers to non-ABI pointees are also rejected.
The allowlist does not define array parameters, callbacks, opaque ownership
transfer, or C++ interoperation.

Every C ABI call is conservatively effectful. A successful declaration says
only how GTI calls the symbol; it does not make the native implementation safe,
portable, available on every target, or linked into the executable.

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

- a fixed-width signed or unsigned integer, `float`, or `double`;
- another valid `[[c_abi]]` record by value; or
- a one-level raw pointer to `void`, an admitted scalar, or a valid C ABI
  record, with optional pointee `const`.

Transparent aliases follow the resolved type. `bool`, `char`, enums, ordinary
nominal types, references, owners, borrowed values, `expected`, fixed arrays,
symbolic types, and cleanup-owning values are rejected. Fixed arrays remain a
later record-field family because their native representation must not depend
on the backend's `std::array` choice. Empty records and recursive by-value
records are rejected; a one-level pointer is the bounded linked/opaque edge.

Fields remain in source order. Starting at offset zero, semantics rounds each
field offset up to that field's ABI alignment, advances by its size, records
the maximum field alignment as the record ABI alignment, and rounds final size
up to that alignment. Every operation is checked in the compiler's unsigned
64-bit layout domain. `sizeof(NativePoint)` and `alignof(NativePoint)` consume
these frontend facts just like the existing scalar and fixed-array queries.
`GTI-S2064` owns invalid native-record declarations and fields; `GTI-S2063`
continues to own an unsupported standalone layout query.

A field cannot have a GTI initializer. A C ABI record is representation only:
default construction policy belongs in an ordinary safe GTI wrapper or in a
native factory function. A record can be received from native code, copied,
stored, inspected, passed back, or initialized from another record value while
its generated default constructor remains unavailable. Keeping initialization
policy out of the record also ensures that the compiler-generated C++ header
and generated program contain the same C++ type definition. `mut` controls
source write access to the field and has no representation effect.

The semantic model owns size, ABI alignment, and every field offset. HIR and
MIR retain that metadata. Generated C++ uses a passive struct and compile-time
standard-layout, trivially-copyable, `sizeof`, `alignof`, and `offsetof`
assertions to audit the native compiler. Native C++ layout never substitutes
for the frontend calculation.

C ABI records may cross `extern "C"` by value or one-level pointer. Generate
the checked declarations with:

```sh
gti binding.gti --emit-native-header -o binding.native.h
```

The header contains the selected program's non-private `[[c_abi]]` records,
their checked size/alignment/offset assertions, and its source `extern "C"`
prototypes. Root-namespace record names remain readable in C. A namespaced GTI
record receives a deterministic encoded C name, recorded beside its qualified
source name in a comment, because C has no namespace facility. The C++ branch
instead recreates the exact source namespace. Nested by-value records are
defined dependency-first and pointer edges use forward declarations.

The header is compiler output and should be regenerated when the GTI boundary
changes rather than edited. It does not import a foreign header, infer a C
declaration, or make arbitrary native types layout-stable.

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
shim. Only the declared C ABI types cross into GTI. A C++ exception must never
escape through an `extern "C"` function; the shim must catch it and translate
it to a result allowed by the declared boundary. General C++ ABI calls remain
outside the language contract.

## Pointer-Bearing Calls And Unsafe

A declaration containing an allowed raw pointer is not itself unsafe. This
includes a raw pointer nested inside a by-value `[[c_abi]]` record. Calling it
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

A C function whose source signature contains only the fixed-width scalar
allowlist, pointer-free C ABI records, `float`, `double`, `void`, or the special
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
Direct compiler mode forwards every argument after `--` to the selected native
C++ compiler, so a library can be linked explicitly:

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
| `cpp-sources` | Existing package-contained `.cpp`, `.cc`, or `.cxx` files compiled before linking |
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
source paths must use `.cpp`, `.cc`, or `.cxx`. Dependencies may widen this
boundary only through a future declared package root; native fields do not
grant arbitrary filesystem discovery.

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
checked; only the root package may declare them. Use `c-sources`,
`cpp-sources`, `include-dirs`, `library-dirs`, and `link-files` when GTI should
validate package-relative inputs. Reserved standard, optimization, output,
language-mode, response-file, and non-executable-mode options are rejected.
`raw-args` are placed after the build-owned output arguments. Dependencies do
not exist yet; future dependency manifests must not contribute trusted
argument fields without a separate trust policy.

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
agreement remains the programmer's responsibility.

`gti build` and `gti run` pass the effective inputs through the same
`ExecutableBuildRequest` as direct mode. `gti check` validates the selected
native configuration, including selected C and C++ source path existence, but
remains frontend-only and does not discover or invoke a native compiler. The
`gti metadata` schema version 7 reports every target kind, declared execution
profile, effective native vector, C standard, C source, and C++ source for each
target/profile plan without creating the build tree.
Arguments after `gti run --` remain the executed program's arguments.

Source-level native includes and linker flags are not GTI language syntax.
They belong to the toolchain so platform selection, ordering, diagnostics, and
cache keys remain explicit. Structured native files and directories contribute
their content (and path where native path spelling is observable) to the
whole-program project cache. Trusted exact argument strings also contribute to
the key, but GTI does not interpret embedded file paths inside those opaque
arguments. Use structured fields or `gti build --no-cache` when such an
argument depends on undeclared mutable external state.

## Runtime Relationship

The standard prelude now declares its output and file host calls through the
same bounded `extern "C"` syntax. Their native definitions live behind
`runtime/include/gti/runtime.h` and use `gti_c_string_view` for text inputs.
The ordinary GTI wrappers in `gti_internal::runtime` keep those implementation
symbols out of public library APIs. Only the trusted prelude and physical
standard-library units can resolve that namespace; application declarations,
references, and namespace aliases are rejected with `GTI-S2058`, and shared
language queries filter the same private identities from application tooling.

The current runtime entry symbols and C prototypes are exactly:

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
