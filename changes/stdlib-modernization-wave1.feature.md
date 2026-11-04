- Faster standard-library hot paths with unchanged results:
  `print`/`println` of a `std::string` make one counted write; string
  `append`, `insert` and `replace` grow geometrically instead of
  reallocating on every append; `std::advance`, `next`, `prev` and `distance`
  jump random-access cursors, so `lower_bound`, `upper_bound`,
  `partition_point` and `binary_search` move in O(log n); `std::io` readers
  and writers copy bytes in bulk; integer `std::midpoint` vectorizes.
  `std::io::buffered_reader` exposes its read-ahead window through
  `fill_buffer()`, `buffered()` and `consume(n)` and the new
  `std::io::buffered_source` concept, which `std::io::getline` uses to scan
  whole windows. `std::stoi`, `stol`, `stoll`, `stoul` and `stoull` over a
  `string_view` are `constexpr`.
