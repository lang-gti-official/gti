Fix an internal compiler error (`derived loan ... conflicts with active sibling
loan`) when iterating or reading through a local reborrow of a `mut` reference
parameter that holds an exclusive view, such as a range-for over a
`mut std::mutable_span<T>&` parameter or a `T& alias = parameter;` binding.
