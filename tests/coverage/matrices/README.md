# Combination matrices

Each script sweeps the product of two language axes and classifies every cell
as ok / ICE / invalid-C++ / link error / silent / diagnostic. They exist
because several defects were only visible when two ordinary constructs were
combined — no single cell is exotic, and each axis alone is well covered by
the feature-owned tests.

```bash
tests/coverage/matrices/declaration_matrix.py build/gti
tests/coverage/matrices/value_shape_matrix.py build/gti
```

| Script | Axes | Found |
| --- | --- | --- |
| `declaration_matrix.py` | declaration form × parameter shape | 14 ICE cells: every bodyless declaration with an unnamed parameter |
| `value_shape_matrix.py` | value producer × consumer | the parenthesis, ternary and `std::move` ICEs |

A cell that is not `ok` is not automatically a defect — some are correct
rejections, and the scripts print the diagnostic code so those can be read off.
What matters is the *shape* of the failures: a row or column that fails
uniformly usually means one defect, not many.
