# TCP Socket Ownership

Status: current bounded standard-library contract

`#include <std/tcp>` provides a small POSIX socket owner implemented in
ordinary GTI over the bounded C ABI:

```gti
#include <std/tcp>

int main() {
  mut auto opened = std::tcp::open();
  if (!opened) {
    return 1;
  }

  auto closed = opened.value().close();
  return closed ? 0 : 2;
}
```

## Public Surface

The current public names are:

```gti
namespace std {
namespace tcp {
enum class errc {
  open_failed,
  close_failed,
  not_open,
};

class socket {
public:
  socket(socket& other) = delete;
  socket(socket&& other) = default;
  static expected<socket, errc> open();

  // The module supplies automatic public cleanup.
  bool is_open();
  expected<void, errc> close() mut;
};

expected<socket, errc> open();
}
}
```

`socket::open()` is the authorized construction path. It asks POSIX for an
IPv4 stream socket using `AF_INET` and `SOCK_STREAM`, returns `open_failed` when
creation fails, and otherwise returns an `expected` whose value alternative
owns the move-only socket. The free `std::tcp::open()` function is a convenience
wrapper around that static factory. The descriptor-adopting constructor is
private, and `gti_internal::tcp_socket_handle` is visible only inside
compiler-trusted source units. Application declarations, references, and
namespace aliases involving root `gti_internal` are rejected with
`GTI-S2058`; completion, hover, definition, and semantic classification do not
present the handle to application documents. The public API exposes no
descriptor getter or adoption path.

The owner is noncopyable and movable. Its generated move transfers the active
cleanup obligation, so only the destination may close the descriptor. A socket
that remains open is closed once during lexical destruction. Destructor close
is best-effort because cleanup cannot return an error; call `close()` explicitly
when the result matters.

The returned `expected<socket, errc>` owns the socket in its value alternative
and is itself move-only. Move that result as a whole when transferring it.
GTI's current partial-place rules do not permit moving the socket separately
out of `result.value()`.

`close()` invalidates the private handle before calling POSIX `close`. A native
close failure therefore returns `close_failed` but is not retried by a later
call or by destruction. Calling `close()` after ownership has already ended
returns `not_open` without entering native code.

## Native And Target Boundary

The optional unit declares only these exact C symbols:

```gti
extern "C" {
  int32_t socket(int32_t domain, int32_t type, int32_t protocol);
  int32_t close(int32_t descriptor);
}
```

The `gti_internal::runtime` declarations are compiler-private implementation
details. Trusted `<std/tcp>` source can call them, while application lookup and
tooling cannot resolve or present them. The private `socket` constructor adds
the ordinary class-access boundary inside the trusted implementation; neither
layer exposes a raw descriptor API.

The implementation currently relies on the Linux/macOS values `AF_INET = 2`
and `SOCK_STREAM = 1`. Importing `<std/tcp>` on Windows or an unknown target is
a compile-time error. Creating an unconnected socket allocates a local kernel
resource but sends no network traffic.

This slice intentionally does not provide address construction, `connect`,
`bind`, `listen`, `accept`, `send`, or `receive`. Those operations require a
reviewed cross-platform address representation and bounded mutable/read-only
byte-buffer API. GTI's bounded one-level raw pointers do not define native
`sockaddr` layout, a safe slice, a public descriptor API, traffic ownership and
retention rules, or a general unsafe FFI merely because this owner uses C
linkage internally.
