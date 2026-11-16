`std::http::request::add_header`, `set_header` and `set_ca_bundle` return
`void` instead of `expected<void, errc>`. An invalid header name or value,
or a CA path containing NUL, is not applied; the request records the first
refusal, and `send`, `send_stream` and every client verb return it
(`errc::invalid_header` or `errc::request_failed`) before contacting the
server. `request::rejection()` reports it earlier. Remove checks of the
builder results; the send result already covers them.
