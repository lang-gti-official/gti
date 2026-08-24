# Known-failure reproducers

Minimal reproducers for confirmed **open** compiler bugs, captured by the
`Q-FUZZ` debug tooling (`scripts/debug/`, `fuzz/`). Each file is the smallest
input that still triggers its bug.

**These are intentionally NOT registered in CMake.** They crash the compiler or
raise an internal error, so wiring them into the suite would break the build.
When a bug is fixed, move its reproducer into the owning test suite as a
passing regression, delete it here, and add a row to *Graduated* below.

## Compiler reproducers

| File | Issue | Symptom |
| --- | --- | --- |
| `issue-101-pair.gti` | [#101](https://github.com/LukeHHA/gti/issues/101) | `std::make_pair` → `GTI-B0001` in the stdlib body |
| `issue-101-forward-list.gti` | [#101](https://github.com/LukeHHA/gti/issues/101) | `forward_list::push_front` → `GTI-B0001` in its own body |
| `issue-103-lambda-capture-field.gti` | [#103](https://github.com/LukeHHA/gti/issues/103) | reading a field of a captured class inside a lambda → `GTI-B0001` (MIR verifier) |
| `issue-104-global-array-index.gti` | [#104](https://github.com/LukeHHA/gti/issues/104) | indexing a global array → `GTI-B0001` (the #100 fix took `Field` chains, not `Index`) |

## Confirmed root causes

Each was located by instrumenting the rejecting gate with
`scripts/debug/explain_rejection.py`, not inferred from reading the source.
They share the `GTI-B0001` code but are three independent defects in two
subsystems.

Anchors are symbol names and diagnostic strings because **line numbers drift**
— grep the anchor, or re-run the tool, rather than trusting the parenthesised
line.

| Issue | Subsystem | Gate |
| --- | --- | --- |
| #101 | C++ emitter | `cpp_mir_body_emitter.cpp`, `cppMirOwnedParameterFieldBindings` — no move/drop proof for a constructor taking owned parameters into fields |
| #103 | MIR verifier | `mir.cpp`, `"lambda capture place does not match its exact environment field"` (~15694) — `place.type` is the *projected field's* type but is compared against `captureTypes[...]`, the whole capture's type, so any field projection mismatches |
| #104 | C++ emitter | `cpp_mir_body_emitter.cpp`, `fieldStorage` (~20676) — a symbol-rooted place admits a chain of `Field` projections but not `Index` |

## Finding the gate for a new GTI-B0001

`scripts/debug/explain_rejection.py` automates what located every root cause
in the table above: it temporarily instruments the rejecting subsystem,
rebuilds, runs the reproducer, names the gates that fired with their guarding
comment, and always restores the source.

```sh
python3 scripts/debug/explain_rejection.py --source bad.gti              # emitter
python3 scripts/debug/explain_rejection.py --source bad.gti --gate verifier
```

Gates inside speculative `is it shape X?` lambdas are flagged `(probe)` and
are usually not the cause; a gate in the function body proper is. If no gate
fires, the rejection is in the other subsystem — try the other `--gate`.

Two caveats. The printed comment is just the nearest one above the gate, so
when a gate has no comment of its own it borrows an unrelated one — read the
code at the reported line before believing it. And `--gate verifier` prints the
compiler's own error text, which usually names the broken rule outright and is
often the better clue.

## Checking status

`check.py` compiles each compiler reproducer with a given `gti` and reports
whether it still fails. It is a helper, not a CTest — a green line means the
bug is fixed and the reproducer should graduate.

```sh
python3 tests/known_failures/check.py build/gti
```

## Graduated

Fixed upstream, with regression coverage now in the owning suites, so the
reproducers were removed from this directory.

| Issue | Fixed by | Regression coverage |
| --- | --- | --- |
| [#92](https://github.com/LukeHHA/gti/issues/92) nested read-only range-`for` | `5d26719` | `tests/compiler_tests.cpp` |
| [#93](https://github.com/LukeHHA/gti/issues/93) parser stack overflow | `5d26719` | `tests/compiler_tests.cpp` — deep nesting is now `GTI-P0005`, a diagnostic rather than a crash |
| [#95](https://github.com/LukeHHA/gti/issues/95) global with no `main` | `5d26719` | `tests/cpp_mir_representation_snapshot_tests.cpp` |
| [#96](https://github.com/LukeHHA/gti/issues/96) formatter not idempotent | `5d26719` | `tests/lsp_smoke_test.py` |
| [#100](https://github.com/LukeHHA/gti/issues/100) global class member access | `ca18fac` | `tests/fixtures/global_class_storage.gti`, `tests/global_class_storage_runtime_test.py` |

The `formatter/` subdirectory held inputs that are not compiler source (so
`check.py`'s non-recursive glob skipped them); it is empty now that #96 is
fixed. Check any future formatter reproducer with the fuzz harness instead:

```sh
build-fuzz/fuzz/gti_formatter_fuzzer -runs=1 tests/known_failures/formatter/<file>.gti
```
