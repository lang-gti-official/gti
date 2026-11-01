Fixed-size arrays can be borrowed with `T[N]&`, `mut T[N]&`, and inferred
`auto&` bindings. Exact extents survive generic calls and checked reference
returns, with ordinary lifetime, exclusive-access, and checked-index rules.
