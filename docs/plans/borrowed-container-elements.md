# Shared borrowed container elements: implementation and acceptance

Status: local implementation under validation; unpublished. This is the
owner-authorized shared-element stage of [RFC 0003](https://github.com/lang-gti-official/rfcs/pull/3),
stacked on the lifetime/MIR work. The starting compiler commit is
`2ac3358f0909295a7305bc851aab1e3a30a6c60f`. Passing checks below are focused
contributor evidence, not release or full-suite validation.

## Delivered behavior being validated

Ordinary source-defined owners can retain shared dependencies belonging to
several external objects. Safe reads preserve those dependencies; copying a
value does not erase them. Replacement, copies, branch/loop joins and helper
calls use MIR dependency flow rather than special treatment of a public
container name. The same mechanism admits dynamic text views and unrelated
records containing shared references in `std::vector` and fixed-array literals.

Owner-aware construction, relocation and reads carry an exact typed owner.
Raw addresses alone cannot establish a checked owner or make stored contents
ownerless. LLVM consumes verified lifecycle and owner facts. The storage
provider remains responsible for the raw validity and resource obligations
specified in [ownership and lifetimes](../language/ownership-and-lifetimes.md#standard-library-storage)
and [raw pointers](../language/raw-pointers.md).

## Acceptance ledger

| Requirement | Owning evidence | Current result |
| --- | --- | --- |
| Local replacement, copies, joins and retained owners | `borrowed_dependency_updates`, `mir_carrier_flow` | The full 363-case native matrix passed on LLVM O0/O3, including loop-copy dependency retention, observing cleanup and restored-reference aliases. A subsequent general move-cleanup repair adds a 364th case; final affected validation is running. |
| Generic user storage, vector growth, aliases and checked reads | `ownerless_text_contracts`, native matrix, example 194 | The final compiler ran all 99 named owning tests: 98 passed and one forged-MIR fixture failed before reaching its intended assertion. Correcting its duplicate loan endpoint and supplying program context makes that exact expired-dependency assertion pass independently. Reference-payload semantics and editor recovery also pass on the final compiler; the final native matrix remains pending. |
| Fixed-array literals, nested arrays and indexed copies | Native array cases and `testBorrowedArrayCleanupContainment` | Focused LLVM O0/O3 and independent forged-transfer rejection checks passed. |
| Observing cleanup and partial failure | `object_construction_runtime`, array and vector cleanup fixtures | Array, vector and existing raw/owned/reference cleanup profiles passed at O0/O3, including failed construction. An additional owner-scope safety gap was found: an observing destructor could run after its referent died. The repair preserves known retaining stores through incomplete effect summaries and expires observing dependencies at owner scope exit. Production O0/O3 regressions now reject observing cleanup after owner scope exit, allow passive cleanup, and pass the array/vector partial-failure cleanup harness. |
| Constructor and returned-view composition | Declaration tests, `testDerivedCarriers`, native constructor cases | Original iterator-copy and identity-relay reproducers compile and execute at O0/O3. The two-owner factory return and its forged wrong-owner/duplicate-field checks pass. The affected full suite previously failed only that repaired factory case. |
| Failure-safe LLVM placement of fresh payloads | `llvm_admission`, `payload_fresh_class_operands_runtime` | Fresh enum initialization/replacement and the fresh-payload runtime harness passed O0/O3. The owning admission test exposed a restored-reference return liveness regression; its isolated frontend reproducer passes the return-statement attribution repair, and the full LLVM admission suite plus read-only/mutable restoration runtime regression now pass. |
| Editor diagnostics and recovery | `editor_mir_diagnostics_lsp` borrowed record/vector/array cases | Final-compiler diagnostic and repair tests passed. Paired editor measurements using the same baseline standard library measured cold diagnostics 33.44 → 36.47 ms (+9.1%) and repeated-edit diagnostics 66.18 → 69.26 ms (+4.7%); work invariants pass. The script's `targetMet` flag requires a 30% speedup for an earlier optimisation task and is not this feature's acceptance threshold. |
| Integration and runtime cost | `compiler_pipeline`, native smoke, paired compiler benchmark | The generic moved-owner cleanup repair passes its isolated reproducer and carrier tests; final integration rerun is pending. On the final compiler, four paired runtime workloads measured candidate/baseline ratios of 1.0365 (vector growth), 1.0005 (reserved vector), 1.0539 (string growth/search) and 0.9965 (owned allocation). All 95% bootstrap interval upper bounds are below the owner's 10% threshold. Executable sizes differ by at most 32 bytes. Three compile workloads cost 2.11–3.02 times baseline; this remains a measured compiler-analysis cost, not a runtime improvement. Compiler peak RSS increased by about 19–38 MB across the four workloads. The standalone path-storage probe measured borrowed views at 4.18% of owned-copy elapsed time with equal digests. Its earlier O0/O3 allocation measurement counted 4,099 allocations for owned copies versus 3 for borrowed views, with all counted allocations freed. |

Final timing evidence uses LLVM 23.1.2 on macOS arm64 and the unchanged baseline
`2ac3358f0909295a7305bc851aab1e3a30a6c60f`. Candidate binaries came from snapshot
`abf001b7879d4b622be298f23c25651b8f12332ec09b5da61c654ad9f32b8d14`;
subsequent changes correct a test fixture and documentation only. Runtime
measurements use 15 alternating samples; compile measurements use five paired
samples. Another compiler test occupied one core during measurement, so this
is not an idle-machine claim. Reported intervals quantify the observed timing
variation. The path-storage comparison is between two ownership contracts:
views require their input strings to remain alive, while owned copies do not.


The runnable entry point is [example 194](../../examples/194-borrowed-containers.gti).
The native harness contains both acceptance and source-diagnostic rejection
cases; parser errors, internal errors and unrelated admission failures do not
count as owner-protection evidence. MIR tests also mutate proof metadata and
require independent rejection.

## Explicit boundaries

- Elements with exclusive dependencies remain rejected.
- Direct indexed value copies can retain external owners independently of
  container storage. Adapter-derived copies conservatively retain the physical
  container borrow as well; independence through every adapter remains work.
- Generic construction of dynamic text fields in a class with a user-defined
  destructor remains rejected. Reference-bearing records with observing
  destructors are covered separately; these are different publication paths.
- Consuming iteration requires an independently owned, relocatable result.
  Shared borrowed elements use ordinary iteration and checked indexed reads.
- This stage does not claim RFC M5 associative-container admission or complete
  M6 support for every returned/nested container shape. Existing unsupported
  escape shapes remain diagnosed. Their generalization needs its own complete
  lifetime, cleanup and caller-substitution proof.

Completion requires the pending final integration and cleanup checks to pass
on the final compiler changes. A successful fixture alone does not
close those gates, and this document does not claim publication or integration.
