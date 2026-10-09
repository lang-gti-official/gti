# String growth and search

This promotes the runtime-audit `strings` screening input to a reproducible
container-migration workload. Each of 400 rounds builds a fresh 50,000-byte
ASCII string by `push_back`, searches for every `q`, and accumulates its positions
and the final string length. One work unit is one appended byte: 20,000,000 in
total. No capacity is reserved: allocation, growth, relocation, the trailing NUL,
search and destruction all contribute to the measured execution.

The three variants perform the same work. GTI uses checked ordinary library
APIs. The semantic C++ twin explicitly checks signed arithmetic, unsigned
addition, index conversion and string-view indexing; the idiomatic twin lets
native optimization use the fixed input bounds. Both use the native string's
growth policy, which may include small-string storage. They are success-domain
comparisons, not implementations of GTI's defined-failure ABI. All inputs remain
in range: the signed pattern expression is at most 350,392, and the final
checksum is 19,250,373,078. No comparison depends on undefined behavior.

The checked-in result is independently derived by summing `i` for every
`(i * 7 + round) % 26 == 16`, then adding 50,000 for each round:

```text
GTI-BENCH-1 digest:000000047b6961d6 work-units:20000000
```

Define `FULL_STATE` in all variants to additionally print every round's entire
string and every found position, before the result record. Compare these outputs
byte-for-byte outside the timing window; the checksum alone cannot establish
whole-string equality. Do not time the instrumented full-state variants.

Use `scripts/benchmark_compare.py --workload string-growth-search` with the
same native compiler and flags, five warmups and at least twenty interleaved
samples, without concurrent compilation. Its `--smoke` mode checks only builds
and the record. Compare the candidate with the preserved private-prefix GTI
toolchain and its matching stdlib/runtime as well as both C++ twins. Preserve
all source/tool identities, emitted C++, MIR, disassembly, code size and peak
memory. In a separate GTI build, define `ALLOCATION_COUNTS` and link
`tests/fixtures/stdlib_adoption_allocation_counts.cpp` to observe both the old
aligned-allocation and new process-provider boundaries. Its window includes
the complete 400-round loop and ends before result formatting; all temporary
strings have been destroyed. It reports allocation requests and bytes, and
the helper checks exact release identity. Do not combine it with `FULL_STATE`
or time either instrumented executable. No performance acceptance is implied
by adding this workload or by passing its smoke check.
