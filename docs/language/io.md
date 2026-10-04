# Byte I/O And Bounded Formatting

Status: Current contract

GTI provides recoverable byte-oriented host I/O through ordinary GTI wrappers
in `<std/cstdio>` and a narrow, versioned C-linkage runtime boundary. The public
library does not expose a C pointer, native descriptor, caller-owned raw
buffer, or native C++ stream type. This layer is intended to support portable
library code such as parsers and serializers; buffering, text decoding, and
domain-specific records remain ordinary higher-level libraries.

## Surface

`<std/panic>` provides explicit terminal reporting, including formatted messages:

```gti
std::panic("configuration is missing");
std::panic("asset {} could not be loaded", asset_name);
```

The one-argument overload treats its message literally, including braces.
With formatting arguments, the source-defined wrapper calls the existing
recoverable formatter and panics with its successful text. Invalid syntax,
argument-count mismatch, and unsupported arguments use distinct stable literal
fallback messages; reporting a format error does not recursively format another
message. An otherwise unknown format error has a final literal fallback.

Raw message capture copies at most 512 bytes into the failure record before
cleanup. The formatted overload can allocate while constructing its message;
allocation failure there follows the normal allocation-failure contract.
Neither overload resumes or exposes a catch operation. Caller attribution passes
through the tracked wrapper rather than naming its internal formatting call.
See [execution semantics](execution.md#410-defined-runtime-failure) for cleanup,
message truncation and containment.

Formatting and stdout convenience APIs adopt this facility as described below.
General concept-failure diagnostics can explain declared constraints, but
literal-format validation and a public formattable protocol are not introduced
by panic. Unsupported format arguments still produce a runtime format error;
that limitation is not a claim that an unsupported static type is inherently
dynamic.

```gti
#include <std/cstdio>
#include <std/memory> // when spelling std::unique_ptr explicitly

expected<int32_t, std::io_errc> std::getchar();
expected<std::string, std::io_errc>
    std::read_stdin_some(std::size_t maximum);
expected<std::string, std::io_errc> std::getline();
expected<std::string, std::io_errc> std::getline(char delimiter);

expected<std::unique_ptr<std::FILE>, std::io_errc>
    std::fopen(std::string_view path, std::file_mode mode);
expected<std::unique_ptr<std::FILE>, std::io_errc>
    std::fopen(std::string_view path, std::string_view mode);

expected<int32_t, std::io_errc> std::fgetc(mut std::FILE& stream);
expected<std::string, std::io_errc>
    std::getline(mut std::FILE& stream);
expected<std::string, std::io_errc>
    std::getline(mut std::FILE& stream, char delimiter);
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

`getline()` reads standard input one byte at a time through newline and returns
an owning string without the newline. `getline(stream)` applies the same policy
to an open `FILE`; the overloads taking a `char` stop at that byte instead.
An encountered delimiter produces a successful result even when the line is
empty. EOF before any byte is `end_of_file`, while EOF after one or more bytes
returns the final unterminated line successfully. Bytes before the delimiter
are preserved exactly, including carriage returns and zero bytes. A read error
returns its `io_errc` and does not publish a partial line.

A terminal prompt without a newline must be flushed before waiting for input:

```gti
auto prompted = std::try_write_stdout("password: ");
if (!prompted) {
  return 1;
}
auto flushed = std::try_flush_stdout();
if (!flushed) {
  return 2;
}
auto password = std::getline();
if (!password) {
  return 3;
}
```

This is ordinary terminal input and does not disable echo. Hidden password
entry still requires a future terminal-control abstraction over the host APIs.

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

Ordinary projections in this example are consumed within their enclosing full
expressions. One bounded retained form is also available: a read-only range may
iterate `value()` from a named local or by-value
`expected<std::string, E>` while the complete `expected` remains unchanged:

```gti
mut auto chunk = std::fread(stream, std::size_t(4096));
if (!chunk) {
  return 4;
}
for (auto& byte : chunk.value()) {
  consume(byte);
}
```

The checked extraction happens before `begin()`. Its failure follows the
ordinary defined-failure path, and the successful loop holds a shared loan on
the complete `expected` until every loop exit has cleaned up. Moving,
replacing, mutating, or dropping `chunk` during the loop is rejected. Mutable
payload traversal, field or global receivers, retaining a standalone payload
reference, and returning an element remain unsupported; see
[`ranges.md`](ranges.md#named-expected-payload-ranges).

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

`<std/string>` also adds binary32 text through `std::to_string(float)` and the
matching `std::print`/`std::println` overloads. The output is an optional minus
sign, the integer part, a point, and exactly six fractional digits, with `nan`,
`inf`, and `-inf` for non-finite values and no locale, width, precision, or
exponent form. The digits are the stored value's exact decimal expansion,
rounded to nearest with ties to even at the sixth place — not a re-rounded
approximation — and signed zero retains its minus sign. The text therefore
matches C's default `printf("%f", (float)value)` result for every finite
binary32 under the default rounding mode, including magnitudes past the range
of a 64-bit integer. `<std/format>` is deliberately independent of that legacy
fixed-display conversion. It formats both binary32 and binary64 values through
the bounded, locale-independent character-conversion runtime described below.
Floating input for both widths is available through `<std/charconv>`.

`<std/format>` provides explicit convenience/recoverable pairs:

```gti
namespace std {
  enum class format_errc {
    invalid_format,
    argument_count_mismatch,
    unsupported_argument,
  };

  enum class print_errc {
    invalid_format,
    argument_count_mismatch,
    unsupported_argument,
    write_failed,
  };

  [[track_caller]] string format(string_view pattern);
  [[track_caller]]
  string format<First, Args...>(string_view pattern, First first, Args... args);

  expected<string, format_errc> try_format(string_view pattern);

  expected<string, format_errc>
  try_format<First, Args...>(string_view pattern, First first, Args... args);

  [[track_caller]] void format_to(mut string& output, string_view pattern);
  [[track_caller]]
  void format_to<First, Args...>(mut string& output, string_view pattern,
                                First first, Args... args);

  expected<void, format_errc>
  try_format_to(mut string& output, string_view pattern);

  expected<void, format_errc>
  try_format_to<First, Args...>(mut string& output, string_view pattern,
                               First first, Args... args);

  expected<void, print_errc> try_print(string_view text);

  expected<void, print_errc>
  try_print<First, Args...>(string_view pattern, First first, Args... args);

  expected<void, print_errc> try_println(string_view text);

  expected<void, print_errc>
  try_println<First, Args...>(string_view pattern, First first, Args... args);

  [[track_caller]]
  void print<First, Args...>(string_view pattern, First first, Args... args);
  [[track_caller]]
  void println<First, Args...>(string_view pattern, First first, Args... args);
}
```

The admitted arguments are `bool`, `char`, every fixed-width integer,
binary32 `float`, binary64 `double`, `std::string_view`, and `std::string`.
Booleans render as `true` or `false`; characters append their one code unit;
integers default to canonical base-10 text; and floating values default to the
shortest round-tripping decimal representation. String values preserve all
counted bytes, including embedded zero bytes.

```gti
mut std::string host = std::string("gitea.example");
std::string_view method = "api/v1/user";
mut auto endpoint =
    std::format("https://{}/{}", host.view(), method);
```

GTI's current named value packs are by value and reference-pack declarators
are not yet implemented. Consequently a named move-only `std::string` is not
copied implicitly: pass `text.view()` to borrow it, or `std::move(text)` to
transfer it deliberately. A fresh `std::string(...)` can be passed directly.

`{}` consumes the next argument. `{0}` selects an explicit zero-based argument
and may reuse or reorder it; automatic and explicit indexing cannot be mixed.
`{{` and `}}` emit literal braces without consuming one. The supported format
specifier grammar is `[[fill]align][sign][#][0][width][.precision][type]`.
Alignment is `<`, `>`, or `^`; integral presentations are `d`, `x`, `X`, `o`,
`b`, `B`, and `c`; floating presentations are `f`, `F`, `e`, `E`, `g`, and
`G`; and strings accept `s`. Precision truncates strings, controls floating
digits, and is bounded to keep formatting resource use finite. Floating
conversion is locale independent, preserves signs and non-finite spellings,
and applies sign-aware zero padding.

A lone opening or closing brace, mixed indexing, or an incompatible specifier
produces `invalid_format`; an out-of-range index or automatic field-count
mismatch produces `argument_count_mismatch`. A type outside the admitted set
produces `unsupported_argument`. Pattern grammar and field count are validated
first, so those errors take precedence. Named fields, dynamic width or
precision, locale-specific formatting, and user formatter customization are
not yet part of this bounded surface.

The overload family separates the zero-argument case from the nonempty
`First, Args...` case and permits heterogeneous admitted arguments in one
call. `std::format` returns a newly owned string; `try_format` returns that
string inside `expected`. `format_to` and `try_format_to` append only after
the whole replacement succeeds, leaving the destination unchanged on a format
error. The convenience wrappers call the recoverable implementation and
translate its error to a terminal panic; they do not duplicate formatting.

With at least one format argument, `print`, `println`, `try_print`, and
`try_println` construct the complete formatted result before writing anything.
Invalid input therefore produces no partial output. The formatted `println`
variants append the newline to the result and submit both in one serialized
runtime call. A host error can still leave a prefix written: output is not
transactional. `print_errc` preserves each format category and additionally
reports `write_failed`.

With **only one text argument**, all four print functions write literally,
including braces. This preserves the prelude's raw-output contract without
making semantics depend on whether `<std/format>` is included. In contrast,
`format(pattern)` and `try_format(pattern)` always interpret replacement syntax,
even with no arguments. Use `println("{}", text)` to request formatted output
explicitly. This changes the previous zero-argument `try_print` interpretation.

Convenience failures report `GTI-R0017` at the external caller, with stable
messages identifying invalid format, argument-count mismatch, unsupported
argument (including a conversion hint), or stdout write failure. Tracked user
wrappers can forward the caller further. Existing raw prelude and owning-string
output overloads now also panic on failed submission; they no longer discard
the runtime status. Cleanup and secondary panic follow the same defined-failure
contract as explicit `std::panic`.

These operations are ordinary GTI library code. A source-defined exact adapter
for each admitted type feeds the bounded, source-order pack fold; the compiler
does not recognize a formatting function, public string type, or pattern.
Formatted output uses the existing counted stdout runtime boundary. Existing
raw views use their counted runtime write; raw owning strings and individual
`char` values cross through the scalar
`gti_rt_write_stdout_byte(uint8_t)` entry after the explicit lossless
`uint8_t(char)` code-unit extraction, preserving embedded zero bytes.

Successful submission is not a flush or a durability guarantee. Buffered host
errors detected only by a later flush remain observable through
`try_flush_stdout`; these wrappers do not add an implicit flush. Raw numeric
and owning-string print callers can obtain a recoverable alternative by
formatting through `try_print("{}", value)` (borrow strings with `.view()`).
Allocation failure while constructing an owning result follows the current
defined-failure allocation policy rather than becoming a format or print error.

Migration: code handling the old `format` or `format_to` expected result should
use `try_format` or `try_format_to`. Code intentionally terminating on failure
uses the plain name and no longer calls `.value()`. `try_print` errors are now
`print_errc`, not `format_errc`. File open/read/write, external-input parsing,
HTTP/network requests, entropy and managed-thread startup remain expected-first;
panic adoption is not a blanket rename of environmental operations.

Buffered streams, encoding conversion, named/dynamic fields, and formatter
customization remain future library layers. A future buffer/stream object
should build on this byte surface without turning the runtime ABI into the
public GTI ABI.
