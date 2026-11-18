A method result written in a region its class stores, such as `int&<a>` in
`class View<lifetime a>`, may now also return a parameter written in that
region: `int&<a> get_or(uint64_t key, int&<a> fallback)` returns either a
stored element or `fallback`. Callers keep both the receiver's stored referents
and those arguments borrowed while the result is live. Before, returning the
parameter was rejected.
