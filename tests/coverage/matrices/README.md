# Combination matrices

Each script sweeps the product of two language axes and gives every cell one
verdict. They exist because most of the defects in `../FINDINGS.md` were only
visible when two ordinary constructs were combined — no single cell is
exotic, and each axis alone is well covered by the feature-owned tests.

All of them build with the LLVM backend, GTI's only tested backend, through
one shared runner, `matrix_support.py`. It owns the command line, the
watchdog, the verdicts and the report, so the matrices differ only in the
programs they generate:

```bash
tests/coverage/matrices/declaration_matrix.py build-release/gti
tests/coverage/matrices/value_shape_matrix.py build-release/gti --levels O0,O2,O3
tests/coverage/matrices/llvm_wrongcode_matrix.py build-release/gti --matrix order raii \
    --json /tmp/wrongcode-new.json --baseline /tmp/wrongcode-old.json
tests/coverage/matrices/lsp_survival.py build-release/gti_lsp file.gti [more.gti ...]
```

- The compiler defaults to `build-release/gti`. `--levels` takes a
  comma-separated subset of O0–O3: O0 for the classification matrices, O0,O2
  for the wrong-code matrix. At more than one level, a cell whose levels
  disagree is a `DIVERGE(...)` verdict listing each level's result. That
  needs no oracle: a program one level runs and another refuses or runs
  differently is a defect whichever level is right.
- Cells are written to a fresh temporary directory, or `--out`, which must be
  new or empty. Concurrent runs never share files; only the sources are kept.
- `--json` writes each cell's per-level verdicts; `--baseline` takes an
  earlier `--json` report and lists every cell whose verdict changed. Use the
  pair to compare a compiler change against its parent.
- A run exits nonzero when any cell has a hard verdict, so a matrix can be
  scripted as a check.
- `--compiler-flag FLAG` (repeatable) adds a flag to every compile, such as
  `--compiler-flag=-g` to check that debug information changes nothing. The
  flags are recorded in the `--json` report.

The verdicts (`matrix_support.py` has the exact rules):

| Verdict | Meaning | Hard |
| --- | --- | --- |
| `ok` | built and, where the matrix runs it, exited 0 with the expected output | |
| `ICE` | internal compiler error, `GTI-B0001` | yes |
| `LLVM-VERIFY` | the backend produced a module the LLVM verifier rejects | yes |
| `UNSUPPORTED[shape O-LLVM-nn]` | the LLVM backend declined a declared shape (`GTI-B0003`) | |
| `LINK` | the native linker reported undefined symbols | yes |
| `NATIVE` | another native toolchain error with no GTI diagnostic | yes |
| `CRASH(SIG…)` | the compiler died on a signal | yes |
| `SILENT` | nonzero compiler exit with no output | yes |
| `TIMEOUT` | the compiler exceeded its time limit | yes |
| `RUN(…)` | the program crashed, hung or exited nonzero | yes |
| `WRONG(…)` | the program printed something other than expected | yes |
| `S2067`, `P0001`, … | an ordinary GTI diagnostic | |
| `DIVERGE(…)` | the selected levels disagree | yes |
| `ACCEPTED(expected rejection)` | a program that must be rejected built | yes |
| `WRONG-REJECTION(code)` | a program that must be rejected was, but not with the expected kind of diagnostic | read by hand |

`lsp_survival.py` takes the server path and then the files to open. **Do not
point it at `../stress/namespace-depth-63.gti` without an external memory cap** —
the server consumes memory without bound on that input.

The "Found" column records what each matrix found when it was written, mostly
on the C++ backend that was then the default; the defect IDs are in
`../FINDINGS.md`.

