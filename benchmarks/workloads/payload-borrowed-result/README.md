# Reference payload results versus built-in fallible borrows

All variants perform ten million lookups. Success borrows the current local
integer without copying or allocating it; every fourth lookup instead owns the
same 61-byte error string. Compare matching contracts:

- `source` consumes the enum and moves its error into an arm-local owner;
  `builtin-consumed` moves the expected error into an ordinary local owner.
- `source-observed` borrows the enum's error field; `builtin` observes the
  expected error before ordinary scope cleanup.

Each owned error is destroyed once. All check the full sum before printing the
same benchmark record. Consumed versus observed results are useful additional
cost information, not a parity claim. This comparison does not measure protocol
dispatch or claim identical public APIs.

Run `scripts/benchmark_compare.py --gti <compiler> --cxx <native-compiler>
--optimization 2 --cpp-standard c++23 --warmup 3 --runs 30 --seed 178
--output <evidence> --workload payload-borrowed-result` on a quiet host.
The harness retains source/compiler identities, native build costs, executable
sizes and paired runtime samples. Measure allocations separately with a native
allocator counter; timing and an expected allocation count are not allocation
measurement evidence. Define `ALLOCATION_COUNTS` and link
`tests/fixtures/stdlib_adoption_allocation_counts.cpp` for an untimed build.
The counter window covers the loop, including every error's cleanup, and ends
before formatting. It observes GTI's process allocation provider and legacy
aligned allocation boundary, and checks each release's identity.
