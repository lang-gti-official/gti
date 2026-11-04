- Concept applications such as `std::integral<T>` and user-defined `has_size<T>`
  are constant Boolean expressions. `if constexpr` selects an implementation
  using the existing constraint guarantees, including protocol dispatch;
  conjunction combines branch facts, while disjunction and negation add none.
