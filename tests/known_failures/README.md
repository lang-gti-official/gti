# Known-failure reproducers

Minimal reproducers for confirmed **open** compiler bugs, captured by the
`Q-FUZZ` debug tooling (`scripts/debug/`, `fuzz/`). Each file is the smallest
input that still triggers its bug.

**These are intentionally NOT registered in CMake.** They crash the compiler or
raise an internal error, so wiring them into the suite would break the build.
When a bug is fixed, move its reproducer into the owning test suite as a
passing regression, delete it here, and add a row to *Graduated* below.

## Compiler reproducers

These are the shapes left broken by an otherwise-landed fix — see
[#117](https://github.com/LukeHHA/gti/issues/117). Each has a passing sibling
that the same fix already handles, which is what makes them worth keeping.

Every closed issue from this branch has now been probed around, not just the
ones where a gap surfaced by accident. Six fixes covered their family — #109,
#111, #112, #113, #114 and #116 — and the six below did not. The fixes that
held are largely the ones whose issues enumerated the family explicitly, which
is a lesson about how the issues were written as much as how they were fixed.

| File | Left over from | Symptom |
| --- | --- | --- |
| `issue-117-iife-capture-field.gti` | [#103](https://github.com/LukeHHA/gti/issues/103) | a captured field read in an *immediately-invoked* lambda; binding the lambda first works |
| `issue-117-nested-capture-emission.gti` | [#108](https://github.com/LukeHHA/gti/issues/108) | capture-of-capture is now accepted by semantics but cannot be emitted, so `GTI-S2027` became `GTI-B0001` |
| `issue-117-indirect-self-nested-generic.gti` | [#110](https://github.com/LukeHHA/gti/issues/110) | a generic reappearing in its own arguments at depth 2; the direct form is fixed |
| `issue-117-global-element-field-write.gti` | [#115](https://github.com/LukeHHA/gti/issues/115) | `table[0].v = x` on a global array of class; the read and the plain write are fixed |
| `issue-117-array-of-cleanup-owning-class.gti` | [#106](https://github.com/LukeHHA/gti/issues/106) | an array of a class **with a destructor**; without one it compiles. Reports the same gate #106 was filed with |
| `issue-117-static-element-field-write.gti` | [#115](https://github.com/LukeHHA/gti/issues/115) | the same `[0].v = x` failure through a static class field rather than a global — one defect, two surfaces |

## Memory-safety reproducers

`asan/` holds inputs whose defect is invisible to `check.py`, because the
normal compiler exits with a correct diagnostic and only a sanitizer build
shows the fault. Run them through the ASan fuzz harness instead:

```sh
build-fuzz/fuzz/gti_pipeline_fuzzer -runs=1 \
  -artifact_prefix=/tmp/gti-artifacts/ tests/known_failures/asan/<file>.gti
```

`-artifact_prefix` matters: on a crash libFuzzer writes a `crash-<sha>` file
into the **current directory** by default, so reproducing one of these from
the repository root litters the tree.

| File | Issue | Symptom |
| --- | --- | --- |
| `asan/issue-117-loan-conflict-uaf.gti` | [#107](https://github.com/LukeHHA/gti/issues/107) | the #107 reproducer still reports a heap-use-after-free, now in `reportLoanPlaceConflict`; the freed `Symbol` and the scope-stack table are the same, so only the consumer changed |

## Wrongly rejected programs

`rejected/` holds valid programs the compiler refuses. There is no crash and
no ICE — the defect is the *rule*, so `check.py` cannot classify these either
(it sees an ordinary non-zero exit). Compile one and read the diagnostic.

There are currently no quarantined wrongly rejected programs.

## Confirmed root causes

Previous root causes were located by instrumenting the rejecting gate with
`scripts/debug/explain_rejection.py`, not inferred from reading the source.

Anchors are symbol names and diagnostic strings because **line numbers drift**
— grep the anchor, or re-run the tool, rather than trusting the parenthesised
line.

The previously confirmed root causes are retained in the graduated regression
tests below rather than duplicated here.

## Finding the gate for a new GTI-B0001

`scripts/debug/explain_rejection.py` automates what located the graduated root
causes below: it temporarily instruments the rejecting subsystem,
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

## Sweeping for unspellable places

Most backend ICEs here are the same kind of defect: a *place* — a storage root
plus a chain of projections — that MIR verifies but the emitter cannot spell.
The holes cluster, and fixing one pair routinely leaves its neighbours broken
(#104 is precisely the neighbour #100's fix missed).

`scripts/debug/place_shape_matrix.py` enumerates that space directly, crossing
each storage root (local, global, parameter, lambda capture, `this`, raw
pointer, static field) with each projection chain (none, `.field`, `.field.field`, `[i]`,
`[i].field`, deref, arrow) in both read and write position, then compiling one
minimal program per cell. Enum, payload-enum and `expected<T, E>` cells cover
the aggregate-shaped neighbours of the fixed array that produced #106, and
generic cells cover instantiation identity, which is not a place problem but
fails the same way — a program the frontend should accept and does not.

```sh
python3 scripts/debug/place_shape_matrix.py
python3 scripts/debug/place_shape_matrix.py --only capture --write-dir /tmp/cells
```

It exits non-zero listing every `GTI-B0001` cell. It found #106, and its cells
are what establish the *scope* claims in #103, #104 and #106 — that writes fail
exactly where reads do, that `this` and parameter roots are unaffected, and
that an unprojected capture is fine while any projected one is not.

Cells that already pass are kept deliberately. Most of the grid is green, and
that is the point: a green cell turning red is the cheapest possible signal
that a backend change broke a shape nobody thought to test. `error` cells are
kept for the same reason — #109 was found by reading one, where the rejection
was right but its message could not name the type it rejected.

## Wrong-code reproducers

`wrong-code/` holds programs that compile cleanly and then compute the wrong
answer. These are the most serious entries here and the least visible: there
is no crash, no ICE, and no diagnostic, so `check.py` sees an ordinary
successful compile. Build one and compare its exit status against the comment
at the top of the file.

There are currently no quarantined wrong-code reproducers.

`wrong-code/check.py` builds and runs each one, comparing its exit status
against the `// expected exit status: N` header on its first line:

```sh
python3 tests/known_failures/wrong-code/check.py build/gti
```

## Differential-testing the optimizer

`scripts/debug/optimizer_differential.py` builds each generated program at
every optimization level and requires the exit statuses to agree with each
other *and* with the value the model computes in Python.

```sh
python3 scripts/debug/optimizer_differential.py --count 24
python3 scripts/debug/optimizer_differential.py --levels O0 O2
python3 scripts/debug/optimizer_differential.py --count 0   # invariants only
```

Alongside the generated corpus it runs hand-written invariants from
`scripts/debug/optimizer_invariants/`, each declaring its expected exit status
in a first-line comment. The generated programs are int32/bool arithmetic and
cannot express the properties an optimizer is most likely to break, so those
are written by hand: that a dynamically out-of-range index still produces its
defined runtime failure, that an in-range one still reads what was written,
that a write through an aliasing mutable reference is observed across a loop,
that `and`/`or` skip the operand they are supposed to skip, that destructors
run in reverse construction order, and that signed overflow through `+`
remains a defined failure rather than folding to a wrap.

Those last three are specified runtime behaviour with no other guard across
levels. `tests/evaluation_order_runtime_test.py` already covers strict
left-to-right evaluation at O0 and O3, so that one is deliberately not
duplicated here, and `tests/defined_integer_runtime_test.py` covers
`std::checked_add` — the library form — rather than the operator.

Two reasons it earns its place next to `local_language_audit.py`, which
already builds at `O0` and `O3`:

- `-O1` and `-O2` had no behavioural differential at all. The pipeline fuzz
  harness exercises `O2`, but for crashes, not for answers.
- An independent oracle beats a pairwise one. Comparing levels against each
  other finds divergence but not a fault they all share; comparing against
  `Program.evaluate()` — pure Python, never touching the compiler — finds
  that too. Failures are reported as `divergent` or `wrong` accordingly, and
  the two point at very different bugs.

#111 came from the second kind, via the model's metamorphic pair: the same
program with and without redundant parentheses must return the same value, and
did not.

## Differential-testing over the repository's own programs

`scripts/debug/corpus_differential.py` trades the model oracle for a much
richer corpus: it builds every program in `examples/` — and with `--fixtures`
every one in `tests/fixtures/` — at each optimization level and requires
identical behaviour.

```sh
python3 scripts/debug/corpus_differential.py
python3 scripts/debug/corpus_differential.py --fixtures --jobs 4
```

No expected output is needed, which is the point: optimization must not change
observable behaviour, so a disagreement between levels is a bug regardless of
which level is right. That also states its blind spot exactly — a fault every
level shares is invisible here, which is what `optimizer_differential.py`'s
model oracle is for. The two are complementary: one has the oracle and a
narrow corpus, the other has the corpus and no oracle.

A program that disagrees with *itself* across two runs at the baseline level
is reported as `nondet` and excluded, so nondeterminism is not blamed on the
optimizer.

A program that fails to build is `skipped` — some want a package or a target,
and some fixtures are negative tests — except that `GTI-B0001` is reported as
`ICE` and `GTI-B0003` as `unsupported`. Folding internal errors into a skip
count is how [#112](https://github.com/LukeHHA/gti/issues/112) nearly went
unnoticed. A specifically classified, bounded backend capability gap may use
`GTI-B0003`; an unclassified rejection remains a compiler defect and therefore
stays visible as `ICE`.

## Sweeping the standard library by include

`scripts/debug/stdlib_include_sweep.py` compiles each module under
`stdlib/std/` on its own — one `#include` and an empty `main` — so a failure
is attributable to the header alone.

```sh
python3 scripts/debug/stdlib_include_sweep.py
```

That shape had never been tested. Every case in the surface manifest *uses*
the module it covers, while an unused generic declaration legitimately has no
concrete MIR lifecycle instance. [#116](https://github.com/LukeHHA/gti/issues/116)
made that absence an internal backend error. The sweep is now a primary CTest
gate and all 19 shipped modules independently reach C++ emission.

## Multi-file programs

Nothing in this directory or in `place_shape_matrix.py` is multi-file — every
cell is a single source. Probed by hand and currently clean:

- an include cycle is refused with `GTI-I0001` naming the offending file
- a diamond (`A` includes `B` and `C`, both include `D`) loads `D` once and
  compiles, as the grammar's "loaded once" note promises
- a generic instantiated in one unit satisfies the whole program, so #116
  does not fire when the use is in a different file from the declaration

Extending the matrix to emit multi-file cells would be a real change rather
than a new entry; it is not worth it while this surface is clean, but the gap
is worth knowing about.

## Probing the language server

`scripts/debug/lsp_probe.py` checks two properties of `gti_lsp` that an
ASCII-only, single-shot test cannot see.

```sh
python3 scripts/debug/lsp_probe.py
python3 scripts/debug/lsp_probe.py --check staleness
```

*Position encoding.* The server advertises `positionEncoding: utf-16`, so its
ranges are UTF-16 code units — which agree with bytes and codepoints for ASCII
and with neither elsewhere. Each case puts a different width of character
before a declaration on the same line and checks that slicing the line by the
returned range reproduces the declaration. A byte-counting server fails from
the Latin-1 case on; a codepoint-counting one fails only on the emoji case.

*Edit staleness.* The server reuses frontend source units across edits, so the
hazard is answering from a snapshot the user already edited away. The check
renames the only declaration through `didChange` and requires the old name to
disappear.

Both currently pass. The target is optional — configure with
`-DGTI_BUILD_LSP=ON` if `build/gti_lsp` is missing.

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
| [#103](https://github.com/LukeHHA/gti/issues/103) projected lambda capture | `v0.306.1` | `tests/mir_class_value_lifecycle_runtime_test.py` |
| [#104](https://github.com/LukeHHA/gti/issues/104) global fixed-array indexing | `v0.306.1` | `tests/fixtures/global_class_storage.gti`, `tests/global_class_storage_runtime_test.py` |
| [#106](https://github.com/LukeHHA/gti/issues/106) fixed arrays of class values | `v0.306.1` | `tests/fixtures/fixed_array_class_lifecycle.gti`, `tests/fixed_array_class_lifecycle_runtime_test.py` |
| [#107](https://github.com/LukeHHA/gti/issues/107) assignment in a logical operand | `v0.306.1` | `tests/compiler_tests.cpp` |
| [#108](https://github.com/LukeHHA/gti/issues/108) nested capture of an enclosing capture | `v0.306.1` | `tests/compiler_tests.cpp` |
| [#109](https://github.com/LukeHHA/gti/issues/109) payload-class diagnostic spelling | `v0.306.1` | `tests/sum_type_tests.cpp` |
| [#110](https://github.com/LukeHHA/gti/issues/110) self-nested generic identity | `v0.306.1` | `tests/returned_class_field_runtime_test.py` |
| [#111](https://github.com/LukeHHA/gti/issues/111) grouped binding and comma operands | `v0.306.1` | `tests/fixtures/mir_backend_scalar_cfg.gti`, `tests/mir_backend_scalar_cfg_runtime_test.py` |
| [#113](https://github.com/LukeHHA/gti/issues/113) virtual call through a base-class reference | `v0.306.1` | `tests/reference_parameter_range_runtime_test.py` |
| [#114](https://github.com/LukeHHA/gti/issues/114) concrete public-derived argument to a base/interface reference | `v0.306.1` | `tests/reference_parameter_range_runtime_test.py` |
| [#115](https://github.com/LukeHHA/gti/issues/115) global/static fixed-array member write | `v0.306.1` | `tests/fixtures/global_class_storage.gti`, `tests/global_class_storage_runtime_test.py` |
| [#116](https://github.com/LukeHHA/gti/issues/116) include-only generic destructor and optional empty state | `v0.306.1` | `cpp_mir_representation_snapshot`, `stdlib_include_sweep` |

The `formatter/` subdirectory held inputs that are not compiler source (so
`check.py`'s non-recursive glob skipped them); it is empty now that #96 is
fixed. Check any future formatter reproducer with the fuzz harness instead:

```sh
build-fuzz/fuzz/gti_formatter_fuzzer -runs=1 tests/known_failures/formatter/<file>.gti
```

## Running the fuzzers without polluting the tree

libFuzzer writes every coverage-increasing input into the **first** corpus
directory on its command line. Passing `fuzz/corpus` directly therefore drops
thousands of untracked files next to the 25 tracked seeds. Give it a scratch
directory first and the seed corpus second, so seeds are read and new inputs
land somewhere disposable:

```sh
build-fuzz/fuzz/gti_pipeline_fuzzer -max_total_time=300 /tmp/gti-corpus fuzz/corpus
```
