Conformance blocks accept trailing requirements, including recursive protocol
requirements, with bounded cycle diagnostics. `std::formatting` and
`std::hashing` let generic types opt in once; pairs format as `(first, second)`
and work as unordered keys when their elements support the required operations.
Exact formatter specializations retain a compatibility adapter; legacy hash
specializations remain available as direct or explicitly selected policies.
