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

This is deliberately a call-only C ABI, not a general unsafe foreign-function
interface. It does not expose native variables, pointers, C structs, callbacks,
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
  `uint16_t`, `uint32_t`, `uint64_t`, and `float`;
- parameters: the same fixed-width scalar types, passed immutably by value,
  plus `std::string_view` as the counted input-buffer case; and
- compatibility spellings `int`, `uint`, `int8` through `int64`, and `uint8`
  through `uint64` resolve to their documented fixed-width types and therefore
  follow the corresponding scalar rule.

`bool` and `char` are intentionally not C ABI scalars in this contract because
their source meaning should not inherit platform C representation choices.
Enums, classes, structs, interfaces, `expected`, owners, references, arrays,
mutable parameters, packs, and string-view returns are also rejected. GTI has
no ordinary raw-pointer type, so APIs requiring pointers or caller-visible
native records need a future reviewed interop layer or a narrow C adapter.

Every C ABI call is conservatively effectful. A successful declaration says
only how GTI calls the symbol; it does not make the native implementation safe,
portable, available on every target, or linked into the executable.

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

Project commands do not yet accept native include, library, framework, or
linker settings in `gti.toml`. `gti build` and `gti check` reject arguments
after `--`, while `gti run -- ...` reserves them for the executed program.
Structured, target-aware project native-link settings remain deferred to the
build-system proposal. Until that contract ships, use direct compiler mode or
arrange the required symbol through the native toolchain outside project mode.

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
should use the explicit linkage block and satisfy this allowlist.
