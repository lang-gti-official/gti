Add formatted input and output over file and memory transports, including
checked, chainable `<<` and `>>` operators. `std::ifstream`, `std::ofstream`,
and `std::fstream` support path construction, a static `open` factory,
`try_open` on an existing stream, and typed errors through `error()`. Input
and output on a bidirectional file share one buffered cursor.
