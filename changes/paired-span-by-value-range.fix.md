A by-value range-for over a `std::paired_span` (`for (auto row : rows)`) now
compiles instead of failing with an internal compiler error or a spurious
`GTI-S2045`.
