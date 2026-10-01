# spdlog for GTI

`gti-spdlog` is a source-defined, synchronous logging package modelled after
the public shape of [spdlog](https://github.com/gabime/spdlog). It uses GTI's
ownership, checked failures, standard formatting, source locations, atomics,
and mutexes directly; it is not a C or C++ binding and has no native build
phase. It requires GTI 0.357.0 or newer.

The initial package deliberately excludes asynchronous logging. It is suitable
for console, basic-file, size-rotating-file, null, ring-buffer, multi-sink, and
explicit-registry workflows.

## Add the package

Pin an immutable revision in `gti.toml`:

```toml
[dependencies]
spdlog = { git = "https://git.internal.howetotech.dev/gti-lang/spdlog.git", rev = "<40-hex-commit>" }
```

Then run `gti fetch`, commit `gti.lock`, and include the public unit:

```gti
#include <spdlog/spdlog>

int main() {
  spdlog::logger_ptr log = spdlog::stdout_color_mt("tea");
  log->set_level(spdlog::level::level_enum::debug);
  log->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");

  auto status = log->info("listening on port {}", uint16_t(8080));
  if (!status) { return 1; }
  return 0;
}
```

Logging operations return `expected<void, spdlog::errc>` instead of throwing.
Handle `format_error`, `invalid_pattern`, `open_failed`, `io_error`, and
`clock_error` where the application can recover.

## Source locations

`spdlog::source_loc::current()` uses GTI's caller-resolved
`std::source_location`. It owns its file and function names, so deferred
backtrace entries do not borrow compiler or caller storage:

```gti
auto status = log->log(
    spdlog::source_loc::current(),
    spdlog::level::level_enum::warn,
    "configuration is using defaults");
```

The source-aware pattern flags are `%g` (full file), `%s` (basename), `%#`
(line), and `%!` (function).

## Shared sinks

Sink polymorphism and sink lifetime are separate. `sinks::sink` supplies the
interface; `spdlog::sink_ptr` is a `std::shared_ptr<sinks::sink>` so multiple
loggers can retain one synchronized sink. A concrete handle is explicitly and
safely upcast while preserving its original shared control block:

```gti
mut auto concrete =
    std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(std::size_t(32));
auto inspect = concrete.clone();
spdlog::sink_ptr sink =
    std::upcast_shared<spdlog::sinks::sink,
                       spdlog::sinks::ringbuffer_sink_mt>(
        std::move(concrete));
spdlog::logger_ptr log = spdlog::make_logger("memory", std::move(sink));
```

The shared pointer makes lifetime safe; each supplied sink's mutex makes its
mutable contents safe. The compiler also rejects an upcast that would erase a
concrete sink's `[[no_transfer]]` or `[[no_share]]` restriction.

## File loggers

```gti
mut auto opened = spdlog::rotating_logger_mt(
    "service", "service.log", std::size_t(10 * 1024 * 1024),
    std::size_t(3), true);
if (!opened) { return 1; }
mut spdlog::logger_ptr log = std::move(opened).value();
if (!log->info("started")) { return 2; }
```

`basic_logger_mt(name, path, truncate)` appends by default.
`rotating_logger_mt(name, path, maximum_size, maximum_files,
rotate_on_open)` retains numbered backups `path.1` through `path.N`. Zero size
or backup count returns `invalid_argument`. File writes are immediate; `flush`
is retained for API consistency.

The `_st` class and factory spellings are supported as aliases of the same
synchronized implementation. GTI intentionally does not expose a second
unsynchronized interior-mutation policy merely to make `_st` observably
different.

## Implemented surface

- `level::level_enum`, long/short names, parsing, and filtering;
- `source_loc`, `details::log_msg`, `formatter`, and `pattern_formatter`;
- shared `sink_ptr` and `logger_ptr` handles;
- stdout/stderr, color, null, ring-buffer, basic-file, and rotating-file sinks;
- one or many sinks per logger, logger and per-sink levels, patterns, custom
  formatter cloning, `flush_on`, and synchronous flush;
- `trace`, `debug`, `info`, `warn`, `error`, `critical`, general `log`, and
  heterogeneous `std::format` arguments;
- bounded message backtrace with `enable_backtrace`, `disable_backtrace`, and
  `dump_backtrace`; and
- an explicit synchronized `registry` with register/get/default/drop,
  all-logger level/pattern updates, and flush.

Pattern formatting currently implements `%%`, `%v`, `%n`, `%l`, `%L`, `%g`,
`%s`, `%#`, `%!`, `%Y`, `%m`, `%d`, `%H`, `%M`, `%S`, `%e`, `%f`, `%z`, `%t`,
`%^`, `%$`, and `%+`. Time is rendered in UTC; `%z` is `+00:00`; `%t` is the
stable placeholder `0`; and automatic color mode currently selects no color
because GTI has no portable terminal-capability query. Choose
`color_mode::always` when ANSI color is known to be accepted.

## Intentional differences and current omissions

- There is no process-global default logger or global `spdlog::info`-style
  state. Use an owned `logger_ptr` or `registry`; this keeps lifetime and
  teardown explicit.
- Errors are typed `expected` values, not C++ exceptions.
- Async loggers, thread pools, overflow policies, periodic flush, daily/hourly
  file rotation, syslog/platform sinks, callback sinks, distribution sinks,
  duplicate filters, environment configuration, and binary-to-hex helpers are
  not in this release.
- The logging backtrace is spdlog's bounded message-history feature. A native
  call-stack `std::stacktrace` is not currently available in GTI and is not
  emulated here.
- Pattern padding/truncation modifiers and the remainder of spdlog's pattern
  flags are not yet implemented. Unsupported flags return `invalid_pattern`
  when a message is rendered.

Run the package test with an optimized GTI compiler:

```sh
gti test --profile dev --no-cache
```

The package is MIT licensed.
