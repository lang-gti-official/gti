# Shared borrowed container elements: implementation and acceptance

Status: shared-element stage validated locally; reconciliation with current main
is under validation for integration. This is the
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
| Local replacement, copies, joins and retained owners | `borrowed_dependency_updates`, `mir_carrier_flow` | The current candidate passed all 379 native acceptance/rejection cases (547.02 seconds), including loop-copy dependency retention, observing cleanup, restored-reference aliases, moved-owner cleanup, reference helpers and packs, record replacement, fixed arrays and iterator compositions. Admitted programs execute on LLVM O0 and O3. |
| Generic user storage, vector growth, aliases and checked reads | `ownerless_text_contracts`, native matrix, example 194 | The final candidate passes all 99 named owning tests (565.60 seconds), including the corrected forged-MIR expired-dependency fixture. Reference-payload semantics (91.48 seconds), view fields (534.08 seconds), editor recovery (12.09 seconds), construction cleanup (16.04 seconds), native smoke and example 194 also pass. |
| Fixed-array literals, nested arrays and indexed copies | Native array cases and `testBorrowedArrayCleanupContainment` | Focused LLVM O0/O3 and independent forged-transfer rejection checks passed. |
| Observing cleanup and partial failure | `object_construction_runtime`, array and vector cleanup fixtures | Array, vector and existing raw/owned/reference cleanup profiles passed at O0/O3, including failed construction. An additional owner-scope safety gap was found: an observing destructor could run after its referent died. The repair preserves known retaining stores through incomplete effect summaries and expires observing dependencies at owner scope exit. Production O0/O3 regressions now reject observing cleanup after owner scope exit, allow passive cleanup, and pass the array/vector partial-failure cleanup harness. |
| Constructor and returned-view composition | Declaration tests, `testDerivedCarriers`, native constructor cases | Original iterator-copy and identity-relay reproducers compile and execute at O0/O3. The two-owner factory return and its forged wrong-owner/duplicate-field checks pass. The affected full suite previously failed only that repaired factory case. |
| Failure-safe LLVM placement of fresh payloads | `llvm_admission`, `payload_fresh_class_operands_runtime` | Fresh enum initialization/replacement and the fresh-payload runtime harness passed O0/O3. The owning admission test exposed a restored-reference return liveness regression; its isolated frontend reproducer passes the return-statement attribution repair, and the full LLVM admission suite plus read-only/mutable restoration runtime regression now pass. |
| Editor diagnostics and recovery | `editor_mir_diagnostics_lsp` borrowed record/vector/array cases | Final diagnostic and repair tests pass. Five paired samples with the same baseline standard library measure cold diagnostics 38.20 → 41.13 ms (+7.7%) and repeated-edit diagnostics 68.01 → 70.73 ms (+4.0%); frontend work invariants pass. The script's `targetMet` flag requires a 30% speedup for an earlier optimisation task and is not this feature's acceptance threshold. |
| Compiler integration | `compiler_pipeline`, native smoke | All 241 compiler tests pass across frontend, declaration, ownership, compile-time, diagnostics and end-to-end suites (1,048.58 seconds), with exact manifest census verification. The lifecycle query uses the existing nominal recurrence guard and memoizes repeated branches. Reference-pack lanes preserve ordinary reference-formal content-entry rules. All scheduled library integration checks and native smoke pass. |
| Runtime, allocation and compiler costs | Paired compiler benchmark and standalone allocation probe | Final measurements are below. These are measured costs of this stage, not a claim of full-platform CI or universal runtime parity. |

## Final measured costs

LLVM 23.1.2, macOS arm64; unchanged baseline
`2ac3358f0909295a7305bc851aab1e3a30a6c60f`, final candidate snapshot
`951a9f533f11021f25502d0ff082a114ff9c402d30432a724b9a5590c8b54df1`.
Remote run `run-c6c680b75c97478690c163161947ebf2` passed every scheduled step.
Production sources and tests match the measured snapshot; subsequent edits
only update documentation. Retained evidence is in
`/private/tmp/gti-borrowed-lifecycle-completion-results`.

Runtime uses 15 alternating samples per workload. Ratios are candidate/baseline;
all upper bounds are below the owner's 10% runtime threshold. Digests match.

| Workload | Median ratio | Paired bootstrap 95% interval |
| --- | ---: | --- |
| Vector growth | 1.0382 | 1.0362–1.0418 |
| Reserved vector | 0.9997 | 0.9916–1.0029 |
| String growth/search | 1.0556 | 1.0495–1.0592 |
| Owned allocation | 0.9982 | 0.9884–1.0075 |

