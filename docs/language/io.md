# Byte I/O And Bounded Formatting

Status: Current contract

GTI provides recoverable byte-oriented host I/O through ordinary GTI wrappers
in `<std/cstdio>` and a narrow, versioned C-linkage runtime boundary. The public
library does not expose a C pointer, native descriptor, caller-owned raw
buffer, or native C++ stream type. This layer is intended to support portable
library code such as parsers and serializers; buffering, text decoding, and
domain-specific records remain ordinary higher-level libraries.

## Surface

```gti
#include <std/cstdio>

expected<int32_t, std::io_errc> std::getchar();
expected<std::string, std::io_errc>
    std::read_stdin_some(std::size_t maximum);

expected<std::unique_ptr<std::FILE>, std::io_errc>
    std::fopen(std::string_view path, std::file_mode mode);
expected<std::unique_ptr<std::FILE>, std::io_errc>
    std::fopen(std::string_view path, std::string_view mode);

expected<int32_t, std::io_errc> std::fgetc(mut std::FILE& stream);
expected<std::string, std::io_errc>
    std::fread(mut std::FILE& stream, std::size_t maximum);
expected<std::size_t, std::io_errc>
    std::fwrite(std::string_view value, mut std::FILE& stream);
expected<void, std::io_errc>
    std::fseek(mut std::FILE& stream, int64_t offset,
               std::seek_origin origin);
expected<int64_t, std::io_errc> std::ftell(mut std::FILE& stream);
expected<void, std::io_errc> std::fclose(mut std::FILE& stream);

expected<std::size_t, std::io_errc>
    std::try_write_stdout(std::string_view value);
expected<std::size_t, std::io_errc>
    std::try_write_stderr(std::string_view value);
expected<void, std::io_errc> std::try_flush_stdout();
expected<void, std::io_errc> std::try_flush_stderr();
```

`std::FILE` provides the same operations as mutable members named `get`,
`read_some`, `write`, `seek`, `tell`, and `close`, plus read-only `is_open`.
It is a move-only RAII owner. `fopen` returns it through `std::unique_ptr`
because this matches the familiar pointer-shaped C++ API without exposing null
as an error channel or requiring manual deletion.

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
integer; it is `unexpected(std::io_errc::end_of_file)`. Chunk reads instead
return an empty successful string at EOF so a caller can use one ordinary loop
without treating normal completion as a failure.

The error categories are `end_of_file`, `open_failed`, `read_failed`,
`write_failed`, `seek_failed`, `flush_failed`, `close_failed`,
`invalid_stream`, and `unsupported_mode`.

## File and mode semantics

The typed `file_mode` overload is the stable policy surface. The string
overload accepts familiar C spellings and maps them exactly:

| Typed mode | String spellings | Behavior |
| --- | --- | --- |
| `read` | `"r"`, `"rb"` | Existing file, read only |
| `write` | `"w"`, `"wb"` | Create or truncate, write only |
| `append` | `"a"`, `"ab"` | Create if absent, append every write |
| `read_write` | `"r+"`, `"rb+"`, `"r+b"` | Existing file, read and write |
| `read_write_truncate` | `"w+"`, `"wb+"`, `"w+b"` | Create or truncate, read and write |
| `read_append` | `"a+"`, `"ab+"`, `"a+b"` | Create if absent, read; append every write |

Other strings return `unsupported_mode`. On the supported macOS and Linux
targets all modes are byte exact; the `b` spelling is accepted for source
familiarity and portability rather than changing newline or encoding policy.

A path is passed to the host operating system as its exact counted byte
sequence. Absolute paths remain absolute, while relative paths resolve against
the program process's current working directory. GTI performs no
canonicalization, `~` expansion, environment expansion, globbing, or native
header search. Empty paths and paths containing an embedded NUL fail to open.

`read_some(maximum)` and `read_stdin_some(maximum)` return a newly owned
`std::string`, preserve embedded zero bytes, and request at most 4096 bytes per
call. The result can be shorter than requested. `maximum == 0` is an empty
success and performs no host read. A medium-level reader accumulates chunks
until it receives an empty success; it does not need access to the internal
fixed buffer.

`write` and `fwrite` submit one complete counted byte sequence. A successful
result equals the input size; a short count is never reported as success. A
failure can still leave a prefix written because operating-system writes are
not transactional. On POSIX targets a closed pipe is translated to
`write_failed`; the runtime confines the generated `SIGPIPE` to the calling
thread rather than letting it terminate the process. Files are unbuffered at
this layer, so there is no file flush operation. `seek` uses an explicit
`seek_origin`; `tell` reports the current byte offset. Both return `seek_failed`
when the native object is not seekable or the requested offset is invalid.

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

Single-byte compatibility calls and bounded chunk calls cross the runtime.
`runtime.h` owns the versioned open-mode and seek-origin ordinals plus exact
`*_v1` functions for standard-stream chunks, file open/read/write/seek, and
standard-stream flush. The legacy single-byte and read-only entries remain for
existing generated programs. Ordinary `gti_internal::runtime` wrappers and
the trusted `<std/cstdio>` implementation translate status values into the
public `expected` API.

Paths and writes cross as immutable, counted, non-retained
`gti_c_string_view` records from `<gti/c_abi.h>`. The only mutable pointer is a
fixed 4096-byte buffer inside trusted standard-library source, scoped by
`unsafe`; it never enters the public API. The runtime owns any temporary
NUL-terminated native path it needs. Runtime declarations are unavailable to
application lookup, completion, hover, and definition; direct access is
`GTI-S2058`. The complete bounded ABI and lifetime rules are in
[`native-c-interop.md`](native-c-interop.md).

`try_write_stdout` and `try_write_stderr` preserve counted bytes and expose
host submission errors. Each individual standard-stream call is serialized so
its bytes do not interleave with another call. This is not a language memory
synchronization primitive. The explicit flush functions expose C stream flush
failure; ordinary file writes use unbuffered descriptors.

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

The `format_errc` result reports pattern and argument errors. The existing
formatting helpers and infallible `print` family still do not merge a host I/O
error into that distinct result type; callers needing direct submission status
can format to an owning string and pass its view to `try_write_stdout`.
Allocation failure while constructing an owning result follows the current
defined-failure allocation policy rather than becoming a `format_errc` value.

Buffered streams, filesystem traversal, encoding conversion, floating-point
formatting, and formatter customization remain future library layers. A future
buffer/stream object should build on this byte surface without turning the
runtime ABI into the public GTI ABI.
