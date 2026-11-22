Reject returning a view that retains the address of a local intermediate
object, even when that object borrows a longer-lived stream. Bulk reads
through `std::istream` now call only the underlying read operation.
