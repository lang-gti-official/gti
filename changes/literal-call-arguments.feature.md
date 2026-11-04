Integer literal arguments adopt the integer parameter type of the unique
fitting function, method, constructor, base-initializer, operator or lambda
candidate when no candidate matches exactly, so `twice(4)`, `values[0]`,
`values.push_back(1)` and `Counter{3}` need no explicit `uint64_t(...)` cast.
Exact candidates always win first, a literal that fits several candidates
stays ambiguous, named values keep their types, and an out-of-range literal
is rejected with a hint naming its value.
