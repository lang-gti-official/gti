Run range-based `for` during constant evaluation: loops over fixed arrays,
`std::vector`, `std::span`, `std::array` and your own ranges now work inside
`constexpr` functions, `static_assert`, constant initializers and procedural
macros, including `break`, `continue`, `return`, nested and mutable loops.
Classes that store references, such as iterators, may now have `constexpr`
constructors and methods; a failed cursor check is a compile-time error.
