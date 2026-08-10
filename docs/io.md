# Unbuffered byte I/O

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

## Runtime boundary

Each read requests exactly one byte from the operating system. There is no
library or C stdio input buffer in this slice. The prelude declares the exact
native symbols `gti_rt_read_stdin_byte`, `gti_rt_open_file_read`,
`gti_rt_read_file_byte`, and `gti_rt_close_file` in an `extern "C"` block, then
ordinary `gti_internal::runtime` wrappers translate private integer status
codes into the public `expected` API in GTI source. Paths cross as the
immutable, counted, non-retained `gti_c_string_view` input from
`<gti/c_abi.h>`; the runtime owns any temporary NUL-terminated native path it
needs. The complete bounded ABI and lifetime rules are in
[`native-c-interop.md`](native-c-interop.md).

Buffered streams, writes, seeking, filesystem operations, encoding conversion,
standard error, and structured formatting remain future library layers. A
future buffer/stream object may build on or supersede this byte-at-a-time
surface without changing the runtime ABI into the public GTI ABI.
