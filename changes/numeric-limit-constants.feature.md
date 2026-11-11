`std.numeric` now provides the minimum and maximum of every fixed-width integer
(`std::MIN_INT8` through `std::MAX_INT64`, `std::MAX_UINT8` through
`std::MAX_UINT64`) and the IEEE-754 limits of `float` and `double`
(`std::MAX_FLOAT`, `std::MIN_FLOAT`, `std::LOWEST_FLOAT`,
`std::TRUE_MIN_FLOAT`, `std::EPSILON_FLOAT` and the `_DOUBLE` counterparts).
All are `constexpr` and usable in `static_assert` and other constant contexts.
