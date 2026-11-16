Add `std::string_view::substr(position[, count])`, `remove_prefix` and
`remove_suffix`. A sub-view shares the source's characters and keeps the same
owner borrowed, so it cannot outlive that owner. The count clamps to the
remainder; a position or count past `size()` raises `GTI-R0007`.