Executable sizes differ by at most 32 bytes. Compiler peak RSS increased by
13.6–37.4 MB in these four builds. Five paired compilation samples for vector
growth, owned allocation and string growth/search cost respectively 2.40×,
2.91× and 2.02× baseline. This is a substantial compiler-analysis cost; runtime
acceptance does not imply compile-time parity.

The final allocation probe at both LLVM O0 and O3 reports 4,099 allocations
and frees for owned copies (491,616 requested bytes), versus three allocations
and frees for borrowed views (65,632 bytes), with equal digest 385024.
The independent allocation-interposition control passed before these runs.
The separate path-storage timing probe uses 34,085 rounds and 15 alternating
samples: owned copies take 2.834838 seconds, views 0.118797 seconds (4.19%),
with equal digest 13123543040. These are different ownership contracts:
views require their input strings to remain alive, while owned copies do not.

An unrelated language-server process occupied one core during measurement;
this is not an idle-machine claim. Recorded load and raw samples accompany
the results. Intervals quantify observed timing variation, not all possible
machines or workloads. The retained evidence includes `Testing/borrowed-performance/paired.json`,
`Testing/borrowed-compile-cost/evidence.json`, `Testing/borrowed-editor-cost.json`,
`Testing/allocation-evidence.json` and `Testing/owned-view-runtime/evidence.json`.



The runnable entry point is [example 194](../../examples/194-borrowed-containers.gti).
The native harness contains both acceptance and source-diagnostic rejection
cases; parser errors, internal errors and unrelated admission failures do not
count as owner-protection evidence. MIR tests also mutate proof metadata and
require independent rejection.

## Explicit boundaries

- Elements with exclusive dependencies remain rejected.
- Borrowed fixed-array initialization and indexed reads are covered; indexed
  replacement of a text element retains its existing ownerless-input restriction.
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

All planned correctness, cleanup, example, allocation and performance gates
for this bounded shared-element delivery passed on the final candidate.
The explicit boundaries above remain unsupported; this does not claim the
whole RFC is complete, or that the local implementation has been published,
integrated or released.

## Integration reconciliation

The local stage was completed at `248de0e9d`. Integration merges main at
`3d342f75b`, retaining its constant-allocation provider, counted-view narrowing
and improved source diagnostics. Owner-aware storage operations use the same
MIR constant evaluator as the existing raw storage operations: owner witnesses
remain verified metadata, constructor arguments exclude the witness, and
initialization/relocation preserve the live/vacant storage checks. The source
wrappers retain main's `constexpr` declarations. The combined candidate is
being revalidated, including the existing compile-time/runtime container
fixtures; the measurements above describe the pre-reconciliation candidate.


### Integration blockers (2026-11-16)

PR #425 remains a draft, not ready for merge. The reconciled published head
`537a058de` passes `constexpr_owned_storage_runtime`,
`constexpr_owned_containers_example`, `view_fields`,
`object_construction_runtime`, `reference_payload_semantics`,
`ephemeral_carrier_mir`, both operator-place composition suites, and
`mir_carrier_flow`. However, `compiler_pipeline` fails the portable `import
std` case and `ownerless_text_contracts` reports 28 failed assertions.
The pre-reconciliation results above do not certify this combined head.

The failures span retained-input publication, borrowed-record construction,
cloning, borrowed-array return identity, and conditional cleanup/verification.
They must be resolved and the focused integration plan rerun before handoff.
Do not suppress these checks or treat existing native execution evidence as a
substitute for independently verified MIR.

A focused follow-up recognizes owned fields selected through reference formals
when tracing an opaque content entry to its actual storage primitive. It checks
the physical reference/field path and selected callee type, rejects indexing
or additional dereferences, and permits read-only fields under a mutable
formal. The owned aggregate runtime regression passes on LLVM O0 and O3;
`testOwnedStorageReferenceReceiver` passes its direct and projected cases and
forged-path rejection. This is focused evidence only, not resolution of the
remaining integration failures. Nested aggregates containing borrowed
containers remain outside the declared delivery boundary.

The integration run is `run-3469e552844c47dcb39f276d36126e52` (LLVM 23.1.2,
macOS arm64), bound to `537a058de` and snapshot
`03126430c06e7c599c03a1ca4154b952051074f038da8dd2485c4ee224b022f8`.
The follow-up probes used an override object linked against that build; they
are diagnostic evidence and do not constitute a clean-head verification.
