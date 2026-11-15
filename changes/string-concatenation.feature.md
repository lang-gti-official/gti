`std::string` supports `+` with a `std::string_view` or `std::string` right
operand and `+=` for appending. A temporary or moved left operand appends in
place, so `std::string("id: ") + std::to_string(n) + "!"` reuses one buffer.
A literal on the left of `+` now gets a hint pointing at `std::string("...")`.
