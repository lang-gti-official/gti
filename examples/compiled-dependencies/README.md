Build and run the application with `gti run app --release` in this directory.
The driver builds the shared `base` dependency, then `left` and `right`, then
links `app` with each object's dependency closure exactly once. Repeating the
build reuses libraries whose complete source, compiler configuration, and direct
dependency digests still match. Editing `base.gti` rebuilds its dependents.

`left_tick()` and `right_tick()` access the same private namespace counter and
class static counter in `base`. Alternating calls check that linking the diamond
does not duplicate or reset either counter. Each executable process starts with
fresh zeroed counters. The private `constexpr` increment is passive constant data.
This storage requires no startup code or cleanup. Ordinary explicit initializers
(including `mut int calls = 0;`), thread-local storage and storage requiring
destruction still need the future per-library initialization/retirement contract.

The `api/*.gti` paths are virtual declaration interfaces; no generated source
files are written there. `gti check app --release` remains read-only and requires
previously built dependency artifacts. Source target edges currently refer to
`gti-library` targets in the same manifest; distributed `.gtilib` selections also
remain available through `compiled-libraries` tables without producer sources.
