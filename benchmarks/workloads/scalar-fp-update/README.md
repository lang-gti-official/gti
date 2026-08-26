# Scalar floating-point update

This strict floating-point control updates three scalar fields and calls one
closed read-only method. Every variant uses the runner's `-fno-fast-math` and
`-ffp-contract=off` policy. The semantic C++ variant additionally mirrors the
checked integer induction step; no variant reassociates floating-point work.

Expected record:

```text
GTI-BENCH-1 digest:00000000034ee502 work-units:60000000
```
