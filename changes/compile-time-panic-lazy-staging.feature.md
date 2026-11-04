`std::panic` in compile-time code now fails with its own message, and every
compile-time context follows one lazy rule. When a panic runs during a
`consteval` call, a `constexpr` binding, `static_assert` or `if constexpr`, the
compile error quotes the message at your `std::panic` call and shows how
evaluation reached it. Only the code that actually runs during compilation must
be `constexpr`: a panic, a non-`constexpr` call, `std::assert` or a macro on a
branch that isn't taken no longer rejects the call. A non-`constexpr` call that
does run is reported by name, the same way in every context. Formatted
messages such as `std::panic("odd {}", v)` still can't run during compilation.
A `static_assert` that reads a parameter now points to the
`if (!condition) { std::panic("..."); }` pattern.
