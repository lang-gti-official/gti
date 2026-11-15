Run owning containers during constant evaluation: `std::string`,
`std::vector` and containers built on `std::storage` now work inside
`constexpr` functions, `static_assert` and procedural macros, with every raw
storage access checked and every allocation released before a passive result
is published. Runtime allocation is unchanged.
