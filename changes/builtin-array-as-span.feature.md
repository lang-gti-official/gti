Built-in fixed arrays now provide `as_span()` through `std.span`.
The checked read-only view supports constexpr traversal and can retain
a static lifetime when it borrows a static array, including constexpr bytes.
