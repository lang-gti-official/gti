# Combination matrices

Each script sweeps the product of two language axes and classifies every cell
as ok / ICE / invalid-C++ / link error / silent / diagnostic code. They exist
because most of the defects in `../FINDINGS.md` were only visible when two
ordinary constructs were combined — no single cell is exotic, and each axis
alone is well covered by the feature-owned tests.

All take the compiler path as their first argument:

```bash
tests/coverage/matrices/declaration_matrix.py build/gti
tests/coverage/matrices/lsp_survival.py build/gti_lsp file.gti [more.gti ...]
```

`lsp_survival.py` takes the server path and then the files to open. **Do not
point it at `../stress/namespace-depth-63.gti` without an external memory cap** —
the server consumes memory without bound on that input.

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
| `llvm_wrongcode_matrix.py` | owned value kind × transfer × class shape, plus container, control-flow, hierarchy, concrete-base, payload-enum, erased-callable, destruction-order and lifecycle-dispatch sweeps on LLVM | at `b69828811`: an owned base-initializer argument left the base unconstructed on LLVM — silent wrong code when moved in, `Derived(T v) : Base(std::move(v))` (35 cells, every owned kind, plain and generic bases, one defect), and an internal error when created in place, `Base(make())`. At `bcf538640` both are refused as `UnstagedBaseInitializer` (GTI-B0003) and all 528 cells are clean: 378 run correctly on LLVM, 96 are refused with GTI-B0003, 54 are rejected by semantics |

`llvm_wrongcode_matrix.py` differs from the others: its programs check their
own results against values derived from the literal inputs and count tracked
destructions, so an LLVM cell needs no C++ oracle. That matters where the C++
backend cannot compile a shape at all, which is where its silent miscompiles
hid. It builds each cell on LLVM at O0 and O2 and on C++ at O0, runs them under
a watchdog, and exits non-zero when an LLVM cell computes the wrong answer.
The `order` and `dispatch` sweeps compare sequences, not counts: each
destructor body and tracked field reports a code, and the program checks the
order that `docs/language/execution.md` specifies. At `bcf538640`, 55 cells run
correctly on LLVM where C++ does not. In 45 of them C++ builds and dispatches
to the most-derived override during base destruction, the known C++ backend
defect; the other 10 are refused by the C++ backend with GTI-B0001.

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
