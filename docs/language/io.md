# Byte I/O And Bounded Formatting

Status: Current contract

GTI provides its first recoverable host-I/O slice through ordinary GTI
wrappers in `<std/cstdio>` and bounded C-linkage runtime declarations. The
public library does not expose a C pointer, a native descriptor, or a native
C++ stream type.

## Surface

```gti
#include <std/cstdio>

expected<int32_t, std::io_errc> std::getchar();
expected<std::unique_ptr<std::FILE>, std::io_errc>
    std::fopen(std::string_view path, std::string_view mode);
expected<int32_t, std::io_errc> std::fgetc(mut std::FILE& stream);
expected<void, std::io_errc> std::fclose(mut std::FILE& stream);
```

`std::FILE` also provides `is_open()`, mutable `get()`, and mutable `close()`
members. It is a move-only RAII owner. `fopen` returns it through
`std::unique_ptr` because this matches the familiar pointer-shaped C++ API
without exposing null as an error channel or requiring manual deletion.

`stdlib/std/cstdio.gti` uses a constructor taking the compiler-private
`gti_internal::file_handle` to assemble the wrapper inside the trusted unit.
Although that constructor is source-visible to the implementation, its
signature contains a private type and is not published as an application
constructor candidate. Application source also cannot name or alias the
handle: direct access is `GTI-S2058`. The public contract therefore exposes no
descriptor-adoption path. The private handle explicitly opts out of transfer
and sharing despite its integer representation; `FILE`'s declared cleanup also
denies automatic cross-thread capabilities. Ordinary same-thread moves remain
available.

`getchar`, `fgetc`, and `FILE::get` return the next unsigned byte widened to
`int32_t`. This preserves all values from 0 through 255. EOF is not a magic
integer; it is `unexpected(std::io_errc::end_of_file)`.

The current error categories are `end_of_file`, `open_failed`, `read_failed`,
`close_failed`, `invalid_stream`, and `unsupported_mode`.

## File and mode semantics

The first slice is read-only. `fopen` accepts exactly `"r"` and `"rb"`; all
other mode strings return `unsupported_mode`. Both supported modes perform
byte-identical reads. The separate spellings retain familiar C++ source and
leave room for platforms or later text facilities to define an explicit text
policy.

A path is passed to the host operating system as its exact counted byte
sequence. Absolute paths remain absolute, while relative paths resolve against
the program process's current working directory. GTI performs no
canonicalization, `~` expansion, environment expansion, globbing, or native
header search. Empty paths and paths containing an embedded NUL fail to open.

## Ownership and close behavior

Explicit `close()` or `fclose()` invalidates the handle before returning. A
second close reports `invalid_stream`. If the native close fails, the handle
remains invalid because retrying an interrupted or otherwise failed close is
not portable. Destruction makes one best-effort close when the stream is still
open; use explicit close when the application must observe the result.

The checked value returned by `fopen` owns the unique pointer. A mutable
`expected` receiver provides mutable access through `value()`, so this is valid:

```gti
mut auto opened = std::fopen("input.bin", "rb");
if (!opened) {
  return 1;
}

auto byte = opened.value()->get();
if (!byte) {
  return 2;
}

auto closed = opened.value()->close();
if (!closed) {
  return 3;
}
```

These projections are consumed within their enclosing full expressions.
Returning or retaining a borrow reached through
`expected<owner, E>.value()` is a nested owner dependency and remains deferred;
unwrap an ordinary owner reference before crossing a helper boundary instead.

## Runtime boundary

Each read requests exactly one byte from the operating system. There is no
library or C stdio input buffer in this slice. The prelude declares the exact
native symbols `gti_rt_read_stdin_byte`, `gti_rt_open_file_read`,
`gti_rt_read_file_byte`, and `gti_rt_close_file` in an `extern "C"` block, then
ordinary `gti_internal::runtime` wrappers translate private integer status
codes into the public `expected` API in GTI source. Paths cross as the
immutable, counted, non-retained `gti_c_string_view` input from
`<gti/c_abi.h>`; the runtime owns any temporary NUL-terminated native path it
needs. Those runtime wrappers are visible to trusted library implementation
units but not to application lookup or language-server presentation. The
complete bounded ABI and lifetime rules are in
[`native-c-interop.md`](native-c-interop.md).

## Stdout and bounded formatting

The implicitly available `std::print` and `std::println` overloads write a
counted string view, one `char`, or one fixed-width integer. `<std/string>` adds
read-only overloads for an owning `std::string`. Integer output is canonical
base-10 text with a leading minus sign for negative values, no locale,
grouping, width, or alternate base, and complete coverage of signed minima and
unsigned maxima. `<std/string>` supplies the same conversion as an owning
`std::to_string(integer)` result.

`<std/format>` adds the first replacement-field layer:

```gti
namespace std {
  enum class format_errc {
    invalid_format,
    argument_count_mismatch,
  };

  expected<string, format_errc> format(string_view pattern);

  expected<string, format_errc>
  format<std::integral First, std::integral Args...>(
      string_view pattern, First first, Args... args);

  expected<void, format_errc> try_print(string_view pattern);

  expected<void, format_errc>
  try_print<std::integral First, std::integral Args...>(
      string_view pattern, First first, Args... args);

  expected<void, format_errc> try_println(string_view pattern);

  expected<void, format_errc>
  try_println<std::integral First, std::integral Args...>(
      string_view pattern, First first, Args... args);
}
```

`{}` consumes the next argument. `{{` and `}}` emit literal braces without
consuming one. A lone opening or closing brace returns
`invalid_format`; a valid pattern with too few or too many arguments returns
`argument_count_mismatch`. Replacement is strictly sequential. Indexes,
names, format specifiers, width, precision, bases other than decimal, locale,
floating-point values, owning-string arguments, and user formatter
customization are not part of this bounded surface.

The overload family separates the zero-argument case from the nonempty
`First, Args...` case; every supplied argument must independently satisfy
`std::integral`, so a call may mix fixed-width signed and unsigned types.
`std::format` returns a newly owned string. `try_print` and `try_println`
validate the complete grammar and exact argument count, then construct the
complete owning result before making any stdout write. Invalid input therefore
produces no partial output, and `try_println` submits its newline only after
successful formatting and after the formatted text has been submitted to
stdout.

These operations are ordinary GTI library code. The bounded language pack fold
only supplies source-order iteration over the integral argument pack; it does
not recognize a formatting function or interpret a pattern. Literal views use
their counted runtime write. Dynamic owning strings and individual `char`
values cross through the scalar `gti_rt_write_stdout_byte(uint8_t)` entry after
the explicit lossless `uint8_t(char)` code-unit extraction, preserving embedded
zero bytes.

The `format_errc` result reports pattern and argument errors. Recoverable host
stdout failure remains part of the wider hosted-service/failure work: the
current output wrappers discard the runtime status just as the existing
infallible `print` overloads do. Allocation failure while constructing an
owning result follows the current defined-failure allocation policy rather than
becoming a `format_errc` value.

Buffered streams, file writes, seeking, filesystem operations, encoding
conversion, standard error, floating-point formatting, and formatter
customization remain future library layers. A future buffer/stream object may
build on or supersede this byte-at-a-time surface without changing the runtime
ABI into the public GTI ABI.
