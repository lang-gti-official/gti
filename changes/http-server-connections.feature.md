Third-party servers can serve HTTP/1.1 using the incremental
`std::http::server_codec` on a nonblocking connection or
`std::http::open_server_connection` on a blocking duplex stream. Both support
streamed request bodies, ordered response fields and persistent connections.
