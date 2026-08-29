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
| `conversion_matrix.py` | source numeric type × target numeric type | S12 — implicit integer→float conversions that lose value |
| `semantics_oracle.py` | stdlib and control-flow results × expected values | nothing — 11/12, the twelfth an API mistake in the test |
| `lsp_survival.py` | files × `gti_lsp` staying alive and answering | that S13's memory exhaustion is reachable through the editor |

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
