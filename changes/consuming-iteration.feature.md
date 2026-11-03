Add explicit consuming range iteration on LLVM. `std::move(values).into_range()`
transfers a vector and yields owned, potentially move-only elements without
cloning or exposing vacant slots. Early exit cleans up all remaining elements.
User-defined ranges can implement the same structural `has_next`/`take_next`
protocol; unsafe `std::take_at` transfers a live raw object into an owned value.