| Script | Axes | Found |
| --- | --- | --- |
| `declaration_matrix.py` | declaration form × parameter shape | S04 — 14 ICE cells, every bodyless declaration with an unnamed parameter |
| `value_shape_matrix.py` | value producer × consumer | S01, S02, S03 |
| `wrapper_context_matrix.py` | syntactic wrapper × context | S10, and the scope of S01 |
| `class_kind_matrix.py` | class kind × operation | the full extent of the `std::string`-field link failure |
| `generic_shape_matrix.py` | generic shape × type argument | S11 |
| `ownership_flow_matrix.py` | ownership operation × control-flow context | nothing — 60/60 clean |
| `elementary_forms_matrix.py` | elementary declaration and statement forms | S09, and confirmation that its 32 siblings are correct |
| `conversion_matrix.py` | source numeric type × target numeric type | S12 contract audit — accepted integer→float rounding in assignment contexts |
| `semantics_oracle.py` | stdlib and control-flow results × expected values | nothing — 11/12, the twelfth an API mistake in the test |
| `lsp_survival.py` | files × `gti_lsp` staying alive and answering | that S13's memory exhaustion is reachable through the editor |
| `payload_enum_matrix.py` | payload kind × operation | S15 — a class payload with no destructor |
| `concurrency_matrix.py` | threading construct × execution profile | nothing — 10/10 under `concurrent`, correctly rejected under the default |
| `llvm_wrongcode_matrix.py` | owned value kind × transfer × class shape, plus container, control-flow, hierarchy, concrete-base, payload-enum, erased-callable, destruction-order, lifecycle-dispatch and standard-library-container sweeps on LLVM | at `b69828811`: an owned base-initializer argument left the base unconstructed on LLVM — silent wrong code when moved in, `Derived(T v) : Base(std::move(v))` (35 cells, every owned kind, plain and generic bases, one defect), and an internal error when created in place, `Base(make())`. At `bcf538640` both are refused as `UnstagedBaseInitializer` (GTI-B0003) and all 534 cells are clean: 384 run correctly on LLVM, 96 are refused with GTI-B0003, 54 are rejected by semantics. Parity mode at `c0547e544` adds idiomatic sweeps (polymorphic-owner containers, erased callbacks, optional, expected and payload enums, relayed views and references, generics, algorithms, threads and cleanup order under control flow): 809 cells, 0 LLVM wrong code, 48 cells that C++ runs and LLVM refuses. 28 are the known `PolymorphicTransfer` (O-LLVM-10) closure and thread captures; 20 return a view, span or reference produced by a virtual or interface call out of another function (O-LLVM-05). A second round adds user operators, user iterators and ranges, nested generics, formatting with user types, fixed arrays and struct-of-arrays, owned recursive trees, expected and optional chains, and static and constexpr state: 55 cells, 0 LLVM wrong code, 0 parity gaps. Three cells reach an internal error on both backends (a range-for over a field reached through `unique_ptr`'s `->`, twice, and a range-for over a by-reference capture inside a lambda); two run on LLVM but not C++ (returning `std::array` of a class, moving fixed-array elements by constant index) |
| `llvm_wrongcode_matrix.py` RFC 0013 modules | `weak_owners` (holder × operation plus cycles, 42 cells), `thread_local_owners` (storage × payload × thread, 33), `deferred` (guard kind × exit, 28), `propagation` (carrier × position, 49), `panics` (failure source × cleanup context, 59), `payload_binding` (22), `owning_enum_transport` (enum shape × transport, 60), `borrowed_elements` (container × access, 60), `conversions` (18), `allocation` (13), `lifetimes` (10 valid, 8 invalid siblings) | at `9bae45998`, O0–O3: [#687](https://github.com/lang-gti-official/gti/issues/687) — an ICE for a generic factory returning a generic class holding a by-reference closure (`deferred/borrowing_capture`); [#689](https://github.com/lang-gti-official/gti/issues/689) — virtual conversion operators rejected although `static-semantics.md` 3.9.2 admits them (`conversions/virtual_operator_declared`). Every other cell runs or is rejected as documented at every level, with no level divergence |

`llvm_wrongcode_matrix.py` differs from the others: its programs check their
own results against values derived from the literal inputs and count tracked
destructions, so a cell needs no second backend as an oracle. The `order` and
`dispatch` sweeps compare sequences, not counts: each destructor body and
tracked field reports a code, and the program checks the order that
`docs/language/execution.md` specifies. Each cell is built with
`--execution-profile concurrent` at O0 and O2 (or `--levels`) and run under a
watchdog; a cell that prints anything but `ok`, a level divergence, an
internal error or another hard verdict fails the run. A refused cell prints
its LLVM refusal shape, such as `UNSUPPORTED[Shape.PolymorphicTransfer
O-LLVM-10]`; those are declared limitations, counted but not failures.

Until the C++ backend was frozen, the matrix also built each cell on C++ at O0,
and a `--parity` mode reported cells that C++ ran correctly but LLVM did not.
Both are gone: the C++ backend is no longer tested and is not an oracle for
GTI semantics. At `bcf538640`, 55 cells ran correctly on LLVM where C++ did
not; in 45 of them C++ dispatched to the most-derived override during base
destruction.

At `ec726745e` (0.494.0, macOS arm64, LLVM 23.1.2) all 864 cells are clean
at O0 and O2: 810 run correctly, 54 are rejected by semantics, none is
declined by the LLVM backend (the 96 `GTI-B0003` refusals recorded at
`bcf538640` are all admitted now), and no cell diverges between levels. The
classification matrices at O0,O2 are clean too, apart from one level
divergence: `elementary_forms_matrix.py`'s `comma-expr` builds at O0 and
reaches an internal error at O1 and above
([#677](https://github.com/lang-gti-official/gti/issues/677)).

The RFC 0013 modules (slices 2 and 3) extend the cell format: a cell may
carry an expectation instead of plain `ok` — an exact stdout trace (each
tracked object prints a code when destroyed), an exit status with a report
substring on standard error (status 70 and `runtime error[GTI-Rnnnn]` for a
defined failure), or the diagnostic code an invalid sibling must be rejected
with (an accepted invalid sibling is `WRONG`). Every module states its
contract and excluded shapes in its docstring and carries a
`NEGATIVE_CONTROL`, a one-line mutation of one cell that the oracle must
flag; `--negative-controls` builds them all:

```sh
tests/coverage/matrices/llvm_wrongcode_matrix.py build-release/gti \
    --negative-controls --levels O0 --jobs 2 --matrix weak_owners panics ...
```

At `9bae45998` (macOS arm64) the 402 cells of the eleven modules were run at
O0, O1, O2 and O3: two cells fail at every level, both filed (#687, #689),
none diverges between levels, and all eleven negative controls are flagged.

## RFC 0013 oracle matrices

These apply the oracles of RFC 0013 (compiler bug-finding coverage) that need
no expected values beyond the program itself. All run locally only; none is
scheduled.

| Script | Oracle | Result at `9bae45998` |
| --- | --- | --- |
| `constexpr_equivalence_matrix.py` | an expression evaluated in a constant context and at run time (behind an opaque value) gives the same result, and fails in both or neither: `--family arith` random integer trees for 32/64-bit types, `small` for 8/16-bit types (with promotion to `int32_t`, [#694](https://github.com/lang-gti-official/gti/issues/694)), `shapes` for loops, recursion, arrays, records, enums, `expected`, references, text, floating point and generics | 600 + 105 seeded expressions and every shape: no disagreement |
| `module_split_matrix.py` | each self-checking `llvm_wrongcode_matrix.py` cell, moved into one module, behind a facade, or split across a re-exporting chain, still prints `ok` | 2,586 programs at O0,O2: no hard failure; the 54 declined cells are exactly the single-file semantic rejections. The 447 cells added by #688 split into 1,341 programs: the `chain` split of `owning_enum_transport/nested_thread_local_array` reaches an internal error that a class in the same unit hides ([#699](https://github.com/lang-gti-official/gti/issues/699)); `deferred/borrowing_capture` fails as it does in one file (#687); 23 `propagation` chains are correctly refused by the conformance orphan rule (GTI-S2123), because the split separates a `std::from` conformance from its types |
| `invalid_sibling_matrix.py` | 15 ownership and borrow violations, each with a valid twin, in 7 contexts (main, branch, loop, switch, lambda, generic, method): the twin builds and runs, the sibling is rejected with a GTI-S diagnostic | 210/210 correct |
| `ui_level_agreement.py` | every single-file passing `tests/ui` program builds, and runs alike, at O0–O3 | 3,063 tests: [#686](https://github.com/lang-gti-official/gti/issues/686) and [#691](https://github.com/lang-gti-official/gti/issues/691) (internal errors at O1 and above); the other 15 disagreements are `raw_context` fixtures that call an undefined native function only reachable at O0 |

`llvm_wrongcode_matrix.py --compiler-flag=-g` (864 cells) found no wrong code;
19 cells are refused only under `-g`
([#690](https://github.com/lang-gti-official/gti/issues/690)); the cells added
by #688 add nine more, all `std::span`.

While another session's verification holds the suite slot, run it at
`--jobs 2` or less, one matrix at a time; run larger passes through
`python3 scripts/build_window.py --kind suite -- ...`.

## Reading the output

A cell that is not `ok` is not automatically a defect — some are correct
rejections, and the scripts print the diagnostic code so those can be read off.
What matters is the *shape*:

- A **row or column failing uniformly** usually means one defect, not many.
  `value_shape_matrix.py` showed 25 of 64 cells failing, which collapsed to
  three bugs once the producers were unwrapped.
- A **single cell failing in an otherwise clean row** is the interesting case.
  S10 was found that way: one column where an *unwrapped* expression failed.
- A **clean matrix is a result worth recording.** `ownership_flow_matrix.py`
  passing 60/60 is evidence that moves, borrows and drops agree with branching.
- **Check whether the harness itself caused the failure.** The payload-enum
  sweep showed two rows failing uniformly with `GTI-S2067`, which turned out to
  come from a `produce()` helper the generator put in every file — a documented
  limitation, correctly diagnosed, and not a defect. Verify a cell in isolation
  before believing it.
