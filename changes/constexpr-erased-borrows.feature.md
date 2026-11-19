Use checked read-only erased borrows in constexpr code with `std::erase_ref`
and `std::restore_ref<T>`, including borrows stored in local view types.
`std::array<T, N>::as_span()` now returns the same checked read-only
`std::span<T>` as vector and string factories, with constexpr traversal of
eligible array and vector storage.
