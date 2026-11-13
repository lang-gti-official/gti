`std::io_errc` gains the transport categories `would_block`, `timed_out`,
`connection_reset` and `broken_pipe`. `std::tcp` streams and buffered
streams report them through the `std::io` reader and writer concepts instead
of collapsing to `read_failed`/`write_failed`. `listener::accept_from()`
returns the accepted stream with the peer address the host recorded, which
`serve` now passes to handlers, and `resolve(host, port, address_family)`
filters endpoints by family.
