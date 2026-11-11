`std.tcp` now connects, listens and transfers data. It adds IPv4/IPv6 address
values with strict parsing and RFC 5952 formatting, `socket_address`,
`resolve`, and move-only `stream` and `listener` owners with connect/accept,
send/receive, shutdown, timeouts, nonblocking mode and readiness waits. Streams
satisfy `std::io::reader`/`writer`, and addresses format through `std::format`.
The unconnected `std::tcp::socket` and `std::tcp::open()` are removed.
