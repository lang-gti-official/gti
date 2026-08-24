# Known-failure reproducers

Minimal reproducers for confirmed open compiler bugs, captured by the
`Q-FUZZ` debug tooling (`scripts/debug/`, `fuzz/`). Each file is the smallest
input that still triggers its bug.

**These are intentionally NOT registered in CMake.** They currently crash or
raise an internal error, so wiring them into the suite would break the build.
When a bug is fixed, move its reproducer into the owning test suite as a
passing regression and delete it here.

| File | Issue | Symptom |
| --- | --- | --- |
| `issue-92-nested-readonly-loans.gti` | [#92](https://github.com/LukeHHA/gti/issues/92) | nested read-only range-for over one container → `GTI-B0001` (MIR loan verifier) |
| `issue-93-parser-deep-nesting.gti` | [#93](https://github.com/LukeHHA/gti/issues/93) | deeply nested `(` → parser stack overflow, crashes `gti` |

## Checking status

`check.py` compiles each reproducer with a given `gti` and reports whether it
still fails. It is a helper, not a CTest — a green line means the bug is fixed
and the reproducer should graduate.

```sh
python3 tests/known_failures/check.py build/gti
```
