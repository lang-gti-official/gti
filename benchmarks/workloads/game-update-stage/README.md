# Game update stage

This is the initial O-GAME-01 scalar/container stage. It applies one fixed
update stream to 1,024 vector-backed entity records and hashes every final
field. Rendering, input, allocation after setup, timing, and reporting stay
outside the update. Later O-GAME-01 rows may add independently attributable
constituents without changing this baseline's meaning.

Expected record:

```text
GTI-BENCH-1 digest:0000000012e84ad4 work-units:61440000
```
