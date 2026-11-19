Add counted text search, prefix and suffix checks, comparison, and checked
character access to `std::string_view`. The read-only operations work in
constant expressions. Searches and comparisons accept owning string operands
by borrow when `std.string` is imported. `view.substr()` returns the full view
with the same owner dependency.
