# GTI Dependency-Ordered Implementation Sequence

> **Plan status:** Accepted operational sequencing and status plan. It does not
> define current language semantics or replace the detailed design documents
> for an individual subsystem.

Checkpoint: maintained against current `main`; canonical behavior remains in
`docs/language/` and `docs/architecture/`.

This document turns GTI's architecture reviews, language review, accepted
plans, and current implementation checkpoint into one executable work queue.

The direction is an independent, fully usable systems language with excellent
safety, strong C++ syntax familiarity, source-defined public policy and sound
compiler/runtime support. Historical assumptions and earlier private boundaries
are revisable against that goal. Rows describe outcomes, prerequisites and
acceptance evidence; an unmet prerequisite is further work to schedule, not a
permanent language limit or permission to substitute generated C++ behavior.

It answers three questions for every substantial piece of future work:

1. what must be decided or implemented first;
2. what one coherent change should accomplish; and
3. what evidence allows the next change to begin.

Current behavior remains authoritative in [`docs/language/`](../language/) and
[`docs/architecture/`](../architecture/). The durable release capability map
remains [`roadmap-to-1.0.md`](roadmap-to-1.0.md). Current implementation
evidence belongs in architecture documents and tests; the older
[`compiler-roadmap-status.md`](archive/compiler-roadmap-status.md) is retained
only as a historical checkpoint. Detailed domain decisions remain in the other
files under `docs/plans/`. The documents under
[`docs/third-party-audit/`](../third-party-audit/) are evidence and review
input, not a second implementation queue.

## Runtime Performance Requirement: 2026-10-09

Runtime performance is now a first-class design and delivery requirement under
[ADR 012](../decisions/012-outcome-first-systems-readiness.md#runtime-performance-requirement-2026-10-09).
This explicitly supersedes benchmark/proxy-first admission restrictions in the
older O-PERF, O-MIR and RT rows below. General optimization architecture can be
designed and implemented from a reasoned cost model and the needs of systems
operation families; no previously benchmarked regression, individual hot client
or minimum proxy speedup is required to start. Technical prerequisites, phase
authority, correctness proofs and task scope still apply.

Measurements remain implementation acceptance evidence, not permission to work.
Validate optimized representative execution and its runtime, allocation,
code-size and relevant compile/memory tradeoffs. Material regressions require
correction or explicit owner acceptance; functional success plus a follow-up
issue no longer suffices. Historical completed campaigns and their measurements
remain historical facts, not proof that runtime performance is finished or that
new architecture must wait for another regression. This documentation revision
does not itself implement an optimizer or storage migration.

## Runtime audit follow-up: 2026-10-09

`RT-1` is complete as a bounded client: reduce source-storage success-path costs
by outlining only the incoming cleanup-failure check. PR #166 is reviewed as
proposal input, with immediate versus future dispositions in
[optimization.md](optimization.md#pr-166-audit-disposition-2026-10-09).
Prerequisite: reconciled modules/reachability integration (#174, main
`b6614cb4`), a semantics-preserving current-head proxy, and unchanged source
storage policy. Acceptance: exact frozen-snapshot production comparison,
complete allocation/destructor observations, runtime/code-size/memory controls,
and cleanup/panic/managed-thread/sanitizer/malformed-MIR tests. Clean optimized
`f2fea6c7` passes all 570 unfiltered CTests, both 130-example corpus oracles,
thirteen staged-library consumers and the installed source-provider matrix.
Production storage medians improve 70.464→59.177 ms and 76.490→66.230 ms,
with full-build time essentially unchanged and the small size/memory tradeoffs
recorded in [the performance evidence](performance-tooling.md#production-comparison).
The earlier historical regression in #169 is not considered fully recovered merely because
this one cause improves. Follow-on RT-2 through RT-10 need their individual
prerequisites and measured clients; they are not added to this implementation.

## Compile-Time Reflection Delivered (2026-10-11)

The owner accepted [`reflection.md`](reflection.md) with its decisions. All
three phases are implemented: phase 1 (`constexpr for`, type and field
metaobjects, reflected field access), phase 2 (declared attributes with
constant arguments, `[[reflect_private]]`, `std::meta::reflectable`,
enumerator/payload, base and generic-argument metaobjects, three fixture
clients) and phase 3 (`std::meta::construct`, method/constructor/parameter
metaobjects, source locations, budget hardening). Reflection is no longer
"later breadth" in the roadmap; what remains restricted (invocation through
metaobjects, static-field metaobjects, runtime reflection, source generation,
layout) is recorded in the alignment ledger. Next: `std::experimental::json`
as the verification gate, after which `std::json` is deprecated, then the
parked `lsp-lazy-bodies` task. The campaign touches the parser,
semantic analysis (declaration-stage attribute evaluation, folded queries,
aliases), HIR lowering and the trusted standard library; its one MIR change
materialises direct-initialized temporaries before projection and its one
backend change emits `[[reflect_private]]` members public. It is not
exclusive: unrelated work may proceed, but changes to `if constexpr`
deferral, constant folding, designated construction or class registration
should reconcile with it. Status and residual questions live in the plan
document.

## Stabilization Reset: 2026-10-08

The owner paused new issue-campaign work to finish the existing lifetime/storage
and payload-enum features, consolidate their handoffs, run the complete suite,
and release. Concurrency is at most the release owner plus **two other agents
in total**, with no child agents. Integration and validation have one owner;
other agents must not launch duplicate full builds or independent releases.

The unfinished-work inventory is:

| Work | Preserved state and release disposition |
| --- | --- |
| General object lifetimes, source sparse storage, conditional unsafe capabilities (PR #94) | Implemented and reconciled candidate; the combined full-suite and installed-toolchain release gate remains authoritative. |
| Source process byte provider (PR #104) | Published implementation is an ancestor of #94; confirm integration before closing its redundant PR. |
| Generic payload enums (PR #110) | Complete implementation and preflight handoff reconciled into #94; use the shared final validation and one integration, not a second release. |
| Constructor campaign (PR #125) | Loop reinitialization, owned constructor packs and stable base stages already included in #94. The later computed scalar base-argument extension remains preserved separately; general preparation frames are still proposed. |
| Other campaign checkpoints (#114, #118, #124, #128) | Published partial-campaign work, not final combined acceptance. Preserve for explicit release-scope selection or the resumed campaign; do not silently claim their open issues fixed. |
| Operator/borrowed-view relay (PR #127) | Preserved draft with a known failing map-key-view matrix; not ready for integration. |
| Source string-view rewrite (PR #109) | Proposal/review-first, including separate unpublished Claude edits. Not approved for automatic adoption or required as a release prerequisite. |
| Release-validation repairs (PRs #131/#132) | Passive-array/payload admission and exact constructor-publication tests, plus inherited failure-cleanup propagation, are consolidated into #94. Validate and integrate once; close these redundant PRs only after their commits reach main. |

The legacy `feature-interactions` edits are already incorporated (or superseded
by additive improvements). The `issues-30-33` test commit and dirty lowering
guard have exact patch-equivalent implementations in the candidate. Preserve
those recovery trees without reapplying them. The detached Claude `gti-base`
emitter edits contain temporary tracing, not a missing semantic fix; retain them
as debugging scratch, outside the release. Unrelated primary-checkout edits and
generated application artifacts remain untouched. No branch/worktree cleanup is
needed to establish release acceptance.

## Paused: 2026-10-07 Issue-Closure Campaign

**Status: partially integrated; paused by the stabilization reset above.**
The initial inventory is the 50 open issues #51, #53–#93 (excluding #94),
#95–#100, #103 and #105. Issue #103 additionally requires resolving every
one of F10, F14, F16, F17, S01, S02, S04, S06, S09, S12 and S15; closing or
splitting the umbrella does not satisfy those outcomes.

Initial non-overlapping batches:

| Owner | Issues | Integration state |
| --- | --- | --- |
| Aggregate-array worker | #63, #85, #93 | PR #106; focused checks passed, reconciliation in progress |
| Generic-instance worker | #57, #95, #98 | merged PR #107 at `5f5f370c`; five focused gates passed, issues closed |
| Main (no subagents) | #58, #68, #69 | `codex/diagnostics-58-68-69`; claimed with direct owner authorization |
| General-lifetime owner, on existing PR #94 | #84 | focused eight-mode checks passed; #70 replay still fails, remains unclaimed |
| Payload-enum owner | #88, #89, #90; #103/S15 overlap | offered; claim pending |
| Coordinator, driver/entry closure | #54, #56, #51 | merged PR #108 at `1e3aa289`; eight focused gates passed, issues closed |

Unassigned issues remain open. Every completed batch must retain source-located
negative coverage and actual runtime/cleanup evidence where relevant, using a
host-optimized compiler. Strict audit expectations change only when the actual
finding is fixed. Publish checkpoints and PRs, reconcile each against canonical
main, and integrate in coordinator order through required PR checks. Full
verification is deferred until the combined campaign, rather than repeated
per issue; failed final gates require fixes, not weaker expectations. Completion
requires all original issue outcomes resolved and closed, all integration on
primary local main, and final broad evidence against that combined tree.

## Completed: Unsafe Declaration Keyword Migration

**Status: implemented and locally validated for the 0.374.0 candidate.** Low-level APIs express
their caller obligation with `unsafe` rather than `[[unsafe]]`. The existing
namespace-scope free-function boundary and explicit implementation blocks
remain unchanged; this is syntax/tooling migration, not memory-model expansion.
The obsolete attribute is rejected with a spelling-only migration fix.

The shared compiler/LSP `unused-unsafe` warning offers keyword-only removal
when checked non-generic code did not use that block's permission. Existing
warning suppression controls apply. Complexity scoring and an opt-in
`unsafe-code` policy remain deferred: they need calibrated audit thresholds
and warning-enable policy respectively. Neither automatic unsafe wrapping nor
unsafe methods, constructors, destructors or operators is part of this slice.

Current behavior is specified in [raw pointers](../language/raw-pointers.md#functions-with-a-caller-obligation);
validation is recorded in [compiler verification](../architecture/verification.md#unsafe-declaration-keyword-migration-and-dependency-warnings).

## Completed: Conflict-Based Stored-Borrow Aggregates

**Status: implemented and locally validated for 0.373.0 after PR #35 reconciliation.** The outcome
is an application-owned services/context object retaining any number of shared
and exclusive references, without introducing source lifetime annotations or
public-type recognition. Existing `T&` and `mut T&` syntax is unchanged.

- Preserve each stored reference's complete field path, originating place,
  access mode, and reborrow ancestry through semantic analysis, HIR and MIR.
  Additional-dependency records carry each path's permission rather than
  assuming that every dependency after the first is shared.
- Replace count-based admission with pairwise overlap/access checks. Independent
  owners and proven disjoint field places may coexist; overlapping shared and
  exclusive access, or two exclusive accesses, must be rejected.
- Infer constructor bindings and structurally unambiguous return projections.
  Moves transfer the complete dependency set; copies must not duplicate any
  exclusive dependency. Carrier storage and external referents remain distinct.
- Preserve every owner while a carrier or reachable derived borrow needs it.
  Projection and last-use handling must not discard unrelated live dependencies
  or retain a fabricated common owner. Ambiguous or unsupported provenance
  remains a source diagnostic, never a backend assumption.
- Prove unrelated user-defined mixed/four-plus-path and nested/generic carriers,
  disjoint and overlapping places, moves/copy rejection, constructor failure,
  derived borrows, return provenance and branch/loop endpoints. Include negative
  MIR mutations for missing paths, permission upgrades and premature release,
  optimized runtime/failure parity, shared LSP diagnostics, a runnable example,
  and canonical ownership/IR documentation before publication.

This extends executable ownership behavior and is not complete merely when the
old declaration rejection is removed. Existing structural restrictions unrelated
to reference cardinality require individual review; they are not permission to
substitute a fixed two-reference feature for the requested dependency-set model.

Implemented capability: constructor-bound mixed paths,
repeated shared origins, disjoint owner fields, nested generic moves, and direct
field reborrows have positive/negative semantic and independent MIR coverage.
Concrete dependency records now keep field identity and access together;
reference-field assignment preserves its executable dereference through HIR/MIR.
Direct child references select the corresponding external owner and permission,
including shared aliases and simultaneous disjoint exclusive children. They do
not borrow whichever owner happened to be the carrier's first field.
Field-mapped factories, reference accessors and whole-value relays now publish
result-field/input-field/access contracts. Concrete generic relay calls infer
the same contract as their instantiated bodies. Independent MIR verification
checks caller metadata against the callee's actual returned origins, including
reordered and repeated inputs; an unused parameter is not a fabricated owner.
Physical carrier storage, abstract entry referents and compact single-path
view origins remain distinct. A noncopyable relay retires the moved source
loan on successful publication while preserving failure cleanup.
The optimized compiler pipeline, carrier-shell-reference, optimizer-foundation,
borrowing-composition pipeline/runtime and nested-borrow runtime suites pass.
`stored_borrow_aggregates.gti` passes C++20/C++23 O0/O1/O3, no-elide and sanitizer
runs. The completed acceptance evidence is recorded below.

Ordinary operators and immediate temporary receivers now preserve the same
field maps. A temporary's complete external dependency set outlives its owned
field cleanup; there is no separate passive multi-owner receiver exception.
Generic accessors, branch/loop endpoints, LSP hover/definition/diagnostics and
example 109 have focused coverage. Nested constructor publication uses each
field's own permission; backend receiver projections follow exact MIR field and
reference steps, including references inside nested aggregates. The old homogeneous
single-origin summary remains a compact representation only where it does not
lose a field choice; it is not authority for mixed factory return mappings.

The user also authorized integral-enum formatting in parallel. The reviewed
handoff `c98f97c8` on `codex/enum-formatting` (example 110) is integrated.
It adds general identity-bound enum representation constraints/extraction and
ordinary source-defined formatter policy. Its focused runtime and LSP checks
pass alongside the aggregate checks. Combined MIR and lowered-program census
inventories pass all four corpus gates over 107 examples. All 422 registered
tests have passing evidence, including focused reruns of six initial failures.
The combined surface version is 0.373.0. All eleven isolated installed-library
consumers, both new examples, installed LSP checks and Tree-sitter gates pass.
See the canonical
[verification record](../architecture/verification.md#stored-borrow-aggregates-and-integral-enum-formatting)
for the exact host, test corrections and remaining platform limits.

## Completed: Composable I/O, Mutable Buffers, And Extensible Formatting

**Status: implemented and locally validated for 0.371.0.** The outcome is one parser/copy routine
working against file, memory, and buffered transports with reusable initialized
byte storage; application-owned types can participate in the same formatting
APIs through checked source-defined customization.

- Extend the existing exclusive carrier/child-loan contracts only where needed
  for mutable subviews and retained shared projections. Preserve owner and parent
  suspension, alias endpoints, no permission upgrade, and MIR verification.
- Keep read-only `span<T>` compatible; add explicit `mutable_span<T>` with the
  familiar checked span operations and owner factories. A byte buffer owns its
  initialized bytes; reading must not expose uninitialized capacity.
- Define generic partial reader/writer contracts, exact-read/all-write/copy
  helpers, memory/file/standard-stream adapters and reusable buffering. Specify
  EOF, zero progress, short operations, partial failure, and explicit flushing.
- Implement public formatting customization and general exact member-signature
  requirements with an independent formatting agent; do not recognize public
  library names in compiler semantics.
- Prove unrelated user-defined borrowers/transports/formatters, negative lifetime
  and contract cases, failure cleanup, O0/O3 and C++20/C++23 behavior, installed
  consumption, runnable examples, and updated source-surface/corpus inventories.

Mutable prefix-backed subviews, retained shared projections, source-defined
buffered adapters and the formatter protocol are implemented. Combined
lifetime/cleanup, native I/O, installed-consumer and regression validation
passes: the eight-mode runtime matrix, all four 105-example corpus gates,
source-surface checks, all 418 registered tests (including targeted reruns),
isolated examples and eleven installed-library consumers. See the canonical
[verification record](../architecture/verification.md#composable-io-and-mutable-buffer-validation)
for the exact host, failures repaired and environment-limited reruns.

## Stdlib Capability Adoption And Upgrade Campaign

**Checkpoint: 2026-10-04, reviewed source baseline `c427474d` / 0.364.1.**
Two independent source reviews compared the current compiler contracts with
stdlib implementations. This section schedules implementation; it does not
claim the proposed rewrites have been compiled, benchmarked, or released.
Subsequent changes on `main` must be reconciled before executing a row.

**Adoption delivery checkpoint: 0.367.0.** Rows 1–6 have been implemented and
validated; row 9 was already delivered in 0.366.0. The other rows were checked
against current source and focused optimized-compiler probes, rather than
assumed unavailable solely from the original review. Their remaining
prerequisites below are real new capabilities or retained correctness defects, not source-only
adoption work. See the [verification record](../architecture/verification.md#standard-library-adoption-campaign).

**Outcome:** make the existing library use the language GTI now has, rather
than preserve obsolete workarounds or grow new APIs around them. The named
clients are a Tea-style HTTP/JSON command-line application and an asset/game
workload with move-only resources, large sorted tables, and compile-time
metadata. They are acceptance workloads, not nominal compiler exceptions.

This is the current adoption schedule within this document's sole queue.
Older Phase L checkpoints below retain historical evidence; this campaign
does not restart completed facilities or supersede an explicitly active
compiler campaign. Before expanding a stdlib family, check its row here and
the canonical [library contract](../language/standard-library.md).

### Reviewed Capability Map

| Implemented facility and boundary | Library consequence | Adoption owner |
| --- | --- | --- |
| Owned Expected propagation, `expect`, `transform`, `and_then`, and `transform_error`; exact error identity and consuming receiver rules remain. Version 0.366.0 also supports checked read-only `expected<T&, E>` extraction through `?` and `expect`; borrowed combinators remain excluded. | Simplify owned fallible pipelines and checked borrowed lookups now. | `L-UP-01`, `L-UP-09`; [Expected](../language/expected.md). |
| Explicit movement, bounded implicit return of an exact whole move-only local, generated lifecycle policy, immediate borrowed-carrier receivers, and sealed restoring/indexed transfers. None permits unrestricted consumption through references or dynamic indices. | Move owned mapper results, exchange initialized mutable places, and reverse/sort relocatable move-only vectors without cloning. | `L-UP-03`, `L-UP-07/08`; [ownership](../language/ownership-and-lifetimes.md). |
| Nested owner-tied text relay, shared multi-origin carriers, checked exclusive access, storage shift/relocation, and source-defined explicit cloning. Multi-owner projections remain conservative. | Remove duplicate URL view backing; retain owner invalidation. Precise range projection and proxy iteration remain separate work. | `L-UP-02`, `L-UP-16`; [relay fixture](../../tests/fixtures/text_owner_relay.gti). |
| Exact operation/comparator concepts, bounded ordinary-method requirements, exact class specialization, exact argument-bearing read/mut member requirements, dependent compound concept arguments, constrained heterogeneous call-pack elements, and attributable contract diagnostics. Static queries, general expression requirements, reference packs, and conditional operators are not implemented. | Source-defined public formatter customization and ordinary read/write protocols now use these facts; conditional indexing remains separate. | `L-UP-04`, `L-UP-13/15`; [concepts](../language/concepts.md). |
| Constructor-local type packs and final-field construction have positive immovable-node evidence. Borrowed argument forwarding still requires a recorded backend composition correction. Optional publication currently transfers a completed payload. | Finish that correctness slice, then adopt node emplacement. Direct inline emplacement is not a forwarding-only rewrite. | `L-UP-10/11/12`; [existing emplacement checkpoint](#active-request-complete-the-existing-standard-library). |
| Staged verified-MIR constant evaluation supports passive records, constructors, read-only methods, fixed arrays and literal-backed text. Allocation, owning aggregates, general Expected/payload execution, packs and mutable receivers are outside the completed profile; the older bounded checked-integer constant-result family already exists. | Add eligible pure chrono observers now. Automatic literal-format checking still needs a general call-site contract. | `L-UP-06/14`; [`L-CONST-01`](#l-const-01-remaining-bounded-compile-time-programming). |
| Exact function items, source-defined owning callable erasure, modules, panic/caller tracking, managed threads, atomic order tags and mutex guards are implemented within documented bounds. | Do not schedule these as absent. Measure callback erasure costs; preserve host error and concurrency policies. Modules do not require a global include-to-import rewrite. | [callable checkpoint](#l-call-02-source-defined-owning-function-wrappers), [concurrency](../language/concurrency.md), [module contract](../language/programs-and-targets.md). |
| Typed feature-interaction generation, differential configurations, metamorphic renaming and bounded reduction are available. | Add composition cases for each adoption; single-feature tests alone do not close a row. | [verification](../architecture/verification.md#failure-capture-and-opt-in-fuzzing). |

AVL `map`, bucketed `unordered_map`, explicit vector/optional cloning, payload
enums, JSON codecs, HTTP URLs, clocks, processes and recoverable file operations
already exist. Their limitations must be reviewed against source, not inferred
from an old audit's absence list. In particular, do not reintroduce withdrawn
custom copy/move constructor bodies, make optional implicitly allocating or
copyable, or resume deferred C-style thread completion in this campaign.

### Execution Order And Readiness

**Ready / adoption** means the enabling general contract is implemented and
the rewrite is a source-inspection-supported candidate; it still needs its own
composition and runtime evidence. **Ready / new capability** means no earlier
row is required: adopt the bounded contract and implement its first client in
the same vertical slice. It does not mean the missing capability exists.
Rows with unmet prerequisites cannot be marked complete by a source-only workaround.

| Order / ID | State | Hard prerequisite | User-facing result |
| --- | --- | --- | --- |
| 1 / `L-UP-01` | complete in 0.367.0 | current owned Expected contract | Shorter owned JSON/allocation/clock error pipelines without losing error context. |
| 2 / `L-UP-02` | complete in 0.367.0 | current nested owner-view relay | URL component/rendered views without three duplicate backing buffers. |
| 3 / `L-UP-03` | complete in 0.367.0 | current owned call results and writable replacement | `transform` accepts suitable move-only output elements instead of copying each result. |
| 4 / `L-UP-04` | complete in 0.367.0 | current ordinary-method constraints and diagnostics | Invalid library calls fail at their API contract, starting with optional `value_or`. |
| 5 / `L-UP-05` | complete in 0.367.0 | current comparator contract; copyable elements | Comparator sorting with bounded O(n log n) comparison growth. |
| 6 / `L-UP-06` | complete in 0.367.0 | completed passive-record MIR constant profile | Compile-time duration/time-point constants and pure observers. |
| 7 / `L-UP-07` | complete in 0.369.0 | current places, lifecycle and failure authority | Sound generic exchange/swap of eligible initialized writable places. |
| 8 / `L-UP-08` | complete in 0.369.0 | `L-UP-07`, checked disjoint indexed exchange | Array swap and move-only vector reverse/sort without cloning elements. |
| 9 / `L-UP-09` | complete in 0.366.0 | current checked borrowed Expected and loan facts | Propagation/extraction of a checked borrowed success without a manual wrapper dance. |
| 10 / `L-UP-10` | ready / correctness prerequisite | existing constructor-pack implementation and retained failure | Borrowed constructor-pack composition succeeds end to end. |
| 11 / `L-UP-11` | planned | `L-UP-10` | Node-container emplacement constructs immovable mapped values at their final address. |
| 12 / `L-UP-12` | planned | `L-UP-10`; selected-constructor/final-slot initialization contract | Direct sparse/inline emplacement for currently eligible payloads, without an intermediate T temporary/move. |
| 13 / `L-UP-13` | implemented and locally validated for 0.371.0 | exact argument-bearing read/mut signatures, dependent compound concept facts, constrained pack calls | Public source-defined formatter protocol and compile-time unsupported-type diagnostics; examples 105/106 and compiler/runtime/editor/corpus/installed gates pass. |
| 14 / `L-UP-14` | ready / new capability, staged | current literal-text/passive-record MIR constant profile; shared validator before call-site admission | Bad literal format strings fail before execution; dynamic strings stay recoverable. |
| 15 / `L-UP-15` | ready / new capability | current validity-only requirements and operator selection | Conditionally available map indexing without imposing default construction on every mapped type. |
| 16 / `L-UP-16` | ready / new capability | current multi-origin shared-carrier facts | Precisely projected read-only views retain only their actual owner dependencies. |
| 17 / `L-UP-17` | ready / new capability | typed allocation status, lifecycle rollback and exact owner publication | Recoverable unique-owner allocation rather than terminal allocation failure only. |
| 18 / `L-UP-18` | planned | `L-UP-12`; separate storage-element and generated-wrapper lifecycle eligibility proofs | Address-stable immovable storage and a deliberately bounded immovable inline optional. |

The order is priority, not an artificial dependency chain. Rows 1–6 can land
independently; do not wait for exchange to repair sort complexity or for a new
borrow engine to remove URL copies. Complete one coherent row, update its
state/evidence here, then choose the earliest unfinished eligible row.
`L-UP-10` is a nearest prerequisite for a named library outcome, not a new
general infrastructure campaign. If another agent owns that compiler boundary,
coordinate integration and select an independent adoption row.

### L-UP-01: Adopt Owned Error-Pipeline Helpers

- **Implementation (2026-10-04):** JSON string insertion, recursive parser
  propagation and codec document loading now use exact-error `?`. Typed getter
  and array-codec error branches that prepend JSON Pointer context remain
  explicit. Arena/pool allocation factories propagate the same `alloc_errc`;
  elapsed subtraction maps arithmetic errors to `clock_errc` through a typed
  `transform_error` mapper. No recoverable operation becomes terminal.
  Adoption exposed a general backend admission defect: a void-success
  `ExpectedValue` observer was not counted alongside `ExpectedError` for an
  owning error shell. Admission now retains both existing verified MIR arms;
  the unrelated `Value`/`Error` loop fixture covers success, recoverable error,
  terminal failure and exact cleanup, with a forged-result verifier mutation.
  Example 100 demonstrates an owning application error and a JSON pipeline.
  Focused JSON, arena/pool, propagation, owning mapper and backend tests pass;
  the new adoption matrix covers C++20/C++23 O0/O3, no-elide and ASan/UBSan.
  Installed-consumer, census and broad campaign verification pass.
- **Original client/evidence:** JSON text insertion, arena/pool factories, and
  elapsed clock conversion contained check-then-move code in
  [`json.gti`](../../stdlib/std/json.gti),
  [`memory.gti`](../../stdlib/std/memory.gti), and
  [`chrono.gti`](../../stdlib/std/chrono.gti).
- **Steps:** inventory these bounded paths; use `?` for exact same-error
  extraction and `transform_error` with an eligible typed mapper where a
  deliberate category conversion already exists. Keep straightforward control
  flow where mapping needs custom path/location context. Do not remove a move
  into Expected merely because exact whole-local returns now move implicitly.
- **Boundary:** parsing, I/O, HTTP, randomness, allocation and thread startup
  remain recoverable. Plain formatting/stdout panic adoption is already done;
  do not convert ordinary domain errors or bounds failures into generic panic.
- **Exit:** original error categories and JSON paths/locations match; exact
  once-only evaluation, success/error movement and reverse cleanup match for
  owning errors, nested calls and terminal mapper failures. Include an unrelated
  application error type and an installed fallible-pipeline example.

### L-UP-02: Remove Obsolete URL View Storage

- **Implementation (2026-10-04):** views borrow the existing owned strings;
  all three mirror buffers and copy loops are removed. Ordinary private
  factory/clone constructors now compile, so the former public bridges are
  private. The view-mutation, consumption and temporary-escape negatives remain
  rejected. The runtime allocation fixture measures parse/clone/append/chaining
  with exact rendered-output checks under C++20/C++23 O0/O3, no-elide and
  ASan/UBSan. The short URL has 32 bytes; the long URL has 4,116 bytes.

  | Input / operation | Before allocations / bytes | After allocations / bytes |
  | --- | --- | --- |
  | short parse or clone | 12 / 192 | 9 / 141 |
  | short append | 7 / 169 | 6 / 132 |
  | short two-segment chain | 25 / 595 | 20 / 454 |
  | long parse or clone | 10 / 12,403 | 7 / 8,268 |
  | long append | 6 / 16,472 | 5 / 12,351 |
  | long two-segment chain | 21 / 37,196 | 16 / 24,803 |

  Source-derived eliminated mirror-byte appends (not total measured memory
  traffic) are respectively 48/48/36/136 for short parse/clone/append/chain and
  4,132/4,132/4,120/12,388 for long inputs. Allocation bytes include terminators.
  Injected parse/clone/rebuild allocation failures preserve cleanup and leave
  no audited allocation live. Installed-consumer and broad validation pass.
  The separately diagnosed implicit-field spelling defect is repaired in
  0.367.1: `return field.view()` and `return this.field.view()` retain the same
  checked receiver summary. Shadowing and lifetime rejection remain enforced;
  this does not broaden the single-origin relay contract.
- **Original client/evidence:** `url` kept strings plus `scheme_view_data`,
  `host_view_data`, and `rendered_view_data` with byte-copy loops in
  [`http.gti`](../../stdlib/std/http.gti). Its issue-#13 comment predates the
  implemented [nested text relay](../../tests/fixtures/text_owner_relay.gti).
- **Steps:** return views of the existing owned fields; remove only the three
  mirrored buffers and their maintenance. Keep `string()`/`clone()` ownership,
  URL parsing, encoding, chaining and rendering contracts unchanged. Audit the
  separate issue-#12 public construction-bridge workaround with a focused
  private-factory probe; a remaining construction defect is not permission to
  infer that visibility can already be tightened.
- **Boundary:** preserve receiver-origin and full-owner invalidation. No
  unchecked pointer view, temporary-owner escape, independently mutable
  backing, or URL-name compiler exception. If this exact composition fails,
  minimize the general field-origin/lowering defect before rewriting around it.
- **Exit:** parse/clone/append/render output equivalence, embedded-byte policy,
  view lifetime negatives and failure cleanup. Record allocation and copied-byte
  counts before/after, including long URLs; latency is supporting evidence, not
  an assumed speedup. Private bridge cleanup is a separately evidenced substep.

### L-UP-03: Move Owned Transformation Results

- **Implementation (2026-10-04):** `transform` explicitly moves its owned
  mapper result into the destination and requires `movable<T>`. User resource
  and string fixtures prove unchanged source values, zero clones and exact
  replacement drops. Failure fixtures cover mapper failure, replaced-element
  destructor failure and undersized destination cleanup across standards,
  optimization, no-elide and sanitizers; overlap, read-only output and wrong
  result types remain frontend errors. Example 101 is the asset-build client.
  Broad campaign integration and the installed client pass.
- **Original client/evidence:** [`algorithm.gti`](../../stdlib/std/algorithm.gti)
  constructed an owned `produced` then copied it into `destination[index]`;
  `transform` consequently required `copyable<T>`.
- **Steps:** move that owned local into the destination and express only the
  actual construction/assignment requirements. Prove the rule with a
  user-defined move-only resource and mapper, then owning strings.
- **Boundary:** source access remains read-only; this does not move from a
  dynamic indexed element or broaden the existing same-element-type API.
  Same-owner source/destination conflict and
  undersized-destination behavior remain checked.
- **Exit:** correct output, zero accidental clones, exact replacement drops,
  mapper and old-element destructor failure cleanup, and invalid overlap
  rejection. Do not loosen the concept unless the implementation proves it.

### L-UP-04: Make Existing Method Contracts Explicit

- **Implementation (2026-10-04):** `value_or` declares
  `requires std::copy_constructible<T>`. Positive user-record and move-only
  optional-other-member fixtures pass the native matrix. The existing
  `GTI-S2029` facts locate failure at the call, retain the concrete required
  application and library declaration, and explain that assignment is not
  required. Frontend/LSP tests cover the same spans and facts, UTF-16, absence
  of an unsafe fix, incomplete member completion and correction. No new
  compiler diagnostic or editor syntax is needed. The ordinary-method audit
  found cloning, container resize/fill, callable copy-erasure construction and
  replacement, ordering/equality clients and map policy construction already
  constrained. Pack emplacement and conditional operators remain separate
  rows, not additional `value_or` exceptions. Broad integration passes.
- **Original client/evidence:** [`optional.gti`](../../stdlib/std/optional.gti)
  documented the copy requirement of `value_or`, but let a failed copy surface
  from inside its body. Current requirements and `GTI-S2029` already explain
  attributable source contracts.
- **Steps:** start with the actual copy-construction requirement on `value_or`,
  not unnecessary copy assignment. Audit ordinary methods in existing value,
  container and callable APIs for similarly unstated expressible requirements;
  split any independent family into a bounded follow-up rather than inventing
  missing parameter-pack or arbitrary expression predicates.
- **Boundary:** move-only optional remains usable through its other methods.
  Explicit clone/reset/emplace policy is unchanged. Do not synthesize custom
  lifecycle bodies or schedule implicit optional copying as a diagnostics fix.
- **Exit:** positive copyable and negative move-only user types; primary error
  at the attributable argument/call with related contract declaration; identical
  structured CLI/LSP facts and completion continuing on erroneous source.

### L-UP-05: Remove Comparator Sort's Quadratic Workaround

- **Implementation (2026-10-04):** iterative source-defined heapsort retains
  one mutable owning comparator, checked element access and copyable elements.
  Neither whole-vector sort had a documented stability guarantee; their
  instability is now explicit. Stable list sorts are unchanged. A frequency
  oracle verifies the permutation independently from ordering for empty,
  singleton, reverse, sorted, equal, organ-pipe, nearly sorted and shuffled
  inputs in both directions. Every case checks a `2*n*(ceil(log2(n))+1)`
  comparison bound and zero allocations within the sort. Comparator state and
  exactly-once cleanup survive normal completion, comparison failure and
  destructor failure across both standards/optimizations, no-elide and
  ASan/UBSan.
  Reverse-input comparisons for n=128/256/512/1024 changed from
  8,128/32,640/130,816/523,776 to 1,294/3,106/7,203/16,407. Source-level copies
  and assignments are bounded per sift step, hence O(n log n), rather than
  per inversion; these are not instrumented custom copy-constructor counts.
  Matching pre/post O3 binaries of the 48-case oracle (before adding its
  allocation/bound assertions), one warmup and nine interleaved runs on macOS
  arm64, measured median process times 8.971ms/3.059ms, ranges
  8.881–9.467ms/2.924–3.251ms, and executable sizes 509,920/512,064 bytes.
  These short process-level timings are supporting evidence, not a general
  application speedup claim. Separately, three current Release-compiler O3
  measurements of the instrumented fixture during broad-test contention gave
  medians 7.399s for C++ emission and 9.798s for the complete native build;
  these include contention and are not an isolated compile-time comparison.
  The source algorithm digest is
  `e4545de11a8ad92f45e53f7b313b8a068fa66991c3d769cc64d70c3f08216ca8`
  on base `4ea7d42e` plus this campaign's changes, with AppleClang 21 and LLVM
  22.1.8. Broad campaign integration and the installed client pass.
- **Original client/evidence:** comparator `sort` in
  [`algorithm.gti`](../../stdlib/std/algorithm.gti) used insertion sort to
  avoid recursive forwarding of an owned comparator.
- **Steps:** freeze current ordering, stability promise and comparison/copy
  counts. Implement iterative heapsort or bounded iterative introsort while
  retaining one `mut Compare`. Keep the copyable-element boundary for this row;
  no callable-reference feature or indexed movement is required. Preserve any
  documented stability contract, or explicitly decide it before changing it.
- **Boundary:** comparator signature, repeated mutable invocation, permutation,
  borrow exclusivity and defined-failure behavior remain exact. Native C++ sort
  must not become execution authority.
- **Exit:** independent sorted/permutation oracle; empty, equal, reverse,
  nearly sorted and adversarial inputs; owning comparator state and failure
  cleanup. Comparison growth must establish O(n log n) scaling, with
  allocations/copies and compiler/runtime cost recorded separately.

### L-UP-06: Adopt Eligible Pure Constant Evaluation

- **Implementation (2026-10-04):** duration/time-point constructors, read-only
  comparison and Unix observers now use the existing `constexpr` profile.
  The frontend fixture records exactly 12 staged MIR executions, covering
  signed extrema, negative floor/remainder, constructor and receiver identity;
  runtime results match in O0/O1/O3 and C++20/C++23. Host clock reads remain
  rejected in constants; depth/step exhaustion remains `GTI-S2057`.
  Example 102 composes frame budgets with passive asset metadata. Direct
  observation of a returned duration temporary remains a staging limitation;
  binding it to an ordinary local works without a new interpreter path.
  Scalar numeric helpers, pair and fixed-array observers already have constexpr
  adoption; allocation, checked-result elapsed subtraction and mutable
  receivers have not been relabeled constexpr. Broad integration and the
  installed frame-budget client pass.
- **Original client/evidence:** passive duration/time-point constructors and
  scalar observers in [`chrono.gti`](../../stdlib/std/chrono.gti) were not constexpr,
  despite the implemented record/constructor/read-method constant profile.
- **Steps:** begin with `nanoseconds` construction/count, then eligible pure
  time-point construction, comparison and Unix decomposition. Build a small
  compile-time frame-budget/asset-metadata example. Inventory other pure value
  helpers only after this client passes; arrays, pair and numeric helpers
  already have substantial constexpr adoption.
- **Boundary:** clock reads, allocation, owning strings, checked-result
  Expected execution and mutable receiver methods do not become constant
  merely by adding a keyword. Follow [`L-CONST-01`](#l-const-01-remaining-bounded-compile-time-programming):
  semantics decides eligibility and staged canonical MIR performs execution.
- **Exit:** compile-time/runtime parity, signed boundaries, pre-epoch floor and
  remainder cases, shared budget/depth failures, exact identities and proof that
  tests exercised the MIR evaluator rather than native C++ constexpr behavior.

### L-UP-07: General Restoring Exchange And Swap

- **Complete in 0.369.0:** `codex/restoring-exchange` adds
  source-defined utility wrappers over sealed MIR restoring transfers. Focused
  scalar/resource, loan-rejection and verifier-mutation tests pass, as does the
  replacement/cleanup failure matrix (C++20/C++23, O0/O1/O3, disabled elision,
  ASan/UBSan). The complete eligibility review repaired intrinsic-result
  transport for owning `expected` values using its existing result-slot
  protocol. Raw owning and cleanup-free move-only fixed-array exchange now
  use explicit result construction; owning error alternatives and payload enums
  also pass the shapes matrix (both standards, O0/O1/O3, no-elide, sanitizers).
  Array pending-return cleanup and double failure pass the same matrix.
  Shared ownership, optional engagement transitions, projected fields and
  indexed places have additional regression coverage. Dynamic text-view
  exchange rejects during semantics instead of failing MIR verification.
  Compiler pipeline, focused LSP/formatter recovery, and the 101-example
  native corpus oracle pass. Broad verification repaired a duplicate
  passive-array constructor publication. Directly returned, discarded and
  by-value-consumed raw-array
  results now have production transport, with a later-argument failure fixture
  and independent publication/transfer mutations passing focused validation.
  The combined runtime fixtures pass 176 outcomes across the eight native
  modes. A clean staged install passes all 11 library consumers, both new
  examples and the feature LSP check. All 404 broad CTests and four separate
  corpus gates pass on the final implementation. Examples 103/104, editor
  checks and canonical contracts accompany the feature. Reconciliation with
  Main's diagnostics and issue 30–33 fixes passes all 412 combined gates,
  including targeted reruns after the checked-update/subscript repairs.
- **Pre-implementation adoption audit (2026-10-04):** a new semantic capability, not a
  stale library workaround. A current optimized-compiler probe over an
  unrelated `exchange<T>(mut T&, T)` rejects `std::move(target)` with
  `GTI-S2018` (cannot consume a reference). The public utility declarations
  remain bodyless. Existing relocation does not establish restoration or
  two-place disjointness, so row 8 is not newly unlocked.
- **Original client/evidence:** generic `swap`/`exchange` in
  [`utility.gti`](../../stdlib/std/utility.gti) are declarations, not complete
  implementations. Existing reborrows do not permit arbitrary consumption
  through `mut T&`.
- **General rule/steps:** represent a bounded exchange of an initialized
  writable place with a prepared replacement. State origin, permitted access,
  escape boundary, endpoint and transfer/drop identities. Stage replacement
  before retiring old liveness; transfer the old value without destroying it;
  publish the new live value only on success. Then define two-place swap,
  including exact self/alias policy and required disjointness proof.
- **Ownership:** semantics owns the protocol; HIR carries exact selected places
  and construction; MIR verifies restoration and normal/failure obligations;
  LoweredProgram preserves those facts. Backend casts or temporary reference
  consumption cannot establish safety. Reuse existing owned transfer operations
  where sufficient rather than creating a second lifecycle engine.
- **Exit:** unrelated owning Resource plus passive values; aliases, overlapping
  loans and forbidden escapes rejected; exact self behavior; construction,
  movement and secondary cleanup failures. Independent verifier mutations must
  reject missing restoration, forged identity and premature publication.

### L-UP-08: Checked Indexed Exchange And Move-Only Algorithms

- **Complete in 0.369.0:** a sealed indexed-swap intrinsic,
  checked `swap_elements` methods, array swap and source-defined move-only
  heapsort/reverse execute through verified MIR in `codex/restoring-exchange`.
  Move-only resource/string arrays
  and vectors pass the runtime matrix, including left/right/equal-invalid
  bounds, comparator panic, partial element construction and consuming wrapper
  constructor failure. Owning fixed-array argument transport now retains the
  verified lifetime slot instead of default-constructing/copying an SSA array.
  Independent mutations reject lost publication/transfer evidence. Ordering,
  permutation, duplicates, zero-clone and comparison-budget fixtures pass the
  same matrix, including zero-length array swap. Passive record-array
  construction now has an explicit MIR field write, preserving constexpr
  construction through the existing MIR evaluator rather than adding AST
  movement interpretation. Installed-consumer verification and all 408
  registered gates pass, including four separately run corpus gates.
  The final diagnostics/issue reconciliation passes all 412 combined gates,
  including targeted reruns after the checked-update/subscript repairs.
- **Original prerequisite/client:** `L-UP-07`; the bodyless
  [`array::swap`](../../stdlib/std/array.gti) and copy-based vector
  `reverse`/`sort` are the public clients.
- **Steps:** establish a sealed exchange of two initialized checked indices,
  validating both bounds before mutation and proving disjointness or exact
  self-alias. Preserve the initialized-prefix invariant without an observable
  hole. Adopt array swap, vector reverse, then sorting one substep at a time;
  reuse `L-UP-05` where completed, but that source-only sort upgrade is not a
  prerequisite for the exchange proof.
- **Boundary:** structural eligibility and conservative owner loans remain;
  this is not permission for arbitrary `std::move(values[i])`, iterator escape
  or mutation while an element reference is live. Storage relocation alone
  does not provide swapping semantics. Replace copied pivot/element staging,
  not only assignments: move-only ordering inspects eligible read-only
  references. Reuse the algorithmic structure of `L-UP-05`, not its copyable
  implementation unchanged.
- **Exit:** move-only resource/string arrays and vectors; exact moves/drops,
  zero clones, alias/bounds/borrow negatives and failure cleanup at every
  transfer stage. Sorting retains ordering/permutation and comparison evidence.

### L-UP-09: Preserve Loans When Extracting Borrowed Results

- **Completed before this campaign:** released as 0.366.0 in `77b83a8f`,
  reconciled on main by `8b9252e3`. Prefix `?` and consuming `.expect` preserve
  the external owner's read-only loan across carrier cleanup without moving
  or copying the referent. Mutable successes, stored fallible carriers and
  borrowed combinators remain outside the bounded contract. Example 99,
  semantic/IR mutations, LSP tests and 42 runtime outcomes across seven native
  configurations cover this slice; see the canonical Expected contract.
  The original prerequisite description below records the design rationale,
  not a remaining absence of these two extraction operations.
- **Original client/evidence:** [Expected](../language/expected.md) admitted
  checked read-only `expected<T&, E>`, but `?`/consuming helpers required owned
  success alternatives. Borrowed lookup pipelines therefore retained manual
  carrier checks and `.value()` extraction.
- **General rule/steps:** select a bounded read-only success family with an
  eligible owned error. Explicitly transfer the referent origin/path, complete
  owner dependencies and loan endpoint across extraction and wrapper cleanup.
  Start with prefix propagation; admit `expect` only when it preserves that
  same rule. An unrelated user-defined checked lookup must prove the contract.
- **Boundary:** never move the referent or leave a loan anchored only to a
  consumed wrapper. Error propagation preserves exact E and cleanup; the error
  branch cannot publish a success loan. Reference-producing transforms,
  mutable results and wider borrowed-state alternatives need separate evidence.
- **Exit:** safe owner-tied local/return composition, rejected local-owner and
  temporary-owner escape, rejected owner move/mutation during the loan, once-only
  cleanup and independently verified origin/endpoint transport. LSP must explain
  an invalid lifetime rather than publish a backend internal error.

### L-UP-10: Close Borrowed Constructor-Pack Staging

- **Current-head check (2026-10-04):** the optimized 0.366.0 compiler still
  rejects `forward_list_emplacement_borrowed.gti` during C++ emission of the
  concrete `make_unique` body (`GTI-B0001`). The completed constructor-field
  publication fix does not close this separate pack composition. Rows 11/12
  are therefore not newly unlocked source-only adoptions.
- **Client/evidence:** the retained
  [`forward_list_emplacement_borrowed.gti`](../../tests/fixtures/forward_list_emplacement_borrowed.gti)
  is recorded as failing constructor-field backend admission in the
  [existing emplacement checkpoint](#active-request-complete-the-existing-standard-library).
  Reproduce on current main first; that historical failure is not a fresh test.
- **Steps:** reduce to an unrelated generic node constructor forwarding an
  owned pack element into an exact read-only constructor argument. Repair only
  the missing general selected-constructor/call-input representation. Carry
  borrow roles, argument order, pack place identities and lifetime obligations
  through HIR, verified MIR and lowered representation.
- **Boundary:** no native variadic forwarding fallback, extra move, missing
  owning-pack drop, or a forward-list-specific exemption. If already repaired,
  close this row with current passing evidence and reuse it in the next row.
- **Exit:** retained fixture enters the passing runtime matrix; empty/mixed/
  repeated pack forwarding, immovable final fields, later-argument/constructor/
  destructor failures and forged borrow/move/order identities all have coverage.

### L-UP-11: Adopt Node-Container Direct Emplacement

- **Prerequisite/client:** `L-UP-10`; `map`/`unordered_map` currently construct
  a completed mapped value then move it through entry/node stages in
  [`map.gti`](../../stdlib/std/map.gti) and
  [`unordered_map.gti`](../../stdlib/std/unordered_map.gti).
- **Steps:** reuse ordinary constructor-local packs and tagged in-place
  final-field construction; remove the redundant V temporary in one container,
  then the other. Preserve key ownership, comparator/hash exactness, stable
  addresses and success-only node publication.
- **Boundary:** duplicate `try_emplace` must not construct V, but invocation
  arguments still evaluate eagerly. Do not promise lazy argument evaluation,
  change copy/clone policy or redesign the container's representation.
- **Exit:** immovable mapped Resource at a stable final address; owning keys,
  borrowed constructor inputs, duplicate insertion and rehash/tree rotation;
  allocation/constructor/cleanup failures. Count constructors, moves and
  allocations, and prove no unpublished entry survives a failed insertion.

### L-UP-12: True Sparse And Inline Emplacement

- **Prerequisite/client:** `L-UP-10` plus a general final-slot protocol. Public
  [`storage::construct`](../../stdlib/std/memory.gti) accepts a completed
  payload; [`optional::emplace`](../../stdlib/std/optional.gti) deliberately
  constructs a temporary before one-slot publication. The existing optional
  capability rejects hidden constructor targets.
- **Steps:** audit the selected-constructor path of private sparse storage;
  expose a bounded source-policy emplacement using exact constructor identities
  and verified argument stages. Then extend that same initialization/rollback
  contract to an empty inline slot, eliminating intermediate movement for
  currently eligible optional payloads. Do not make a variadic intrinsic
  secretly execute a source constructor or invent another representation-specific
  lifetime system.
- **Boundary:** optional resets the old value before construction and remains
  empty on failure; preserve its original failure record and partial-field
  cleanup. Allocation, argument evaluation and construction are distinct
  stages. Keep the present movable-element constraints in this first slice;
  `L-UP-18` owns immovable storage/inline eligibility. Vector growth only admits
  immovable elements if a separate relocation proof exists; inplace construction
  cannot imply relocatability.
- **Exit:** zero-intermediate-move final-slot construction, empty/engaged reset,
  partial construction and secondary cleanup failures; poisoned publication/order/
  constructor/drop metadata rejected. Compare temporary/move counts. Optional
  copy and assignment policy is explicitly not expanded by this row.

### L-UP-13: General Formatter Contracts And Useful Type Errors

- **Implemented bounded slice:** [`format.gti`](../../stdlib/std/format.gti)
  exposes source-defined `std::formatter<T>`, `std::format_spec`, and
  `std::formattable<T>`. Built-in and application specializations use one exact
  safe read-only instance method returning `expected<void, format_errc>`.
  Missing static formatters are ordinary source contract errors, including
  unused later heterogeneous pack arguments. Format/print/panic twins all use
  the same implementation and retain dynamic error categories.
- **General rule:** exact argument-bearing member requirements accept checked
  read-only or mutable receivers and exact value/read/mut parameter types.
  Dependent compound concept arguments use canonical substitution; constrained
  call-pack folds validate every concrete element through ordinary function
  viability. Symbolic generic bodies consume these same signature facts.
  Unrelated counters/policies prove the rule without stdlib names. No static
  member-query, general expression probing, reference pack, associated-type
  inference, or overload ranking was needed or added.
- **Boundary:** preserve counted text, ordinary source grammar/parser ownership,
  exact return/borrow modes, dynamic format errors and panic-versus-try policy.
  Decide borrowed adapter inputs explicitly; avoiding a clone is not permission
  to retain a caller's owner. Do not expose the private detail namespace as the
  supported extension API or recognize `std::format` in semantics.
- **Evidence:** runnable examples 105/106, compiler signature/negative tests,
  dynamic custom formatter rejection and move-only exactly-once cleanup,
  native optimization/standard/sanitizer matrix, CLI/LSP argument attribution
  and edit recovery, Tree-sitter corpus, and whole-example native corpus oracle.
  Installed consumer validation passes in the combined 0.371.0 slice. Dynamic
  unsupported presentation remains a runtime domain error; literal validation
  is the next distinct row.

### L-UP-14: Validate Literal Patterns Through One Parser

- **Adoption audit (2026-10-04):** passive text/record constexpr execution is
  available (and used by row 6), but `validate_format_pattern` returns Expected
  and current required constant evaluation does not execute that general
  result shape. There is no general required call-site literal-validation
  contract. Adding constexpr to a wrapper alone would not make invalid
  literal calls diagnostic, so this remains a new-capability row.
- **Prerequisite/client:** the current literal-text/passive-record MIR profile.
  Substep A is a shared constant-eligible validator; substep B's call-site
  validation requires A's passing evidence. Adopt the general explicit
  call-site constant-validation rule with that client, not as unowned future work.
  `L-UP-13` is needed only if the accepted contract also validates user argument
  adapters; pure pattern validation need not wait for formatter customization.
- **Steps:** isolate pure pattern validation from output allocation while
  retaining the same grammar and error categories. Establish a constant-eligible
  passive result rather than assuming Expected/packs already execute in CTFE.
  Prove compile-time/runtime parity, then adopt the bounded call-site/literal
  carrier contract and make invalid literal patterns source diagnostics.
- **Boundary:** current `constexpr` alone does not require literal arguments to
  be evaluated at a call. The rule must apply to an unrelated validated-literal
  API, run at the correct semantic query stage using canonical verified MIR,
  and preserve deterministic resource limits. No second format grammar inside
  the compiler, optimizer-only check, or native C++ compile-time authority.
- **Exit:** invalid brace/index/specifier/argument-count cases at source spans,
  valid literal examples and identical dynamic `try_format` errors; custom
  adapter cases when in scope. Dynamic strings remain supported and recoverable.

### L-UP-15: Conditionally Available Member Operators

- **Adoption audit (2026-10-04):** still explicitly rejected by semantic
  declaration analysis with `GTI-S2049` for an operator's trailing requirement.
  Existing ordinary-method constraints used by `get_or_insert` do not remove
  that boundary; row 4 does not silently widen it.
- **Client/evidence:** associative containers supply `get_or_insert` guarded
  by `default_initializable<V>`; conditional `operator[]` remains excluded by
  [concepts](../language/concepts.md) and
  [`L-CONT-02`](#l-cont-02-associative-containers).
- **General rule/steps:** extend exact validity-only requirements to bounded
  non-polymorphic member operators; preserve the condition through selection,
  concrete instances, MIR/lowering and tooling. Prove a generic user-defined
  operator family before adding map indexing.
- **Boundary:** no constraint-based ranking, C++ implicit key conversion or
  global default-construction requirement. Indexing inserts only when the
  source API's exact default-value contract applies; `at`/lookup and explicit
  fallible insertion retain distinct meanings. Review eager key ownership and
  returned exclusive-loan restrictions in the public example.
- **Exit:** default-constructible and non-default/move-only mapped types;
  missing/duplicate keys, exact key matching, forbidden overlapping mutation,
  attributable diagnostics and coherent operator representation. Conditional
  special-member copying is not implicitly part of this feature.

### L-UP-16: Precise Borrowed Projections Before Proxy Ranges

- **Adoption audit (2026-10-04):** an unrelated `PairView` storing two spans,
  returning only its left element, still prevents mutation of the right owner
  while that projection is live (`GTI-S2017`). This current-head probe confirms
  conservative multi-owner retention remains; row 2's single-owner relay is
  not evidence of precise projection summaries.
- **Client/evidence:** [`paired_span.gti`](../../stdlib/std/paired_span.gti)
  provides indexed views; its projections conservatively retain both owners.
  Canonical [ownership](../language/ownership-and-lifetimes.md) does not infer
  arbitrary callee-internal field transforms or extract a reference path from
  a multi-path by-value carrier.
- **General rule/steps:** begin with an unrelated two-owner PairView. Define
  semantic origin-plus-projection summaries carrying access, complete owner
  dependencies, loan endpoint, physical carrier lifetime and body-change
  invalidation. Preserve them through HIR/MIR; release only genuinely unrelated
  dependencies. Only the projected result's dependency set narrows: a still-live
  complete carrier or full copy continues to protect both owners. Adopt
  independently useful read-only paired-span projections.
- **Boundary:** no mixed/exclusive multi-origin expansion, mutable zip or
  backend-inferred referent. Zip-style proxy values require a separate general
  iterator-yield contract: current structural dereference expects a checked
  reference. Projection precision alone does not make zip iteration possible.
- **Exit:** unrelated-owner mutation allowed only after the last full-carrier
  use and the projection proof; reject mutation when the complete carrier is
  used again after projection. Reject actual owner invalidation/escape and
  verify copies/moves/returned carriers and summary-cache changes. Broader proxy
  range work needs a named next client and its
  own row; do not hide it in this adoption.

### L-UP-17: Recoverable Owned Allocation

- **Adoption audit (2026-10-04):** `make_unique` still calls the terminal
  declaration-bound `allocate_unique_owner`, and no recoverable unique-owner
  factory or corresponding status-returning publication path exists. Typed
  recoverable storage allocation is real, but cannot safely adopt raw storage
  into a unique owner. This remains a distinct capability, not a source-only
  `try_` alias.
- **Client/evidence:** `try_storage` reports typed allocation status, but
  `make_unique` is still type-level infallible and `try_make_unique` is absent.
  This is the recoverable-factory sub-slice of
  [`L-OWN-01`](#l-own-01-shared-and-weak-ownership), not a second ownership plan.
- **General rule/steps:** distinguish recoverable allocation failure from
  constructor/argument defined failure; preserve exact selected construction,
  unpublished owner state and failure rollback. Add a declaration-bound typed
  owner-allocation result path if ordinary GTI cannot safely express it.
  Co-deliver `try_make_unique` with the existing factory as the eventual parity
  target and an unrelated fallible application owner. State the
  first supported constructor shapes explicitly: a completed movable-value
  starting slice must record direct immovable construction and other pack
  compositions as remaining parity work, not claim full factory coverage.
- **Boundary:** no public raw-pointer adoption, implicit copy hooks or swallowed
  constructor panic. Constructor-pack families depend on `L-UP-10` when their
  exact composition is involved; a completed-value bounded factory need not
  wait for inline emplacement. Shared promotion/weak lifetime and object/control
  block co-allocation require separately evidenced substeps of `L-OWN-01`.
- **Exit:** forced allocation failures, size/alignment status, once-only
  arguments, constructor/secondary-cleanup failure, no leaked unpublished
  allocation, exact Expected transport and installed consumers. Memory pressure
  is not a reliable substitute for deterministic failure injection.

### L-UP-18: Immovable Payload Eligibility After Direct Construction

- **Fresh-result prerequisite delivered (#150):** exact same-type wrappers of
  class-valued constructions/calls now denote their producer directly; local
  and return publication can therefore retain an immovable object's final
  address without an extra lifecycle move. This does not make a completed
  immovable object movable, remove container constraints, or deliver the
  optional/storage parity goals below. Remaining fresh-result breadth includes
  conditional/comma result construction and immovable by-value parameter
  transport. The latter needs a verified final-place parameter storage protocol
  with stable identity through later argument failure and callee cleanup;
  acceptance must prove address identity and once-only cleanup, not merely
  relax the current movable argument check.
- **Prerequisite/client:** `L-UP-12`. The public storage owner requires
  `movable<T>`; public/private optional requires `move_constructible<T>` and its
  private traits currently permit generated movement. Direct emplacement alone
  cannot remove those restrictions. The clients are a fixed-address Resource
  in stable allocated storage and a locally constructed inline optional of it.
- **Steps:** first separate stable allocation-owner movement from element
  movement. Widen only eligible storage elements and constrain operations that
  actually relocate them. Then decide and represent the inline wrapper's
  generated lifecycle: an engaged optional cannot move an immovable T, so its
  movement must remain unavailable. Co-deliver empty/engaged observation,
  reset/direct emplacement and exactly-once drop without by-value transport.
- **Boundary:** this is a general storage/lifecycle eligibility extension, not
  `L-UP-15`'s conditional ordinary operators. Preserve structural type facts
  through semantics, HIR/MIR and lowering. Do not mechanically remove both
  constraints, hide a native copy, add custom copy/move bodies or alter explicit
  clone policy. Broader optional assignment is a separate decision.
- **Exit:** prove stable owner movement leaves element addresses untouched;
  inline wrapper movement/relocation is rejected. Test unrelated immovable
  owners, construction/reset/secondary failure cleanup, forged lifecycle facts,
  and generic/installed compositions within the stated non-transporting profile.

### Measured Follow-Ups, Not Automatic Rewrites

| Candidate | Evidence needed before scheduling | Hard semantic boundary |
| --- | --- | --- |
| Callback small-buffer storage / borrowed `function_ref` | Measure model allocation, registration, clone/reset and invocation separately against direct constrained generic calls; owning wrappers in [`functional.gti`](../../stdlib/std/functional.gti) allocate today. | Inline erasure needs target layout/alignment, move/clone/drop and allocation fallback. Borrowed erasure additionally needs exact origin/escape proof. Existing exact-receiver optimization does not automatically devirtualize owning allocated models. |
| Bulk byte I/O / response filling | Allocation/copied-byte and throughput evidence for [`cstdio.gti`](../../stdlib/std/cstdio.gti)'s bounded stack-buffer/per-byte append path and real HTTP clients. | Writable capacity, initialized length, native write bounds and success-only prefix publication must be represented; no exposing untracked storage or losing TLS/error policy. |
| Flat/pool container representations and shared-owner co-allocation | Allocation/cache/lookup measurements against current AVL/bucket implementations and separate shared object/control-block allocation. | Stable entry addresses, immovable elements, borrow invalidation, last-owner cleanup and future weak-owner lifetime remain authoritative. Indexed links are not inherently a language workaround. |
| Cached/preparsed formatting and lower-cost source locations | Measure repeated pattern parsing/argument scans and owned location construction; separate runtime cost from compiler latency. | One parser, bounded CTFE profile and source-owned representation; literal descriptor lifetime/pinning is not interchangeable with owned `source_location` or hidden caller tokens. |
| Further numeric/SIMD, coroutine/async, module linkage or concurrency breadth | A real client plus existing row prerequisites, not C++ API parity or LLVM availability. | Keep exact arithmetic/failure, target support, ownership and published concurrency contracts; the existing scoped-thread/weak-owner/freestanding gaps are not closed by adoption. |

### Common Implementation And Completion Gates

1. Reconcile current main, preserve unrelated work, and reproduce the selected
   workaround with a compiler built in Release, RelWithDebInfo or MinSizeRel.
   Freeze old observable behavior and operation/allocation counters before
   changing code. A source comment, declaration or this review is not runtime
   evidence that a composition works or still fails.
2. For an adoption row, try ordinary GTI first. For a new compiler facility,
   state the rule without a stdlib name, prove it using an unrelated user type,
   and carry one authoritative set of identities/lifetime/failure facts through
   the applicable phases. Unsupported source gets an owning-layer diagnostic,
   not accepted syntax followed by `GTI-B0001` or an AST/native escape hatch.
3. Add positive and negative contracts, independent verifier mutations for
   changed proof metadata, and once-only movement/drop/failure tests. Exercise
   selected pairs/triples such as generic + move-only + Expected, pack + borrow
   + cleanup, and view + mutation + temporary with the
   [interaction facilities](../architecture/verification.md#failure-capture-and-opt-in-fuzzing).
   Extend that bounded generator only for the row's actual supported grammar;
   retain and minimize failures instead of silently filtering them out.
4. Run focused compiler/library and C++20/C++23 O0/O3 execution gates, relevant
   no-elide/sanitizer cases, syntax/formatter/LSP checks when surfaces change,
   and an installed consumer plus a runnable user example. Validate performance
   using semantic counters and controlled measurements, not shorter source or
   presumed native optimization. Broader relevant optimized CTest and corpus/
   surface registration gates are required before declaring a row complete.
5. Update canonical contracts when behavior changes, the restriction ledger
   when a gap closes, and this row with exact scope/evidence and remaining
   boundaries. Follow normal compatibility/VERSION policy for observable
   changes; initiating the normal release path does not require waiting for
   asynchronous CI/CD. This documentation-only review changes no runtime API
   and requires no VERSION advance.

**Recommended next implementation prompt:** `L-UP-10`, the retained compiler
correctness prerequisite before more emplacement APIs are built around the
unresolved borrowed-pack composition. Rows 1–6 no longer need adoption work;
`L-UP-07/08` now implement the independent exchange and move-only algorithm
capability, with local completion evidence recorded above.

## Release Bug Closure

**State:** the six open Gitea bug reproductions (#20, #23, #25, #26, #28, #29)
are fixed and locally validated. All 389 optimized CTest gates have passing
combined evidence after correcting the new regression's census registration;
fresh installed-consumer and installed panic/LSP checks also pass. The user has
authorized closing the bugs and publishing 0.363.0, superseding the earlier
publication hold. Gitea access is restored; reconciliation on 2026-10-04
confirmed unchanged remote main and the same six open issues. Fix commit
`318051e8` carries their closing references, and merge `a610575c` preserves the
user's main-branch documentation work. The canonical main push dispatches the
0.363.0 release; no CI wait or manual competing tag is required. Active
installation is unchanged. Earlier publication holds below are historical.

The fixes retain verified-MIR execution: concrete type-only alias identities,
shared nested fixed-array receiver access, passive record copy/staging, exact
class-returning callback forwarding and temporary by-value operator arguments.
No new ownership rule, source fallback or public-library-name special case is
introduced. The original Tea snapshot builds after the deliberate `try_format`
API migration. Current evidence and platform limits are recorded in
[`verification.md`](../architecture/verification.md#release-bug-closure-after-panic-adoption).

## Panic Adoption In Formatting And Stdout

**State:** implementation and local validation complete, held with
the unreleased 0.363.0 foundation below. This is ordinary source-library policy
over the already verified terminal/caller contracts, not a new compiler feature.

The named client is ergonomic diagnostic/output code: `format` returns an
owning string, `format_to` returns void, and formatted `print`/`println` terminate
with a useful domain reason at the caller if they cannot complete. The
authoritative recoverable implementations are `try_format`, `try_format_to`,
`try_print`, and `try_println`; the last two now report native submission errors
as well as formatting failures. Existing raw output no longer drops host status.
Single-text-argument output remains literal, including for the `try_` siblings.

File and network operations, parsing external input, randomness and thread
startup remain expected-first. Checked container preconditions already have
specific defined-failure identities and are not relabelled as generic panic.
Compile-time format validation and customization are separate general facilities,
not introduced through public-name magic. `panic_adoption_runtime` covers both
C++ standards, O0/O2/O3, no-elide and sanitizers, including closed-pipe errors,
owning arguments/results, transactional append, reverse cleanup and exact caller
locations. Canonical contracts and migration are in [I/O](../language/io.md).

Completion evidence: all 386 optimized CTest gates have passing combined
results after reviewing the formatting example's inventory changes. The final
nine-gate rerun covers both census and executable corpus gates, the 144-outcome
adoption matrix, panic contracts, compiler pipeline, source-surface manifest and
LSP protocol. Isolated installed-consumer, reference and syntax-tooling checks
also pass. Publication and active installation remain held. Exact accounting
and platform limits are in
[`verification.md`](../architecture/verification.md#panic-adoption-in-formatting-and-stdout).

## Panic, Expected Ergonomics, And Constraint Diagnostics

**State:** implementation and local end-to-end validation complete for 0.363.0;
publication and active installation held at the user's request.
The user-facing clients are explicit terminal reporting with
`std::panic(message)` and formatted messages, consuming `expected::expect`,
`transform` and `and_then`, and actionable generic-constraint diagnostics that
identify the responsible argument without public standard-library-name rules.

The runtime uses a separately versioned bounded message record, snapshots the
message before cleanup, and preserves existing hosted and managed-thread
containment. Formatted panic initially uses the existing allocating formatter;
allocation-free message capture does not promise allocation-free argument
evaluation or user cleanup. That foundation left formatting/I/O return types
unchanged; the subsequent adoption slice above deliberately changes them.

General `[[noreturn]]` and `[[track_caller]]` declaration contracts must be
checked by semantics and retained through HIR/MIR. Caller-location tokens are
separate from detector effects: a tracked wrapper forwards a token, a detector
creates the original failure record, and propagation never rewrites it. The
first token consumer is explicit panic, not arbitrary checked operations.
Unsupported native callback transport must be diagnosed rather than losing the
hidden parameter. Exact function items stored in source-defined callable
wrappers preserve their selected target; an untracked wrapper establishes a
fresh source anchor, while an explicitly tracked wrapper forwards its incoming
anchor. Compiler/backend admission and independent verification remain mandatory.

Completion evidence: all 385 optimized CTest gates have passing results,
including reviewed test-only assertion/inventory refreshes after the broad run.
The panic matrix passes 312 outcomes and Expected passes 196, covering both
C++ standards, O0/O1/O3, no-elide and sanitizer checks. Terminal contract tests independently
reject forged caller metadata, ordinary return continuations and lost call-input
consumers; cleanup-owning arguments preceding a terminal argument now lower
through the same verified failure-cleanup graph. The representation suite covers
an unrelated primary/exact class family in a lexical-callable program, fixing
the native specialization omission exposed by formatted panic. All 11 staged
consumers, installed panic/Expected probes, installed contract-diagnostic LSP
checks, syntax-tooling, formatting and isolated local-installer package checks
pass. The minor version advance records the new surface; no release was
initiated. Detailed evidence and local-platform limits are recorded in
[`verification.md`](../architecture/verification.md#panic-expected-ergonomics-and-library-contract-diagnostics).

The general diagnostic foundation explains source-defined concept failures;
it does not introduce a public-name formatter rule, a new formatter protocol,
or literal-format validation. Those remain separate language/library work.

## Ephemeral Borrowed Receivers And PR Reconciliation

**State:** receiver implementation, validation and publication are complete at
`be149354` (0.361.0), following the 0.360.0 stabilization release at `61f3880a`.
PRs 17 (source modules) and 22 (discard quick fixes) are reconciled for 0.362.0
at `3c2fe8fd`, with all 381 CTests, staged consumers and editor/package gates
passing. PR 18's lambda error mappers were already integrated and validated in
`61f3880a`; the older branch is superseded, not replayed over newer lifetime work.
Only canonical publication and PR/verified-fixed-issue bookkeeping follow the
completed implementation and validation. The detailed evidence is in
[`verification.md`](../architecture/verification.md#combined-03620-release-validation).

The immediate client is an ordinary service facade:
`api.auth().create_access_token(...)`. A directly produced class/struct value
with one proven stable stored owner dependency may supply one immediate read
or mutable receiver. The receiver's physical loan must end after invocation;
the stored owner dependency must survive the carrier's full-expression drop,
including argument failure, callee failure, and propagation. The result may
not export a reference or borrowed dependency. Named carriers retain their
existing rules. This adds no syntax, library-name recognition, runtime lifetime
tracking, or general temporary-argument/reference binding.

Use the existing caller-visible borrow summary: a method returning a carrier
from a private field still protects its complete caller receiver unless the
existing summary identifies a more precise source expression. This work does
not claim new callee-internal return-place transforms. Physical shell loans
and stored dependencies remain distinct; a dependency is not a writable
physical reborrow parent.

Exit evidence: positive service/view and unrelated user-defined cases;
negative escape, multiple-owner, temporary-owner and overlap cases; independent
MIR mutations; exact normal and failure cleanup through the MIR-only backend;
compiler/LSP parity; C++20/C++23 at O0/O1/O3, no-elide and sanitizers. Verify
branch/short-circuit cleanup explicitly before declaring the capability done.
Use an optimized compiler throughout. Publish the validated feature commit
promptly to canonical `main` as 0.361.0, starting its automatic stable release
without waiting for CI. Then reconcile the open PRs as one coherent candidate.
The user narrowed this campaign to those two outcomes: unresolved issue fixes
and remaining branch/worktree cleanup are deferred. Preserve unvalidated drafts
and the active local installation; they are not part of this release. Panic APIs
and compiler stabilization are follow-up work, not an extension of this feature.
The high-level changelog proceeds independently; incomplete proposals are not
listed as shipped features.

Final receiver evidence covers all 379 registered CTests on unchanged optimized
production source, including successful retries of three timeout-only results,
plus 11 staged external consumers and nine clean staged execution cases. The
exact run accounting is recorded in
[`verification.md`](../architecture/verification.md#ephemeral-borrowed-carrier-receivers).

## Everyday Program Stabilization

**State:** implementation, combined release validation, local installation and
0.360.0 publication complete at `61f3880a`. The candidate reconciles canonical `main` through
`0ff67a4e`. This campaign closes
ordinary application composition gaps before adding more advanced surface.
Source diagnostics for invalid programs are part of every row, not a substitute
for implementing valid programs. Existing compiler-owned ownership, failure,
and MIR execution rules remain authoritative.

| Priority | State | Client and exit evidence |
| --- | --- | --- |
| 1 — construction and replacement | complete; combined validation passed | Nested owning objects and optional in-place construction, including mixed-argument fallible emplacement, preserve ordered initialization, exact first failure and partial cleanup. Reduced Tea-style regressions run through the production backend. |
| 2 — borrow composition | complete; combined validation passed | Ordinary helpers, lookup and tree traversal compose checked references/views without representation-only rejection; unsafe escape, overlapping mutable access and use-after-move retain precise source errors. |
| 3 — container/value-wrapper consistency | complete; combined validation passed | Movement, replacement, explicit cloning, emplacement and lambda error mapping work across everyday eligible optional/container/expected combinations, using structural library contracts rather than public-name compiler exceptions. |
| 4 — owning payload transport | complete; combined validation passed | Concrete generic wrappers and collections can carry owning event/message alternatives through verified construction, movement and exactly-once destruction. Unsupported structural boundaries remain explicit and diagnosed at source. |

The final focused pass covers the mixed constructor's 88 native outcomes,
64 direct/pack generated-transport outcomes, lambda mapper/compiler/LSP
matrices, and the constructor mutability advisory. In particular, staged owner
cleanup cannot replace an earlier capacity failure with a later destructor
failure. All compiler targets have been rebuilt with host optimization after
the interrupted session. A broad run exposed a concrete-pack pointer-input
compatibility regression; it was stopped before reconciling the newly merged
practical examples. The existing checked conversion predicate is now shared
by pack lowering and independent MIR verification; the original regression and
new positive/negative coverage pass. The fresh 377-test combined suite passes
with no failures or skips in 1617.18 seconds, including both executable corpus
oracles and the expanded 95-example inventory. All 11 clean staged consumers
and the staged carrier, lambda-mapper and mixed-emplacement probes pass.
Formatting and syntax-tooling checks also pass. Local installation and
canonical Gitea dispatch subsequently completed. Neither the interrupted run nor prior installed
checkpoints substitute for the final evidence recorded in
[`verification.md`](../architecture/verification.md#combined-03600-release-validation).

The following checkpoint narrative records development history. References to
pending integration below describe that checkpoint, not additional work beyond
the current status above.

Each row requires focused positive/negative tests and relevant compiler/LSP
diagnostic evidence using an optimized compiler. Integrated completion requires
the broader affected verifier, backend, corpus and runtime gates. SIMD and
native-bridge handoff work is independently audited before reconciliation; it
does not replace these four outcomes. The user also transferred structural
relocation into this campaign. Its candidate at `875f288d`, original follow-up
`6cf8e3d8`, and reviewed fail-closed/transactional hardening `596e4264` are
integrated locally. The client is checked transfer between typed-storage
owners and vector/string growth, without user-defined movement or raw-address
APIs. Reconciliation must preserve empty-destination preflight, exclusive
borrows, exactly-once cleanup, conservative relocation traits, and independent
MIR verification. SIMD and relocation's separately proposed serialization and
version changes used combined `mir-v82`/`mir-body-v69`; the accessor follow-up
advances this to `mir-v83`/`mir-body-v70` for exact stored-dependency edges. Integrated validation
is in progress, including nonempty SIMD-vector growth and shrinkage. The
optional consistency audit also reproduced and repaired reset losing caller
cleanup on a failing payload destructor, using the existing MIR removal
recipe. Optional cloning now delegates an engaged payload to ordinary
`std::clone`, with empty-state, owned string/vector, scalar and unrelated
user-defined clone policy checks across C++20/23, O0/O1/O3, no-elide and
sanitizers. Nested optionals and unrelated construction-only envelopes now
move and clone under `std::move_constructible`, without granting assignment.
Finite closed-wrapper traversal no longer loses relocation or leaf concurrency
facts to the generic-growth guard; exact cycles and uninterrupted generic growth
remain bounded. Focused compiler checks and the same eight-mode native matrix
cover these cases. The vector consistency slice now uses move construction,
rather than assignment, for empty-slot transport; concrete borrowed-payload
admission and container/expected integration checks are still in progress.
Exact generated special-member construction from a concrete owned pack now
lowers as a verified copy or transfer, preserving caller/parameter cleanup;
the focused pack matrix and nested optional emplacement checks pass.
The broader library sweep exposed an immobile-field constructor publication
regression after conservative failure edges were introduced. The backend now
proves exact failure-free constructor success chains from MIR while retaining
all original failure and cleanup metadata. Unrelated user-defined records and
the eight-mode once/thread runtime matrix pass; the full combined sweep still
needs to complete.
Fresh owning constructor arguments now reuse their exact prepared slots when
publishing a containing field, and destructor-bearing owners can restore
construction-only fields during rollback. Constructor results also publish
through owned Expected bindings using the existing success-edge obligation
and failure-free generated transfer proof. The reduced nested-owner fixture
preserves its exact success and first-failure cleanup in all eight native
modes: C++20/C++23 at O0/O1/O3, no-elide, and ASan/UBSan. All 66 focused
lifecycle-emitter cases also pass, including independent rejection of lost
constructor target, result ownership, or Expected publication. Broader
reconciliation remains in progress.
The subsequent scalar-initializer repair suppresses duplicate metadata-only
field writes; prefix/postfix increments, compound/plain assignment, and
overflow rollback pass the eight-mode native matrix. Passive error bindings
also use uninitialized Expected storage, including loop reuse, a non-default
constructible inactive success alternative and `void` success. These two
focused fixtures cover 40 executions. The vector construction-only matrix
passes 128 executions in the same modes, including sized construction and
resize with partial-prefix rollback, first/secondary destructor failures and
cleanup nested inside optional storage. These are focused results, not a
combined completion claim. Declaration-initializer admission in
`constructor_nonassignable_fields` and owning Expected temporary-return
representation retirement are repaired and pass their focused gates.
Fallible constructors containing owning Expected fields now pass the original
nested-argument regression and the both-owning-alternative extension. The
five-fixture owning-error matrix passes 160 executions across all eight native
modes, including private-empty-state transport and both cleanup failure kinds.
The separately handed-off constructor-formatter feature is committed at
`96e21754` in `codex/ctor-initializer-format`, with focused formatter/LSP
verification. Its patch is reconciled locally with candidate version 0.360.0;
combined verification remains pending. It has not been published or installed.
The completion audit additionally found two cross-feature gaps: later arguments
after an owning Expected constructor input, and fallible field construction of
an owning enum whose payload cannot be assigned. Expected transport now reuses
the existing owned-argument CFG/cleanup proof and passes the six-fixture,
216-outcome native matrix. Construction-only enum fields reuse exact producer
slots for fresh, moved-parameter, and helper-returned values; the 19-case
C++23/O0 check and malformed-MIR mutation gate pass. Its expanded 152-outcome
matrix also passed in the completion sweep. A staged invalid-payload check
also exposed duplicate publication of class-owned diagnostics during generic
constructor reanalysis. The narrow repair retains distinct constructor errors,
source spans and instantiation notes, and passes the compiler and LSP gates.
The optimized all-target rebuild and all 11 staged external consumers pass.
The first complete 368-test sweep passed 358 checks. It exposed constructor
representation inconsistencies for passive move-only mutex payloads, nested
generic defaults and polymorphic replacement; their focused runtime regressions
now pass after shared structural repairs. Updated negative emitter checks
preserve exact initializer ownership, status-only placement, cleanup proofs and
two-phase borrow ordering. Stale generated-text expectations were replaced with
actual retirement/publication identity checks. The HTTP test passed separately
with loopback access, and the declaration census was refreshed after both
independent corpus checks passed. The supplemental full language audit passed
with the absent GTI/C++ comparison-fixture phase explicitly skipped.
The follow-up standard-library sweep now passes after the mutex repair;
the borrowing-composition LSP gate also passes. The optimized optional compiler
gate now additionally proves that distinct concrete instances retain separate
class/constructor diagnostics at shared generic spans. Its LSP counterpart
preserves every substitution and instantiation site when presentation groups
identical primary errors, and clears them after correction. The executable
corpus tests still need the final all-target relink so their completion evidence
covers the latest constructor-admission hardening.
Final all-target/full-suite and clean staged validation must be repeated on the
completed source. Nonempty fallible vector emplacement with primitive scalar
inputs now retains the checked in-place constructor channel and passes 48
native outcomes in the eight-mode matrix. The fixture verifies the final
element address, heterogeneous scalar inputs, evaluation order, partial field
rollback, prior-prefix/caller cleanup, and first/secondary failure identity.
Focused structural mutations also pass; the final combined gates are pending.
The subsequent completion sweep caught a failure-free scalar constructor whose
HIR arguments were still grouped in a concrete pack during failure-effect
normalization. Normalization now uses the exact semantic-selected constructor,
not the pre-flattening argument shape. The reduced `Pair(int, int)` case checks
`None` propagation and ordinary in-place emission alongside the fallible
constructor route. The CLI workflow, full emitter preflight, and updated
48-outcome scalar matrix pass. After the all-target optimized relink, the fresh
369-test sweep passed in 1416.04 seconds, including the library surface,
MIR/C++ corpus, verifier, LSP, and native runtime gates. All 11 fresh
staged-toolchain consumers and the staged scalar emplacement success/failure
probe also pass against that rebuild. This establishes the existing-candidate
stabilization checkpoint before the accessor follow-up; it does not complete
the remaining campaign or replace validation of subsequent changes. The
interrupted previous sweep is not completion evidence.

The non-scalar in-place input candidate now preserves selected constructor
parameter roles and retains prepared argument obligations through a distinct
`StorageConstructPreflight`. Its unique success continuation transfers them once at
constructor entry and publishes prefix length only on success. Independent
verification ties both steps to the same storage, target, call inputs and
failure control flow; the storage input's extra non-consuming use is confined
to that pair. Serialization advances to `mir-v84`/`mir-body-v71` and exhaustive
optimizer coverage treats the check as a conservative trapping barrier.
Mixed owning/reference/owner-dependent-text/raw-pointer regressions and forged
pair/cleanup tests are implemented. The first coordinated optimized build is
in progress; these programs are not considered delivered until their full
native matrix and combined gates pass. Controlled backend rejection was not
treated as a completion claim.
At that earlier checkpoint, the campaign was not complete.
No custom copy/move constructor bodies,
module implementation, weakened borrow rules, or unchecked failure paths are
part of this campaign.

### Receiver-accessor lifetime integration (validated and installed locally)

After the 369-test stabilization checkpoint, implement receiver-tied reference
returns to objects that themselves retain borrowed dependencies. The motivating
client is Tea's `client.auth()` accessor, where the contained auth service
borrows a separate HTTP service. The candidate now represents the returned
reference to the service shell and its inherited external-owner dependencies
with separate access authority and loan endpoints. The carrier integration
passed its full 371-test inventory, followed by the optimizer correctness
repair and fresh installed checkpoint described below.
The planning handoff is a proposal to audit against current semantics, not
permission to remove a rejection without the corresponding lifetime proof.
Baseline inspection also found that direct local references to an exclusive
carrier are rejected, while a shared carrier reference is currently attached
to the external owner loan without protecting its physical shell. A reduced
program can consequently move or mutate the shell and then use that still-live
reference without a source error. This is part of the same lifetime repair;
fixing accessor syntax alone would leave the general rule unsound.

The audited design direction keeps physical containment/reborrowing separate
from a shell reference's inherited stored dependency. Reuse the existing
complete stored-owner loan through an explicit checked dependency relation;
do not widen the homogeneous read-only multi-owner source set into unrelated
mixed permissions or infer the relation in the backend. The dependency remains
live for dependent shell references and their descendants. Each access is
bounded by both shell and inherited permissions, without changing the stored
loan's original permission. Entry/return provenance, cross-argument aliases,
normal/failure retirement, and optimizer alias/escape consumers must verify the
relation. The candidate implements this rule with unrelated user-defined
carriers and concrete generic instances, positive/negative semantic,
MIR-verifier, native runtime, and compiler/LSP diagnostic evidence. Preserve
safety for shell movement,
replacement, escape, reborrowing, and conflicting external-owner access.

The focused native matrix passes 32 outcomes across C++20/C++23, O0/O1/O3,
no-elide, and ASan/UBSan. It includes generic accessor forwarding, readonly
carrier copies, iterator reference aliases, checked mutation overflow, and
cleanup failure after provisional reference or copied-value return. The LSP passes accessor
navigation, exact UTF-16 escape diagnostics, invalid-source recovery, and
diagnostic clearing. Broader ownership testing exposed missing dependency
propagation for scalar references returned through carrier references; the
producer now preserves that existing edge. Copied-value return publication
and its independent verifier proof pass, including actual-source MIR mutations
that remove required retirement and must fail at the production lowered
boundary. All 60 ownership compiler cases pass. The first post-integration broad
run exposed an abstract receiver-entry sibling conflict in existing algorithm
and associative-iterator operations; that run was interrupted for focused
repair, not counted as a pass. Exact designated referent chains now distinguish
the abstract declaration authority from a competing physical borrow, with
wrong-field/access/dereference and real-sibling negatives. The unrelated
Cursor/Box/Leaf chain also runs at O3. An audited SSA-producer alias query now
includes inherited owners. The rebuilt compiler passes all seven focused
groups, including the complete algorithm and container regressions, and all
11 fresh staged-toolchain consumers plus four staged accessor cleanup cases.
The complementary 364-test sweep finished with 356 passes and eight failures:
three mutex groups and the stdlib surface exposed an abstract guard entry
incorrectly claiming the caller's release obligation; nested-borrow runtime
and lifecycle emission exposed a prematurely reconstructed, missing-dereference
place; the alias text test expected the superseded shared-loan representation.
The HTTP runtime test separately lacked sandbox permission to bind its local
test server. These were repaired and revalidated, not waived. Source
repairs keep capability ownership and exact projection verification strict;
the representation test now requires physical-reference authority separate
from the external dependency, including checked reference-to-reference aliases.
The fresh optimized full inventory passed 371/371 in 1213.74 seconds, including
the HTTP local-server test with the required permission. All 11 freshly staged
toolchain consumers and four standalone staged accessor cases also passed.
An additional audit then reproduced a pre-existing optimizer wrong-code case:
an exact source call mutating a scalar reference left the caller's old value
fact available to discharge a required overflow check. The pre-accessor SDK
reproduces it too. The repair now invalidates exact mutable call-input origins
and separates proved result facts from opaque effects, while preserving
independently proved object postconditions and supported storage-loop proofs.
All seven focused gates passed, including the 42-outcome native regression.
The fresh optimized full inventory passed 372/372 in 1236.58 seconds; all 11
restaged external consumers, four staged accessor cases, and all 42 staged
optimizer cases also passed. The repository installer then passed archive and
headless Neovim checks and installed 0.360.0 locally. Terminal commands now
use the same toolchain as Neovim, with hash-verified backups of the original
terminal executables and a backup of the prior Neovim configuration. The old
0.359.0 primary-checkout toolchain is unchanged. Standalone installed accessor
execution passes at C++23/O3, and all four installed accessor cases pass at
C++20/O0 without build-tree resource overrides.
The carrier checkpoint does not claim completion of the full stabilization goal.

That standalone check also exposed an advisory follow-up: `GTI-S2096` called
the carrier fixture's mutable owner unnecessary even though its constructor
passes that owner to an exact mutable-reference parameter. Selected constructor
arguments now use the same mutability-use accounting as ordinary calls.
Compiler tests cover three construction spellings, rejection after removing
required `mut`, and a read-only control that must still warn. LSP regression
coverage checks UTF-16 spans and rejects stale fixes after the constructor
becomes mutable. These changes passed the final combined 0.360.0 validation
and remain covered by the 0.362.0 run.

The user's requested order is current stabilization, this accessor capability
and its validation, then the repository-supported local install and installed
toolchain smoke tests, followed by any remaining work. That local install is
complete. The user has now authorized finalizing all remaining work and starting
a production release; the previous publication hold is lifted. Tea sources are reproduction
input only: do not edit the application or execute authenticated network calls.

The separate `expected::transform_error` lambda-mapper handoff in Gitea PR 18
(`12e8cc1635277cc5fd0f71d260aa64d73059ca58`) was reconciled, independently
reviewed and published in 0.360.0 at `61f3880a`. Its eager conditional inputs,
closure publication and owned error-parameter cleanup overlapped the HIR/MIR
and backend work. Reconciliation retained source-order evaluation even on the
success branch, exactly-once capture cleanup on both branches and on failure,
and the prepared-input and carrier-lifetime proofs. HIR value traversals for
the added conditional-input edges were audited, and the compiler, runtime,
LSP, independent verifier and combined gates passed on the integrated source;
the handoff's separate branch results were not substituted for that evidence.
The review reproduced a moved named owning mapper reaching `GTI-B0001`:
semantic admission is valid, but the synthesized read invocation stages its
moved receiver as `Value` where the retained-lifetime proof requires
`MoveValue`. The shared staging contract is corrected without changing read-only
invocation or full-expression cleanup. The named non-moved control passes
success/error/later-failure execution, two concrete generic mapper instances
pass, and receiver failure preserves capture-source cleanup order at O0/O3 in
the handoff build. These reduced probes are now integrated regressions, along
with moved-binding reuse rejection, a later successful argument before capture
cleanup, and malformed eager-input HIR plans. The 0.361.0 and 0.362.0 combined
runs also retain these regressions.

## L-INFER-01: Local Inferred Reference Bindings

**Status: Complete in 0.387.0; combined acceptance passed for 0.390.0.** Local
reference inference shares ordinary referent/binding validation and retained
loans. Paired semantic, HIR/MIR, native cleanup and LSP checks pass, including
borrowed closures and external borrowed-expected extraction. Example 131 is the
runnable client. The reconciled candidate passed all 565 local CTests and the
staged SDK/consumer checks described in the canonical verification document.
Whole-array and raw-pointer references retain their existing explicit-reference
boundary; no backend inference or new owner is introduced.

**Outcome:** Applications can retain a checked borrow without spelling a long
concrete type. For example, reading an array nested in a parsed JSON document
permits:

```gti
auto& scopes_array = parsed.get("scopes").expect("scopes are required");
for (auto& scope : scopes_array.elements()) {
  auto text = scope.as_string().expect("scope must be a string");
  scopes.push_back(std::move(text));
}
```

### Evidence And Prerequisites

At proposal time `SemanticVisitor::Impl::analyzeInferredVariable` explicitly rejected
ordinary local `auto&`. Its reference path already constructed a
resolved reference type, calls `validateReferenceBinding`, and records retained
borrows. Explicit local `T&` declarations use the same reference validation and
retained-borrow machinery. This overlap supports a bounded generalization;
removing the diagnostic alone is not sufficient implementation evidence.

Borrowed `expected<T&, E>` extraction through `.expect()` is implemented and
documented in [expected values](../language/expected.md). It yields a place tied
to the original owner, even after destruction of the temporary expected shell.
`std::json::value::get` returns this shape; it does not yield an owned JSON node.
The move-only-copy diagnostic on `mut auto value = borrowed_result` rejects a
real attempted copy. The old unconditional movement hint has been replaced:
current guidance explains `auto&` and conditional explicit cloning, and suggests
movement only for a legally transferable whole owner. It does not offer an
ownership-changing fix-it.

### Language Rule

An initialized ordinary local declaration `auto& name = expression;` infers
the exact complete referent type and requests a read-only checked reference.
`mut auto& name = expression;` infers that same type and requests mutable
access. Each form must accept exactly the initializer shapes accepted by the
corresponding explicit `T&` or `mut T&` declaration, subject to existing generic
instance analysis. Expression referent types and value categories must be used
without accidentally creating a reference-to-reference type.

Plain `auto` continues to infer a value binding. The written `&` is what
requests borrowing; `mut` cannot upgrade a read-only result to mutable access.
Inference performs no copy, movement, conversion ranking, lifetime extension,
or independent ownership creation. The initializer is evaluated once.

The origin place or dependency set, access mode, permitted escape boundary,
and loan endpoint must be identical to those of an explicit reference binding.
Borrowed referents remain owned and dropped by their original owner. Existing
last-use, invalidation, reborrow, and escape rules remain authoritative.
Fresh owned temporaries remain invalid local reference initializers; a
temporary carrier that returns a checked borrow of stable external storage is
judged by that storage's established lifetime, not merely by call syntax.

The initial scope is ordinary initialized local declarations, including locals
in concretely analyzed generic bodies. It adds no inference for fields,
namespace/global declarations, function parameters, returns, structured
bindings, `auto&&`, untyped braces, or explicit `auto` array declarators.
Reference inference from an existing fixed-array place must follow explicit
array-reference support and preserve extents rather than decay to a pointer.

Prove the general rule with an unrelated application type as well as JSON:

```gti
struct Record { mut int count = 1; };

int main() {
  mut Record owner{};
  auto& observed = owner;
  int snapshot = observed.count;
  mut auto& writable = owner; // shared borrow has reached its last use
  writable.count = snapshot + 1;
  return writable.count - 2;
}
```

### Implementation And Acceptance

1. Semantics must resolve inferred references through the ordinary reference
   contract. Audit inferred/range/generated-local special cases and factor
   only the shared logic needed to prevent explicit and inferred drift.
2. HIR and MIR must preserve the same concrete binding types, origin/dependency
   facts, access, loan endpoints, and cleanup behavior as explicit references.
   The backend consumes these resolved facts; it must not delegate inference
   or borrow permission to generated C++ `auto&`.
3. Add paired explicit/inferred positive and negative tests for stable locals,
   fields, supported array places, checked reference-returning calls, borrowed
   expected extraction, concrete generic instances, mutation conflicts,
   read-only-to-mutable upgrades, moved owners, temporary owners, and escaping
   local storage. Unsupported retained reborrow families remain rejected in
   both spellings. Exercise later failure and exactly-once owner cleanup.
4. Improve copy-attempt hints where a stable read-only initializer can be
   borrowed: explain `auto&` and explicit cloning where applicable. Offer
   movement only when it represents a legal ownership transfer. Do not
   automatically change owning code into borrowing code through a fix-it.
5. LSP diagnostics and hover must use compiler facts, display the inferred
   referent/access accurately, and retain definition/navigation identity.
   Test edits from `auto` to `auto&` and `mut auto&`, incomplete source, recovery,
   and dirty dependency overlays. Confirm formatter and Tree-sitter support
   the syntax without introducing separate inference rules.
6. Add a runnable example under `examples/` and update its index, corpus and
   census registrations. Update current static semantics, ownership and grammar
   docs, revise `R-INFERENCE`, and apply the user-visible language version policy
   only when the implementation lands. This proposal itself changes no VERSION.

**Exit gate:** The inferred and explicit forms agree on semantic admission and
borrow effects for the tested structural families; valid examples execute with
an optimized host compiler across C++20/C++23 and GTI O0/O3; invalid cases stop
with frontend diagnostics; relevant MIR verification and LSP gates pass. Any
missing structural family must name its prerequisite and remain rejected in
both forms rather than gaining a nominal exception for one library type.

## Explicit Cloning And Native Bridge Follow-Up

The explicit-duplication slice supplies source-defined `std::clone`,
`member_cloneable`/`cloneable` concepts, and vector element cloning (including
strings and nested vectors). The language prerequisite is a general exact
read-only nullary member-signature requirement, shared by concept facts and
boolean queries; actual calls remain concrete HIR and verified MIR. General
expression probing, borrowed-result requirements, and method-argument/generic
deduction are not implied. Owning behavior is documented in
[`concepts.md`](../language/concepts.md).

The independent bounded native bridge slice admits macOS Objective-C++ `.mm`
sources through existing `cpp-sources`, frameworks and C ABI declarations.
It adds no Objective-C syntax or ownership model to GTI. `.m`, native Objective-C
type import, non-Apple runtimes and structured ARC policy remain outside this
slice. Current behavior and evidence live in the build/driver architecture,
native C interop contract and `objective_cpp_bridge` gate.

## Current Sequential Optimizer Follow-Up

The combined compiler/library work is locally checkpointed at `113e112a` on
top of reconciled `main` `b9609587`. The user subsequently authorized combined
publication after fixing the Tea constructor-emission failures. The held
publication labels below describe the original local-validation checkpoints,
not a continuing release restriction.
These two requested tasks are sequential, not parallel compiler campaigns:

| Order | ID | State | Outcome and exit gate |
| --- | --- | --- | --- |
| 1 | `O-CONFORM-01` | **complete — reconciled locally at 72a8087a** | MIR language growth fails closed across optimizer classification, analyses, edits/replay, and lowered/backend admission. Exhaustive compiler classification, unknown-vocabulary/alias and cache-invalidation regressions, deterministic barrier reasons, canonical docs, optimized-compiler focused tests, MIR/editor/backend/corpus/differential/report gates. No new optimization facility. |
| 2 | `O-MEASURE-02` | **complete — measured bounded facility at 2c8d4561; publication held** | Started from clean reconciled `main` 72a8087a. Five candidate families retain legal proxies and complete-state comparisons. Exact local interface dispatch saves 46.2% in its update kernel, with independent replay, O0/O2/O3 C++20/C++23 parity, 354 broad tests and 16 final focused passes. Runtime, compiler-time, size, and memory controls show no material regression. The performance plan records exact identities, raw evidence, larger unimplemented callback/helper ceilings, and platform/workload limits. |

The second row must preserve exact failure/cleanup/ownership and evaluation/FP
ordering, prove independent replay and O0/O2/O3 C++20/C++23 parity, and retain
fixed-array/vector/scalar/game/thread, compiler-time, size, and memory controls.
The subsequent explicit release authorization covers both completed rows.

### C-TIME-01: Bounded Compiler Latency Investigation

**State:** bounded investigation and small improvement complete; general
compile-time comparability remains an open target, not a claimed plateau.
Nine focused gates and all 355 Release CTest gates pass on local arm64 macOS.

Start from reconciled `main` `a687b0d4` after the two rows above. The client is
interactive development of ordinary GTI container, numeric, and game-update
programs. Compare equivalent idiomatic GTI/C++ workloads at matched native
settings, separately reporting clean, no-op, and edited-build costs. Equal line
counts alone are not a comparison contract.

The provisional small-program band is `GTI <= 2 * C++ + 250 ms`, not a shared
CI gate. The bounded implementation removes unused intermediate safety-state
copies while preserving exact Invoke failure snapshots, proof precision, and
independent replay. It adds a reusable clean-compilation measurement path, not
a new optimizer, persistent semantic cache, weakened verifier, or unchecked
mode. Current measurements and the distinction between comparable performance
and an explicitly incomplete performance target live in
[`performance-tooling.md`](performance-tooling.md). This work is included in the
subsequently authorized combined publication.

### C-TIME-02: Chip8 Build Latency And Successful Cache Reuse

**State:** complete as a bounded improvement. The user-supplied Chip8 project
reopens C-TIME-01 with a representative approximately 39-second plain
`gti build`, rather than a synthetic line-count target. The optimized host
compiler profile identifies redundant work; fresh, edited and unchanged
project builds are measured separately.

The initial profile identifies backend ownership-pattern searches repeatedly
probing unrelated scalar aggregate elements. The bounded implementation moves
necessary structural exclusions before those searches; applicable ownership
proofs and independent verification remain intact. Five interleaved paired
measurements reduce emit-only time from 40.554 s to 1.656 s with byte-identical
C++, backed by positive and malformed-MIR admission coverage. The separate
generated-derived-constructor linkage failure is fixed through ordinary
semantic/HIR/MIR constructor scheduling without losing caller cleanup. The
unmodified original project now builds successfully in 1.989 s, and a genuine
comment edit builds in 1.992 s.

The existing cache returns the original project's unchanged builds in 51 ms
and correctly invalidates on source, configuration, and same-timestamp
native-header edits. No additional cache layer is justified. All 413 combined
CTest gates and eleven isolated installed consumers pass; existing vector and
game-update controls show no material regression. Final evidence and
limitations live in the existing performance-tooling plan. General C++
compile-time comparability remains an open target, not a claim of this slice.

### C-TIME-04: Optimizer Proof And State Bookkeeping

**State:** bounded implementation and paired performance acceptance complete;
delivered through PR #181. General compiler-latency improvement remains open.

The outcome is a faster optimized edit/build loop for the existing HTTP/JSON
client, without changing its GTI program or native representation. CPU sampling
identifies repeated proof-vector allocation/sorting and whole-state copies in
SCCP joins. Internal evidence now uses ordered insertion and canonical set
union; SCCP detects per-fact changes without copying the complete destination.
Facts, complete proof identities, failure edges, convergence budgets and
independent replay remain unchanged. This does not change language semantics,
add a persistent cache or remove verification.

Exhaustive evidence-set regressions and the owning optimizer/native checks
pass. All 130 runnable examples produce byte-identical C++, optimized MIR,
optimizer reports and diagnostics with the baseline and candidate. HTTP/JSON
O2 emission takes 14.3% less time and full builds 10.6% less time in five paired
measurements; O0 emission is unchanged. Controls and shared-host/platform
limitations are recorded in
[`performance-tooling.md`](performance-tooling.md#c-time-04-optimizer-proofstate-bookkeeping-2026-10-09).
Further analysis-state and
proof-construction costs remain opportunities for separately validated work;
this slice does not establish a compile-time limit.

### Tea Constructor-Emission Follow-Up

The release fix covers nonassignable empty-field placement and qualified or
unqualified replacement of an initialized owning field from a call result.
The original Tea build and reduced success/failure cleanup cases exercise that
scope. It does not establish arbitrary constructor/capability composition.

The everyday-stabilization campaign has locally fixed the two broader cases
exposed by stress fixtures: cleanup-free nested field construction now retains
its Invoke/publication schedule, and optional emplacement constructs its
payload in ordinary GTI before exact completed-value publication. New partial
construction regressions and the existing field-publication cases pass all
eight C++20/23, O0/O1/O3, no-elide and sanitizer modes. MIR mutations cover the
failure edge and the optional capability's exact operand/constructor boundary.
Broader checks exposed older straight-line native-constructor assumptions after
the wider cleanup-free Invoke rule. Exact success-edge publication now handles
those cases, including prepared-argument obligation handoff. The complete
lifecycle-emitter suite and seven related constructor/optional gates pass
together; malformed argument, field, literal, lifecycle and failure-edge
mutations still reject. Reconciliation and broader campaign verification remain
pending; these results do not establish arbitrary capability composition.

## How To Use This Plan

Ordinary feature work should select one row from this plan, or one explicitly
named bounded sub-slice. The immediate backend-authority recovery campaign is
the deliberate exception: one declared implementation phase may combine the
applicable `M-EXEC-01`, `M-FAIL-01`/`Q-FAIL-01`, and `M-BACK-01/02` slices when
that is required to put a complete body family into production. The phase is
the outcome boundary; it may land as several reviewable commits, but it must
not stop at an IR-only checkpoint when the matching production cutover is
within the declared scope.

Every row has one of these states:

| State | Meaning |
| --- | --- |
| **ready** | All prerequisites are implemented or the row is design-only. |
| **planned** | A goal is identified; named prerequisites or design work remain before its acceptance criteria can pass. |
| **active** | One task owns the row and has recorded its bounded scope. |
| **done** | The implementation, tests, and canonical documentation passed the exit gate. |
| **measured defer** | A prior measurement deferred that slice. For runtime architecture, this is historical evidence, not a benchmark prerequisite to renewed work under ADR 012. |
| **later breadth** | No current systems-readiness workload requires the broader form; evidence may move it earlier. |

Readiness role is separate from dependency state:

- **systems-ready** means the capability is required by one or more accepted
  readiness workloads;
- **bounded-first** means the first useful form should ship with its client
  while broader forms remain explicit;
- **design-first** means a cross-cutting contract must be settled before the
  first executable client; and
- **measured defer** means a benchmark, client, or ownership design must exist
  before implementation is scheduled.

Under [ADR 012](../decisions/012-outcome-first-systems-readiness.md), `1.0` is a
soft, revisable systems-readiness goal, not a scheduling horizon. Legacy
pre/post-version labels in superseded evidence do not control this queue.

For each selected row or recovery phase:

1. re-check the prerequisite evidence and current source rather than trusting
   an old audit line number;
2. state the user-facing program/API/workflow, owned files/layers, and explicit
   non-goals;
3. preserve the phase direction syntax -> semantics -> HIR -> MIR -> backend;
4. apply the executable-authority admission rule: new or changed runtime
   behavior must reach production through a verified MIR-emitted family, never
   through a new AST/semantic/HIR-only C++ emission case;
5. add focused positive, negative, structural, and diagnostic tests at the
   owning layer;
6. run the row's focused gate and the broader matrix it names;
7. update every covered row, the checkpoint, and the owning
   architecture/language
   document in the same change; and
8. stop when the declared row or recovery-phase exit gate passes.

## Decisions Made By This Sequence

This plan accepts the following conclusions from the reviews without treating
every review recommendation as a release commitment:

1. **Concurrency semantics are fixed before public concurrency.** ADR 008
   adopts safe data-race freedom, structural transfer/share facts, the concurrent-global
   boundary, sequentially consistent first atomics, and owned automatic-join
   threads. A bounded public profile is systems-readiness work; detach, weak
   memory-order breadth, and advanced reclamation are not.
2. **Concurrency starts from the adopted language model, not a pthread
   wrapper.** Implementation follows the capability, lifetime, ordered-
   execution, failure, runtime, and MIR prerequisites in this plan.
3. **The executable critical path remains places -> initialization ->
   temporaries/drops -> ordered evaluation -> defined-failure edges ->
   MIR-backed emission.** These facts unblock mutable ranges, allocators,
   richer FFI, backend-independent failure, and public concurrency.
4. **Layout queries precede allocator and broad FFI work.** `sizeof` and
   `alignof` may initially support only types whose layout GTI owns; this does
   not imply a stable GTI ABI, packed records, unions, or manual lifetime.
5. **A general pass manager is not a standalone prerequisite.** The first
   transforming MIR slice adds only the editor, repair, and invalidation it
   actually uses. Dominance continues to use full recomputation until a real
   client proves incremental maintenance worthwhile.
6. **A generic MIR dataflow solver does not automatically become semantic
   authority.** ADR 001 still makes semantics own language validity and loan
   endpoint selection. Any migration of ownership-state analysis requires a
   separate authority decision and cross-phase contract.
7. **LLVM remains a required dependency but not a default design answer.** It
   may implement language-neutral algorithms or private storage behind
   GTI-owned interfaces. It does not define GTI identities, semantics,
   serialized forms, public APIs, or cross-phase representations.
8. **Type interning remains deferred.** Types were about three per cent of
   retained semantic memory in the audit workload after the occurrence-table
   fix; snapshot/context ownership and a type-allocation benchmark are still
   missing.
9. **Evaluation is strictly left to right.** ADR 010 fixes receiver, argument,
   operand, assignment-target, initialization, full-expression, temporary,
   cleanup, and program-wide initialization order. M-LIFE/M-EXEC represent the
   rule; production conformance belongs to matching M-BACK migrations.
10. **New executable behavior must be MIR-owned.** New or changed language
    behavior that affects runtime values, effects, control flow, ownership,
    lifetime, cleanup, failure, or initialization must lower through verified
    MIR and pass the general backend preflight. Recording MIR while production
    executes a source-tree interpretation is not completion. AST, semantics,
    and HIR may still supply declaration, identity, layout, ABI, and target
    representation facts; they may not regain executable body authority.

## Immediate Backend-Authority Recovery Campaign

This campaign is **complete for source executable bodies**. The retained phase
list records how the cutover was reached. Follow-on backend work concerns
generated-item representation contracts, not restoration of the removed
AST/HIR executable route.

The campaign maximizes coherent work per phase rather than accumulating more
shadow infrastructure:

1. **First production seam — done (`M-BACK-01`, `scalar-leaf-v1`).** The
   deliberately failure-free fixed-width-integer leaf-function family now
   emits from verified optimized MIR at C++20/C++23 and O0/O1/O3. Ineligible
   bodies remain wholly on the explicit compatibility path; no body mixes MIR
   scheduling with AST/HIR evaluation.
2. **Scalar CFG expansion — done (`M-BACK-02`, `scalar-cfg-v1`).** The
   coherent call-free, failure-free, cleanup-free scalar CFG family now emits
   wholly from verified MIR: scalar locals and parameters, selected scalar
   computations, initialization and assignment, and branch, switch,
   short-circuit, loop, and return CFG. Its dedicated structural and runtime
   gates cover O0/O1/O3 and C++20/C++23 while near misses stay compatible.
3. **Scalar direct-call expansion — done (`M-BACK-02`,
   `scalar-direct-call-v1`).** Exact ordered scalar calls now execute wholly
   from verified MIR across a closed acyclic static-call graph. MIR v20 owns
   the bounded defined-failure summary used to normalize proved-false calls;
   dedicated mutation and O0/O1/O3 × C++20/C++23 gates keep recursion,
   checked/unknown targets, and header near misses compatible.
4. **Class-default cleanup — done (`M-BACK-02`,
   `class-default-cleanup-v1`).** Exact generated-default construction of empty
   concrete class locals, scalar return loading before cleanup, bounded source
   destructors, reverse lexical drops, and one normal cleanup boundary now emit
   wholly from verified MIR through strict raw-storage lifetime slots. Its
   dedicated structural and runtime gates cover O0/O1/O3 and C++20/C++23.
5. **Remaining failure-free closure — done (`M-BACK-02`,
   `owned-lifecycle-call-v1`).** One atomic acyclic free-function graph now
   constructs, moves, passes, transfers, and drops exact passive-scalar-field
   owners wholly from verified MIR. Exact constructor/destructor schedules,
   reverse HIR caller closure, strict lifetime slots, and fail-closed
   source/optimized-MIR coherence keep unsupported connected shapes wholly
   compatible.
6. **Failure-capable closure — done (`M-EXEC-01` +
   `M-FAIL-01`/`Q-FAIL-01` + `M-BACK-02`,
   `scalar-failure-callgraph-v1`).** One bounded hosted no-argument `int32_t`
   component now carries exact checked-integer records through an atomic closed
   call graph, runs verified failure cleanup, reports once, and exits 70. Its
   gate covers O0/O1/O3 and C++20/C++23; broader signatures, lifecycle failure,
   initialization, double failure, embedding, and callbacks remain separate
   work rather than implied completion of M-FAIL-01.
7. **Final authority cutover — done (`M-BACK-02`).** Every source body in the
   reviewed `tests/mir_census_baseline.json` inventory emits through
   `CppMirBodyEmitter` from verified optimized MIR and copied representation
   rows. Functions,
   constructors, destructors, lambdas, class initializers, module
   initialization, hosted startup, generic specializations, ownership,
   failure, storage, callable, and native-boundary forms are covered by the
   general route. The no-MIR `CppEmitter` construction path and executable AST
   statement emission are removed. The exact census, endpoint corpus oracle,
   structural mutation tests, and focused native runtime matrices provide the
   post-cutover evidence described below.

Backward compatibility with retired compiler-route behavior or with
the textual form of generated C++ is not a migration requirement. GTI has no
downstream source ecosystem that justifies preserving those implementation
details. A family that passes its MIR production exit gate deletes its replaced
AST/HIR production route immediately and cannot retain it behind a legacy mode
or fallback flag. No unmigrated source-body family or public direct-emitter API
remains. This does not
permit breaking an explicitly accepted language semantic, runtime ABI, or
native-interoperability contract.

### Post-Cutover Evidence

The differential oracle was removed with the old executable emitter. Keeping
that route callable for comparison would violate the architecture the oracle
was meant to establish. The current evidence is:

- an exact reviewed per-example census in `tests/mir_census_baseline.json`;
- `tests/mir_census_test.py`, which rejects every body-inventory change;
- `tests/mir_cutover_corpus_oracle_test.py`, which builds and runs every
  example at `-O0`/C++20 and `-O3`/C++23, checks exact body ownership, and
  compares observable behavior between endpoints;
- structural verifier and backend tests that mutate MIR and representation
  inventories to prove fail-closed behavior; and
- focused O0/O1/O3 by C++20/C++23 runtime matrices for high-risk ownership,
  cleanup, failure, and call-graph forms.

The census is an inventory contract, not a coverage percentage. Any added,
removed, or changed body count must be reviewed and the corpus oracle rerun
before its baseline is refreshed. Generated C++ text is not a public contract;
marker rows are only an ownership witness.

ADR 016 is complete. Hosted entry, program initialization, structural
operators, callable invocation, lifecycle cleanup, same-thread native callbacks,
and concrete generic function/constructor instances all have exact target-
independent identities, payloads, dependencies, and roots. Ordinary C/runtime
boundaries remain ABI declaration/body rows rather than generated wrappers.
`BackendInput` and the AST/semantic/HIR C++ declaration route are removed; all
production backends receive only an immutable `LoweredProgram` plus explicit
backend policy.

## Systems-Readiness Outcome Lanes

The queue exists to make these programs possible. A prerequisite row should be
scheduled with the nearest lane it unblocks instead of becoming an open-ended
infrastructure project.

| Outcome lane | First dependency path | Acceptance signal |
| --- | --- | --- |
| Real C-library wrapper | `S-LAYOUT-01/02` -> `S-ABI-01/02/03` -> opaque-handle sub-slice of `S-FFI-02` -> `S-FFI-02` families F1/F2/F3; independently `M-LIFE/M-FAIL/M-BACK` -> `S-CALL-01` | Layout-stable records, exact symbols, nominal pointer-only handles, and named same-thread callbacks share one generated C/C++ adapter header. A wrapper can hide C structs or C++ classes behind ordinary GTI RAII. The named acceptance client is GLFW 3.4: all 124 functions, all five records, callback typedefs, opaque handles, and Vulkan-facing declarations bind with no GTI-caused omission and cross a deterministic C harness. |
| Arena or pool allocator | `M-LIFE-01` + failure/layout facts -> `S-ALLOC-01/02` -> one `S-ALLOC-03` client | Application GTI owns allocation policy and one container/value family proves initialization, failure, and cleanup. |
| Multithreaded work queue | `M-LIFE/M-EXEC/M-FAIL` -> `C-MIR/RUNTIME/CALL` -> `C-ATOM/THREAD/SYNC` -> `C-CONFORM` | Owned tasks, ordered scalar atomics with integer fetch operations, mutex-guard access, join, and worker failure work through public GTI. |
| Renderer/game update loop | mutable ranges/views + completed vector/string + exact domain operators + time/files/allocation | A frame/update workload mutates collections and domain values without raw-pointer escape hatches. |
| Host-embedded gameplay module | `E-EMBED-00` -> `B-LIB-01` -> `E-EMBED-01A/B/C` -> `B-LIB-02` -> `T-EMBED-01` -> `Q-EMBED-01` | A C17 or C++20/C++23 engine host loads a versioned GTI shared library, validates its descriptor, creates its one module context, invokes generated C wrappers repeatedly, receives an exact defined-failure record without process exit, destroys the context, unloads the library, and loads it again. No ordinary GTI or generated C++ ABI crosses the boundary. |
| Compiler-style AST/protocol | `M-LIFE-01` + layout -> `L-SUM-01` | Generic payload cases support exhaustive move/borrow matching and deterministic cleanup. |
| Fallible pipeline | `M-LIFE/M-EXEC` -> `L-ERR-01` plus `expected`/host APIs | Propagation is concise, exact-type, and cleanup-correct across a nontrivial call chain. |

The first five user-facing lanes should normally outrank unrelated
generalization work once their immediate prerequisites are ready. This does
not permit skipping a correctness dependency; it prevents proof machinery with
no named consumer from displacing a bounded executable slice.

## Verified Starting Point

The following foundations are complete and should not be reopened merely to
start a later phase:

| Foundation | Evidence at 0.144.0 |
| --- | --- |
| Numeric semantics | Checked fixed-width operators remain the default; explicit fixed-width wrapping, saturating, and `expected`-returning checked-result add/subtract/multiply share one private `APInt` authority and public `<std/numeric>` API; exact IEEE binary32 and binary64 use GTI-owned width-tagged bits and private `APFloat` computation. |
| Ownership | Shared read-only loan identity, bounded stable-place exclusive reborrows, parent suspension/reactivation, single-origin read-only owner dependencies, and exact single-threaded global/static borrow returns reach verified MIR. |
| MIR integrity | CFG, places, values, loans, drops, effects, use indexes, and deterministic printing exist; fresh GTI-ID dominance verifies value availability. MIR v18 retains bounded `Invoke`/`PropagateFailure` edges, fixed-record parameters, and deterministic failure cleanup while adding caller-owned ordinary-call parameter stages, normal-edge-only initialization for one cleanup-owning result shape, and exact local-detector or ownership-free static direct-call argument edges after an earlier prepared owner. MIR v19 added verifier-owned source/identity-fold provenance for `Compute/Literal` instructions so transformed production emission does not consult the HIR replacement table. MIR v20 introduced function definition provenance and the bounded function `mayRaiseDefinedFailure` vector. MIR v21 makes the canonical effect result cover functions, constructors, and destructors; retains the acyclic closed scalar/static-call proof; adds the exact class-default-cleanup proof; and derives the exact passive-scalar-class constructor/destructor/function closure consumed by `owned-lifecycle-call-v1` after separate source/MIR graph and lifecycle verification. The lowerer verifies an all-conservative provisional program, derives the three-kind result, then lowers and verifies the exact final facts; generic verification accepts conservative true but rejects unproved false claims and exact `None`/`DirectCall` propagation drift. It retains exact borrow origins, synchronization records, conservative `PackFold`, and artifact-local failure identity. |
| LLVM boundary | One mandatory LLVM 18+ build with matching Clang and required APIs/RTTI; installed headers are LLVM-free; only the approved support link surface is used. |
| Compiler performance | LSP HIR-stopped analysis preserves concrete generic diagnostics while skipping failure metadata/MIR; indexed source locations, instance delta analysis, tooling-occurrence opt-out, and HIR instance indexing are implemented. |
| Driver/build | Direct compilation and manifest `build`, `check`, `run`, `test`, `clean`, and `metadata` share compiled compiler/driver libraries; executable/test kinds and direct/project execution-profile selection resolve through driver-owned plans. |
| Tooling | Formatter, Tree-sitter shipped-source parsing, diagnostics, semantic tokens, hover, completion, and definition have tested foundations. |
| Compiler-private capabilities | Source roles distinguish application, prelude, and physical standard-library units; `gti_internal` declarations and presentation are trusted-only, private types bind by exact prelude declaration identity, and application forging is `GTI-S2058`. |
| Transfer/share capabilities | `SemanticTypeTraits` and HIR retain structural transfer/share facts for concrete types; C++-familiar nominal attributes implement safe opt-out, interface requirements, and unsafe positive assertions with `GTI-S2059`. |
| Concurrent global policy | Explicit single-threaded/concurrent selection reaches semantics, HIR, and MIR; `GTI-S2060` enforces immutable share-capable process-wide storage only in the concurrent profile. |
| Synchronization IR | C-MIR-01 gives HIR/MIR backend-independent thread spawn/join, atomic load/store/RMW/compare-exchange, and mutex lock/unlock identities with validated atomic orders, concurrent-profile enforcement, deterministic serialization, and exhaustive non-speculatable/non-removable/non-reorderable effects. |
| Place/ownership authority | M-OWN-01 defines one snapshot/body-scoped value key, exhaustive equal/prefix/disjoint/may-alias relation, finite ownership-state transfer, and semantics -> HIR -> MIR authority/invalidation contract. |
| Temporary/drop authority | M-LIFE-01 gives supported lexical storage and materializing values typed HIR/MIR obligations, exact cleanup descriptors, lifecycle transitions, normal-edge verification, and recursive cleanup-owning global/static rejection. |
| Evaluation design | ADR 010 and Execution Section 4.2 define strict left-to-right evaluation, target-first assignment, direct destination materialization, LIFO full-expression obligations, reverse partial cleanup, and lexical dependency-first program initialization. |
| Target/layout queries | Exact `os`/`vendor`/`arch` facts and supported-triple errors feed one GTI-owned 64-bit little-endian layout. Type-only `sizeof`/`alignof` expose exact unsigned-64 frontend constants for supported scalars, pointers, integral scoped enums, passive unions, aliases, and positive concrete arrays; installed probes check the host facts against each native build target. |
| Performance measurement | A hermetic, threshold-free benchmark runner records strict workload descriptors, correctness digests, exact build commands and tool identities, emitted-code evidence, deterministic raw samples, and a checked-vector GTI/semantic-C++/idiomatic-C++ baseline. |
| Callable design | One accepted concrete identity, exact signature, read/mut/once capability, capture/lifecycle, and confined/owned escape contract serves algorithms, tasks, and callbacks; local copy/move environments plus exact generic return and one-field owner transport implement its current bounded lifecycle. |
| Concurrency design | ADR 008 defines explicit single-threaded/concurrent profiles, safe data-race freedom, transfer/share facts, owned-only automatic-join tasks, ordered scalar atomics, scoped mutex guards, global policy, and contained worker failure. Public managed threads, ordered scalar atomics including integer fetch operations, and mutex guards implement the bounded systems-ready surface. |
| Defined failure | ADR 007 defines allocation-free records, cleanup-preserving propagation, hosted/embedding/task containment, and original-record re-raise at join. Landed M-FAIL-01 slices provide exact outcome/propagation identity, deterministic artifact descriptors and detector sites, plus verified fixed-record failure edges and cleanup for eligible full-expression-root scalar operations and exact later local-detector or static direct-call arguments after prepared owners. |

MIR is the sole executable-body authority. It still has bounded verifier gaps
around complete ordered parameter/result materialization, additional nested or
owning failure edges, partial-constructor rollback, and double failure, but
those gaps cannot select another executable route. Object layout, ABI, and
generated representation are combined with optimized MIR at the complete
`LoweredProgram` frontier. Unsupported contracts fail before backend emission.

## Dependency Map

```mermaid
flowchart TD
  R["D-LANG-01 restriction ledger"] --> MA["D-MEM-02 adopted memory-model decision"]
  MD["D-MEM-01 memory-model proposal"] --> MA
  R --> EO["D-EXEC-01 adopted evaluation and full-expression contract"]
  R --> FC["D-FAIL-01 failure contract"]
  FC --> MA
  R --> DC["D-CALL-01 callable ownership and escape contract"]

  PA["M-OWN-01 place and ownership-state authority"] --> IP["M-OWN-02 indexed places and definite initialization"]
  IP --> TD["M-LIFE-01 temporary and active-drop authority"]
  EO --> TD
  IP --> SD["M-OWN-03 stored and escaping mutable dependencies"]
  TD --> SD
  IP --> EP["M-OWN-04 named expected-payload range loans"]
  TD --> EP
  TD --> OE["M-EXEC-01 per-family ordered MIR lowering"]
  FC --> MF["M-FAIL-01 + Q-FAIL-01 failure IR/runtime substrate"]
  OE --> MF
  OE --> MB1["M-BACK-01 scalar-leaf-v1 done"]
  MB1 --> MB2["M-BACK-02 executable-body authority cutover done"]
  MF --> MB2

  R --> DL["S-LAYOUT-01 target data-layout contract"]
  DL --> SA["S-LAYOUT-02 sizeof and alignof"]
  SA --> NR["S-ABI-01 native-record proposal"]
  NR --> NRI["S-ABI-02 bounded native records"]
  NRI --> NH["S-ABI-03 generated C/C++ bridge header"]
  SA --> AL["S-ALLOC-01 allocator and provenance contract"]
  TD --> AL
  FC --> AL

  R --> CP["I-CAP-01 private capability identity"]
  CP --> MF
  CP --> CT["C-TYPE-01 transfer/share capabilities"]
  MA --> CT
  CT --> CG["C-GLOBAL-01 global/static policy"]
  CG --> AT["C-ATOM-01 atomic first slice"]
  CG --> TH["C-THREAD-01 joined owned thread"]
  MB2 --> TH
  TD --> LC["L-CALL-01 foundational owned callables"]
  DC --> LC
  DC --> CC["C-CALL-01 thread-task contract"]
  CT --> CC
  MF --> CC
  CC --> TH
  SD --> MX["C-SYNC-01 mutex and guard-tied access"]
  TH --> MX

  IP --> RG["L-RANGE-01 dedicated element/range loans"]
  TD --> RG
  RG --> EP
  RG --> MI["L-RANGE-02 mutable iteration and invalidation"]
  MI --> VW["L-RANGE-03 spans and dynamic views"]
  VW --> ALG["L-RANGE-04 foundational algorithms"]
  LC --> ALG

  TD --> SUM["L-SUM-01 payload enums and matching"]
  TD --> Q["L-ERR-01 propagation operator"]
```

This is an abbreviated critical-path view. The `M-EXEC-01` edge into M-BACK is
per selected body family, not a requirement to finish the entire row before
production emission starts. Design rows may run in parallel. Rows that edit the
same semantic or lowering authority must serialize even if the graph has no
logical edge between them.

## Ready Queue

This is the sole compiler implementation queue. At adoption, already in-flight
work completed its original scope before the reconciled `main` commit activated
the exclusive structural gate. That gate has now passed; its preserved record
is the
[archived structural-refactor plan](archive/compiler-structural-refactor.md).

The current user-requested stdlib modernization order is the
[capability adoption campaign](#stdlib-capability-adoption-and-upgrade-campaign)
above. Its readiness table and detailed `L-UP` rows live here, not in a second
queue. They distinguish ordinary adoption from missing general compiler proofs;
completed structural/performance rows below are historical prerequisites.

The structural sequence completed on 2026-08-28. Its detailed measurements and
exact equivalence evidence are retained in the
[archived structural-refactor record](archive/compiler-structural-refactor.md).

| Order | ID | State | Phase outcome | Exit evidence |
| --- | --- | --- | --- | --- |
| 1 | `R-STRUCT-00` | **complete** | Freeze one exact behavioral, dependency, and build baseline. | Reconciled commit `4d4c8ae2`, the integration broad validation, controlled build measurements, and the complete artifact/MIR/diagnostic oracles are recorded in [`compiler-structural-baseline.json`](compiler-structural-baseline.json). |
| 2 | `R-STRUCT-01` | **complete** | Consolidate semantic request reset and partition compiler/backend tests by ownership. | One request owner/reset passes reuse regressions; the manifest-backed 125-test compiler aggregate, 81-test emitter census, focused suites, and exact-census corpus sweeps pass. |
| 3 | `R-STRUCT-02` | **complete** | Separate lexical-unit/directive ownership, semantic catalog and body/session state, callable authority, constexpr/flow services, and HIR instance access. | Source/cache/recovery and the 125-test semantic aggregate pass; the sealed catalog, explicit callable projection, narrow HIR instance provider, three semantic headers, static boundary test, self-contained compilation, and exact three-object analyzer-facade rebuild prove the ownership/include boundary. |
| 4 | `R-STRUCT-03` | **complete** | Separate MIR model/query, canonical edit/replay, body/program verification, failure effects, body/program lowering, hosted startup, and lowered-program closure. | Separate service/header boundaries, fail-closed component mutation tests, optimizer suites, MIR/lowered census, hosted startup, and the 444-mode cutover oracle pass unchanged. |
| 5 | `R-STRUCT-04` | **complete** | Make one immutable C++ body-emission plan authoritative and separate backend preflight, support text, and output writers. | Pointer-free body/program plans outlive input, preflight remains fail-closed, writer responsibilities are separate, and baseline C++20/C++23 O0/O1/O3 artifacts are byte-identical. |
| 6 | `R-STRUCT-05` | **complete** | Close dependency, include, CMake/test, installed-target, and measured build boundaries. | Exact 444-configuration equivalence, 141/141 CTest, installed libraries/toolchain, formatter/Tree-sitter/LSP/docs, one-object focused rebuilds, -48.9% longest Release edge, and bounded shipped-binary/runtime latency pass; the measured clean-build and compiler-archive costs are explained in the archived report. |

From 2026-08-26 through 2026-08-27, the runtime-performance campaign was the
previous exclusive compiler gate. `O-PERF-00` through `O-PERF-05` and
`O-GAME-01` closed in order. The completed rows and their exact evidence remain
here as the baseline for later measured optimizer work. The later structural
gate is also complete; future rows follow their declared prerequisites and the
permanent executable-authority rule below.

While an exclusive row is active, the only exceptions are correctness or
security fixes, release/CI unblockers, and changes explicitly named as its
prerequisites. An exception must remain narrowly scoped, preserve the campaign
baseline, and state why it cannot wait. Documentation and editor-only work may
continue when it does not change compiler semantics or consume the active
compiler ownership boundary. An exclusive gate never permits unsafe shortcuts,
benchmark special cases, weaker failure semantics, or skipped verification.

During an exclusive campaign, complete one row before beginning the next. A
failed upper-bound proxy can complete a row only when its raw evidence
identifies the next bottleneck or proves the proposed family has no material
value. Update this table and the detailed row together; do not copy the
sequence into another plan.

| Order | ID | State | Prerequisite | Phase outcome | Exit evidence |
| --- | --- | --- | --- | --- | --- |
| 1 | `O-PERF-00` | **complete** | current benchmark runner and reconciled audits | Freeze reproducible attribution workloads and separate GTI optimization effects from native optimization effects. | Exact clean commit `a2bd29dc` records conclusive checked-array, checked-vector, scalar-chain, scalar-FP, control, game-update, and managed-thread baselines; strict generated arithmetic ceilings; and fixed-native O0/O1/O2/O3 attribution. |
| 2 | `O-PERF-01` | **complete** | `O-PERF-00` | Prove bounded loop-carried arithmetic safe with the existing MIR safety/replay architecture. | Exact clean commit `f1a4075f` discharges all five hot arithmetic outcomes and the three hot fixed-array bounds outcomes; GTI measured 1.011x idiomatic C++ at O2 and 0.999x at O3 with no control regression above 0.4%. |
| 3 | `O-PERF-02` | **complete** | `O-PERF-01` | Carry exact range/container facts through one closed static-call family only as required by the checked-vector client. | Exact clean commit `d6374eb9` uses bounded contextual summaries to discharge six overflow outcomes and four arithmetic-domain outcomes; the O2 vector median fell from 1,213.192 ms to 558.336 ms while all conservative near misses and independent replay passed. |
| 4 | `O-PERF-03` | **complete** | `O-PERF-02` | Establish one logical vector length authority and remove only proved redundant prefix-storage checks. | Exact clean commit `eaaea34d` removes vector's mirrored count and discharges seven public-accessor private prefix rechecks. Follow-up exact commit `4dbb50bd` carries the existing scalar envelope through exact indexed return borrows and finite closed-call loop bounds; vector medians are 1.042x idiomatic at O2 and 1.049x at O3. |
| 5 | `O-PERF-04` | **measured defer** | `O-PERF-03` plus a genuinely opaque checked-loop client | Select or reject one exact checked-loop fast path for operations that static analysis cannot discharge. | Exact clean commit `4dbb50bd` records a semantics-preserving source preflight at 538.225 ms versus 540.055 ms checked GTI, only 0.34% available value; checked GTI is already 1.003x idiomatic, so the required 30% entry gate rejects versioning. |
| 6 | `O-PERF-05` | **complete (measured no-op)** | `O-PERF-04` | Remove one measured residual backend/ABI representation cost after safety side exits are addressed. | Exact clean commit `9da44477` measures scalar GTI at 1.005x semantic C++ and the closed-call vector client at 0.109x semantic and 1.044x idiomatic; native hot loops contain no surviving call/failure boundary, so no backend change is justified. |
| 7 | `O-GAME-01` | **complete** | `O-PERF-00` through `O-PERF-05` | Prove the combined result in a deterministic game-style update workload. | Exact clean implementation commit `c5bbfaa8` records 58.503 ms GTI versus 208.126 ms semantic C++ and 58.454 ms idiomatic C++, identical complete-state output, 53 replayed safety discharges, stable generated artifacts, bounded compile/memory controls, and conclusive fixed-array, vector, scalar, and managed-thread controls. |

`P-MEASURE-01` benchmark breadth was consumed by `O-PERF-00`. The end of that
campaign made `A-CACHE-01` and other unfinished rows logically eligible under
their own prerequisites. The later `R-STRUCT` gate then controlled scheduling
until it completed on 2026-08-28. Completed performance and structural rows
remain historical prerequisites and do not occupy the queue.

`M-BACK-02` no longer blocks those outcome rows. After `R-STRUCT-05`, the
permanent executable-authority gate remains: any new executable feature must
extend verified MIR, its verifier, and the general backend preflight; it may
not add an AST/semantic/HIR-only C++ body case.

## Engine Embedding Delivery Campaign

> **Campaign status:** foundation complete through `E-EMBED-01A`.
> `E-EMBED-01B` is the next ready row; wrapper generation, artifact
> publication, tooling, and the dynamic-host gate remain blocked behind it.

This campaign delivers the two capabilities required before an engine can
integrate GTI as a trusted native gameplay language:

1. a project `library` target that produces a loadable shared library rather
   than an executable; and
2. a generated, versioned, host-to-GTI C embedding boundary over explicitly
   selected GTI functions.

Completion means an engine can begin integration. It does not mean that GTI
has become a dynamic, sandboxed, or ABI-stable language. The acceptance client
is one host-owned process loading an unloadable gameplay module and invoking it
on the context-creating thread. The engine remains responsible for ECS and
resource storage, generation-checked engine handles, scheduling, file
watching, state migration, and the policy that chooses when to swap modules.

### First-Slice Contract

The following decisions bound every row in this campaign:

- The target is a **trusted native gameplay module**, not an untrusted mod
  sandbox. A VM, JIT, WebAssembly backend, instruction quota, capability
  sandbox, and process isolation are separate outcomes.
- Project configuration selects `kind = "library"` and
  `linkage = "shared"`. Static libraries, separately compiled GTI dependencies,
  and a stable ABI for ordinary GTI values remain out of scope. The target has
  one root source graph and shall not define `main`.
- Export selection is artifact policy in `gti.toml`, not new source syntax. An
  export maps one exact qualified GTI free-function name to one explicit C
  symbol. The selected function remains an ordinary GTI function internally;
  only its generated wrapper is public.
- The first boundary admits non-generic, non-overloaded namespace-scope source
  functions with bodies. Methods, constructors, operators, lambdas, virtual
  dispatch roots, compiler-private declarations, native imports, and generic
  or overloaded sets are rejected as exports.
- Wrapper parameters and results use a reviewed subset of the existing C ABI:
  `void`, fixed-width integers, `float`, `double`, valid passive `[[c_abi]]`
  records, and one-level pointers to valid passive records or nominal
  `[[c_opaque]]` engine handles. References, owners, ordinary classes,
  containers, payload enums, `expected`, callable values, strings, nested
  pointers, ownership transfer, and native C++ types do not cross this first
  boundary. Pointer use inside GTI retains the existing lexical `unsafe`
  requirement.
- Every exported wrapper takes the generated module context first, returns a
  versioned embedding status, writes a non-`void` result only on success, and
  writes an immutable `gti_failure_record_v1` only for a GTI defined failure.
  Invalid host arguments are embedding-status errors and do not forge GTI
  failure records.
- One loaded library image owns at most one live context. Creation,
  invocation, and destruction are serialized on the creating host thread;
  recursive entry and concurrent entry fail before GTI code begins. This
  matches the first game-loop client without claiming foreign-thread entry or
  per-context copies of program globals.
- Context creation explicitly runs the verified GTI module-initialization body;
  no user GTI initialization is moved into an uncontrolled native dynamic
  initializer. Existing restrictions on cleanup-owning global/static state
  remain in force. Successful scalar/passive global effects persist between
  calls and are reset only by complete destruction/unload/reload.
- A defined failure completes invocation-owned MIR cleanup, invokes the
  configured observer under ADR 007, returns the original record, and performs
  no default report or process exit. Earlier effects are not rolled back.
  Re-entry is allowed only when the generated context guard proves retained
  state structurally valid; otherwise the context becomes poisoned. A failure
  during failure cleanup, observer contract violation, escaping native
  exception, abort, or long jump remains process-terminal and is never
  translated into a recoverable GTI record.
- The descriptor and its failure-site data remain valid only while the library
  is loaded. The host must copy rendered information it wants to retain and
  must destroy the context before unloading. Calls after destruction and
  unload with a live context are host contract violations.
- A GTI function-to-C callback adapter is rejected in an unloadable library
  target for this slice. Its current process-lifetime address contract cannot
  safely survive library unload. Engine-to-GTI events use generated exports;
  GTI-to-engine calls use the existing outbound `extern "C"` surface and a
  linked engine C shim.
- The embedding ABI is versioned independently of `VERSION`. Its C records,
  status values, descriptor, wrapper calling convention, result publication,
  and symbol visibility are stable for ABI version 1. Generated C++ names and
  ordinary GTI layout are explicitly not part of that ABI.

The initial manifest shape to freeze under `E-EMBED-00` is:

```toml
[targets.gameplay]
kind = "library"
linkage = "shared"
root = "src/gameplay.gti"

[[targets.gameplay.exports]]
function = "game::update"
symbol = "game_update_v1"
```

There must be at least one export. Export declarations are ordered for
diagnostics but canonicalized by public symbol for descriptor/header emission
and cache identity. Duplicate functions, duplicate symbols, implementation-
reserved names, fixed embedding-runtime names, and names unsafe in either C17
or C++20/C++23 fail before native compilation.

### Ownership And Phase Boundaries

| Fact or operation | Producer and owner | Consumers and lifetime |
| --- | --- | --- |
| Target kind, linkage, root and requested export spellings | `gti_driver` manifest/project resolution | Immutable build and analysis plans, cache/build-report identity, metadata, and LSP project facts for one request. |
| Library program kind and exact selected export symbols | Frontend/semantics from the driver request | HIR root discovery, lowered-program ABI facts, diagnostics, language queries, and backend planning for one snapshot. |
| Exported executable closure | HIR instance discovery rooted at module initialization plus every selected export | MIR lowering, effect/failure closure, verifier, and backend preflight for one artifact. Unselected declarations do not become ABI merely because they were parsed. |
| Invocation cleanup and defined-failure propagation | Verified MIR bodies and program verifier | Generated wrapper body. The wrapper selects containment policy but cannot reconstruct cleanup from AST, HIR, or C++ exceptions. |
| C type projection, wrapper symbol and module descriptor rows | Immutable lowered-program embedding plan | C header backend and C++ backend. Both consume the same rows; neither independently guesses ABI eligibility or spelling. |
| Context guard, observer firewall and fixed ABI records | Narrow runtime C ABI plus compiler-generated containment code | One loaded library/context. Runtime supplies mechanisms; generated code owns initialization, exact calls, cleanup, poison decisions, and result/failure publication. |
| Native linker command, visibility, output names, publication and cache | `gti_driver` native/artifact layers | Shared library and matching generated header. The CLI only presents paths/status. |
| Engine resources, handles, scheduling and reload state | Embedding host | Remain outside the compiler/runtime. GTI sees bounded passive values or non-owning opaque handles only. |

The compiler should extract a shared immutable C ABI projection service from
the current native-header path when necessary. It must not parse a generated
header, maintain a second type allowlist, or let the driver inspect semantic
types. Likewise, the library target reuses the existing frontend, HIR, MIR,
optimizer, backend, runtime, and native-toolchain path; it is not a second
compiler pipeline.

### Serialized Implementation Sequence

The reconciled foundation ships its compiler SDK request/root metadata and
explicit disabled-library diagnostics in 0.382.0. This is not library activation
or a usable host ABI implementation. The original campaign's deferred-version
assumption is revised for that observable packaged SDK surface; activation
still requires its own surface/version review and the complete acceptance gate.

Rows may land as reviewable commits on one campaign branch, but the manifest
must not accept a user-visible half-target on a release branch. `library`
activation, its minor `VERSION` advance, and public documentation happen only
when the shared artifact and boundary pass `Q-EMBED-01`.

| Order | ID | State | Prerequisite | Phase outcome | Exit evidence |
| --- | --- | --- | --- | --- | --- |
| 1 | `E-EMBED-00` | **complete** | Completed C ABI, failure, MIR-backend, project and runtime foundations | Freeze the embedding ABI, manifest schema and acceptance fixture before implementation. | Commit `ae8dbb71` accepts ADR 026 and freezes every ABI-v1 status, context/descriptor/wrapper prototype, 64-bit record layout, lifecycle, symbol, failure, and unload rule. The C17/C++20 header oracles and non-executable gameplay fixture pass in the complete matrix. |
| 2 | `B-LIB-01` | **complete** | `E-EMBED-00` | Carry a dormant shared-library target and export request through driver and compiler ownership without exposing incomplete artifacts. | Commit `1fcbe268` carries library/shared/export facts and exact spans through manifest, workspace, project facts, build plan, cache identity, compilation request, and internal `check`. Focused project/CLI tests and the complete matrix prove missing/unknown linkage, selection and command legality, no executable regression, and default `GTI-B2000` rejection; no shared artifact is exposed. |
| 3 | `E-EMBED-01A` | **complete** | `B-LIB-01` | Resolve exact exports semantically and make them the library's executable roots. | Commit `1fcbe268` resolves and validates the export set once in semantics, then exact-compares immutable canonical roots through HIR, MIR, optimization, failure metadata, and `LoweredProgram`. The positive/negative matrix covers declaration kinds, admitted and excluded boundary families, collisions/reserved names, `main`, callbacks, unused declarations, forged plans, and request/declaration-order independence. |
| 4 | `E-EMBED-01B` | **ready** | `E-EMBED-01A` | Implement the reusable host-call containment and context lifecycle before dynamic loading obscures failures. | A public C17-compatible runtime header and private implementation supply versioned context/status primitives. A statically linked host harness proves create/init, repeated sequential calls, successful result publication, exact defined-failure return after cleanup, observer ordering, valid re-entry, poison rejection, recursive/concurrent/wrong-thread rejection, destruction, null/out-parameter validation, descriptor lifetime, and the terminal double-failure/observer/native-exception firewalls. Sanitizer coverage checks context and output lifetimes. |
| 5 | `E-EMBED-01C` | **blocked** | `E-EMBED-01B` | Generate the exact C wrappers, artifact descriptor and matching host header solely from lowered ABI facts and verified MIR. | C and C++ compile oracles consume the same generated header. Wrapper runtime tests cover every admitted scalar/record/pointer parameter and result, `void`, success, propagated failure, prior effects, invocation-owned cleanup, module initialization failure, symbol stability and reserved-name diagnostics at C++20/C++23 and O0/O3. Backend preflight fails closed if a selected body, result publication, failure edge, or representation row is incomplete. No wrapper calls an unverified source-body route. |
| 6 | `B-LIB-02` | **blocked** | `E-EMBED-01C` | Publish a self-contained shared library plus its embedding header through the normal project build. | `gti build` emits platform-correct `.dylib`, `.so`, or `.dll` output with PIC-compatible hidden runtime/support code and only the descriptor/context/export surface visible; Windows import artifacts are managed explicitly. Publication is atomic, paths remain contained, clean removes managed outputs, metadata/build reports name every artifact, and a schema-bumped cache keys target kind, linkage, export plan, embedding ABI, complete source graph, runtime and native toolchain. `run` and ordinary `gti test` reject library targets; direct mode remains unchanged. |
| 7 | `T-EMBED-01` | **blocked** | `B-LIB-02`; extends the completed `T-LSP-04` project-facts client | Make the library target usable during real editing and integration rather than only from a terminal fixture. | Driver-owned project facts expose library/linkage/export configuration; the LSP analyzes a library root without inventing missing-`main` diagnostics, applies exact export diagnostics to dirty overlays, and rejects stale generations. CLI/reference documentation shows the manifest, generated files, ABI restrictions, failure handling and host lifecycle. No new Tree-sitter or formatter rule is added because the campaign adds no GTI source syntax. |
| 8 | `Q-EMBED-01` | **blocked** | `T-EMBED-01` | Prove both requested capabilities together in a real dynamically loaded engine-style vertical slice. | A C17 and C++ host builds the module with the installed toolchain, loads it through `dlopen`/`dlsym` or `LoadLibrary`/`GetProcAddress`, validates ABI/descriptor identity, creates the context, calls initialize/update/event/shutdown-style exports repeatedly, observes one module call into a linked engine C shim, receives and resolves an exact GTI failure without host termination, calls again according to the validated re-entry rule, destroys the context, unloads, reloads, and repeats. Linux, macOS and Windows CI cover the platform surface; C++20/C++23 and O0/O3 produce equivalent behavior; export-table inspection finds no accidental GTI/C++ symbols; full compiler, driver, runtime, LSP, installed-toolchain and corpus gates pass. |

### Campaign Exit And Handoff To The Engine

Capability 1, **native library targets**, is complete only when `B-LIB-02` and
`Q-EMBED-01` pass. Capability 2, **generated host-to-GTI embedding**, is
complete only when `E-EMBED-01A/B/C`, `T-EMBED-01`, and `Q-EMBED-01` pass. Do
not mark the campaign complete from a generated `.so`/`.dylib`/`.dll`, a
header-only mock, direct calls to mangled C++ symbols, or a success-only host
demo.

At the exit gate the engine team receives:

- one documented `gti.toml` library/export configuration;
- one platform shared library and its generated C17/C++ header;
- a versioned descriptor/context/status/failure contract;
- a complete host lifecycle and unload checklist;
- an engine C-shim example using opaque handles and passive frame/event
  records; and
- exact diagnostics for ABI mismatch, invalid context, unsupported exports,
  stale/destroyed calls and poisoned state.

Automatic file watching, background compilation, frame-boundary swap policy,
serialization and state migration, ECS/component registration, editor UI,
generation-checked engine handle storage, and retaining old modules until the
engine releases them are then engine integration work. The compiler may later
add convenience around those workflows, but none may weaken the v1 boundary.

### Explicitly Deferred Follow-On Work

- multiple contexts per library image or per-context program-global storage;
- concurrent, recursive, worker-thread or foreign-thread host entry;
- module-owned object handles or GTI allocation ownership crossing the ABI;
- retained GTI-to-C callbacks and unload-safe callback-registration tokens;
- dynamic strings, slices, containers, callable tables or `expected` projected
  directly across the boundary;
- automatic hot reload, state schema reflection or state migration;
- static GTI libraries, separately compiled GTI packages, binary modules or a
  stable ABI for ordinary GTI types;
- arbitrary C++ linkage, exception translation or host unwinding; and
- untrusted-mod sandboxing, bytecode, JIT or WebAssembly execution.

One of these items advances only with a concrete engine client and its own
ownership, lifetime, failure and compatibility contract. It is not required to
declare capabilities 1 and 2 ready for the first engine integration.

## Phase D: Cross-Cutting Language Decisions

These rows intentionally produce specifications or decisions before compiler
features. A design row must not smuggle in syntax or a runtime ABI merely to
make the proposal feel concrete.

### D-LANG-01: Restriction Ledger

- **State/role:** done; readiness roles recorded in the maintained
  [`language-alignment.md`](language-alignment.md) restriction ledger.
- **Prerequisites:** none.
- **Scope:** Expand [`language-alignment.md`](language-alignment.md) into a
  maintained ledger. For each current restriction, record whether it is a
  permanent safety/simplicity rule, an unimplemented proof, an unimplemented
  lowering, a library omission, or an undecided language choice. Record the
  readiness role, user-facing client, and the plan row that owns any change.
- **Required coverage:** at least the complete gap list in the external
  language audit, all explicit specification gaps in `docs/language/`, and all
  restrictions that were then justified by the transitional C++ backend.
- **Non-goals:** changing current language semantics or choosing syntax merely
  to complete the classification.
- **Exit gate:** no restriction says merely “deferred”; it names why, the
  current client/role, and what evidence permits reconsideration.
- **Completion evidence:** the ledger covers every external language-audit
  finding, every original alignment area, every explicit language-specification
  gap, and the architecture audit's backend-visible language gaps. Each entry
  has one class, readiness role, user-facing client, owner, and reconsideration
  evidence. ADR 012 supersedes the former version split and makes bounded
  concurrency, native records/callbacks, public allocator capability, payload
  sums, propagation, domain operators, and one associative container part of
  systems readiness.
- **Unlocks:** informed systems-readiness priorities and all other design rows.

### D-MEM-01: Concurrency And Memory-Model Proposal

- **State/role:** done; design-first proposal completed in
  [`concurrency-memory-model.md`](concurrency-memory-model.md). D-MEM-02 later
  adopted it in ADR 008.
- **Prerequisites:** current ownership/execution contracts; completed
  `D-LANG-01` records classifications that agree with this proposal.
- **Scope:** Create a focused plan under `docs/plans/`, not an ADR and not code.
  It must propose answers for:
  - whether safe GTI makes data races unrepresentable/ill-formed and what
    remains undefined inside `unsafe` raw-pointer code;
  - structural transfer and sharing capabilities for primitives, aggregates,
    classes, interfaces, `unique_ptr`, future shared owners, references,
    borrowed-state carriers, raw pointers, globals, and captured values;
  - how cleanup-owning or native-resource types opt out of structural transfer,
    whether user opt-in is permitted, and which opt-in requires an explicit
    unsafe proof because thread affinity is not derivable from fields alone;
  - whether any borrow can cross a thread boundary in the first model, and the
    additional proof required for scoped borrows later;
  - how `mut`, mutable static storage, interior mutability, and synchronized
    access interact;
  - the atomic value domains, initial sequential-consistency baseline, future
    memory-order vocabulary, and happens-before obligations;
  - thread creation, join, detach, thread-local state, initialization,
    destruction, failure, and process termination;
  - synchronization operations and conservative MIR/optimizer effects;
  - native/FFI threads entering GTI, runtime callbacks, and raw-pointer proof
    obligations; and
  - a staged semantic, HIR, MIR, backend, runtime, stdlib, diagnostic, and test
    matrix.
- **Non-goals:** choosing final source spelling where a semantic contract is
  sufficient, implementing `pthread`, adding `std::thread`, or borrowing C++'s
  memory model by reference.
- **Exit gate:** every question above has one recommended answer; genuine
  alternatives are isolated with consequences and a requested decision.
- **Completion evidence:** the focused proposal defined safe data-race
  freedom, unsafe race obligations, transfer/share derivation and nominal
  policy, first-model borrow/global/atomic/thread boundaries, FFI entry,
  synchronization effects, and the staged implementation/test matrix. It
  isolated executable horizon, automatic join, worker failure, and final
  spelling; D-MEM-02 has resolved those choices.
- **Unlocked:** completed `D-MEM-02`; neither design row unlocks concurrency
  code by itself.

### D-MEM-02: Adopt The Memory-Model Boundary

- **State/role:** done; durable decision adopted in
  [ADR 008](../decisions/008-safe-concurrency-memory-model.md).
- **Prerequisites:** `D-MEM-01`, `D-LANG-01`, and `D-FAIL-01`, all complete.
- **Scope:** Resolve the remaining choices in an ADR and update the current
  execution and ownership specifications with the accepted single-threaded and
  concurrent boundary. Adopt the restriction ledger's decision that
  transfer/share facts and concurrent-global policy precede public execution.
  ADR 012 now assigns the bounded public thread/atomic/mutex profile to the
  systems-readiness work-queue lane.
- **Required invariant:** the accepted rule must compose with the existing
  borrow model rather than adding a parallel runtime-only type-trait system.
- **Exit gate:** the ADR, language docs, roadmap horizon, and restriction ledger
  agree; compile-fail examples are specified for values that cannot cross or
  be shared across a thread boundary.
- **Completion evidence:** ADR 008 and canonical execution/ownership semantics
  adopt explicit single-threaded/concurrent profiles, safe data-race freedom,
  structural transfer/share with safe negative and unsafe positive nominal
  policy, owned-only crossing, concurrent globals, SC first atomics, automatic
  join, contained worker failure, native-entry obligations, and the
  bounded-first executable scope. The D-MEM-01 review matrix names
  the required positive and negative implementation cases without inventing
  source spelling before C-TYPE-01.
- **Unlocks:** `C-TYPE-01` design and the concurrency implementation lane.

### D-EXEC-01: Evaluation Order And Full Expressions

- **State/role:** done; prerequisite `D-LANG-01` is done; durable decision
  adopted in [ADR 010](../decisions/010-deterministic-evaluation-and-full-expressions.md)
  and [Execution Section 4.2](../language/execution.md#42-evaluation-order).
- **Prerequisites:** `D-LANG-01`.
- **Scope:** Specify evaluation order for call receivers, arguments, operators,
  assignment targets, initializers, conditional/short-circuit expressions,
  cross-source module/static initialization, temporary destruction, and
  cleanup. State when a full expression ends and when transient loans end.
  Choose a rule GTI can lower deterministically on every supported C++ backend.
- **Non-goals:** implementing emitter hoisting in the same change. Earlier IIFE
  and statement-hoisting sketches are unsound without explicit temporary
  lifetimes and remain rejected as semantic authority.
- **Exit gate:** canonical examples have one required order and destruction
  trace; the plan identifies the exact HIR/MIR facts and the backend migration
  slice needed to realize them.
- **Completion evidence:** strict left-to-right receiver, argument, operand,
  capture, pack, and initializer order; target-first one-time assignment-place
  evaluation; direct destination materialization; LIFO full-expression
  obligations; reverse successful partial cleanup; return publication; ordered
  hosted count/argument setup; and a lexical dependency-first program-
  initialization walk have required traces.
  The decision assigns source boundaries and the program-init plan to
  semantics; `FullExpressionId` and concrete operand roles to HIR;
  temporary/drop obligations to M-LIFE; ordered CFG and invalidation to M-EXEC;
  and production emission to M-BACK. Conservative semantic restrictions remain
  explicit verifier/lowering gaps; the production compatibility emitter no
  longer exists.
- **Unlocked:** `D-COMPAT-01`, `M-OWN-02`, and `M-LIFE-01` are complete, and
  the bounded `M-EXEC-01` ordinary-call and ordinary-constructor input
  schedules are implemented. The
  conservative
  both-argument overlap restriction may be removed per operation family only
  after ordered MIR and its matching production backend migration are
  authoritative.

### D-FAIL-01: Defined Failure And Embedding Contract

- **State/role:** done; prerequisite `D-LANG-01` is done; durable decision
  specified in
  [Execution §4.10](../language/execution.md#410-defined-runtime-failure),
  with rationale in [ADR 007](../decisions/007-defined-runtime-failure.md).
- **Prerequisites:** `D-LANG-01`.
- **Scope:** Decide the stable failure category, source-location token, report
  contract, exit status, cleanup performed, panic/termination hook, behavior
  when embedded, and boundary between terminating checks and APIs returning
  `expected`. Cover allocation failure and future thread failure explicitly.
- **Non-goals:** source exceptions, catch/resume syntax, native exception-ABI
  unwinding, or making programmer contract violations recoverable through
  ordinary GTI source. Compiler-managed non-resumable failure propagation is
  required to reach cleanup-preserving containment boundaries.
- **Exit gate:** all existing checked-integer, conversion, indexing, owner,
  private-storage, and allocation failures map to the contract; the runtime can
  later expose one narrow entry point without erasing categories.
- **Completion evidence:** stable `GTI-R0001` through `GTI-R0014` categories,
  artifact-qualified source tokens, status 70 and exact report framing,
  deterministic cleanup and double-failure behavior, observer constraints,
  program/embedding/task/callback boundaries, expected-versus-terminating
  policy, and every current trap mapping are canonical. Current emitter aborts
  remain an explicit M-FAIL-01/Q-FAIL-01 implementation gap.
- **Unlocked:** D-MEM-02's worker-policy adoption is complete. `M-FAIL-01`,
  public allocation design, hosted concurrency, and a future generated
  embedding boundary remain downstream clients.

### D-CALL-01: Callable Ownership And Escape Contract

- **State/role:** done; prerequisite `D-LANG-01` is done; design-first decision
  recorded in
  [`callable-ownership-and-escape.md`](callable-ownership-and-escape.md).
- **Prerequisites:** `D-LANG-01`.
- **Scope:** Define one GTI-owned callable model covering exact parameter and
  return shape, invocation-count capability, capture ownership, movement,
  destruction, escape, recursive/self-reference restrictions, and generic
  identity. Isolate the smaller contracts needed by range algorithms, consumed
  thread tasks, and native callbacks so those features reuse one representation
  without waiting for every ergonomic callable feature.
- **Non-goals:** choosing all capture syntax, cloning `std::function`, adding
  threads or callbacks, or using a C++ closure type as GTI identity.
- **Exit gate:** algorithm, owned-task, and native-callback examples map to one
  representation and capability vocabulary; unsupported borrowed escape and
  invocation-count cases have an explicit diagnostic contract.
- **Completion evidence:** one concrete lexical/class/function-item identity
  model, exact signatures, read/mut/once invocation capabilities, copy/move
  capture and lifecycle rules, confined versus exact owned transport, generic
  identity, recursion exclusions, cross-phase records, diagnostics, and test
  gates now serve all three clients without choosing final syntax or changing
  current lambda behavior.
- **Unlocks:** `L-CALL-01`, `C-CALL-01`, and `S-CALL-01`.

### D-COMPAT-01: Compatibility And Edition Policy

- **State/role:** done; independent release-policy decision adopted in
  [ADR 011](../decisions/011-language-compatibility-and-editions.md).
- **Prerequisites:** `D-LANG-01`, `D-EXEC-01`, `D-FAIL-01`, and `D-MEM-02`,
  all complete.
- **Scope:** Define how a 1.x compiler preserves old source meaning and how a
  future incompatible memory-model, evaluation, or ownership change would be
  selected. State that the current non-textual, direct-visibility `#include`
  spelling remains supported and how a future edition could introduce a module
  vocabulary without changing old source. Do not add an `edition` manifest key
  until the compiler actually implements and enforces it.
- **Exit gate:** release and deprecation policy is public, testable, and does
  not permit silently ignoring an unknown compatibility selector.
- **Completion evidence:** ADR 011 and Scope Section 1.6 define SemVer behavior
  before and after 1.0, Edition 1's protected meaning, correction and urgent
  safety rules, package/source-graph selection, a permanent Edition 1 default,
  hard failure for unknown selectors, deprecation/removal windows, and the
  stable non-textual direct-visibility `#include` contract. The future
  manifest/direct selectors remain rejected until one coherent implementation
  row can enforce and publish them.
- **Unlocks:** `Q-DEPRECATION-01` after `T-LSP-01`. A second edition requires a
  new bounded selector row; it cannot be introduced as an incidental manifest
  or parser change.

## Phase M: Places, Lifecycles, And Executable MIR Authority

This is the principal implementation critical path. The work should land in
small complete operation families, not as one replacement of semantic
analysis, HIR, MIR, and the backend.

### M-OWN-01: Ownership-State And Place Authority Decision

- **State/role:** done; design-first decision recorded in
  [`place-and-ownership-state.md`](place-and-ownership-state.md).
- **Prerequisites:** existing stable root/field/dereference loan places and MIR
  dominance.
- **Scope:** Define one GTI-owned `PlaceKey`/overlap contract for roots,
  fields, constant fixed-array indices, dynamic indices, dereferences, raw
  addresses, and opaque call results. Decide which flow facts semantics must
  still choose, which facts HIR carries, and which fixed-point properties MIR
  verifies or computes. Preserve ADR 001 explicitly.
- **Non-goals:** implementing every projection, moving semantic validity into
  the optimizer, or introducing a generic solver before naming its first
  client.
- **Exit gate:** examples for equal, prefix, disjoint, and may-alias places have
  one phase owner and one conservative answer; snapshot/body identity and
  invalidation rules are explicit.
- **Completion evidence:** one `PlaceDomain`/value-owned `PlaceKey` contract
  covers program/body/formal/receiver/temporary/materialized/raw/opaque roots
  and named-field, constant/dynamic-index, checked, raw, and opaque
  projections. The exhaustive relation is equal, either strict-prefix
  direction, disjoint, or may-alias. A finite uninitialized/available/moved
  state-set transfer assigns source validity and diagnostics to semantics,
  concrete keys/events to HIR, and reachable CFG fixed-point verification to
  MIR. The example matrix and invalidation table define snapshot, concrete
  instance, lifetime-epoch, projection dependency, and edit boundaries.
- **Unlocks:** `M-OWN-02` and `M-LIFE-01`, which are now complete; bounded range
  and allocator design subsequently became ready, and `S-ALLOC-01/02` are now
  complete.

### M-OWN-02: Indexed Places And Definite Initialization

- **State/role:** done; prerequisite `M-OWN-01` is done; bounded-first
  implementation.
- **Scope:** Add directly owned fixed-array constant-index places first. Track
  available, moved, and restored state across every reachable branch, loop
  backedge, return, and drop. Dynamic indexes remain may-alias until a separate
  proof exists. Extend in prompt-sized families: local arrays, fields containing
  arrays, then supported checked-owner projections.
- **Non-goals:** vector slot extraction, raw-pointer partial moves, or a general
  allocation model.
- **Focused tests:** move one element, reject whole-owner use while partial,
  restore before whole-owner use, preserve exact partial state at a drop
  boundary, disjoint constant elements, dynamic-index conservatism, joins and
  loops, HIR/MIR place identity, forged verifier cases.
- **Exit gate:** semantics and MIR agree on availability at every exit; no
  backend C++ behavior repairs an invalid state.
- **Completion evidence:** one shared `PlaceKey`/relation/state/event vocabulary
  now covers directly owned local arrays and fields containing fixed arrays.
  In-range constant elements move and restore independently; dynamic indices
  remain may-alias. Semantic branch/loop state, concrete HIR domains/events,
  and the reachable MIR fixed point agree. Focused tests cover whole-owner use
  while partial, disjoint elements, branch and loop joins, partial drop
  boundaries, distinct frontend snapshots/incompatible domains, forged event
  identity, missing restoration, use-before-initialization, and double
  initialization; CLI execution passes at O0/O3 and in the supported C++20
  compatibility mode.
- **Unlocks:** `M-LIFE-01`, payload enums, richer storage, and precise range
  elements.

### M-LIFE-01: Explicit Temporary And Drop Obligations

- **State/role:** done; `M-OWN-02` and `D-EXEC-01` are done; systems-readiness
  prerequisite shared by every accepted outcome lane.
- **Scope:** Give lexical storage, MIR temporary places, and owning SSA results
  explicit typed drop obligations. Model ownership transfer, moved-from
  structural destruction, reverse construction order, full-expression cleanup,
  path-conditional initialization, and normal failure-free exits. Retain exact
  destructor identity and active-drop requirements in MIR. Reject namespace
  globals and static fields whose concrete type requires active cleanup,
  including enclosing aggregates and generic instances, because GTI has no
  global/static shutdown authority.
- **Non-goals:** source/native exceptions, failure propagation, partial
  constructor rollback, global/static shutdown, custom move bodies, or
  aggregate drop trees beyond the supported place slice. M-FAIL-01 consumes
  this failure-free drop authority and adds the supported failure edges,
  rollback, and boundary cleanup required by Execution §4.10.
- **Focused tests:** discarded owning call result, nested temporary arguments,
  conditional temporary, short-circuit arms, return transfer, break/continue,
  reverse order, declared-cleanup global/static/aggregate rejection,
  double/missing drop mutations, and join-state mismatch.
- **Exit gate:** a MIR dataflow/verifier proves each supported obligation is
  initialized, transferred, and structurally dropped exactly once on every
  normal edge; cleanup-owning global/static storage is rejected before backend
  entry; runtime behavior matches at O0/O3.
- **Completion evidence:** Semantics now selects AST full-expression roots. HIR
  maps them to body-local identities and exact lexical/value drop descriptors.
  MIR maps those obligations to concrete places and records initialize, move,
  reparent, replace, transfer-out, and drop events; its fixed point rejects
  inactive sources, double/missing or out-of-order cleanup, forged boundary or
  destructor metadata, non-consuming transfer operands, and invalid predecessor
  conditionality. Reached full expressions retain ordered obligation membership
  and one verified cleanup boundary. Logical and conditional branch-local
  temporaries retain path-conditional obligations through their merge and
  clean up at the enclosing full-expression boundary; return,
  break/continue, nested call temporaries, and reverse lexical cleanup have
  focused coverage. Semantics recursively rejects aliases, arrays, bases,
  fields, captures, and concrete generic shapes requiring global/static active
  cleanup with `GTI-S2061`. Compiler tests and default-mode O0/O3 plus C++20
  compatibility CLI traces cover the supported normal-exit slice without
  treating native call argument order as authoritative.
- **Unlocks:** `M-EXEC-01`, allocators, payload enums, owned callables, and
  temporary ranges. It supplies the lifecycle prerequisite consumed by the
  completed bounded `M-OWN-03`; broader generalized scope-join graphs remain
  separately client-gated.

### M-OWN-03: Stored And Escaping Mutable Dependencies

- **State/role:** complete bounded-first systems-readiness proof;
  prerequisites `M-OWN-02` and `M-LIFE-01` are done.
- **Delivered scope:** Extends the existing local parent/child loan graph to
  one stored or escaping mutable dependency with an exact stable origin. It
  defines creation,
  parent suspension, movement, explicit copy/assignment rejection, call/return
  propagation, path joins, child-first cleanup, and reactivation in semantics,
  HIR, and MIR. Start with
  one origin and one carrier shape before considering nesting or multiple
  owners.
- **Non-goals:** raw or opaque origins, dynamic-index precision, multi-origin
  carriers, arbitrary lifetime parameters, mutex syntax, or treating every
  stored reference as safe merely because the backend can represent it.
- **Focused tests:** stored view/guard-shaped carrier, returned mutable carrier,
  move and reassignment, early return, conditional creation, parent access
  while live, exact cleanup on every exit, and forged parent/escape graphs.
- **Exit gate:** satisfied: every supported stored or escaping child has one
  authoritative origin and exactly one path-sensitive end; MIR rejects forged ancestry,
  premature parent access, double end, and escaping unsupported shapes.
- **Completion evidence:** declaration and ownership semantics admit one exact
  direct `mut T&` field, make its carrier move-only/non-assignable, propagate
  exact mutable argument/carrier return summaries, and suspend/reactivate its
  origin. HIR/MIR retain the parent/child graph across construction,
  conditional return, and by-value move relay. Verification admits only exact
  `Construct`/`Call` transfer operands and rejects forged ancestry and double
  retirement. Compiler ownership tests cover owner conflict, child reborrow,
  move, return, unsupported shape diagnostics, and MIR mutation; the dedicated
  runtime fixture passes O0/O3 under C++20/C++23, and example 41 demonstrates a
  source-defined exclusive view. Raw/opaque/dynamic-index, nested, captured,
  global, custom-destructor, reseatable, and multi-origin carriers remain
  rejected.
- **Unlocks:** consumed by `C-SYNC-01` and scoped-thread guards; richer
  one-origin mutable views may build on the completed contract.

### M-OWN-04: Named Expected-Payload Range Loans

- **State/role:** complete bounded systems-readiness integration for portable
  byte-input parsers and serializers; `M-OWN-02`, `M-LIFE-01`, and
  `L-RANGE-01` remain its foundation.
- **Named client:** iterate the owned string returned by
  `std::fread(...).value()` without moving it out of the named
  `expected<std::string, std::io_errc>` solely to satisfy the ownership model.
- **Scope:** Add one exact active-value projection for a successful `value()`
  extraction from an available named local or by-value `expected<T, E>` place.
  Admit that projection only as the source of an existing read-only range
  plan when `T` independently satisfies the range contract. Conservatively
  protect the complete expected root through the loop. Preserve the checked
  extraction, source projection, range/element loan ancestry, endpoints, and
  cleanup from semantics through HIR and verified MIR; keep the LSP a
  diagnostic adapter.
- **Non-goals:** recognizing `std::string` or `std::fread`; general local,
  stored, captured, or returned nested-owner references; mutable payload
  iteration; `error()` ranges; multiple owners; conditional-origin merges;
  lifetime inference in the parser, backend, or LSP.
- **Focused tests:** positive string and non-string nominal payload ranges;
  empty/nonempty traversal; nested reads; `continue`, `break`, `return`, and
  failure cleanup; owner move/replacement/mutation/drop conflicts; element
  escape; unsupported receiver shapes; exact semantic/HIR/MIR identities;
  forged projection/root/order/endpoint/drop verifier cases; O0/O3 and
  C++20/C++23 `<std/cstdio>` execution; LSP publication and clearing.
- **Exit gate:** `for (auto& element : result.value())` is accepted for the
  bounded named read-only case, every owner-changing operation remains rejected
  until all loop exits end the loans, and no accepted source relies on native
  expected/reference lifetime behavior to repair a missing frontend or MIR
  proof.
- **Landed evidence:** semantic, HIR, and MIR identity assertions plus verifier
  mutation tests; nested local/parameter traversal with `continue`, `break`,
  and `return`; wrong-state failure; owner-conflict and escape rejection;
  O0/O3 × C++20/C++23 native execution; `<std/cstdio>` chunk traversal; and
  versioned LSP diagnostic publication/clearing.
- **Design owner:** the exact contract and phase map live in
  [`place-and-ownership-state.md`](place-and-ownership-state.md#9-proposed-m-own-04-named-expected-payload-range-slice).

### M-FAIL-01: Failure Operations And Cleanup Edges

- **In-place storage construction:** the everyday stabilization candidate now
  carries the selected zero-argument or staged scalar/owning/reference/text/raw
  prefix-append constructor's failure
  channel and effects. Capacity preflight precedes construction at the final
  slot; only success extends the initialized prefix. Failure preserves the
  constructor's original record and rolls back completed fields, earlier
  elements and caller owners. The retained client is an unrelated owner with
  optional string storage and a failing default constructor, used by both
  sized vector construction and resize. Combined validation is still in
  progress. Mixed inputs retain exact selected parameter roles and capacity
  preflight before ownership transfer. Insert and sparse storage construction
  retain their separate general propagation gaps; prefix-append support does
  not claim those families are complete.

- **Toy-engine field initialization (Gitea #2):** concrete source constructors
  for owners without a generated-default route now select declaration defaults
  into their own initialization schedule. This supports owning class-value calls
  including `std::make_unique<T>()`, caller propagation, reverse sibling/parameter
  cleanup, and explicit override suppression without duplicate execution.
  **Remaining:** generated/defaulted constructors still have no concrete MIR
  constructor body and retain their declaration schedule; fallible class-value
  calls on that route still report `GTI-B0003`. Materialize that route's exact
  constructor/initializer failure channel before claiming universal support.
  A terminal wrapper is not completion. This remains M-FAIL-01/M-BACK-02 work,
  not a change to source-language validity.

- **Shared failure cleanup (Gitea #222, 2026-10-11):** failure cleanup is
  lowered separately for every failure edge, so a body's size grows
  quadratically with its live owners. Each failure-capable operation gets its
  own reverse drop chain over every owner live at that point. Every block of
  that chain is bound to the edge's failure record (`MirBlock::activeFailure`).
  Each failure-capable drop in the chain adds a secondary record and a
  `TerminateCleanupFailure` block. Measured on main `f3f85336` at `-O0`:
  - A function whose N owning locals are each built by a failure-capable
    constructor lowers to 1,261 blocks for N = 20 and 4,921 for N = 40.
  - With an early return after each local, it lowers to 4,861 and 29,721
    blocks.
  - `tests/fixtures/stdlib_surface_json.gti`'s 240-line `main` has 146 calls
    and about 25 returns. It lowers to 31,299 drops in 61,225 blocks, and emits
    30 MB of the 33.5 MB generated C++ unit. That unit's native `-O3` build
    takes 117 s on a lightly loaded M2 Max and has passed 300 s under
    concurrent suite load. The `json` surface case therefore records a
    600-second build limit instead of the default 300.

  **Direction:** edges whose remaining obligations and cleanup phase are the
  same should share one cleanup tail, like a drop tree. That makes a body's
  cleanup linear in its owners. Prerequisite: a shared block cannot be bound to
  one static `activeFailure`, so the record it propagates must travel with the
  edge, for example as a block parameter. The verifier, failure effects,
  optimizer replay and the C++ emitter's cleanup admission must then accept a
  cleanup block reached from several records. They must still preserve exact
  record identity, reverse cleanup order, loan ending, and termination when
  cleanup itself fails.

  **Acceptance:**
  - The synthetic N-owner body grows linearly in N.
  - The JSON unit shrinks proportionally, and the `json` case returns to the
    default build limit.
  - Runtime output and failure attribution are unchanged.
  - Verifier mutations reject a shared tail with a mismatched obligation
    suffix, phase or record.

- **State/role:** active for broader failure semantics; `D-FAIL-01`,
  `I-CAP-01`, `M-LIFE-01`, and the source-body backend cutover are done. Exact
  origin/site metadata, propagation and cleanup edges, hosted containment, and
  the versioned runtime record/report/firewall substrate are implemented for
  all currently admitted source forms. Double failure and newly introduced
  operation/result families still require explicit MIR contracts.
- **Scope:** Represent exact local categories/details and canonical frontend
  source anchors in HIR, then assign deterministic artifact-local site IDs for
  MIR through the failure-metadata builder. Represent call-like propagation
  separately so it preserves an existing record without a transitive origin
  set or caller re-siting. Add explicit failure
  successors across direct, virtual, constructor, and expected-observer calls;
  represent cleanup of supported initialized and partially initialized
  invocation-owned shapes and termination at the hosted-program boundary; and
  add reusable fixed-record, observer, reporting, arbitration, and firewall
  primitives for later backend/task/callback/embedding rows. Assemble the
  immutable artifact descriptor before backend entry from frontend source facts
  plus the pre-optimization site table. Extend the frontend/driver/backend
  handoff with SourceGraph, exact source bytes, and direct/project logical-root
  facts rather than reconstructing paths in a backend. Add identity-bound
  trusted public-container bounds origins so ordinary vector/string wrappers
  select their public details without backend call-stack or spelling inference.
  Materialize the owned-entry adapter as a compiler-generated hosted-startup
  HIR/MIR operation with the fixed negative-count and checked-count-conversion
  origins plus the source `main` anchor. Preserve the exact vector/string
  constructor and append calls with their callee-owned allocation records;
  there is no adapter-local owned-argument-allocation origin.
- **Landed bounded slice:** `DefinedFailureOperation` records exact local
  outcome sets at snapshot-local source-unit/line/offset origins and records
  direct, virtual, constructor, and callable propagation separately. Semantics
  classifies the current arithmetic, conversion, bounds, owner, expected,
  storage, allocation, call, construction, and resolved-operator families; HIR
  and MIR retain the identity without absolute paths. MIR verification rejects
  forged vocabulary, origins, duplicates, placement, target, or propagation,
  and optimizer effects preserve every such operation as an observable trap.
  The post-HIR metadata builder now derives logical names, deterministic
  external-unit route identities, definition-site coalescing, one-based
  `FailureSiteId` assignments, canonical descriptor bytes, and the SHA-256
  artifact identity before optimization. MIR retains and verifies exact local
  detector sites while propagation remains un-sited. MIR v17 additionally
  gives eligible full-expression-root scalar operations in function and lambda
  bodies one `Invoke` with distinct normal/failure successors, a body-local
  fixed-record failure parameter, active-loan ending, reverse-construction
  temporary/lexical cleanup, and an exact `PropagateFailure` endpoint. The
  verifier rejects normal predecessors, missing/forged invokes, record rewrite,
  failure-result use, misplaced cleanup, and cleanup-order drift. MIR v18 adds
  caller-owned class-value parameter stages to ordinary calls and initializes
  one eligible cleanup-owning call result only on the invoke success edge; the
  failure edge neither owns nor drops that unconstructed result. An exact local
  scalar `Compute` or `Load` used as a later indexed ordinary-call argument now
  receives an `Invoke` after any earlier class-value stages: failure destroys
  each stage once in reverse construction order before propagation, while only
  the final normal-path call transfers them. The verifier derives eligibility
  from exact value/input identity, dominance, and prepared obligations and
  rejects detached arguments or omitted stage cleanup. An ownership-free static
  direct call used as that later argument now receives the same bounded edge:
  it remains un-sited, forwards its existing record unchanged, unwinds the
  earlier outer stages, and feeds its result into exactly one normal-path
  `CallInput`. A nested call with its own owning parameter remains excluded.
  The co-delivered runtime substrate now fixes the C ABI for ordinary records,
  per-site allowed outcomes, immutable artifact descriptors, and the emergency
  envelope. It provides allocation-free Unicode-15.1 report formatting,
  partial/`EINTR`/broken-pipe-safe report I/O, observer firewalling over the
  original record, one process-terminal winner, and immediate status 70. The
  all-zero
  artifact/site runtime sentinel requires no descriptor; every nonzero record
  requires exact artifact, site, and outcome membership.
  General compound argument evaluation, assignment destinations, borrowed and
  owning results, constructors, partial rollback, hosted origins, generated
  containment, descriptor emission, and executable backend use have since
  landed through the general MIR route. Double failure and any operation shape
  not represented by current MIR remain open.
- **Exit gate:** every current failure family has exact semantic/HIR
  local-origin and source-anchor snapshots plus MIR outcome/site snapshots;
  artifact descriptor/digest tests are stable across optimization levels;
  verifier mutations reject forged outcomes, sites, cleanup, and boundaries;
  allocation-free C/runtime harnesses exercise record handoff and arbitration.
  Its co-delivered Q-FAIL-01 slice owns exact
  hosted report/status/observer and report-I/O tests. End-to-end source
  execution and C++20/C++23 parity are covered by the completed `M-BACK-02`
  route and its focused runtime matrices.

### M-EXEC-01: Ordered Expression And Call Lowering

- **State/role:** in progress; `M-LIFE-01` and `D-EXEC-01` are done. Concrete
  non-intrinsic ordinary calls, concrete ordinary constructors, and concretely
  resolved class `operator()` calls with scalar/reference parameters and
  eligible non-borrowed class-value parameters now retain exact HIR input roles
  and verified MIR receiver/argument/invocation order. Direct self-contained
  class-value construction/call results now also have verified read/mutable
  temporary receiver materialization and full-expression cleanup; the
  remaining families are systems-readiness implementation.
- **Scope:** Decompose one complete expression family into ordered MIR values
  and temporaries, including receivers, arguments, transient loans, and cleanup.
  Remaining work includes operators and one-time assignment places, compound
  expressions, and wider partial-construction/failure schedules. The semantic
  `ProgramInitializationPlan`, `HostedProgramEntryPlan`, closed later-storage
  effect proof, HIR plan copies, exact constant-substitution lowering, merged
  module MIR body, and production verification are landed. New slices extend
  those authorities rather than reconstructing source order or startup targets.
  Add structural verifier mutations for ordering, materialization,
  full-expression boundaries, and cleanup.
- **Landed bounded slices:** Eligible ordinary calls retain one HIR receiver
  and source-ordered arguments; eligible ordinary constructors retain no
  receiver, one exact constructor target, and at least one source-ordered
  argument. A concretely selected class `operator()` retains an exact read or
  mutable receiver borrow, reusable value receiver, or consuming `MoveValue`
  receiver for an explicit move or trailing-`&&` target, including once-callable
  fallback to an exact read/mutable overload. These use exact selected
  parameter types and value/class-copy/class-move/read-borrow/mutable-borrow
  roles. Direct ordinary constructor and by-value GTI call results of concrete
  self-contained class/struct type may additionally use exact read/mutable
  temporary-borrow receiver roles for non-virtual ordinary methods and reusable
  `operator()`, with one producer-matched mutable place and full-expression
  drop obligation. MIR emits
  one-use `CallInput` checkpoints and verifies call-site, role, index, type,
  dominance, and strict receiver-when-applicable, arguments, then `Call` or
  `Construct` order. For ordinary calls, each eligible class copy or move
  materializes one distinct caller-owned prepared-parameter obligation. Copy
  initializes the stage, move reparents its exact active source obligation when
  present, and the final call transfers every stage exactly once as the callee
  begins. Scheduled ordinary constructors use the same prepared-parameter
  handoff and retain their separate partial-field rollback obligations.
  Mutation tests cover wrong sites,
  duplicate/abandoned and bypassed inputs, type drift, reordering, forged
  receiver/copy/move modes, missing or misplaced transfer, and erased target
  identity. Generated/default zero-argument and copy/move special construction,
  packs, unresolved callables, and operators other than concrete `operator()`
  remain outside the original bounded slices; their later slices retain their
  own verification contracts.
- **Landed checked-borrow composition slice:** an earlier mutation may finish
  before a later overlapping lookup creates a borrow in ordinary calls or
  constructions. Generic/free helpers preserve that order and origin. Ordinary
  call and constructor inputs share one HIR classification, including exact
  copied/constructed borrowed carriers and fresh borrowed call-result transfer.
  MIR preserves the unique parameter handoff, retained owner loan, success-edge
  publication and full-expression endpoint. User-defined tree/table and View
  clients cover reallocation, mutation/lookup/invocation failures, source
  diagnostic spans, invalid reverse order/escape, and forged MIR input order.
  This does not shorten temporary loans, permit simultaneous mutable aliases,
  or create a second backend execution authority.
- **Landed generated special-member pack transfer:** one concrete pack element
  selected for exact generated copy/move construction lowers to the canonical
  element `Load(Copy)`/`Move`, not an ordinary constructor call. Generic
  construction helpers now preserve owning entry-to-result transfer and
  cleanup-free result lifetimes; copyable packs remain reusable. User-defined
  payloads cover return transport, later-argument and callee failure cleanup,
  while malformed transfer/copy and repeated move-only expansion remain invalid.
- **Landed program-authority slice:** Semantics preserves ordered configured
  prelude roots and lexical dependency-first/source declaration order in one
  exact data-only/executable-step plan. `GTI-S2068` rejects a later-storage
  access or incomplete transitive effect closure, while exact representable
  value substitutions are retained through HIR and lowered to verified MIR
  constants rather than early loads. HIR exact-compares the semantic/HIR
  analysis seal, initialization/hosted plans, module inventory, and
  provenance. The generated hosted startup executes the merged verified
  `Module/0` body inside the containment boundary; generic static-storage
  scheduling remains a separate open contract.
- **Implemented bounded follow-on (`M-TEMP-ARG-01`; GTI 0.297.0):** A
  direct fresh, self-contained class/struct result may initialize one exact
  read-only `T&` call parameter through a full-expression-owned ephemeral
  loan. Existing exact candidates requiring no ephemeral borrow form the first
  viability tier; the new borrowed candidates are considered only when that
  tier is empty, so an exact owning overload remains preferred without C++
  conversion ranking. Semantics proves freshness and non-escape, HIR retains a
  dedicated argument role, MIR schedules producer/place/`BorrowRead`/call/loan
  end/drop, and production consumes that verified schedule. Mutable borrows,
  local or stored references, explicit moves, borrowed-state or escaping
  results, compound producers, and native calls remain excluded. The detailed
  contract and failure/runtime matrix are in
  [`mutable-temporary-receivers.md`](archive/mutable-temporary-receivers.md#implemented-follow-on-one-call-read-only-temporary-arguments).
- **Non-goals:** broad AST emitter rewrite, production body emission, or an
  IIFE workaround. M-BACK owns production C++ consumption of this schedule.
- **Exit gate:** deterministic HIR/MIR snapshots and verifier mutations prove
  effectful source order, one target evaluation, invocation after parameter
  setup, and balanced full-expression obligations for the selected family.
  Runtime O0/O3/native-compiler traces and compatibility-path removal belong to
  the matching M-BACK closed-body migration. The conservative semantic
  restriction is removed only when that production family is authoritative.
- **Unlocks:** each completed family may enter M-BACK immediately; completion
  of the whole row is not a global prerequisite. Production migration then
  unlocks more precise borrow acceptance, hosted threads, and optimizer control
  of those expressions.

### M-BACK-01: First MIR-Emitted Body Family

- **State/role:** done in 0.143.0. `scalar-leaf-v1` is the deliberately
  failure-free fixed-width-integer leaf-function family.
- **Prerequisite:** the selected family's control flow, values, places,
  lifecycles, and terminators are complete and verified in MIR. Completion of
  the entire `M-EXEC-01` row is not required.
- **Conditional prerequisite:** if the selected family can take a checked
  failure edge, the applicable `M-FAIL-01` and containment slices must also be
  done. The selected first family deliberately avoids that dependency.
- **Implemented scope:** `CppBackend` re-verifies its exact optimized MIR
  snapshot and emits eligible non-entry, non-generic free functions with
  fixed-width-integer parameters/results (plus a no-op `void` result),
  parameter loads, source or verified identity-fold literals, trivial
  full-expression boundaries, and one return. Every ineligible body remains
  wholly on the explicit compatibility path.
- **Exit evidence:** `mir_backend_first_family` and
  `mir_backend_first_family_runtime` prove a single MIR executable authority,
  optimized-instruction control, invalid/stale/forged-snapshot rejection, and
  O0/O1/O3 execution at C++20/C++23 while near-miss bodies remain compatible.
- **Unlocks:** executable use of matching `O-MIR-01` transforms, `O-MIR-03`,
  and the immediate consolidated `M-BACK-02` migration campaign.

### M-BACK-02: Complete MIR Body-Family Migration

- **State/role:** complete for source executable bodies. Every identity in
  `tests/mir_census_baseline.json` uses the verified-MIR production route.
  The named families below are historical migration stages, not current
  selectors or fallback authorities.
- **Completed scalar-CFG scope:** `scalar-cfg-v1` admits fixed-width integers,
  `bool`, and `char` in parameters and results, plus `void` results and
  unprojected scalar binding and temporary places; its verified literal,
  identity, logical-not, bitwise,
  equality, and ordering computations; loads, initialization, plain
  assignment, trivial boundaries; and
  `Branch`/`Goto`/`Switch`/`Return`/`Unreachable` CFG including loop backedges.
  Calls, checked failure, loans, drops, cleanup, construction, projections,
  and non-scalar ownership remain excluded.
- **Scalar-CFG exit evidence:** `mir_backend_scalar_cfg` and
  `mir_backend_scalar_cfg_runtime` prove exact whole-family selection, direct
  emitted-CFG control by verified MIR, compatibility near misses, optimized
  literal control, and O0/O1/O3 execution at C++20/C++23.
- **Completed scalar-direct-call scope:** `scalar-direct-call-v1` admits exact
  ordered scalar `CallInput`/`Call` stages across one closed acyclic static
  source-function graph. Each graph node independently satisfies the production
  declaration and scalar-CFG policy, every target remains coherent across
  semantics/HIR/MIR, and each call is normalized to ordinary continuation only
  under MIR v20's proved-false defined-failure summary. The summary is broader
  than production eligibility: a concrete internal, constrained, or
  `constexpr` body may validly summarize false while remaining compatible. A
  conservative true summary on an independently eligible graph rejects
  production emission as noncanonical; header-, body-, dispatch-, and
  cycle-ineligible graphs remain compatible. Once selected, later graph,
  identity, provenance, or instruction drift also rejects emission.
- **Scalar-direct-call exit evidence:** `mir_backend_scalar_direct_call` and
  `mir_backend_scalar_direct_call_runtime` prove whole-graph selection,
  exact target and ordered-input coherence, summary/provenance fail-closed
  mutations, cross-namespace and heterogeneous scalar calls, void and zero-arg
  calls, branch/loop/nested calls, IdentityFold emission, HIR-`for` and other
  near misses, and O0/O1/O3 execution at C++20/C++23.
- **Completed class-default-cleanup scope:** `class-default-cleanup-v1` admits
  non-entry, non-generic, zero-parameter source free functions with a non-void
  scalar result and one straight-line root scope. Each local is an exact empty,
  base-free, field-free, non-polymorphic concrete class created by
  generated-default `{}` construction with no constructor target. The return
  global is loaded before reverse lexical cleanup. Its exact public source
  destructors contain bounded scalar-literal assignments to mutable top-level
  globals and are proved failure-free by MIR v21. Production C++ uses strict
  raw-storage lifetime slots with explicit construction and destruction.
  Declared constructors, fields or bases, nested scopes, branches, calls,
  loans, and failure-capable destructors remain compatible.
- **Class-default-cleanup exit evidence:**
  `mir_backend_class_default_cleanup` and
  `mir_backend_class_default_cleanup_runtime` prove exact function/destructor
  selection, construction/reparent/drop/cleanup ordering, strict lifetime-slot
  representation, conservative and forged summary handling, HIR/MIR drift
  rejection, return-before-cleanup and reverse-destruction behavior, and
  O0/O1/O3 execution at C++20/C++23.
- **Completed owned-lifecycle scope:** `owned-lifecycle-call-v1` admits one
  atomic acyclic graph of eligible failure-free non-entry free functions plus
  exact concrete, base-free, non-polymorphic passive-scalar-field classes. Each
  class has one exact scalar constructor and destructor and no custom copy/move
  policy. Production emission consumes verified constructor initializer,
  construction, move, call-input, call, transfer, lexical cleanup, and drop
  stages through explicit lifetime slots; exact failure-free reverse callers
  join the same graph. Its bounded statement grammar covers blocks, scalar and
  owned variables, expressions, `if`, and `return`; comma, logical/conditional
  sub-CFG, loops, and switches remain compatible. Checked lifecycle bodies and
  all other near misses also stay wholly compatible.
- **Owned-lifecycle exit evidence:** `mir_backend_owned_lifecycle` rejects
  summary, initializer, same-typed-argument, destructor source/CFG, graph,
  call, transfer, and drop drift. `mir_backend_owned_lifecycle_runtime` runs the
  close-bit/first-close program at O0/O1/O3 under C++20/C++23 and proves moved
  inner cleanup precedes outer cleanup without a native double-destruction
  escape.
- **Completed hosted-failure scope:** `scalar-failure-callgraph-v1` admits one
  unique no-argument `int32_t` hosted entry plus its exact closed acyclic graph
  of `int32_t` source free functions. It reuses only exact v21 lifecycle-proof
  classes, admits checked fixed-integer add/subtract/multiply/divide/remainder,
  both shifts, negate, and dynamic conversion, and emits a private
  bool/out-result/record ABI. Local detectors create the exact artifact/site
  record, calls forward the same record pointer, MIR failure edges run exact
  drops before propagation, and only MIR Return publishes the result. Whole-
  program selection rejects incoming edges or class-representation users from
  every HIR body kind, cycles, native/virtual/normal-ABI edges, checked
  lifecycle bodies, dynamic initialization, and HIR/MIR drift. The single
  hosted boundary reports once through the runtime and exits 70; its catch-all
  immediately exits 70 without forging a GTI record.
- **Hosted-failure exit evidence:**
  `mir_backend_scalar_failure_callgraph` proves exact ABI, record forwarding,
  Return-only publication, reverse cleanup, source sites, complete reverse
  closure, C-linkage/virtual/lambda/initializer/class-use exclusions,
  value-owning array/generic containment with nonowning raw/reference retention,
  and fail-closed mutations. `mir_backend_scalar_failure_callgraph_runtime` runs
  normal and every admitted signed/unsigned detector outcome at O0/O1/O3 under
  C++20/C++23, requiring the exact report and status 70.
- **Completed scope:** Every source function, constructor, destructor, lambda,
  class initializer, module initializer, hosted-startup body, and verified
  native boundary shell is preflighted and emitted from MIR. The public no-MIR
  emitter and executable AST statement route are gone. Program initialization
  executes through its verified `Module/0` schedule and planned hosted adapter.
- **Completed backend frontier:** Generated structural, callable, lifecycle,
  native-callback, and concrete-instance families have exhaustive lowered
  contracts. C++ declaration/source assembly consumes lowered declarations,
  symbols, instances, MIR, and generated items. The transitional backend tuple
  and frontend-backed snapshot/emitter routes are deleted.
- **Historical migration ledger:** The checkpoints below explain how the
  general route was reached. Counts and compatibility references describe the
  state at each named release and are not current architecture.
- **Measured cutover position (0.150.0).** Production emits exactly one body
  from verified MIR across the 57-example corpus, a single `scalar-cfg-v1`.
  The six completed families each select correctly on their dedicated fixtures
  (3-25 bodies) but effectively never match shipped code, because each carries
  a whole-program selection contract rather than a per-body one.
  `CppMirBodyEmitter` reports 1953 of 2429 corpus bodies emitter-ready, but it
  is a fail-closed analysis gate with no text-emission step and no production
  caller, so that figure is a precondition, not emission. The
  `mir_emission_readiness` assertions inside `cpp_mir_body_emitter` now track
  it, including that every example still reaches MIR, since a rejected source
  silently inflates every readiness figure. That gate catches a
  compilation-breaking regression, which is the mode that masked two unsound
  attempts in this campaign, but it does not by itself establish semantic
  correctness: relaxing the loan exclusion in
  `requiresMirFailureControlFlow` alone raises readiness to 2006 with zero
  rejections and zero incoherent bodies, while leaving the enclosing scope to
  end a loan the failed operation never produced. Readiness movement is
  therefore necessary evidence for a cutover step, never sufficient.
- **The declaration gate is the binding constraint, and it is structural.**
  Instrumenting the real `selectedMirScalarCfg` over the corpus shows the
  first-match rejection reasons are operator 1124, member 944, generic 892,
  compiler-private 798, `constexpr` 432, non-scalar parameter type 268, entry
  114, with no function reaching the body-shape gate at all. A previous
  checkpoint recorded the opposite conclusion from an approximate model of that
  gate; the approximation wrongly excluded `Lifecycle`, which the real gate
  already admits as a pure full-expression marker. Treat the instrumented
  numbers above as authoritative.
- **Member and generic admission is not a gate relaxation.** After the
  declaration gate, the selector requires exactly one MIR function instance per
  source declaration with empty type and value arguments, and throws otherwise.
  A member of a generic class has one instance per instantiation, so admitting
  members makes the backend fail with "missing the MIR instance for an eligible
  source function". Emission is currently keyed per source declaration; the
  next increment therefore requires per-instance body emission rather than a
  wider selector.
- **Emitter capability ranked by bodies unlocked.** Of the 899 bodies outside
  the scalar-CFG set: `Lifecycle` 899, `Call` 647, `CallInput` 420,
  failure edges and checked operations 354, drops 47, `Construct` 26, loans and
  borrows 16. `Lifecycle` is required by every one of them and is already
  classified `RepresentedByMir`, so the first emission increment should cover
  `Lifecycle` plus the existing scalar-CFG set, then `Call`/`CallInput`.
- **Wiring landed (0.156.0-0.157.0).** (1) `buildCppMirBodyEmissionMapRows`
  is the production representation-row builder, reusing the extracted
  `cpp_representation` naming authorities. (2) `CppMirBodyEmitter::emitBodyText`
  is the text-emission step; the scalar-leaf, scalar-cfg, and
  scalar-direct-call families delegate their production body text to it, with
  receiver-place handling derived from MIR rather than family flags.
  (3) `CppBackend::generate` builds and owns the rows beside the program
  plan and passes them into the emitter, which no longer derives a spelling
  authority of its own. (4) Scalar-cfg admission is analysis-driven
  (0.159.0): the general emitter's fail-closed readiness plus its
  `supportsBodyText` vocabulary probe decide per body, the HIR body-shape
  walk and its two MIR shape/coherence re-proofs are deleted, and a
  declined body stays on the compatibility path instead of becoming a
  near-miss internal error. Declaration-identity facts, concrete-instance
  resolution, and the HIR/MIR metadata seal remain on the selection path.
- **Scalar-leaf and scalar-direct-call families dissolved (0.160.0).**
  Analysis-driven per-body admission covers every body the two families
  selected, so their selectors, whole-graph verification walkers, and text
  routes are deleted and their bodies publish under the general scalar-cfg
  label. Per-body admission also emits the ordinary callers those
  whole-graph contracts had to reject: constexpr, static-member, and
  internal-linkage callees stay on the compatibility path while calls to
  them emit from verified MIR. The text step asserts literal-value
  fidelity, so a forged out-of-range literal remains an internal error
  rather than implementation-defined C++. The remaining HIR-shaped
  admission lives in the three atomic families (scalar-failure-callgraph,
  class-default-cleanup, owned-lifecycle).
- **Storage locals, allocation, and the narrowed cleanup gate
  (0.197.0).** Storage-typed locals join the lifetime-slot
  representation (the slot classification widens beyond class owners),
  with slot values reaching storage staging, moves, and
  construct-by-move through the slot's checked accessor; allocation
  spells through mir_prefix_allocate_v1 publishing into its
  storage-typed result value; a storage value assigns into its owner
  field by move with the copy echo suppressed; and the blanket
  failure-cleanup gate narrows to its one real hazard — a cleanup
  destructor that may itself raise stays behind the unrepresented
  double-failure envelope, while bodies whose cleanup cannot raise are
  admitted and governed by the vocabulary probe, the MIR verifier
  owning drop-schedule correctness. The do-while fixture body converts
  under the narrowed gate, its compat loop-text pin inverted to the
  verified-MIR marker.
- **ADR 019: the generic-owner transformed-member boundary
  (0.196.0).** A failure-capable member of a generic owner emits per
  concrete instance as an explicit member specialization pair: the
  primary class template declares the transformed sibling
  (definition-free — the fixpoint guarantees only emitted
  specializations are referenced), each eligible instance defines
  `template <> Owner<Args>::name__gti_mir_failure(...)` with the
  general failure body, and the boundary wrapper is the explicit
  specialization of the original member routing failure through the
  runtime terminator — the free-generic pattern (0.182.0) carried onto
  members without friend machinery, receiver respelling, or whole-class
  specializations. The admission fixpoint recognizes generic-owner
  member callees through the same per-instance eligibility. Proven
  end to end behaviorally: the generic vector owner's out-of-bounds
  erase now reports the defined contract (exit 70,
  `index_out_of_bounds in private_storage`, the stdlib site) instead
  of the legacy storage abort, pinned in the runtime suite. Corpus
  markers 1271 -> 1276 with byte-identical defined-program behavior.
- **P-STORAGE-01 slice 4 begins: the storage failure form emits
  (0.195.0).** The general emitter spells prefix-storage operations
  through the shipped mir_prefix_*_v1 checked helpers: a value loaded
  from a storage-typed place is a staging class that never
  materializes (the call spells the field or binding lvalue directly,
  relocation staging a second place), Move instructions stage by-value
  elements, the modeling receiver on storage calls is validated and
  never spelled, and each operation publishes into the standard
  per-detector failure status whose returned outcome the existing
  status-sourced record write already carries — multi-outcome sites
  need no new record form. The first storage bodies leave the
  compatibility route: the concrete string owner's storage mutations
  emit from verified MIR across the corpus with byte-identical
  behavior; the generic vector owner's members remain
  compatibility-bound by the transformed-member boundary, and the
  reserve-class bodies wait on allocation, storage locals, and the
  cleanup drain vocabulary.
- **Write-staged receiver calls (0.194.0).** The receiver-carrying
  call convention gains the mutable form: a receiver staged as a write
  borrow reaches exactly a mutable member, a read borrow exactly a
  read-only member, with the same staged-place spelling and qualified
  member name in emission. This is the storage vocabulary's
  prerequisite — every container mutation begins with a write-staged
  receiver call — and the Chooser fixture pins the convention
  behaviorally through a mutable forwarding member.
- **P-STORAGE-01 wrapper migration: vector and string on prefix
  storage (0.193.0).** Both containers' internals moved wholly onto the
  prefix capability — fields, the iterators' storage borrows (the
  default-library borrow-carrier rule admits the prefix kind), and
  every operation: construction loops and push/emplace append at the
  live length, pop/clear/resize-down pop the last element, reserve and
  shrink relocate the complete prefix, and insert/erase use two new
  sealed invariant-preserving primitives (`prefix_storage_insert`,
  `prefix_storage_erase`) whose element shift and construction or
  destruction happen inside one runtime operation — strictly stronger
  than the sparse shift-then-construct pair, which exposed a
  partially initialized gap between two calls. The native evidence
  battery grew matching editing scenarios and guards across the same
  standard/optimization/sanitizer matrix. The example corpus stayed
  byte-identical in stdout and exit codes, the checked-vector
  benchmark workload passes on the migrated containers, and the
  emission-gate census keeps every body coherent. Sparse `storage<T>`
  remains the arbitrary-slot capability with both families rejecting
  each other's storage.
- **P-STORAGE-01 slice 3 (capability evidence): the native battery
  (0.192.0).** A dedicated suite compiles trusted generated programs
  natively and proves the prefix contract end to end: the behavioral
  fixture passes at C++20/C++23 across native -O0/-O2/-O3 and runs
  clean under dedicated generated-program ASan/UBSan builds — append
  order, logical-prefix reads and mutation through the borrow, pop
  destroying exactly the last element, relocation transferring the
  complete prefix while the moved-from shells correctly skip user
  destructor bodies (the lifecycle flag cleared by move), move
  transfer, and reverse destruction at scope exit, all pinned through
  a destructor-order checksum — and all five runtime guards trip
  their exact diagnostics. The comparative benchmark gate and the
  construction-failure rollback interplay bind at wrapper migration.
- **P-STORAGE-01 slice 2: the prefix-initialized storage capability
  (0.191.0).** A distinct compiler-private capability for
  vector/string-shaped owners: `gti_internal::prefix_storage<T>` binds
  by trusted declaration identity to its own type kind and a
  seven-intrinsic family (allocate, append, pop, read, read-mut,
  length, relocate) whose contract is the prefix invariant —
  construction appends exactly at the live length, destruction removes
  exactly the last live element, relocation transfers the complete
  prefix into an empty destination, and reads check the logical prefix
  structurally, with no per-slot initialization bitmap in the backend
  representation. Semantics, HIR, MIR (schema `mir-v31`), the
  optimizer effects tables, and the compatibility backend all bind the
  capability; borrows present as place-category element values exactly
  like the sparse reads; sparse `storage<T>` remains for
  arbitrary-slot owners, and each family rejects the other's storage
  type. Trusted-prelude fixtures pin frontend acceptance, the mutual
  rejection, and the emitted backend representation; the public
  wrappers stay unmigrated until the slice-3 evidence battery passes.
- **P-STORAGE-01 slice 1: the identity-bound public logical-size check
  (0.190.0).** A new trusted prelude intrinsic —
  `gti_internal::index_bounds_check(index, size)`,
  `IntrinsicKind::StorageBoundsCheck`, a pure two-scalar comparison
  whose only effect is the defined index_out_of_bounds failure — guards
  every public `vector` and `string` accessor (`at`, `operator[]`,
  `front`, `back`) against the logical size before the storage read.
  The outcome detail is identity-bound: semantics names the enclosing
  trusted default-library container, so the sites report
  `index_out_of_bounds` in `vector`/`string`, and an empty container's
  public indexing reports the logical bound ("container index out of
  logical bounds") rather than leaking the private storage invariant
  (`GTI-R0010` uninitialized access) — the cli scenario that pinned the
  leak migrates to the defined contract. Sparse `storage<T>` remains;
  the prefix-initialized capability, its evidence battery, and the
  backend capability row are the remaining P-STORAGE-01 slices.
- **Initializer bodies join general MIR emission: field and static
  slices (0.189.0).** A concrete class whose verified
  field-initializer body is passive — one straight-line block ending in
  Exit whose only work is literal materialization, bare
  default-initialization, and lifecycle boundaries — hands its in-class
  initializer spelling to the general route: staged literals spell from
  the MIR schedule through the shared range-asserting literal writer,
  bare defaults spell no initializer text, and the class publishes a
  field-initializers-instance marker after its definition. The
  verified-empty static-field-initializers body publishes its own
  marker. Fail-closed declines keep every other shape with
  compatibility: checked detectors (the in-class negative-literal
  initializers), storage reads, generic owners (template text is
  instance-shared), C-ABI records and unions (token-equivalence with
  the bridge header), and unsupported literal representations — the
  raw-pointer literal initially threw instead of declining, caught by
  the suite. Corpus markers 1072 -> 1261 (65 field + 124 static
  initializer bodies) with byte-identical stdout and exit codes; the
  Module bodies and checked field initializers remain for the next
  slice of this row.
- **Constructor bodies join general MIR emission, success form
  (0.188.0).** A constructor projects onto the shared scalar body facts
  like a mutable-receiver member — its receiver is inherently mutable
  under construction — and spells its verified initializer schedule
  inside the constructor body with no C++ member-initializer list.
  Fail-closed selection keeps every shape the body form cannot
  preserve: generic declarations keep their compatibility template
  (native compile on the length-parameterized constructor caught the
  missing gate), bases, unions, polymorphic and C-ABI owners decline,
  failure-capable construction stays with the rollback machinery, the
  family routes keep precedence, and the owner's in-class field
  initializers must be observation-free (bare default-initialization
  and literal stores only) so running them under the body form is
  indistinguishable from initializer-list suppression. Corpus markers
  1048 -> 1072 with byte-identical stdout and exit codes across all 57
  examples.
- **Receiver-carrying calls: borrow-staged call inputs and qualified
  member-call spelling (0.187.0).** A receiver or by-reference argument
  stages as a read borrow of a spellable place; the call spells the
  staged place followed by the qualified member name in both the
  success and the transformed defined-failure conventions, pinned
  behaviorally by a member chain whose transformed receiver call
  propagates the leaf record (exit 70, the member's own line). The
  failure selector admits operator members and reference parameters for
  concrete owners; the admission fixpoint sharpens member-callee
  availability to the selector's actual decision, so a member of a
  generic owner — which never emits a transformed form — correctly
  drops its callers; and the failure-free effect proof admits reference
  parameters with scalar field loads through their dereference carriers
  (v0.177's lesson: flip the summary, do not teach call sites to
  compensate). A staged receiver reached only through an implicit owner
  dereference declines — caught fail-loud by the native sweep before
  the type-identity rule closed it. Corpus markers 1038 -> 1048 with
  byte-identical stdout and exit codes; READY-NOVOCAB function bodies
  72 -> 46, retiring the iterator-comparison pool except the
  string-owner operators the storage fixpoint correctly withholds.
- **Loan erasure slice 1b: reference parameters, dereference chains,
  operator members (0.186.0, ADR 018).** A reference parameter keeps its
  C++ reference at the signature boundary and binds a pointer carrier in
  the body (`const auto *__gti_mir_p_N = &__gti_mir_arg_K;`); a
  dereference-projected place spells through its base carrier with field
  chains (`(*__gti_mir_p_N).field`); and operator members of generic
  owners join the member-specialization path — the friend adapters
  dispatch through the member unchanged, so the exclusion was
  unnecessary. The signature boundary admits single-argument reference
  parameters, and the fixture's reference-reader sibling
  (`compatibility_reference`) converts, inverting its old compatibility
  pin. Corpus markers 1030 -> 1038 with byte-identical stdout; the
  member fixture pins the pointer-carrier binding and dereference chain
  end to end.
- **Loan erasure slice 1a implemented (0.185.0, ADR 018).** The general
  emitter carries the core loan machinery: one hoisted pointer local per
  MIR loan identity (`const` follows access mode), `Borrow` as address-of
  through a composable place expression (bindings, storage globals,
  receiver fields, receiver field elements, sibling-array elements, loan
  carriers), `EndBorrow` as a boundary comment, and a returned loan as
  `return *__gti_mir_loan_<id>;` under the reference-returning ABI
  signature. The Borrow capability row ships as `mir_loan_pointer_v1`.
  Local and Return loan kinds are admitted; call-result, stored, and
  parameter loans decline until their slices. The first production
  consumer is a loan-returning accessor emitting as a generic-owner
  member specialization, pinned corpus-wide in the byte-agreement
  census; remaining slice-1 shapes (reference parameters, dereference
  projections for the iterator operators) follow.
- **Loan erasure representation ratified (ADR 018).** Loans erase to
  typed pointers with deref-at-use inside lowered bodies and C++
  references at ABI boundaries — the same lowering a production compiler
  applies to references — with compile-proven endpoints emitting as
  boundary comments and the Borrow capability row naming the contract
  (`mir_loan_pointer_v1`). The measured pool splits 49 storage-free
  failure-free bodies (slice 1), 31 storage-free checked bodies
  (slice 2), and 100 storage-bound bodies sequenced behind
  `P-STORAGE-01`. Success-edge loans cross the transformed failure ABI
  as pointer out-parameters when a measured pool demands it.
- **Member specializations for generic owners (0.184.0).** The
  specialization machinery extends to member functions of generic
  classes: each admitted concrete instance publishes as the explicit
  member specialization (`template <> RET Owner<Args>::name(...)`)
  holding the general body text, declared immediately after the class
  definition so no use can instantiate the member first, with the
  deferred template definition remaining for unadmitted instantiations.
  Owners whose representation collapses to a native alias decline (their
  spelling is no template-id), and a leading global qualifier is dropped
  after the return type to avoid maximal-munch misparsing — both caught
  fail-loud by the native sweep during development. The historical
  single-generic-instantiation hazard (substituted HIR bodies regressing
  examples 07/17 under declaration-keyed emission) does not apply to the
  per-instance form, and its old boundary pin inverts. Corpus markers
  1020 -> 1029; success-form only, since a transformed sibling cannot be
  specialized into a template.
- **Expected-returning checked intrinsics in the general vocabulary
  (0.183.0).** A checked-result arithmetic intrinsic produces its failure
  inside the Expected value with no edges, and now spells as the shipped
  `::gti_internal::backend::checked_*` helper with the error type as its
  template argument; the signature boundary admits an Expected of a
  scalar payload and an enum (or scalar) error as a passive
  value-semantic sum. The entire checked_add/sub/mul wrapper cohort
  converts through the specialization machinery: corpus markers
  948 -> 1020 with byte-identical stdout, and the byte-agreement census
  pin inverts from "no checked helper may appear" to "at least one
  must".
- **Per-instance template specializations from verified MIR (0.182.0).**
  A generic free-function declaration keeps its compatibility C++
  template, and every admitted MIR instance with concretely spellable
  substituted types additionally publishes per instance: a success-form
  instance as the explicit `template <>` specialization holding the
  general body text, a failure-form instance as its transformed
  suffix-named overload (parameter types disambiguate instances of one
  declaration) with the boundary wrapper as the specialization; both are
  forward-declared beside the primary template. Pack instances and
  parameter-typed substitutions stay on the primary template — the first
  cut leaked unsubstituted pack types into specializations and every
  variadic example failed native compile fail-loud before any test pin.
  The integer print chain now runs from verified MIR end to end
  (`println(42)` emits three specializations, the transformed
  array-walking digit printer, and byte-identical output — pinned in a
  new runtime scenario). Corpus markers 942 -> 948: the corpus prints
  strings far more than integers, so the chain's corpus footprint is
  honest but small; the machinery converts template-generic instances
  wholesale as later vocabulary lands.
- **Fixed arrays, the void failure ABI, and detector constants
  (0.181.0).** The general vocabulary gains fixed-array places
  (`std::array` rows), empty `Aggregate` value initialization, one-Index
  element places spelled as subscription on their sibling array,
  bounds-checked element loads and stores through the new shipped
  `mir_checked_array_read/write_v1` helpers (exact
  `INDEX_OUT_OF_BOUNDS`/`FIXED_ARRAY` record data), proven-safe dynamic
  indexes as plain subscription, unchecked `Convert` as `static_cast`,
  `numeric_cast` conversion intrinsics, constant operands on checked
  detectors, and the void transformed ABI (no out-parameter; the wrapper
  returns through the boundary). The Bounds and Aggregate capability rows
  now name genuinely shipped surfaces, the analysis retires its blanket
  dynamic-index rejection in favor of per-instruction vocabulary, and the
  effect proof admits string-view literals (the digit-printing chain's
  last conservative link). Corpus markers 880 -> 942 with byte-identical
  stdout; a dynamic out-of-bounds read reaches the defined contract at
  exit 70 (pinned end to end); the integer print chain is fully
  probe-admitted and awaits per-instance specialization emission for
  template-emitted generic prelude bodies — the next campaign.
- **Transformed-callee calls in the failure form (0.180.0, ADR 017
  slice 2).** A failure-form body may call a failure-capable GTI callee
  through the callee's own transformed body: the call assigns a
  per-call success bool, forwards the caller's record pointer unchanged,
  publishes into the scalar result (or a typed discard), and the paired
  `Invoke` branches on the success bool with no record write of its own.
  Admission becomes a greatest fixpoint: locally supported bodies drop
  out until every may-raise callee's transformed body is inside the set,
  so mutual recursion survives and hosted-family callees (a different
  spelling of the same convention) decline this slice. The first-family
  fixture proves the protocol end to end — a caller chain whose leaf
  overflow surfaces the leaf's own site through the caller's edge and
  the boundary wrapper at exit 70 — and its behavioral check caught a
  real miscompile during development (a stale result-assignment prefix
  clobbering the out-parameter after the call returned). Corpus markers
  hold at 880: the corpus's remaining failure-capable chains end in
  fixed-array or ownership vocabulary, so this slice's corpus yield
  arrives with those; the machinery is consumed and pinned by the
  fixture chain today.
- **Member bodies join the defined-failure boundary (0.179.0).** The
  per-body failure route admits ordinary non-operator members of one
  concrete non-generic class instantiation, mirroring the success route's
  member boundary: the transformed member and its same-signature wrapper
  are declared side by side in the class and defined through the deferred
  qualified form, with receiver constness following the declared
  mutability and the seal requiring exact receiver-mutability agreement.
  The prelude's `file_handle::release` cohort converts in every program
  (corpus markers 820 -> 880, failure-form bodies 4 -> 64, stdout
  byte-identical). The hosted suite's single-termination pin re-scopes to
  the hosted main, since each boundary wrapper now legitimately carries
  its own termination call.
- **String-view literals in the general vocabulary (0.178.0).** The probe
  and text step admit `std::string` literals of `StringView` type, spelled
  through the new single authority `cppMirStringViewLiteralSpelling`
  (`std::string_view{"…", N}` with escaped data and explicit length); the
  compatibility emitter's two literal sites delegate to it and its local
  `quote` helper is gone. This unlocked `println`, `print_decimal_digit`,
  and their instantiations across every program: corpus markers
  704 -> 820, stdout byte-identical, suite and oracle green (fixture
  prelude lists gain both names; first-family count 19 -> 21).
- **Failure-free effect proof widened to views, native calls, and
  intrinsics (0.177.0).** `deriveMirFunctionDefinedFailureEffects` admits
  passive string views throughout its function component, accepts
  C-linkage/runtime-binding call targets by their `None`-propagation
  language contract, and accepts wrapping/saturating intrinsic calls.
  Prelude I/O bodies whose may-raise summaries were purely conservative
  (`write_stdout`, `print`, and their chains) now prove failure-free at
  the MIR layer, their callers re-lower without Invoke edges, and the
  bodies convert through the existing success route — the phase-correct
  resolution of the transformed-callee question for conservative chains,
  produced at the layer that owns effect facts. Corpus markers 647 -> 704
  with byte-identical stdout; suite and oracle green; fixture prelude
  lists gain `print` (first-family count 18 -> 19).
- **Per-body defined-failure boundary, first slice (0.176.0, ADR 017).**
  The general emitter gains the failure form of its vocabulary: checked
  detectors spell as `mir_checked_*_v1` status helpers through one shared
  spelling authority, `Invoke` branches on the status and writes the exact
  MIR-owned record (site from the instruction, artifact identity from the
  program's failure metadata) on the failure edge, `PropagateFailure`
  returns false after its cleanup block, and `Return` publishes through
  the transformed out-parameter. A leaf failure-capable free function
  emits the transformed private body plus a same-signature boundary
  wrapper that routes failure into `gti_rt_failure_terminate_v1` — the
  structured report and exit 70 of the defined contract — so callers stay
  unchanged and no call-graph closure is required. Calls inside the
  failure form are admitted only toward proved-failure-free targets;
  failure-capable callees await the transformed-callee slice, and member
  receivers await the member site. Migrated behavior: leaf checked
  arithmetic now reports through the defined contract instead of the
  legacy abort (cli scenarios moved to the structured table; the
  first-family fixture pins the transformed body, wrapper, and a
  failure-path run at exit 70). Corpus markers 643 -> 647 with stdout
  byte-identical; the larger yield arrives with the follow-up slices.
- **String-view signatures and the C marshalling boundary (0.175.0).** The
  scalar-CFG signature gate admits `SemanticType::StringView` parameters
  and returns — a passive value-semantic view spelled `std::string_view`
  by its representation row — and the general vocabulary marshals view
  arguments to C-linkage targets through the shipped `to_c_string_view`
  converter, byte-identical to compatibility call sites; a view result
  from a C-linkage call stays declined (no reverse converter is
  modelled). The gap census that chose this slice also recorded the
  admitted-vs-marked truth: 355 of the previously admitted bodies are
  extern C-linkage declaration skeletons (phantom admissions with no GTI
  body), and the failure-record ABI pool now stands at ~480 Ready bodies
  awaiting its design release. Corpus markers 529 -> 643; program stdout
  byte-identical across all 57 examples; suite and oracle green. The
  first widening attempt shipped no marshalling and every example failed
  native compile — the fail-loud native boundary caught it before any pin.
- **Arithmetic-intrinsic vocabulary and constexpr admission (0.174.0).**
  The general emitter spells the six wrapping/saturating integer-arithmetic
  intrinsics as their shipped `::gti_internal::backend` helpers through one
  spelling authority shared with the compatibility call-site emission
  (`cppIntegerArithmeticIntrinsicSpelling`); the three checked kinds stay
  declined per body because their `Expected` result is outside the scalar
  vocabulary (pinned corpus-wide). The scalar-CFG declaration gate also
  stops rejecting `constexpr` declarations — GTI constant contexts are
  frontend-evaluated and the emitted monomorphized definition was never
  C++-`constexpr` on either route; the coherence seal keeps exact
  HIR/MIR/semantic constexpr agreement. Together these convert the
  admission census into production markers: corpus markers 385 -> 529 and
  general admission 881 -> 1,025 of 2,429 bodies, native sweep and
  differential oracle green. The `constexpr_target` fixture pins migrate
  from the compatibility list to the selected list.
- **NativeInterop and Intrinsic capability rows shipped (0.173.0).** The
  backend boundary now names the two shipped helper surfaces the analysis
  was withholding: `gti_rt_c_symbols_v1` (the runtime's C prototypes,
  declared by every emitted artifact and linked from gti_runtime) and
  `gti_internal_backend_helpers_v1` (the intrinsic lowering family). The
  honesty survey found exactly four native symbols emitted inside MIR
  bodies across the corpus (`gti_rt_write_stdout_byte`,
  `gti_rt_read_stdin_byte`, `gti_rt_read_file_byte`, `gti_rt_close_file`),
  each spelled through its body-name row byte-identically to compatibility
  emission; no intrinsic body emits because the text vocabulary declines
  what it cannot spell. Yield measured by the body census: general
  admission 186 -> 881 bodies and production corpus markers 157 -> 385,
  with the differential oracle and full native sweep green. The census
  also established the remaining large buckets: 362 Ready function bodies
  requiring the failure-record ABI, 266 requiring deeper capabilities, and the
  initializer/module kinds are mostly empty Exit-terminated skeletons
  (137 non-empty field-initializer bodies) whose authority migration is a
  representation change, not a body-text slice.
- **Class-default-cleanup destructor route dissolved (0.172.0).** The
  general emitter admits `MirBodyKind::Destructor` bodies: the probe and
  text step share one body-facts projection (a destructor has no
  parameters and an inherently mutable receiver), and the store path
  learned to spell Symbol-rooted destinations through storage rows — the
  probe had always admitted global stores, but the text step previously
  emitted an undeclared binding for them (caught fail-loud by the fixture's
  native compile). The lifecycle-cleanup site now consults the general
  route first, and the family's destructor selector, body emitter, and
  every remaining `class-default-cleanup` predicate are deleted (~360
  lines); the label no longer exists in the compiler. Per-body admission
  also emits the ordinary destructors the family had to reject
  (`ExplicitDefault`, `FieldOwner`, and two empty corpus destructors),
  while the checked-arithmetic near miss stays compatible.
- **Mutable-receiver field writes admitted (0.171.0).** The scalar-CFG
  declaration gate no longer rejects mutable receivers: a member storing to
  its own scalar fields through `this` emits from verified MIR, with the
  field place bound as a non-const reference exactly when the declared
  receiver is mutable. The store direction is fail-closed at the vocabulary
  probe — an `Initialize` or `Assign` destination rooted at `This` under a
  read-only receiver declines the body — and the failure-free function
  proof drops its read-only-receiver restriction, since a scalar store
  through the receiver cannot raise.
- **Class-default-cleanup function route dissolved (0.170.0).** The
  general emitter gains the lifetime-slot vocabulary — sealed slot
  declarations spelled from the LifetimeStorage capability row,
  argument-less generated-default `.construct()`/`.destroy()`, storage
  reads through symbol rows, reparenting Initializes, and cleanup-boundary
  markers — and the family's function selector, its HIR/MIR body walks,
  and its text route are deleted. Per-body admission also emits the
  ordinary lifetimes the whole-family contract had to reject (declared
  zero-argument constructors, field-initialized classes, scoped and
  conditional locals), with the argument-carrying Construct bound
  fail-closed because `.construct()` cannot spell constructor arguments.
  The destructor-definition route remains the class-lifecycle authority
  until general Destructor-body emission lands.
- **Failure-free effect proofs widened (0.168.0).** The function proof
  admits read-only receivers' scalar field loads (the projection carrier
  plus one trivially droppable projected field cannot raise), and the
  destructor proof grows from the single-block literal/assign shape to the
  scalar-CFG shape: receiver field reads, loads, the scalar operation set,
  and Goto/Branch/Return control flow over proven values, with writes still
  confined to storage places. The owned-lifecycle fixture's scalar user
  destructor now summarizes provably failure-free, so its categorical
  double-failure flag never fires; the native-closing prelude destructor
  legitimately keeps its containment flag, so the double-failure envelope
  remains the path for native-cleanup destructors specifically. The emission probe and text step reject
  receiver-carrying calls at both layers, closing the hole the widening
  would otherwise open (a member call spelled without its receiver).
- **Construction readiness measured and pinned (0.167.0).** The rows now
  carry every function instance's emitted name (runtime bindings and
  C-linkage declarations included), the executable module body's own row,
  executable field-initializer body rows, and the DefinedFailure sealed
  helper the artifacts already ship. Under production rows, every
  constructor and field-initializer body of the owned-lifecycle fixture is
  analysis-Ready — pinned in the body-emitter suite — leaving exactly the
  destructor double-failure envelope, the withheld capability families,
  and the Stage-E containment proofs between the atomic families and
  dissolution.
- **Default-field rollback now composes into source constructors.** Every
  unoverridden active-cleanup field completed by the verified per-instance
  initializer body enters the constructor as an initially active
  `ConstructionRollback` on its exact This-rooted place. Program verification
  matches owner, field binding and symbol, type, cleanup descriptor, uniqueness,
  and declaration order; explicit constructor initializers suppress the
  corresponding default entry state. Failure drains the inherited obligations
  with later stages, while normal completion transfers them to the object.
  This closes the concrete `std::filesystem::path(std::string_view)` backend
  gap without source-name recognition or backend-inferred cleanup. State-bearing
  bases and the destructor double-failure envelope remain separate work.
- **Field-initializer bodies joined failure control flow (0.166.0).**
  Owning declaration initializers arm ConstructionRollback obligations on
  their field binding places at the persistent-storage Initialize site,
  FieldInitializers bodies route defined-failure edges under the same
  body-wide unarmed-transfer suppression (with the unarmed definition
  sharpened to attached transfers without ownership continuation), failure
  edges drain through the merged global reverse-order sequence, and the
  emission analysis retires MissingConstructionScheduleMir exactly for
  fully armed bodies. Remaining before family dissolution: base
  subobjects, static-field initializer bodies, and the destructor
  double-failure envelope.
- **Owned-parameter class fields unified onto rollback (0.164.0).** The
  stage branch admits owned-parameter initializers, the moved value's
  obligation reparents into the armed rollback obligation (the verifier's
  owned-move contract accepts that consumption beside the legacy transfer),
  and constructor failure edges now drain temporaries, scope bindings, and
  rollback as one globally reverse construction-ordered sequence behind a
  single boundary — the phase-ordered drain could not satisfy the
  primary-chain rule once parameter bindings sat below armed rollback.
  Unique-owner fields joined the staged form in 0.165.0; remaining legacy
  transfers cover only shapes outside the single-owning-argument stage.
- **Unarmed constructor transfers fail closed (0.163.0).** A constructor
  with any silent subobject transfer into `this` (owned-parameter and other
  unstaged forms) routes no defined-failure edges: lowering prescans the
  initializer list before placing any edge, `mirBodyRoutesFailureEdges`
  holds the body-wide verifier rule, and the emission analysis retires its
  categorical rollback issue exactly when a constructor's verified MIR
  carries complete rollback coverage (no state-bearing bases, no unarmed
  transfers, every non-trivially-droppable field armed).
- **Partial-construction rollback represented (0.162.0, schema v30).**
  Ordinary constructors stage each class field completed from one owning
  temporary as an Initialize that reparents into a ConstructionRollback
  obligation on the This-rooted field place; constructor bodies are admitted
  to MIR failure control flow, failure edges drain the armed set in reverse
  stage order, normal completion retires it, and the verifier rejects a
  forged leaking edge (pinned with a mutation test). Remaining before the
  atomic families can dissolve: base subobjects, owned-parameter stage
  unification, initializer-body schedules, and the destructor double-failure
  envelope.
- **Rows for the next dissolution measured and landed (0.161.0).** The
  production rows now carry namespace-global storage spellings,
  constructor/destructor body names, and the sealed lifetime-slot helper,
  which makes every non-entry scalar function body of the
  class-default-cleanup fixture analysis-Ready (pinned by the body-emitter
  suite). Dissolving the three remaining families requires MIR
  authority, not representation: their constructor and destructor bodies
  still carry the partial-construction-rollback and double-failure
  structural gaps the atomic contracts currently license, so those
  families keep their selectors until that authority lands.
- **Admission widened per body (0.158.0-0.159.0).** The scalar-cfg gate no longer
  excludes compiler-private declarations, so standard-library scalar bodies
  select per body through the same fail-closed walk (corpus production
  emission 35 -> 150 bodies). A constexpr relaxation was measured to yield
  zero selected bodies and was reverted rather than kept as unexercised
  risk.
- **Exit gate:** all reachable GTI bodies have one executable authority; every
  current checked family preserves exact record, prior effects, and cleanup at
  O0/O1/O3 and C++20/C++23; native expected/assert/abort paths and duplicated
  emitter failure helpers are gone; closed-call-graph and verifier mutations
  prove that no failure channel crosses an unconverted caller.
- **Unlocks:** the backend-independent systems-readiness execution gate, generated embedding,
  native callbacks, and managed threads.

## Phase S: Systems Substrate — Layout, FFI, And Allocation

The external language review correctly identifies these as goals needed for a
complete systems-language claim. Systems readiness requires real records,
callbacks, opaque handles, and an application allocator, not every C ABI or
manual-lifetime feature. The foundations are sequenced so those useful slices
do not expose C++ object layout as GTI semantics.

### S-LAYOUT-01: GTI Target Data-Layout Contract

- **State/role:** done; systems-readiness substrate for C interop and allocation.
- **Prerequisites:** existing `TargetInfo` triple, pointer width, and
  endianness.
- **Scope:** Define the target-property vocabulary, triple interpretation,
  unknown-property and supported cross-target behavior, then introduce a
  GTI-owned immutable data-layout description for the scalar domains and
  representation categories GTI actually supports. Include size, ABI
  alignment, preferred alignment where meaningful, pointer width, endianness,
  and unsupported-target diagnostics. The native compiler and any private LLVM
  query may populate it, but neither representation crosses the interface.
- **Non-goals:** stable GTI class ABI, vtable layout, packed records, unions,
  bit-fields, or copying LLVM `DataLayout` into public headers.
- **Exit gate:** supported target triples produce deterministic facts checked
  against the native ABI in installed-toolchain tests; unsupported layouts fail
  before code generation.
- **Completion evidence:** `TargetDataLayout` is an LLVM-free immutable scalar
  fact value with byte size, ABI/preferred alignment, 64-bit pointer width, and
  little endianness. `llvm::Triple` remains a private normalizer mapped into
  exact `os`/`vendor`/`arch` vocabulary; detailed parsing distinguishes
  malformed, architecture, endianness, and OS failures. `GTI-S2062` rejects an
  explicitly unsupported selected layout before parsing/semantics/backend, and
  the installed compiler-library smoke compares every current scalar domain
  with the native ABI. Cross-compilation toolchain selection and aggregate
  layout remain explicit non-goals.
- **Unlocks:** `S-LAYOUT-02`, atomic capability checks, native records, and
  allocator alignment.

### S-LAYOUT-02: Bounded `sizeof` And `alignof`

- **State/role:** done; prerequisites `D-LANG-01` and `S-LAYOUT-01` are
  complete; bounded systems-readiness substrate.
- **Scope:** Add grammar, semantic, constexpr, HIR, MIR, formatter,
  Tree-sitter, LSP, and backend support for types whose layout is already a GTI
  fact: primitives, one-level raw pointers, transparent aliases, and recursive
  fixed arrays with positive concrete extents. The source form is the reserved,
  type-only `sizeof(type)` or `alignof(type)` operator; both have exact
  `uint64_t` type and `alignof` reports ABI alignment. Reject incomplete,
  direct symbolic type parameters, zero-sized or symbolic arrays, overflowing
  arrays, nominal values, and backend-dependent types rather than asking
  emitted C++. Pointer layout does not require its pointee to have a layout
  contract. `S-ABI-02` now consumes the same facts for the explicit passive
  native-record family.
- **Non-goals:** `alignas`, packing, a stable layout for every ordinary class,
  expression operands, direct query expressions in array-extent grammar, or
  using host `sizeof` as a semantic proof.
- **Exit gate:** values are frontend constants and match native probes across
  all supported targets; invalid categories get stable diagnostics.
- **Completion evidence:** `LayoutQuery` retains the type-only syntax;
  semantics resolves aliases and derives checked recursive array facts solely
  from `TargetDataLayout`, recording `GTI-S2063` on unsupported types, symbolic
  or zero extents, and overflow. HIR preserves a layout-query value plus its
  exact constant; MIR lowers it to a literal; the C++ backend emits that number
  and never a native query. Formatter, Tree-sitter, Neovim, LSP, dedicated
  pipeline tests, synthetic supported-target checks, native ABI probes, and an
  installed compiler-library consumer cover the boundary.
- **Unlocks:** allocation contracts, serialization, bounded C records.

### S-ABI-01: Native Record And ABI Proposal

- **State/role:** done; design-first systems-readiness contract recorded in
  [ADR 013](../decisions/013-bounded-native-c-records.md).
- **Scope:** Decide how a source declaration opts into C-compatible record
  layout, which fields/types are allowed, padding and alignment, by-value versus
  pointer passage, ownership prohibition, target dependence, and diagnostics.
  Keep ordinary GTI classes out of the C ABI.
- **Non-goals:** C++ ABI, arbitrary inheritance, bit-fields, packed access,
  unions, or automatic headers/importers.
- **Exit gate:** the proposal defines source opt-in, target dependence, layout,
  allowed signatures, ownership exclusions, diagnostics, and a C-oracle test
  matrix without claiming implementation.
- **Completion evidence:** ADR 013 selects `[[c_abi]] struct`, a closed passive
  field family, source-order checked layout, by-value and one-level-pointer
  passage, recursive unsafe classification, and a native C oracle without
  importing ordinary class ABI.
- **Unlocks:** `S-ABI-02`.

### S-ABI-02: Bounded Native Record Implementation

- **State/role:** done; bounded systems-readiness implementation.
- **Scope:** Implement the accepted closed record family through syntax,
  semantics, HIR, MIR, backend, headers/runtime, and tooling.
- **Exit gate:** a C-compiled layout/call oracle agrees with GTI on every
  supported target; unsupported fields/signatures fail before backend entry.
- **Completion evidence:** syntax, semantic layout, `GTI-S2064`, layout-query
  composition, extern-C validation, HIR/MIR retention and verification,
  backend layout assertions, formatter, Tree-sitter, LSP, installed-library
  smoke, and the O0/O3 x C++20/C++23 C oracle are integrated. Pointer-to-pointer
  out parameters, callbacks, and ownership transfer remain separate rows.
- **Unlocks:** `S-ABI-03`, C struct APIs, platform address records, and broader
  networking.

### S-ABI-03: Generated C/C++ Bridge Header

- **State/role:** done; bounded systems-readiness tooling and representation
  refinement.
- **Scope:** Emit one compiler-owned header from checked `[[c_abi]]` records
  and source `extern "C"` declarations. Its C17 branch uses deterministic C
  record names and strict prototypes; its C++20/C++23 branch preserves exact
  GTI namespaces and declares the same symbols with C linkage. Both branches
  retain semantic size/alignment/offset assertions. Make native records pure
  representation declarations by rejecting field initializers, canonicalize
  their emitted C++ field spellings, and keep GTI lifecycle policy out of the
  cross-translation-unit definition. Expose the artifact through
  `NativeHeaderBackend`, the installed compiler library, and direct
  `--emit-native-header` mode.
- **Non-goals:** importing or parsing foreign headers, general C++ ABI,
  exceptions across C, callbacks, pointer-to-pointer out parameters, ownership
  transfer, or automatic project include-path publication.
- **Exit gate:** one generated header compiles unchanged as C17 and
  C++20/C++23; C and C++ implementations link together with a GTI caller at
  O0/O3; a namespaced record preserves C++ identity; emitted GTI and header C++
  definitions agree; installed consumers can construct the backend.
- **Completion evidence:** `native_header_pipeline` covers deterministic
  header shape, namespace/name mapping, canonical definitions, strict empty C
  parameter lists, and layout assertions. `native_header_c_cpp_oracle`
  generates the artifact through the CLI, compiles separate C and C++ shims,
  uses a private C++ class behind the adapter, and executes all four
  O0/O3-by-C++20/C++23 combinations. `GTI-S2064` rejects representation-field
  initializers before backend entry.
- **Unlocks:** checked adapter authoring for existing C and C++ libraries and
  the independent pointer-only opaque-handle sub-slice. Callback and annotated
  ownership-transfer families remain gated.

### S-CALL-01: Function Items And C Callback Boundary

- **State/role:** done for the bounded same-thread C-library client.
- **Scope:** A namespace-scope `using` declaration names an exact nullable
  native function type. The type may appear in passive native records and C
  parameters/results. Contextual conversion selects exactly one non-generic,
  non-member namespace GTI function with a body and exact parameter/result
  types. Semantics owns selection, HIR owns concrete target identity, and MIR
  owns adapter identity, target failure effect, `TerminateInvocation` policy,
  and native-exception firewall. The backend emits one process-lifetime
  `extern "C" noexcept` thunk, so same-thread native retention cannot outlive
  code or captured state.
- **Initial boundary:** the complete recursively bounded C ABI family,
  including fixed-width scalar/pointer, `c_string`, passive native records,
  and nested named callback aliases.
- **Non-goals:** direct callback-value invocation, C varargs, arbitrary casts,
  lambdas/captures, closure-to-`void*` erasure, inferred userdata ownership, or
  foreign/native-thread GTI entry. The latter still requires runtime attachment
  and the adopted concurrency capability rows.
- **Completion evidence:** `native_callback_pipeline` covers syntax,
  contextual exactness, null state, record fields, callback returns, HIR/MIR
  adapter verification, failure containment, C/C++ header shape, formatter,
  and negative forms. `glfw_3_4_c_oracle` exercises retained same-thread
  callbacks and the complete GLFW callback surface. A failing callback exits
  through status 70 without returning across C; LSP and Tree-sitter gates cover
  the same syntax and diagnostic.

### S-FFI-02: Additional C ABI Families

- **State/role:** done for the selected bounded families and GLFW 3.4
  acceptance client. The independent pointer-only opaque-handle family landed
  in 0.119.0. The demonstrated C-library API this row's families are selected
  against is **GLFW 3.4**: a real, widely used
  C library whose surface is broad enough to exercise records, strings,
  out-parameters, and callbacks, and small enough to bind completely.
  Callbacks remain owned by `S-CALL-01` rather than blocking handle identity.
- **Implemented sub-slice:** `[[c_opaque]] struct Name;` creates one nominal,
  incomplete, nongeneric, baseless handle usable only behind a one-level raw
  pointer. `[[c_abi]]` records and `extern "C"` signatures admit those pointers;
  the generated C17 branch emits an incomplete typedef and the C++20/C++23
  branch an exact namespaced forward declaration. C or C++ may complete the
  private representation, while GTI infers no layout, ownership, nullability,
  transfer, or cleanup. `GTI-S2065`, formatter, Tree-sitter, LSP, compiler
  library tests, and the mixed C/C++ native oracle cover the boundary.
- **Layering contract:** The `extern "C"` surface is a faithful mirror of the
  C declaration and makes no safety claim; safety is a wrapper written in
  ordinary GTI over that surface, per ADR 004. A family is admitted when the C
  declaration can be spelled exactly and its obligations stated, not when the
  declaration is provably safe. Restricting what may be *declared* pushes the
  problem outside the language, where GTI can say nothing about it at all.
- **Completed families, ordered against the GLFW binding.** A binding attempt
  at 0.199.0 bound 3 of 5 records and roughly 90 functions; GLFW 3.4 has 124
  public `GLFWAPI` declarations. The measured prerequisite goals below are now complete.
  Each family states
  initialization, retention, aliasing, nullability, cleanup, and unsafe
  obligations. A selected ownership family must build on the opaque identity
  rather than retroactively making raw handle pointers owners.
  Diagnostic codes are retained beside the family that owns them.

  - **F1 — fixed-array `[[c_abi]]` fields (complete).** Admit `T[N]` fields of otherwise
    admissible element types. ADR 013 already anticipates this: it excluded
    them only until their backend representation is defined directly rather
    than inherited from `std::array`. C array layout is unambiguous, so no
    layout proof is lost, and element access keeps GTI's existing bounds
    checking. Cheapest family; do it first.
    - *Diagnostics.* `GTI-S2064` stops naming fixed arrays. Two replace it:
      `GTI-S2069` a fixed-array `[[c_abi]]` field has a zero or negative
      extent; `GTI-S2070` its element type is itself outside the C ABI field
      set. Both name the field and its written type, as `GTI-S2064` does.
    - *Obligations.* Initialization: the array is inline record storage and
      follows the record, adding no per-field step. Retention: none; the
      elements are stored, not referenced. Aliasing: element access aliases
      only within the enclosing record, introducing no relation the record did
      not already have. Nullability: not applicable; an array field is not a
      pointer. Cleanup: none, because the admitted element types are exactly
      the passive ones already required. Unsafe: none; element access is
      ordinary bounds-checked GTI indexing, so an out-of-range read is a
      defined `GTI-R0007` failure rather than C's silent overread. F1 is the
      cheapest family precisely because every obligation is already discharged
      by the surrounding record.
    - *Acceptance.* `GLFWgamepadstate` (`uint8_t buttons[15]`,
      `float axes[6]`), which unblocks `glfwGetGamepadState`. Its measured C
      layout is size 40, alignment 4, `buttons` at 0, `axes` at 16; that
      target is pinned in `tests/native_record_c_oracle_test.py`. The C oracle
      now proves layout plus C-to-GTI and GTI-to-C element access at O0/O3
      under C++20/C++23, while native-header tests prove direct C declarators
      and reject a `std::array` representation.
  - **F2 — the C-string boundary (bounded boundary and owner-production
    slices complete).** `char` stays excluded from `[[c_abi]]`
    records for the implementation-defined-signedness reason already recorded
    in `R-NATIVE-RECORDS`. The gap is that a C API's `const char*` means
    *pointer to NUL-terminated bytes*, and spelling it `const uint8_t*` loses
    that meaning and forces hand conversion at every call site. Define one
    boundary type carrying the NUL-terminated contract. The completed core
    maps `c_string` to `const char*`, converts complete static string views at
    exact call-input boundaries, supports nullable returns and `c_string*` out
    parameters, preserves exact-overload preference, and exposes no unchecked
    dereference or index operation. C17/C++20/C++23 oracle coverage exercises
    all of those forms.
    - *Diagnostics.* Owner-derived values reuse the existing retained-loan and
      invalidation diagnostics (`GTI-S2017`) and owner-dependent storage/escape
      diagnostic (`GTI-S2045`) rather than introducing a second vocabulary for
      the same rule. `try_c_string()` reports an embedded terminator as a null
      value instead of a compile-time diagnostic.
    - *Owner obligations.* Initialization from an owner is never
      inferred from its C++ representation and never from a bare pointer
      outside `unsafe`. Retention is a borrow, and its loan is tracked
      against the owner, so the dangling `c_str()` that C++ permits silently
      is rejected. Aliasing: read-only, with no mutation through the
      view. Nullability: a `c_string` received from C may be null and reading
      one remains outside this bounded read-free surface;
      `std::string::c_string()` never returns null, while
      `std::string::try_c_string()` uses null to reject an embedded terminator.
      Cleanup: none, as it is non-owning. Unsafe: required to build one from a
      raw pointer, not required to obtain one from a GTI string. A native
      callee's hidden retention cannot be inferred and remains a binding-level
      contract.
    - *Acceptance.* `glfwGetVersionString`, `glfwGetMonitorName`,
      `glfwGetWindowTitle`, `glfwSetWindowTitle`, `glfwCreateWindow`,
      `glfwWindowHintString`, `glfwGetKeyName`, `glfwGetJoystickName`,
      `glfwGetJoystickGUID`, `glfwUpdateGamepadMappings`, `glfwGetGamepadName`,
      `glfwSetClipboardString`, `glfwGetClipboardString`, and
      `glfwExtensionSupported`.
  - **F3 — out-parameter and returned pointer-plus-count (complete).** C's "returns an
    array, writes the length through an out parameter" idiom. Admit it as one
    bounded boundary form rather than admitting `T**` generally, which would
    import pointer nesting and aliasing that `R-RAW-POINTERS` excludes as a
    durable rule.
    - *Diagnostics.* `GTI-S2073` `[[c_array]]` names something that is not an
      integer out-parameter of the same declaration; `GTI-S2074` a two-level
      pointer appears outside an annotated `[[c_array]]` boundary, which keeps
      the general `R-RAW-POINTERS` rejection intact and explains the one
      exception.
    - *Obligations.* Initialization: GTI's ordinary definite-initialization
      rule still requires storage for the count before its address is passed;
      the native call may overwrite it, and the returned pointer exists only
      after the call completes. The attribute does not create a hidden pair
      value. Retention: the returned array is owned by the C library and its
      lifetime is that library's contract; GTI infers nothing and each binding
      states it per function.
      Aliasing: elements may alias anything, and GTI makes no claim.
      Nullability: the array pointer may be null when the count is zero, and
      the wrapper must handle that rather than index it. Cleanup: none by GTI;
      an API that requires freeing the result belongs to a separate
      ownership-transfer family, not this one. Unsafe: every element access
      requires `unsafe`, and the intended shape is one `unsafe` block in the
      wrapper that converts the pair into an ordinary GTI view.
    - *Acceptance.* `glfwGetError`, `glfwGetMonitors`, and
      `glfwGetRequiredInstanceExtensions`.
    - *Completion evidence.* `[[c_array(count)]]` resolves one exact writable
      fixed-width integer out parameter and admits either one C ABI pointer or
      the bounded two-level return whose inner pointer is independently
      admissible. Semantics, HIR, MIR, the MIR verifier, C++ emission, native
      header generation, formatter, Tree-sitter, LSP hover/definition/tokens,
      and C17/C++20/C++23 oracle coverage agree. Explicit nested-pointer locals,
      parameters, fields, and unannotated returns remain rejected.
  - Callback semantics remain owned by completed `S-CALL-01`; this row consumes
    that type family when exact callback aliases occur in records and C
    parameters/results.
  - The surface these families are built against is written out in
    [`c-interop-target-surface.md`](archive/c-interop-target-surface.md): the raw
    mirror, the safe wrapper, and the acceptance program.
- **Later breadth:** C varargs, unions, bit-fields, and packed records remain
  separate proposals. `printf` alone is not sufficient justification for
  importing C's least checkable call surface.
- **Exit gate for each family:** the selected family passes a real C oracle across supported
  targets, has explicit initialization/retention/cleanup diagnostics, and adds
  no backend-derived type or ownership authority.
- **Row exit gate:** complete. With F1 through F3 and `S-CALL-01`, the GLFW 3.4
  header binds all 124 functions, all five records, every callback typedef, and
  the Vulkan-facing declarations without omission. The generated header
  compiles as C17 and C++20/C++23, all 124 function names are exact-compared
  with the official header inventory, representative calls link and run at
  O0/O3, and callback failure is contained. The fixture is regression-covered
  under `tests/fixtures/`; a real display/window smoke belongs to a downstream
  GLFW package because headless CI should not define language correctness.

### S-ALLOC-01: Allocator, Provenance, And Initialization Proposal

- **State/role:** complete; adopted by
  [ADR 022](../decisions/022-safe-typed-allocation.md) after `M-LIFE-01`,
  `D-LANG-01`, `D-FAIL-01`, `S-LAYOUT-02`, and `M-OWN-02`.
- **Scope:** Define allocator ownership, byte/typed provenance, size and
  alignment, allocation failure, zero-sized requests, initialization state,
  placement construction, destruction, deallocation, and interaction with
  `unsafe`. Start from safe typed storage and arenas rather than exposing
  source-level `new`/`delete`.
- **Exit gate:** the proposal can express an arena and pool without duplicate
  destruction, uninitialized safe reads, hidden owner creation, or C++ layout
  assumptions.
- **Completion evidence:** ADR 022 defines a typed move-only allocation origin,
  owner-carried release authority, checked per-slot lifetime state, receiver-
  tied borrows, failure-atomic `expected` allocation, minimum alignment,
  overflow/exhaustion separation, successful zero capacity, reverse cleanup,
  and the homogeneous arena/pool composition boundary without public raw
  addresses or manual deallocation.

### S-ALLOC-02: Narrow Public Allocator Capability

- **State/role:** complete first public slice; `S-ALLOC-01` and `I-CAP-01` are
  done; systems-readiness implementation.
- **Scope:** Implement one GTI-owned allocator interface over narrow trusted
  operations, plus a recoverable factory returning `expected` and a typed
  partial-storage owner on which an arena, pool, or container can build policy.
- **Non-goals:** global replacement allocation, manual raw deletion, polymorphic
  allocator hierarchies, or changing `std::vector` in the first slice.
- **Exit gate:** ownership, partial initialization, failure, and cleanup are
  visible in HIR/MIR and pass sanitizer/runtime failure tests.
- **Completion evidence:** `<std/memory>` implements ordinary source-defined
  `std::allocator`, `std::storage<T>`, `std::alignment`, and
  `std::alloc_errc` over exact trusted allocation/state operations. HIR/MIR
  retain recoverable allocation, observer, construction, borrow, destruction,
  lifecycle, failure-origin, and effect facts. The public owner requires
  `std::movable T` without borrowed state. The executable fixture covers
  zero size, invalid/over-alignment, byte overflow, resource exhaustion,
  sparse construct-by-value, checked read/mutation/destruction, movement,
  reverse clear/drop, and structured failure across C++20/C++23 and O0/O3,
  including cleanup when construction of an input value fails. Public forging,
  copying, variadic emplacement, and mutation during a receiver-tied borrow are
  rejected.
- **Unlocks:** `S-ALLOC-03` container integration.

### S-ALLOC-03: Bounded Arena And Pool Clients

- **State/role:** complete first composition slice; prerequisites
  `S-ALLOC-02` and `L-RANGE-02` are complete; bounded-first systems-readiness
  evidence for source-owned allocation policy.
- **Scope:** Implement fixed-capacity homogeneous arena and pool policies over
  typed storage without C++ allocator-traits complexity. Define movement,
  allocation/insertion failure, reset/erase invalidation, and cleanup before
  generalizing to allocator-aware growing containers.
- **Non-goals:** stateful allocator propagation, public raw storage, growth,
  heterogeneous allocation, or generation-bearing pool handles.
- **Exit gate:** the selected clients prove allocator lifetime, move-only
  propagation, failure, cleanup, reuse, and receiver-borrow invalidation across
  C++20/C++23 and O0/O3 without recognizing a public allocator name in compiler
  code.
- **Completion evidence:** `<std/memory>` implements ordinary source-defined
  `std::arena<T>`, `std::pool<T>`, and recoverable `try_make_*` factories over
  `std::storage<T>`. Arena insertion is monotonic until reset; pool insertion
  reuses the lowest empty position. Both are move-only, support successful zero
  capacity, distinguish allocation from full-policy errors, retain checked
  receiver-tied access, and destroy live values in reverse slot order. Runtime
  evidence covers reset/erase/reuse, bounds and empty-slot failures, cleanup
  while producing an input value fails, and deterministic teardown. Compile
  evidence rejects copying, nonmovable elements, forged storage adoption, and
  mutation during a retained element borrow. The compiler recognizes only the
  pre-existing exact private storage operations.

### S-ALLOC-04: General Low-Level Memory Facilities

- **Follow-up boundary revision (implemented and locally validated):** raw
  provenance/access/effect auditing is
  optional (`off` by default, `warn`, or CI-oriented `strict`). Ordinary
  typing, unsafe permission, checked ownership/loans/escape, and HIR/MIR
  verification remain mandatory. Unsafe raw validity and witness association
  are programmer obligations, not acceptance predicates requiring complete
  interprocedural proof. Independent per-demand query budgets and root-only
  reuse repair declaration-order dependence inside the optional analyzer.
  The historical Phase 1 evidence below predates this revision. Current
  validation covers Off/Warn emitted-MIR/C++ parity, mandatory negative controls,
  ordering/recursion/cache-capacity and CLI/project/LSP policy tests. All 440
  registered production gates pass across the full run and documented reruns;
  see [combined verification](../architecture/verification.md#optional-raw-audit-and-combined-reconciliation)
  for the exact scope, installed-SDK checks and platform limitations.
  See the revision in ADR 022 and the current raw-pointer language contract.
- **State/role:** implemented and locally validated, full Phase 1 campaign.
  This extends ADR 022; the existing checked typed-storage interface remains
  supported.
- **Outcome:** unrelated application-defined owners, raw-backed views, and
  storage abstractions can expose safe, lifetime-checked interfaces.
- **Current progress:** sparse final-address construction and rollback are
  implemented. Raw origins survive copies, projected fields, aggregate
  transport, branch joins, loop backedges and object replacement. Ordinary
  unsummarized call transport conservatively retains possible input origins.
  The explicit owner-witness bridge reaches verified MIR and C++20/23, with
  source-wrapper parameter obligations and loop-origin checking. Receiver and
  aggregate raw-field obligations now propagate, and compact retained-reference
  witnesses resolve to their external owners. Callee-produced result origins
  now use bounded isolated queries of the ordinary semantic analyzer, preserving
  local expiry, formal/receiver/global origins, default arguments and constructor
  fields. Unsupported source-result families retain an unresolved marker rather
  than being mistaken for unknown foreign addresses. Safe address formation is
  implemented independently of unsafe operand evaluation. Integer-backed raw
  carriers and byte-preserving native-record transport implement inert expired
  addresses; focused warning, syntax and editor checks pass. Explicit allocation
  release now reuses ordinary movement and cleanup, including original
  destructor panic and reverse-slot
  cleanup. The existing typed allocation request supplies ordinary-object
  layout authority without new constexpr/ABI promises. Erased-address
  conversions preserve constness, known pointee types and lifetime facts;
  focused semantic/MIR/LSP and all eight runtime modes pass. Broad
  completion verification is recorded below; native ABI transport has focused
  six-mode and container eight-mode evidence as well.
- **Approved public-language amendment:** application authors must be able to
  define a raw-to-checked-reference helper without a privileged library call.
  Add an optional declaration-level, identity-resolved return-borrow origin
  selecting a parameter or receiver, retaining unambiguous inference. Safe
  returns must prove the declared origin; unsafe raw-reference creation must
  explicitly assert validity within lexical `unsafe`; type/access restrictions
  remain mandatory and contradictory provenance is an optional audit finding.
  Reuse the implemented bridge/loan facts and replace
  the convenience helpers' private-only implementation with ordinary GTI.
  This is not general lifetime generics or permission to relabel a local borrow.
  The `[[borrowed_from(owner)]]`/`[[borrowed_from(this)]]` syntax, explicit-summary
  precedence, direct lexical-unsafe return assertion and ordinary GTI helpers
  are now implemented. Focused syntax/semantic/MIR/runtime and editor checks
  cover this path. An intermediate audit exposed additional
  source-callee-produced raw result origins (local expiry/global identity) that
  the conservative input union did not retain. The source-query repair below
  preserves those facts through call-result provenance. Polymorphic
  raw-assertion requirements fail closed rather than being lost through a
  virtual prototype. The query implementation rejects the original probe
  (returning `&local` and asserting it against an unrelated live witness)
  and adds local/global/default/aggregate/constructor positive and negative
  regressions. Produced-origin and write summaries preserve declaration-order
  and concrete-instance independence without exporting body-local loans;
  pending summaries remain distinct from genuinely unknown native addresses.
  Lambda/deferred/dynamically dispatched, variadic,
  recursive, incomplete and budget-exhausted source queries conservatively
  block access to their unresolved raw outputs, while allowing inert transport.
  The raw-pointer, raw-borrow and conversion contracts pass on the rebuilt
  Release compiler. These bounded summaries do not claim full interprocedural
  inference.
- **Contract:** raw pointers remain non-owning. Unsafe code may assert unknown
  validity subject to programmer-owned raw obligations; optional auditing
  preserves known provenance while mandatory type/access and checked lifetime
  rules still apply. An explicit raw-to-reference assertion names an owner/lifetime
  witness; storing a pointer in a field is not such a witness. Returned shared
  or exclusive references use the existing place, loan, and escape model.
  Built-in `&place` is safe when its supported operand is safe to evaluate;
  it preserves access/provenance but creates no lasting loan. Nested raw
  dereference/index/member operations retain their independent unsafe checks.
  Copying an expired address is distinct from accessing its dead pointee;
  the native representation must substantiate that distinction.
- **Native-representation contract (implemented):** a native `T*`
  carrier alone does not establish the promised inert expired-address copy/equality
  semantics across arbitrary selected C++20/C++23 toolchains. WG21
  [P3347R5](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p3347r5.pdf)
  documents the implementation-defined invalid-pointer operations and the
  need to capture integer addresses before expiry. LLVM's
  [object-lifetime contract](https://llvm.org/docs/LangRef.html#object-lifetime)
  permits non-dereferencing IR operations, but is not a blanket guarantee for
  arbitrary C++ frontends. The accepted repair is a typed internal integer
  address carrier, captured while the native pointer is valid, with integer
  copy/equality and explicit pointer recovery at unsafe access/foreign calls.
  Preserve native pointer ABI for C-facing fields and signatures through
  explicit boundary conversion, and gate the supported integral-pointer ABI.
  Exact-toolchain runtime tests are evidence, not a substitute for this
  representation contract. The ordinary integer carrier and reference-based
  byte staging for contextual native output slots are implemented. Focused
  runtime evidence covers expired address transport through source calls,
  passive source records, arrays and erased conversions. C-compatible native
  record fields now use byte-preserving staging for internal copy, argument,
  result and container transport without changing external ABI. The container
  matrix passes all eight modes and the native transport matrix passes six
  modes with the integrated access summaries. Restoring transfers reuse
  semantic cleanup/relocation facts to distinguish inert transport from
  possible user execution. These focused passes complement the full completion
  evidence below rather than replacing it.
- **Virtual transport closure implemented:** a shared lowered virtual-root
  query selects a uniform optional output channel for class-bound symbolic
  results and every connected override. Native-record instances use indirect
  publication; scalar and owning instances retain their existing return/drop
  convention. The optimized `raw_native_record_transport_runtime` gate passes
  C++20/C++23 at O0/O2/O3 for unrelated generic interface/model relays,
  nongeneric overrides of generic bases, and exactly-once owning-result
  cleanup. This preserves a family accepted by the canonical `Release`
  version `0.374.0`, rather than disguising a regression as an unsupported
  language shape. The same six-mode gate also covers class-owned callback
  argument packs and two method-type variants sharing one retained owner;
  emitted-signature assertions guard against silent native by-value copies.
- **Implementation sequence:** (1) generalize the existing final-slot
  constructor/preflight protocol to sparse storage, preserving partial-member
  rollback and exact constructor failure records; (2) expose typed allocation,
  explicit construction/destruction/release and typed transfer, using a typed
  request as the ordinary-object layout authority; (3) implement explicit
  erased/typed address conversion and owner-witness checked-reference bridges;
  (4) preserve and optionally audit contradictory known raw provenance across these
  operations and existing pointer operations; (5) close runtime, verifier,
  optimizer, LSP, installed-toolchain and documentation/example evidence.
- **Required evidence:** an unrelated owner and raw-backed view demonstrate
  final-address construction, shared/exclusive checked borrowing, movement,
  destruction and constructor-failure cleanup. Reject lifetime escape,
  conflicting checked access and permission upgrades; optional auditing covers
  raw/checked conflicts. Allocation failure
  remains recoverable; constructor panic propagates its original identity and
  never becomes an allocation error. Prove exactly-once cleanup, including
  partial construction, optimized builds and C++20/C++23 parity. Independent
  MIR mutations must reject forged lifecycle, loan and failure metadata.
  Also prove unrelated source-defined safe multi-input origin selection and
  unsafe raw-reference helpers, malformed-contract diagnostics, ordinary
  wrapper/generic preservation, and compiler/LSP/syntax-tooling agreement.
- **Boundaries:** no stable foreign ABI promise, arbitrary integer/address
  conversion, implicit ownership inference, or new lifetime syntax. Private
  mechanisms retain declaration-bound identity; public policy remains source
  defined. No requirement to null moved-from pointers. Stateful allocator
  protocols, resource leases and allocator-aware containers belong to Phase 2,
  not this campaign. Completing only emplacement or checked storage does not
  satisfy Phase 1.
- **Completion evidence (2026-10-06):** the optimized macOS arm64 compiler
  passes all 433 registered gates across the full run (429 passes) and a
  four-test rerun after test-only representation/allowlist corrections. No
  production changes intervened. This includes optimizer/evidence, verifier,
  corpus, runtime/failure, library, native-boundary and LSP gates. The isolated
  install passes eleven external SDK consumers, CLI/project/LSP checks, local
  archive Neovim smoke, and examples 111–114 at C++20/O0 and C++23/O3.
  Tree-sitter corpus/query/shipped-source checks and all 422 first-party
  C/C++ formatting checks pass. This evidence does not claim Linux/Windows
  execution or bundled-LLVM release packaging; see
  [verification](../architecture/verification.md#low-level-memory-phase-1).

### S-ALLOC-05: Programmable Allocators And Safe Container Integration

- **State/role:** complete (2026-10-07). The statically borrowed lease
  direction was approved on 2026-10-06 through the user's delegated design
  review. The public protocol, a real placement-choosing arena, fixed-address
  owners, the growable `resource_vector` and an unrelated application container
  are implemented and pass the full production, installed-toolchain and tooling
  gates recorded below. Remaining structural limits are listed under
  **Completion evidence**; they are follow-up work, not open Phase 2 gates.
- **Outcome:** an engine-defined arena obtains one backing region, chooses
  differently sized/aligned subregions, and simultaneously supplies owning
  objects, a substantial growable container and an unrelated application-defined
  container. Ordinary default allocation remains straightforward.
- **Prerequisite checkpoint (2026-10-06):** unsafe member declarations and
  structural requirements preserve call-site permission; runtime object-layout
  queries cover ordinary owning and immovable types; general cleanup-bound
  borrowed guards retain their resources across moves, by-value calls, nested
  field publication and constructor rollback. Examples 117–119 demonstrate
  these capabilities. Focused verifier mutations and optimized native matrices
  cover the guard/rollback path, including no-elide and ASan/UBSan modes;
  broader reconciliation is still in progress. The original nested mutex
  capability boundary remains explicit and distinct from ordinary cleanup
  retention. None of this is yet allocator acceptance: selected-region
  adoption, the public resource/lease API, real arena placement, fixed-address
  immovable ownership and allocator-aware containers remained to implement at
  that checkpoint.
- **Storage prerequisite (implemented):** private external-region adoption
  now carries explicit unsafe permission, an exact mutable destination and a
  writable address/extent/alignment contract through MIR verification and
  backend admission. Its fallible slot-bitmap allocation does not take over
  region release authority. Public sparse storage now permits immovable
  elements through final-address emplacement; completed-value insertion and
  relocation retain their capability constraints. Example 120 and native
  cleanup fixtures pass the eight-mode C++20/C++23, no-elide and sanitizer
  matrix. The public resource clients below build on these primitives;
  primitive-only evidence is not allocator/container acceptance.
- **Starting evidence (2026-10-06):** inspected main `db6622a3`, including the
  optional-audit revision. `storage<std::movable T>` and its factories still
  restrict immovable elements; the private sparse storage representation obtains
  its allocation and initialization bitmap from the process heap. Application
  source cannot adopt an independently selected region. Public layout queries
  reject ordinary owning classes, although private allocation knows their
  representation. Unsafe declarations currently reject members, and structural
  member requirements exclude unsafe methods. Ordinary reference-bearing
  classes reject source destructors; the trusted mutex guard is a separate
  existing exception, not a general allocation-lease facility.
  Four disposable baseline probes with the optimized Release compiler 0.376.0
  reproduce unsafe-member rejection (`GTI-P0001`), borrowed-destructor rejection
  (`GTI-S2045`), owning-layout rejection (`GTI-S2063`) and immovable storage's
  `std::movable` requirement (`GTI-S2029`). The frontend and library sources for
  these rules are unchanged between the validated 0.376.0 candidate and the
  inspected head. These are prerequisite evidence, not Phase 2 acceptance tests.
- **Completion evidence (2026-10-07):** source-defined `resource_ref`,
  `resource_storage` and `resource_vector` clients, a real monotonic arena,
  a counting/failure adapter and an unrelated fixed-address container are
  implemented. The optimized macOS arm64 Release compiler (LLVM 22.1.8,
  AppleClang) passes all 460 registered gates in one full run. The allocator
  matrix runs example 121 and seven resource fixtures in all eight native
  configurations (C++20/C++23 at O0/O1/O3, disabled elision and ASan/UBSan):
  aligned placement from one backing region, simultaneous leases, zero size,
  invalid alignment, overflow, exhaustion, injected growth failure, a consumed
  failed push, constructor rollback through storage and `try_emplace_back`,
  owner-only movement, independent element destruction, cross-resource
  relocation and exactly-once destruction/release through the correct resource.
  Ordinary client shapes are executable: owners as class members, mutable
  container parameters, owning and unique elements, and the process resource.
  Negative controls reject provider reset or movement beneath live owners,
  reset through a shared binding, conflicting element access, element escape,
  forged bindings and immovable elements on relocating paths. Off/Warn
  raw-pointer auditing accepts the same allocator programs and emits identical
  C++. An isolated install passes all eleven external SDK consumers, example
  121 and the client fixture at C++20/O0 and C++23/O3, the resource LSP gate
  and the installed raw-audit policy harness. Tree-sitter's 72 corpus cases,
  highlight/locals/rainbow checks, 183 shipped sources and pinned regeneration
  pass; all 453 first-party C/C++ sources pass formatting and API
  documentation generation is deterministic. Linux/Windows execution and
  bundled-LLVM release packaging are not claimed.
  Integration repaired three backend gaps: chain-form generated moves now
  admit their `TransferOut` handoff as a native no-op, direct concrete
  instances of generic active-drop classes no longer receive duplicate
  explicit-specialization lifecycle definitions, and storage release publishes
  a prebuilt empty storage. Remaining structural limits: a free function cannot
  return a borrowed carrier constructed from a by-value carrier parameter
  (`GTI-S2085`, pre-existing and general, so resource-owner factories are
  unavailable); only `resource_vector` is allocator-aware; resources are
  compile-time selected, single-threaded unless their own contract says
  otherwise, and the example arena reclaims space only by whole reset; and the
  optional audit reports many analysis-limitation findings inside provider
  implementations.
  A follow-up audit found a missed composition: moving a by-value cleanup-bound
  parameter through local bindings lost its formal MIR provenance and raised
  an ICE. The correction retains that entry identity while independently
  verifying every local carrier's exact lifecycle transfer; it does not relax
  borrowed-owner lifetime rules. The audit also corrected unsafe member
  requirement advice that incorrectly demanded a safe implementation.
  Explicit nested constructors now
  compose formal-to-field dependency paths, with generic three-level and
  reordered two-origin tests, and backend nested publication is admitted.
  Client integration exposed a separate lifetime bug:
  an accessor returning an owned field of a borrowed carrier must protect that
  carrier's physical storage, not substitute its external resource dependency.
  Unrelated-class semantic regressions now cover reset/move conflicts, exact
  nested shared-view returns and physical-shell versus external-owner
  references. The expanded ownership suite, the resource-protocol contract,
  the lifecycle emitter and the forged lifetime/return proof mutations all
  pass in the full run. The owned-field,
  passive-field assignment and constructor-rollback regressions pass all
  eight native modes, including copied-formal status failure. Generic source
  destructors retain caller-owned receiver dependencies without acquiring
  their own release authority; normal, primary-failure, destructor-failure and
  double-failure traces pass all eight configurations. Iterator/reference
  tests distinguish physical container storage from its external resource,
  including explicitly retained iterators, copied adapters and range loops.
  A separate raw-region owner regression passes O0/O1/O3 under C++20/C++23,
  disabled elision and ASan/UBSan: generated movement preserves the region
  address and transfers exactly one release obligation. The arena shell now
  uses this existing generated-move contract; its separately allocated backing
  region and metadata do not move. This is not evidence for safe owner loans.
  The independent arena-placement fixture now passes all eight native modes,
  including aligned subregions, invalid requests, recoverable exhaustion,
  reset and reuse. The separate typed-owner/container matrix now also covers
  growth, failure preservation and exact release; unrelated raw placement alone
  was not used as evidence for those loans.
- **Approved resource model:** a move-only allocation lease
  retains a shared checked borrow of its resource through final cleanup. Each
  allocation owns an independent region; object references borrow its typed
  owner, not merely the backing resource. Several leases may coexist. A safe
  reset requires exclusive access to the resource and is rejected while any
  lease remains live. No implicit reference counting, shared ownership or new
  broad interior-mutability permission is introduced. Stateful bookkeeping may
  use encapsulated unsafe raw operations under an explicit protocol contract;
  it cannot mutate, release or reuse another live region. The initial resource
  is local unless its separate concurrency contract establishes more.
  General destructor-bearing borrowed objects must retain every dependency
  through cleanup, including movement, constructor rollback and failure, rather
  than inheriting a view's ordinary last-use endpoint. This rule must be tested
  with an unrelated non-allocation resource guard and audited against the
  existing mutex-guard special case before implementation.
- **Approved binding boundary:** application code acknowledges a custom
  resource's unsafe provider contract once, with an explicit unsafe binding
  operation. The resulting checked handle can supply safe typed owners and
  containers without repeating unsafe at each allocation. Concept satisfaction
  only checks the operation signatures; it does not prove alignment, valid
  extents, disjoint live regions, truthful identity or matching release.
  Binding never authorizes safe reset/reuse beneath a live owner or reference.
- **Bookkeeping mechanism:** keep mutable metadata in separately allocated,
  non-const storage, reached through private writable raw pointers rather than
  by casting away access on a shared resource reference. A shared borrow of the
  resource retains its shell/release authority; it grants no checked reference
  into this metadata. No checked metadata reference may overlap a raw mutation.
  Unsafe implementation obligations cover exclusive metadata access, reentrancy,
  live-region disjointness and correctly ordered release. Ordinary fields of a
  shared resource remain read-only. Raw writes retain their MIR memory effects;
  no backend purity/no-alias permission follows from a read-only receiver.
  Verify this mechanism with an unrelated source-defined bookkeeping owner
  before using it in the allocator, and do not add automatic thread safety.
  If the actual general aliasing/representation rules cannot support this
  separation, propose a narrow general mutation capability before introducing
  broader semantics. No implicit shared ownership or refcount is authorized.
- **Cleanup-path audit:** at the starting head, `createRetainedLoan` set
  `lifecycleBound` only for the trusted mutex-guard shape. The program verifier
  validated that marker specifically against mutex storage and a lock token.
  The prerequisite checkpoint separates general cleanup-bound dependency
  retention from mutex authority (including the guard's effective exclusive
  access), preserving the dependency set through normal/failure drop and
  independently verifying both contracts. This is a prerequisite for the
  approved model, not permission to broaden ordinary reference access.
  The canonical runtime architecture's
  pre-emplacement allocation description has been corrected to match the
  existing source-constructor/preflight MIR path; no executable behavior changed.
- **Protocol and placement requirements:** compile-time generic resource
  selection; validated size/alignment requests; recoverable allocation failure;
  allocation identity, usable extent and matching resource/release authority.
  Publish compiler-owned ordinary-object layout information usable for actual
  placement, without promising stable foreign ABI or universal constexpr
  layout. Provide aligned subregion selection with checked overflow, selected
  storage adoption, final-address construction and independent destruction.
  Unsafe member operations and structural requirements must preserve their
  caller obligations through ordinary/generic calls and forwarding. Safe
  wrappers establish those preconditions; neither concept satisfaction nor
  optional raw auditing proves an allocator algorithm correct.
- **Storage and container requirements:** preserve the existing MIR lifecycle
  engine, exact constructor-failure identity and partial-member rollback.
  Moving the allocation owner transfers the region and release obligation,
  without moving its elements. Provide a fixed-address owning path for
  immovable elements; only relocation operations and growable-container paths
  may require the corresponding element capability. Integrate one substantial
  allocator-aware growable container (prefer vector or its substrate), process
  allocation, a stateful application allocator, a counting/failure-injection
  adapter and an unrelated user container. Specify differing-resource movement
  and release explicitly; failed growth preserves the original contents and
  ownership. No silent allocator-propagation framework.
- **Reviewable implementation order:** (1) resource and unsafe contracts
  approved; (2) implement general missing lifetime/unsafe-call prerequisites
  and a process/custom-resource vertical slice; (3) prove genuine aligned
  suballocation and simultaneous leases; (4) close immovable emplacement,
  growable-container integration and rollback; (5) close diagnostics, tooling,
  canonical documentation, runnable examples and broader validation. Keep
  syntax in parsing, resolved ownership and effects in semantics, concrete
  facts in HIR, lifecycle/cleanup in verified MIR and representation in the
  backend. Reuse declaration-bound private capabilities only for irreducible
  mechanics; no recognition of public allocator/container names.
- **Completion gates:** demonstrate multiple live allocations from one backing
  region, natural/requested alignment, zero size, invalid alignment, arithmetic
  overflow, recoverable exhaustion, injected growth failure, original
  constructor failure and rollback, checked-reference escapes/conflicts, safe
  reset/reuse rejection, owner-only movement, relocation restrictions and
  exactly-once destruction/release through the correct resource. Exercise the
  same protocol with an unrelated application container and owning elements.
  Add focused positive/negative compiler and verifier tests, optimized-compiler
  runtime/cleanup matrices, applicable C++20/C++23 and optimization parity,
  Off/Warn audit independence, LSP/syntax/formatter coverage where affected,
  installed-toolchain checks, runnable examples and their index/corpus/census
  registrations. Use the repository's broader production gates after focused
  iteration. Update the canonical ownership/raw-pointer, runtime/IR and ADR 022
  contracts to implemented behavior, including stale Phase 1 descriptions of
  final-address emplacement; record all evidence and remaining structural limits.
- **Non-goals:** stable object ABI, arbitrary integer/address conversion,
  virtual-memory/device/mapped allocation, lock-free resources and migration of
  every container. Do not encode process-heap assumptions into the generic
  protocol that would exclude those later extensions. The final report must
  identify the new application capabilities, public resource/lifetime contract,
  remaining unsafe obligations, allocator/container examples and validation.

### S-ALLOC-06: General Object Lifetimes And Source-Defined Storage

- **State/role:** implemented through PR #94 and the subsequent issue-campaign
  integration, following completed `S-ALLOC-04/05`. The combined
  [release gate](../architecture/verification.md#combined-lifetimestorage-and-payload-enum-release-gate)
  records its original reconciliation evidence; the
  [S-ALLOC-07 completion acceptance](../architecture/verification.md#completion-acceptance-2026-10-08)
  also reruns the source-storage/resource integration on current main.
  This brief records the accepted direction; canonical language documentation
  and source describe the implemented API and its structural bounds.
- **Outcome:** an application can implement inline typed storage and an
  unrelated pool/container using public GTI facilities, without teaching the
  compiler its type name. Reimplement `std::storage<T>` policy on those same
  facilities while preserving its safe interface and Phase 2 integration.
- **Handoff:** continue from the agent's existing memory/lifetime context.
  This is a generalization of the completed work, not a restart of Phase 2.
  Reconcile current source before using historical progress descriptions.

#### Historical Implementation Audit And Accepted Choices (2026-10-07)

The source sparse/resource-owner integration now passes the focused resource
contract and native allocator matrix, including same-resource metadata failure
rollback. General multiple borrowed-field constructor admission has independent
malformed-MIR coverage and an unrelated runtime fixture. Example 128 adds a
fixed-address inline application pool. The legacy sparse semantic type,
intrinsics, C++ helpers and optimizer initialized-set model have now been
removed from the candidate; source slot checks remain conservative barriers.
General raw construction also consumes exact staged reference inputs, and
passive fixed-array arguments preserve their exact value type through generic
emplacement. Focused runtime matrices pass, including the allocator and
fixed-array regression. This remains an intermediate checkpoint: current-main
reconciliation, census inventories and the broad acceptance gates below are
still outstanding.

The baseline is `52f27033`; the initial audit checkpoint did not implement the
public manual-lifetime APIs below. Phase 2's accepted borrowing,
unsafe binding and failure contracts remain unchanged.

The owner approved **two separate allocations from the same bound resource**:
one object region and one live-slot metadata region. Zero capacity requests
neither. Metadata allocation is source policy, with checked byte extent and
alignment, and failure returns the object region through its original lease
before reporting the error. Element cleanup precedes release of either region.
The initial metadata layout is one byte per slot, matching the existing
implementation's state granularity without importing its hidden process-heap
allocation. Changing representation later must preserve the documented
resource-request contract or be treated as an observable policy change.

For carrier movement, the owner selected a Rust-inspired manual-storage model
adapted to GTI and delegated exact bounds. The implementation direction is no
automatic payload destruction and no hidden engagement flag. Ordinary carrier
movement must preserve potentially uninitialized representation without reading
it as a live `T`; initially it is available only for structurally bitwise-
relocatable payloads. Copying additionally requires copyable `T`. Other
carriers remain fixed-address, including when empty; an initialized movable
payload can instead use explicit typed relocation. Deleted payload movement
remains a hard exclusion. This is intentionally narrower than Rust's
`MaybeUninit` movement: GTI's generated owning relocation is not a license to
byte-copy its nontrivial backend representation. General owning relocation
must execute verified lifecycle transfer. Do not add a runtime live flag or
custom move body to make the raw carrier movable.

Keep the public spelling C++-familiar: typed pointer access, `construct_at`,
`destroy_at`, explicit `relocate_at`, and existing variadic constructor
selection. The Rust comparison guides the separation of raw representation,
live values and unsafe initialization assertions; it does not import Rust
syntax or override GTI's existing ownership, access and cleanup semantics.
Do not add C++ placement-new syntax, exceptions or user-defined move hooks
where the existing GTI call and lifecycle machinery expresses the operation.

The initial source inventory identifies these reuse/retirement boundaries:

| Existing implementation | Campaign action |
| --- | --- |
| `semantic_ownership_analysis.cpp` constructor selection and `preserveStorageConstructorParameters`; HIR concrete call inputs | Reuse for raw-address final construction, preserving exact selected constructors and staged owning arguments. |
| `mir_body_lowering.cpp` prepared-argument transfer, `StorageConstructPreflight`, `elementCleanup`, and failure edges; independent MIR verifier | Generalize typed lifecycle authority; sparse vacancy/bounds checks become source policy before handoff. Raw validity stays an unsafe obligation, not a runtime bitmap or mandatory audit proof. |
| `cpp_support_preamble.cpp` sparse `storage<T>` capacity/live flags and `try_adopt` heap bitmap | Replace with GTI fields and the two source-owned resource leases. Retire sparse capability representation when parity gates pass. |
| `StorageConstruct`, `StorageDestroy`, `StorageRelocate`, `StorageShiftRight/Left` and their sparse backend helpers | Replace with general typed lifetime operations plus source loops; remove the subsumed enum/dispatch/failure/representation paths rather than keeping aliases permanently. |
| `StorageCapacity/Initialized/Read/ReadMut`, `StorageTryAdopt/TryAllocate`, `AllocateStorage` | Migrate observers, validation, allocation publication and checked access into source; use the existing owner-witness bridge for element loans. |
| Prefix storage, optional storage, unique/shared owners | Preserve as separate current clients: initialized-prefix/NUL policy, active-alternative policy, and allocation/control-block ownership differ from sparse bookkeeping. They must share general lifecycle facts where applicable; wholesale public-policy migration is not claimed. |

`source_storage_drain_runtime` establishes a prerequisite without changing the
compiler: an ordinary GTI cleanup guard resumes reverse slot traversal after
the first destructor failure, preserves its record, and stops according to the
existing secondary-failure contract. It covers immovable owning elements,
member cleanup, ordinary drain and entry during failure cleanup. The guard's
marker distinguishes source traversal from the outer storage's fallback drop.
The guard advances its cursor before attempting destruction; the migrated
storage must likewise retire its live flag before `destroy_at`. This does not
yet prove the new raw operation, source metadata allocation or sparse migration.
All eight standard/optimization/no-elide/sanitizer modes pass on the optimized
0.379.3 baseline compiler. Carrier movement, raw construction/destruction and
relocation, metadata rollback, source migration, removal and full acceptance
gates remain required work.

The next implementation checkpoint adds public unsafe `destroy_at` backed by
declaration-bound `ObjectDestroy`, reusing exact MIR element cleanup and original
destructor failure propagation. The optimized `object_lifetime_contract` covers
scalar and unrelated immovable destructor types, C++20/C++23 O0/O3 emission,
unsafe/type negatives and forged cleanup/permission/failure metadata. The raw
scalar runtime test uses a native-created live object with separately released
bytes and passes C++20/C++23 O0/O1/O3, no-elide and ASan/UBSan. Adjacent object
layout, storage adoption and optimizer conformance gates pass. This is not yet
owning-object runtime evidence or a completed public manual-storage feature:
final-address construction, typed carriers, relocation, source migration,
examples, full acceptance and release gates remain outstanding.

The construction checkpoint adds public unsafe `construct_at` through
declaration-bound `ObjectConstruct`, sharing ordinary constructor selection,
prepared-parameter transfer and verified failure rollback. An unrelated source
byte owner and slot wrapper exercise owning and immovable payloads, failed
argument preparation, partial construction, destruction failure, secondary
failure and slot reuse. The eight-mode runtime matrix passes on the optimized
compiler. A passive record returned by a fallible field-initializer call also
required a backend representation fix: stage its result without a semantic
drop, then publish only on the immediate success edge. No source constructor or
cleanup is invented for the temporary representation. Typed inline carriers,
relocation, source sparse migration, runnable campaign examples and the full
integration gates are still outstanding; this remains an intermediate draft.

The next checkpoint adds the layout-only inline carrier and raw destructive
relocation. Focused carrier and owning-relocation runtime matrices pass all
eight standard/optimization/no-elide/sanitizer modes. Static tests reject
immovable carrier movement, incompatible access and forged capability/lifetime
facts. Example 125 demonstrates the public inline owner with a source-written
empty constructor, checked references and owning relocation. The latter exposed
two overly restrictive backend checks: direct field construction required move
assignment, and the passive field-initializer proof excluded the layout-only
carrier's observation-free default construction. The construction-only proof
and exact private-capability case now admit it without moving its payload or
admitting arbitrary source constructors. The example joins the carrier's full
eight-mode runtime matrix and public API inventory. Adjacent constructor tests
also exposed an invalid `void` field in unused dependent carrier declarations;
the declaration spelling now preserves generic staging while executable
instances retain concrete semantic/MIR copy and move eligibility. Combined census
reconciliation, source sparse migration/retirement, the
unrelated pool/container example and broad acceptance gates remain outstanding.

The source-policy checkpoint introduces ordinary `sparse_slots<T>` and its
reverse drain guard in `std/detail/sparse_storage`. Source fields and loops
perform metadata publication/retirement, checks, final-address construction,
destruction, relocation and shifts; narrow private boolean checks reuse the
existing invalid-storage failure families. The candidate runtime matrix passes
C++20/23 O0/O1/O3, no-elision and ASan/UBSan, including first and secondary
destructor failures. Checked element loans block mutation and owner movement.
This helper is not yet wired into public storage: the two source leases,
allocation rollback and retirement of the old sparse paths remain outstanding.

The owner approved conditional unsafe transfer/share assertions to preserve
payload-dependent thread capabilities without recognizing a container name.
The bounded spelling `[[unsafe_transfer(T), unsafe_share(T)]]` names one
non-pack generic type parameter per assertion. Semantics resolves and
substitutes that parameter through existing capability analysis; the wrapper
author still audits all implementation state, access and cleanup. This is a
general source feature, not an exemption for sparse storage. Conditional-policy
tests, tooling, a runnable audited-owner example and final integration evidence
are part of this checkpoint's validation rather than grounds to call the whole
campaign complete.

Focused conditional-capability contracts and malformed-MIR mutations pass,
including nested wrappers and Expected payloads. Example 126 passes all eight
native modes; the LSP test verifies generic-binder navigation, exact invalid
parameter spans and restoration after unsaved edits. Pinned Tree-sitter passes
72 corpus cases and all 186 shipped GTI sources. The reviewed 120-example
census now includes the five private detector declarations/bodies in every
example and the earlier memory API declarations in memory consumers, with no
change to existing generated-family counts. Full native corpus, installed
consumers and final current-main reconciliation remain final campaign gates.

The public-storage integration remains under validation, not complete. Sparse
policy resides directly in `<std/memory>` so ordinary source-unit visibility
does not require transitive includes. Both process storage and resource storage
use separate object and live-slot leases; `storage_element<T>` makes the existing
no-checked-borrow payload boundary an explicit source constraint. A narrow
native address-alignment observation preserves defensive provider validation;
it does not prove provenance or replace the unsafe provider obligations.
Source sparse runtime/cleanup and native alignment/ABI checks pass. The
multi-field lifetime identity and constructor-representation gaps are fixed,
with unrelated guarded-pair runtime and malformed-MIR regressions. The full
focused resource runtime matrix now passes, including two-allocation rollback
and original descriptor/release ordering. Legacy sparse compiler paths have
been retired. Current-main/census reconciliation, optional-audit parity, broad
production and installed-consumer acceptance remain outstanding; the focused
results are not a completed-campaign claim.

The current acceptance fixes also close general composition gaps exposed by
the source owners: moved multi-field carriers retain their exact input loans
through ordinary generic forwarding, and independent compact borrowed inputs
retain their own origins when combined. Prefix mutators still reject live
overlapping loans. Generic primary-body definition checks defer only while
actual enclosing bindings remain open, then reject missing concrete callees.
These rules are covered with unrelated application types rather than storage
name recognition.

Default construction with declaration-field initializers now selects an
explicit generated constructor schedule. Its class-level obligation survives
semantics, HIR, MIR and detached LoweredProgram verification, so erasing the
selected constructor cannot silently restore native field initialization.
Field publication arms rollback only after successful construction; implicit
scalar/null/expected initialization uses existing semantic conversion rules.
This is not the separate omitted-field/default-policy work. Focused generated
schedule, cleanup, ownership and backend tests are being reconciled with the
new representation. Raising passive expected publication now passes its
eight-mode native matrix with representation-only payload staging and no
premature wrapper construction. The state-bearing base-constructor path remains
acceptance work; this checkpoint is not ready for integration until those tests
and the final combined gates pass.

#### Why This Campaign Exists

Phase 2 successfully separated resource selection and region release from
typed element ownership. However, the public sparse-storage class still
delegates slot tracking, construction, destruction, relocation and shifts to
specialized private storage operations. External-region adoption also obtains
its initialization bitmap from the process heap. A public class written in GTI
is not yet a fully source-defined implementation if its algorithms and state
remain inside those operations.

The direction is not to remove all compiler support or rewrite everything on
top of `malloc`. Byte allocation, object lifetime and checked borrowing are
different responsibilities. A libc or OS provider can obtain bytes; it cannot
select a GTI constructor, execute GTI cleanup or establish a checked reference.
General object-lifetime operations remain legitimate compiler capabilities.
Expose them through public, C++-familiar APIs, with narrow private mechanisms
where needed, and put expressible storage policy in ordinary GTI.

The reference backend must implement this contract, not derive GTI's meaning
from native placement construction, destructors or byte copying. Another
backend should implement general layout/lifetime/effect operations rather than
recreate a separate family of container algorithms. Source definition reduces
that specialized backend surface; it does not eliminate backend work for
object representation and cleanup.

#### Existing Foundations To Preserve And Reuse

Audit source and tests before adding anything. Current foundations include
runtime `layout_of<T>()` for permitted ordinary owning and immovable types,
erased/typed address conversions, public `borrowed_from` contracts and
owner-anchored `assume_ref`/`assume_mut`, unsafe members and structural
requirements, generated cleanup-transfer movement, final-address construction
and rollback, and destructor-bearing borrowed carriers whose dependencies
remain live through cleanup. These are foundations, not missing features.

Preserve move-only resource leases, simultaneous allocations from one resource,
safe reset/move rejection beneath live leases, owner-tied element references,
fixed-address immovable objects, allocation failure, constructor failure
identity, partial-member rollback and exactly-once release through the original
resource. Do not add hidden reference counting, allocator propagation, broad
interior mutability or custom C++ move semantics to accomplish this campaign.

#### General Public Capabilities

**1. Typed uninitialized storage.** Provide a general facility, illustratively
`std::uninitialized<T>`, with the selected size/alignment of `T` but no initial
live `T`. Creating the carrier must not invoke a `T` constructor; dropping an
uninitialized carrier must not invoke a `T` destructor. Safe reads and checked
references require a live object, established by a safe wrapper or an explicit
unsafe assertion. Prove the capability with inline storage as well as heap
regions. A byte array alone is insufficient unless the language also supplies
the required alignment, representation and lifetime contract.

Separate ownership of the storage carrier from ownership of a live payload.
Specify address formation, copying/movement, failure and cleanup precisely;
moving a carrier containing a live object must not silently byte-copy an
immovable or owning payload. Do not infer that every public manual-storage
wrapper is movable merely because its representation contains bytes. Choose
the smallest coherent structural contract, asking the owner if this requires
a materially new movement or lifetime policy. It need not introduce new syntax
or a second optional/variant ownership model.

**2. Final-address construction.** Expose a general unsafe operation resembling
`std::construct_at<T>(address, args...)`. Semantics selects the actual GTI
constructor, including permitted immovable types, and records argument
preparation, transfer and failure facts. Reuse the existing MIR constructor
preflight and partial-construction machinery; do not create a second native
constructor path. Caller obligations include suitable writable extent,
alignment, vacancy and nonconflicting access. The complete object becomes live
only after success. Failure cleans initialized subobjects and prepared owning
arguments under existing rules and retains the original failure identity.

**3. Destruction without deallocation.** Expose a general unsafe operation
resembling `std::destroy_at(address)`. It runs the actual GTI destructor and
field cleanup, ends the object lifetime, and does not release its bytes.
Describe exactly when the cleanup obligation is consumed, including destructor
failure and secondary failure. Source bookkeeping must follow that established
contract; marking a slot before or after destruction is not an arbitrary
implementation choice when destruction can fail. Preserve existing failure
containment and cleanup ordering.

**4. Ownership transfer between locations.** Expose the established relocation
capability through a general unsafe operation resembling
`std::relocate_at(destination, source)`. Distinguish it from ordinary move
construction where GTI's contracts differ. Define initialized source,
uninitialized destination, overlap restrictions, source lifetime retirement,
destination publication and failure/rollback boundaries. On successful
destructive relocation exactly one destination owns the cleanup obligation;
the old source does not remain a second live owner. Keep existing structural
eligibility rules, including immovable exclusions. Do not authorize arbitrary
`memcpy` of owning types, inherit C++ relocation assumptions, or add custom
copy/move bodies as an unrelated prerequisite.

These names are proposals. Prefer the existing source conventions and reuse
existing typed identities/operations where they express the contract. A public
library function backed by a narrow intrinsic is acceptable. Bind any intrinsic
by trusted declaration identity, never by a public spelling such as
`std::construct_at` or by recognition of a particular application container.

#### Safe And Unsafe Boundary

Mandatory typing, access modes, safe ownership, checked loans/escape and
HIR/MIR verification remain in force. Unsafe callers assume raw validity,
alignment, initialization, vacancy, aliasing and truthful owner association.
The compiler need not prove an arbitrary allocator algorithm. Optional raw
auditing stays off by default; Off/Warn must not change executable semantics
or reject code solely because a bounded raw query cannot establish validity.

For a safe source-defined accessor, check bounds and the source-owned live
state, calculate the slot address, then use the existing unsafe owner-witness
bridge to return a checked reference. That reference borrows the actual typed
owner, not merely the resource that supplied its region. The assertion is the
unsafe implementation's obligation; subsequent checked lifetime protection is
the compiler's responsibility. Preserve physical carrier storage versus
external resource dependency distinctions and shared/exclusive access modes.

Do not require compiler-owned runtime initialization flags for every raw object.
Flags are appropriate policy for safe sparse storage. A general manual lifetime
operation need not maintain a hidden bitmap for arbitrary unsafe application
code. Equally, do not weaken `std::storage<T>` checks merely to remove its private
representation.

#### Responsibility And Migration Map

| Fact or operation | Authority after migration |
| --- | --- |
| Object layout and structural lifecycle eligibility | Semantics/concrete type facts; backend-selected representation |
| Constructor/destructor selection, ownership transfer, failure and cleanup | Existing semantic/HIR facts and verified MIR lifecycle engine |
| Capacity, live-slot map, bounds checks, shifting and growth algorithms | Ordinary GTI storage/container source |
| Byte-region acquisition, alignment guarantee and matching release | Source-selected resource plus its narrow host/provider adapter |
| Raw-to-checked association | Explicit unsafe assertion; checked owner/access/endpoint facts thereafter |
| Resource and element loan origins/endpoints | General borrowing semantics, preserved and independently verified in MIR |

Inventory sparse and prefix storage, optional storage, unique ownership and
other overlapping clients before implementing. The named migration is sparse
`std::storage<T>` and its existing resource clients; do not promise migration of
every container. General operations should subsume sparse slot-specific
construction/destruction/relocation/shift paths. Record why any remaining
capability is a distinct invariant, rather than retaining a nominal exception
for each wrapper. Prefix and other clients must not regress; their further
migration may be recorded separately if outside this coherent slice.

Put slot bookkeeping, validated address calculation, clear/drop sequencing,
shifting and relocation loops in GTI source. Preserve argument preparation and
consumption, reverse destruction, release ordering and failed-growth behavior.
Keep metadata allocation explicit in source and resource-controlled or
source-selectable, not an incidental backend heap request. Specify metadata
extent/alignment, release authority and recoverable failure before publication;
avoid circular dependence on the storage implementation being built. Retain
public API compatibility where possible; ask before choosing a new observable
metadata-resource policy or silently adding costs to allocation.

For process allocation, libc is a viable provider adapter if it meets extent,
alignment, zero-size and matching-release contracts. It is not mandatory to
replace a correct current provider during this campaign. Never release aligned
C++ allocations through `free`, or libc allocations through native `delete`.
Do not promise universal constexpr layout or stable foreign object ABI.

#### Reviewable Implementation Order

1. Audit the existing implementation and list reused, generalized and retired
   operations. State the public contracts and identify unresolved semantic
   choices requiring owner approval; do not reopen accepted Phase 2 decisions.
2. Deliver the smallest general lifetime vertical slice, including inline
   uninitialized storage, unrelated owning/immovable types and construction/
   destruction failure. Keep semantic, HIR/MIR, effects, backend, diagnostics
   and affected tooling coherent at each published checkpoint.
3. Add general relocation with existing eligibility and failure rules. Prove
   cleanup transfer independently of a nominal standard-library owner.
4. Migrate sparse storage and resource integration to source-owned algorithms
   and metadata policy. Use parity evidence before retiring specialized paths;
   do not leave permanent parallel implementations of the same behavior.
5. Close unrelated application pool/container evidence, regression and
   performance checks, runnable examples, canonical documentation, installed
   consumption and PR integration. Record remaining structural limits honestly.

#### Completion Evidence Required

- A user-defined inline-storage owner and an unrelated pool/container use the
  public facilities without compiler changes for their type names. Demonstrate
  safe checked access around encapsulated unsafe internals; primitive-only
  probes or private-prelude fixtures are not the application acceptance gate.
- Positive/negative cases cover owning and immovable construction, empty/live
  state, slot reuse, movement, structural relocation eligibility, shared and
  exclusive access, owner/resource escape and invalidation, zero capacity,
  alignment, overflow and recoverable byte/metadata allocation failure.
- Runtime traces prove exactly-once cleanup, reverse destruction, prepared
  argument failure, partial construction, destructor failure and original
  failure identity. Failed growth preserves the original contents/ownership;
  elements die before their region is released through the original provider.
- Independent MIR mutations reject forged constructor, lifecycle, access,
  transfer, cleanup and failure metadata. Backends consume verified facts
  rather than rediscovering them from public names or C++ traits.
- Use an optimized GTI compiler for all compilation/analysis evidence. Run
  affected native standard/optimization, no-elide and sanitizer matrices,
  Off/Warn parity, broader production gates and installed-consumer checks.
  Measure representative storage/resource workloads and investigate new hidden
  allocation, cleanup or compile-time costs rather than assuming a source
  rewrite is automatically cheaper.
- Ship runnable inline-storage and unrelated pool/container examples, with
  example index/corpus/census registration. Update affected LSP, formatter,
  Tree-sitter, diagnostics and public-source inventories as required by the
  actual surface. Follow the existing evaluator direction if constant
  execution is touched; constexpr expansion is not an incidental promise.
- Update canonical ownership/raw-pointer, stdlib, runtime, IR and ADR 022
  descriptions as implementation lands. This planned brief does not change
  current semantics. Report migrated source policy, removed specialized paths,
  remaining justified compiler mechanisms, unsafe obligations and evidence.

**Scope control:** this is not a new lifetime syntax, custom copy/move campaign,
general interior-mutability campaign, allocator-traits framework, whole-stdlib
rewrite or second backend. Ask before materially new semantics; routine
implementation choices remain delegated. Follow the normal repository PR
lifecycle and local-main synchronization, without installing the toolchain
unless requested.

### S-ALLOC-07: Source-Defined Process Allocation Over Native Byte Providers

- **State/role:** source-provider migration and completion acceptance are done
  through PR #94, the issue reconciliation release and PR #162. The final local
  gates pass; unavailable native-platform evidence and the measured performance
  regression remain explicitly reported below and in follow-up #169.
  Process request validation, native-size/zero handling, target selection and
  failure mapping are GTI source policy over POSIX/CRT forwarding adapters.
  Default sparse storage and its metadata both use that provider; generated
  sparse allocation is retired. The completion audit below records the
  evidence. This does not claim every allocation client has migrated or every
  ABI adapter is removable.
- **Outcome:** the default typed-storage and process-resource paths acquire
  bytes through source-defined GTI policy and ordinary native bindings, rather
  than specialized C++ allocation policy. Application pools and resource-backed
  containers retain the same checked ownership, failure and cleanup contracts.
- **Continuation:** reuse the preceding campaign and Phase 2 context. Do not
  reopen their accepted lifetime or unsafe boundaries, or duplicate their
  construction/destruction/relocation machinery.

#### Completion Audit (2026-10-08)

The audit starts from canonical main `f0ba8383`, after the issue campaign was
integrated. The implementation already satisfied the named source-provider
migration. PR #162 completes its acceptance evidence and corrects stale boundary
descriptions, rather than introducing a second allocator implementation. It also
fixes the general checked-numeric-argument admission defect exposed by the final
cleanup fixture (#165); the fully tested implementation revision is `fa72b523`.

| Required outcome | Owning implementation and evidence |
| --- | --- |
| Source request policy and canonical platform boundary | `gti_internal::process_resource` is an ordinary class in `stdlib/std/memory.gti`; no compiler source recognises its name. `process_provider_target_contract` selects POSIX/CRT calls for macOS, Linux and Windows; the C ABI smoke fixes their exact signatures. |
| Default typed clients and source metadata | `std::allocator::try_storage`, process leases and `try_allocate_sparse` use two same-provider regions. `process_storage_policy.gti` observes their exact requests, one metadata byte per slot, zero/invalid/overflow requests, first/second acquisition failure, rollback, simultaneous owners, immovable owning elements, slot reuse, owner movement and element-before-region release. |
| Existing custom resources and checked safety | `resource_protocol_contract`, `programmable_allocators_runtime`, `storage_adoption_contract`, source-storage/lifetime matrices and allocator Off/Warn parity retain resource/element borrow rejection, malformed descriptor checks, failed growth, constructor/destructor failure and original release authority. |
| Retired and remaining mechanisms | Production `include`, `src`, `stdlib` and `runtime` contain no old `gti_rt_memory_allocate_v1`/`release_v1`, sparse capability or sparse backend allocator. The [allocation inventory](../architecture/standard-library-and-runtime.md#allocation-inventory-and-remaining-boundaries) identifies remaining prefix, unique/shared and host-service families. |
| Installed and runnable clients | The provider matrix has an explicit installed mode that rejects missing stage resources and pins their exact paths, preventing build-tree fallback. CI and release staging run it; the installed library smoke links the native memory boundary. Examples 119–121, 125–126, 128–129 and `libc-region-provider` exercise the existing public APIs. Their source, example index and census identities are unchanged. |
| Broad, platform and performance acceptance | The clean optimized `fa72b523` candidate passes the full 558-test production/tooling suite, 13 installed consumers, both native/installed provider matrices and 14 staged example executions. A quiet-host frozen comparison records a material runtime regression (process 3.68×, resource 2.12× baseline), unchanged scalar/destructor state and the deliberate doubling of resource acquisitions for source metadata. [Completion evidence](../architecture/verification.md#completion-acceptance-2026-10-08) includes the investigation and [performance follow-up #169](https://git.internal.howetotech.dev/gti-lang/gti/issues/169). Local execution is macOS arm64; Linux native CI is queued without a runner and Windows native execution is unavailable. Target/ABI checks are retained without claiming native execution on those platforms. |

The ABI-adapter rationale is revised explicitly in ADR 022: native `c_size_t`
and nested `void**` are now supported. Unique forwarding symbols still avoid
occupying applications' C declarations, while Windows reserved symbols and
target facts justify the remaining small adapters. No semantics or allocation
policy change is required for that correction.

#### Starting Point And Architectural Purpose

At the planning baseline, `stdlib/std/resource.gti`'s `process_resource` calls
`gti_rt_memory_allocate_v1`/`gti_rt_memory_release_v1`. That is already an
ordinary C ABI boundary, not nominal compiler recognition of the public class.
`runtime/src/memory.cpp` validates requests and uses aligned C++
`operator new`/`operator delete`. Replacing those two native calls alone is
not this campaign's architectural outcome: move expressible request validation,
failure translation and provider selection into GTI source as well.

Calling libc from GTI still uses the selected backend for ordinary code and C
interop. Independence means that the backend does not own the allocator's
policy or invent ownership from `malloc`; it does not mean executing source
without lowering. Another backend must implement GTI's general lifetime and C
ABI contracts, not reimplement a bespoke storage allocator.

#### Chosen Boundary

| Responsibility | Owner |
| --- | --- |
| Validate requests, normalize zero size, check arithmetic/native-size conversion, map failures and publish descriptors | Ordinary GTI provider source |
| Slot state, metadata policy, construction sequencing, growth and rollback | Source-defined storage/container code from `S-ALLOC-06` |
| Acquire/release byte regions and expose necessary target ABI facts | Ordinary native bindings, with minimal platform adapters where necessary |
| Resolve constructors/destructors, layout and structural transfer eligibility | General GTI semantic/type facilities |
| Preserve checked loans, transfer/drop obligations, effects and failure edges | Existing HIR and verified MIR contracts |
| Emit those operations and marshal native calls | Backend representation and execution support |

Compiler facilities are justified for object validity, layout and execution of
the selected GTI lifecycle. They are not justified merely because an existing
container happens to call a C++ helper. Public spelling must never confer
allocation or lifetime meaning. Do not add a `malloc` intrinsic or recognize a
particular libc package, allocator or resource name in semantic analysis.

#### Source Provider And Platform Adapter Contract

Keep the existing public `memory_resource`, `raw_allocation`, process-resource,
lease and typed-storage APIs compatible. The hosted default provider is a
source-defined resource with a small target-selected implementation. Selection
uses the compilation target, not the compiler host, and belongs to ordinary
source/build configuration rather than a special backend allocation path.

Use libc/CRT byte allocation rather than C++ object allocation on the migrated
paths. The initial aligned paths are POSIX `posix_memalign` paired with `free`,
and the Windows CRT `_aligned_malloc` paired with `_aligned_free`. Normalize
POSIX alignment to satisfy both the GTI request and the native pointer-size
constraint. Prevalidate Windows arguments and handle zero bytes before calling
the CRT so invalid requests do not invoke its invalid-parameter handler.
See [the POSIX-conforming aligned-allocation contract](https://man.openbsd.org/posix_memalign.3)
and [the Windows aligned-allocation contract](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/aligned-malloc?view=msvc-170).

Ordinary `malloc`/`free` is also a valid source provider and should be exercised
by an unrelated raw-region example. An optimized default path may use it when
the target's documented alignment guarantee covers the request. Do not guess
that guarantee from GTI object layout or pointer width alone. A single aligned
allocation family is an acceptable initial default; family selection must be
recoverable from the unchanged descriptor and immutable target policy, or
retained explicitly by the implementation. Never free an aligned CRT block
with ordinary `free`, or a libc block with native `delete`.

Provide the minimal canonical bindings needed by the stdlib; reuse existing
libc declarations where available and correct. Do not require an independently
downloaded third-party package to bootstrap default stdlib allocation. Preserve
target-native signatures, particularly `size_t` width and output pointers;
`uint64_t` is not a universal C ABI substitute. Where a header macro, target
constant, calling convention or unsupported ABI shape needs a native shim,
permit a small reviewed C ABI adapter. That shim supplies native facts or
forwards a call; it must not conceal storage algorithms, typed construction,
bitmap policy, GTI cleanup or application resource selection. Document every
remaining shim and why an ordinary binding is insufficient.

Do not promise that every target has libc. Keep this hosted provider separate
from the generic resource protocol and the later freestanding profile. Custom
resources may use another allocator or OS service without changing language
semantics. No runtime-selected global allocator, replacement-`new` framework,
allocator traits or dynamic provider erasure is required.

#### Observable Allocation And Release Contract

- Validate nonzero power-of-two alignment before native calls. Stronger actual
  alignment is allowed; preserve the descriptor's contract and matching release.
- Preserve successful zero-size allocation without a native allocation request,
  using the existing empty-descriptor convention and harmless empty release.
- Reject arithmetic or target-native size conversion overflow before allocation,
  including any padding/rounding used by an adapter. Do not wrap a request into
  a smaller allocation. Native exhaustion remains recoverable and distinct
  from invalid requests and GTI construction failure.
- Normalize native return/status conventions in GTI source. Publish a complete
  descriptor only after successful byte acquisition. A failed request must not
  leak a region, mutate a live owner's release authority or consume its contents.
- Returned bytes initially contain no live GTI objects. Start/end lifetimes
  only with the general facilities from `S-ALLOC-06`; destroy every required
  object before releasing its region. Moving an owner transfers its original
  descriptor/provider and does not move its elements.
- Release receives an outstanding descriptor from the same allocation family.
  It does not throw native exceptions across C ABI, execute object destructors,
  read destroyed elements or authorize duplicate release. Preserve GTI's
  existing destructor/failure cleanup scheduling around that call.

Do not use `realloc` to grow a region containing arbitrary live GTI objects.
Container growth obtains a new region, performs the approved typed transfer,
and releases the old region in the established order. Preserve immovable and
failure-atomicity restrictions. Byte-only reallocation is separate future
breadth, not permission to bypass those rules.

#### Metadata, Bootstrap And Unsafe Obligations

Use `S-ALLOC-06`'s explicit source metadata policy; do not reintroduce a hidden
backend process-heap bitmap through this adapter. The byte-provider bootstrap
must not depend on allocating the typed container it is intended to support.
Validate independent object/metadata extents and clean acquired regions if a
later acquisition fails. Counting/failure-injection evidence must see the
allocations promised by the documented policy, including padding and metadata.

Native allocation establishes bytes, not compiler-proved allocator correctness.
Unsafe implementation obligations still cover truthful extents, alignment,
disjointness, initialization, owner association and matching release. Safe
leases and element references remain checked after their explicit trust step.
Optional raw auditing stays optional and must not change executable acceptance
or behavior in Off/Warn. Do not broaden shared mutation or thread-safety
promises merely because the underlying native heap supports concurrent calls.

Source-defined code improves attribution of type, access, borrow and ordinary
control-flow errors to GTI source. It does not turn every allocator bug into a
compile-time diagnostic: false unsafe assertions, incorrect release and other
logic errors still need runtime tests/sanitizers. General lifetime lowering
errors remain compiler defects. Preserve source locations through ordinary
calls and panic paths without disguising an internal compiler failure as a
user allocator error.

#### Migration And Completion Gates

1. Inventory actual bindings, supported target ABIs and all remaining allocation
   sites. Separate GTI-program allocation from the compiler's own host memory
   and unavoidable representation mechanisms. Classify each residual helper
   and identify which migrated users previously called the runtime C++ path.
2. Implement the canonical bindings/minimal adapters and source-defined process
   provider, with ordinary raw allocation/release tests before typed clients.
   Test target selection, native signatures and null/status conversion rather
   than assuming that a declaration with a familiar name matches the ABI.
3. Route the default sparse-storage factories and existing process-resource
   clients through the source provider. Preserve public contracts, Phase 2
   resource leases, construction/failure identity and metadata policy. Retire
   subsumed runtime/backend allocation policy after parity tests pass. Keep an
   old exported ABI only if it still has a compatibility consumer; document
   that reason rather than leaving a second permanent policy implementation.
4. Prove zero-size, invalid/natural/over-alignment, native-size overflow,
   exhaustion and injected failure, simultaneous regions, allocation/release
   family pairing, metadata rollback, owner movement, constructor/destructor
   failure, failed growth and exactly-once region release. Exercise owning and
   immovable elements and unrelated application providers/containers.
5. Add a runnable source-defined libc provider example and validate migrated
   storage/resource examples with an optimized GTI compiler. Update their
   index/corpus/census and public-source inventories. Run relevant C++20/C++23,
   optimization, no-elide and sanitizer matrices, mandatory borrow/escape
   negatives, Off/Warn parity, installed consumers and broader production gates.
   Verify each supported hosted platform through available native CI; identify
   unavailable evidence and do not present local macOS success as Windows/Linux
   execution. Maintain target/ABI tests even when native execution is unavailable.
6. Compare representative allocation/storage workloads and compilation costs;
   explain changed allocation counts or regressions. Update canonical stdlib,
   native-interop, runtime and ADR 022 descriptions to actual support. Report
   source-owned policy, retired private paths, justified remaining shims and
   any other stdlib allocation clients still awaiting their own migration.

Completion is not merely a change from `operator new` to `malloc` inside a C++
helper. It requires source-owned provider policy on the named typed clients and
unchanged safety/cleanup behavior. It does not require moving compiler host
allocation into GTI, migrating every distinct owning representation, inventing
a second backend or implementing freestanding allocation. Follow the existing
PR lifecycle and local-main synchronization; installation remains opt-in.

### S-ALLOC-08: Rust Allocation-Failure Model

- **State/role:** done in 0.397.0. The contract is
  [ADR 022's Rust allocation-failure revision](../decisions/022-safe-typed-allocation.md#revision-rust-allocation-failure-model-2026-10-09)
  and [ADR 007's exhaustion revision](../decisions/007-defined-runtime-failure.md#allocation-exhaustion-revision-2026-10-09).
- **Outcome:** each allocation behaves as it would in Rust.
  - Infallible allocation (`make_unique`, `make_shared`, vector and string
    construction and growth, the resource vector's plain operations, owning
    callables and boxed polymorphic payloads) aborts on exhaustion after a
    best-effort, allocation-free `GTI-R0011` report. No cleanup runs.
  - Fallible `try_` allocation keeps its `expected` result.
  - Size-computation overflow stays a contained defined failure.
  - Process allocations are not observable effects; MIR optimization may
    remove an unused process allocation and its release. Calls into an
    application `memory_resource` are never removed.
- **Completion evidence:**
  - `allocation_abort_runtime` covers the direct, managed-worker,
    observer-reentry, invalid-record, closed-stderr and broken-pipe abort
    paths with no cleanup or allocation.
  - Each infallible family has an exhaustion test that requires the report,
    the abort status and an empty cleanup trace:
    - `make_unique`: `unique_scalar_allocation_runtime`,
      `unique_class_allocation_runtime`,
      `inherited_unique_allocation_runtime` and
      `existing_value_allocation_failure`;
    - `make_shared`: `shared_ownership_failure`;
    - vector and string growth: `source_prefix_owner_runtime` and
      `source_prefix_string_runtime`;
    - the resource vector's plain operations: `source_resource_prefix_runtime`;
    - owning callables and boxed payloads: `owning_function_policy_failure` and
      `inline_polymorphic_runtime`.
  - Fallible families keep their exact `expected` results in
    `allocator_storage_runtime` and `programmable_allocators_runtime`.
  - `process_allocation_elision` shows the unused owning temporary's
    allocation removed at O3 while an escaping owner still allocates.
    `resource_container_usage` requires exact arena allocation and release
    counts in every O0/O1/O3 mode, so provider calls are not removed.
  - The census refresh is reviewed in the
    [verification record](../architecture/verification.md).

### S-ALLOC-09: Raw-Buffer Vector And String

- **State/role:** done in 0.397.0. The contract is
  [ADR 022's raw-buffer revision](../decisions/022-safe-typed-allocation.md#revision-initialized-prefix-sequences-over-a-raw-buffer-2026-10-09).
  It completes migration rows M01, M02 and M04, now S22–S24 in the
  [standard-library ledger](../architecture/standard-library-and-runtime.md#a-policy-already-implemented-in-ordinary-gti).
  M03, string's terminator, follows it.
- **Outcome:** `std::vector` and `std::string` use Rust's `RawVec`/`Vec`
  layering in ordinary GTI, and the private `prefix_storage` capability is
  retired:
  - a source-defined raw buffer over a memory resource;
  - an initialized-prefix length invariant with no per-slot metadata;
  - one check per safe public operation, with raw access internally;
  - `unsafe` unchecked accessors on vector, string, span and fixed array;
  - byte-copy growth for bitwise-relocatable elements.
- **Completion evidence:**
  - The source-container optimizer proofs pass unchanged, as does native
    optimization evidence. The general rules that recover them are described
    in [optimization.md](../architecture/optimization.md); the compiler
    recognizes no container or buffer name.
  - Against the preserved private-path compiler on the same machine,
    interleaved medians are 74.48 against 75.36 ms for the checked vector
    loop, 78.83 against 78.97 ms for game update and 32.73 against 72.95 ms
    for string growth. The checked vector loop measures 72.07 ms against
    71.44 ms for idiomatic C++.
  - Allocation counts are identical: one 20,000-byte region for the vector
    loop and 6,800 allocations of 52,435,200 bytes for string growth.
    Executables are smaller: 127,320 against 142,072 bytes, 143,816 against
    164,536 and 127,928 against 146,112.
  - Final-address emplacement, construction rollback, exactly-once cleanup,
    checked loans and string's C-string bridges keep their focused native
    matrices, and the full suite passes on the reconciled head.
- **Remaining boundary:** process allocation still reaches the opaque native
  allocator, so constructing a second sequence erases the first sequence's
  element facts. Modeling process allocation as fresh, non-observable memory
  is a separate precision goal.

### S-FREE-01: Freestanding Target Profile

- **State/role:** later breadth; `S-LAYOUT-01`, `D-LANG-01`, and `D-FAIL-01`
  are done. A concrete embedded/kernel workload may move it earlier.
- **Scope:** Define a smaller prelude and explicit required runtime services for
  a freestanding target without changing core expression, ownership, or
  cleanup semantics. The first bounded sub-slice inventories current hosted
  runtime services and proposes the target profile; later sub-slices diagnose
  unavailable allocation/output/thread services at the source/runtime boundary.
- **Non-goals:** kernel support by merely omitting libraries, implicit target
  feature probing, or a stable ABI.
- **Exit gate:** the adopted profile enumerates every required runtime service;
  one installed freestanding smoke either builds using exactly those services
  or fails early with a target-capability diagnostic, never a native link
  surprise.

## Phase C: Concurrency And The Language Memory Model

D-MEM-01 is complete as design evidence. D-FAIL-01 selected contained worker
failure with cleanup and original-record preservation, and D-MEM-02 adopted
the complete boundary in ADR 008. ADR 012 assigns its bounded public profile to
the systems-readiness work-queue outcome. The rows below make the
implementation dependency explicit; their
presence is not authorization to skip a named capability, lifetime, runtime,
or MIR prerequisite.

### I-CAP-01: Secure Compiler-Private Capability Identity

- **State/role:** done in 0.97.0; prerequisite `D-LANG-01` is done; architecture
  fix and prerequisite for every new private atomic/thread
  capability.
- **Scope:** Reserve `gti_internal` against ordinary application declarations
  and references, while allowing the trusted prelude/runtime units that own it.
  Bind compiler-private types by trusted declaration identity rather than
  qualified spelling. Cover aliases, source-unit visibility, LSP presentation,
  and forged declarations.
- **Non-goals:** making public wrappers compiler-known or adding source
  attributes that grant intrinsic authority.
- **Exit gate:** applications cannot forge or directly consume private
  capabilities; ordinary GTI stdlib wrappers continue to work by selected
  identity.
- **Completion evidence:** source-unit roles grant trust only to the implicit
  prelude and physical configured standard-library units; root `gti_internal`
  declarations, direct references, and alias targets in application source
  produce `GTI-S2058`. Private owner/storage/text-view types bind through exact
  trusted-prelude class identities, aliases and public-signature publication
  preserve privacy, the backend consumes semantic capability facts, and shared
  completion/hover/definition/semantic-token queries filter application
  presentation. Focused application-forging, trusted-wrapper, source-role, and
  LSP tests pass.
- **Unlocks:** `C-TYPE-01`, atomics, threads, and a later allocator capability.

### C-TYPE-01: Transfer And Sharing Capabilities

- **State/role:** done; `I-CAP-01` and `D-MEM-02` are done; durable concurrency
  foundation.
- **Scope:** Add compiler-owned structural facts equivalent to the accepted
  transfer/share concepts without importing Rust spelling by default. Derive
  them through fields and concrete generic instances. Cover primitives,
  aggregates, classes, interfaces, unique owners, borrowed-state carriers, raw
  pointers, destructors, cleanup-owning resources, and native thread-affine
  handles. Implement the adopted explicit or unsafe opt-in/opt-out rule rather
  than deriving thread affinity from fields. Keep capabilities in semantics
  and HIR so library constraints query one authority.
- **Initial conservative boundary:** references, mutable borrowed-state values,
  raw pointers, and borrowed/reference captures of local storage do not cross a
  thread boundary;
  `unique_ptr<T>` transfers only when `T` does; no type is shareable merely
  because C++ can copy it.
- **Exit gate:** focused generic/nominal/recursive positive and negative tests,
  thread-affine cleanup and opt-in/opt-out cases, deterministic diagnostics,
  and no backend name recognition.
- **Evidence:** `SemanticTypeTraits`, `ClassTypeInfo`, lambda/binding records,
  and concrete HIR class instances retain the independent facts. Public
  `std::transferable`/`std::shareable` concepts query them. Declaration
  attributes implement safe negative policy, interface requirements, and
  unsafe positive assertions; `GTI-S2059`, formatter, Tree-sitter, hover, and
  semantic-token regressions cover the source/tooling boundary. Compiler tests
  cover primitives, arrays, expected alternatives, concrete generics,
  recursive unique ownership, borrowed views, raw pointers, cleanup, native
  handles, interfaces, and callable captures.
- **Unlocks:** atomics and thread APIs.

### C-GLOBAL-01: Global And Static Shared-State Policy

- **State/role:** done; adopted concurrent-profile policy.
- **Scope:** Apply the adopted policy to namespace globals and static class
  fields, including aliases, generic instances, native wrappers, cleanup-owning
  types, and internal linkage. Current semantics permits mutable globals, so
  diagnostics and migration notes must make any new restriction explicit.
- **Non-goals:** public threads, atomics, mutexes, a target `threads`
  capability, or a second initialization mechanism. D-EXEC-01's single
  program-initialization contract remains normative; M-LIFE/M-EXEC and the
  matching backend migration still own its executable schedule and the
  general single-threaded cleanup-owning-global restriction.
- **Exit gate:** safe source cannot create unsynchronized shared mutable global
  state; immutable/shareable values and approved synchronization wrappers
  follow one initialization-before-entry rule.
- **Evidence:** `TargetInfo::executionProfile` is selected explicitly by
  direct `--execution-profile` or a project profile's `execution-profile`
  field before frontend entry and is retained in `SemanticModel`, HIR, and
  MIR. `GTI-S2060` rejects mutable or non-share-capable namespace globals and
  static fields only in the concurrent profile, with nominal opt-out,
  cleanup, base, stored-reference, and field causes when available. Compiler,
  project-model, direct-CLI, and project-CLI tests cover aliases, concrete
  generics, raw/borrowed state, native-like opt-outs, cleanup, internal
  linkage, default compatibility, and explicit overrides.
  Concurrent selection creates no alternate initializer path; accepted values
  remain subject to the one D-EXEC-01 initialization-before-entry contract.
- **Unlocks:** the global-policy prerequisites of `C-ATOM-01` and
  `C-THREAD-01`; their MIR, runtime, lifetime, failure, callable, and backend
  prerequisites remain.

### C-MIR-01: Synchronization Operations And Effects

- **State/role:** done in 0.133.0; systems-readiness concurrency foundation.
- **Scope:** Represent atomic and synchronization operation categories in HIR
  and MIR, including the ordering dimensions selected by the memory-model
  decision. Extend the existing conservative operation-effect table first.
  `O-MIR-02` remains the sole owner of per-function summary computation and
  consumes these facts whenever that row is done. Unknown/native/user-code
  calls remain synchronizing until proven otherwise.
- **Non-goals:** reordering optimizations, LLVM IR atomics, or weakening the
  current conservative effect table.
- **Exit gate:** exhaustive enum/effect tables, HIR/MIR verifier tests, and
  optimizer tests proving synchronization operations are never speculated,
  removed, or reordered. Summary propagation tests remain owned by `O-MIR-02`.
- **Evidence:** `SynchronizationOperation` is carried from HIR calls into MIR
  v14 with thread spawn/join, atomic load/store/RMW/compare-exchange, and mutex
  lock/unlock identities. The MIR verifier enforces operation-specific order
  legality and rejects represented operations under the single-threaded
  profile. The completed public thread slice later added the narrow dormant
  trusted-wrapper exception required by the complete source-library inventory;
  it accepts only exact private thread intrinsics and never a task adapter.
  Exhaustive names/effects and mutation tests cover propagation,
  malformed orders, misplaced metadata, profile rejection, and optimizer
  barriers. No public wrapper or backend lowering was introduced.

### C-ATOM-01: Sequentially Consistent Atomic Scalars

- **State/role:** done; `C-MIR-01`, `C-GLOBAL-01`, and the target thread
  capability substrate in `C-RUNTIME-01` are complete. Exact backend lowering
  and the public source-defined wrapper now close the systems-readiness slice.
- **Scope:** Introduce an ordinary GTI standard-library atomic wrapper over a
  closed scalar allowlist with construction, load, store, exchange, and one
  compare-exchange shape. The first executable contract is sequentially
  consistent and makes no lock-free or stable-layout promise.
- **Non-goals:** relaxed/acquire/release spelling, atomic references,
  wait/notify, floating atomics, or arbitrary trivially-copyable classes.
- **Exit gate:** semantic type/capability tests, exact HIR/MIR operations,
  native multithreaded runtime tests through a controlled harness, TSAN where
  available, and optimizer barrier tests.
- **Unlocks:** `C-ORDER-01` and safe synchronized shared-state building blocks.
- **Completion evidence:** `<std/atomic>` provides move-only
  `std::atomic<T>` for `bool` and every fixed-width signed/unsigned integer,
  with construction, load, store, exchange, and strong compare-exchange. Exact
  declaration identity selects the compiler-private storage capability and
  intrinsics; semantic analysis, HIR, MIR verification, representation
  sealing, and the C++ backend all validate the operation and SC order. The
  `atomic_semantic_ir`, `atomic_end_to_end`, `atomic_lsp`, `atomic_tsan`,
  optimizer, corpus, standard-library surface, and installed nightly/release
  checks cover domain rejection, private forgery, target rejection, strong-CAS
  observed values, modification order, a deterministic SC store-buffer
  litmus, O0/O3, C++20/C++23, and ThreadSanitizer where supported.

### C-CALL-01: Thread-Task Transfer Contract

- **State/role:** initial exact `void()` task slice done in 0.309.0; exact
  owned-result transport completed in 0.313.0.
- **Scope:** Bind the GTI-owned callable representation to one consumed task
  shape. Prove transfer/share capabilities for the callable and every capture,
  record exactly-one invocation and task-entry metadata in HIR/MIR, and define
  return/failure behavior without adding a second callable representation. The
  implemented shapes are consumed `void()` task objects and exact
  context-required `Result()` task objects supported by the existing callable
  foundation; general callable storage and broader capture ergonomics do not
  block them.
- **Non-goals:** general callable storage, additional capture syntax, type
  erasure, implicit reference capture, coroutine frames, or native callback
  ABI; those remain owned by `L-CALL-01` or `S-CALL-01`.
- **Exit gate:** accepted task values transfer and drop exactly once; borrowed
  or non-transferable captures cannot enter a task; generic invocation retains
  exact calls and conservative effects.
- **Completion evidence:** `CallableOwnedTransportKind::ThreadTask` extends the
  existing owned-callable contract only for an exact private spawn identity.
  Fresh or explicitly moved exact `void()` and context-required `Result()`
  lambdas and nominal callables are accepted; borrowed, raw-pointer,
  non-transferable, mismatched-result, parameterized, copied named, and
  repeated-use tasks fail with `GTI-S2084` or ordinary move diagnostics.
  HIR/MIR retain one exact once-invocation adapter, exact result type, and
  optimizer synchronization barrier per spawn.

### C-THREAD-01: Joined Owned Thread With Owned Arguments

- **State/role:** initial source-defined owned thread slice done in 0.309.0;
  public `std::thread` spelling and `std::thread_result<Result>` completed in
  0.313.0.
- **Scope:** Make ordinary source-defined `std::thread` GTI's normal public
  managed-thread model. Move a transfer-capable callable and arguments into the
  thread, define join result/failure, and make handle destruction automatically
  join under the adopted contract. Do not add a separate C++-style
  detach-capable `std::thread` or a detach operation. References cannot cross
  unless a later scoped-borrow proof is designed.
- **Exit gate:** move-only argument transfer, rejection of borrow/raw/global
  violations, join cleanup on every normal edge, original-record re-raise
  distinct from recoverable native join error, runtime stress, and sanitizer
  coverage. Automatic join has the same re-raise behavior as explicit join.
  Initialization sequenced before spawn must happen-before task entry, and task
  completion must happen-before a successful join.
- **Unlocks:** owned thread convenience APIs and later scoped borrowing.
- **Completion evidence:** `<std/thread>` owns move-only handle policy,
  recoverable `start`/`join`, `joinable`, destructor autojoin, and the typed
  `std::thread_result<Result>` owner in ordinary GTI source. Exact
  HIR/MIR/Lowered task adapters and verified-MIR C++ lowering preserve
  spawn/join synchronization, once invocation, exact result transport,
  explicit result take or automatic abandon, creation cleanup, reverse worker
  cleanup, exact ordinary failure re-raise, and `GTI-R0014`. Direct/project and
  C++20/C++23 O0/O3 matrices cover noncapturing lambdas, owned move capture,
  nominal callables, handle movement, typed named and closure tasks,
  explicit/automatic join, deterministic result cleanup and failure output,
  and repeated spawn/join stress.

### C-RUNTIME-01: Target And Runtime Thread Capability

- **State/role:** done for the shared thread runtime capability; managed
  threads, atomics, and mutexes retain their own operation rows.
- **Scope:** Add an explicit target/runtime `threads` capability, private
  owning handle, generated task-entry thunk contract, thread-safe runtime
  diagnostics/allocation, fixed failure-record handoff, platform linkage, and
  early unsupported-target diagnostics. Classify existing output, file,
  socket, native-handle, and other
  hosted services for thread safety, output interleaving, handle transfer, and
  synchronization effects before public tasks can invoke them.
- **Non-goals:** public C function pointers, detach, or importing pthread types
  into GTI source.
- **Exit gate:** installed-toolchain runtime/library smoke tests pass on every
  platform that advertises the capability, captured worker records survive
  handoff without allocation or native unwinding, and unsupported targets fail
  before backend invocation.
- **Completion evidence:** `TargetInfo::RuntimeCapabilities::threads`, the
  concurrent profile check, identity-bound private handle operations, the
  version-one opaque C runtime ABI, native platform thread linkage, and exact
  ordinary/emergency worker record handoff are implemented. Runtime fault seams
  prove creation cleanup exactly once and recoverable join-state preservation;
  stdout calls are serialized, task capture never observes/reports on the
  worker, and installed nightly/release workflows run the public end-to-end
  matrix.

### C-SYNC-01: Mutex And Guard-Tied Mutable Access

- **State/role:** done; `C-THREAD-01` and the bounded `M-OWN-03` owner-tied
  carrier are complete.
- **Scope:** Model a mutex guard as an owner-tied capability whose lifetime
  gates mutable access. Integrate failure cleanup according to `D-FAIL-01`.
- **Non-goals:** poisoning, public manual unlock, recursive/read-write mutexes,
  fairness, or condition-variable wait/reacquire.
- **Exit gate:** unlock occurs exactly once on every edge; protected access
  cannot escape the guard; two threads pass race/stress tests.
- **Completion evidence:** source-defined `std::mutex<T>` and move-only,
  non-transferable `std::mutex_guard<T>` use sealed private storage/token
  capabilities. Semantic owner dependencies exclude relock/owner move and
  prevent temporary/local guard loans from escaping. HIR/MIR retain exact
  lock/unlock identities, guard-read loan origin, move transfer, and reverse
  cleanup. `mutex_semantic_ir` includes verifier mutations;
  `mutex_end_to_end` covers C++20/C++23 at O0/O3, move-only payloads,
  contention, and unlock-before-failure-propagation; `mutex_tsan` covers the
  shared counter on supported hosts. No poisoning is introduced: deterministic
  cleanup preserves the primary GTI failure and permits a later lock.

### C-ORDER-01: Explicit Atomic Memory Orders

- **State/role:** done; the validated `C-ATOM-01` client is complete.
- **Scope:** Add only the orderings accepted by `D-MEM-02`, with operation-
  specific legality and happens-before tests. Keep sequential consistency the
  simple default.
- **Exit gate:** every accepted load/store/RMW/CAS order has semantic and MIR
  legality coverage plus deterministic message-passing/litmus tests; forbidden
  combinations fail before backend invocation.
- **Completion evidence:** distinct source-defined
  `std::memory_order_relaxed`, `memory_order_acquire`,
  `memory_order_release`, `memory_order_acq_rel`, and `memory_order_seq_cst`
  tag types make operation legality an overload property rather than a runtime
  enum branch. Default operations remain SC. One-order strong CAS derives its
  standard failure order; the two-order form exposes exactly nine legal pairs.
  `atomic_semantic_ir` proves the complete HIR/MIR matrix and forbidden source
  combinations, optimizer and backend checks preserve/map exact metadata, and
  `atomic_end_to_end` proves release/acquire publication plus every order at
  O0/O3 under C++20/C++23. `atomic_tsan`, LSP, stdlib surface, corpus, and
  installed nightly/release checks cover the remaining boundaries.

### C-ATOM-02: Integer Fetch Read-Modify-Write Operations

- **State/role:** done; `C-ATOM-01` and `C-ORDER-01` provide the sealed atomic
  identity/order substrate, and production generic callable emission supports
  the source-defined update loops.
- **Scope:** Add `fetch_add`, `fetch_sub`, `fetch_and`, `fetch_or`, and
  `fetch_xor` to every fixed-width integer atomic, with every accepted
  read-modify-write order. Keep `bool` outside the integer-only family.
- **Required invariant:** each operation returns the value before its successful
  update. Add/subtract use defined checked-overflow failure before publication,
  rather than inheriting C++ wrapping. Public policy remains an ordinary
  source-defined strong-CAS loop with exact MIR synchronization metadata.
- **Exit gate/evidence:** the complete operation/order matrix and bool negatives
  pass semantic/HIR/MIR verification; O3 preserves every load/CAS barrier;
  C++20/C++23 O0/O3 runs prove prior-value behavior and defined overflow;
  concurrent increments, LSP discovery, installed stdlib surface, and TSAN
  cover the integration boundary.

### C-CONFORM-01: Concurrency Conformance Gate

- **State/role:** complete for public `std::thread`, ordered scalar atomics
  including integer fetch operations, and scoped mutex guards; active again
  when scoped threads or another public concurrency family is added.
- **Scope:** Add litmus tests, stress tests, TSAN jobs where supported,
  optimizer-ordering tests, native-entry tests, and documentation examples.
  No timing-sensitive test may be the sole correctness oracle.
- **Exit gate:** every public concurrency operation selected by `D-MEM-02` has
  deterministic semantic/IR tests, O0/O3 runtime equivalence, installed-
  toolchain coverage on each advertised target, sanitizer coverage where
  supported, and no unexplained reliance on native C++ ordering.
- **Current evidence:** `managed_thread_semantic_ir`,
  `managed_thread_runtime`, `managed_thread_end_to_end`, `atomic_semantic_ir`,
  `atomic_end_to_end`, `atomic_lsp`, `atomic_tsan`, `mutex_semantic_ir`,
  `mutex_end_to_end`, `mutex_tsan`, `lsp_protocol`, the stdlib surface/corpus
  gates, installed workflows, and sanitizer runs cover the shipped thread,
  ordered scalar-atomic/fetch, and mutex surfaces. Timing is never a
  correctness oracle.

### C-CTHREAD-01: C-Style Threading Library Completion

- **State/role:** deferred outside the current standard-library completion and
  1.0 goal at the user's request (2026-09-24). Preserve and regression-test the
  implemented surface; thread-specific storage and explicit exit/detach are
  not completion gates for this campaign. The existing safe managed-thread,
  atomic, and mutex APIs remain in scope.
  This is a separate C-style surface, not a rename of `std::thread` and not
  completion of a module merely because its include or yield operation works.
- **First executable slice:** `std::thrd_yield()` permits cooperative backoff
  on the initial or a managed worker. It uses existing native-call MIR effects
  and supplies no synchronization edge, resource, failure result, delay, or
  fairness guarantee. Focused runtime and exact-call-retention tests own it.
- **Create/join outcome:** ordinary unsafe wrappers now create one C-style
  worker from a named `int32_t(void*)` callback and join its integer result.
  Exact trusted native identities and resolved call-graph requirements enforce
  concurrent-profile/target admission. HIR/MIR retain registered callback
  attachment, spawn/join synchronization, and hosted startup/shutdown calls.
  Raw context retention and manual join lifetime are explicit unsafe
  obligations. The generated callback terminates with the original defined
  failure after verified cleanup; it does not adopt managed-thread failure
  re-raising. Remaining lifecycle/synchronization families below still prevent
  declaring the module complete.
- **Caller-contract prerequisite:** namespace-scope GTI `unsafe` functions
  make the raw-context/manual-lifetime obligation visible at direct calls and
  forbid erasing it through safe callable conversion. This does not complete
  create/join by itself: attachment, concurrent entry, synchronization,
  shutdown, and lifetime/failure containment are supplied by the integration
  above, not by an unsafe annotation alone.
- **Native runtime prerequisite implemented:** source-private create/join now
  has never-reused handle identities, retained context obligations, integer
  results and join publication, recoverable creation/join errors, exact
  once-only registered callback entry, and initial-thread-owned shutdown that
  closes creation while preserving outstanding joins. The separate archive
  member leaves the existing yield-only boundary unchanged. Focused native
  tests cover repeated/nested/competing joins, stale and self handles,
  unregistered/mismatched/replayed entry, in-flight shutdown, and native
  exception/incomplete-adapter firewalls under C++20/C++23 O0/O1/O3 plus
  ASan/UBSan and TSAN on macOS. The private header is not installed; this
  foundation did **not** itself expose a new GTI operation or relax concurrent callbacks.
  The create/join integration above now connects that foundation through the
  stable C boundary, exact trusted declarations, hosted/callback MIR contracts,
  synchronization verification, and ordinary unsafe wrappers. Focused public
  tests cover result/context publication, nested workers, owned-argument entry,
  callback failure cleanup, unregistered entry, and outstanding-handle shutdown.
- **Local integration evidence:** the public create/join matrix passes at
  C++20/C++23 O0/O1/O3 plus ASan/UBSan and TSAN, including a fresh installed
  compiler with resource overrides removed. The actual runtime implementation
  passes its separate instrumented matrix. All nine installed-library consumers,
  C-thread semantic/MIR/LSP checks, the 3,907-body emitter corpus, and the
  82-example native corpus pass. The new conditional runtime-header dependency
  and textual MIR operation ordinals have focused regressions. These are macOS
  results, not cross-host evidence. Publication remains pending the inherited
  class-allocation regression recorded below; the stdlib campaign is not done.
- **Identity/equality implemented locally:** safe ordinary GTI wrappers now
  expose stable `thrd_current()` and total `thrd_equal()` in both profiles.
  Manual workers observe their exact create handle; initial, managed, and
  foreign native threads share the never-reused identity space without gaining
  callback-entry or manual-join permission. No compiler opcode, new syntax,
  backend public-name recognition, or synchronization meaning is added.
  The 40-case public yield/identity matrix passes at C++20/C++23 O0/O1/O3
  plus ASan/UBSan and TSAN, both in-tree and through a fresh installed compiler
  with workspace resource overrides removed. The actual native runtime passes
  the same eight modes; all nine installed-library consumers pass, including
  the C11 identity/create-handle check. Ten focused regression gates pass,
  including the compiler aggregate, default-profile/no-thread-capability
  identity admission, concurrent-profile forged-join rejection, complete stdlib
  surface run, include sweep, and C-thread editor checks. The independent
  create/join matrix also passes after the shared identity allocator change.
  The stale yield-only README/reference descriptions are corrected; all 68
  page-owned examples, 11 content/highlighting tests, type checking, and the
  single-worker static-site build/render checks pass. These are local macOS
  results, not Linux/Windows evidence or completion of the C-thread/stdlib
  campaign. Full campaign publication and branch reconciliation remain pending.
- **Relative sleep implemented locally:** ordinary `std::timespec` values and
  value/reference plus unsafe pointer `thrd_sleep` overloads use existing
  verified-MIR native calls. Input is snapshotted for in-place retry; only
  interruption publishes a normalized remainder. Invalid duration, null input,
  unrepresentable host time, and host errors return -2 without changing the
  output. Zero duration needs no host wait. No new synchronization, callback,
  compiler intrinsic, or profile requirement is introduced. The separate scalar
  runtime ABI uses one POSIX sleep attempt or a Windows alertable relative
  timer; it never exports native `timespec` layout.
  The local macOS matrix passes eight native configurations and 24 GTI
  executions across C++20/C++23 O0/O1/O3 plus ASan/UBSan and TSAN. It covers real
  host interruption as well as deterministic native/result-publication oracles,
  safe/raw aliasing, null handling, invalid inputs, and managed/manual workers.
  Six focused CTest gates pass, including semantic admission without thread
  creation support, unsafe-pointer rejection, C ABI, manifest, and CMake
  ownership. The same eight-mode sleep matrix passes using a fresh installed
  compiler with all workspace resource overrides removed, and all nine
  installed-library consumers pass, including C11 sleep calls. Seven broader
  regression gates pass: yield/identity, native runtime boundary/firewall,
  create/join, complete stdlib surface, include sweep, and C-thread LSP.
  Reference API generation, type checking, all 11 content/highlighting tests,
  all 69 page-owned examples, and the single-worker static build/render checks
  pass. The real signal-interruption test drains pending signals before
  restoring the caller's handler, and Windows test headers suppress `min/max`
  macros. Windows and Linux execution are not established by these macOS
  results. Full campaign release gates, publication, and branch reconciliation
  remain outstanding.
- **Manual mutex family implemented and locally verified:**
  `mtx_init`/`mtx_destroy`, `mtx_lock`, `mtx_trylock`, `mtx_timedlock`, and
  `mtx_unlock` now cover plain, recursive, timed, and combined flags. Exact
  trusted native identities enforce concurrent-profile/target admission and
  retain acquire/release records through HIR/MIR and detached declarations.
  Program serialization advances to v68; the ordinary native-call body schema
  remains v59. The runtime registry prevents reuse and in-flight retirement,
  checks recursive ownership, handles spurious wakes, and validates exact
  signed-nanosecond UTC deadlines. Output-preserving allocation failure and
  invalid/busy destruction are independently tested. No native `mtx_t` layout,
  public-name intrinsic, automatic manual-resource cleanup, or backend-only
  synchronization semantics are introduced.
  The final eight-mode matrix passes actual native runtime and terminal
  destruction tests plus 24 public GTI runs at C++20/C++23 O0/O1/O3 with
  ASan/UBSan and TSAN, both in-tree and through the installed toolchain with
  resource overrides unset. Deterministic blocked-waiter tests cover ordinary
  and timed acquisition, and injected post-wakeup failure hands notification
  to another waiter without claiming ownership. Semantic/verifier gates cover
  unsafe/profile/target admission and erased/reversed/forged synchronization.
  All 20 selected cross-phase/regression CTests and all nine clean-stage
  library consumers pass; the final wakeup fix was followed by rerunning the
  native/public mutex matrices and all installed consumers. Reference API
  generation, type checking, all 12 content/highlighting tests, all 70
  page-owned examples, and static build/render checks pass. The page-owned
  mutex example selects an
  explicit concurrent execution profile; the reference runner now accepts only
  the two compiler profile values and displays the selected option. These are
  macOS results, not Linux or Windows execution evidence. Full campaign
  release gates, publication, and branch reconciliation remain outstanding.
  Condition variables and the other full-library families below remain incomplete.
- **Manual condition variables implemented and locally verified:**
  `cnd_init`/`cnd_destroy`, signal/broadcast, and wait/timed-wait now have ordinary
  unsafe GTI wrappers over exact private native declarations. Program v69/body
  v60 retain `ConditionWait` and `ConditionNotify` identities and optimizer
  barriers. Waiting atomically enrolls/releases exactly one mutex acquisition
  and reacquires before success, timeout, or recoverable wait-error return.
  Native waiters retain both resources through reacquisition; invalid input
  preserves ownership, notifications are not saved, and in-use destruction or
  failed reacquisition terminates. No scoped-owner or automatic-unlock contract
  is implied. Seven initial focused checks pass, including semantic/verifier,
  optimizer, LSP, C ABI, manifest, native boundary, and the eight-mode native
  plus 24-public-program matrix. The final matrix includes injected terminal
  reacquisition failure and runs both in-tree and through a fresh installed
  toolchain with resource overrides unset. All 21 selected compiler/concurrency/
  stdlib regression CTests and all nine external library consumers pass.
  Reference API generation, type checking, 12 content/highlighting tests, all
  71 page-owned examples, and the one-worker static build/render checks pass.
  Formatting checks cover 335 first-party C/C++ files. These are local macOS
  results; Linux/Windows execution and the remaining full-campaign release
  gates are not established here. No syntax, AST, formatter grammar, Tree-sitter
  grammar, or backend-only execution path was added. Once/TSS and explicit
  thread lifecycle remain unfinished. Publication and branch reconciliation
  remain outstanding, with the canonical host's TLS error recorded below.
- **Once implementation locally verified (2026-09-24):** `once_flag` owns an
  ordinary GTI atomic state machine, with a separate verified callback role for
  `() -> void` initializers. Semantic/verifier, native registration, C ABI, LSP,
  and all eight runtime modes pass locally: C++20/C++23 O0/O1/O3, ASan/UBSan,
  and TSAN, covering 72 public-program executions plus native boundary probes.
  Global `ONCE_FLAG_INIT`, nested distinct flags, initial/manual/managed callers,
  null/foreign/unregistered callbacks, and original-diagnostic terminal cleanup
  are covered. Successful callback return restores managed failure handoff.
  Fresh immobile construction, canonical module-storage views, and passive
  by-value argument staging remain MIR-owned; a later argument mutation cannot
  change the earlier copied value. Checked scalar conversions in nested
  constructed-field initializers remain a separate backend admission gap.
  This is not completion of the C-thread or full standard-library campaign;
  installed/release and other-platform evidence is tracked separately.
- **Following families:** thread-specific storage;
  explicit exit/detach with their adopted ownership and shutdown contracts.
  `thrd_exit` must not call native termination in a way that bypasses GTI's
  verified cleanup schedule. Thread-specific destructors and callback failures
  must not unwind across C or silently discard the original failure.
- **Authority:** public API policy in ordinary GTI; irreducible attachment,
  thread exit, synchronization, and lifetime facts selected by semantics,
  retained through concrete HIR and verified MIR, and consumed by the backend.
  Host handles stay behind a narrow runtime ABI with no public-name intrinsic.
- **Exit evidence:** deterministic invalid-profile/lifetime/entry tests,
  semantic and verifier mutations for every new executable boundary, exact
  normal/failure cleanup, native create/join and mutex/condition clients at
  O0/O3 under C++20/C++23, installed headers/stdlib/runtime, and sanitizer
  coverage where supported. Timing and scheduler fairness are not correctness
  oracles. `C-CONFORM-01` reopens for the new families as they land.

The following are further goals beyond the initial concurrency profile: detached
threads; scoped borrowed captures; recursive/read-write locks; condition
variables before guard release/reacquisition is modeled; cross-thread shared
ownership before shared/weak ownership is specified; pointer atomics before
public pointer provenance; native callbacks before `S-CALL-01`; volatile/MMIO,
signals, coroutines, lock-free guarantees, atomic aggregates, and reclamation
schemes.

## Phase L: Owner-Dependent Library And Language Completion

These rows validate the execution/lifetime foundations in ordinary GTI. Public
containers and algorithms remain source-defined and must not gain compiler
name recognition.

For the 2026-10-04 stdlib review, follow the
[capability adoption campaign](#stdlib-capability-adoption-and-upgrade-campaign)
above. It refines the next implementation prompts using today's source; the
older completion checkpoints below are not an instruction to redo shipped
facilities or to infer that all constructor-pack compositions now pass.

### Active Request: Complete The Existing Standard Library

The forward-list emplacement work is in progress and unpublished. The current
vertical slice adds constructor-local packs and ordinary GTI node construction,
including immovable elements. It repairs plain-enum initializer publication,
unpublished return-loan cleanup, and borrowed bare-local receivers across
conditional scalar arguments. A fallible element is constructed at its final
field address, with success-only rollback publication, rather than assigned
from a temporary. Address-stability and exact success/failure cleanup pass
C++20/C++23 O0/O1/O3 and ASan/UBSan. The final 63-case forward-list runtime
matrix, 10 compiler/optimizer/MIR/backend gates, and the 259-case source-
destructor cleanup matrix pass locally.

This does not yet complete emplacement. Forwarding an owned `std::string`
through the pack to an element constructor taking `std::string&` now retains
the selected reference type and read-borrow role in MIR, rather than moving
the element. The declarations test checks the exact pack projection and rejects
forged move roles and out-of-bounds projections. The retained
`forward_list_emplacement_borrowed.gti` still fails the C++ constructor-field
representation gate; it is not in the passing runtime matrix.
The forwarding audit also repaired a pre-existing ownership defect, reproduced
with the installed 0.338.0 compiler. An unused owning pack previously lacked
explicit MIR drop obligations: native parameter destruction skipped the
caller's cleanup after a destructor failure and could replace an active
failure instead of reporting the required secondary-failure envelope.
Concrete pack elements now retain distinct HIR drop identities, entry-active
canonical MIR lifetime places, and explicit reverse normal/failure cleanup.
Forwarding moves transfer the exact element obligation into a prepared input;
copies preserve it and initialize a separate prepared owner. Repeated
expansions share the entry place without forging expression provenance.
Read-only folds use that same live slot, not a moved native parameter shell.
The original `pack_parameter_cleanup_failure.gti` now preserves the caller's
`9` cleanup on a first failure and reports `GTI-R0014` on a second failure.
The `pack_parameter_runtime` matrix covers empty/single/mixed owning packs,
move forwarding, repeated copied allocation arguments, and repeated borrowed
folds in 56 executions across C++20/C++23 O0/O1/O3 plus ASan/UBSan. Verifier
mutations cover exact element coverage/order/activation, copy provenance, and
unowned projection aliases. Initial broader checks found a copy-input verifier
regression in class allocation; that was repaired, and the compiler,
optimizer, and class-allocation gates passed again.
After the fold-place repair, all 14 selected CTest gates pass locally,
including the 56-case pack, 63-case forward-list, and 259-case destructor-
cleanup matrices. The 12-way build, focused pack lifetime mutations, formatting
check, and `git diff --check` also pass. Full corpus, surface/reference/editor,
and fresh-installed verification remain outstanding for this unpublished
emplacement slice; these focused results are not release-completion evidence.

`pack_constructor_copies.gti` now passes direct `T(args...)` construction
with copied elements through the shared prepared-parameter protocol. All eight
focused C++20/C++23 O0/O1/O3, no-elide, and ASan/UBSan runs produce the expected
repeated copies. This supersedes its prior `GTI-B0001` report; it is additional
focused evidence, not a fresh full pack-matrix run. Allocation through
`std::make_unique<T>(args...)` retains its separate coverage.

The remaining work includes the reference-parameter composition above, the complete constructor-
pack ownership/constraint and mutation matrix, allocation-failure evidence,
surface/reference/editor synchronization, and corpus/installed-toolchain
validation before release. No claim of complete generic forwarding, complete
standard-library coverage, or another-platform execution follows from the
passing scalar and immovable-element cases.

The storage-swap slice implements vector, string, and forward-list swap in
ordinary GTI and makes path swap delegate to string. Receiver-local replacement
helpers avoid consuming borrowed references. The C++ backend now preserves
private storage return handles and exactly staged member-call class results
through their existing MIR return/replacement/cleanup schedules. General
swap/exchange, array swap, and the remaining ownership/container families below
are still incomplete; this checkpoint does not close the campaign.
Local macOS evidence includes the seven-mode build-tree and fresh-installed
swap matrix, zero-allocation observations, exact normal/failure cleanup,
borrow rejection, and malformed backend result-record rejection. The full
224-test CTest run plus focused reruns validates 223 gates in the working tree;
the project workflow passes in an isolated HEAD snapshot with this stdlib
overlaid because the working tree's game-engine example was user-deleted.
Three pre-existing generated-C++ assertions reproduced on 0.337.2 and now
check the existing cleanup-before-publication success/rollback paths explicitly.
The reviewed corpus adds six helper bodies without changing generated-item
counts; all 82 examples pass both endpoints. All 72 reference examples,
static-site checks, and nine fresh-installed library smoke tests pass (the
system-LLVM install uses its matching Homebrew LLVM CMake package).
No Linux/Windows execution is claimed.

The numeric-boundary repair completes the existing gcd/lcm/midpoint operations'
edge semantics in ordinary GTI: signed inputs, zero lcm, full-width midpoint,
floating overflow/subnormals, and unrepresentable-result cleanup. It also
corrects the existing MAX_UINT64 constant. The independent boundary oracle,
surface fixture, and failure matrix own this evidence; this is not completion
of the remaining ownership, container, iterator, utility, or hash families.
Local macOS validation passed all seven numeric configurations in both the
build tree and a fresh installed toolchain, 20 selected compiler/library
CTest gates, the CLI workflow, and all 72 reference examples plus the static
site checks. The project workflow passed in an isolated HEAD snapshot with
the numeric changes overlaid; its original worktree run could not find the
user-deleted game-engine example. Linux/Windows execution is not claimed.

The current user-requested campaign includes **every operation already present
in the shipped stdlib, except further C-style thread completion**, not merely
the executable entries in the surface manifest. Declaration-only functions,
placeholder constructors, and currently
unsupported compositions remain completion work. The manifest is an evidence
ledger, not permission to exclude a declared API. Required language capabilities
must ship through verified MIR with their real library clients.

The initial remaining families are generic swap/exchange and container swaps;
safe linked-node traversal and the declared forward-list/list operations;
shared/weak ownership; the remaining iterator/algorithm forms; primitive hash
implementations; and any incomplete declared string, optional, span, and other
library operations revealed by source-by-source review. Existing placeholder
bodies require behavior tests, not just successful linkage. This campaign does
not claim completion from an include-only or manifest-only pass.

**Current scope override (2026-09-24):** the user removed further C-style
thread work from this goal because it is not a core 1.0 requirement. This
supersedes the earlier request to complete `stdlib/std/cthread.gti` and the
historical checkpoints below that list full C-thread completion as remaining
campaign work. Do not remove existing APIs, tests, or their safety contracts;
their regressions still matter. Defer TSS, detach, and exit under
`C-CTHREAD-01` without claiming that module complete. The safe managed-thread,
atomic, and mutex APIs are unaffected. Prioritize the core ownership,
container, iterator, algorithm, and utility families listed above.

The first local executable slice adds `std::thrd_yield()` for cooperative
scheduling/backoff in ordinary or managed code. It has no resource, failure
result, synchronization edge, delay, or fairness guarantee. Both execution
profiles use ordinary verified-MIR native calls over a narrow runtime entry;
this does not add a thread-creation or callback permission. Its initial and
managed-thread tests and exact native-call oracle pass C++20/C++23 at
O0/O1/O3 plus ASan/UBSan, including the complete matrix against a clean installed
toolchain. The managed fixture also passes ThreadSanitizer. All 35 stdlib
surface cases, the include sweep, direct CLI workflow, eight installed-library
consumers, 66 reference examples, content/type checks, and single-worker static
site build/render checks pass locally on macOS. Other host platforms still
need their own execution evidence. Subsequent create/join, identity, sleep,
mutex, condition, and once evidence is recorded in `C-CTHREAD-01` above.
Explicit lifecycle and thread-specific storage are still unimplemented but,
under the current scope override, no longer block this campaign. Yielding
alone was not module completion.

Binary32 mathematics is the first completed slice: all 18 previously
bodyless `<std/cmath>` functions now have ordinary GTI wrappers over a scalar
host ABI. Local macOS evidence covers `cmath_runtime` at O0/O1/O3 under
C++20/C++23 plus ASan/UBSan, all 32 stdlib surface cases, C header compatibility,
installed-toolchain execution and seven installed-library checks. All 65
reference examples and the production reference-site build/render checks pass.
Canonical meaning is in Standard Library §7.3.1. No new public-name intrinsic
or AST/HIR backend path is introduced; other host math libraries still need
their platform test runs.

The same request requires reconciliation of outstanding branch work before
branch cleanup. A verified full-history bundle is retained locally at
`out/stdlib-branch-history-20260918.bundle`. Thirty-five local branch tips
already contained in `main`, and 88 remote branches already contained in
`origin/main`, have been removed. Their commits remain reachable from `main`
and the backup. Ten more remote branches with zero unique patches and no
unmerged merge commits were removed after exact patch-equivalence checks;
their original tips remain in the backup. Twenty-one additional remote branch
names were removed after reviewing the integrated/reconciled patches, checking
their exact tips against the bundle, and publishing the coverage merge below.
Ten further remote names were removed after reviewing their mainline
replacements, confirming each exact remote tip against the bundle, and
retaining the one missing conditional-reference-return regression in
`c321e93c`. That expanded runtime matrix passed all nine cases under C++20
and C++23 at O0 and O3. The reviewed mappings are:

- Inline capturing-lambda arguments and symbolic `auto` locals: `d3ee9e34`,
  preserving canonical MIR dominance and per-instance semantic type ownership.
- Reference-parameter returns: `f2d9d9f0`, with conditional-return coverage in
  `c321e93c`; MIR publishes a fresh return loan instead of backend repair.
- Mutable-reference retained chains: `871ecbec`.
- Nested subscript reference forwarding: `3355ab56` and the mainline
  call-result-to-return-loan publication path.
- Prvalue assignment to bindings, array elements, and mutable referents:
  `56848951`, `be0be7eb`, and `1f485c49`.
- Public allocator storage: `7f25d552`, preserving the stricter mainline
  reference-return producer and access checks.
- Forward iterators and callable/movement iterator capabilities: `39f483fb`
  and `d59f9c0d`.
- Owning payload-enum callable transport, its contract, and admission tests:
  `f7a7085c`, `a3eceb69`, and `7e70e171`.

The local `merge-c04` tip was also removed after proving its tree and both
parents exactly match the published merge `001ac978`, with no additional
conflict-resolution changes. Its original `06452d83` tip remains in the bundle.
The class-scoped alias branch was also removed after matching its handwritten
implementation, grammar, and tests to `c1fbca4b`; generated parser differences
belonged to its older grammar baseline. Its `347e2901` tip remains in the bundle.
At the former GitHub remote, the cumulative cleanup was 36 local and 130
remote branch names, with eight remote topic branches still to reconcile.
These are historical counts, not an audit of the subsequently selected Gitea
remote; `main` remains the only local branch. The
repository-wide pointer/reference-formatting branch requires a style decision;
its different formatting policy is not assumed to be redundant.
The outstanding tests-only stabilization branch was reviewed and merged into
`main` as `e9b0848d`. Its opt-in matrices were syntax-checked; the retained
findings describe the historical compiler they audited, not a current
acceptance baseline. Production compiler and runtime code were unchanged by
that merge.

The canonical remote is now
`https://git.internal.howetotech.dev/gti-lang/gti.git`. Its prepared
`codex/gitea-primary-migration` branch is integrated with the newer stdlib
checkpoints, retaining their changes and advancing the installer/release fix
to 0.333.1 instead of restoring the branch's older 0.331.1. Gitea owns release
dispatch; the former GitHub remote is no longer a publication destination.
The Gitea topic branches require their own reachability and patch review before
cleanup. Their presence is not evidence that the previous cleanup was undone.
The 0.334.1 publication includes the first Gitea cleanup batch: 20 topic tips
are ancestors of the validated `main`, and 11 more have zero unique patches
and zero unmerged merge commits. All 31 exact tips are retained in the verified
complete-history backup `out/stdlib-gitea-history.OFwGKI/branches.bundle`.
Publication pairs their deletion with the checkpoint in one atomic push,
guarding each deletion against its reviewed tip; the remaining 32 topic refs
required the individual reconciliation recorded below. `main` is the only
local branch.
The hosting, release-asset synchronization, release-version policy, CMake test
ownership, C ABI, and stdlib manifest gates pass. Workflow YAML and embedded
shell syntax checks pass, as does the complete Neovim installer/editor smoke
against a fresh macOS system-LLVM 0.333.1 archive. The latter exposed and fixed
a test setup assumption: regex highlighting must be explicitly enabled while
checking the fallback after Tree-sitter has disabled it. Both highlighting
assertion sets remain intact. Remote Actions and other native runner platforms
are not local validation evidence, and publication does not wait for them.

The second Gitea audit reconciles 31 of those 32 remaining topic refs against
`f26f304b`. All of their unmatched commits are accounted for below, their
mainline replacements are ancestors of `main`, and none contains an unreviewed
merge commit. Every exact topic tip is present in the same verified
complete-history bundle. The table records original branch tips, not new
cherry-picks: restoring their old `VERSION`, generated-parser baseline, corpus
counts, or superseded lowering would regress the reconciled mainline.

| Gitea branch | Reviewed tip | Mainline disposition |
| --- | --- | --- |
| `claude/callable-range-capabilities` | `5e571602` | `d59f9c0d`, following the iterator foundation in `39f483fb`; current corpus baselines retain subsequent features |
| `claude/fix-class-move-assignment-emission` | `619f0a49` | `90f604d9` |
| `claude/fix-cross-argument-borrow-conflict-ice` | `b4973a56` | `b4331a04` |
| `claude/fix-disjoint-field-projection-emission` | `6ff0c62c` | `e697f71e` |
| `claude/fix-field-through-subscript-store` | `0c8180a5` | `634e5181` |
| `claude/fix-implicit-destructor-field-drop` | `13100a1a` | `98fe9258` |
| `claude/fix-inline-capturing-lambda-argument-ice` | `16542303` | `d3ee9e34`; uses canonical dominance/loop analysis rather than a separate reachability implementation |
| `claude/fix-mixed-capture-template-representation` | `143047d8` | `870ffd80`; differing aggregate test-count comments are historical |
| `claude/fix-moved-loop-local-lifetime-rearm` | `6b894d9f` | `a160fcef` |
| `claude/fix-multidimensional-dynamic-index` | `68050e9d` | `ba09844b` |
| `claude/fix-mut-cursor-read-receiver-loans` | `29b3f857` | `9e02ffd7` |
| `claude/fix-mut-reference-retained-chain` | `9caac444` | `871ecbec`; preserves the exact returned address and parent-loan lifetime |
| `claude/fix-nested-borrow-argument-loan-ordering` | `4adfa833` | `b8d7f5b0` |
| `claude/fix-nested-subscript-loan-emission` | `000192dd` | `3355ab56` plus the existing MIR-owned call-result/return-loan transfer |
| `claude/fix-owner-tied-local-by-value-argument` | `9ff1b05b` | `95d86778` |
| `claude/fix-owner-tied-reference-argument-loan-emission` | `4e983389` | `388b50ca` |
| `claude/fix-prvalue-assign-into-projection` | `7582c5e3` | `1f485c49`; predecessor fixes `92536d53` and `e673b595` are retained by `be0be7eb` and `56848951` |
| `claude/fix-prvalue-assignment-into-binding` | `dfd28067` | `56848951`; this older Gitea mirror tip lacks the later checked-array/same-block refinements |
| `claude/fix-readonly-reference-binding-argument-return` | `1c966f1f` | `f2d9d9f0` |
| `claude/fix-reference-parameter-return-loan` | `3b6c202a` | Superseded by fresh MIR return-loan publication in `f2d9d9f0`; conditional-return regression retained in `c321e93c` |
| `claude/fix-retained-borrow-result-place-typing` | `f7a4e4ba` | `5ff8bbf5` |
| `claude/fix-silent-fixed-array-oob-write` | `e477e7b3` | `1b9795f4`; MIR schema advanced over intervening changes instead of restoring the branch's old version |
| `claude/fix-symbolic-auto-local-ice` | `b40308a6` | `d3ee9e34`; concrete generic types remain instance-owned, not written into shared declaration symbols |
| `claude/l-op-01-domain-operators` | `e9933beb` | `e56abf6b`; preserves the already-expanded iterator constraint inventory |
| `claude/payload-enum-ownership` | `d09e1035` | `1df1f893` |
| `claude/pr-queue-merge-order` | `9713f1db` | Historical ordering for the old 0.321.0 PR queue; retained in the bundle, not imported as a current implementation plan |
| `codex/class-scoped-using` | `347e2901` | `c1fbca4b`; handwritten implementation, grammar, and tests retained against the current generated-parser baseline |
| `codex/fix-gitea-actions-deps` | `c245b488` | `4ceaca7d` and subsequent canonical-Gitea workflow changes retain Clang and sanitizer runtime packages |
| `codex/owning-payload-enums` | `e92a82ff` | `87e1d602`, followed by field containment in `3325d94d` |
| `codex/payload-enum-transport` | `8420baa5` | `7e70e171`; predecessors `a1d93c26` and `ee378508` retained by `a3eceb69` and `f7a7085c`, including the existing function-item exemption |
| `codex/public-allocator-storage` | `431d1cd7` | `7f25d552`; retains stricter return-loan producer, access, and carrier checks |

The old queue's verifier warning remains relevant: a builder and verifier
sharing a predicate can agree on an incorrect rule. Independent malformed-IR
mutations and observable runtime results remain required evidence; structural
assertions alone previously preserved the invalid entry/return-loan shape.

Local validation passes all 37 selected CTest gates: the 141-case compiler
aggregate, optimizer and MIR emitter contracts, the affected ownership,
reference, assignment, iterator, allocator, domain-operator, and payload-enum
runtime matrices, LSP, query consistency, and hosting/release policy checks.
Native regressions cover C++20/C++23 at O0/O3. The focused Tree-sitter
class-scoped alias corpus case also passes against the current parser. Checks
run serially at low priority. This is a documentation-and-ref cleanup: the
compiler corpus, installed toolchain, and reference-site matrices from the
0.335.0 checkpoint are not repeated, and no other-platform or CI result is
claimed.

Publication pairs these 31 deletions with this audit record in one atomic push,
with an exact expected-tip lease for each deletion. No compiler behavior changes
in this checkpoint, so `VERSION` remains 0.335.0. The sole retained Gitea topic
is `codex/engine-embedding-sequence` at `827fe75b`: it contains genuinely new
engine ABI design and dormant compiler/driver roots, not redundant stdlib work.
It remains intact pending the user's integration decision and its own review
and validation. This cleanup does not complete that integration or the stdlib
campaign, including the explicitly requested full C-style thread API.

The 2026-09-22 condition-variable pass could not refresh this branch audit:
`git fetch origin` and a separate HTTPS HEAD request to the configured Gitea
host both failed during TLS negotiation with `tlsv1 unrecognized name`, before
authentication. No TLS verification override, remote rewrite, push, or branch
deletion was performed. The retained local tracking ref still contains four
unmerged engine-embedding commits, but is not evidence of current server state.
Local stdlib implementation and validation continue; remote reconciliation and
publication need a working canonical HTTPS endpoint.

The 2026-09-24 publication follow-up uses the user's replacement canonical
endpoint, `https://git.internal.howetotech.dev/gti-lang/gti.git`.
Fetching now succeeds and the destination was reconciled before publication;
the retained engine-embedding branch remains untouched. Repository CI fixes
cover build-tree resource discovery in release tests, Linux linker wrapping
for the C-thread yield oracle, the scalar CFG `char8_t` expectation, and bounded
compile/test worker counts. ASan/UBSan preflights fail early on incompatible
runner hosts. The reported Linux TSAN mapping failure is now represented as an
explicit runner capability: the generic Docker workflows disable TSAN
execution, mixed matrices retain ordinary and ASan/UBSan coverage, and
dedicated TSAN CTests report skipped rather than passed. A compatible dedicated
runner must pass the TSAN preflight and enable the CMake option before its
result counts as race-checking evidence. No host configuration or security
policy was changed. The supplied runner's capacity four and three-hour timeout
also need operator review (recommended capacity one and six hours).

The local macOS Neovim installer stages a checksummed toolchain through the
existing plugin installer and writes a backed-up local Lazy specification.
Its unit checks and dry run passed; its end-to-end installation was not run,
at the user's request. The serial Release build, focused C-thread/CLI checks,
macOS sanitizer preflights, formatting, and workflow syntax checks passed.
The broader serial CTest run passed its first 90 of 217 selected tests before
being stopped to wrap up at the user's request; this is not a complete-suite
pass. Fresh installed-consumer and reference-site gates were not repeated in
this follow-up. These delivery fixes do not complete the stdlib campaign or
establish a successful Linux CI run.

The temporary-receiver/backend failure reproduced by constructing a path and
calling an owning-string-returning method in one expression has a local repair
admitting the existing MIR constructor-argument transfer when the outer result
is a verified temporary receiver or read-borrowed argument, not only a named
binding. The production path remains verified MIR; no source syntax, ownership
rule, or AST/HIR emission path changes. The focused regression covers original
owning-string output, read/mutable receiver and read-borrowed argument forms,
success activation, exact transfer, and normal/failure cleanup. Focused and
corpus checks pass. The broad CLI workflow timed out twice with the Debug
compiler, then passed in 92.34 seconds with the optimized compiler; that
validation prerequisite now passes. All 12 selected compiler/MIR/runtime/project
checks pass with the optimized compiler, including the new sanitizer matrices.
Isolated C++ emission of the reported path program measured 0.67 seconds with
a locally optimized compiler versus 6.48 seconds with Debug, with byte-identical
output. These are single-run local measurements, not a benchmark guarantee or
evidence of a crash cause. Validation now uses one low-priority job at a time
and the optimized compiler; broad Debug suites are not being restarted.

The indexed string constructor also had a silent placeholder body. Its local
source-defined repair copies `[begin, end)` after checking both interval
ordering and the source extent, and does not retain the source borrow. The
focused C++20/C++23 O0/O1/O3 and ASan/UBSan matrix passes independent copies,
empty/end ranges, embedded zero and UTF-8 code units, source-lifetime
independence, and seven invalid-range cleanup cases. The checked-in surface
ledger now includes the constructor and its executable fixture. All 33 stdlib
surface cases pass against the intended-file snapshot; all 65 reference
examples, 11 content tests, registry type checks, and the single-worker static
site build/render checks pass. That snapshot did not count the then-empty
`cthread` module as implemented. These two fixes complete one checkpoint, not
the full standard-library campaign or the separate constructor rollback work.

The broader failure tests exposed a separate incomplete composition, retained
as `tests/fixtures/temporary_constructor_argument_user_drop_gap.gti`: a
fallible constructor owning a field with a user-written destructor. The
backend's aggregate rollback representation cannot simply admit this shape:
it needs correct field construction state and explicit failure-aware cleanup
that retains the original failure if field destruction also fails. The
body-ordered move-parameter initializer proof also still requires a generated
empty-state owner; an enclosing source destructor plus a moved owning field
and later constructor failure needs its own coherent extension. These remain
language-completion work, not reasons to narrow the campaign's scope.

The forward-list checkpoint implements both `resize` overloads, `reverse`,
stable `merge`/`sort`, `remove`, and adjacent `unique` in ordinary GTI. They
detach and reattach exclusive nodes without retained traversal aliases;
clear and ordinary destruction drain iteratively. This also repairs missing
member-restoration ownership events, checked-arrow store projection, bounded
local/loan owner-field replacement, and mixed passive/active node-constructor
publication. MIR itself proves when a moved parameter's explicit shell drop
cannot invoke its source destructor. Zero-argument class-field construction
can precede an independently proved generated empty owner default; source
calls and observable defaults do not gain that representation permission.
The 23 focused compiler/MIR/optimizer/runtime gates pass locally, including
scalar, move-only, and destructor-bearing defaults, stable ordering, 30,000-node
cleanup, and defined-failure cleanup at O0/O1/O3 under C++20/C++23 plus
ASan/UBSan. The 18 broader stdlib/CLI/constructor/inventory gates pass, including
all 36 surface cases. The emitter corpus proves 3,907 ready bodies; all 82
examples also build and run with unchanged MIR ownership at the C++20/O0 and
C++23/O3 endpoints. Eight clean installed-library consumers and the complete
installed list runtime matrix pass. All 67 reference examples, 11 content
tests, type checks, and the single-worker static build/render checks pass.
The full optional language audit also passes: 24 generated differential
programs, four generated compositions, 100 malformed-source mutations, 83
public examples in four configurations, and the paired C++ comparison suite.
These are local macOS results, not evidence of asynchronous CI or other host
platforms. No syntax, token, query, or native ABI changed; grammar/editor
contracts remain on their existing paths. Iteration, emplacement, swap, and
all other remaining campaign families—including the selected C-style thread
API—remain open.

The 0.334.1 owner-access checkpoint replaces the legacy terminal accessor
with a local MIR detector, mandatory failure edge, and success-only reference
publication. The original reproducer now lives at
`tests/fixtures/unique_ptr_empty_access_cleanup.gti` as a passing regression.
Read-only/mutable dereference and arrow access preserve their trusted lexical
site, run ordinary cleanup before `GTI-R0008`, and clean an already completed
right-hand owner if checked destination formation fails. Failure while already
cleaning preserves the primary record and takes `GTI-R0014`. The separate
custom fallible-arrow fixture remains independent coverage, not a substitute
for the real unique-owner path. All 45 selected compiler/MIR/optimizer,
runtime/stdlib, CLI, and inventory gates pass, including all 36 stdlib surface
cases. The strengthened emitter lifecycle assertions also pass after rebuilding.
The new runtime matrix performs 21 builds and 63 runs across C++20/C++23 at
O0/O1/O3 plus ASan/UBSan, against both the build-tree and fresh installed
compiler. Eight installed-library consumers pass. The emitter corpus retains
3,907 ready bodies, and all 82 examples build and run at both C++20/O0 and
C++23/O3 with identical MIR ownership. All 67 reference examples, 11 content
tests, type checks, and the single-worker static build/render checks pass.
The optional audit's 23 contract cases also pass; its generated, composition,
mutation, comparison, and full public-example searches were not repeated in
this checkpoint. These are local macOS results, not CI or cross-host evidence.
No syntax, token, query, or native ABI changed.

The 0.335.0 scalar `make_unique` checkpoint accepts the exact/default primitive
initialization family and explicit plain-enum, null, raw-pointer, and
native-callback values.
The private MIR allocation retains a mandatory status edge, success-only owner
activation, and original-record cleanup propagation. Concrete callback pack
inputs retain their passive native signature through HIR/MIR. C++ representation
also preserves distinct `char`/`uint8_t` generic arguments instead of emitting
colliding native member specializations. Focused verifier mutations, exact
semantic diagnostics, and injected-allocation runtime tests own this boundary.
At that checkpoint, class allocation still used a terminal helper; exhaustion
cleanup and fallible class-constructor rollback remained open. This did not
close the broader stdlib campaign, including full C-style threads and remaining
list operations.

All 45 selected compiler/MIR/optimizer, runtime/stdlib, native ABI, CLI, LSP,
and inventory gates pass after the focused regression reruns. The scalar
runtime matrix performs 21 builds and runs across C++20/C++23 at O0/O1/O3 plus
ASan/UBSan, against both the build-tree and fresh installed compiler. It covers
exhaustion, primary-record preservation during failing cleanup, native callback
transport, and distinct character/byte owners plus lossless byte-alias
conversion. Eight clean installed-library consumers pass. The emitter corpus
retains 3,907 ready bodies, and all 82 examples build and run at both endpoints
with unchanged MIR ownership. All 68 reference examples, 11 content tests,
type checks, and the single-worker static build/render checks pass. The optional
audit's 23 contract cases pass; its broader generated and mutation searches
were not repeated. These are local macOS results, not CI or cross-host evidence.
No syntax, token, query, or public native ABI changed.

The 0.335.1 selected class `make_unique` checkpoint separates allocation from
ordinary constructor entry in verified MIR. It fixes the missing native
constructor call for a fallible pointee and preserves caller cleanup on
allocation exhaustion. Copied and moved class arguments occupy prepared slots
before allocation; constructor entry transfers them only after allocation
succeeds. Raw storage has its own failure cleanup and cannot be borrowed,
aliased, or published as an owner before construction succeeds. Constructor
failure preserves the original record, and allocation failure during cleanup
preserves the primary failure under `GTI-R0014`.

All 65 selected CTest gates pass, including the compiler aggregate, 14 malformed
allocation-protocol mutations, optimizer/backend contracts, all 36 stdlib
surface cases, standalone includes, constructor and owner regressions, CLI,
LSP, and managed threads. The new failure matrix performs 14 builds and 56 runs
across C++20/C++23 at O0/O1/O3 plus ASan/UBSan, using both the build-tree and a
fresh installed compiler. Eight external installed-library consumers pass.
The emitter corpus retains 3,907 ready bodies; all 82 examples build and run at
both native endpoints with unchanged MIR ownership. All 68 reference examples,
11 content tests, type checks, and the single-worker static build/render checks
pass. All compilation and test coordinators ran serially at low priority. The
unrelated project-CLI aggregate was not rerun because it hard-codes parallel
four-target builds; the optional
full language audit was not repeated. These are local macOS results, not CI or
cross-host evidence. Syntax, tokens, language queries, and the public native ABI
are unchanged, so editor grammar regeneration is not part of this slice.

The existing bounded constructor rollback proof remains unchanged. This does
not admit arbitrary user-destructor/subobject rollback, migrate targetless
generated default/copy/move allocation, or complete the full C-style thread API
and remaining standard-library campaign. The separate engine-embedding branch
remains preserved pending the user's scope decision.

The subsequent C-thread validation exposed a concrete remaining allocation
regression in `examples/game-engine/gti`: `std::make_unique<game::App>()`
reached the new split allocation/constructor MIR schedule, but the C++ status
constructor proof rejected its owning `psych::Application` base. The project
CLI gate therefore failed with `GTI-B0001`. Supporting the base's fallible
initialization and rollback was required to restore this existing application;
relaxing the backend gate or silently terminating instead of consuming the MIR
failure edge is not a fix. This prerequisite remained open at that checkpoint;
the bounded base-status follow-up below resolves the reproduced project failure.

Investigation of that prerequisite also found and fixed an existing field
rollback ordering defect: the C++ emitter deferred completed-field `Drop`s
until caller-side representation destruction, after constructor parameters had
already been cleaned. It now consumes the exact MIR field drops in place and
recreates only their proved empty native states. The shared field-state proof
also permits body-ordered owned parameters with a disarmed enclosing source
destructor, without granting an empty-value constructor to that class.
The strengthened class-allocation matrix covers success, exhaustion, failure
after two completed fields, failure before the second field completes, and
the existing secondary-allocation-failure case: 35 runs across C++20/C++23
O0/O1/O3 plus strict ASan/UBSan. The enclosing destructor runs only on success;
completed fields clean in reverse order before earlier-created parameters.
The MIR lifecycle gate checks both generated and declared enclosing cleanup
and rejects a missing lifecycle guard. All 20 selected local gates pass,
including the compiler aggregate, allocation/failure metadata, constructor and
temporary-owner matrices, forward-list cleanup, C-thread create/join, and the
driver. This is field-schedule evidence, not completion of base rollback or
arbitrary fallible subobject cleanup.

The project runner now accepts `--jobs 1`, including its cached multi-target
build; default CTest/CI runs retain the four-job scheduling comparison. A
serial project rerun reproduced the same game-engine failure. The next
compiler step at that checkpoint was explicit base initialization and success-only rollback,
including cleanup before earlier-created parameters. The field replacement
helper cannot be applied to C++ base subobjects. No publication or branch
cleanup has occurred; the fresh installed/corpus evidence recorded above
predates this field fix and must be refreshed before delivery.

The next local slice introduces explicit `InitializeBase` MIR and its
success-only rollback obligation (program v58/body v50). It admits the
parameterless owning-base/cleanup-free-derived-field family needed by
`std::make_unique<game::App>()`, including passive integral-enum defaults.
The C++ backend consumes the exact native base status and does not construct
a standalone base value or apply field placement replacement to a base
subobject. The admitted family has no earlier owning constructor parameters
or owning derived fields, so caller-side destruction of the unpublished
representation finishes the last base cleanup without crossing their order.
Base arguments, owning derived fields, declared base/derived destructors, and
general secondary failure during aggregate cleanup remain separate work.

The constructor-lifecycle issue campaign extends that shared stage to exact
scalar parameter/literal arguments, owning derived fields, and declared
base/derived cleanup, with native-prefix and explicit base-drop proofs. The
five original A45 forms and an unrelated resource-bearing inheritance fixture
are the focused acceptance inputs. The follow-on implements checked/effectful
scalar arguments such as `Base(s + 1)` using a verified pre-native-base partition,
carrying the selected scalar values and original failure record before native
base entry. It preserves argument order, scalar parameter mutations, once-only
global effects, and prefix/base/body failure cleanup without reconstructing
source expressions. This is not closure of the full base-initializer goal:
owned derived parameters need one reference-bound frame with no extra native
moves/copies/drops; owned base arguments need exact `CallInput` transfer and
prepared-drop facts; borrowed arguments need original places/access/loan
endpoints and temporary/escape rejections. Generic owners require exact carrier
identities and declarations. Owning/nested bases require a recursively inactive
shell that suppresses observable defaults until preparation succeeds, retaining
the original typed constructor schedule. Acceptance must cover preparation,
ancestor, field and body failure, primary/secondary records and exact reverse
cleanup without standalone base construction or base placement replacement.
Historical entry-active declaration-field rollback tokens cannot precede a
fallible base; declaration-default construction must use the typed constructor
schedule that arms each field after its successful initializer. The concurrent
general-object-lifetime work owns that schedule, and the two changes must be
reconciled and retested together before broader base-construction closure.

The inherited-allocation runtime matrix passes success, base failure, derived
failure, and exhaustion before base entry across C++20/C++23 O0/O1/O3 plus
ASan/UBSan (28 runs), with exact original failure sites and cleanup traces.
The headless game-engine application now builds and completes successfully,
and the complete project workflow passes with `--jobs 1`. Independent MIR
mutations and emitter tests bind the base identity, constructor, status-only
route, activation, cleanup, retirement, and absence of a field alias. The older
terminal-only derived-constructor expectation is updated to require its new
explicit failure edge. Broader and installed/corpus validation must use the
refreshed compiler before publication; no commit, push, or branch deletion
has occurred in this slice.

Review additionally found that the new base operation was missing from exact
interprocedural effect composition. It now follows the selected constructor,
and the independent mutation matrix rejects a derived summary that hides the
base's allocation effect. The initial broad batch passed 39 of 40 gates; its
only failure was the missing registration of the new lifecycle test in the
emitter manifest, now corrected. Those results predate this effect fix, which
was then rebuilt successfully. All 12 refreshed compiler, optimizer, failure
metadata, lowered-contract, backend-rejection, emitter, and inherited-allocation
gates pass, including all 17 malformed-base mutations and the 28 native runtime
runs. Formatting and patch checks are clean. Installed and corpus evidence
were then refreshed as recorded below.

The refreshed emitter corpus passes all 82 sources and 3,907 ready bodies;
the native corpus passes all 82 examples at C++20/O0 and C++23/O3 with unchanged
ownership. A targeted probe of the documented secondary-cleanup gap then
confirmed an admitted miscompile, retained as
`tests/fixtures/inherited_constructor_cleanup_failure_gap.gti`: derived
construction fails, then its base's unique-owned pointee destructor fails, but
the report contains only the secondary `GTI-R0002` instead of `GTI-R0014` with
both records. The generated allocation caller invokes `std::destroy_at` on
the unpublished object without passing the primary failure into the nested
destructor wrapper. This must be fixed before publishing the constructor slice;
the success/first-failure matrices and corpus do not establish this contract.
The next implementation step is failure-aware aggregate rollback that consumes
the MIR cleanup obligation and preserves the primary record through nested
subobject destruction, not another relaxation of backend admission.

The same cleanup-record failure reproduces at all four C++20/C++23 O0/O3
endpoints with the freshly installed compiler. All nine external installed-
library consumers pass, including the C11 C-thread runtime consumer. Their
initial configure needed the same explicit local LLVM/Clang package paths as
the build tree; configuration, serial compilation, and all consumer tests
then succeeded. These are macOS local results, not CI or other-platform
evidence. The new failed-runtime reproducer is intentionally not registered as
a passing gate; the constructor change remains unpublished and incomplete.

The follow-up introduces a MIR v59/body v51 cleanup descriptor for a
generated single-owner field/base chain ending in a lifecycle-guarded leaf
destructor. The actual stdlib path includes a source-defined `unique_ptr`
class wrapper, so handling only the primitive unique owner was insufficient.
Each shell now retains its immediate subobject identity; independent MIR
derivation checks the complete descriptor, and `Drop` uses the existing
`Invoke` and primary/secondary failure protocol. C++ only spells those steps
and tears down empty native owners. No AST/HIR fallback or ambient failure
context was introduced.

The full serial build passes. `failure_metadata` passes six malformed-
descriptor/control-flow mutations per selected drop. The newly registered
`unique_pointee_cleanup_failure_runtime` passes all 49 runtime cases across
C++20/C++23 O0/O1/O3 plus ASan/UBSan, including empty owners, first cleanup
failure, primary failure with successful cleanup, and double failure through
ordinary, field, and inherited-base cleanup. The original reproducer is now
`tests/fixtures/inherited_constructor_cleanup_failure.gti` and is a passing
regression. This does not establish multi-owner aggregate cleanup or pointees
with their own active-cleanup fields. Broader validation and an admission
probe for the multi-owner base boundary are in progress; publication remains
pending, and the full stdlib/C-thread campaign remains active.

The refreshed 38-gate serial batch passes, including compiler/driver,
optimizer, failure metadata, lowered/backend contracts, emitter lifecycle and
census, C-thread runtime/semantic/LSP, class and inherited allocation,
temporary constructor arguments, stdlib surface/include sweep, and repository
release-policy checks. Formatting and patch checks pass. The adjacent admission
probe then confirms that a two-owner base still loses the primary record at
all four C++20/C++23 O0/O3 endpoints. The exact regression is retained as
`tests/fixtures/inherited_multi_owner_cleanup_failure_gap.gti`; every run
exits 70 with `cleanup` and only the destructor's `GTI-R0002`.

The next required implementation replaces the single-target assumption with
an ordered MIR-owned aggregate cleanup schedule. It must represent every
active field/base step and each step's effects, preserve the first failure
while draining remaining cleanup, and terminate immediately with both records
on a second failure. The existing `MirClassInstance::fieldDropOrder` supplies
field order, but a list of types alone is not the failure CFG or proof that
the backend consumes it. Keep leaf cleanup, owner release, and native
representation teardown distinct; do not reuse the first leaf's destructor
identity to stand for a multi-target cleanup or solve this by narrowing the
requested library outcome. The ordinary/field/base matrices and independent
missing/reordered/retargeted-step mutations must cover the resulting protocol.
No commit, push, or branch deletion occurred. A new corpus/project/installed
refresh is deferred until this known cleanup defect is fixed, since it would
otherwise have to be repeated after the required compiler change.

The next local implementation replaces that single-target assumption with
MIR v60/body v52 ordered aggregate descriptors. Generated field/base and
unique-pointee steps retain their exact types, identities, order, and all leaf
destructor targets. Lowering constructs the tree from concrete HIR; program
verification independently derives it from MIR declarations. The backend
consumes its immediate steps, keeps the first failure while draining remaining
cleanup, and avoids native destruction of unfinished subobjects after a second
failure. Scalar unique owners are included as non-failing cleanup leaves;
primitive storage/prefix-storage leaves also admit ordinary string ownership
beside fallible resources. Generated hosted-string obligations and their
independent verifier use the same exact descriptors.

Before the final string/hosted extension, all five focused gates pass:
`failure_metadata`, class allocation, inherited allocation, the 49-case
single-owner regression, and the new 91-case aggregate runtime gate. The last
includes two distinct destructor targets, mixed scalar ownership, ordinary
first-failure draining, constructor/field/body double failure, nested generated
pointees, moved/empty owners, and the exact original two-owner reproducer at
C++20/C++23 O0/O1/O3 plus ASan/UBSan. The original fixture is promoted to
`inherited_multi_owner_cleanup_failure.gti`. Independent mutations remove,
duplicate, reorder, and retarget nested steps, including substitution of a
different valid destructor identity. The final string/hosted extension then
passed a full serial build and all 45 selected compiler, optimizer, MIR/backend,
runtime, stdlib, boundary, and C-thread editor gates, including the 91-case
aggregate matrix. A subsequent effect-summary review found that allocation
release must remain a runtime effect even when every source destructor is
empty. The correction passed a fresh full incremental build, the focused
`failure_metadata` regression, and all 12 selected compiler, optimizer,
MIR/backend, C-thread semantic, cleanup-runtime, and issue-regression gates.
This rerun includes the 49-case single-owner and 91-case aggregate matrices.
The earlier 45-gate result predates the effect correction. The refreshed
emitter corpus passes for all 82 sources: 3,907 bodies are ready and all 2,179
general-emitter comparisons match. A fresh installation at
`/private/tmp/gti-aggregate-install.IswxCd/stage` passes all nine external
library consumers and the full 91-case aggregate runtime matrix, with build-tree
stdlib/runtime/vendor path overrides removed. The native two-endpoint example
corpus then passed all 82 examples and 3,904 reviewed body identities; the
serial project-workflow refresh also passed, including the game-engine
application. Full formatting and patch checks passed. These are local
macOS/AppleClang results, not cross-host or CI evidence, and predate the
prepared-call correction below.

An adjacent `vector<unique_ptr<Resource>>` cleanup probe demonstrated a real
remaining defect: a checked failure in the body followed by failure in an
element destructor reports only the destructor's `GTI-R0002`, rather than
retaining both records in `GTI-R0014`. The prior installed pre-aggregate build
shows the same result; this is not evidence about the clean HEAD build.
The native prefix-storage element loop still needs a MIR-owned cleanup
descriptor and explicit primary/secondary record protocol. A vector-owning
base constructor also exposed an independent verifier disagreement: ordinary
call `PreparedParameter` transfers were mistaken for unarmed subobjects.

The local correction exempts the exact ordered-call parameter handoff, while
retaining the independent unique-input/transfer verification. A failing-then-
passing `failure_metadata` regression covers both constructors and field
initializers, with missing/duplicate/cross-call transfer mutations. The
original vector-owning base now compiles. A stronger runtime regression then
exposed the status constructor's missing admission of ordinary prepared
argument cleanup when a later argument fails; its representation now reuses
the existing exact class-result staging proof. The full serial build now
passes, as do `failure_metadata` and all 42
`constructor_owned_call_runtime` cases across C++20/C++23 O0/O1/O3 plus
ASan/UBSan. The runtime trace proves prepared-argument cleanup before field
rollback, callee ownership after handoff, later constructor cleanup, and exact
double-failure reporting. The final matrix also combines a later-argument
failure with a second failure in the still-caller-owned argument's destructor.
The full 14-gate compiler/optimizer/backend/allocation/cleanup refresh passes
(293 seconds), including the 49-case single-owner and 91-case aggregate
matrices. At that checkpoint the vector element-loop failure was still
unresolved, so publication was not ready. The prior corpus, project, and installed-consumer refreshes predate
this constructor correction and must be refreshed before final publication.

The next local implementation adds MIR v61/body v53 collection-drop recipes:
one unprojected element cleanup, reverse live-prefix/initialized-slot traversal,
and allocation release after the elements drain. Generated helpers use the
same explicit failure cursor as field/base aggregates; source-leaf class
elements disarm their lifecycle guard before native representation teardown.
Both compiler and backend build. The new collection runtime matrix passes all
105 cases across C++20/C++23 O0/O1/O3 plus ASan/UBSan, including the original
inherited vector-base regression, sparse holes, nested first-failure draining,
empty/moved owners, direct leaf objects, and aggregate/string elements.
The full serial build and independent malformed-element mutations pass.
All 25 selected compiler/optimizer/backend/allocator/cleanup/include checks
now pass across the batch and a focused rerun. The initial compiler-pipeline
failure was an old assertion expecting the class-only verifier message; the
forged destructor now reaches the shared canonical descriptor check. Updating
the expected message retained the rejection assertion, and the rebuilt
pipeline passes. The sparse-storage fixture now also constructs out of slot
order; its final 105-case matrix refresh and `failure_metadata` both pass.
Earlier corpus/project/installed evidence predates v61 and is not final
publication evidence.

Full clang-format checking passes for all 324 first-party C/C++ files, and
`git diff --check` passes. A final C++20/O0 probe confirms the next live defect:
`pop_back()` on a vector holding resource owners 1 and 2 terminates immediately
when resource 2's destructor fails. It prints only `2`, not the required
remaining-vector/outer cleanup trace `2, 1, 9`. The source destructor's
`GTI-R0002` is reported, but the explicit prefix-pop intrinsic has no contained
failure handoff to its MIR caller. Collection scope destruction is now proved;
explicit removal must carry the element recipe and status/record through its
own operation before this part of the library can be called complete.

The next local change (MIR v62/body v54) closes that explicit pop defect and
the corresponding sparse-slot destruction defect. Both calls now carry an
independently verified element recipe, exact destructor effects, and the
original propagated record alongside their existing local bounds/state
status. The slot is retired before cleanup and its disarmed representation
ends once. Public vector/storage `clear()` composes the same operations.
The expanded fixture first failed at C++20/O0 phase 15 with the original
`2`-only trace, then passed all 189 cases across C++20/C++23 O0/O1/O3 and
ASan/UBSan. It covers successful removal, first/secondary failure, direct
leaves, sparse holes, nested collection pop, clear, empty public pop, and
invalid-slot errors retaining their own source. The initial invalid-slot
expectation used a physical filename; the observed canonical logical source
is `<std/memory>`, and the exact expected spelling was corrected. The focused
`failure_metadata` gate passes, including twelve forged removal recipe/effect
mutations. The full serial build and 31 selected final-tree checks now pass
(618 seconds): compiler/library boundaries, pipeline, optimizer, metadata,
initialization/startup, C-thread semantic/create-join, lowered/backend contracts,
emitter structure, forward-list, allocation/cleanup matrices, allocator/storage,
stdlib manifest/surface/include sweep, and issue regressions. The collection
matrix passed all 189 cases again in that final-tree run. Clang-format checks
all 324 first-party C/C++ files successfully, and `git diff --check` passes.
Final publication
still requires refreshed corpus/project/installed-consumer evidence and the
remaining implementation closure; nothing is published.

The next local extension (MIR v63/body v55) adds the same exact recipe to
arbitrary-index prefix erase. Its baseline runtime regression failed at
C++20/O0 phase 27 with only `2` printed rather than `2, 3, 1, 9`. The operation
now detaches the suffix while destroying the selected element. Success uses
independently proved generated relocation to close the hole; failure drains
the detached suffix backwards before returning the original record to caller
prefix cleanup. A second suffix failure terminates before representation
teardown, retaining the distinct primary and secondary source sites.
All 308 expanded runtime cases pass across C++20/C++23 O0/O1/O3 and ASan/UBSan,
including first/middle/last erase, bounds failure, successful append after
erase, direct guarded leaves, nested collections, mixed string/resource
aggregates, and inherited owning elements. One new verifier mutation initially
selected a generated wrapper without a guard, making its guard-removal change
a no-op; the test now requires a guarded source-destructor class. The rebuilt
verifier passes, including eighteen removal-recipe/effect mutations and two
generated-move/guard mutations. The full serial build and all 27 selected
compiler/optimizer/backend/allocator/cleanup/include/issue checks pass
(264 seconds), including all 308 collection cases again. The refreshed serial
emitter corpus also passes: 82 sources, 3,907 ready bodies, and 2,179 matching
general-body emissions. Clang-format passes for all 324 first-party C/C++ files,
and `git diff --check` passes. This refresh does not substitute for the still-pending
final native corpus, project, and installed-consumer refreshes or the remaining
implementation work; nothing is published.

The next local extension (MIR v64/body v56) composes source-destructor bodies
with their represented owning fields. A baseline reproduced lost primary
failure reporting when the owner body and a field destructor failed during
caller unwinding. The exact MIR recipe now runs the guarded body first,
followed by reverse fields/base, with independent descriptor verification and
the shared primary/secondary sequence. A failure-free body does not hide its
fields' failure effects. Private empty-state construction can represent a
guarded source-destructor field only with a complete recipe, generated move
assignment, recursively empty fields, and an explicitly inactive guard.

The original `temporary_constructor_argument_user_drop_gap.gti` exposed two
additional backend defects during this extension: a partial native initializer
proof suppressed body publication even when no native field binding was
selected, and a nested status-only construction acquired an invalid ordinary
overload call. Publication now uses the same complete binding as move emission,
and plain constructor calls must prove their ordinary representation exists.
The original reproducer now returns the correct values and body-before-field
cleanup trace at C++20/O0. It is part of the required new runtime matrix, not an
unresolved or skipped fixture. The initial 217 source-body/field, nested,
collection-removal, move, and partial-construction cases passed all six
C++20/C++23 O0/O1/O3 modes plus ASan/UBSan. After the full serial build, the
final expanded 259-case runtime matrix, MIR verifier, and emitter lifecycle
tests pass (59 seconds). The broader checks initially caught a test-registration
ordering mismatch between the new lifecycle test and its manifest; the manifest
order is corrected without changing the test or its assertions. Final review
also tightened ordinary-constructor admission: an initializer-only tagged
containment overload does not authorize an ordinary body call. After rebuilding,
all 30 selected compiler/library/optimizer/verifier/backend/allocation/cleanup/
include/issue tests pass (289 seconds), including the 259 new cases and the
existing 308 collection cases again. The final serial emitter corpus passes
with all 82 sources, 3,907 ready bodies, and 2,179 matching general emissions.
Clang-format checks all 324 first-party C/C++ files successfully, and
`git diff --check` passes.
This does not replace the pending final native corpus, project, and installed-
consumer refreshes, or prove the remaining stdlib implementation complete.

This is not the end of aggregate failure work: cyclic ownership-type graphs,
other unrepresented cleanup families, and the broader base-construction
combinations still need their full protocols. The stdlib completion and full
C-thread objectives remain active; no publication or branch deletion has
occurred in this slice.

The next local extension (MIR v65/body v57) represents recursive ownership
with finite exact class back-edges. The original `forward_list<Value>` probe
lost both remaining caller cleanup and an earlier primary error when a node's
destructor failed. The root recipe now contains every reachable destructor
while re-entries reference an enclosing class. Independent MIR derivation and
six back-edge mutations enforce the contract; an independent class-graph walk
checks that effect targets are not lost through either self or mutual recursion.

`unique_ptr::reset()` now detaches the old capability into a local public owner
before destruction, so its source body uses ordinary MIR cleanup. A raw
capability local was insufficient: existing passive-pointee/reset backend gates
rejected it. The public-owner form passes those gates without changing their
assertions. A trial ordinary assignment overload was not retained: ordinary
method calls cannot replace the compiler's moved-place reinitialization rule
used by list algorithms.

Mutually owning node types also exposed two native-representation defects.
Generic type arguments were incorrectly counted as by-value layout
dependencies even for pointer-only carriers. Planning now follows the exact
MIR fields/bases instead. C++23 additionally instantiated constexpr defaulted
carrier special members while pointees were incomplete; generated unique-owner
carrier default/move/destruction now has explicit non-constexpr spelling with
the same field order and guard policy. C++20/C++23 and sanitizer executions
then agree on 26 of 27 phases (182 of 189 executions). The mandatory
`recursive_cleanup_failure_runtime` gate remains failing, not waived.

The remaining phase 17 at that checkpoint was **live-owner assignment**: replacing a nonempty
owner with a newly allocated node still destroys the old value through native
assignment without an explicit MIR failure/commit protocol. It prints only
`2`, rather than cleaning the rest of the old chain, the uncommitted new value,
and the caller (`2, 1, 3, 9`). The next slice must retain the staged RHS until
old-value cleanup succeeds, propagate failure before publication, and verify
that ordering independently. Do not reinterpret terminal native cleanup as a
passing outcome or bypass this with a public-name backend special case.

`recursive_owner_projection_gap.gti` separately preserves a reduced accepted
program rejected by C++ representation planning at `root->next->next`. The
recursive destruction oracle builds the same mutual chain through constructors,
so it can test destruction independently without hiding the projection defect.
The projection gap, general replacement, remaining cleanup families, full
stdlib/C-thread completion, and branch reconciliation/cleanup remain required.
The final full serial build passes. In the 26-gate affected regression set,
24 passed initially; the compiler aggregate's remaining assertion expected the
old `= default` native move spelling. It now checks the explicit field move,
non-constexpr default/destructor, deleted copy, and owned-result publication,
and the rebuilt aggregate passes. Thus 25 gates pass and only the new recursive
runtime gate remains red for phase 17 in all seven modes. The same final
runtime matrix also includes default field initializers on the mutually owning
types. Existing allocation, inherited allocation, source-destructor cleanup,
collection cleanup, owner access, and 30,000-node iterative list gates pass.
The serial emitter corpus passes all 82 sources, 3,907 ready bodies, and 2,179
matching general emissions. This is partial verification, not a release claim:
live replacement and the projection reproducer remain wrong, and final native
corpus/project/installed-consumer refreshes remain pending. No commit, push,
or branch deletion has been performed.
Clang-format checks all 324 first-party C/C++ files successfully and
`git diff --check` passes.

The next local extension (MIR v66/body v58) separates old-value cleanup from
replacement commit. `CleanupBeforeAssign` carries the independently derived
recipe and has a symmetric exact pair with `Assign`. Fallible preparation
routes through `Invoke`; the RHS remains owned on failure, while only the
unique success edge publishes it. Generated assignment after cleanup is
independently proved non-failing. Six verifier mutations cover recipe/pair/
effect/success-edge corruption. The backend's moved and constructed temporary
representations now follow the pair, retaining failure-path drops rather than
requiring unconditional instruction adjacency. Storage-handle stages also
drain explicitly on that path.

The original 27-phase regression now passes all 189 executions. Ten added
phases cover empty/live destinations, successful and repeated replacement,
named moved RHS values, direct source-destructor objects, checked owner fields,
and a second failure while cleaning the uncommitted RHS. All 259 executions
pass across C++20/C++23 O0/O1/O3 plus ASan/UBSan. Those cases additionally exposed
and fixed generated empty owning arguments passed directly into a status
constructor; the representation proof checks the literal-only field initializer
and its MIR `Exit`, not an invented `Return`.

The full serial build passes. The 30-gate affected regression run initially
passed 28 gates; two structural assertions still demanded the old adjacent
move/assign/drop schedule. They now require the explicit preparation pair,
exact success edge, loan ending, and normal-path shell drop, and both rebuilt
tests pass. The final four-gate rerun (metadata, preflight, lifecycle, recursive
runtime) passes, including all 259 runtime executions after tightening the
generated-empty argument proof to passive fields and native owner/storage
handles. The expanded regression run also passes the public C-thread
create/join and yield gates, all existing stdlib-surface cases, include sweep,
allocation/owner-access/constructor-owned calls, collection cleanup, source
destructor cleanup, and iterative forward-list tests. The final serial emitter
corpus passes 82 sources, 3,907 ready bodies, and 2,179 matching general
emissions. Formatting checks all 324 files; `git diff --check` passes.
The remaining Python MIR-header assertion now expects v66, and
`issues_117_120_regressions` passes on the final compiler.

This remains partial release verification: final native corpus, project, and
clean-stage installed-consumer refreshes have not run for this extension.
Checked-place assignments that still
carry local bounds detectors do not yet use the split, and unrepresented
cleanup families, recursive owner projection, full stdlib/C-thread completion,
and branch reconciliation/cleanup remain required. No publication or branch
deletion has occurred.

The subsequent nested-owner projection repair fixes the original
`root->next->next` reproducer in MIR lowering. Borrow-source canonicalization
had appended a pointee's field to the call's owner dependency instead of
retaining the returned referent. Projected call-result sources now keep their
loan root and parent chain; independent program verification rejects the old
flattened-origin form even when its dependency key compares equal. No backend
admission relaxation, source-wrapper name matching, syntax change, or new MIR
operation is involved.

The focused verifier gate and expanded recursive runtime gate pass. All 42
trace/failure phases run at C++20/C++23 O0/O1/O3 plus ASan/UBSan (294 runs),
alongside the original minimal projection reproducer in all seven modes.
The added cases cover deep assignment/reset/read, a longer mutual chain,
empty-intermediate-owner failure with staged-RHS cleanup, and recursive
destructor failure. The full serial build also passes. This removes the
preserved projection defect; the remaining goals cover checked-place assignment,
cleanup families, stdlib/C-thread completion, release validation and branch cleanup.
All 18 additional affected gates pass: compiler aggregate, optimizer foundation/
safety/analyses, lowered-program contract/rejection/boundary, C++ plan/control
flow/preflight/text/lifecycle/representation, forward-list and owner-access
runtime, include sweep, reference-parameter ranges, and mutable stored borrows.
The serial emitter corpus again passes 82 sources, 3,907 ready bodies, and
2,179 matching general emissions. Changed C++ files pass clang-format and
`git diff --check` passes. Native corpus, project, installed-consumer, and
release gates remain pending; nothing has been committed, pushed, or deleted.

MIR v67/body v59 extend replacement preparation to built-in fixed-array
elements. The exact bounds origin now belongs to `CleanupBeforeAssign`, which
validates the destination before touching the old value, then runs its complete
cleanup recipe. Both bounds-only and destructor-propagating preparations use
`Invoke`. The paired `Assign` is detector-free and only commits on success;
the RHS remains active on either failure. Independent verification requires
the checked recipe/pair, exact bounds outcome, and success-only publication.
The C++ representation checks each dimension before forming its next subscript
and retains the original failure site. Fresh ordinary-call results, direct
constructed objects, and named moved values use the same explicit staged
ownership as non-indexed replacement.

The new `checked_owner_replacement_runtime` fixture passes its initial
19-phase matrix in all seven C++20/C++23 O0/O1/O3 and ASan/UBSan modes (133
executions). It covers success, unchanged old elements on invalid bounds,
positive/negative indices, first and secondary cleanup failures, non-failing
old cleanup, and both dimensions of a chained destination. The metadata gate
checks bounds-only and bounds-plus-cleanup effects and mutates detector
presence/placement, outcome, success routing, publication, and pair identity.
The full serial build and all 28 affected regression gates pass, including
the compiler aggregate, verifier mutations, optimizer and backend contracts,
old/new cleanup families, owning array lifecycle, scalar/out-of-bounds/chained
array access, polymorphic arrays, and existing array assignment regressions.
A copied source-destructor probe is rejected by the existing `GTI-S2003`
noncopyable-type rule, not by the backend; it is retained as a negative test.
Final release validation remains pending; this does not finish unrepresented
cleanup families, the full stdlib/C-thread surface, or branch reconciliation/
cleanup.
The final serial emitter corpus passes all 82 sources (3,907 ready bodies and
2,179 matching general emissions). The final checked-replacement rerun passes
all 133 executions plus the negative-copy assertion, and CMake test ownership
passes. Changed C++ formatting and `git diff --check` pass. No commit, push,
or branch deletion has been performed; native corpus, project, installed
consumer, and release refreshes remain outstanding for the full campaign.

### L-GEN-01: Production Generic Function System

- **State/role:** done; systems-readiness language and standard-library
  capability. GTI owns generic declaration syntax, exact type inference and
  explicit type/value arguments, inferred fixed-array `uint64_t` values,
  bounded type packs, declaration defaults and dependent value expressions,
  source concepts and trailing requirements, concrete semantic reanalysis,
  HIR/MIR/lowered instance identity, ownership/effects, verified-MIR execution,
  public callable relations, and deterministic concrete backend identity.
- **User-facing unlock:** reusable source-defined standard-library algorithms,
  containers, allocators, and concurrency utilities can state their complete
  type/value/callable requirements, instantiate deterministically, and retain
  the same ownership, failure, diagnostics, and tooling behavior as a concrete
  function. A library author must not need a dummy deduction argument, a
  public-name compiler special case, or native C++ substitution to express a
  valid generic operation.
- **Required slices, in dependency order:**
  1. **Completed:** explicit immutable `uint64_t` function and method
     arguments preserve ordered type/value identity through concrete
     reanalysis, HIR, `mir-v53`, LoweredProgram, constexpr evaluation, exact
     backend calls, formatting, Tree-sitter, and LSP signatures. Literal,
     named-constant, symbolic forwarding, mixed type/value, value-only, and
     method calls execute across the production mode matrix;
  2. **Completed:** declaration-owned trailing type/value defaults and closed
     dependent `uint64_t` constant expressions are selected by GTI after exact
     inference, preserve complete call/type identity, diagnose checked
     arithmetic and ordering errors, and appear coherently in formatting,
     Tree-sitter, and LSP without return-context inference or native-template
     defaults;
  3. **Completed:** public unary/binary callable, predicate, operation,
     comparator, indirect iterator-call, and once-action relations expose the
     accepted exact read/mut/once contract. Comparator `sort`, whole-vector
     predicate/operation clients, constrained numeric reducers, and
     `std::invoke_once` use them without ranking or backend validity;
  4. **Completed:** `E-SYM-01`/`E-INST-01` give every nameable source-defined
     generic free-function instance, eligible top-level class application, and
     ordinary member instance a deterministic GTI-owned symbol derived from
     lowered nominal plus concrete type/value/signature identity. Selected
     constructors emit as ordinary concrete overloads, and exact class
     specializations emit as already-selected ordinary classes. Symbols remain
     byte-identical when unrelated declarations shift discovery-order IDs. A
     native physical carrier remains only for an unused family, nested type
     family, direct private unique-owner storage, compiler/C ABI capability, or
     a program containing an unnameable lexical closure, together with the
     transitive dependent generic families required to spell those carriers.
     Concrete initializer rows and selected native-owner member
     specializations remain instance-exact. Deduced callable members on a
     generic owner compare canonical MIR only within one concrete owner and
     project signatures by substituting owner parameters while retaining the
     method parameters; the multi-owner `int32_t`/`uint8_t` regression executes
     in all four native modes. These carriers consume GTI-selected facts and
     cannot perform validity, lookup, overload, ownership, failure, or
     execution decisions; and
  5. **Completed:** the production conformance matrix covers direct and project
     driver workflows plus installed CLI/library smoke clients; concrete valid,
     invalid, constrained, ownership, borrow, move-only, variadic, cleanup, and
     failure cases; HIR/MIR and backend mutation gates; O0/O1/O3 and
     C++20/C++23 execution; formatter; Tree-sitter/Neovim; and LSP hover,
     completion, and signature help. The named executable evidence includes
     explicit value generics, the generic algorithm cluster, public callable
     concepts, reference-parameter ranges, mixed-capture carriers, standard
     containers, and the installed generic CLI corpus; and
  6. **Completed in 0.328.0:** source concepts and trailing clauses admit
     precedence-aware `&&`/`and`, `||`/`or`, and parentheses. Concrete
     viability accepts either complete alternative; symbolic bodies receive
     only common atomic and structural facts, and inline constraints retain
     exact concept identity even when the common set is empty. Formatting,
     Tree-sitter, LSP signatures, structured diagnostics, lowered metadata,
     and the four-mode native runtime matrix cover the surface.
- **Durable boundaries:** exact unique-match overload resolution, explicit
  ownership transfer, no ADL or implicit call conversions, validity-only
  requirements, frontend-owned substitution, and verified-MIR execution do
  not change. General C++ requires-expressions, partial specialization,
  subsumption/ranking, CTAD, type erasure, arbitrary reflection, and
  unrestricted compile-time execution are not production prerequisites unless
  a standard-library client cannot be expressed by the bounded exact model.
- **Exit gate:** at least one value-parameterized utility, one constrained
  multi-type algorithm, one repeatable callable/predicate algorithm, one
  consuming callable utility, and one move-only generic owner compile as
  ordinary installed GTI library clients. Every discovered concrete instance
  has one semantic/HIR/MIR/lowered identity and executes only its verified MIR;
  invalid substitutions and ownership/failure violations are GTI diagnostics;
  generated C++ performs no semantic selection; and the complete production
  matrix above passes.

### L-INIT-01: Contextual Fixed-Array Arguments

- **State/role:** done in 0.130.0; bounded generic initialization capability
  for ordinary APIs and container clients.
- **Implemented scope:** A brace argument to a named function, method, or
  constructor initializes one exact by-value `T[N]`. An inferred-only
  `uint64_t` function, method, or constructor value parameter may name that
  complete extent. Exact type/value arguments survive semantic selection and
  concrete HIR identity; the backend emits an explicit fixed-array value and
  never delegates list-overload selection to C++.
  `std::vector<int>({1, 2, 3})` is the first public client, not a
  compiler-recognized special case.
- **Non-goals:** `std::initializer_list`, common-type deduction, CTAD,
  list-preferred overloads, explicit callable value arguments, generic
  `operator()` brace context, or broader value-generic expressions.
- **Exit evidence:** generic functions, methods, and constructors,
  alpha-equivalent duplicate rejection, exact/ambiguous overloads, HIR/MIR
  identities, formatter, Tree-sitter, LSP hover, C++20/C++23 runtime, stdlib,
  and example coverage pass.

### L-RANGE-01: Dedicated Range And Element Loans

- **State/role:** done in 0.299.0; systems-readiness capability for
  container/game workloads.
- **Implemented scope:** Semantics records an exact read-only range plan for
  fixed-array indexed traversal and the existing nominal structural protocol.
  HIR and MIR retain the owner, range carrier, iterator/sentinel, per-iteration
  element place, access mode, selected operations, loans, loop edges, array
  extent, and owned-temporary/drop identity. Addressable ranges are borrowed;
  prvalues transfer into loop storage without copying a move-only owner.
  Element loans end before increment and every exit. MIR verifies normal,
  `continue`, `break`, `return`, nested-loop, and defined-failure cleanup before
  backend admission.
- **Non-goals:** pointer decay, generic view pipelines, parallel iteration, or
  mutable container iteration in the same slice.
- **Exit evidence:** fixed-array value/read-only and owned temporary range tests
  cover empty/non-empty, nested loops, `break`, `continue`, `return`, move-only
  owners, mutation/escape/reuse rejection, forged HIR/MIR rejection,
  C++20/C++23 O0/O3 runtime cleanup, and LSP publication/clearing without
  backend lifetime extension.
- **Unlocks:** mutable iteration and range invalidation.

### L-RANGE-02: Mutable Iteration And Invalidation

- **State/role:** done in 0.300.0; systems-readiness capability for in-place
  container/game workloads.
- **Implemented scope:** `mut T&` and `mut auto&` range declarations take one
  exclusive parent loan over a mutable fixed array, mutable nominal lvalue, or
  owned temporary. Fixed arrays use the existing indexed plan. Nominal ranges
  select exact `begin_mut()` plus `end()`, comparison, mutable dereference, and
  prefix increment; the compiler-generated iterator is the confined carrier
  of that parent. Each iteration creates one mutable child and ends descendants
  child-first before increment and on `continue`, `break`, `return`, normal
  cleanup, and failure cleanup. `std::vector` supplies the first mutable
  checked iterator. Whole-owner conflicts reject reserve, push, clear, moves,
  overlapping reads/traversals, and other invalidating access while the plan is
  active.
- **Non-goals:** public container invalidation traits, simultaneous disjoint
  mutable element traversal, user-retainable mutable stored iterators,
  iterator-position insertion/erasure, pointer ranges, or dynamic views.
- **Exit evidence:** focused semantics/HIR/MIR tests cover exact parent/child
  identities, immutable owners, missing mutable endpoints, overlap, mutation,
  escape, owner reuse, forged plans, and child-first endings. Runtime coverage
  exercises fixed arrays, `std::vector`, empty and owned temporary ranges,
  nested loops, `continue`, `break`, and `return` at O0/O3 under C++20/C++23,
  including exact normal/failure temporary cleanup. Formatter, C++ emission,
  LSP publication/clearing, and incomplete-source recovery are pinned.
- **Unlocks:** `L-RANGE-03` spans and dynamic views.

### L-RANGE-03: Spans And Dynamic Views

- **State/role:** completed for the bounded initial release; prerequisite
  `L-RANGE-02` is done; systems-readiness implementation.
- **Implemented scope:** Source-defined read-only `std::span<T>` values come
  from `std::vector<T>::as_span()` and `std::string::as_span()`, support checked
  access, iteration, subviews, empty ranges, local copies/moves, and exact
  factory/relay calls, and never expose a pointer-length pair. Dynamic
  `std::string_view` values come from `std::string::view()` while literal views
  remain ownerless static-storage values. Semantics, HIR, and verified MIR
  preserve each retained value's one read-only owner dependency through calls,
  returns, and drops; backend emission consumes that verified representation.
- **Safety boundary:** owner invalidation, movement, destruction, and local
  owner escape reject while a view is live. Dynamic text views also reject
  reassignment/reseating and storage in globals, fields, fixed arrays, or
  dynamic storage.
- **Deliberately deferred:** mutable views, multiple owners, nested or general
  stored dependency graphs, arbitrary pointer ranges, and unchecked public
  pointer-length construction.
- **Exit evidence:** focused semantics/HIR/MIR tests cover retained dependency
  summaries, invalidation, movement, escape, and nested-storage rejection.
  Runtime coverage exercises complete/sub/empty spans plus dynamic text-view
  copies, relays, checked access, drop, and owner reuse at O0/O3 under
  C++20/C++23. Focused LSP coverage publishes and clears the same diagnostics
  and exposes the source-defined `string::view` contract.

### L-CALL-01: Foundational Owned Callables And Captures

- **State/role:** in progress; `D-CALL-01`, `M-LIFE-01`, explicit confined
  boundary records, exact context-supplied and directly nested callable value
  results (concretized before HIR), repeatable read/mut
  invocation, consuming once-callable cardinality, and ordered local
  copy/move closure environments are done. Exact same-type generic return and
  the bounded one-field generic owner are also done. A repeatable confined
  callable may now carry an independent exact-return transport when every
  reachable return moves that same parameter; this serves source-defined
  `std::for_each`. Public unary/binary callable, predicate, operation,
  comparator, indirect iterator-call, and once-action capabilities now expose
  that same exact contract to reusable generic source. An eligible namespace
  free-function name may now contextually satisfy one of those confined
  contracts or an exact managed-thread task contract as a nominal zero-state
  item, with exact target identity through HIR/MIR/backend and no general
  storage or function-pointer conversion. External callable owners remain
  systems-readiness implementation shared by algorithms, tasks, and callbacks.
- **Scope:** Implement the accepted callable contract for exact return types,
  read/mut/once invocation capability, owned storage, and explicit move capture
  one independently tested sub-slice at a time. Keep callable identity,
  captures, movement, destruction, and invocation in GTI-owned
  semantic/HIR/MIR records. The completed confined slices already serve
  operation-based `std::accumulate`/`std::inner_product` and unary
  `std::transform_reduce`, plus `std::for_each`, including stateful mutable
  operation objects, without public-name recognition.
- **Non-goals:** immediately cloning `std::function`, arbitrary reference
  capture, thread transfer, native callback ABI, or hiding ownership in type
  erasure.
- **Exit gate:** the supported callable shapes move/drop exactly once, reject
  escaping borrowed captures, preserve exact generic calls/effects, and serve
  as ordinary GTI inputs to the first foundational range algorithms.

### L-CALL-02: Source-Defined Owning Function Wrappers

- **State/role:** agreed API implemented, validated, and installed locally.
  The fresh combined production suite passes all 294 tests. Repository
  publication includes the other tasks' shared-checkout work with explicit
  user approval; Gitea owns the release tag and asynchronous packaging.
  The earlier owning-wrapper deferral in L-CALL-01 is superseded.
- **Unlocks:** a command schema with an owned handler and an object API such as
  `set_auth_login_callback(lambda)` that retains and invokes the callback later.
- **Required surface:** C++-familiar `std::function<R(Args...)>` with copyable
  owned targets, plus the discussed move-only owning counterpart for move-only
  captures; construction, replacement/reset, empty-state observation, calls,
  explicit ownership transfer, and deterministic capture destruction. Complete
  the read/mut/consuming receiver contract without inheriting backend semantics.
- **Approved declaration direction (2026-09-27):** retain the caller spelling
  `std::function<void()>` / `std::function<int(int, int)>`. Define the library
  class with a headerless generic pattern, for example
  `namespace std { class function<R(Args...)> { /* ... */ }; }`.
  Do not introduce a C++ `template <typename ...>` header. This is a patterned
  generic primary declaration, not a requirement to write a separate primary
  plus a partial specialization. The pattern must be a reusable language
  facility, independent of the public class name. Preserve its source shape in
  the AST; semantics owns binder declaration, exact decomposition, and concrete
  instance identity. Before admitting execution, define how binder positions
  differ from existing type references, retain source ranges for tooling, and
  carry the resolved bindings through HIR/MIR and lowered representation. This
  approved syntax does not authorize C++-style specialization ranking or
  backend-only matching. The parser now retains signature-first patterned
  primary heads separately from flat generic parameter declarations, including
  component source spans. Semantic binding now preserves one public signature
  argument while deriving result/pack parameters through resolved projections.
  Exact matching and concrete reanalysis precede HIR. Lowered declarations retain
  the shape and projections, including for native template representations; the
  C++ backend does not define matching or binder identity.
- **Implementation sequence:** function-signature types; reusable signature
  matching/decomposition and generic class packs; general exact callable field
  ownership and invocation; namespace function storage; ordinary source-defined
  callable models and polymorphic ownership/clone; public wrappers and callback
  registration/schema consumers. Add missing reusable language operations where
  a source implementation needs them. Do not recognize public wrapper names or
  delegate GTI lifetime/dispatch rules to native `std::function`.
- **Implemented contract:** ordinary GTI classes in `<std/functional>` provide
  `function<R(Args...)>` and `move_only_function<R(Args...)>` with read-only,
  `mut`, and `&&` signature families. They support empty construction and
  observation, invocation, reset/replacement, and explicit ownership transfer;
  `function` accepts copy-constructible targets and offers explicit deep
  `clone()`, while `move_only_function` also accepts move-only captures and has
  no clone. Wrapper values themselves are move-only. Models use ordinary
  interfaces and unique ownership, not public-name compiler matching or native
  `std::function` policy. Consuming invocation retains remaining captures
  through the caller's full expression. Setter and recursive command-schema
  consumers, exact value/reference arguments, owned results, allocation and
  cleanup failures, and malformed-MIR rejection have focused regression
  coverage. Combined production and installed evidence is recorded below.
- **Deliberate exclusions:** this delivery does not add implicit/reference
  capture, borrowed closure results without an origin contract, arbitrary
  borrowed-pointee consumption, custom copy/move constructor bodies, implicit
  signature conversions, or C++ API parity such as allocator customization and
  small-buffer optimization. General typed brace expressions remain a separate
  language limitation; named brace declarations and parenthesized construction
  support the approved wrapper surface. These are not unfinished requirements
  of the agreed callback API. A newly demonstrated valid-source composition
  failure must still be reduced and fixed rather than hidden by an exclusion.
- **Historical implementation record:** the following checkpoints describe
  incremental work and its then-current limitations. Statements that a later
  implemented wrapper feature was missing are historical, not additional open
  gates. Their individual test results do not substitute for fresh combined
  production or installed-consumer evidence.
  Unqualified signature metadata `R(Args...)` was added
  across AST, semantic aliases/generic arguments, lowered type representation,
  compiler queries, and Tree-sitter. Dedicated tests cover invalid object use,
  MIR runtime-value rejection, formatting, and executable generic tags. This
  prerequisite does not deliver owning callback storage.
  Fixed-arity signature deduction now reuses ordinary generic inference,
  including `void` result arguments/defaults and exact reference components.
  Concrete class aliases are revalidated after substitution. The pointer-copy
  failure proof distinguishes opaque `void*` value transport from forbidden
  pointee operations. Generic classes now retain one final type pack, form
  `R(Args...)`, and forward the exact list into another class pack, including
  empty and reference-metadata cases. Concrete identities are flattened before
  HIR and reference storage remains checked independently. Owner-pack by-value
  methods now use the receiver's prescribed signature for ordinary/operator()
  calls and virtual dispatch. HIR signatures are independent of source argument
  grouping, including empty packs; MIR retains per-element ownership and cleanup.
  The ordinary exact generic matcher now deduces final signature/nominal tails,
  including metadata-only function packs, explicit and repeated-pack agreement,
  constraints, and symbolic forwarding. It shares its diagnostic and candidate
  paths. Headerless signature-pattern primaries now support aliases, exact
  repeated/fixed components, empty packs, ordinary and owner-pack methods,
  and symbolic forwarding with concrete revalidation. This is primary
  decomposition, not competing class-pattern selection. Constructor owner packs
  and borrowed signature argument packs remain prerequisites for the full
  owning wrapper.
  The retained-field invocation slice now records an exact symbolic field
  requirement, rechecks its concrete target, and emits ordinary MIR invocation
  through source-generic owner representations. Focused tests pass for repeated
  read calls, mutable nominal targets, scalar and owned captures, exact result
  rejection, and normal exactly-once cleanup across C++20/C++23 O0/O1/O3.
  Capture-destructor failure now uses an explicit MIR aggregate recipe and a
  status-returning environment cleanup representation. The original regression
  passes C++20/C++23 O0/O1/O3, no-elide, and ASan/UBSan: caller cleanup drains
  after one capture failure, and a second failure preserves both records.
  The expanded gate passes 120 executions across those eight modes, including
  direct and stored two-capture closures, reverse destruction, first/second
  capture failure, body failure, and double cleanup failure. Focused mutations
  reject forged capture indices, missing/reordered children, missing destructor
  targets, and erased failure effects. The five existing callable ownership
  tests also pass.
  The next minimal client is a source-defined `Model<T> : Invoker` retaining
  `T callback`, allocated with `make_unique<Model<T>>(std::move(callback))`
  and upcast to `unique_ptr<Invoker>`. A generic factory returning that owner
  now reaches its concrete factory body: otherwise ordinary direct by-value
  generic parameters have an owned-entry contract, independent of return syntax.
  Multi-statement and branching factories use the existing value/loan/lifecycle
  rules, with exact entry-state and cleanup mutations covered. The 120-case
  capture matrix also passes through a multi-statement owning factory. Existing
  confined contracts and implicit-transfer rejection remain intact.
  The broader ownership suite exposes the pre-existing
  `testCompleteRangeConceptsAndAlgorithms` fixed-array-reference rejection
  (three assertions); disabling owned-value discovery reproduces the same
  diagnostics. Preserve that failure for the full integration gate rather than
  treating the focused callable evidence as a green whole-suite result.
  Final by-value pack forwarding now preserves per-element owned contracts with
  flattened argument indices, including a fixed prefix and interleaved ordinary
  values. Copyable closures get fresh prepared lifetimes; move-only closures
  transfer their entry lifetimes and cannot be forwarded twice. MIR mutations
  reject forged element indices, traits, entry cleanup, and copyability. The
  native representation uses exact pack-index type aliases, tuple argument views,
  explicit transformed-call type arguments, and real prepared closure slots.
  The extended callback matrix passes 160 executions / 24 native builds across
  C++20/C++23 O0/O1/O3, no-elide, and ASan/UBSan, including pack cleanup, body
  failure, first cleanup failure, and primary/secondary failure preservation.
  Focused stored-callable/signature suites and the six existing callable tests
  pass; variadic storage/vector, unique allocation, and polymorphic upcast tests
  also pass. No public wrapper is delivered by these prerequisites.
  Concrete `unique_ptr<Model<T>>` factories now compile and execute through
  ordinary source-defined allocation. Exact explicit generic arguments retain
  body-local overlays, and nested owners/references preserve the unnameable
  closure type. Both passive and move-only captured models have native evidence;
  the heap-owned failure fixture checks normal cleanup, body failure, first
  cleanup failure, and double failure with exact capture/caller order.
  The expanded 216-execution / 24-build matrix passes C++20/C++23 O0/O1/O3,
  no-elide, and ASan/UBSan (236 seconds); focused signature and stored-callable
  suites also pass, including rejection of a forged allocation type argument.
  At that checkpoint, a factory returning its call directly failed representation when
  fallible parameter cleanup separates that call from Return; the multi-statement
  factory with a local result and explicit move worked. The subsequent pending
  result-lifetime work below addresses that gap. The heap fixture computes its scalar argument
  before borrowing the receiver: a short-circuit/conditional argument evaluated
  after an owner-borrow receiver also exposes an incomplete staged-loan CFG path.
  Nominal generic owning-carrier parameters now receive the ordinary owned
  entry contract, so `upcast_unique` no longer rejects a closure-containing
  owner merely because the parameter is not a direct type parameter. MIR
  follows its exact prepared move and independently verifies the prepared
  lifetime. Focused mutations reject a copied stage, removed activation,
  wrong argument index, and erased source ownership event. Bodyless transport
  still reports `GTI-S2046` at the argument with updated owning-carrier wording.
  Generic-owner virtual failure-family discovery now uses its template overlay,
  and unnameable owners no longer also emit placeholder concrete definitions.
  A native virtual bridge is emitted from the verified status body. It now
  preserves source result/pack spellings for closure-bearing owners whose
  represented instances share one concrete signature. Multiple signature
  variants still require a complete native representation.
  The standalone passive erased-owner consumer builds and runs at C++23/O0,
  C++20/O3, and C++23/O1 with ASan/UBSan. Focused stored-callable and signature
  tests pass, including the active factory's body representation check; the
  seven selected ownership checks (owned transport, confined parameters and
  results, callable member and consuming operators, lambdas, and polymorphic
  unique-owner upcast) also pass. This is not a passing erased-capture cleanup
  matrix or a complete owning wrapper.
  A returning factory can now represent multiple definitely moved-from cleanup
  continuations. The new active-capture erased regression gets past that
  factory but initially failed direct result publication inside `upcast_unique`:
  the owner's method empties a field, which does not make the whole parameter's
  remaining drop statically inactive. Do not silently rewrite the standard
  library to hide this general result-publication/rollback gap.
  The expanded invocation fixture additionally combines heap and erased models
  and moves a callback through nominal carriers. Its allocation-template merge
  rejection is fixed: closure-bearing free-function instances now keep distinct
  artifact-local native symbols and exact MIR bodies, including different
  selected constructor tags. Native templates carry physical types only; they
  no longer merge the free-function instances by parameter-cleanup shape.
  Focused tests reject forged instance signatures/identities and check agreement
  between emitted declarations and call rows. The invocation fixture passes all
  eight C++20/C++23 O0/O1/O3, no-elide, and ASan/UBSan modes.
  The pack fixture now mixes empty, single-closure, mixed passive, and
  active-capture packs of the same free declarations. Its 40 native executions
  pass the same eight modes with exact cleanup order and failure records.
  The expanded failure fixture retains seven erased-owner cleanup cases.
  Direct publication inside `upcast_unique` now
  compiles: MIR v72/body v62 retain a `ReturnResult` lifetime from completed
  operand construction through fallible callee cleanup, retiring it only at
  final publication or on failure. The backend places direct/moved/constructed
  class results in caller storage and follows the exact rollback schedule;
  wrapped `expected` values and storage handles also retain correct cleanup.
  The focused regression passes 136 native executions across C++20/C++23
  O0/O1/O3, no-elide, and ASan/UBSan. Static mutations, signature tests, and the
  lifecycle emitter suite pass. No library function was rewritten to avoid the
  return rule. MIR v73/body v63 now retain dynamic polymorphic owner cleanup.
  Typed allocation binds the complete object's verified recipe before owner
  publication, and upcasts preserve it. The expanded runtime gate passes all
  272 executions across the eight modes, including failing erased captures,
  remaining caller cleanup and both failure records. Static mutations reject
  missing dynamic dispatch and missing pointee identity. A separate ordinary
  polymorphic-object regression passes 48 executions across those eight modes:
  declared destructors, reverse field cleanup, primary/secondary failures and
  assignment preserving allocation metadata. Constructed fields now reuse the
  passive-interface-base proof; active bases remain excluded from hoisting.
  Lambda invocation now expands concrete by-value argument packs with exact
  semantic checking, an explicit HIR receiver, and MIR receiver/argument
  staging. Native emission reads that receiver without copying its environment.
  Owner-pack constructor overlays preserve the concrete binding identity, and
  template member bodies project flattened MIR arguments into native packs.
  Local empty/multi-argument calls and the first generic erased signature model
  pass 16 executions across all eight native modes; focused static suites pass.
  This is not yet arbitrary signature composition or the public wrapper API.
  Exact void expression returns now lower through the ordinary MIR evaluation,
  failure and cleanup schedule without a result operand; the verifier rejects
  a forged value/loan on void Return. The new 48-execution runtime matrix passes
  all eight native modes, including generic callable-parameter forwarding,
  temporary/parameter/local cleanup and primary/secondary failures. A passive
  copied closure passed to a generic pack now materializes one native type;
  the call no longer creates a second incompatible lambda literal.
  Erased void status calls now preserve the symbolic declaration's unused
  result slot, with virtual-root/override representation agreement checked.
  Concrete inherited packs expand before override signature comparison.
  Member families select verified MIR variants by exact nameable signatures
  and cleanup shape, rejecting ambiguous or unrepresented variants.
  The void and mixed-signature reproducers now pass in the expanded
  32-execution signature matrix across all eight modes. This also exposed and
  fixed the default polymorphic allocation helper's missing cleanup binding;
  explicit constructor allocation already carried that binding.
  The cloning client `tests/fixtures/stored_callable_clone.gti` passes all eight
  modes in the expanded 40-execution signature matrix: independent cloning, empty cloning, reset,
  replacement, multiple distinct closure identities, and int/void/mixed-argument
  signatures. Ordinary closure arguments now use prepared copy/move lifetimes;
  constructor-local template overlays retain exact substitution, signature and
  identity checks. Constructor-local failure cleanup and replacement of a
  completed owning field preserve ordinary MIR rollback. The expanded
  constructor test passes 70 cases across C++20/C++23 O0/O1/O3 and ASan/UBSan,
  including local cleanup failure and secondary field cleanup failure.
  Virtual class results use uninitialized result slots. Materialized closure
  environments carry private MIR identity selectors so member bodies targeting
  different concrete helpers are not merged solely by call signature.
  The 272-case stored-callable matrix and 48-case void-return matrix pass again.
  Exact-field callable transport now composes with an unpublished return result:
  failure-only cleanup is not treated as a second successful use, and the
  transfer proof follows the exact reparenting through final publication.
  Focused ownership regressions and mutations reject missing reparenting,
  missing publication and an invalid cleanup phase. The expanded return-result
  matrix passes 160 executions, including normal and primary/secondary-failure
  cleanup of a moved captured callback inside a returned owner.
  Source-defined `std::function` and `std::move_only_function` now have an
  unqualified, repeatable read-only subset in `<std/functional>`. The public
  consumer retains a captured callback, deep-clones it, transfers it through a
  setter, resets/replaces it, calls a void callback and retains a move-only
  capture. This does not complete receiver-qualified signatures, reference
  packs, namespace function storage, broader failure coverage or editor and
  installed-consumer gates. The full library manifest gate also still reports
  the pre-existing stale `forward_list` fingerprint; do not claim it passes.
  The public-wrapper eight-mode runtime matrix passes after the immutable
  template-plan cache change; the focused ownership, stored-callable and
  signature gates also pass. Concrete parameter packs now accept exact
  read-only reference elements, with ordinary matching, HIR borrow roles,
  MIR element projections and no transfer/drop of the referent. Lambda
  invocation consumes those staged borrows using the ordinary call-site/index
  and invoke-success-chain checks. Public callbacks now cover scalar and
  noncopyable object borrows, mixed signatures, repeated expansion and clones;
  malformed-MIR and LSP tests cover the same boundary. The two-fixture runtime
  matrix passed 16 executions across eight configurations. Namespace function
  items now retain their exact nominal identity through locals, generic return,
  captures, source-defined model fields, clone and replacement. Their value-pack
  transport uses passive stages, not closure cleanup. Callee-producing effects
  stay before arguments in MIR, and a field-held namespace function does not
  acquire a method receiver. The three wrapper fixtures pass 24 native runs
  across eight configurations. Parenthesized constructors now resume deferred
  value arguments in source order before inference; the prior unresolved
  function name no longer reaches MIR as an unknown value. A nested immutable
  command schema retains and invokes an owned leaf callback. That schema and
  a generic constructor-order regression pass another 16 native runs, including
  sanitizers. The six focused semantic/tooling gates pass. Mutable reference packs now
  preserve per-element entry loans, exact forwarding and non-escape through
  HIR/MIR and erased interface dispatch. `void(mut int&)` uses ordinary GTI
  parameter access syntax; mixed references pass the eight-mode native matrix.
  Invalid aliasing and forged loan access/escape/duplication are rejected.
  Copied passive lambda arguments use the existing ordered copy-staging storage
  instead of being deferred to the native argument list.
  The mutable-reference failure fixture passes 16 success/failure runs across
  eight native configurations with exact capture/caller cleanup. Stable peer
  borrow stages also compose with a later scalar conditional argument.
  Signature syntax now accepts trailing `mut` and
  `&&`, retaining exact receiver identity through aliases, generic deduction,
  patterned primaries, serialization, and native metadata types. A source-defined
  qualified mutable wrapper erases a retained counter behind an interface and
  preserves state through repeated invocation and movement. Its runtime fixture
  and a three-qualifier deduction fixture pass 16 executions across all eight
  native modes, including exactly-once target destruction. A public
  `move_only_function` temporary-target regression passes another eight runs.
  Generated default construction no longer rejects a source destructor when
  the independent initialization and exact active-cleanup transfer proofs hold.
  Exact receiver-family selection now retains distinct class/member identities
  through aliases, generic forwarding, and concrete specialization. Public
  `std::function<R(Args...) mut>` and `std::move_only_function<R(Args...) mut>`
  use source-defined mutable interface models; clone, replacement, reset,
  movement, and target destruction are covered by the mutable fixture.
  Consuming-owner invocation and consuming public wrappers now have focused
  execution evidence below; final campaign integration remains unfinished.
  The nominal consuming-receiver slice now retains the Move's Value obligation
  in the caller until its enclosing full expression ends. The receiver stage
  no longer transfers that obligation through `preparedParameterDrops`;
  invocation aliases the materialized slot. MIR mutations reject erased
  full-expression identity, duplicate executable input use, missing activation,
  and invocation transfer. The lifetime/failure fixtures passed 88 executions
  across eight native modes, including argument/body/later-sibling failure,
  reverse temporary cleanup, and primary/secondary failure preservation.
  Moved-out generic receiver fields now compile after field aliases and stores
  adopted the existing consuming-or-mutable receiver capability; their matrix
  passed another 16 executions across the same eight modes. Read/mut fallback
  representations have focused checks.
  Cleanup-owning local lambda receivers now use the same retained Value slot;
  the lifetime/failure fixtures passed 88 executions across eight native modes,
  including failing capture destruction and primary/secondary preservation.
  Verifier mutations reject lost full-expression identity, activation, and
  invocation transfer; a source negative rejects later reuse of the lambda.
  Exact generic by-value invocation passed another 24 executions across the
  same modes, distinguishing its callee-local receiver cleanup from the
  caller's later argument evaluation. Pack-expanded owning invocations now
  retain a `MoveValue` receiver stage without transferring cleanup. A separate
  single-use proof preserves cleanup-free materialized closures through their
  ordinary value stage. The pack fixture passed another 24 executions; the
  combined lambda runtime CTest passed all 136 executions. Focused consuming
  ownership, direct/staged lifetime verifier mutations, and signature LSP
  checks also pass. Nested generic callable results now receive the exact
  selected outer parameter context instead of defaulting to void. The argument
  and its suffix remain deferred until selection and are analyzed once in
  source order. Constructor, lambda, function-item, and concrete call-operator
  consumers use the same mechanism; explicit outer generic arguments and
  overload selection from earlier typed arguments are covered. Ambiguous
  contexts and wrong concrete callback results remain errors, and deferred
  results do not infer otherwise-unresolved outer type parameters. Nested
  callable emission also shares the ordinary prepared-receiver handoff rule
  and admits proven frozen local closure chains inside generic functions.
  The two nested fixtures add 48 executions across all eight native modes;
  the expanded six-fixture runtime CTest passes all 184 executions. The fresh
  static suite rejects forged receiver call-site and local-closure capture
  metadata, and signature LSP and backend text checks pass.
  A delegated pure-GTI CLI consumer exposed two additional reduced defects:
  `/private/tmp/gti-vector-duplicate.gti` reproduces duplicate concrete vector
  constructors when hosted argv and ordinary fields instantiate vector<string>;
  `/private/tmp/gti-discard-nominal.gti` reproduces GTI-B0001 when discarding a
  fallible scalar-only nominal result. Hosted constructor enqueueing now keeps
  owner arguments in the constructed type rather than constructor-local
  arguments. HIR/MIR verifier mutations and all eight direct native modes pass;
  the corrected runtime gate now passes all 16 direct runs plus dev/release
  package runs. The unused cleanup-free result uses a proven no-alias MIR
  result slot, retired after successful publication; its full eight-mode
  success/failure matrix passes all 24 runs. The prior load-related timeouts
  were followed by successful reruns. The full 47-test lifecycle suite passed
  before the subsequent ordinary-constructor staging change, including
  active-cleanup and borrowed-state rejection mutations.
  The CLI package remains at
  `/private/tmp/gti-cli-native` with its workarounds intact. Further reported
  cases include borrowed-view results through
  aggregate storage, constructor-body vector mutation/rollback, and composing
  unsigned-option registration with signed result retrieval; those
  still need reductions and verification.
  The nested fallible nominal-argument reduction now compiles. The shared MIR
  failure predicate routes cleanup-free class calls even in nested positions;
  resolved nominal call operators use exact argument/local result slots.
  Stable bare-local receiver staging also covers conditional arguments around
  nested read calls with scalar inputs and cleanup-free nominal results. The
  callback/constructor regression passes 40 native runs and its destructor-owned
  callback companion passes 24, across eight modes. Failure routing, activation,
  and capability mutations pass, as does the full 48-test lifecycle suite.
  Concrete callable adapters now declare friends inside their owners and define
  their forwarding bodies after type layouts. A separate namespaced fixture
  returning a class that stores its callable exercises this dependency cycle
  and named result publication; all eight native modes pass. Read-only, mutable,
  and receiver-family callback fixtures pass another 24 native runs after the
  adapter change, and the owning-wrapper static/production-emission suite passes.
  Retained native primary-template adapters now spell signature types through
  an owner-dependent exact-type identity while preserving inline ADL friends.
  Read, mutable, and consuming targets with later concrete class parameters and
  results pass emitted-text checks and 32 native runs across eight modes.
  A declared result constructor relying on a default-initialized passive class
  field now publishes an explicit This-field Initialize even without active
  cleanup. The verifier requires that stage, and the backend consumes its exact
  generated-default/field/boundary schedule. The callback fixture observes a
  scalar declaration default through the result rather than testing only an
  empty owner; its eight-mode rerun and the copied-field regression pass 16
  executions. The expanded 49-test lifecycle suite, owning-wrapper type checks,
  and emitter census pass. Fresh class arguments followed by inline conditional
  arguments now retain exact constructor failure edges and prepared-result
  publication. The 40-case callback matrix and refreshed 32-case generic adapter
  matrix pass in all eight native modes. The same prepared-argument context is
  retained for fixed arguments preceding a pack expansion; its separate mixed
  owning/passive regression passes all 32 executions across those modes.
  Direct owned class returns through virtual erased dispatch and stored lambda
  invocation now use exact MIR-proved caller placement. Public wrapper resource,
  unique-owner, and string result fixtures pass 72 executions across all eight
  modes, including producer and later caller failure. This is not completion of
  generic/owned callable composition or the remaining consuming-wrapper contract.
  Gitea #9's two inline owned `expected` payload extractions now retain the first
  prepared payload through the second extraction's CFG. Exact cleanup and partial
  move rejection pass 32 native runs plus eight independent MIR/representation
  mutations. No source-language ownership rule or wrapper-name shortcut changed.
  Fresh owning closure literals now reparent their exact value lifetime into
  prepared argument storage, and native emission constructs the closure at its
  represented creation step. The verifier distinguishes lifetime metadata from
  executable aliases; mutation tests reject forged ownership transfer. Abstract
  generic virtual roots now retain their failure-ABI declaration even without a
  concrete override. The generalized read/mutable fixture passes 16 native cases
  across eight modes. The full 52-test lifecycle suite and focused owning-wrapper,
  census, abstract-contract and expected-payload mutation gates pass. The new
  public-wrapper empty invocation, allocation/clone/replacement failure and
  successful replacement/reset matrix passes all 64 cases across those eight
  native modes. No installed or released completion is claimed.
  The complete existing read/mutable wrapper runtime matrix now passes all
  104 outcomes across the same eight modes. The fresh owning-lambda/later-
  argument failure regression adds 16 passing outcomes. Program-plan admission
  no longer reruns whole-program MIR verification for every initializer probe;
  operation-scoped counter and stale-input mutations, full preflight/text/
  lifecycle suites, emitter census, and owning-wrapper type checks pass.
  Three preflight shape expectations from earlier lowering changes were
  reconciled while retaining equivalent positive and negative coverage. This
  improves iteration speed without completing the consuming-wrapper contract.
  A delegated keyring package at `/private/tmp/gti-keyring` also reports an
  `expected<void, error>` wrapper emission failure after an extern-C call with
  string views and an output pointer; that case is recorded but not yet reduced.
  Public consuming models must preserve erased target ownership
  and field-state rules; this is not completed by admitting nominal receivers.
  Virtual, pure, and interface consuming operators now reuse exact override
  checking and the existing MIR virtual-dispatch/failure ABI. A moved concrete
  implementation passed 24 executions across eight native modes, plus exact
  capability and mismatch checks. The independent
  `consuming_polymorphic_receiver.gti` reproducer still fails at the borrowed
  pointee move; the declaration prerequisite already passes. Do not relax generic
  `Dereference` ownership or recognize a public wrapper by name to admit it.
  The semantic place path currently labels overloaded dereferences generically,
  while `BorrowOriginPlace` retains exact paths only for global/static origins;
  receiver/argument return origins do not establish owned-pointee provenance.
  **Consuming-wrapper lifetime decision accepted:** in
  `combine(std::move(callback)(), later())`, remaining captured state stays alive
  through the outer full expression, matching ordinary consuming receivers.
  The user selected this policy on 2026-09-28. Implementation requires reusable
  owned-receiver projection/dispatch and retained cleanup authority; destroying
  the target at the inner invocation's return would not meet this contract.
  It must not move through an ordinary borrow, leave a reusable receiver
  partially moved, or bypass MIR lifecycle/failure schedules. This decision
  records intended behavior, not completed consuming-wrapper support.
  Ordinary nominal `Owner<Callable>::operator()() &&` forwarding through
  `std::move(this.callable)()` now retains the field in its outer receiver.
  Semantics authorizes only exact immediate invocation (including grouping),
  and HIR/MIR distinguish consumed availability from physical transfer.
  `CallInput(Receiver, RetainedReceiver)` leaves cleanup authority unchanged.
  The new single/nested/extraction runtime matrix passes all 72 outcomes across
  eight native modes, including invocation and later-argument failure. Plain
  extraction still owns a callee-local lifetime. Reference-returning fallback
  targets are rejected before HIR. Replacement and failure cleanup pass another
  24 outcomes. The consuming-receiver suite (including 19 new malformed-MIR /
  native-prologue proof mutations), 52-case lifecycle suite, 24-case preflight
  suite, emitter census, and 128-outcome consuming runtime matrix pass.
  Stored lambda fields now carry that checkpoint for fixed arguments and packs;
  the consuming-receiver suite passes with 12 additional malformed-capability /
  exact-lambda-identity mutations and repeated-use negatives. A minimal native
  capture program proves invocation/later-failure cleanup timing. The full
  fixed/pack, single/nested runtime matrix now passes 96 outcomes across all
  eight native modes. Generic constructors compare executable representation
  without no-op publication comments; unnameable generic destructors retain
  checked primary-template body adapters with distinct instance cleanup recipes.
  The independent representation suite passes, including 32 forged cleanup-form
  and owner-overlay cases, an admitted distinct-body grouping rejection, and
  a source rejection for destructor re-consumption. Lowered-program contract,
  C++ plan, and representation-snapshot suites pass. The public read/mutable
  callback matrix timed out after its six standard/optimization modes; both
  remaining modes passed when resumed independently (104 outcomes across the
  combined runs). The default eight-mode gate remains unchanged.
  Uniquely owned erased pointee dispatch is now implemented through an exact
  receiver-relative owned-pointee summary rooted in the declaration-bound
  private unique-owner borrow operation. Arbitrary borrows/dereferences acquire
  no capability. Ordinary receiver-derived references remain insufficient
  evidence. The accessor-side proof is represented independently as
  `OwnedPointeeProjection`: semantics identifies a direct receiver field ending
  in the private unique-owner capability, concrete HIR preserves that identity,
  and MIR reconstructs the checked borrow/return body. The first slice covers
  only a nonvirtual, zero-argument, single-return accessor over one direct field;
  it does not grant general reference consumption. Caller-side retained invocation
  preserves the accessor's actual temporary CallResult loan for
  execution, consumes availability of the whole owning wrapper field, and retains
  that field's physical cleanup. There are no synthesized caller accesses to
  private wrapper storage or exemptions for other live loans.
  Accessor proof validation passes four concrete standard/renamed-owner cases
  and 69 malformed-summary/body/declaration checks, plus all eight ordinary
  accessor native modes including sanitizers. Existing consuming-receiver and
  owning-function type suites pass. `consuming_polymorphic_receiver.gti` now
  executes through the retained capability. Source-defined public
  `function<R(Args...) &&>` and `move_only_function<R(Args...) &&>` use it,
  including clone, owned captures, and function items. The expanded 80-outcome
  native matrix passes all eight modes, with target failure, later-argument
  failure, empty-owner failure, and primary/secondary destructor failure
  preserving the accepted full-expression capture lifetime. It includes fresh closure construction,
  mutable/read-only borrowed arguments, and nominal/unique-owner results.
  The public allocation-policy matrix passes 96 outcomes with consuming
  empty invocation, allocation failure, and clone-allocation failure.
  Editor hover retains consuming signatures and shared diagnostics reject
  missing moves and repeated invocation, clearing after corrected edits.
  A fresh cleanup-free closure constructor argument now uses its MIR prepared
  Initialize schedule, not an invented source-value drop or deferred native
  argument evaluation. Focused structural checks cover that publication and
  malformed schedules.
  Direct destructor re-consumption is rejected by existing receiver tracking;
  this does not establish a general cross-body rule for destructor reads after
  physical field extraction, which still needs a separate ownership audit.
  Ordinary constructors now share the call-input preparation/rollback path
  with functions and pack-expanded construction, rather than transferring
  argument ownership before later inputs are evaluated. The original
  `consuming_receiver_fields_temporaries.gti` now compiles and runs through
  existing prepared-slot emission, without a new backend shortcut. A new
  fixture covers first/second argument, later scalar, and constructor-body
  failure; all 48 runtime executions across the eight native modes pass.
  The consuming-receiver suite passes, including new mutations for missing
  preparation, early handoff, and missing invocation transfer. The borrowed
  temporary construction emitter test now verifies prepared-slot retirement
  while retaining its activation, cleanup, and access mutations. The fresh
  full lifecycle suite (47 cases), emitter census, and owning-wrapper static
  suite pass. Public command-schema and mutable-reference callback failure
  regressions pass another 24 runs across all eight native modes. Copied
  constructor-pack reuse passes eight additional runs. The non-failing owning
  declaration default in `CleanupMutableFallback` now uses its exact
  Construct/Initialize, Reparent, and field handoff schedule. A focused emitter
  gate rejects four broken lifecycle schedules; its generated-constructor
  receiver lifetime fixture passes all eight native modes. Public consuming
  by-value resource arguments also pass all eight modes with exact cleanup.
  Retained-owner member-template pack variants now preserve explicit concrete
  type arguments even when only the owner contains a closure. Selectors guard
  exact pack arity before forming indexed closure types and compare dimensions
  within that arity family. The combined empty/scalar/passive-closure/owning-
  closure/mixed-pack fixture passes all eight native modes, including reverse
  parameter cleanup. Same-arity cleanup variation was already supported; the
  fix preserves its canonical-body equality checks. The production integration
  gate must now validate the combined result-publication, polymorphic,
  staged-loan, and other callable-boundary cases rather than treating these
  earlier three reproductions as unresolved. Do not whitelist any wrapper or
  allocation function or infer a new AST shape.
  The first combined Release/Ninja production run completed all 275 CTest
  registrations: 223 passed and 52 failed. This is a failing integration gate,
  not delivery evidence. Grouped repairs cover prepared callable/borrowed
  argument lifetimes, concrete-owner selector substitution, generic array
  parameter preservation, constructor field publication, and stale diagnostic
  or representation assertions. The managed-task ownership, callback signature,
  optimizer, generic-owner callback, lambda-pack invocation, and stored-reference
  constructor regressions have subsequently passed focused checks. Remaining
  backend/corpus failures, updated full-suite evidence, loopback HTTP validation
  outside the restricted sandbox, and installed consumer checks still block
  completion. Do not refresh corpus baselines to conceal rejected examples.
- **Installed-consumer evidence (2026-09-28):** the fresh Release/Ninja build
  installs as the `gti_toolchain` component into a clean prefix. A separate
  Release/Ninja consumer configured against that prefix passes all nine
  exported-library tests. Outside the repository, with `GTI_STDLIB_PATH`,
  `GTI_RUNTIME_INCLUDE`, `GTI_RUNTIME_LIBRARY`, and `GTI_VENDOR_INCLUDE` unset,
  the staged CLI builds and runs the recursive callback schema and owned
  Expected-result fixture at C++20/O3, and the consuming-capture lifetime
  fixture at C++23/O0. Exact outputs and exit statuses match their regressions.
  The staged LSP passes signature tooling and executable-entry diagnostics.
  These checks are independent of the combined suite and local installation.
- **Combined completion evidence (2026-09-28):** the full Release/Ninja build
  uses all 12 detected CPUs. `ctest --test-dir out/neovim-local-build
  --parallel 4 --output-on-failure` passes 294/294 tests in 1055.98 seconds,
  including generic/owned transport, allocation and cleanup policy,
  setter/schema workflows, malformed MIR, C++20/C++23 O0/O1/O3/no-elide/
  sanitized execution, and editor behavior. The seven failures from the
  preceding 292-test run were repaired and passed focused reruns before this
  fresh combined pass. Tree-sitter passes 63 corpus cases, highlight checks,
  and parsing of 129 shipped sources; clang-format checks all 363 files.
  `scripts/install_neovim_local.py` succeeds with 12 build jobs, verifies the
  archive and Neovim integration, and installs GTI 0.346.0 locally.
  The optional, non-gating full audit passed its contract, generated,
  composition, malformed-source, and numbered-example checks but could not
  run the intentionally deleted `examples/gti-vs-cpp/verify.py`; that deletion
  is preserved and the optional audit is not claimed wholly green.
- **Publication:** the user approved including the other tasks' work in the
  combined publication on 2026-09-28. `VERSION` advances to 0.346.0; the
  canonical Gitea `main` push initiates the stable release workflow. Successful
  dispatch is the delivery endpoint, not completion of asynchronous packaging
  or cross-host CI. Implementation, production verification, and local
  installation are complete.

### L-RANGE-04: Foundational Range Algorithms

- **State/role:** substantially implemented; `L-RANGE-03` is complete. The
  implemented iterator/sentinel slices include
  multi-parameter concepts, validity-only trailing `requires`, exact
  input-iterator/sentinel/dereference/accumulator capabilities, distinct
  copy-construction capability, `std::forward_iterator`, source-defined
  `std::binary_search`, default and operation-based `std::accumulate`,
  homogeneous `std::inner_product`, unary `std::transform_reduce`, the
  confined predicate algorithms, exact iterator/sentinel `std::find` and
  `std::count`, and repeatable exact-return `std::for_each`. The
  writable/movement capability family is also implemented:
  `std::output_iterator`/`writes_to` (mut dereference plus increment),
  `subscripts_to_mut` (positioned mutable subscript),
  `bidirectional_iterator` (`--`), `random_access_iterator` (`+=`/`-=`), and
  `sized_sentinel_for` (iterator-to-sentinel distance) — with symbolic
  generic-body forms for dereference/subscript/compound assignment, concrete
  satisfaction checks, read-vector-cursor operator coverage, and the
  source-defined `std::copy`, whole-container `std::fill`/`std::transform`/
  `std::reverse`/ordered and comparator `std::sort`, whole-vector predicate
  clients, signed `std::advance`/`std::next`, and `std::prev` clients.
- **Implemented complete-range slice:** exact structural `input_range`,
  `range_of`, `mutable_range_of`, and `sized_range` concepts cover nominal
  ranges and fixed arrays without associated types or public-name recognition.
  Exact range predicate/operation relations now drive generic whole-range
  `all_of`, `any_of`, `none_of`, `count_if`, and `for_each`; `fill` uses the
  mutable exact-element relation. Symbolic range carriers are replaced by
  concrete protocol identities or fixed-array plans before HIR/MIR.
- **Implemented ordered-query composition:** the source-defined slice adds
  `partition_point` and comparator `lower_bound`/`upper_bound`/`binary_search`
  over exact homogeneous keys, preserving checked cursor loans and one owned
  repeatable callback in ordinary GTI. Example 139 demonstrates custom-record
  ordering. Callback failure, exact returned-owner rejection, and allocation/
  traversal-cost fixtures cover the same rules with unrelated cursors.
- **Remaining scope:** indexed element move-out; structural complete-range
  relations for `transform`, `reverse`, and `sort`; and further
  iterator/sentinel overloads behind a demonstrated subrange client.
  Forward-list traversal, heterogeneous search keys/projections and indirect
  binary callable relations remain separate prerequisites, not compiler
  exceptions for these APIs. Iterator-pair in-place spellings remain
  excluded because `begin_mut()` and `end()` from one container overlap by
  design; semantics now diagnoses that conflict before MIR, while read-only
  methods on a parameter-held mutable cursor are supported. Iterator
  assignment, implicit conversions, overload ranking, and a parallel C++
  iterator-category hierarchy remain unauthorized.
- **Exit gate:** no public algorithm name is recognized by the compiler and
  value/borrow/move behavior is covered for every supported range category.

### L-NUM-01: Defined Integer Arithmetic Modes

- **State/role:** done in 0.111.0; systems-readiness arithmetic capability for
  renderer/game and low-level systems clients.
- **Implemented scope:** `<std/numeric>` provides exact overloads of
  `wrapping_add/sub/mul` and `saturating_add/sub/mul` for all eight fixed-width
  integer domains, plus `checked_add/sub/mul` returning
  `expected<T, std::arithmetic_errc>`. All nine public operations are
  non-failing, constexpr-capable within the documented observer subset, retain
  exact semantic/HIR/MIR identities, use the shared private `APInt` authority,
  and have explicit memory-free/non-trapping effects. O0/O3 and C++20/C++23
  execute the same public example. Ordinary operators remain checked and
  failure-producing.
- **Later breadth:** checked division/remainder/shift result families require a
  demonstrated client and exact zero-divisor/invalid-shift error policy;
  constant `error()` observation can widen with the general aggregate
  evaluator. An `unsafe` block never globally disables checks, and native
  overflow flags never become language semantics.
- **Exit evidence:** exhaustive fixed-width boundaries, expected success/error
  construction, bounded constexpr observation and failed-`value()` diagnostic,
  HIR/MIR identity/effects, LSP source identity, and O0/O3 × C++20/C++23 runtime
  parity pass.

### L-FLOAT-01: Binary64

- **State/role:** done in 0.110.0; systems-readiness numeric capability.
- **Scope:** `double` is exact IEEE-754 binary64; `d`/`D` selects it while
  unsuffixed decimals retain binary32 compatibility. GTI-owned width-tagged
  bits cross tokens, semantics, HIR, and MIR. Private `APFloat` owns parsing,
  arithmetic, comparisons, and conversions. Mixed arithmetic promotes to
  binary64, widening is implicit, narrowing is explicit, and the native
  policy verifies both IEEE host representations. TypeContext and vector/SIMD
  work remain separate.
- **Exit evidence:** the binary32 semantic/evaluator/MIR/backend/native matrix
  has binary64 parity across O0/O3 and C++20/C++23; formatter, Tree-sitter,
  LSP, layout, generic numeric, diagnostics, and shipped-example gates pass.

### L-SIMD-01: Bounded Floating-Point Lane Values

- **State/role:** implemented; a source-library systems/game numeric slice.
- **User outcome:** batch float/double updates use fixed-size values and explicit
  lane masks without exposing raw pointers, alignment requirements, or native
  vector ABI types. The public `<std/simd>` classes are ordinary GTI; six
  identity-bound private operations carry exact nonescaping read/read/write
  array borrows through semantics, HIR, MIR verification, and effects.
- **Bounds:** 1..64 float/double lanes, independent arithmetic and comparisons,
  checked owner-array copying, explicit mask reductions, and left-to-right
  summation. Copied native 128-bit chunks and scalar fallback preserve strict
  FP ordering. Integer SIMD, pointer/masked loads, gather/scatter, ISA selection,
  and constexpr execution remain separate work, not silently unchecked cases.
- **Exit evidence:** semantic and malformed-MIR checks; C++20/C++23 O0/O1/O3
  parity; subnormal/NaN/signed-zero/non-FMA checks; forced scalar parity;
  ASan/UBSan; full-range copy failure observed during cleanup; and native
  instruction inspection. Generic class call-result assignment regression tests
  cover ordinary user-defined aggregates independently of the SIMD client.

### L-SUM-01: Payload Enums And Exhaustive Matching

- **State/role:** in progress; prerequisites `D-LANG-01`, `M-OWN-02`,
  `M-LIFE-01`, and `S-LAYOUT-01` are done. The first passive, non-generic
  payload family is implemented across syntax, semantics, HIR, MIR, C++20/23,
  formatter, Tree-sitter, and LSP tooling. The cleanup-free class-payload
  family is also implemented: ordinary class values without active cleanup or
  borrowed state live inside alternatives, constructed from prvalues, named
  copies, or explicit moves (each an exact fused emission vocabulary), with
  copied class bindings armed and dropped like ordinary locals, payload-enum
  struct definitions scheduled after every class definition, and enum
  copy/move/ownership traits folded from payload components. The bounded
  owning-class family is also complete: active-cleanup class operands transfer
  into a variant-aware enum obligation, whole-enum move construction and
  replacement assignment transfer the active alternative, tag-only exhaustive
  patterns observe move-only payloads, and normal/control-flow/defined-failure
  exits drop only the active alternative exactly once. Direct owning values
  also cross ordinary source-defined free functions, non-polymorphic methods
  and ordinary constructors, and exact typed lambdas by value. Ordered
  argument transfer, lexical parameter entry, direct result placement,
  success-only activation, and defined-failure cleanup remain explicit through
  HIR and verified MIR. The first aggregate containment slice is complete:
  an ordinary non-polymorphic class or struct may own the enum in a
  direct field, initialize it in-class or through a constructor, move it with
  the containing value, replace a mutable field, and preserve reverse-order
  rollback after partial construction. Exact concrete generic functions,
  methods, constructors, and owning fields reuse that contract, enabling
  user-defined envelopes, vectors of owning events, and optional inline event
  storage. Prepared-argument failure cleanup and consuming-method result
  placement retain exact MIR obligations. Recursive finite owning sums and
  borrowed patterns are also implemented. Polymorphic/native direct transport,
  direct enum capture, fixed arrays of enums, direct enum result alternatives,
  and stored borrowed payload state remain bounded separately.
- **Implemented slice:** Nominal payload cases construct by exact case calls.
  `switch` accepts qualified case patterns, creates immutable copied bindings,
  and requires a `default` or every variant. Semantic variant identity and
  exhaustiveness survive through HIR/MIR; the C++ backend uses a replaceable
  `std::variant` wrapper without making that library type source semantics.
- **Remaining scope:** Add direct enum lambda capture, consuming payload
  bindings, fixed-array and direct result-alternative transport, generic
  payload declaration completion gates, and deterministic layout in separately proven families.
  The general generic-declaration campaign is active in
  `codex/generic-payload-enums`. The candidate retains ordinary semantic binders
  and exact substituted alternative signatures, including defaults, type/value
  arguments and a final metadata pack. Concrete construction/matching, HIR
  discovery, verified lifecycle and detached representation use the complete
  enum type rather than the primary declaration alone. Forward defaults,
  alias payload validation and nominal deduction have focused regressions.
  The original 22-fixture native matrix passes all 176 configurations with
  19 source-rejection cases; added generic/nongeneric owning-field fixtures
  pass all 16 native configurations. Exact instance LSP signatures and fresh
  dependency diagnostics pass the focused protocol gate. Original A47, A48,
  and A49 reproducers also pass all 24 native configurations. S15 plain-class
  payload emission and its generic/nongeneric publication fixtures pass focused
  native checks. Final review repairs cross-nominal default-ordering, exact
  concrete layout/relocation traversal, snapshot reset and nested executable
  payload storage identity. Focused declaration/backend/optimizer/raw-audit
  gates and nested/receiver/sibling native matrices pass. Two valid-source
  composition gaps remain follow-ups, not new source restrictions: a nested
  local reborrow while two shared sibling payload bindings remain live reaches
  a MIR sibling-conflict error
  ([#121](https://git.internal.howetotech.dev/gti-lang/gti/issues/121)); two
  fresh class constructors with enum arguments inside one alternative call
  exceed the current fused emission staging proof
  ([#122](https://git.internal.howetotech.dev/gti-lang/gti/issues/122)).
  Preconstructing those class inputs is supported. The expanded 28-fixture
  native matrix passes all 224 configurations and the 19 source-rejection
  cases. All 24 broader focused gates pass, with two test-maintenance retries;
  both complete MIR/Lowered corpus censuses and 12 isolated installed-library
  consumers also pass. Further backend review repairs alias-driven phantom
  class-layout discovery while retaining exact enum rows and real payload
  layouts; synthetic catalog and signed-minimum failure-contract checks gain
  independent malformed-input rejection cases. All eight post-correction
  owning gates, the 224 native configurations, refreshed installed consumers
  and both unchanged corpus censuses pass. The standalone 427-case coverage
  gate reports two
  `GTI-S2106` JSON failures (`c137` and `c138`), also reproduced by the optimized
  pre-enum 0.382.1 compiler under native closed-program compilation; their
  expected passing outcomes remain unchanged; the reduction is recorded on
  [the definition-check issue](https://git.internal.howetotech.dev/gti-lang/gti/issues/51#issuecomment-458).
  Reconciliation with canonical PRs 116 and 115 preserves passive native
  call-result and class-field publication. The optimized overlap gates pass
  14/14 and then 10/10 on the resulting main-based tree, including the full
  224-configuration enum matrix, both corpus censuses and the upstream
  publication regressions. These are focused integration checks, not a full
  release-suite claim. The allocator campaign owns the symbolic-primary
  definition-check repair; its published checkpoint is combined only in the
  isolated draft, not released or integrated into canonical main.
  A non-mutating preview identifies the two emitter resolutions: retain the
  passive-default assignment proof together with the stricter ordinary
  construction guard, and consistently use the passive-default helper.
  Subsequent reconciliation with canonical PR126 retains deterministic field
  identity and exact-owner callable transport; all 12 focused overlap gates
  pass, including the enum matrix and both censuses. Tree-sitter regeneration
  has no diff, all 76 corpus cases and 187 shipped sources parse, and
  highlight/locals/source-text checks pass with the correct rebuilt worktree
  parser rather than another worktree's shared cached grammar.
  A subsequent combined-release fixture adds distinct owning enum instances
  in base, derived and enclosing fields, requiring reverse-order cleanup after
  a later derived-field construction fails. This 29th fixture is pending the
  allocator campaign's selected-base-stage implementation; the standalone
  branch reports the existing located backend-vocabulary rejection. The prior
  224-configuration evidence does not claim this new integration fixture passed.
  Draft reconciliation of published allocator checkpoint `514c859e` preserves
  exact enum identities, assignment-shell proofs and generated constructor
  schedules. Combined snapshots reserve MIR95/body80/Lowered15; the merged
  grammar passes all 76 cases. Optimized combined compiler and runtime checks
  remain pending, together with the allocator's selected-base-stage correction.
  The optimized combined build subsequently succeeds. Eighteen of 22 focused
  cross-layer checks pass, including exact enum queries/backend, manual-lifetime
  and conditional-capability contracts/runtime/LSP, cleanup mutations and
  symbolic-definition publication. Both formerly failing JSON coverage programs
  compile natively at O3 and run successfully. Aggregate inventory and stale
  native-default spelling assertions are repaired without dropping tests:
  declaration fixtures retain read-only MIR fields and selected generated
  constructor identities; target fixtures construct/read their selected object.
  The complete compiler aggregate then passes (39.02 seconds). Base body
  vocabulary checks and the reviewed corpus census refresh remain outstanding;
  this is not combined full-release evidence.
  Published base-stage checkpoint `f832985c` is subsequently reconciled in the
  isolated draft. The new base/derived/enclosing enum regression passes all
  eight native configurations (including no-elide and ASan/UBSan), retaining
  exact `Event<Resource, 1/2/3>` identities and cleanup order `22`, `11`, `9`.
  Lifecycle vocabulary and the complete native base-constructor matrix pass.
  The broader base unit fixtures and an old inheritance-spelling assertion
  still require reconciliation with the allocator agent's next checkpoint;
  the earlier aggregate pass precedes this newer base-stage integration.
  The combined 29-fixture matrix first exposes four container clients failing
  MIR storage verification: new allocator trait helpers queried only a primary
  enum ID, losing concrete arguments. All three lookup sites now use exact
  enum types. General conditional-capability tests cover distinct real payload
  policies and metadata-only parameters; no storage or borrow permission is
  relaxed. All six focused gates then pass (110.63 seconds), including every
  one of the 232 native enum configurations and 19 source rejections.
  Reconciliation with canonical `00cc2c09` and published allocator
  `9d3c993a` completes the optimized all-target build. An independent
  integration review preserves identical scoped semantics for both enum
  spellings and exact concrete identities throughout storage, capabilities,
  relocation and emission. A test-only preflight lookup is corrected to use
  the complete enum type. The refreshed enum matrix passes all 232 native
  configurations and 19 source rejections; declaration, backend, integral
  enum and LSP gates also pass. All 76 Tree-sitter cases, 191 shipped sources
  and 486 formatted C/C++ files pass. A fresh isolated installed stage passes
  all 12 SDK consumers, LSP and project-CLI checks, and example 129 runs at
  C++23/O3 with library overrides unset. The general preflight gate exposes
  six constructor/subscript representation assertions requiring review, and
  installed CLI acceptance exposes an empty-owner constructor specialization
  ordering failure. These failures remain visible. The two original goals
  will be delivered together through PR94 only after shared full-suite,
  corpus/census and installed acceptance; PR110 stays published until remote
  integration and absence of residual work are confirmed.
  The six preflight assertions are subsequently updated to the existing typed
  schedules rather than restoring superseded emission routes: declaration
  defaults execute in selected constructors without duplicate native field
  initialization; exact generated construction retains its prepared slot;
  subscripts evaluate once and use loan-rooted load/arithmetic/assign with
  checked reference publication and overflow routing. Missing required target
  representation and erased arithmetic failure-edge mutations remain rejected.
  The complete optimized preflight gate passes (6.65 seconds). No executable
  semantics change in this test-maintenance checkpoint. Shared release
  validation and the empty-owner native ordering correction remain with the
  combined release owner.
  Combined full-suite/coverage-corpus
  validation and upstream release integration remain delivery gates. This
  campaign is not an `expected` implementation. The owner's spelling
  amendment makes `class` optional with identical scoped semantics for both
  declaration forms.
- **Non-goals:** Implicit conversions, open variants, making `std::variant`
  compiler magic, or treating the separately implemented passive native union
  as a tagged sum or C ABI union.
- **Exit gate:** an ownership-capable generic payload family has exact
  partial-initialization/drop state, move/borrow and positive/negative tests,
  deterministic layout facts, and O0/O3 runtime parity. The passive first slice
  already has exact construction, exhaustive diagnostics, and runtime parity.

### L-ERR-01: Error Propagation Operator

- **State/role:** complete in 0.311.0; `M-LIFE-01`, `D-LANG-01`, and
  `D-EXEC-01` provide the cleanup and evaluation substrate.
- **Scope:** Define one explicit propagation form over `expected`, including
  conversion prohibition, early-return cleanup, source spans, HIR/MIR control
  flow, formatter, Tree-sitter, and LSP behavior.
- **Implemented slice:** Prefix `?expr` evaluates one `expected<T, E>` once,
  produces `T`/`void` on success, and returns `unexpected(E)` on failure only
  from an enclosing `expected<U, E>` callable. Named places copy; prvalues and
  explicit `std::move` operands move. HIR retains the selected extraction and
  transport plan; `mir-v53`/`mir-body-v46` carry explicit mutually exclusive
  early full-expression endpoints, cleanup, and return control flow; C++20 and
  C++23 spell the same verified behavior.
- **Completion evidence:** Positive and negative compiler tests cover exact
  types, invalid contexts, copy/move selection, lambdas, multiple ordered
  propagations, formatter stability, MIR verification, and both backend
  spellings. The runtime matrix covers scalar, `void`, owning temporary, named
  move-only, named copyable, and move-only error alternatives at C++20/C++23
  and O0/O3; Tree-sitter and LSP fixtures cover parsing, highlighting,
  formatting, and the `GTI-S2086` source range.
- **Exit gate (met):** success and propagation paths preserve exact
  result/error types, drop every live obligation once, retain source-facing
  spans through HIR/MIR, and pass formatter/parser/LSP/runtime plus full
  repository validation.

### L-OP-01: Broader Operators And Capability Policy

- **State/role:** first family implemented; the demonstrated client is
  renderer/math domain values (`vec2`/`vec3`-shaped types and a second
  integer-scalar domain type) driving one generic body.
- **Implemented family:** four structural capabilities — `adds<T>`,
  `subtracts<T>`, `negates<T>`, and `scales_by<T, Scalar>` — each proving one
  exact public read-only member returning an *owned* value of the domain type.
  They are spelled publicly as `std::addable`, `std::subtractable`,
  `std::negatable`, and `std::scalable_by`, composing into
  `std::vector_space<T, Scalar>` with `std::copyable<T>`. Generic bodies gain
  the matching symbolic candidates for binary `+`/`-`, unary `-`, and scalar
  `*` on a constrained type parameter; concrete instances re-select the real
  member declarations. `std::lerp` and `std::negated` in `<std/numeric>` are
  the source-defined clients, with exact `float`/`double` overloads of
  `std::lerp` for built-ins.
- **Deliberate boundary:** the family is member-only, matching the language's
  member-only user-operator rule. A built-in numeric does not satisfy it —
  admitting one would resolve its operators as symbolic member calls and drop
  the canonical local failure outcomes checked integer arithmetic must retain
  (observed directly while implementing this row). A borrowed operator result
  is also excluded, since the produced value would be tied to a receiver whose
  lifetime the requirement cannot name.
- **Remaining scope:** compound domain operators (`+=` and friends), a
  matrix/product family with distinct operand and result domains, and any
  ordering or hashing capability for domain values are separate families under
  the same one-family-at-a-time rule.
- **Exit gate (met for this family):** one lookup rule (exact member-only
  selection, no conversion), focused positive and negative diagnostics for each
  capability, and O0/O3 × C++20/C++23 runtime evidence over two domain types.
  No native C++ overload behavior is needed to explain accepted source.

### L-OWN-01: Shared And Weak Ownership

- **State/role:** the requested strong-ownership and borrowing-composition
  slice is implemented in 0.354.0 (2026-09-30), with explicit cloning and
  read-only shared access. Custom copy and move constructor bodies remain
  out of scope by user decision. Weak ownership and recoverable shared
  allocation remain open in this broader row.
- **Scope:** Implement source-defined `shared_ptr` and `weak_ptr` policy over
  narrow trusted control-block operations, and provide the selected
  recoverable unique/shared allocation factories. Make copy, move, observation,
  locking, drop, cycle limitations, and allocation failure explicit.
- **Accepted shared-access rule:** pointee access is read-only. A mutable
  handle permits reset and swap, not exclusive access to an aliased pointee.
  Shared mutation uses checked synchronization such as `mutex<T>` or
  `atomic<T>`. Reference-count synchronization must not be confused with
  synchronization of the pointee. Cross-thread transfer/sharing must follow
  the implemented capability rules and protect both access and last-owner
  cleanup; control-block atomics alone do not grant either capability.
- **Duplication policy (2026-09-30):** the user withdrew custom copy constructor
  bodies for further design consideration. Preserve compiler-derived copy
  behavior and the existing copy/move `= default` / `= delete` declarations.
  Shared handles duplicate through explicit `clone()`, not an implicit
  user-code copy hook or public-type compiler exception.
- **Accepted movement policy (2026-09-30):** after reviewing the ownership and
  optimization implications, the user withdrew custom move constructor bodies
  from this campaign. Movement remains compiler-controlled ownership transfer;
  `Type(Type&&) = default` / `= delete` remain policy declarations, not user
  hooks. Custom transformations belong in explicit consuming methods or
  factories. Shared ownership uses generated movement of its private handle,
  not a public `std::shared_ptr` name exception. Do not reintroduce custom move
  bodies as a prerequisite of custom copying or shared ownership.
- **Archived copy research (not active implementation):** local branch
  `codex/archive-custom-copy-constructors`, checkpoint `4cdec2ed`, preserves
  the experiment and its borrowing-composition baseline for possible reuse.
  Explicit/implicit copy, capture, nested-wrapper, and rollback fixtures had
  focused native-matrix evidence. Conditional-result work was unfinished;
  array/base and broader composition remained unresolved. This is a WIP
  snapshot, not a validated feature or release. Its copy-only compiler changes
  and tests have been removed from the active tree.
- **Rollback checkpoint:** a fresh Release rebuild passes all 42 declaration
  cases, both custom-body rejection probes (`GTI-S2020`), the five borrowing
  pipeline/runtime/LSP gates, and focused stored/nested/owner-return checks.
  This establishes separation of the experiments, not shared-owner delivery.
- **Shared-owner implementation slice:** represent a private move-only strong-owner
  capability by trusted declaration identity, not a public wrapper name or
  the legacy implicitly copyable shared-pointer type. Ordinary GTI owns
  explicit cloning and handle policy. A read-only pointee borrow protects the
  particular strong handle; other handles may alias its allocation. Clone
  and release have control-block effects, and distinct handle roots must not
  imply disjoint pointees. Last-owner release must run the exact verified MIR
  cleanup recipe and propagate defined failure before freeing storage.
  Constructor failure must reclaim unpublished allocation state. Start with
  conservative cross-thread capabilities requiring both pointee sharing and
  transfer permission; address-stable synchronization objects may require a
  separate general allocation/remote-cleanup distinction, not a mutex-name
  exception.
- **Implemented slice:** the private nominal capability,
  ordinary source wrapper/factory, semantic ownership and concurrency traits,
  MIR operation/failure contracts, and recursive cleanup recipes are present.
  Focused optimized tests reject implicit copying, writable pointee access,
  live-borrow reset, use-after-move, escaping references, and borrowed payloads;
  malformed MIR tests cover handoff, traits, failure edges, and capability shape.
  The eight-mode native failure matrix passes empty dereference, control-block
  allocation rollback, primary final-release failure, and secondary cleanup
  failure. Passive/recursive/container/fixed-array and managed-thread integration
  passes focused tests, including owned worker results and immediate stop at a
  secondary capture-cleanup failure. Legacy cleanup regressions found in the
  first broad run were corrected. All 325 registered gates have passing
  results after the three final test/manifest corrections were rerun; clean
  installed-library consumers pass 9/9, and installed shared ownership,
  paired-span, failure, thread-result, URL-chain, and LSP probes pass outside
  the checkout. The initial pointee family matches unique allocation: classes,
  structs, and scalars without borrowed state, not direct Expected or payload
  enum allocations.
- **Parallel borrowing client:** source-defined `std::paired_span<L, R>` over
  independently typed contiguous buffers drives
  multiple independent read-only stored dependencies, including checked
  per-field constructor paths, complete call/return origin sets, alias copies,
  move forwarding, and owner invalidation. Projections conservatively retain
  the complete owner set; independently shortening unrelated projection loans
  and reference extraction from a multi-path by-value parameter remain explicit
  precision boundaries. Preserve the implemented single exclusive-dependency
  contract. Do not represent two independent owners as one synthetic owner or
  infer their lifetimes in the backend.
- **Delivery evidence:** positive and negative semantic cases; explicit
  selected-constructor and dependency identities in HIR/MIR; independent
  verifier mutations; once-only values and cleanup through construction/body/
  later-argument failures; generic and container composition; C++20/C++23
  O0/O1/O3, no-elide and sanitizers; LSP diagnostics and navigation; source
  library examples and installed consumers; full optimized completion suite.
  Both coordinated implementations were reviewed together. The optional audit
  passed contracts, 24 typed differential cases, four composition cases,
  100 malformed-source mutations, and 85 examples in four variants; its final
  comparison showcase could not run because `examples/gti-vs-cpp/verify.py`
  is absent. This is not a complete optional-audit pass.
  Weak ownership remains in the broader row's scope; it is not silently
  counted as delivered by completing the requested shared-owner campaign.
- **Exit gate:** exactly-once object/control-block destruction, weak expiration,
  cycles documented as a limitation, allocation failure, and HIR/MIR lifecycle
  tests.

### L-VALUE-01: Optional And Foundational Value Utilities

- **State/role:** in progress. The production move-only `std::optional<T>`
  sub-slice is complete over exact declaration-bound inline one-slot storage;
  pair breadth, swap/exchange, limits, and broader `expected` observers remain
  systems-readiness standard-library work.
- **Scope:** Implement `optional<T>` over checked one-slot storage, then
  complete pair, comparison, swap/exchange, limits, and `expected` observers
  without compiler-recognized public names.
- **Implemented optional boundary:** empty and engaged construction,
  observation, checked read-only/exclusive access, reset, variadic emplacement,
  move-only payload transfer, deterministic wrong-state failure, and
  exactly-once cleanup are complete. The private representation is inline and
  allocation-free. Copy construction and both assignment forms fail closed;
  source-defined conditional special-member assignment remains a later
  prerequisite, while `reset`/`emplace` provide explicit replacement now.
- **Exit gate:** empty/engaged construction, move-only values, assignment,
  destruction, failure, generic constraints, and constexpr-safe constants.

### L-CONST-01: Remaining Bounded Compile-Time Programming

- **State/role:** the staged MIR constexpr foundation is implemented and locally
  validated; publication is held at the user's request. It enables source-defined numeric helpers,
  passive records and fixed-array metadata tables, including read-only text
  inspection for game-engine asset descriptions. Declaration-owned generic
  defaults retain exact identity through the same frontend-owned constant
  model. The remaining AST interpreter is a compatibility path for unmigrated
  profiles, not the intended endpoint for future execution facilities.
- **Implemented scope:** The existing evaluator admits concrete non-pack type
  and `uint64_t` value-generic calls, exact scoped enums, concrete fixed arrays,
  fully initialized structural class/struct values, ordinary constexpr
  constructors, and read-only instance methods. Constants retain exact
  declaration/type/value identity through semantics and required HIR/MIR data;
  the backend only materializes an already-proven value. One shared 4096-step
  budget and 64-call-depth limit cover recursive/default aggregate work.
- **Text-inspection slice (implemented):** Literal-backed string-view `size()`,
  `empty()`, and checked unsigned-byte indexing now use backend-neutral constant
  kernels selected by resolved intrinsic/type identity. Focused frontend and
  O0/O1/O3, C++20/C++23 differential tests cover an asset-path inspector,
  embedded NULs, non-ASCII bytes, bounds, runtime-input rejection, and shared
  budgets. The early kernels share runtime operation meaning and are consumed
  by the staged executor. A separate completed-body HIR boundary
  now permits early scalar/text calls through canonical MIR while preserving
  the ordinary whole-program seal. Continue extending this staged evaluator to
  subsume the AST call/control-flow driver and reuse these text kernels, not
  maintain a second text rule.
- **MIR-execution foundation (implemented and locally verified):**
  `mir_constant_evaluator` executes verified canonical MIR for scalar/text
  functions, local mutation, CFG, and
  recursive constexpr calls with shared limits and located failures. Conversion
  policy has moved from the semantic AST driver into the shared constant
  service. Differential tests cover existing frontend constants, integer and
  floating domains, byte inspection, failed calls/arithmetic, malformed MIR, and
  optimizer-independence. Completed free-function closures, including exact
  concrete type/value generic instances, are now
  wired into semantic constant requests using real source failure metadata.
  Early-only tests prove array extents, static assertions, and branch selection
  actually execute MIR; incomplete roots and whole-program publication reject.
  Generic staging lazily forks semantic state, retaining enclosing scopes and
  live loans and rejecting invalid concrete static assertions. It does not
  reenter the caller's mutable body-analysis context.
  Fixed-array local storage, default construction, nested indexed mutation,
  independent copies, and by-value array parameters now execute through the
  same MIR place model; element work and storage copying are budgeted.
  Focused early-only, bounds/resource-negative, and native parity checks cover
  this family. Passive structural record parameters, copies, field/index
  mutation, and returned records now retain exact class/field identity through
  staged catalogs and MIR. Recursive argument validation rejects incomplete or
  mistyped records; passive lifetime bookkeeping cannot execute active cleanup.
  Explicit completed constexpr constructors now retain semantic eligibility in
  HIR/MIR and execute explicit field writes, including trivial fixed arrays.
  Nested/generic construction and read-only receiver methods reuse ordinary
  selected calls and `This` places; std::array construction/observers and
  unrelated user records exercise both paths. Constructor failure locations
  retain their original initializer expression. Direct constructor and
  read-only/static method roots now use the same staging boundary, including
  concrete generic owner identity. Generic constexpr declarations now permit
  ordinary runtime-only owning substitutions without relaxing required constant
  evaluation or normal ownership checks. Source-defined `min`, `max`, `clamp`,
  `lcm`, `midpoint`, binary32 `abs`, and passive `pair`/`make_pair` now exercise
  the foundation, with actual MIR execution and O0/O1/O3 C++20/C++23 parity.
  Constant aggregate factories retain exact concrete type identity, preventing
  runtime owning instances from inheriting native constexpr requirements.
  Checked read-only reference parameters, local bindings, and returned
  projections now execute through ordinary MIR loans, with immutable referent
  observations and no persisted constant references. `std::array::at`, `front`,
  and `back` and an unrelated record exercise the same boundary; mutable and
  stored-reference facilities remain excluded. A game-engine-like asset table
  now composes literal-path inspection, passive record construction, fixed-array
  storage and checked reference traversal, with staged execution counts and
  six native parity configurations. Passive array constructor failure proofs
  now require their explicit field-write stage, matching the scalar rule;
  an aggregate computation alone cannot stand in for object initialization.
  Read-only arithmetic/bitwise, comparison, subscript, dereference, and call
  operators now share the named constexpr-call staging boundary. Ordinary
  selected targets, concrete generic owners, and read-only reference results
  execute through MIR, including nested nominal call-operator invocation.
  `std::array::operator[]` and unrelated math records exercise the same rules;
  no operator body is added to the legacy AST evaluator. Arrow/contextual-bool
  operators and mutable/consuming receivers remain outside this slice.
  Declaration-owned default
  schedules remain migration
  work for library clients. Generated native construction is not proof of const
  eligibility or completed semantic analysis. Value/type-dependent
  `if constexpr` selection now defers only unselected arms and branch-dependent
  flow proof in primary analysis; independent assertions and ordinary name/type
  checks remain active. Concrete analysis reruns return, movement,
  receiver-restoration and borrow checks, and HIR rejects an unselected branch.
  Staged closures publish exact rows for enums, including generic enums, named
  only by an instance's type arguments. Their absence previously failed MIR
  verification and fell back to the AST driver, which cannot select dependent
  branches; member-requires probes over enum arguments cover this path.
  Nested required predicates stage from the canonical base snapshot with a
  shared execution budget and bounded staging depth. Focused fixtures cover
  recursive generic calls, discarded invalid code, constructors, methods and
  owner restoration. The combined production and installed-consumer validation
  below covers this final slice alongside the reconciled library changes.
  Unsupported profiles still retain the legacy driver; arithmetic/resource
  failures never fall back. Completion requires the combined production,
  installed-consumer and tooling gates in addition to these focused clients;
  implementation of the staged bridge alone is not sufficient evidence.
- **Foundation completion evidence (2026-10-01):** the combined Release build
  has passing evidence for all 350 registered CTest gates: 348 passed in the
  full run, the compiler aggregate passed after correcting two manifest entries'
  order, and HTTP passed with the localhost socket access denied by the sandbox.
  All 21 constexpr test groups, nine constexpr runtime gates, 11 clean installed
  library consumers, staged asset-table/CLI/LSP/project checks, and Tree-sitter
  corpus/highlight/source checks pass. The optional audit passes its contract,
  differential, composition, malformed-source, and 87-example/four-variant
  matrices, but its final comparison runner is absent; it is not a full audit
  pass. This completes the reusable-foundation outcome, not every remaining
  constexpr profile below. These are macOS arm64 results, not unrun platform or
  release evidence.
- **Proposed evaluator direction:** Semantics remains the authority for whether
  an expression is permitted in a constant context and remains the consumer of
  the resulting `ConstantValue`. Execution should progressively move out of the
  semantic AST walker into a constant-evaluation service backed, where
  coherent, by canonical unoptimized body-local MIR plus ordinary MIR
  verification and an additional const-eligibility verifier. That profile must
  reuse MIR operations, places, CFG, selected calls and constructors, concrete
  generic identity, and source provenance; it must not introduce an unrelated
  CTFE bytecode or second type system. Eligibility is structural over typed
  operations and effects, not public `std::*` names, optimizer success, emitted
  C++, or backend behavior.
- **Phase constraint:** Full post-semantics MIR is too late for every constant
  request. `if constexpr`, static assertions, array extents, and value-generic
  arguments can determine which semantics and types are valid before complete
  HIR/MIR exists. Migration therefore needs lazy or staged typed-body lowering,
  or a deliberately small early constant kernel; it cannot merely append a MIR
  interpreter after the existing frontend pipeline. The early kernel must not
  grow into a second full execution engine.
- **Extension rule:** Every new constexpr feature must audit the corresponding
  runtime MIR rule and either migrate a coherent vertical slice to the proposed
  service/profile or explain the blocking prerequisite for a bounded interim
  AST-evaluator addition. Interim work retains target-correct checked
  operations, exact identities, source-located diagnostics, deterministic
  step/call-depth limits, and compile-time/runtime differential evidence. Its
  plan must say how overlapping AST execution is removed or subsumed rather
  than accepting two permanent meanings.
- **Remaining scope:** Mutable/stored references, allocation, receiver mutation,
  payload enums, unions, inherited or cleanup-owning aggregates, generic packs,
  arrow/contextual-bool operators, consuming receivers, and polymorphism remain excluded. Do
  not widen exact qualified specialization into partial specialization as a
  side effect.
- **Initial exit gate:** met by positive/negative identity and limit tests plus
  an O0/O3 and C++20/C++23 runtime-parity matrix whose result is independent of
  native C++ constant evaluation.

### L-TEXT-01: Formatting, Parsing, And Text Policy

- **Library completion campaign (0.355.0):** owning-string search/slicing/editing,
  checked integer and binary32/binary64 parsing, real filesystem operations,
  and executable `list`, `deque`, `set`, `unordered_set`, and `queue` are
  implemented and locally verified. General prerequisites include nested-owner
  text-view relays, constrained collection hooks through ordinary semantic
  requirement checking, and consistent mixed generic field-initializer catalogs;
  unrelated user-type regressions cover the compiler rules. All 335 production
  gates have passing evidence: 334 in the integrated run and the sandbox-blocked
  HTTP gate in a socket-enabled rerun. The refreshed 93-case stdlib sweep,
  both 86-example corpus gates, editor checks, eleven installed consumers, and
  staged archive smoke checks pass. Broader formatting work below remains open.
- **State/role:** in progress. Canonical base-10 conversion and direct stdout
  output are implemented for every fixed-width integer. Bounded sequential
  `{}` replacement is implemented in ordinary GTI for heterogeneous booleans,
  characters, fixed-width integers, binary32 floats, string views, and
  explicitly transferred owning strings through `std::format`,
  `std::try_print`, and `std::try_println`; malformed patterns,
  argument-count mismatches, and unsupported arguments return `format_errc`
  before output. The source-text contract sub-slice is complete. This
  replacement slice can proceed without completing `L-RANGE-03` or the remaining
  `L-CALL-01` work: exact source-defined argument adapters, indexed counted text,
  a bounded call-pack fold, and scalar byte output are sufficient.
  Public formatter customization is now implemented by `L-UP-13` with exact
  member-signature concepts and constrained pack calls. Range-oriented text
  algorithms retain their applicable range/callable prerequisites. Each additional
  float width joins formatting/parsing in its own numeric sub-slice rather than
  blocking the whole text layer.
- **Implemented source-text sub-slice:** Source units are strict UTF-8 with one
  optional byte-zero BOM, exact byte identity/offsets, LF or CRLF input, LF
  formatter output, ASCII identifiers/tokens, and Unicode scalar values in
  comments and string literals. The lexer rejects invalid UTF-8, lone CR, and
  non-ASCII syntax before later phases can cascade; the formatter, source
  manager, Tree-sitter grammar, and UTF-16 LSP boundary follow the same policy.
- **Remaining scope:** State the public runtime byte/UTF-8 policy,
  owning/view conversions, numeric parsing, formatting, and explicit failure.
  Prefer ordinary GTI algorithms and narrow host calls over exposing C buffers.
  The current formatter recognizes automatic or numeric-indexed fields,
  escaped braces, bounded presentation specifiers, binary32/binary64 values,
  and public exact `std::formatter<T>` specializations. It validates before
  stdout; named/dynamic fields and compile-time literal validation remain
  later breadth. Named move-only strings use an
  owner-tied view because GTI does not yet admit `Args&...` reference packs.
  Recoverable counted stderr output is now supplied by `L-HOST-01`.
- **Implemented binary32 text sub-slice:** `std::to_string(float)` plus the
  `std::print`/`std::println` binary32 overloads and the matching
  `std::format` adapter render an optional sign, the integer part, a point, and
  exactly six fractional digits, with `nan`, `inf`, and `-inf` for non-finite
  values. The digits are the stored value's *exact*
  decimal expansion, rounded to nearest with ties to even at the sixth place,
  produced by ordinary
  GTI arithmetic with no host call and no wider-float round trip: a magnitude
  at or above 2^24 is normalised by exact halving and its mantissa's decimal
  digits are doubled, while a smaller magnitude extracts fractional digits in
  binary64, where a binary32 fraction times ten needs at most 28 significant
  bits and is therefore exact. Output is pinned byte-for-byte against C's
  `printf("%f", (float)x)` across O0/O3 and C++20/C++23.
- **Named next sub-slices:** literal format validation (`L-UP-14`), named and
  dynamic fields, and client-driven range/text breadth. Binary64 text/parsing
  and public formatter customization have separate implemented slices.
  Exact package-scope specializations satisfied the heterogeneous dispatch
  prerequisite; general signature concepts now make the public protocol checkable
  without compiler recognition of `std::format` or any public string type.
- **Exit gate:** the selected byte/UTF-8 policy is normative; owning/view and
  numeric round trips cover valid, invalid, boundary, and allocation-failure
  cases; APIs expose no native buffer lifetime and behave identically at O0/O3.

### L-HOST-01: Remaining Hosted Services

- **State/role:** active. Counted stdin/stdout/stderr, file open/read/write,
  seek/tell, standard-stream flush, and exact synchronous child-process
  execution are implemented through ordinary GTI wrappers over versioned
  runtime ABIs. Exact signed-nanosecond monotonic and wall-clock observation is
  also implemented. The remaining independent minimum service is process
  environment access.
- **Scope:** The remaining systems-ready minimum covers process environment
  access.
  Deterministic PRNG plus nondeterministic seed, process arguments, and the
  bounded I/O family are implemented. The first public process slice owns exact
  argv without shell expansion, recoverable create/execute/wait errors, and
  ordinary-exit versus signal status while inheriting the current environment,
  working directory, and standard streams. Capture/redirection, overrides, and
  asynchronous child ownership remain client-driven follow-ons.
  The first chrono slice provides distinct steady/system points, checked
  elapsed subtraction, and normalized Unix timestamps; calendar, time-zone,
  formatting, and generic-duration breadth remain client-driven follow-ons.
  Recoverable failures return `expected`; public APIs expose no backend handles
  or raw C buffers. Broader filesystem traversal/watch, connected networking,
  and traffic buffers are client-driven sub-slices after a demonstrated need or
  their native-record and borrowed-buffer contracts exist.
- **Exit gate:** each selected service family has a target capability, exact
  ownership/failure contract, installed-toolchain runtime test, unsupported-
  target diagnostic, and no public backend handle or raw-buffer lifetime.

### L-CONT-01: Vector And String Completion

- **State/role:** active bounded vector slice; complete range integration still
  depends on `L-RANGE-03`; systems-readiness implementation for the
  renderer/game workload.
- **Scope:** Indexed vector insertion/erasure for move-only elements is
  implemented through checked private slot shifts. Complete iterator-position
  forms and corresponding string operations still require exact invalidation,
  owner-dependency, failure, and range behavior. Range
  sorting/search/partition/copy/move algorithms remain owned by `L-RANGE-04`.
- **Non-goals:** associative containers, allocator customization, or container
  proliferation before the shared protocol and benchmark evidence exist.
- **Exit gate:** insertion/erasure/string operations have exact invalidation
  and failure behavior across move-only and borrowed elements; semantics, MIR,
  runtime, sanitizers, and range integration agree.

### L-CONT-02: Associative Containers

- **State/role:** bounded implementation complete and locally validated.
  Ordinary source-defined `std::map` and `std::unordered_map`
  unlock command registries and keyed application state. They use the existing
  checked owning storage and allocation/failure contracts, not a new allocator
  model or public-name compiler intrinsic.
- **Implemented:** AVL ordered storage and bucket-chain hashed storage;
  source-defined comparison/hash/equality constraints; owning keys/values;
  lookup, insertion, erasure, iteration, and capacity/load-factor operations.
  Insertion returns ordinary `std::pair<iterator, bool>`, enabled by one checked
  external-owner dependency through a nested nominal field path. Concrete
  class aliases expose the corresponding iterator and key/value aliases.
- **Safety boundary:** iterator and reference loans conservatively protect the
  whole container. Nested carriers retain exactly one borrow path; multiple
  borrowed fields, reseating, nested lifecycle guards, and hidden owner escapes
  remain rejected. This does not adopt C++ iterator invalidation rules or permit
  unchecked embedded references.
- **Remaining breadth:** allocator customization, `operator[]` (which needs
  conditional operator requirements), multi-owner carriers, finer-grained
  invalidation, and comparative performance tuning are separate follow-ups.
  The current gates cover capability rejection, tree balancing, hash collisions,
  ownership/cleanup, failure behavior, all selected native modes, and editor
  integration; they do not claim a comparative performance benchmark result.

## Phase O: Optimization And Backend Migration

Optimization may proceed in parallel with language design, but no transformed
operation controls output before its lifetime, failure, and effect contract is
complete.

### O-MIR-01: First Shadow Transform With Its Minimal Editor

- **State/role:** complete and superseded by stable proof-carrying rewrite
  infrastructure; systems-readiness implementation foundation.
- **Scope:** The original shadow slice introduced one constant fold and a
  minimal editor. Its temporary block-index addressing has since been removed;
  current edits use stable `{MirBodyAddress, MirInstructionId}` evidence,
  atomic apply/repair, verification, and explicit analysis invalidation.
- **Non-goals:** a general pass manager with no transforms, incremental
  dominance, `LoopInfo`, or new optimizations justified only by LLVM
  availability.
- **Exit gate:** shadow results match the current compatibility fold; malformed
  patches fail verification; deterministic dumps and effect/failure tests pass.
  Production use of the fixed-width-integer transformed result landed through
  `M-BACK-01`/`scalar-leaf-v1` plus eligible M-BACK-02 `scalar-cfg-v1` and
  `scalar-direct-call-v1` bodies; broader shadow-fold coverage was not their
  prerequisite.
- **Evidence:** the compatibility fold now uses stable
  `{MirBodyAddress, MirInstructionId}` identity and typed transformation
  records. Independent source-MIR replay validates every replacement before
  `LoweredProgram` publication. Primitive integer, binary32/binary64,
  character, boolean, and null grouping identities fold at `-O1+`; strings,
  trapping operations, and dynamic values remain conservative near-misses.
  O0 is byte-identical and malformed/stale/duplicate records fail closed.

### O-MIR-02: Per-Instance Effects

- **State/role:** complete foundation; broader effect precision is
  client-gated.
- **Implemented:** every concrete function, constructor, destructor, and
  lambda carries a verified summary for place/unknown-memory access,
  allocation, runtime and user-code invocation, traps, moves, drops, loans,
  synchronization, and receiver/argument escape. A deterministic call-graph
  fixed point derives summaries from complete MIR. Exact acyclic static calls
  may refine instruction effects; recursive SCCs, virtual dispatch,
  declarations, native boundaries, and unresolved calls remain conservative.
  Verification rejects under-approximation and permits conservative
  over-approximation. The older defined-failure facts remain their separate
  exact failure contract.
- **Remaining scope:** add precision for general transformations that can prove
  an existing conservative boundary more exactly. Define consumers and validate
  their performance; no prior measured speedup is required.
- **Exit gate:** separate-compilation assumptions are not invented; unknown or
  recursive calls remain safe barriers; focused optimizer tests cover each
  effect dimension.

### O-MIR-03: Local CFG/Value Optimization

- **State/role:** bounded production slice complete; SCCP continuation is
  complete in `O-MIR-05`.
- **Implemented:** scalar non-owning block parameters and edge arguments,
  conservative acyclic scalar-place promotion, bounded literal/compute/branch
  folding, unreachable-block cleanup, and dead unused computation removal.
  The pass/analysis manager revisions and conservatively invalidates all
  current analysis families. Current transformations are deterministic, typed,
  independently replayed, and consumed through the one lowered backend input.
- **Scope:** this row is closed; further CFG/value work requires its own
  numbered row, proof records, verifier coverage, and measured client.
- **Non-goals:** inlining, devirtualization, bounds-check removal, or loop
  analysis without a concrete proof/client.
- **Exit gate:** satisfied for the implemented local pipeline by verifier,
  report determinism, native-`-O0` structural tests, and O0/O1/O2/O3
  differential runtime coverage. Apply the same gate to every later pass.

### O-MIR-04: Proof-Carrying Safety Optimization

Historical tracking ID: issue #36.

- **State/role:** first production slice complete. O2/O3 run separate bounds,
  initialized-storage-state, and arithmetic-overflow discharge passes over one
  canonical body-local safety analysis; O0/O1 remain unchanged.
- **Implemented scope:** interval and branch-predicate proofs discharge
  in-range fixed-array scalar reads and non-overflowing checked integer
  add/subtract/multiply/negate. Exact sparse-storage state discharges read and
  mutable-read bounds plus initialized-slot outcomes only after a successful
  exact-slot construct not invalidated by destroy or an opaque mutation.
  Discharges retain the checked MIR instruction and failure CFG, name the exact
  local outcome, and carry stable related block/instruction/value/place
  identities. Controlled editing reruns the canonical analysis, coherence
  independently replays every proof from source MIR, and all backends consume
  only the sealed optimized instruction metadata.
- **Remaining scope:** widen one measured operation family at a time. Array
  writes/borrows, prefix storage, division/remainder/shifts/conversions,
  interprocedural range facts, and loop-derived safety proofs remain checked
  and must bring their own exact proof and evidence. Place-alias facts already
  refine mutation invalidation but never discharge a check by themselves.
- **Non-goals:** deleting checks by optimization level, recognizing public
  wrapper names, treating emitted C++ shape as proof, or replacing GTI's
  left-to-right intermediate overflow behavior with a widened final check.
- **Exit gate:** satisfied for the implemented families. Every removed check
  carries a reproducible proof tied to GTI places/ranges/effects and emits a
  deterministic applied optimization remark identifying the operation family,
  body, source site, and proof. Forged or stale proofs fail verification, and
  near-miss cases retain the check. O0/O2/O3 native-`-O0` differential tests
  preserve successful results, failure category and origin, cleanup,
  alias/mutation barriers, and boundary values. Native
  vectorization remarks and assembly may support a performance conclusion but
  never establish semantic validity. A loop-versioned fast path is permitted
  only when its side-effect-free preflight and checked scalar fallback preserve
  failure order, cleanup, and partial effects exactly.

### O-MIR-05: Deterministic Local Dataflow And SCCP

- **State/role:** complete bounded production slice. O2/O3 run SCCP; the
  existing safety analysis uses the shared solver at every optimization level
  where safety facts are requested.
- **Implemented:** one typed monotone forward solver owns deterministic FIFO
  scheduling, canonical terminator-slot executable-edge identity, update
  counts, bounded widening, malformed-CFG and block-argument rejection,
  monotone executable-edge growth, and a hard convergence limit. Safety
  retains its prior interval/storage lattice and exact proof results, including
  switch-default propagation; an independent FIFO oracle and the full safety
  suite cover migration equivalence. SCCP is the first new client and uses
  `unknown -> exact typed constant -> overdefined` facts over scalar non-owning
  SSA. Exact literal/branch edits carry stable evidence and independent
  source-MIR replay.
- **Non-goals:** place or memory propagation, ownership/loan facts, speculative
  trapping operations, unbounded fixpoint iteration, or using native optimizer
  results as evidence. Switch and invoke edge selection remain conservative.
- **Exit gate:** deterministic repeated results, safety-equivalent proofs,
  positive branch-merge propagation, conservative unknown/effect near misses,
  canonical else/default edge identity, malformed CFG/argument/transfer
  rejection, report schema/provenance coverage, and O0/O1 identity all pass.

### O-MIR-06: Natural Loops And Bounded Relational Ranges

- **State/role:** complete analysis foundation; no loop transformation is
  enabled by this row.
- **Implemented:** dominance backedges derive natural loops; same-header
  latches merge; strict containment produces a deterministic loop forest; and
  multi-entry SCCs are marked irreducible. Reducible loops record
  side-effect-free scalar invariants and a bounded single-latch affine
  induction through MIR block parameters with one literal initial value, one
  positive literal add/subtract step, and an optional invariant relational
  continuation bound. A predecessor with repeated edges to the header and
  ambiguous block arguments produces no induction fact.
- **Non-goals:** polyhedral models, unrestricted symbolic algebra, recurrence
  solving across calls or memory, irreducible-loop transforms, LICM,
  vectorization, loop versioning, or mutation of MIR.
- **Exit gate:** nested/merged-loop identity is deterministic; malformed or
  unsupported or repeated-edge-ambiguous recurrences produce no fact;
  irreducible CFGs are marked; and a hand-built block-parameter loop proves the
  exact induction/range record.

### O-MIR-07: Place Alias And Escape Facts

- **State/role:** complete body-local analysis foundation with alias-aware
  safety-state invalidation as its first client.
- **Implemented:** stable MIR places derive `must-alias`, `no-alias`, and
  `may-alias` for precise roots, fields, and constant projections. Verified
  ordinary-record sibling fields are disjoint; union sibling fields overlap.
  Address taking, returns, retained borrows, non-local stores, raw memory, and
  calls produce explicit escape reasons. Closed exact static summaries project
  receiver/argument escape; native, virtual, recursive, and unresolved calls
  remain conservative. Loan, value-rooted, dynamic, raw, and unclassified
  field relationships do not become `no-alias` without stronger MIR identity.
  Duplicate `must-alias` IDs share one safety fact key; writes preserve safety
  facts only for proved `no-alias` places. The analysis manager binds cached
  facts to one MIR program snapshot and rejects cross-snapshot reuse.
- **Non-goals:** interprocedural pointer identity, optimistic raw-pointer
  provenance, public-library-name recognition, MemorySSA, load commoning, or
  store elimination.
- **Exit gate:** relation symmetry and projection cases, raw/dynamic
  and union-overlap conservatism, exact-call escape, unknown-call escape,
  cache invalidation and snapshot isolation, disjoint-place fact preservation,
  and overlapping-place fact invalidation have focused evidence.

### O-PERF-00: Evidence Freeze And Benchmark Admission

- **State/role:** complete at exact clean commit `a2bd29dc`. This row reconciles the exact-main
  O-MIR-08 audit with the independent PR #164 audit before compiler code
  changes.
- **User outcome:** every following optimizer decision is tied to a durable,
  reproducible workload that distinguishes GTI optimization, native
  optimization, representation overhead, and deliberate safety cost.
- **Independently reproduced evidence:** with compiler `gti 0.312.9`
  (`d578914271ab316007cc48042483ec4893b29004eea4fa858ac0cc7ad4427194`)
  and Apple Clang 21 on arm64 Darwin, fresh emitted vector C++ matched SHA-256
  `932cfc1f2fddb44c0e23644cb703caf2947fcb83da3c0615b096a12a0e682938`.
  At fixed native O3, `vector-checked-loop` measured 117.285 ms median and a
  disposable generated-overflow proxy measured 9.519 ms with the same digest,
  a 12.32x ceiling; a stricter rebuild limited to the three MIR checked-helper
  guards confirmed the same order of magnitude. The fixed-array
  overflow probe measured 73.684 ms versus 15.882 ms, a 4.64x ceiling.
  Mandelbrot emitted at GTI O0/O1/O2/O3 and rebuilt at the same native O3
  measured 169.321/169.383/169.622/169.673 ms, establishing no material
  current GTI-pass runtime effect on that client.
- **Interpretation boundary:** an ablated binary preserves only the selected
  successful benchmark result; it deliberately violates GTI behavior for
  failing inputs. It is an attribution/upper-bound proxy, never proof that a
  proposed transform preserves semantics. PR #164's separate-native result is
  accepted; its claim that ordinary O0-to-O1 timing alone attributes gains to
  Clang is not.
- **Closed evidence:** report schema 4 bound every admitted workload to clean
  compiler/source commit `a2bd29dce07260697de916fd2a8aa54a59bb9ef1`
  and compiler SHA-256
  `5ff0d43de820dbd529525f7daac60fecf522d16134c1cf82afdecf8ff4149b82`.
  Five warmups and twenty seeded interleaved samples were conclusive for every
  variant. The complete-state fixed-array client measured 283.568 ms checked
  versus 60.377 ms for its strict three-helper proxy; the scaled vector client
  measured 1235.735 versus 83.189 ms. Separate schema-3 runs emitted at GTI
  O0/O1/O2/O3 and rebuilt at native O3 measured 152.597/152.008/151.073/150.768
  ms, and all four rebuilt executables had the same SHA-256. Optimization
  reports, MIR dumps, profiles, native disassembly, report hashes, and raw
  samples are retained in
  [`performance-tooling.md`](performance-tooling.md).
- **Scope:** admit long-running checked scalar-chain, fixed-array overflow,
  scalar-FP update, checked-vector, fixed-array/bounds control,
  managed-thread control, and staged game-update workloads into the canonical
  harness. Add a runner mode that holds native optimization fixed while GTI
  optimization changes. Record compiler/runtime/source hashes, exact commands,
  median/MAD and raw samples, optimization reports, profiles, disassembly, and
  normalized correctness/failure records. Scale every timing client above its
  declared process-startup floor; the audit's 3-4 ms FP probe is directional
  evidence only.
- **Exit gate:** clean exact-head runs reproduce the two overflow ceilings and
  the fixed-native optimizer comparison through checked-in workload
  descriptors and threshold-free CI smoke tests. The initial `O-GAME-01`
  scalar/container stage has a correctness-equivalent baseline. No compiler
  transform lands in this row.

### O-PERF-01: Loop-Carried Arithmetic Range Proof

- **State/role:** complete at exact clean commit
  `f1a4075f3848d3f24531f12364f227fde29b887d`; first implementation row after
  completed `O-PERF-00`.
- **Corrected starting point:** the canonical safety analysis already runs a
  deterministic loop fixpoint and widens interval facts after repeated block
  updates. It does not need a second solver. The current loss comes from
  widening expanding to the complete integer domain, missing exact
  remainder/mask transfer, and having no alias-safe fixed-array content
  summary. SCCP is a separate exact-constant client and is not interval
  authority.
- **Bounded scope:** add exact GTI remainder and bit-mask interval transfer for
  the admitted sign/domain cases; add one scalar fixed-array-root content
  interval joined on dynamic writes and invalidated on may-alias escape,
  calls, raw memory, lifetime change, or unknown mutation; then add only the
  loop-header threshold widening and bounded narrowing required by that
  client. Reuse `O-MIR-06` loop identity, `O-MIR-07` alias facts, the shared
  solver, the existing arithmetic discharge records, `MirEditor`, and
  independent replay. Source-MIR cyclic scalar promotion is not an entry
  prerequisite unless a focused probe proves the place-based analysis cannot
  carry the required fact.
- **Semantics gates:** each intermediate checked operation remains
  left-to-right. Remainder ranges follow GTI's signed and zero-divisor rules;
  a later modulo cannot excuse an earlier overflowing multiply/add. Array
  summaries never become per-element truth and never survive an unproved
  alias, call, escape, union overlap, or mutation.
- **Exit gate:** the admitted fixed-array overflow workload discharges more
  than 90% of its hot arithmetic outcomes and reaches at most 1.2x idiomatic
  C++ at O2/O3. Near misses for signed ranges, zero divisors, type extrema,
  alias/escape, unknown calls, irreducible loops, widening, and failure paths
  retain checks. O0 identity, exact failure origin/order, cleanup, raw reports,
  and all campaign controls pass with no conclusive regression above 5%. If a
  source-MIR proxy proves this bounded analysis cannot discharge the admitted
  client or cannot approach the runtime gate, close the row as measured-defer
  with that retained evidence rather than adding a broader solver by default.
- **Closed evidence:** the implementation added exact remainder and bit-mask
  interval transfer, a scalar fixed-array-root envelope with alias/effect
  invalidation, and a finite reducible-loop accumulator threshold on the
  existing dataflow solver. The admitted body discharged five of five hot
  arithmetic outcomes and all three hot fixed-array bounds outcomes. Five
  warmups and twenty seeded interleaved runs produced 61.807 ms GTI versus
  61.111 ms idiomatic C++ at O2, and 61.055 versus 61.113 ms at O3. Exact
  signed-minimum remainder, zero-divisor, dynamic-bound, repeated-write,
  alias-write, call-barrier, lifecycle, and runtime-failure tests retain the
  conservative boundary. No exact-head control regressed by more than 0.4%
  relative to its semantic-C++ twin.

### O-PERF-02: Closed-Call And Container Fact Transport

- **State/role:** complete on exact clean implementation commit `d6374eb9`;
  this row owns the first interprocedural performance client and subsumed the
  bounded campaign client of `O-MIR-10`.
- **Corrected client:** `vector-checked-loop` is deterministic and its hot
  values are bounded: initialization yields 1 through 17, the multiplier is
  selected as -1 or 1, and the complete sum remains in range. It is not
  evidence that static analysis is impossible. The missing fact crosses
  closed `scale`, `sum`, vector methods, and the prefix-storage boundary.
- **Bounded scope:** after profiling, select exactly one concrete,
  non-recursive static-call family: budgeted MIR inlining, fact-specialized
  body cloning, or contextual proof summaries. The selected form must carry
  exact argument, return, and admitted mutable-container content facts needed
  by this workload and no broader abstract interpretation. A return interval
  is a distinct revision-keyed interprocedural analysis result, not an effect
  flag; effect summaries continue to describe behavior rather than values.
- **Architecture gates:** reject native, virtual, recursive, open, owning-
  temporary, incomplete-cleanup, or unsupported failure shapes. Remap or key
  every body/value/place/failure identity within one optimized MIR snapshot;
  preserve Invoke edges, source provenance, left-to-right parameter/result
  materialization, loans, partial effects, and cleanup. Do not recognize
  `std::vector`, `scale`, `sum`, or another public spelling.
- **Exit gate:** the selected closed-call family moves the admitted vector
  arithmetic discharge counters and improves median runtime by at least 30%
  beyond the `O-PERF-01` exact-head baseline. Static/recursive/native/virtual,
  cleanup-owning, aliasing, and conflicting-call-context near misses remain
  conservative. Exact source-MIR replay, all backends, failure injection,
  deterministic budgets, and campaign controls pass. If no candidate proxy
  moves runtime, close this row with the retained evidence and proceed to the
  independently attributable prefix-storage row rather than landing
  interprocedural infrastructure.

The selected implementation is a bounded contextual proof-summary analysis,
not inlining or cloning. It transports exact scalar arguments, returns,
mutable receiver/argument objects, and structurally admitted indexed-container
element envelopes through concrete source-defined static calls and
constructors. A whole-program cache invalidates on any semantic body edit;
metadata-only proof attachment explicitly preserves it. Native, virtual,
recursive, owning-transfer, synchronization, unsupported cleanup, conflicting
context, and non-convergent families fail closed. Proof replay batches one
immutable source and candidate snapshot, so the new whole-program authority is
not recomputed per discharge.

At O2, five warmups and twenty seeded interleaved samples produced 558.336 ms
GTI, 715.229 ms semantic C++, and 87.947 ms idiomatic C++. The GTI median is
54.0% below the `O-PERF-01` exact-head vector baseline; normalizing each GTI
run to its semantic twin gives a 62.6% reduction. O3 produced 570.654 ms GTI,
571.371 ms semantic C++, and 57.471 ms idiomatic C++. Both exact-head reports
are conclusive and retain the same complete-state digest. Detailed report
identities, counters, and raw samples live in
[`performance-tooling.md`](performance-tooling.md).

### O-PERF-03: Single-Bound Prefix Storage Access

- **State/role:** complete on exact clean implementation commit `eaaea34d`;
  second-order container closure.
- **Corrected starting point:** current `std::vector` stores `count` while its
  private `prefix_storage` independently stores `length_`, and every public
  subscript performs a vector-detail bounds check followed by the prefix read's
  private bound check. Contrary to PR #164, unchecked read helpers currently
  exist only for sparse `storage<T>`; prefix storage has no unchecked-read
  vocabulary, and the safety analysis does not prove prefix reads.
- **Bounded scope:** establish one logical live-length authority, preferably by
  deriving vector size from prefix storage and removing duplicated `count`
  state if the source-defined API and mutation invariants permit it. Add a
  prefix-read discharge only when MIR proves the exact same index/length fact
  and no intervening length mutation or alias. A backend-only unchecked prefix
  representation helper may be added only as the consumer of that verified
  discharge; it is not a source intrinsic or trusted-library escape hatch.
- **Exit gate:** native evidence shows one logical check per non-discharged
  public access and no private recheck after a verified discharge. Vector and
  prefix lengths cannot drift by construction; append/insert/erase/pop/clear,
  relocation, failure rollback, move, iteration, empty/boundary access, and
  exact vector-versus-private-storage failure detail pass. The admitted vector
  workload improves by at least 5% and controls regress by no more than 5%.
  Test the duplicate-check proxy first; if it cannot reach that threshold and
  no correctness invariant requires the representation change, close the row
  as measured-defer without adding the new discharge vocabulary.

The implementation removes vector's mirrored `count`; private prefix storage
is now the sole live-length authority across construction, mutation,
relocation, iteration, and access. Safety analysis records an exact relation
between one canonical prefix-storage origin, the scalar origin of an index,
and a successful logical-size check whose length came from that same storage.
Prefix mutation/relocation, index writes, may-alias writes, opaque calls, and
joins without the relation invalidate it. The backend-only unchecked prefix
helper has no GTI source declaration and is selected only after independent
verified-MIR replay accepts the bounds discharge.

The exact vector surface records seven bounds discharges covering read-only and
mutable `front`, `back`, `at`, and `operator[]`; each retains its public vector
check and omits only the private prefix recheck. Native O0/O2/O3 evidence
preserves empty `front`, `back`, and `at` as `index_out_of_bounds in vector`,
while prefix capability evidence retains private guard, move, relocation,
reverse-destruction, mutation, and sanitizer coverage. At O2 the admitted
vector median fell from 558.336 ms to 433.264 ms; at O3 it fell from 570.654 ms
to 405.981 ms. Exact report identities, counters, controls, and raw samples live
in [`performance-tooling.md`](performance-tooling.md).

### O-PERF-04: Exact Fast Path For Undischargeable Checked Loops

- **State/role:** historically measured-defer on exact clean commit `4dbb50bd`;
  the then-required source-proxy gate rejected a transform. ADR 012 now permits
  renewed architectural work without a proxy or 30% entry threshold. Preserve
  this row's opaque-input and exact-failure correctness requirements.
- **Rejected audit shape:** PR #164's wrapping block plus sticky predicate and
  replay is not accepted as semantics-preserving as written. Its own
  side-effect-free legality rule excludes the flagship `scale` loop because
  that loop mutates the caller's vector. A sticky flag also does not by itself
  preserve strict left-to-right integer recurrence, reordered intermediate
  overflow, first failing operation, or already-committed earlier effects.
  LLVM intrinsics cannot be GTI proof or a C++-backend prerequisite.
- **Admissible investigation:** compare a side-effect-free buffered block with
  exact replay against a preflight/versioned fast path that performs no
  observable mutation, then executes an unchecked block only after proving all
  of its checked operations safe; retain the original scalar checked block as
  fallback. Admit no call/user code, allocation, synchronization, raw memory,
  FP reassociation, loan/drop/lifecycle event, unstable read, escaping write,
  or may-alias mutation unless a complete snapshot/commit proof exists.
  Reordered integer evaluation must prove all introduced intermediates and the
  exact result, not just the original sequential prefixes.
- **Entry/exit gate:** identify the legality and cost model for the opaque-input
  fast path; a disposable proxy is optional investigation, not an admission
  gate. The implementation preserves exact successful output, first-failure site
  and record, partial mutation trace, cleanup, O0 identity, fallback behavior,
  and every rejected near miss. It must bring the checked-vector/core-loop gate
  to at most 1.5x idiomatic C++ or close as measured-defer with the next cause
  identified.

The admitted scalar recurrence performs 200 million checked additions whose
loop-invariant `argc` operand is runtime opaque. Its source preflight reads no
mutable state, admits only an exact range that proves the first and every
subsequent addition, uses public wrapping arithmetic in the proved lane, and
retains the original checked loop unchanged for every other input. Five
warmups and twenty seeded interleaved O3/native-O3 samples measured 540.055 ms
for checked GTI, 538.225 ms for the proxy, 539.767 ms for semantic C++, and
538.246 ms for idiomatic C++. The 0.34% proxy improvement is far below the 30%
entry threshold, while checked GTI is already 1.003x idiomatic.

An earlier record/vector experiment was rejected as the row proxy: its 29.28%
ceiling removed operations whose safety follows from modulo-bounded record
fields but whose facts are outside the current scalar element envelope, and it
retained public vector call/check overhead. That result identifies aggregate
element facts and container call boundaries as separate future profiling
targets; it does not justify checked-loop versioning. No CFG cloning, sticky
wrapping, replay machinery, public-name recognition, or backend-specific proof
was added.

### O-PERF-05: Residual Backend Representation Hygiene

- **State/role:** complete as a measured no-op on exact clean commit
  `9da44477`; the admitted clients meet the gate without a backend change.
- **Scope:** profile a scalar-FP/update and one closed-call client whose timed
  region exceeds 50 ms. Compare MIR-equivalent emitted C++ and semantic C++ at
  the same native settings. Test only surviving artifacts such as unnecessary
  out-of-line failure-ABI boundaries, repeated representation loads, missed
  LICM, or missed target policy. `noexcept`, attributes, or C++ spelling are
  backend policy only after exact MIR effects prove them; strict GTI FP order
  and `-ffp-contract=off` remain unchanged.
- **Exit gate:** the admitted residual client reaches at most 1.15x semantic
  C++, or profiler/disassembly and an upper-bound proxy show less than 5%
  available value and close the row as a measured no-op. Every supported
  native compiler, MIR/backend differential, failure ABI, and campaign control
  remains correct.

The exact schema-3 campaign used native O3, strict floating-point flags, five
warmups, and twenty interleaved samples. `scalar-fp-update` measured 80.297 ms
in GTI versus 79.885 ms in semantic C++ and 79.680 ms in idiomatic C++.
`vector-checked-loop` measured 53.650 ms in GTI versus 491.655 ms in semantic
C++ and 51.400 ms in idiomatic C++. Native inspection found a nine-instruction
call-free hot loop in both scalar variants and fully inlined/vectorized vector
loops. The 0.5% scalar semantic gap and 4.4% vector idiomatic gap leave less
than 5% value; no backend annotation, ABI, or representation change was
admitted. Exact report identity and raw samples live in
[`performance-tooling.md`](performance-tooling.md).

### O-GAME-01: Game-Style Runtime Acceptance Workload

- **State/role:** complete; this row released the exclusive compiler gate on
  2026-08-27. Its first scalar/container baseline was admitted in `O-PERF-00`;
  unavailable public APIs remain explicit staged constituents rather than
  compiler-private simulations.
- **Bounded scope:** compare GTI, semantic-equivalent C++, and explicitly
  idiomatic C++ for the same fixed update stream and complete-state digest.
  Cover array-of-structures and structure-of-arrays math, container
  traversal/mutation, static/callable dispatch, bounded allocation/pool reuse,
  joined tasks, and a separately attributable C-FFI call client as public APIs
  become available. Keep rendering, file I/O, input generation, and reporting
  outside the timed region.
- **Evidence policy:** record median and tail update latency, work units, raw
  samples, exact compiler/toolchain/source identity, profiles, emitted-code
  hashes, optimization reports, and correctness/failure records. Shared CI
  remains threshold-free; gates run on the published stable machine.
- **Exit gate:** core checked kernels reach at most 1.5x idiomatic C++. The
  combined update reaches at most 1.5x semantic-equivalent C++ and 2x
  idiomatic C++, with no constituent above either applicable gate without a
  completed fix or measured-defer record. Fixed-array, managed-thread,
  compile-time, code-size, and memory controls have no unexplained regression;
  full compiler/frontend/backend/runtime and installed-toolchain gates pass.
  Closing this row releases the exclusive compiler gate.
- **Closure evidence:** clean implementation commit `c5bbfaa8` produced the
  exact `GTI-BENCH-1 digest:0000000012e84ad4 work-units:61440000` record for
  every variant. GTI measured 58.503 ms median (0.424 ms MAD), semantic C++
  208.126 ms, and idiomatic C++ 58.454 ms. The optimizer discharged 53 of 85
  source safety operations, including both bounds outcomes and all 36 hot
  overflow outcomes; emitted C++ fell to 320,030 bytes and the executable
  remained 115,200 bytes. Exact fixed-array, vector, scalar, and joined-thread
  controls were conclusive. O0 coherence now skips inapplicable whole-program
  safety replay: the same source used 56.26 MB maximum RSS versus 62.59 MB on
  pre-game commit `9da44477`. O3's 87.56 MB peak is the bounded closed-call
  aggregate analysis that supplies the new proofs, limited to 32 global rounds
  and eight updates per body rather than an unexplained all-level regression.
  Full raw samples, hashes, and control reports are recorded in
  [`performance-tooling.md`](performance-tooling.md).

### O-MIR-08: Loop-Derived Safety Proof Client

- **State/role:** measured defer; not implemented and not the active optimizer
  client.
- **Bounded scope:** consume only `O-MIR-06` single-latch affine facts to prove
  one existing fixed-array bounds family over a reducible loop. Reuse the
  existing safety-discharge transform and replay path; add no loop rewrite.
- **Entry/exit gate:** establish replayable source-MIR loop-carried affine facts;
  no upper-bound proxy is required to begin architecture work. A representative
  checked loop must show removed operations and an end-to-end improvement; every induction,
  bound, overflow, early-exit, mutation, and irreducible near-miss retains
  checks; O0/O2/O3 runtime and failure-origin parity passes.

### O-MIR-09: Alias-Safe Local Memory Client

- **State/role:** measured defer; not implemented.
- **Bounded scope:** begin with a redundant-load or store-to-load family based
  on its semantic and execution-cost model. Add the smallest explicit local
  memory-version fact needed to prove no intervening write using `O-MIR-07`
  and exact call effects.
- **Entry/exit gate:** projected aliases, calls, raw memory, borrows, moves,
  drops, synchronization, and lifetime boundaries retain the operation unless
  proved safe. Introduce MemorySSA when general memory reasoning cannot remain
  sound and maintainable with bounded local versions; a measured client is not
  a prerequisite. Validate the production transforms and their runtime costs.

### O-MIR-10: First Interprocedural Transform

- **State/role:** parked as a standalone row; its bounded campaign client was
  subsumed and completed by `O-PERF-02`.
- **Bounded scope:** if `O-PERF-02` selects inlining or body specialization,
  apply this row's closed concrete, non-recursive, non-native, non-virtual
  identity, cleanup, provenance, failure, and deterministic cost gates there.
  A distinct return-range summary remains value analysis rather than an effect
  flag. Devirtualization remains outside the campaign.
- **Entry/exit gate:** the row may reopen for general interprocedural
  optimization needs not already served by the completed `O-PERF-02` summary
  mechanism, without a benchmark-first gate. Verify and measure the resulting
  production implementation.

Incremental dominance and LLVM bit-vector adoption remain measured/client-
gated. A future LLVM backend requires complete MIR layout, ABI,
temporary, drop, and executable authority; support-library adoption does not
imply it.

## Parallel Engineering Tracks

These rows improve maintainability, projects, and tooling without changing the
language critical path. They were parked during the completed runtime-
performance and `R-STRUCT` exclusive gates. Their table states and the active
ready queue now control scheduling by logical prerequisite.

### C-MIG: Compiled Compiler Library

The accepted detailed plan remains
[`compiler-library-migration.md`](archive/compiler-library-migration.md).
That migration is complete and supplies the compiled target foundation for the
completed internal decomposition in the
[archived structural-refactor plan](archive/compiler-structural-refactor.md);
it is not a competing active queue.

| ID | State | Next bounded result | Serialization rule |
| --- | --- | --- | --- |
| `C-MIG-02` | done | SourceLoader and parser algorithms compile behind their existing public contracts with recovery and diagnostics preserved. | Completed without a language behavior change. |
| `C-MIG-03` | done | Semantic records remain public while semantic analysis and concrete-instance reanalysis compile behind `SemanticVisitor`. | Completed without changing semantic authority. |
| `C-MIG-04` | done | HIR instance discovery/lowering and MIR body/CFG lowering compile behind public IR contracts. | Completed without changing lowering behavior. |
| `C-MIG-05` | done | MIR editing, analyses, effects, verification, printing, and current pass implementations compile behind public contracts. | Completed without changing O0/O1 behavior or pass verification. |
| `C-MIG-06` | done | C++ emission compiles behind `BackendInput` in an independently linked and installed `gti_cpp_backend`. | Completed without changing generated C++ or migrating AST emission to MIR. |
| `C-MIG-07` | done | Remaining frontend orchestration, semantic snapshot/query, constant-evaluation, formatting, language-query, and support algorithms compile behind existing contracts. | Completed with public records, templates, `constexpr` operations, trivial accessors, and exact-version facades intentionally retained in headers. |

### A-ID: Frontend Identity And Recovery

| ID | State | Scope and gate |
| --- | --- | --- |
| `A-ID-01` | ready | Add GTI-owned AST kind tags and `gti::isa/cast/dyn_cast`; migrate one heavy cluster at a time, serialized with any active `C-MIG-02/03` slice. Do not use LLVM casting. |
| `A-ID-02` | planned | After `C-MIG-02`, first define source-unit and snapshot lifetime/serialization, then replace path strings in token/span identity with `SourceUnitId`, preserving deterministic diagnostics. |
| `A-ID-03` | planned | After `A-ID-02`, add snapshot-local `NodeId` and migrate side tables incrementally to dense vectors. Raw AST pointers may remain local implementation details during transition, not cross-snapshot identity. |
| `A-FLOW-01` | measured defer | After `A-ID-03`, adopt dense binding state only if profiles still justify it; do not couple it to TypeContext. |
| `A-REC-01` | planned | After `C-MIG-02` establishes the parser migration seam, add honest recovery/error expression nodes with formatter/LSP tests; no second declaration parser. |
| `A-CACHE-01` | planned | After `C-MIG-02`, cache parsed immutable units by path/content identity so standard-library units are not re-lexed/reparsed per edit; never retain snapshot-local AST pointers across incompatible snapshots. |

### E-BACK: Backend Determinism And Concrete Instances

| ID | State | Scope and dependency |
| --- | --- | --- |
| `E-SYM-01` | done under `L-GEN-01` | Nameable source generic free functions, eligible top-level class applications, and ordinary member instances use SHA-256 symbols over versioned lowered nominal plus concrete type/value/signature identity, never counter-shaped IDs or TypeContext pointers. Constructors use already-selected concrete signatures. Discovery-order perturbation tests cover function, class, and member families. |
| `E-INST-01` | done | Nameable generic functions, eligible classes, members, constructors, and exact class specializations emit as ordinary concrete C++ declarations, and every verified-MIR call names the GTI-selected route directly. Native templates remain only as audited physical carriers for unnameable lexical closures, incomplete-pointee ownership wrappers, unused families, nested types, compiler/C ABI capabilities, and their transitive dependent generic families; they have no semantic authority. |
| `E-EMBED-01` | active | `E-EMBED-00`, `B-LIB-01`, and `E-EMBED-01A` are complete; `E-EMBED-01B` is the next ready row. `E-EMBED-01B/C`, `B-LIB-02`, `T-EMBED-01`, and `Q-EMBED-01` remain the sole route to a host-callable artifact; this remains a generated C wrapper rather than a stable GTI ABI. |
| `E-ABI-01` | later breadth | Stable GTI ABI/separate compilation only after layout, names, concrete emission, package identity, and compatibility policy are complete. |

### P-MEASURE: Performance And Representation

Prefix-storage historical tracking ID: issue #37.

| ID | State | Scope and gate |
| --- | --- | --- |
| `P-MEASURE-01` | complete benchmark substrate | The standard-library-only runner, strict descriptors, correctness digests, controlled output paths, raw samples, compiler/build identity, and GTI/semantic-C++/idiomatic-C++ workloads are implemented with threshold-free smoke coverage. `O-PERF-00` owns the fixed-native attribution mode and runtime workload breadth; `O-GAME-01` closed the exclusive campaign. |
| `P-STORAGE-01` | complete | Prerequisites delivered: the checked-vector fixture shipped through `P-MEASURE-01`, `M-FAIL-01`'s defined-failure contract is live, and the failure-capable `M-BACK-02` slices landed (0.176.0-0.189.0). Slice 1 shipped in 0.190.0: the identity-bound public logical-size check (`gti_internal::index_bounds_check`, `IntrinsicKind::StorageBoundsCheck`) guards every public vector/string accessor while sparse storage remains, reporting `GTI-R0007` with the enclosing trusted container's detail (`index_out_of_bounds` in `vector`/`string`) rather than leaking `GTI-R0010`. Slice 2 shipped in 0.191.0: the distinct compiler-private prefix-initialized storage capability (`gti_internal::prefix_storage<T>`, `CompilerCapabilityTypeKind::PrefixStorage`, `SemanticType::PrefixStorage`, the seven-intrinsic family, and the length-tracking backend representation with no per-slot bitmap) exists end to end through semantics, HIR, MIR (`mir-v31`), the effects tables, and the compatibility backend, proven by trusted-prelude fixtures; sparse `storage<T>` remains for arbitrary partial slots and the two capabilities reject each other's operations. Slice 3's capability-level evidence shipped in 0.192.0: the dedicated native evidence battery (`prefix_storage_native_evidence`) compiles trusted generated programs across C++20/C++23 and native -O0/-O2/-O3, runs the full behavioral contract (append order, logical-prefix reads and mutation, pop-last destruction, complete-prefix relocation with moved-from shells correctly skipping user destructor bodies, move transfer, reverse destruction at scope exit), trips all five runtime guards on their exact diagnostics, and runs the success path clean under dedicated generated-program AddressSanitizer/UndefinedBehaviorSanitizer builds. The wrapper migration shipped in 0.193.0: `vector` and `string` internals moved wholly onto the prefix capability (fields, iterators' storage borrows, and every operation), with the shift gap filled by two sealed invariant-preserving primitives — `prefix_storage_insert` and `prefix_storage_erase` — whose element shift and construction or destruction happen inside one runtime operation, so no partially initialized slot is ever observable at the language surface; the example corpus stayed byte-identical in stdout and exit codes and the checked-vector benchmark workload passes on the migrated containers. Sparse `storage<T>` remains the arbitrary-slot capability. The later backend slice is complete: the storage capability rows and general-emitter vocabulary admit these storage-gated bodies to verified-MIR emission. Historical: add the identity-bound public logical-size check while sparse storage remains so vector/string indexing reports `GTI-R0007` rather than leaking `GTI-R0010`. Then add a distinct compiler-private prefix-initialized storage capability for vector/string-shaped owners while preserving sparse `storage<T>` for arbitrary partial slots. Prefix construction appends exactly at the live length, destruction removes exactly the last live element, relocation transfers the complete prefix, and reads check the logical prefix. Semantics, HIR, MIR, effects, and every backend bind the capability by private declaration identity. Construction-failure rollback, relocation, move, clear/pop, reverse destruction, C++20/23, fixed-native-optimization O0/O2/O3 differential, dedicated generated-program ASan/UBSan, and benchmark evidence must pass before migrating the public wrappers. No trusted-source unchecked accessor or public-name special case is permitted. |

- Existing `--time-trace` and deterministic MIR printing count as completed
  foundations; do not reimplement them under the older performance plan.
- Add structured optimization/safety reports only when the first real MIR pass
  produces decisions worth reporting.
- `TypeContext` becomes ready only after a written snapshot/context lifetime,
  a type-allocation/churn benchmark, and evidence that identity migration pays
  for its cross-phase cost. Start with a GTI-owned interface and ordinary
  storage; compare private LLVM allocation/indexing only afterward.
- Shrinking `MirInstruction`, LLVM VFS, `lit`/`FileCheck`, private allocators,
  and sparse bitsets remain measured options rather than queued work.

### B-PROJECT: Builds, Packages, And Dependencies

The next order resolves drift between the build proposal and checkpoint:

| ID | State | Scope and gate |
| --- | --- | --- |
| `B-PROJECT-01` | complete | Project test targets and `gti test` use driver-owned immutable build plans. Deterministic all/named selection, independent outputs, runtime-failure continuation/propagation, metadata kind publication, direct-mode preservation, and installed CLI/library coverage pass. |
| `B-PROJECT-02` | complete | Project build/run/test requests use a deterministic SHA-256 whole-program cache over the compiler-owned loaded source graph, including ordered configured prelude-root provenance, effective target/profile/backend policy, runtime/toolchain identity, and admitted native environment. Verified generated-C++/executable payloads publish atomically under `build/gti/cache/v2`; hits skip frontend/backend/native compilation, corruption rebuilds before replacement, legacy schema entries are ignored, and `--no-cache` bypasses read/write. Requests with declared native sources, native search directories (including dependency-injecting environment paths), opaque native argument vectors, native link operands, or unresolved libraries/frameworks conservatively bypass the cache until their transitive inputs are modeled; pure-GTI checkout moves retain content identity, and direct mode remains uncached. |
| `B-PROJECT-03` | complete | Manifest schema 1 resolves canonical workspace members and recursive source-only path dependencies without network access. Direct aliases feed the compiler-owned source graph, transitive/quoted cross-package access is rejected, cycles/duplicate names/roots/nested workspaces fail before compilation, `--package` selection and shared collision-free outputs are deterministic, schema-9 metadata publishes the graph, and cache identity includes package provenance. Direct mode remains manifest-independent. |
| `B-PROJECT-04` | complete | Exact pinned-revision Git resolution, `gti.lock`, explicit `gti fetch`, `--locked`, and `--offline` use immutable content-verified checkouts and have reproducible CLI and installed-toolchain coverage. Registry ranges, mutable branch/tag selectors, and dependency code execution remain out of scope. |
| `B-PROJECT-05` | complete | `gti/driver/project_facts.h` publishes the resolved workspace as read-only values: package closure with membership/roots/selectability/git provenance/alias edges, the package source-root graph, the selected package's profiles and targets, package-level configuration flags, the default profile name, per-profile analysis configuration, and the workspace-graph model identity. Resolution runs no native tool, git process, or build and creates no directories, using `gti metadata`'s dependency policy. Library tests cover workspace members, path dependencies, standalone packages, discovery/manifest/selection failures, build-plan agreement, and installed-package consumption; `T-LSP-04` now consumes the same facts. |
| `B-LIB-01` | complete | Dormant shared-library targets and exact exports cross immutable project/compiler facts through LoweredProgram. Public manifests remain gated; embedding_foundation owns the structural gate. |
| `B-LIB-02` | blocked | After E-EMBED-01C, publish, cache, report and clean the shared library/header, then activate library targets. |
| `B-PROJECT-06` | complete | Source registry bootstrap: `gti add`, immutable completeness-last `gti publish`, metadata-only shared SemVer resolution, exact logical v2 lock closures, verified user caching and locked/offline fetch integrate with the normal source/native graph. Gitea Generic Packages is the configurable official backend, not package-semantic authority. Normal registry builds and LSP facts acquire nothing. Fake/in-memory registry tests cover conflicts, partial publication, authentication, integrity, concurrent acquisition and offline native consumers. Signing, yanking, search, multi-version canonical identities and feature/target solving remain deferred; ADR 025 and the current build/driver document own the exact limits. |

Each row must preserve direct mode. Dependency fetching, manifests, cache, and
artifact policy stay in `gti_driver`, never `gti_compiler` or the LSP.

### T-LSP: Language Tooling

| ID | State | Scope and dependency |
| --- | --- | --- |
| `T-LSP-01` | hover/completion/signature help complete; API docs ready | `///` Markdown is retained once on declarations and exact compiler symbols; hover publishes it at declarations and resolved uses across dirty dependency overlays, and completion items and signature help publish the same symbol-attached value under the client's declared documentation format. Generated API docs remain a consumer of the same value. |
| `T-LSP-02` | complete for current snapshots | Current-snapshot references, document highlights, hierarchical document symbols over parser-recorded extents, fail-closed function-local rename, and signature help from the selected overload and argument position are implemented through `LanguageQueries` with protocol tests. Project-wide coverage remains gated on `T-LSP-05`. |
| `T-LSP-03` | measured defer | Extract document/snapshot service boundaries when a concrete feature makes the current server class costly to evolve. |
| `T-LSP-04` | complete | Analysis and completion consume driver-owned `ProjectFacts` without fetching/building, publish project-resolution failures, preserve standalone mode, watch manifest/lock changes, and keep dirty dependency buffers authoritative. |
| `T-EMBED-01` | blocked | Extend the completed T-LSP-04 ProjectFacts integration to library/export facts after B-LIB-02; dirty roots must use the exact no-main and export contract. |
| `T-LSP-05` | measured defer | A durable project index only after unopened-file references/rename proves a client. |
| `T-LSP-06` | measured defer | Process-isolated analysis only if in-process crash recovery must guarantee containment of heap corruption. The current guard is state-safe, not process isolation. |
| `T-LSP-07` | complete | Missing-include actions discover actual declarations across the selected project, direct package aliases and public standard-library units. Structured semantic lookup requests, canonical parsing, exact definition identity and speculative frontend reanalysis replace the hardcoded standard-type catalog. Dirty/new/deleted files invalidate discovery results; module/private/incompatible candidates fail closed. No durable project index is introduced. |

Every new syntax/semantic feature still updates formatter, Tree-sitter, LSP
tokens/queries, shipped-source parsing, and documentation in its own row.

### Q-QUALITY: Diagnostics, Conformance, And Release

| ID | State | Scope and gate |
| --- | --- | --- |
| `Q-DIAG-01` | foundation complete; migration active | One checked JSON source now generates the private typed diagnostic identity/default-severity/group/format table and rejects duplicate identities or invalid placeholder contracts. The first semantic/source producers (`GTI-S2009`, `GTI-S2075`, `GTI-S2095`, and `GTI-S2096`) use it; remaining subsystems migrate in bounded slices while preserving current spans/messages unless intentionally changed. |
| `Q-FAIL-01` | hosted consumer, same-thread native-callback firewall, and managed-task handoff/re-raise complete; broader M-FAIL integration active | The versioned C ABI, exact escaped ordinary/emergency reports, artifact/site/outcome validation, status 70, observer firewall, report-I/O fallbacks, terminal arbitration, callback termination adapter, and worker ordinary/`GTI-R0014` containment are implemented and focused tests pass. Embedding, foreign-thread callback containment, and broader checked families remain co-delivered with their M-FAIL/M-BACK slices. |
| `Q-EMBED-01` | blocked | After T-EMBED-01, run the dynamically loaded engine-host acceptance matrix before activating native libraries and generated embedding. |
| `Q-DEPRECATION-01` | planned | `D-COMPAT-01` is done. After `T-LSP-01`, add one bounded `[[deprecated("message")]]` declaration contract with use-site diagnostics, semantic metadata, formatter, Tree-sitter, hover, completion, and deterministic tests. |
| `Q-FUZZ-01` | bounded compiler campaign complete — `Q-FUZZ-01A` capture/replay, `Q-FUZZ-01B` parser/formatter/pipeline/source-loader libFuzzer harnesses, `Q-FUZZ-01C` resumable bundle reduction, and `Q-FUZZ-01D` real-compiler typed differential/composition validation are implemented; constrained eleven-factor interaction generation, pairwise/selected-triple coverage, native/metamorphic oracles, measured-boundary triage, and bounded typed reduction extend that foundation; prerequisite `C-MIG-02` is done | Wider seeded exploration remains opt-in; the seed 20261003 campaign passes after the checked passive Expected argument fix recorded in verification; the fixed interaction smoke is a CTest, not a complete coverage claim. Arbitrary JSON-RPC/LSP fuzzing, a complete generator, and arbitrary semantics-aware source reduction remain deferred. Current commands and boundaries live in [verification](../architecture/verification.md#failure-capture-and-opt-in-fuzzing); the original campaign rationale remains in [the archived tooling record](archive/compiler-bug-finding-and-triage.md). |

Examples, sanitizers, installed-library consumers, release packaging,
deterministic output, and the local language audit are standing release
invariants rather than a finishable task row. Each new feature adds its
conformance case in its owning row.

## Outstanding Work Classification

This table is the top-level disposition for major open work. Detailed scope is
owned by the rows and domain plans above.

| Area | Disposition | Principal prerequisites/owner |
| --- | --- | --- |
| Language restriction ledger | **complete and maintained** | `D-LANG-01`; [`language-alignment.md`](language-alignment.md) |
| Concurrency memory model | **adopted durable decision** | `D-MEM-01` and `D-MEM-02` done; ADR 008 |
| Transfer/share facts and concurrent globals | **complete policy substrate** | `I-CAP-01`, `D-MEM-02`, `C-TYPE-01`, and `C-GLOBAL-01` done; bounded public concurrency is systems-ready work |
| Public threads/atomics/mutex | source-defined owned `std::thread`, `std::thread_result<Result>`, ordered scalar `std::atomic<T>`, and scoped `std::mutex<T>`/`std::mutex_guard<T>` **complete** | `C-CALL-01`, `C-THREAD-01`, `C-RUNTIME-01`, `C-ATOM-01`, `C-ORDER-01`, `C-SYNC-01`, and their `C-CONFORM-01` subsets done |
| Evaluation order | **contract adopted; bounded ordinary call/constructor schedule implemented; systems-readiness implementation required** | `D-EXEC-01` and `M-LIFE-01` done; scalar/reference plus eligible class-copy/class-move ordinary-call and concrete ordinary-constructor HIR/MIR order landed; remaining `M-EXEC-01` and matching `M-BACK-01/02` slices remain |
| Runtime failure contract | contract complete; **systems-readiness implementation required** | `D-FAIL-01`, `I-CAP-01`, and `M-LIFE-01` done -> `M-EXEC-01` -> co-delivered `M-FAIL-01`/`Q-FAIL-01` -> complete `M-BACK-02` migration |
| Source text and documentation comments | **hover/completion/signature-help tooling complete; broader consumers remain** | `///` retention, hover, completion, and signature help are complete in `T-LSP-01`; broader library coverage and generated API documentation remain |
| Target/data-layout facts and `sizeof`/`alignof` | **complete bounded systems-readiness substrate** | `S-LAYOUT-01` and `S-LAYOUT-02` done; aggregate/native layout remains client-driven |
| Compiler-private capability visibility | **complete** | `I-CAP-01` done; trusted source roles, exact private type identity, `GTI-S2058`, and compiler-owned LSP filtering |
| Indexed partial moves | **complete bounded-first slice** | `M-OWN-01` and `M-OWN-02` done; dynamic indices remain conservative |
| Temporary/active-drop authority | **complete normal-exit lifecycle substrate** | `M-LIFE-01` done; failure rollback and full ordered materialization remain downstream |
| Stored/escaping mutable dependencies | **complete bounded-first systems-readiness proof** | `M-OWN-03`; one direct stable-origin mutable carrier, with broader graphs client-gated |
| Mutable iteration/views | L-RANGE-01/02 and the bounded read-only single-owner L-RANGE-03 release are complete; mutable, nested, and multi-owner views remain | Follow-up must first extend the ownership representation explicitly |
| Named expected-payload range loans | **complete bounded systems-readiness integration** for fallible byte-input parsers and serializers | `M-OWN-04`; exact named active-value projection feeding the existing `L-RANGE-01` read-only plan |
| Native C records/handles/callbacks | bounded records, generated adapter header, pointer-only opaque identity, strings, returned arrays, and exact same-thread callback adapters complete; captures, userdata ownership, foreign-thread entry, and ownership transfer remain systems-readiness breadth | `S-ABI-01/02/03`, `S-FFI-02` F1/F2/F3, and `S-CALL-01` done; complete GLFW 3.4 oracle is the acceptance evidence |
| Owned callables and capture | contract complete; confined boundary, exact value-result, repeatable read/mut invocation, consuming once-callable cardinality, MIR-v10 environments/transport, explicit owned move capture, exact same-type generic return including the bounded repeatable-invoke/final-return combination used by `std::for_each`, one-field generic owner, exact namespace function items for confined generic and managed-thread clients, exact consumed managed-thread `void()` and typed-result tasks, and non-capturing same-thread native adapters implemented; broader owner/extraction/concept work remains systems-readiness work | `D-CALL-01`, `C-CALL-01`, and `S-CALL-01` done -> remaining `L-CALL-01` |
| Allocator/provenance model | **typed public allocation/provenance slice complete; allocator-aware client ready** | `S-ALLOC-01/02` complete through ADR 022 and `<std/memory>`; `S-ALLOC-03` remains |
| Freestanding profile | **later breadth until a target workload requires it** | `S-FREE-01` |
| Payload enums/matching | **in progress:** passive payloads plus concrete owning-class construction, variant-aware exactly-once drop, whole-enum move construction/assignment, checked-reference observation, direct by-value ordinary free-function/non-polymorphic method/constructor/exact-lambda transport, ordinary direct-field containment with construction rollback and containing-value movement, copied bindings, and tag-only exhaustive matching are complete; generic/polymorphic callable transport, capture, arrays/indirect wrappers, consuming/borrowed bindings, recursive owning aggregates, generics, and stable layout remain | `L-SUM-01` |
| Integer arithmetic modes | wrapping/saturating and checked-result add/subtract/multiply **complete in 0.111.0**; division/remainder/shift breadth is client-gated | `L-NUM-01` |
| Binary64 | **complete in 0.110.0** | `L-FLOAT-01` |
| Domain operators | **systems-readiness client-gated work** | `L-OP-01`; exact member/capability families only |
| Error propagation syntax | **complete in 0.311.0** | `L-ERR-01` |
| Production generic functions | **complete bounded production system:** type/value/pack declarations, exact inference and explicit expressions, inferred array extents, defaults/dependent values, bounded `&&`/`||` concept expressions, concrete reanalysis, HIR/MIR/lowered identity, callable confinement and public relations, verified-MIR execution, deterministic ordinary free/class/member/constructor emission, ownership/failure evidence, and the stdlib/tooling/runtime matrix are implemented | `L-GEN-01`, `E-SYM-01`, and `E-INST-01` done; general requires-expressions, partial specialization, subsumption/ranking, nested-type direct emission, and removal of the audited closure/incomplete-owner physical carriers remain later breadth |
| Wider/value generics, custom lifecycle bodies, block statics, generalized borrow graphs, `static_assert`, and wider integers | **bounded-first or later breadth as classified** | Stable `R-*` entries in the restriction ledger; a demonstrated readiness client may add a row |
| Shared/weak ownership and optional | systems-readiness library work | temporary/drop authority; atomics only for cross-thread shared owners |
| Formatting, text, host services | systems-readiness library work | ranges/views, failure contract, bounded runtime/FFI inputs |
| Container/algorithm foundation | systems-readiness accepted minimum | `L-CONT-01`, `L-RANGE-04` |
| Associative containers | **systems-readiness work with performance acceptance** | `L-CONT-02`, exact capabilities, allocator proposal; no benchmark-first design gate |
| Runtime performance | **ongoing design and delivery requirement** | Earlier O-PERF/O-GAME campaigns are historical completions; ADR 012 permits proactive general optimization architecture and requires performance acceptance |
| MIR transformations | **bounded local production pipeline** | `O-MIR-01` through `O-MIR-07` provide current transforms/analyses; `O-MIR-08/09` are measured-defer; the closed interprocedural client was subsumed by `O-PERF-02` |
| MIR-backed C++ emission | **ADR 016 complete** | every body in `tests/mir_census_baseline.json` uses the verified-MIR route; every backend consumes the self-contained `LoweredProgram` contract |
| Type interning | **measured defer** | context lifetime and allocation benchmark |
| Natural-loop/range facts | **complete bounded analysis; transform not implemented** | `O-MIR-06`; `O-MIR-08` may proceed from architectural need with replayable source-MIR facts and performance acceptance |
| Place alias/escape facts | **complete bounded analysis** | `O-MIR-07`; first standalone memory transform is gated by `O-MIR-09` |
| Incremental dominance | **measured defer** | add only when a measured transform makes full recomputation material |
| Compiler structural ownership | **exclusive campaign complete** | `R-STRUCT-00` through `R-STRUCT-05`; [archived closure report](archive/compiler-structural-refactor.md) |
| Source/semantic/HIR/MIR compiled migration | **complete foundation** | `C-MIG-02` through `C-MIG-07`; internal decomposition is owned by `R-STRUCT` |
| Deterministic names/concrete instance emission | backend/package preparation | MIR body authority and stable GTI-owned identity |
| Project dependencies/cache | parallel toolchain | tests -> cache -> path/workspace -> Git/lock |
| LSP project index/process isolation | client/risk-gated | project facts or stronger containment requirement |
| Separate compilation/stable GTI ABI | later-breadth proposal | deterministic names, layout/ABI, package model, concrete emission |
| Module vocabulary | Source interfaces implemented; standard-library modules implemented in 0.393.0 | ADR 024 defines imports, explicit exports/re-exports, package maps, and `export #include` republication; every public `<std/name>` unit is importable as `std.name`, and legacy includes remain unchanged. Remaining: an aggregate `import std;`. Its lowering prerequisite landed in 0.393.0, so artifacts lower only reachable code, but every imported unit is still analyzed, and the platform-gated units (`tcp`, `process`) need a selection rule. Also remaining: migrating standard units from republished legacy sources to real exports; separate compilation and independent module linkage identities. |

## Required Architectural Corrections

When any condition below appears, identify the smallest corrective goal and its
acceptance evidence. Implement it within the authorized outcome or record and
schedule it as a named prerequisite. The dependent row remains planned or active
until the correction passes; do not claim completion or bypass the invariant.

- a language rule can be explained only by generated C++ or an LLVM type;
- the proposed fact would have two authorities in semantics and MIR;
- a new private capability can be forged by spelling `gti_internal`;
- an ownership transition has no path-sensitive cleanup/verification story;
- a target/layout result comes from host `sizeof` rather than the selected
  target contract;
- a concurrency API relies on a borrow, capture, global, or cleanup capability
  not represented in semantics/HIR/MIR;
- proof, restriction, or framework work has no named user-facing consumer or
  current correctness defect;
- an optimizer wants a pass manager, incremental analysis, or LLVM container
  before the first client and measurement exist;
- a “mechanical” compiled-library move changes behavior; or
- a new executable language feature would add or extend AST/semantic/HIR-only
  C++ emission, or would treat MIR ignored by the production backend as
  feature completion; or
- a row would need a second prompt-sized feature to make its tests meaningful.

## Prompt Handoff Template

Use this shape when starting the next task:

```text
Implement <ROW-ID or BACKEND-RECOVERY-PHASE> from
docs/plans/implementation-sequence.md.

Before editing:
- verify every prerequisite and cite its current evidence;
- read the owning language, architecture, and domain-plan sections;
- name the readiness workload, public API, or user workflow this slice unlocks;
- classify its executable-authority impact and, for new runtime behavior, name
  the verified MIR-emitted body family that will own production execution;
- state the exact files/layers owned and the explicit non-goals.

Deliver the complete declared row or backend-recovery phase, including its
production MIR consumer. Add focused tests, run its exit gate plus the relevant
broader verification matrix, update the canonical docs and status evidence,
then stop. Reviewable commits are encouraged; an IR-only checkpoint is not a
phase exit when the matching production cutover is in scope.
```

`M-BACK-02` and ADR 016 are complete. A future backend task starts from the
immutable `LoweredProgram` consumer contract, exact corpus census, exhaustive
generated graph, and sealed whole-program plan. It must not link frontend
representations, parse generated C++, or recreate the retired no-MIR emitter as
a comparison, fallback, or legacy mode.

### Completed migration campaign: generated-item inventory

Native-callback, structural-operator, callable, lifecycle-cleanup, and concrete-
instance adapters are completed migration clients. Native callback semantic
selection, exact HIR-to-MIR identity, verified failure policy, and per-body MIR roots are copied into
sealed generated-item rows. Structural and callable adapters instead carry
exact function/operator or function/capability payloads and active-declaration
roots. Lifecycle cleanup carries the exact concrete class and destructor
instances, ordinary-versus-specialization form, failure mode, and destructor
declaration root. The planner verifies each census, source, payload, closure,
and order, and `CppEmitter` uses the sealed plan as the sole adapter-existence
authority. Concrete rows name each generic function/constructor declaration,
owner, optimized MIR body, and failure effect. Ordinary C/runtime declarations
already use ABI declaration/body rows, so native callbacks exhaust generated
native wrappers. C++ spelling remains backend-only.

1. Every generated adapter has a stable identity in the sealed whole-program
   plan and an exact census.
2. Each family uses immutable, target-independent representation rows whose
   inputs are semantic identities, concrete HIR
   instances, and verified MIR schedules. Keep C++ spelling and ABI policy in
   the backend representation layer.
3. Each row is structurally verifiable, its sole production consumer reads the
   sealed plan, and the corresponding AST/HIR eligibility query is deleted. GTI
   has no compatibility obligation during this pre-1.0 migration, so no
   fallback or dual-route comparison remains after a family lands.
4. **Exit gate met.** The generated-item census is complete, mutation tests
   reject missing, duplicate, stale, and reordered rows, and native O0/O3
   C++20/C++23 plus installed-toolchain oracles preserve program behavior. That
   sealed inventory becomes the shared input seam for the current C++ backend
   and a future LLVM backend.
