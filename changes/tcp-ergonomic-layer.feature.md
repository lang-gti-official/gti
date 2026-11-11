`std.tcp` adds an ergonomic layer. `connect(host, port)` and
`listen(host, port)` take host names. `buffered_stream` frames input with
`fill_until` and lends frames in place as `span<uint8_t, this>` until
`consume`, and buffers output until `flush_output`. `serve(listener, handler)`
runs an accept loop over any type with a `for concept
std::tcp::connection_handler` block.
