# TCP Streams And Listeners

Status: current standard-library contract (POSIX targets)

`import std.tcp;` provides IP addresses, connected streams, listeners and name
resolution. Every resource is a move-only owner with deterministic cleanup,
every fallible operation returns `expected<T, std::tcp::errc>`, and the
address, buffering and error policy is ordinary GTI source over a small
versioned runtime boundary.

```gti
import std.tcp;

expected<void, std::tcp::errc> exchange() {
  auto any = std::tcp::socket_address(std::tcp::ipv4_address::loopback(), uint16_t(0));
  mut auto server = ?std::tcp::listener::bind(any);   // port 0: ephemeral
  auto endpoint = ?server.local_address();
  mut auto client = ?std::tcp::stream::connect(endpoint);
  mut auto peer = ?server.accept();
  // client.send_all(...), peer.receive(...), ...
  return;                                              // all three close here
}
```

[`examples/037-tcp-socket-owner.gti`](../../examples/037-tcp-socket-owner.gti)
is a complete low-level loopback exchange, and
[`examples/193-tcp-line-server.gti`](../../examples/193-tcp-line-server.gti) a
line service built on `listen`, `serve` and `buffered_stream`.

## Addresses

`std::tcp::ipv4_address` and `std::tcp::ipv6_address` are plain copyable
values. Each has `loopback()`, `any()` (the wildcard), `parse(string_view)`,
`to_string()`, `is_loopback()`, `is_unspecified()` and equality. IPv4 exposes
`octet(i)` and `to_uint32()`; IPv6 exposes `segment(i)` and
`mapped(ipv4_address)` for `::ffff:a.b.c.d`.

Parsing is strict and allocation-free:

- IPv4 accepts exactly four decimal parts of 1–3 digits, each at most 255,
  without leading zeros (`01.2.3.4` is rejected, as an octal ambiguity).
- IPv6 accepts RFC 4291 text: eight hex groups of 1–4 digits, at most one
  `::` run, and an optional trailing dotted quad.

`ipv6_address::to_string()` produces RFC 5952 canonical text: lowercase, no
leading zeros, the longest run of two or more zero groups (the first on a tie)
written as `::`, and IPv4-mapped addresses in dotted form.

`std::tcp::socket_address` is an IP address plus port (and an IPv6 scope id).
Construct it from `(ipv4_address, port)` or `(ipv6_address, port, scope_id = 0)`,
or parse `a.b.c.d:port`, `[v6]:port` or `[v6%scope]:port` with a numeric scope.
`ipv4()` and `ipv6()` return the host as `expected`, failing with
`invalid_address` for the other family; `port()`, `scope_id()`, `family()`,
`with_port(p)` and `to_string()` complete the value API. Host names are never
parsed here: use `resolve`.

All three address types conform to the `std::formatting` protocol concept, so
`std::format("{}", address)` renders their canonical text.

`std::tcp::resolve(string_view host, uint16_t port)` returns
`expected<std::vector<socket_address>, errc>` in resolver preference order (at
most 16 entries). Numeric hosts need no name service. An empty host is
`invalid_address`; an unknown name is `name_not_found`.
`resolve(host, port, address_family)` keeps only endpoints of that family, and
reports `name_not_found` when the host has none.

## Streams

`std::tcp::stream` is a connected socket. It is noncopyable and movable;
moving it transfers the duty to close the descriptor. It may move to another
thread (`[[unsafe_transfer]]`: a descriptor is process-wide) but is not
share-capable. The public API never exposes the descriptor: the adopting
constructor takes `gti_internal::tcp_socket_handle`, which application source
cannot name (`GTI-S2058`).

