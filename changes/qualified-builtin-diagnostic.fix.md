Explain that built-in types such as `uint64_t` cannot be namespace-qualified,
pointing at the type in `std::uint64_t` instead of reporting an unrelated
declaration error at `constexpr`. The editor publishes the same explanation.
