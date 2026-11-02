Add stable `std::remove` and `std::remove_if` compaction for checked indexed
sequences, returning the retained prefix length. Add `std::erase` and
`std::erase_if` for vector, deque, string, list, and forward_list, plus
predicate erasure for ordered/unordered maps and sets. Erasure returns the
number destroyed, preserves retained order, and supports owned elements
without implicit copying. Linked-list `remove_if` and checked deque/string
`swap_elements` complete the source-defined implementation.