| Operation | Result |
| --- | --- |
| `static connect(socket_address&)` | `expected<stream, errc>` |
| `static connect(socket_address&, std::chrono::nanoseconds timeout)` | fails with `timed_out` after `timeout` |
| `receive(mut mutable_span<uint8_t>&) mut` | bytes received; `0` is the peer's end of stream |
| `send(span<uint8_t>&) mut` | bytes sent (possibly a prefix) |
| `send_all(span<uint8_t>&) mut` | `expected<void, errc>`; retries partial sends |
| `shutdown(shutdown_mode) mut` | ends `read`, `write` or `both` directions |
| `local_address()`, `peer_address()` | `expected<socket_address, errc>` |
| `set_nonblocking(bool) mut` | nonblocking `receive`/`send` report `would_block` |
| `set_read_timeout(nanoseconds) mut`, `set_write_timeout(nanoseconds) mut` | bounds each blocking call; expiry is `timed_out`; zero removes the bound |
| `wait_readable(nanoseconds)`, `wait_writable(nanoseconds)` | readiness, or `timed_out` |
| `set_nodelay(bool) mut`, `set_keepalive(bool) mut` | `TCP_NODELAY`, `SO_KEEPALIVE` |
| `close() mut` | closes once; a second call is `not_open` |
| `is_open()` | whether the stream still owns a descriptor |

Durations are rounded up to whole milliseconds, so a positive timeout never
becomes "do not wait"; a negative duration is `invalid_argument`.

A stream also satisfies `std::io::reader` and `std::io::writer`:
`read_some`/`write_some` return `expected<size_t, std::io_errc>` and `flush()`
succeeds while the stream is open (a stream has no user-space buffer). Every
`std::io` helper therefore works on a socket: `read_exact`, `write_all`,
`copy`, `getline`, and `buffered_reader`/`buffered_writer` borrowing the
stream. Through that interface the transport outcomes keep their meaning:
`would_block`, `timed_out` and `broken_pipe` map to the `std::io_errc` values
of the same name, `connection_reset` and `connection_aborted` to
`connection_reset`, and a closed stream to `invalid_stream`. Other failures
are `read_failed`/`write_failed` (`flush_failed` for a `buffered_stream`
flush). Use `receive`/`send` when the full `tcp::errc` matters.

## Listeners

`std::tcp::listener::bind(socket_address&, int32_t backlog = 0)` binds with
address reuse and starts listening; a nonpositive backlog uses the host
default. Port 0 selects an ephemeral port, which `local_address()` reports.
`accept() mut` returns a connected, blocking `stream`. `accept_from() mut`
returns an `incoming` holding that `connection` and the `peer` address the
host recorded at accept time; unlike a later `peer_address()`, reading it
cannot fail because the client has already reset. A nonblocking listener
(`set_nonblocking(true)`) reports `would_block` when nothing is pending, and
`wait_acceptable(nanoseconds)` waits for a connection or fails with
`timed_out`. `close()` and `is_open()` behave as for streams. A listener has the
same move-only, transferable, non-shareable ownership as a stream.

## Host And Port Helpers

`std::tcp::connect(host, port)` resolves `host` and connects to the first
endpoint that accepts, returning the last attempt's error when none does;
`connect(host, port, timeout)` bounds each attempt. `std::tcp::listen(host,
port, backlog = 0)` binds the first resolved address that binds. Use
`"0.0.0.0"` or `"::"` for every interface and port 0 for an ephemeral port.

## Buffered Streams

`std::tcp::buffered_stream` owns a `stream` plus input and output buffers
(`capacity` bytes each, default 8192). It is move-only and transferable like a
stream.

Reading uses a fill/view/consume protocol, so framing never copies:

```gti
std::size_t length = ?client.fill_until(uint8_t(10));  // through '\n'
{
  auto frame = client.peek(length);   // span<uint8_t, this>: borrows client
  // ... parse frame in place ...
}
client.consume(length);                // the borrow must have ended
```

- `fill()` returns the available count, receiving once only when nothing is
  available; zero is end of stream.
- `fill_until(delimiter)` receives until the available bytes contain the
  delimiter and returns the frame length through it. At end of stream it
  returns the unterminated remainder (zero when nothing is left). A frame that
  does not fit the buffer fails with `resource_exhausted`.
- `buffered()` and `peek(count)` return `span<uint8_t, this>`, a view that
  borrows the object's buffer. The borrow checker rejects any mutating call
  (including `consume`, `fill` or a move) while the view is live.
