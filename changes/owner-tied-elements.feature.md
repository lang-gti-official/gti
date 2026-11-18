Element references from read-only iterators and `std::span` accessors are tied
to the container they view instead of the iterator or span object. `int& x =
*v.begin();`, `int& m = *std::max_element(v.begin(), v.end());`,
`v.as_span()[0]`, and keeping `*it` across `++it` now compile, while mutating,
moving or escaping the container under a live reference stays rejected.
Mutable iterators keep their iterator-tied results.

Temporary iterator wrappers can also relay references through nested carriers; the compiler verifies the complete stored-reference path instead of rejecting it with an internal error.
