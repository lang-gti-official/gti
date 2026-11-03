Conformance blocks accept trailing requirements, including recursive protocol
requirements, with bounded cycle diagnostics. `std::formatting` and
`std::hashing` let generic types opt in once; pairs format as `(first, second)`
and work as unordered keys when their elements support the required operations.
Exact formatter and hash specializations remain supported through adapters.
