Importing standard-library modules is faster: unused `constexpr` library functions are no longer analysed up front, and are analysed only when a compile-time evaluation reaches them.
