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

This is deliberately a call-only C ABI, not a general foreign-definition or
native-layout facility. It supports bounded one-level raw pointers behind
lexical `unsafe`, but does not expose native variables, C structs, callbacks,
variadic calls, C++ linkage, or a stable binary ABI for GTI-defined types.

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
  `uint16_t`, `uint32_t`, `uint64_t`, `float`, and one-level raw pointers whose
  pointee is `void` or one of those scalar types;
- parameters: the same fixed-width scalar types, passed immutably by value,
  one-level raw pointers with immutable bindings and the same permitted
  pointees, plus `std::string_view` as the counted input-buffer case; and
- compatibility spellings `int`, `uint`, `int8` through `int64`, and `uint8`
  through `uint64` resolve to their documented fixed-width types and therefore
  follow the corresponding scalar rule.

A pointer pointee may be qualified with `const`. `T*` and `const T*` describe
native writable and read-only pointee access respectively; leading declaration
`mut` is not permitted on a C parameter and would only describe reseating the
local parameter binding, not the C ABI.

`bool` and `char` are intentionally not C ABI scalars in this contract because
their source meaning should not inherit platform C representation choices.
Enums, classes, structs, interfaces, `expected`, owners, references, arrays,
mutable parameters, packs, string-view returns, pointer-to-pointer types,
function pointers, and pointers to non-ABI pointees are also rejected. The
allowlist does not define native records, array parameters, callbacks, or
ownership transfer.

Every C ABI call is conservatively effectful. A successful declaration says
only how GTI calls the symbol; it does not make the native implementation safe,
portable, available on every target, or linked into the executable.

## Pointer-Bearing Calls And Unsafe

A declaration containing an allowed raw pointer is not itself unsafe. Calling
it requires a lexical unsafe block because GTI cannot infer the function's
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
allowlist, `float`, `void`, or the special non-retained `std::string_view`
parameter remains callable from safe code. The private pointer inside the
`gti_c_string_view` lowering does not turn the source-level call into a raw
pointer operation.

Raw C pointers own nothing and create no GTI semantic loan. A source-defined
wrapper must represent ownership, lifetime, and exactly-once cleanup through
ordinary GTI values and RAII. See [`raw-pointers.md`](raw-pointers.md) for the
complete type, unsafe-operation, proof-obligation, and wrapper contract.

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
| `library-dirs` | Existing native library search directories |
| `link-files` | Existing regular object, archive, or other exact native link inputs |
| `libraries` | Names emitted through the native toolchain's library option |
| `frameworks` | macOS framework names; invalid for another selected OS |
| `compile-args` | Ordered compiler arguments before the generated C++ input |
| `link-args` | Ordered linker arguments after structured link operands |
| `raw-args` | Trusted exact arguments appended after the driver-owned output |

`[[...native.platforms]]` entries require at least one non-empty `os`, `vendor`,
or `arch` selector. Every supplied selector must exactly match the resolved
`TargetInfo`; unmatched fragments contribute nothing and their host-specific
paths need not exist. Duplicate selectors in one native table are rejected.
Frameworks are valid only when the effective target OS is `macos`.

Structured paths are relative to `gti.toml`, canonicalized, and confined to the
package even through symbolic links. Selected include/library paths must be
existing directories; selected C sources and link files must be existing
regular files. C source paths must use the exact `.c` extension. Dependencies
may widen this boundary only through a future declared package root; native
fields do not grant arbitrary filesystem discovery.

Lists concatenate rather than replacing lower scopes. C sources, search paths,
and link operands resolve from target to profile to package so specific inputs
precede broader dependencies. Within one base or platform fragment, operand
categories have the fixed order `link-files`, then `libraries`, then
`frameworks`; array order and duplicates are preserved, but TOML field order
does not interleave categories. C, C++, linker, and raw arguments resolve from
package to profile to target so specific flags occur later. A matching platform
fragment precedes its base for sources, search paths, and operands, and follows
its base for arguments. `c-standard` is accepted only on a base native table;
the most-specific target, profile, then package declaration wins, with `c17` as
the default.

`c-compile-args`, `compile-args`, `link-args`, and `raw-args` are trusted exact
argv elements. GTI does not shell-split, interpolate, execute, or interpret
embedded paths in these fields, so their paths are not package-containment
checked; only the root package may declare them. Use `c-sources`,
`include-dirs`, `library-dirs`, and `link-files` when GTI should validate
package-relative inputs. Reserved standard, optimization, output,
language-mode, response-file, and non-executable-mode options are rejected.
`raw-args` are placed after the build-owned output arguments. Dependencies do
not exist yet; future dependency manifests must not contribute trusted argument
fields without a separate trust policy.

For every selected C source, `gti build` invokes the resolved C compiler with
the C standard, target profile optimization, GTI's runtime include directory,
shared manifest include directories, and effective C-only arguments. This lets
C definitions include `<gti/c_abi.h>` without redeclaring the toolchain path.
Compiler discovery uses `--cc`, `GTI_CC`, `CC`, then `cc`. The staged object
atomically replaces its prior intermediate and is placed before the runtime and
declared libraries in the final C++ link. GTI does not parse C or verify that a
C definition matches the GTI prototype; that cross-language ABI agreement
remains the programmer's responsibility.

`gti build` and `gti run` pass the effective inputs through the same
`ExecutableBuildRequest` as direct mode. `gti check` validates the selected
native configuration, including selected C source path existence, but remains
frontend-only and does not discover or invoke a native compiler. `gti metadata`
schema version 3 reports every effective native vector, C standard, and C source
for each target/profile plan without creating the build tree.
Arguments after `gti run --` remain the executed program's arguments.

Source-level native includes and linker flags are not GTI language syntax.
They belong to the toolchain so platform selection, ordering, diagnostics, and
eventual cache keys remain explicit.

## Runtime Relationship

The standard prelude now declares its output and file host calls through the
same bounded `extern "C"` syntax. Their native definitions live behind
`runtime/include/gti/runtime.h` and use `gti_c_string_view` for text inputs.
The ordinary GTI wrappers in `gti_internal::runtime` keep those implementation
symbols out of public library APIs.

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
