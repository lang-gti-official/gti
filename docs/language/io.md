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

`<std/assert>` also builds `std::assert` and `std::debug_assert` in source on
this panic facility. They use general typed `#macro` declarations, so a
successful assertion does not evaluate or format its optional message.
Disabled debug assertions evaluate no arguments, while retaining static
argument/body checking. Their terminal path has the same bounded snapshot,
caller attribution, deterministic cleanup and thread containment as panic;
there is no separate native `abort` or unwinding policy. See the
[standard-library contract](standard-library.md).
General concept-failure diagnostics can explain declared constraints, but
literal-format validation is not introduced by panic. Unsupported static
argument types are rejected by the source-defined `std::formattable` protocol
described below; dynamic presentation and value errors remain recoverable.

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
this layer; the composable writer's `FILE::flush()` only validates the open
handle and does not perform a durability sync. `seek` uses an explicit
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
expressions. A range may also iterate `value()` directly, because the observer
returns a checked reference tied to the `expected`:

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
ordinary defined-failure path, and the successful loop holds a loan on the
complete `expected` until every loop exit has cleaned up. Moving, replacing,
or dropping `chunk` during the loop is rejected, and so is any other mutation
of it; see [`ranges.md`](ranges.md#ranges-over-expected-payloads).

## Composable buffered I/O

`<std/io>` defines `std::byte_buffer` as `vector<uint8_t>` and source-defined
reader/writer contracts. Include `<std/span>`, `<std/cstdio>` and other units
directly when naming their declarations. Buffers own initialized bytes;
`byte_buffer(size)` establishes writable size, while `reserve` alone does not.

```gti
concept reader<R> = requires(mut R& stream, mut std::mutable_span<uint8_t>& output) {
  { stream.read_some(output) } -> expected<std::size_t, std::io_errc>;
};
concept writer<W> = requires(mut W& stream, std::span<uint8_t>& input) {
  { stream.write_some(input) } -> expected<std::size_t, std::io_errc>;
} && requires(mut W& stream) {
  { stream.flush() } -> expected<void, std::io_errc>;
};
```

These are exact ordinary member-signature requirements. Custom transports do
not inherit a library base class. A successful `read_some` or `write_some`
returns the committed byte count, never more than the supplied span size.
Zero on a nonempty read means EOF. Empty requests succeed with zero without
accessing a byte. A transport must report a partial successful prefix before
reporting a later error; an error alone cannot encode hidden partial progress.

The generic helpers are:

- `read_exact(source, output)`: fills the writable span or reports EOF/error.
- `write_all(destination, input)`: submits the complete span; zero progress on
  a nonempty write is `write_failed`, not an infinite retry.
- `copy(source, destination, scratch)`: copies until EOF using nonempty,
  caller-owned scratch space. It neither rewinds nor implicitly flushes.
- `getline(source, text, delimiter = uint8_t(10))`: clears `text`, consumes but
  excludes the delimiter, and preserves all other bytes (including NUL and CR).
  A final unterminated line succeeds; EOF before any byte returns `end_of_file`.
  This helper currently allocates a one-byte initialized scratch buffer per call;
  use a buffered reader to amortize host reads.

Each helper returns `expected<size_t, io::error>`, where `error` has `code` and
`transferred` fields. Progress counts bytes read into the destination, bytes
accepted by the writer, or line bytes appended, respectively. Copy counts bytes
accepted by its destination, not read-ahead bytes. A writer may buffer accepted
bytes: acceptance is not a flush or durability guarantee. Invalid oversized
transport counts are rejected before indexing or updating progress. On line
failure the accumulated prefix remains in `text`.

`memory_reader` borrows a read-only span and maintains a cursor. `memory_stream`
owns an exclusive span (not its underlying allocation), with read/write/tell
and bounded absolute seek. Writes do not grow storage; a full nonempty write
fails, while reading at the end returns zero. `memory_stream::flush()` has no
work to do; readers do not require a flush operation.

`FILE` implements the same contracts with direct partial reads/writes.
`FILE::flush()` validates that the unbuffered native descriptor is open; it
does not call `fsync` or promise durability. `stdin_reader`, `stdout_writer`
and `stderr_writer` borrow the process streams without closing them; output
flush drains the host stdio buffer and can fail. Applications must not mix
different buffering layers on a stream without accounting for read-ahead and
explicit flushing.

`buffered_reader<R>` and `buffered_writer<W>` exclusively borrow their
underlying transport and own reusable byte storage (default size 4096). A
reader returns pending bytes before refilling; destroying it discards unread
read-ahead bytes and does not rewind the source. A writer accepts only the
prefix fitting its buffer, draining a full buffer before accepting more.
`pending()` reports bytes not yet submitted to the underlying writer.
`flush()` drains them and then flushes the underlying writer. A failed partial
flush preserves exactly the unwritten suffix, so retry does not duplicate the
committed prefix. Zero-capacity adapters reject nonempty operations with
`invalid_stream`; empty requests still succeed.

**Buffered writers require explicit `flush()` and handling its result.** Their
destruction discards pending bytes; it does not perform fallible hidden I/O.
The underlying transport cannot be accessed, moved or destroyed while borrowed
by an adapter. These are synchronous byte-oriented facilities, not a
nonblocking readiness/cancellation or Unicode decoding protocol.

## Runtime boundary

Single-byte compatibility calls and bounded chunk calls cross the runtime.
`runtime.h` owns the versioned open-mode and seek-origin ordinals plus exact
`*_v1` functions for standard-stream chunks, file open/read/write/seek, and
standard-stream flush. The legacy single-byte and read-only entries remain for
existing generated programs. Ordinary `gti_internal::runtime` wrappers and
the trusted `<std/cstdio>` implementation translate status values into the
public `expected` API.

Paths and text writes cross as immutable, counted, non-retained
`gti_c_string_view` records from `<gti/c_abi.h>`. Partial binary writes use
versioned `gti_rt_write_{file,stdout,stderr}_some_v1` pointer/count entries.
Trusted source derives pointers only from checked nonempty initialized spans
or bounded local buffers, scoped by `unsafe`; pointers never enter the public
API and are not retained by the runtime. The runtime owns any temporary
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

The built-in admitted arguments are `bool`, `char`, every fixed-width integer,
binary32 `float`, binary64 `double`, `std::string_view`, and `std::string`.
Integral enums are also admitted by the ordinary primary `formatter<T>`:
their default is the exact underlying integer's numeric presentation, including
its full signed or unsigned width. All integer specifiers and dynamic error
rules apply unchanged. An exact application `formatter<MyEnum>` takes
precedence; an invalid selected specialization fails the `formattable` contract
and never falls back to numeric formatting. Payload enums require an explicit
formatter.
Booleans render as `true` or `false`; characters append their one code unit;
integers default to canonical base-10 text; and floating values default to the
shortest round-tripping decimal representation. String values preserve all
counted bytes, including embedded zero bytes.

The general prelude concepts `std::integral_enum<T>` and
`std::enum_underlying<T, U>` classify integral enums and their exact declared
underlying type. `std::to_underlying<U, E>(value)` extracts that exact integer
value, independently of formatting; for example, a protocol encoder can use
`std::to_underlying<uint8_t, Opcode>(opcode)`. A different width or signedness is
rejected. The result type is deliberately explicit because GTI does not yet
deduce function return types or support dependent underlying-type aliases;
both generic arguments are required because partial explicit argument lists
are unsupported. Extraction is runtime-only in this slice. The concepts
constrain ordinary declarations; bounded member queries on constrained members
can select `if constexpr` branches, as the formatter does. No implicit
enum/integer conversion is introduced. See
[`110-integral-enums.gti`](../../examples/110-integral-enums.gti).

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
mismatch produces `argument_count_mismatch`. Pattern grammar and field count
are validated first, so those errors take precedence over dynamic formatter
errors. A type without a valid formatter is a compile-time constraint error,
including a later pack argument not selected by the pattern. Named fields,
dynamic width or precision, and locale-specific formatting remain unsupported.

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

These operations are ordinary GTI library code. A source-defined exact formatter
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
allocation policy: infallible exhaustion reports `GTI-R0011` and aborts without
cleanup, rather than becoming a format or print error. `try_format` recovers
formatting errors; its name does not make result allocation fallible.

Migration: code handling the old `format` or `format_to` expected result should
use `try_format` or `try_format_to`. Code intentionally terminating on failure
uses the plain name and no longer calls `.value()`. `try_print` errors are now
`print_errc`, not `format_errc`. File open/read/write, external-input parsing,
HTTP/network requests, entropy and managed-thread startup remain expected-first;
panic adoption is not a blanket rename of environmental operations.

Composable buffered streams are provided by `<std/io>` as described above;
they build on the byte boundary without exposing the runtime ABI as the public
GTI API. `<std/encoding>` already implements counted-byte Base64 and hexadecimal
encoding/decoding. Unicode transcoding and named/dynamic formatting fields are
separate unfinished capabilities.

## User-defined formatters

Applications extend formatting with ordinary exact class specialization:

```gti
struct AssetId { uint64_t value; };
class std::formatter<AssetId> {
public:
  expected<void, std::format_errc> format(AssetId& value,
      mut std::string& output, std::format_spec& spec) {
    std::formatter<uint64_t> numeric{};
    return numeric.format(value.value, output, spec);
  }
};
```

The specialization must be default-initializable and provide exactly this
public, safe, read-only, nongeneric method signature. `std::formattable<T>` is a
source-defined conjunction of default initialization and the exact method
requirement, not a compiler-known capability. Normal primary-or-argument package
ownership controls specialization coherence. The primary satisfies the contract
only for integral enums; other application types need an exact specialization.
A selected specialization must satisfy the contract itself. A user may
constrain a forwarding generic with `requires std::formattable<T>`.

`format_spec` exposes the parsed common presentation fields: `fill`, `alignment`,
`sign`, `alternate`, `zero_pad`, `width`, `has_precision`, `precision`, and `type`.
Zero alignment/type means unspecified; the default fill is a space and sign is
`'-'`. A formatter should implement or reject every non-default field it accepts.
It can delegate to a built-in formatter or compose ordinary `try_format_to`
calls. This first public protocol shares the existing common specification
grammar; it does not add C++ iterator/context types or arbitrary per-type parse
grammars. Neither compiler nor LSP parses format strings.

The formatter appends to a temporary result and may report any `format_errc`.
`invalid_format` normally means a rejected presentation; `unsupported_argument`
remains available for a value-dependent rejection by an otherwise supported
type. `try_format`, `try_format_to`, `try_print`, `try_println`, the convenience
twins, and formatted panic all use this one protocol. Failed formatting never
publishes a partially appended destination or submits partial formatted stdout.
Directly calling a formatter is lower-level: its caller owns the destination
and any partial append behavior. A formatter is constructed per selected field,
so repeated indexed fields repeat its method call in pattern order.

Unsupported static types receive ordinary contract diagnostics in CLI and LSP;
no fix can invent the application's formatting policy. Generic forwarding
wrappers should declare the same required contracts. Literal syntax, count, and
specifier validation remain runtime checks until a general call-site constant
validation facility exists. See `examples/106-custom-formatters.gti`.
