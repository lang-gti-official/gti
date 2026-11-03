Add `std::max_element(first, last, compare)` for selecting maxima by a custom
ordering, including mapped values and fields of records without `operator<`.
Comparators can be stateful or move-only; ties retain the first maximum and
empty ranges return the end position.
