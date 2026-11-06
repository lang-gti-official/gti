Build and run the application with `gti run app --release` in this directory.
The driver builds the shared `base` dependency, then `left` and `right`, then
links `app` with each object's dependency closure exactly once. Repeating the
build reuses libraries whose complete source, compiler configuration, and direct
dependency digests still match. Editing `base.gti` rebuilds its dependents.

Each build keeps the exact compiled bytes selected for `base`, `left`, and
`right`. Both paths through the diamond therefore use the same selected `base`,
even if another process replaces its cached artifact before `app` is compiled.
The next `gti run app --release` selects dependencies afresh. This protects
artifact selection; source files are still checked for freshness, and the build
does not claim an atomic snapshot of the entire project directory.

`left_tick()` and `right_tick()` access the same private namespace counter in
`base`. Alternating calls check that linking the diamond does not duplicate or
reset the counter. Each executable process starts with a fresh zeroed counter.
The class static `constexpr` increment is passive constant data.
This storage requires no startup code or cleanup. Ordinary explicit initializers
(including `mut int calls = 0;`), thread-local storage and storage requiring
destruction still need the future per-library initialization/retirement contract.

`base_value` requests `[[inline]]` and `left_value` requests `[[cold, noinline]]`.
Each compiled dependency preserves its own policy, including when one library
imports another. Inline advice does not make imported bodies available for inlining.

The `api/*.gti` paths are virtual declaration interfaces; no generated source
files are written there. `gti check app --release` remains read-only and requires
previously built dependency artifacts. Source target edges currently refer to
`gti-library` targets in the same manifest; distributed `.gtilib` selections also
remain available through `compiled-libraries` tables without producer sources.