- `consume(count)`, `available()`, `read_exact(mut mutable_span<uint8_t>&)`
  and `read_line(mut string&)` (strips `\n` and a preceding `\r`; returns
  bytes consumed, zero at end of stream) complete the read side.

Writing queues bytes: `write(span<uint8_t>&)` and `write_text(string_view)`
send only when the output buffer fills; `flush_output()` sends the rest (the
unsent suffix stays queued on failure); `finish_output()` flushes and shuts
down the write direction; `unflushed()` counts queued bytes. Destruction
discards unflushed output, as `std::io::buffered_writer` does: flush and check
the result first. `get()` returns `mut stream&<this>` for addresses and
options; reading or writing through it bypasses the buffers.

A `buffered_stream` satisfies `std::io::reader`, `std::io::writer` and
`std::io::buffered_source`, so `std::io::getline`, `read_exact`, `write_all`
and `copy` use its buffers directly.

## Serving Connections

`serve` runs a sequential accept loop over any type that implements the
`connection_handler` protocol concept with a conformance block:

```gti
class Echo { /* application state */ };

for concept std::tcp::connection_handler<Echo> {
  expected<std::tcp::serve_control, std::tcp::errc> handle(
      mut Echo& self, std::tcp::stream connection, std::tcp::socket_address peer) {
    // own, wrap or move `connection`; it closes when dropped
    return std::tcp::serve_control::proceed;   // or stop
  }
};

auto summary = ?std::tcp::serve(server, echo);  // serve_summary{accepted, failed}
```

`serve` accepts with `accept_from`, so the handler receives the recorded peer
address. The handler owns each connection. A handler error drops only that connection
and counts in `failed`; `serve_control::stop` ends the loop. Accept failures
caused by one aborted client (`connection_aborted`, `connection_reset`) are
skipped; any other accept failure ends `serve` with that error. A type without
a conformance block is rejected at the call. `serve` handles one connection at
a time; a program built with `--execution-profile concurrent` can instead move
each accepted `stream` into a `std::thread` task, because streams are
transfer-capable.

## Errors

`std::tcp::errc` has stable categories, mapped from host error numbers by the
runtime: `would_block`, `timed_out`, `connection_refused`, `connection_reset`,
`connection_aborted`, `not_connected`, `broken_pipe`, `address_in_use`,
`address_unavailable`, `network_unreachable`, `host_unreachable`,
`permission_denied`, `invalid_argument`, `invalid_address`, `name_not_found`,
`name_resolution_failed`, `resource_exhausted`, `unsupported`, `not_open`,
`close_failed` and `failed` (an unclassified host error). No native error
number crosses the boundary.

## Cleanup

`close()` invalidates the owner before the native close, so success and
failure both end ownership and nothing is closed twice; a failed close is
`close_failed` and is not retried. Destruction performs one best-effort close
of a still-open owner, because cleanup cannot report an error: call `close()`
when the result matters.

## Runtime Boundary

The module calls the versioned `gti_rt_tcp_*_v1` entries declared in
[`runtime/include/gti/tcp.h`](../../runtime/include/gti/tcp.h). They exist
because the host differs where source cannot see: `sockaddr` layout (BSD
`sin_len`), constant values, `errno`, `EINTR` retries, close-on-exec and
`SIGPIPE` suppression (`MSG_NOSIGNAL` or `SO_NOSIGPIPE`). Addresses cross as a
24-byte encoded record, never a native `sockaddr`; buffers cross as
call-bounded pointer and length pairs inside lexical `unsafe` blocks; the
runtime retains nothing. A blocking socket whose `SO_RCVTIMEO`/`SO_SNDTIMEO`
expires reports `timed_out`, while a nonblocking socket reports
`would_block`. Connection timeouts use a nonblocking `connect` and `poll`,
then restore blocking mode.

Importing `std.tcp` on Windows or an unknown target is a compile-time error;
the runtime's Windows entries return `unsupported` only so the archive links.

## Not Yet Provided

UDP, Unix-domain sockets, TLS, a readiness multiplexer over many sockets, a
concurrent or asynchronous `serve`, socket options beyond those listed,
scatter/gather I/O and Windows support.
