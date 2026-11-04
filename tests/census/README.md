# Example census baselines

One file per shipped example, `<example>.json`, records the reviewed MIR body
count (`mir`) and the LoweredProgram inventory (`lowered`: bodies, generated
items and generated items by kind) for both target profiles
(`arm64-apple-macos`, `x86_64-unknown-linux`).

`mir_census_regression` and `lowered_program_corpus_census` fail when an
example's count leaves the census budget (`tests/census_budget.py`: the larger
of five counts or five percent). `--update` re-records only the examples
outside the budget; `--refresh-all` re-records every example, including ones
added since the last refresh, which until then are reported as unrecorded.
`tests/census_store.py` reads and writes these files. One file per example
means two pull requests conflict here only when both change the same example.
