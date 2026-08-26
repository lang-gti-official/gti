# Checked scalar chain

This control keeps checked integer arithmetic in one scalar dependency chain,
where the native processor can normally absorb the successful branches without
losing a vectorization opportunity. The semantic C++ twin checks the same
operations in the same order. The idiomatic twin omits those checks only for
the fixed, proven-bounded input.

Expected record:

```text
GTI-BENCH-1 digest:000000002284c2c0 work-units:100000000
```
