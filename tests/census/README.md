# Example census baselines

One file per shipped example, `<example>.json`, records the reviewed MIR body
count (`mir`) and the LoweredProgram inventory (`lowered`: bodies, generated
items and generated items by kind) for both target profiles
(`arm64-apple-macos`, `x86_64-unknown-linux`).

The default `mir_census_regression` uses sealed LoweredProgram bodies, so new
LLVM features do not depend on the deprecated C++ emitter. The corpus oracle
executes every example on LLVM O0/O3. The hand-dispatched C++ profile (only in
a build with `GTI_ENABLE_CPP_BACKEND`) retains its emitted-text inventory and
single C++23/O3 execution mode.

The census checks report counts outside their budget as advisory findings
(strict advisory mode makes them failures). An example that cannot compile
still fails. The budget is the larger of five counts or five percent
(`tests/census_budget.py`). `--update` re-records only the examples
outside the budget; `--refresh-all` re-records every example, including ones
added since the last refresh, which until then are reported as unrecorded.
`tests/census_store.py` reads and writes these files. One file per example
means two pull requests conflict here only when both change the same example.
