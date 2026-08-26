# Optimizer-level control

This is the canonical fixed-native attribution workload. It mirrors the manual
Mandelbrot optimizer example, checks the complete checksum, and reports one
logical work unit per pixel. Run the same descriptor at GTI O0 through O3 with
`--native-optimization 3`; any runtime difference then comes from different
GTI MIR output rather than a changed native optimization level.

Expected record:

```text
GTI-BENCH-1 digest:000000025efeed0b work-units:700000
```
