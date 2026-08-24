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
| `issue-103-lambda-capture-field.gti` | [#103](https://github.com/LukeHHA/gti/issues/103) | reading a field of a captured class inside a lambda → `GTI-B0001` (MIR verifier) |
| `issue-104-global-array-index.gti` | [#104](https://github.com/LukeHHA/gti/issues/104) | indexing a global array → `GTI-B0001` (the #100 fix took `Field` chains, not `Index`) |
| `issue-106-array-of-class.gti` | [#106](https://github.com/LukeHHA/gti/issues/106) | *declaring* a fixed array of any class type → `GTI-B0001`; no read needed |
| `issue-113-virtual-through-base-reference.gti` | [#113](https://github.com/LukeHHA/gti/issues/113) | a virtual call through a reference to a plain base *class* → `GTI-B0001`; the same shape through an `interface` works |
| `issue-115-global-array-write.gti` | [#115](https://github.com/LukeHHA/gti/issues/115) | *writing* a global or static array element → `GTI-B0001` at a different gate than #104's read |
| `issue-116-uninstantiated-generic-destructor.gti` | [#116](https://github.com/LukeHHA/gti/issues/116) | a generic class with a destructor that is never instantiated → `GTI-B0001`; instantiating it once fixes it |

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
| `asan/issue-107-assign-in-and.gti` | [#107](https://github.com/LukeHHA/gti/issues/107) | an assignment as an operand of `and` → heap-use-after-free in `SemanticVisitor::visitAssignExpr` |

## Wrongly rejected programs

`rejected/` holds valid programs the compiler refuses. There is no crash and
no ICE — the defect is the *rule*, so `check.py` cannot classify these either
(it sees an ordinary non-zero exit). Compile one and read the diagnostic.

| File | Issue | Symptom |
| --- | --- | --- |
| `rejected/issue-108-nested-capture.gti` | [#108](https://github.com/LukeHHA/gti/issues/108) | a nested lambda cannot capture the enclosing lambda's capture → `GTI-S2027`, though capturing the outer lambda's local or parameter is fine |
| `rejected/issue-109-payload-class-diagnostic.gti` | [#109](https://github.com/LukeHHA/gti/issues/109) | the rejection is correct, but `GTI-S2067` prints `'unknown class'` instead of naming the payload field's type |
| `rejected/issue-110-self-nested-generic.gti` | [#110](https://github.com/LukeHHA/gti/issues/110) | a generic inside its own type arguments yields two distinct specializations → `GTI-S2003` naming the same type on both sides |
| `rejected/issue-114-interface-argument.gti` | [#114](https://github.com/LukeHHA/gti/issues/114) | passing a concrete type where an interface reference is required → `GTI-S2003`, though binding it to a reference one line earlier is accepted |

## Confirmed root causes

Each was located by instrumenting the rejecting gate with
`scripts/debug/explain_rejection.py`, not inferred from reading the source.
They share the `GTI-B0001` code but are four independent defects in two
subsystems.

Anchors are symbol names and diagnostic strings because **line numbers drift**
— grep the anchor, or re-run the tool, rather than trusting the parenthesised
line.

| Issue | Subsystem | Gate |
| --- | --- | --- |
| #101 | C++ emitter | `cpp_mir_body_emitter.cpp`, `cppMirOwnedParameterFieldBindings` — no move/drop proof for a constructor taking owned parameters into fields |
| #103 | MIR verifier | `mir.cpp`, `"lambda capture place does not match its exact environment field"` (~15694) — `place.type` is the *projected field's* type but is compared against `captureTypes[...]`, the whole capture's type, so any field projection mismatches |
| #104 | C++ emitter | `cpp_mir_body_emitter.cpp`, `fieldStorage` (~20676) — a symbol-rooted place admits a chain of `Field` projections but not `Index` |
| #106 | C++ emitter | `cpp_mir_body_emitter.cpp`, `MissingAggregateRollbackMir` (~9707) — an `Aggregate` whose element drop is non-trivial is admitted only by `failureFreeFixedArrayAggregateMove` or `passiveFixedArrayConstructAggregate`, and default-initializing an array of class elements matches neither |

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

| File | Issue | Symptom |
| --- | --- | --- |
| `wrong-code/issue-111-parenthesised-return.gti` | [#111](https://github.com/LukeHHA/gti/issues/111) | `return (local);` returns 0 — the parenthesised form allocates a second MIR place and reads it unwritten |
| `wrong-code/issue-111-parenthesised-condition.gti` | [#111](https://github.com/LukeHHA/gti/issues/111) | `if ((flag))` reads the same unwritten place, so **the wrong branch runs** |
| `wrong-code/issue-111-comma-operand.gti` | [#111](https://github.com/LukeHHA/gti/issues/111) | `(y, x)` — the comma's last operand takes the same clone path |

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
and that a write through an aliasing mutable reference is observed across a
loop.

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
and some fixtures are negative tests — **except** when the failure is a
`GTI-B0001`, which is reported as `ICE`. Folding that into a skip count is how
[#112](https://github.com/LukeHHA/gti/issues/112) nearly went unnoticed: two
repository fixtures raise an internal error that the suite expects, and a bare
"5 skipped" said nothing about it.

Those two would then be reported on every run, so the tool reads
`tests/stdlib_surface_manifest.json` and classifies a case whose expected
stderr already contains `GTI-B0001` as `expected-ICE`. The manifest stays the
single source of truth rather than a list hardcoded here, and when #112 is
fixed those entries stop matching on their own.

## Sweeping the standard library by include

`scripts/debug/stdlib_include_sweep.py` compiles each module under
`stdlib/std/` on its own — one `#include` and an empty `main` — so a failure
is attributable to the header alone.

```sh
python3 scripts/debug/stdlib_include_sweep.py
```

That shape had never been tested. Every case in the surface manifest *uses*
the module it covers, and a header that declares a generic class with a
destructor cannot be compiled unless something instantiates it — the
destructor is lowered with no concrete MIR instance and the compile raises
`GTI-B0001`. Using the module makes it go away, which is why the surface cases
pass while the header alone does not.

That reduces to eight lines with no standard library at all, which is how
[#116](https://github.com/LukeHHA/gti/issues/116) is now filed; the stdlib
headers were only the first place it surfaced. Today the sweep reports
`19 module(s): 16 ok, 3 failing`.

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
