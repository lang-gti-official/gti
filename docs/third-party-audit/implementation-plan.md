# Merged Implementation Plan: Architecture Fixes And LLVM Adoption

> **Status:** Historical execution proposal with implementation status notes.
> It is not canonical architecture; current boundaries are owned by ADR 006 and
> `docs/architecture/`. Unimplemented stages remain proposals and must be
> reassessed rather than treated as pre-approved LLVM migrations. Current
> prerequisite ordering and task status live in
> [`docs/plans/implementation-sequence.md`](../plans/implementation-sequence.md).

Reviewed commit: `d861d18` (checkpoint 0.88.0)
Inputs merged by this document:

- [`audit.md`](audit.md) — architectural audit, cited as `A§N`
- [`llvm-audit.md`](llvm-audit.md) — LLVM adoption audit, cited as `L§N`
- [`docs/plans/archive/compiler-library-migration.md`](../plans/archive/compiler-library-migration.md)
  — cited as `M-Phase N`

**Standing direction for this plan:** **LLVM should be selected only when it
clearly provides the best implementation—not merely because it is available.**
LLVM may implement language-neutral algorithms and private storage behind
GTI-owned interfaces. GTI owns language semantics, canonical identities,
cross-phase representations, serialized forms, and public APIs; LLVM types do
not enter public headers or become authoritative representations. LLVM is a
required toolchain dependency, but availability breaks no tie. Section 2.2
records and supersedes the proposal's earlier LLVM-directed choices.

---

## Current Disposition (2026-10-11)

The source/canonical-contract reconciliation below supersedes the early status
labels and stage order, not their original measurements. It records availability
against integrated source through 0.402.0; no fresh compiler or runtime tests
were run for this documentation-only audit.

| Original work | Current disposition / next prerequisite |
| --- | --- |
| Dependency and public boundary | One mandatory LLVM dependency, pinned bundle or compatible system LLVM 18+ with RTTI and exact matching Clang/libclang. Posture A remains standing policy. |
| Library/structural migration | Delivered compiled compiler, backend and driver boundaries, later separated catalog/instance/body/verifier/editor/text responsibilities and immutable lowered frontier. Do not restart Stage 2 or a structural freeze. |
| Ordered execution/private identity | Delivered trusted declaration identity and compiler-private publication rules, exact MIR call stages, normal/failure cleanup and source-site failure records. Remaining materialization profiles extend those general contracts. |
| Type/node/source/flow storage | AST kind queries and GTI casts are delivered; some RTTI sites remain. Canonical TypeContext ownership, dense NodeId/source-token handles and broader compact semantic flow state remain distinct design/migration goals. |
| MIR infrastructure | Stable instruction/body addressing, canonical edits/invalidation/replay, block parameters, dataflow/SCCP, per-instance effects and bounded loop/alias/capture/closed-call facts are delivered. Further memory/loop/inlining transforms need exact proof clients, not another foundational framework. |
| Diagnostics and editor reuse | Generated diagnostic identities/groups, lex/parse cache, error-tolerant query/completion profiles and documentation projection are delivered. General recovery nodes, finer semantic reuse and crash-state isolation remain separate goals. |
| Backend/linkage | Concrete verified MIR instances own execution; source modules and whole-program caches exist. Independent compilation/content-stable linkage needs its own ABI/identity contract, not type interning as an automatic blocker of current caches. |
| Measurement/selection | Preserve the instance-copy/occurrence-table attribution corrections and recorded results below. Architecture and explicit cost models can justify work under ADR 012; representative measurement validates acceptance. A private LLVM swap still must establish that it is the best implementation. |

The [implementation sequence](../plans/implementation-sequence.md) owns new
prompt-sized work. Remaining compiler-memory/identity goals must state snapshot
ownership, equality/serialization, invalidation, affected operation families
and cost model before changing central storage. Original stage numbering is
traceability, not a prerequisite chain to re-enact.

## 0. Recorded Early Execution Status

**Stage 0 and Stage 1 were implemented** (first execution pass, on top of the
exclusive-reborrow checkpoint). The initial optional-dependency probation used
both a portable configuration and LLVM 20.1.0. GTI 0.90.0 ended that probation:
there is now one mandatory LLVM-backed compiler, supplied either by a compatible
system installation or the pinned bundle
(`llvm-project-20.1.0.src.tar.xz`, sha256 `4579051e…aad9a`).

| Item | Status | Notes |
| --- | --- | --- |
| 0.1 ADR | **done** | [ADR 006](../decisions/006-llvm-support-adoption.md) |
| 0.2 CMake vendoring | **done; range revised** | One system LLVM 18+ build with RTTI and exact matching Clang, or the pinned `GTI_BUNDLE_LLVM` bundle; `GTI_RELEASE_BUILD` forces bundling. The original `<23` upper bound is retired. |
| 0.3 link-surface gate | **done** | `scripts/check_llvm_link_surface.py`, enforced in the mandatory LLVM build |
| 0.4 crash handlers | **handlers/guard delivered; state-safe containment incomplete** | `runIsolatedAnalysis` catches ordinary exceptions inside the guard and publishes outside `stateMutex`. Guarded frontend work also accesses shared `FrontendCache` locking; arbitrary non-local fault recovery is not proven state-safe. `test_worker_survives_failed_analysis` covers failed analysis, not every LLVM fatal signal/locked-cache fault. The lock/state-safe seam or process-isolation follow-up belongs in current LSP architecture/evolution. |
| 0.5 license install | **done** | `llvm-LICENSE.txt` installed when bundling |
| 0.6 determinism tests | **done** | `output_determinism` (two-process `--emit-cpp` byte compare) + cross-analysis MIR-print test in `optimizer_foundation` |
| 0.7 SYSTEM includes / no flag import | **done** | LLVM include directories stay private/system on `gti_compiler`; LLVM flags are never imported |
| 0.8 README | **done** | build-requirements note |
| 1.1 `stopAfter` + bounded LSP lowering | **done, revised** | `FrontendPhase` remains the boundary; the LSP now requests `Hir` so concrete generic diagnostics match compilation while failure metadata and MIR stay skipped |
| 1.2 SourceManager line index | **done** | binary-search `locate()`, allocation-free lookup. Error-path benchmark: **10.17 s → 1.29 s** at 25.6k lines/12.8k errors; scaling is now linear in diagnostics |
| 1.3 model move | **done** | deep copy replaced by `takeModel()` after HIR reanalysis finishes |
| 1.4 emitter type recognition | **delivered by later capability work** | Compiler-private functions/types bind by trusted declaration identity; application access to root `gti_internal` and private-signature leakage is rejected. The original emitter-only workaround is not a remaining task. |
| 1.5 ordered argument evaluation | **delivered through MIR** | Strict left-to-right receiver/input staging, active temporary/parameter/result ownership and failure cleanup are verified executable facts. No AST-emitter hoisting workaround or host evaluation-order mitigation defines the language. Remaining unsupported composition profiles extend this same authority. |
| 1.6 `TimeProfiler` | **done** | `PhaseTimeScope` shim (no llvm/* in public headers); five frontend phase scopes; `gti --time-trace <path>` emits valid Chrome Trace JSON |
| 1.7 `parseTargetTriple` | **done** | Private `llvm::Triple` normalization maps to exact GTI `os`/`vendor`/`arch` vocabulary (`aarch64`→`arm64`, `darwin`→`macos`); `parseTargetTripleResult` distinguishes malformed, unsupported-architecture, endianness, and operating-system failures while the optional wrapper remains compatible |
| 1.8 target data layout | **delivered, subsequently extended** | LLVM-free target/scalar/native layout facts, exact queries and general object-layout/manual-lifetime clients are implemented within their canonical eligibility rules. All selected targets remain 64-bit; another target width/layout needs a new coherent contract. |
| S-LAYOUT-02 follow-on | **delivered, subsequently extended** | Frontend `sizeof`/`alignof` queries, passive native records, recursive native pointer/callback fields and general `layout_of`/manual-lifetime families exist. Read canonical layout/raw-memory/native contracts for supported shapes rather than treating the original one-level-pointer/non-record limit as current. |

This follow-on closes the query half of the historical language audit's §4.2
finding without rewriting that audit. `S-ABI-01/02` subsequently closed the
passive layout-stable native-record subset. General layout control and broader
C ABI families remain separate future contracts.

Stage 2 status (second execution pass):

| Item | Status | Notes |
| --- | --- | --- |
| 2.1 M-Phase 3 (semantic data/algorithm split) | **complete; later structurally refined** | Semantic records and queries remain public contracts; catalog, declarations, bodies, ownership and concrete-instance algorithms compile behind private services. |
| 2.2 M-Phase 4 (HIR/MIR lowering to `.cpp`) | **complete; later structurally refined** | HIR instance/lowering and MIR body/program/hosted-startup implementations are compiled with exact ownership boundaries. |
| 2.3 checked-integer operations to `src/compiler/` | **done** | `evaluateCheckedIntegerUnary/Binary` compiled in `src/compiler/checked_integer.cpp`; header keeps types, trivial helpers, and declarations. `constant_evaluator.h` was inspected and left in place: it is a thin conversion layer whose arithmetic authority is entirely the two compiled entry points |
| 2.4 `llvm::APInt` swap | **done** | Two's-complement `APInt` implementation with `sadd_ov`-family overflow detection is the sole active implementation. The former portable evaluator and its completed probation evidence are preserved under `archive/compiler/`; they are not built or maintained as a fallback |

Stage 4/5/7 disposition (early measurements retained):

| Item | Status | Notes |
| --- | --- | --- |
| 4 Public-header posture | **closed: Posture A retained** | Installed headers remain LLVM-free. Changing that standing policy requires a new ADR. GTI owns type identity and casts; a measured private allocator or lookup index remains possible because it does not cross the interface |
| 5a `TypeContext` interning | **remaining ownership/migration goal** | Snapshot/context lifetime, GTI type identity and affected construction/equality/storage costs need a coherent design. The historical ~3% retained-memory result is not a current allocation-churn measurement or a ban on architectural work; private LLVM machinery is not pre-approved. |
| 5a′ compile-path occurrence opt-out | **done — the actual memory fix** | `FrontendOptions::toolingOccurrences` (default true) gates the occurrence table; the driver disables it because only editor position queries read it, while symbols stay recorded for HIR and the emitter. **Peak RSS: 789 MB → 512 MB at 25.6k lines (−35%) and 1,544 MB → 1,021 MB at 51.2k lines (−34%); user time −14%.** 41/41 examples byte-identical; contract covered by `testToolingOccurrenceOptOut` |
| 5b exact IEEE floats + `APFloat` | **done** | `BinaryFloat` is GTI's width-tagged exact binary32/binary64 representation. Decimal literals parse directly with `APFloat`; arithmetic, comparison, and numeric conversion use the same compiled engine; C++ emits retained bits with `std::bit_cast`; the native driver enforces `-fno-fast-math -ffp-contract=off` and defines the generated artifact's strict-policy marker. Focused lexer/evaluator/pipeline/CLI/driver tests cover both widths, promotion, rounding, signed zero, NaN, infinity, and conversion boundaries |
| 7.4–7.5 private dominance | **done, bounded** | `computeMirDominance` copies a valid `MirBody` CFG into a private pointer-stable snapshot, runs LLVM `GenericDomTree`, and returns only GTI reachability/immediate-dominator block IDs. The MIR verifier uses a fresh result for cross-block value availability. No tree is cached; `LoopInfo` and incremental updates remain deferred |

Stage 3 status (third execution pass):

| Item | Status | Notes |
| --- | --- | --- |
| 3a instance delta model | **done** | The four `analyze*Instance` whole-visitor copies are replaced by a detach/restore bracket (`InstanceAnalysisScope`) on the shared analyzer: instance analysis writes into an empty delta `SemanticModel`/`SemanticDatabase` that reads through to the detached base (loan tables deliberately restart instead of falling back, mirroring the old `clearLoans` semantics; record mutators materialize base records before updating; delta `SymbolId`s continue after the base's so identities never collide). **Gates:** HIR lowering at 400 distinct generic instances 8,073 ms → 24 ms; the A§3.3 experiment's per-instance term is eliminated (residual growth is the linear cost of lowering the added functions themselves); **41/41 examples emit byte-identical C++ across the change**. Note: 3a was sequenced after M-Phase 3 for hygiene, but the dependency proved soft — the delta was implementable against the existing model API because the visitor accesses the model exclusively through its methods |
| 3b instance de-dup index | **done, with a correction** | The four `enqueue*` linear scans are replaced by a compiled `HirInstanceIndex` (GTI-owned interface in `include/gti/hir_instance_index.h`, implementation in `src/compiler/hir.cpp` — the first compiled slice of HIR and the M-Phase 4 seam). Keys reproduce the previous scans' equality exactly, including the detail that free functions ignore class arguments and that an instance whose owner failed to resolve was never matchable; hashing uses `llvm::hash_combine`. The former splitmix fallback is archived outside the build. Per ADR 006 the index is lookup-only — the ordered `HirProgram` vectors remain the identity authority and the only thing iterated. **Correction to the audit:** this delivered **no meaningful measured speedup** — 0–3% on realistic workloads and ~10% on a constructed worst case (one value-generic instantiated 800 times). Audit §4.2 attributed the §3.2 quadratic partly to these scans; that was wrong. 3a's visitor copy was essentially all of it, and the scans' fast-fail first comparison keeps them cheap in practice. The change is justified as asymptotic insurance plus the compiled seam, **not** as a performance fix, and by the repository review skill's own rubric it would classify as *prepare for* rather than *fix now*. `audit.md` §4.2 carries the same correction |

## 1. Original Shape Of The Plan

Nine stages. The measured wins land early: editor latency and the diagnostic
quadratic in **Stage 1**, the instantiation quadratic in **Stage 3**, the
memory profile in **Stage 5**.

```text
Stage 0  Foundations            LLVM vendored, safety rails, no behavior change
Stage 1  Free wins              Tier-1 audit fixes + TimeProfiler + Triple
Stage 2  Migration wave 1       M-Phase 3/4 groundwork + APInt
Stage 3  The quadratic          instance delta model -> GTI-owned de-dup index
Stage 4  Header boundary        Posture A retained; later change needs new ADR
Stage 5  Canonical values       binary32/binary64 done; GTI TypeContext deferred
Stage 6  Identity and dispatch  Kind tags + GTI casts, SourceUnitId, NodeId
Stage 7  MIR for passes         infrastructure delivered; transforms remain
Stage 8  Long tail              diagnostics, caching, emission, ABI, ...
```

Dependency structure — Stage 0 establishes the mandatory dependency and Stage
4 is now a standing constraint rather than a future gate:

```text
Stage 0 ──┬─> Stage 1 ─────────────────────────> Stage 6
          ├─> Stage 2 ──> Stage 3
          ├─> exact float contracts ──> Stage 5b (done)
          └─> existing MIR CFG ──> Stage 7.4–7.5 (done)

snapshot/context ownership + operation/cost model ──> Stage 5a (remaining)
verified MIR infrastructure + exact proof clients ──> further transforms

Stage 4 constrains every stage: LLVM stays behind GTI-owned interfaces.
```

Stage 1 and Stage 2 can run concurrently by different people. Later work must
follow its recorded prerequisites, but independent TypeContext, node-identity,
and MIR-editor slices need not be serialized merely because they share this
historical plan.

---

## 2. What changed when merging

### 2.1 Corrections to the source documents

**`L§10` Phase 3's gate was unachievable as written.** It required HIR lowering
to "stop growing at all with unrelated base-program size" — which is `A§3.3`,
caused by the per-instance `SemanticVisitor` copy (`A§4.1`), not by the
de-duplication scan (`A§4.2`) that the proposed lookup index replaced. The
profile in `A§4.1` is dominated by `Symbol`, `FunctionCandidate`, and `ClassInfo` copy
constructors, which scale with whole-program state regardless of how instances
are looked up.

The proposal addressed this by splitting Stage 3 into **3a (delta model)** and
**3b (lookup index)**, each with its own gate. The implementation confirmed
that 3a was the real performance fix. Stage 3b landed as a GTI-owned index with
private LLVM hashing, produced no meaningful speedup on realistic workloads,
and is retained as asymptotic insurance rather than credited with removing the
quadratic.

**`L§7.5` declined TableGen on a premise worth revisiting.** The stated cost was
"a separate build-time executable." But `llvm-tblgen` is built as part of any
LLVM build — LLVM needs it for its own `.td` files — so under `GTI_BUNDLE_LLVM`
it arrives free. See §2.2; the recommendation is still Python, but for a
different and weaker reason.

**`A§4.4` and `L§7.2` agree and this plan follows both:** `llvm::SourceMgr` is
declined, and the line-index fix uses GTI's own code moved down from
`src/lsp/main.cpp:160`. Mandatory LLVM availability does not justify importing
a source model that cannot represent GTI's include graph.

### 2.2 Earlier directed choices and their current disposition

The original proposal intentionally resolved several neutral calls toward
LLVM. ADR 006 supersedes that policy. The table remains useful as an inventory,
but each unimplemented row must be evaluated on its own merits. LLVM's
mandatory presence lowers acquisition cost; it does not prove that an LLVM
abstraction is the best fit.

| Job | Current disposition | Reason |
| --- | --- | --- |
| HIR instance de-dup (`A§4.2`) | **implemented behind `HirInstanceIndex`** | GTI owns the interface, ordered instance vectors, and identity; the private implementation uses `std::unordered_map` and LLVM hashing |
| `isa`/`dyn_cast` helpers (`A§4.6`) | **GTI-owned** | `Kind` tags are the real work; putting `llvm::isa` across core frontend code adds broad surface for little value |
| Type/identifier interning (`A§4.3`) | **GTI contract and standard-library baseline first** | `TypeId`, type nodes, equality, and lifetime belong to GTI. A private allocator or index may be substituted only after measurement, without intrusive LLVM-shaped nodes |
| Dense flow-state sets (`A§4.7`) | **defer LLVM containers** | `std::vector<bool>` is adequate until a real sparse workload and client exist |
| Emitter/printer output | **declined** | Migrating hundreds of call sites changes formatting behavior without a demonstrated bottleneck |
| Editor buffer overlays | **defer** | Reconsider `llvm::vfs` only with a source-loader restructuring that supplies a concrete benefit |
| Snapshot tests | **defer** | Reconsider `lit`/`FileCheck` only with a test-harness restructuring; the current CTest coverage remains authoritative |
| Diagnostic table (`A§4.8`) | **Python generator if pursued** | Avoid making diagnostics depend on availability of a host `llvm-tblgen` executable |

**Why the diagnostic table stays Python.** TableGen would be free under bundled
LLVM, but it would make diagnostics — the most central compiler component —
unbuildable without `llvm-tblgen` on the host. That breaks the
`find_package(LLVM CONFIG)` path on any system whose LLVM package omits the
tool, and diagnostics are the wrong place to accept a build-fragility risk.
Revisit only if GTI standardizes on bundled-only LLVM.

**Posture A is the recorded decision.** Stage 4 is closed, not a recurring
vote. A future proposal to expose LLVM types must supply a concrete need and be
accepted in a new ADR. Private implementation machinery remains possible when
it cannot determine GTI identity, lifetime, serialization, or observable
ordering and measurements establish that it is the best implementation.

---

## 3. Recorded Stage 0 — Foundations

**Goal.** LLVM is vendored, linked, and safe to use. No compiler behavior
changes.

**Prerequisites.** None.

| # | Work | Owner | Source |
| --- | --- | --- | --- |
| 0.1 | ADR: license posture (Apache-2.0-with-exception alongside MIT), adoption rubric, determinism rule, exception rule, and LLVM-free public-header boundary | decision | `L§4`, `L§9.1` |
| 0.2 | `find_package(LLVM CONFIG)` with a pinned supported range; `GTI_BUNDLE_LLVM` via `FetchContent`; `GTI_RELEASE_BUILD` forces it on, mirroring `GTI_BUNDLE_JSON_C` at `CMakeLists.txt:145` | LLVM | `L§9.5` |
| 0.3 | CI link-surface assertion: only `LLVMSupport`, `LLVMDemangle`, `LLVMTargetParser` may be linked; anything under `llvm/IR`, `MC`, `Target`, `CodeGen` fails the build | LLVM | `L§9.5` |
| 0.4 | `install_fatal_error_handler` + `install_bad_alloc_error_handler` in `gti`, `gti_lsp`, and every test binary; `CrashRecoveryContext` around LSP analysis | LLVM | `L§9.3` |
| 0.5 | `llvm-LICENSE.txt` installed into `share/licenses/gti` beside the existing entries (`CMakeLists.txt:219`) | LLVM | `L§9.1` |
| 0.6 | Determinism test: same input, two separate processes, byte-identical `--emit-cpp` and MIR-print output | GTI | `L§9.2` |
| 0.7 | LLVM include dirs applied `SYSTEM PRIVATE`; LLVM's `-fno-rtti` must not propagate to GTI targets | LLVM | `L§9.6` |
| 0.8 | Update `README.md` build requirements and the release workflow for the new dependency | GTI | — |

**Historical gate.** The full `ctest` suite first passed with LLVM linked and
zero LLVM symbols used. That proved the initial build and license integration;
it did not prove complete LSP crash containment, whose current limitation is
now documented separately.

**Current reversal policy.** Replacing an adopted LLVM facility requires an
ordinary architecture decision and one new active implementation. Archived
pre-LLVM code is reference material, not a selectable build mode.

**Why 0.4 comes before any LLVM use.** `gti_lsp` currently survives internal
failures through top-level `catch (...)` handlers (`src/lsp/main.cpp:1306`,
`:2472`). `llvm::report_fatal_error` bypasses those and calls `abort()`.
Introducing LLVM into the analysis path without handlers converts a recoverable
condition into a hard language-server crash.

---

## 4. Recorded Stage 1 — Zero-prerequisite wins

**Goal.** Deliver the contained, user-visible wins and make everything after
this measurable. Runs concurrently with Stage 2.

**Prerequisites.** Stage 0.

| # | Work | Owner | Source | Measured payoff |
| --- | --- | --- | --- | --- |
| 1.1 | `FrontendOptions::stopAfter` (`Semantics`\|`Hir`\|`Mir`); LSP requests `Semantics` | GTI | `A§6.1` | 27–70% of editor latency |
| 1.2 | `SourceManager` line-offset index + binary search; transparent map lookup so `find(string_view)` stops allocating; `line()` reuses `locate()`'s result. Port `SourcePositionIndex` down from `src/lsp/main.cpp:160` | GTI | `A§4.4` | 11× on the error path, quadratic removed |
| 1.3 | Stop deep-copying `SemanticModel` at `include/gti/frontend.h:98` | GTI | `A§4.5` | ~4% of a large analysis |
| 1.4 | Emitter: recognize compiler-private types by resolved `ClassId`, not by name spelling (`include/gti/cpp_emitter.h:3321`) | GTI | `A§5.3` | correctness |
| 1.5 | Emitter: hoist effectful call arguments into ordered temporaries | GTI | `A§7.1` | correctness/portability |
| 1.6 | `TimeProfiler` scopes on `Frontend::analyze`'s phases and `HirLowerer`'s per-instance work; flag-gated Chrome Trace output | **LLVM** | `L§5.5` | replaces the bespoke harness |
| 1.7 | `parseTargetTriple()` in a compiled source using `llvm::Triple`; `TargetInfo` keeps its definition and its `host()` preprocessor path | **LLVM** | `L§5.3`, `A§5.5` | validated targets |
| 1.8 | Add a GTI-owned `TargetDataLayout` (pointer width, endianness, scalar sizes and ABI/preferred alignments); require later source aliases/layout queries to consume it rather than host C++ facts | GTI | `A§5.5` | closes a portability hole |

**Note on 1.7 + 1.8.** These are complementary, not alternatives. `Triple`
supplies parsing, normalization, and arch-derived pointer width; the layout
fields and their meaning are GTI's. Land them as one change.

**Gate.**
- `lsp_protocol` green; an LSP analysis leaves `hirValid`/`mirValid` false.
- A diagnostic near the end of a 25k-line file renders correct line/column, and
  the `A§3.4` error-path benchmark is linear.
- `--emit-cpp` snapshot shows ordered temporaries for `pair(bump(), bump())`.
- A class named `unique_owner` in a user namespace emits as an ordinary class.
- A malformed `--target` produces a GTI diagnostic, not an LLVM error.
- Trace file parses as valid Chrome Trace Format.

**Revert.** Each item is independent and individually revertible.

---

## 5. Recorded Stage 2 — Migration wave 1, and the first LLVM swap

**Goal.** Move the subsystems that LLVM adoption depends on out of headers. This
is `compiler-library-migration` work; this plan schedules it and gives it a
forcing function.

**Prerequisites.** Stage 0. Independent of Stage 1.

| # | Work | Owner | Source |
| --- | --- | --- | --- |
| 2.1 | M-Phase 3: split semantic data from semantic algorithms. Records, IDs, and query facades stay in headers; registration, lookup, overload selection, flow analysis, lifecycle, occurrence finalization, and generic reanalysis move to `src/compiler/` | GTI | M-Phase 3 |
| 2.2 | M-Phase 4: move `HirLowerer` instance worklists and lowering to `src/compiler/hir.cpp`; MIR body construction to `src/compiler/`. IR value types and read-only consumer APIs stay in headers | GTI | M-Phase 4 |
| 2.3 | Move `checked_integer` and `constant_evaluator` operations to `src/compiler/constants.cpp`. Public value types (`CheckedIntegerValue`, `CheckedIntegerDomain`, `ConstantInteger`) keep their definitions | GTI | `L§5.1` |
| 2.4 | Swap checked integer arithmetic to `llvm::APInt`/`APSInt` behind the unchanged public types; remove the `width <= 64` cap | **LLVM** | `L§5.1` |

**Gate.**
- `src/driver/compilation.cpp` compile time drops materially from the measured
  19.7 s (`A§3.6`).
- An implementation-only edit to semantics or HIR no longer recompiles CLI or
  LSP sources (M-Phase 3/4 acceptance criteria).
- **Differential arithmetic harness**: old and new implementations agree
  exhaustively across all 8- and 16-bit domain/operation/operand combinations,
  and across randomized 32/64-bit samples. This is the probation mechanism of
  `L§11` made concrete.
- `optimizer_foundation` and `compiler_pipeline` green.

**Revert.** 2.4 reverts alone by restoring the previous body of the operations;
the public types never changed. 2.1–2.3 are ordinary migration work with their
own acceptance criteria.

---

## 6. Recorded Stage 3 — The instantiation quadratic

**Goal.** Remove the largest measured cost in the compiler. Two ordered
sub-stages with independent gates.

**Prerequisites.** The proposal expected Stage 2 (2.1 for 3a, 2.2 for 3b).
Implementation showed those dependencies were soft: the delta model worked
through the existing semantic API, and `HirInstanceIndex` established the
first compiled HIR seam without migrating the whole lowerer.

### 3a — Instance-scoped delta model (GTI-owned; no LLVM equivalent)

Replace `SemanticVisitor instance = *this;`
(`include/gti/semantic_analyzer.h:2475`, `:2502`, `:2534`, `:2560`) with an
instance-scoped side model:

- a small `SemanticModel` holding only records produced for this instance;
- reads consult the delta first, then `instanceBaseModel`;
- reset the delta between instances instead of reconstructing the analyzer;
- separate the mutable *analysis* state (scope stack, current class,
  substitution maps, depth counters — already reset by
  `prepareInstanceAnalysis()`) from the accumulated *result* state.

Also resolve the `instanceBaseModel` lifetime ambiguity: it is only set by
`analyzeFunctionInstance` (`:2483`), so the constructor, destructor, and
field-initializer paths inherit whatever the copied object held. State the
invariant explicitly rather than depending on call order.

**Gate.** The `A§3.3` experiment: with generic instance count fixed at 100, HIR
lowering time must **stop growing with unrelated base-program size**. Today it
is 203 ms → 1,115 ms across 0 → 1,600 unrelated functions.

### 3b — GTI-owned instance de-duplication index (implemented)

The linear scans in `enqueueClass` and `enqueueFunction` were replaced by
`HirInstanceIndex`. Its public contract is GTI-owned; the implementation uses
`std::unordered_map` plus `llvm::hash_combine` in a compiled translation unit.
The ordered `HirProgram` vectors still assign stable instance IDs and remain
the only iterable authority. The index reproduces the prior equality rules and
cannot affect observable order.

The `A§3.2` benchmark corrected the proposal's attribution: the index produced
0–3% change on realistic workloads and about 10% on a constructed worst case.
Stage 3a removed essentially all of the apparent quadratic. Stage 3b is
therefore asymptotic insurance and a compiled HIR seam, not a claimed
performance fix. Introducing intrusive `FoldingSet` nodes would add
representation coupling without evidence of a better result.

**Revert.** 3b can return to the linear scan without touching 3a. 3a is a
self-contained delta-model change.

---

## 7. Stage 4 — Public-header posture (closed)

**Decision.** Posture A is retained: installed `include/gti/` headers remain
LLVM-free. [ADR 006](../decisions/006-llvm-support-adoption.md) owns the
boundary. A future change requires a new ADR; it is not an implementation
detail that later stages may relax.

This posture does not prevent private LLVM use. `TypeContext` may hide measured
allocation or lookup machinery, `BinaryFloat` exposes exact GTI-owned bits
while compiled code uses `APFloat`, and compiled MIR dominance hides an LLVM
CFG adapter. In every case GTI owns the public query, identity, lifetime, and
serialization contract.

**Standing gates.** Installed-header smoke tests must compile without LLVM
include directories. No `llvm/*` include or LLVM type may enter `include/gti/`.
Consumers still link the LLVM libraries propagated by the installed CMake
package because there is one mandatory LLVM-backed compiler build.

---

## 8. Stage 5 — Canonical types and exact floating-point values

**Goal.** Give exact floating-point values a GTI-owned representation and, if
future evidence justifies it, give semantic types GTI-owned canonical identity
without admitting LLVM representations into cross-phase interfaces.

**Boundary.** Stage 4 applies throughout. The binary32/binary64 phase is complete. Type
interning remains a snapshot/context ownership and storage-migration goal.
State the affected operations and cost model; allocation/equality measurements
validate its acceptance rather than granting permission to design it.

### 5a — `TypeContext` interning (remaining ownership/migration goal)

The early measurement attributed about 3% of retained semantic memory to types
after compile-path tooling occurrences were disabled; it is not a current
allocation-churn profile. Snapshot/context lifetime, canonical identity and
construction/equality/storage costs still need a coherent design. Define those
facts before changing storage and measure the resulting candidate; a prior
benchmark is not permission to begin architectural work.

| # | Work | Owner |
| --- | --- | --- |
| 5a.1 | Design `TypeContext` as a **GTI-owned interface** handing out `TypeId` handles; equality becomes identity comparison; structural comparison stays private to the uniquing table | GTI |
| 5a.2 | Establish the contract with ordinary GTI storage and a structural `std::unordered_map`; type nodes remain non-intrusive and LLVM-free | GTI |
| 5a.3 | Migrate storage sites incrementally: `ExpressionInfo` and `BindingInfo` first, then `HirValue`, `MirInstruction`, and `Symbol`; keep serialized/printed forms structural and deterministic | GTI |
| 5a.4 | Measure allocation churn and lookup cost after migration. Only if material, compare a private `TypeContext::Implementation` using `BumpPtrAllocator` and/or a non-authoritative LLVM index | measured option |
| 5a.5 | Treat identifier interning as a separate measured change; do not couple name identity to the type-storage decision | GTI |

**Sequencing rule if resumed.** First define snapshot/context ownership and the
construction/equality/storage cost model, then establish allocation evidence.
Build and test the GTI contract with ordinary
storage before considering private LLVM machinery. An LLVM allocator may own
bytes, and an LLVM container may accelerate lookup, but neither may define
`TypeId`, node shape, equality, caller-visible lifetime, or iteration order.

**Gate.** Canonical identity and migration correctness are primary. Re-run peak
RSS and allocation profiles, but do not claim the old 802 MB baseline as the
type problem: disabling compile-path tooling occurrences already reduced the
25,600-line case from 789 MB to 512 MB, while types in `ExpressionInfo`
accounted for only about 13.5 MB. Any private storage swap needs its own
before/after evidence. Design the missing ownership prerequisite as part of a
coherent slice instead of treating the old measurements as a permanent veto.

### 5b — exact binary32 and binary64 with `APFloat` (implemented)

GTI's normative binary32 and binary64 behavior is recorded in
`docs/language/execution.md` §4.6. GTI owns exact bits and the language rules;
`APFloat` implements parsing, arithmetic, comparison, and conversion in the
compiled evaluator.

| # | Work | Owner |
| --- | --- | --- |
| 5b.1 | Specified binary32/binary64 literals, promotion, arithmetic, comparisons, NaN, signed zero, contraction, conversion, and rounding environment | **done** |
| 5b.2 | Decimal literal spelling now goes directly to `APFloat`; the `std::stod`/host-`double` ingestion path is archived outside the build | **done** |
| 5b.3 | GTI-owned `BinaryFloat` stores exact width-tagged binary32/binary64 bits in tokens, semantic constants, HIR, MIR, and emitted replacements; no `APFloat` appears in a public header | **done** |
| 5b.4 | Arithmetic, comparisons, and conversions use `APFloat` with explicit round-to-nearest/ties-to-even or truncation-toward-zero as the operation requires; statuses map to GTI's specified default IEEE results or checked conversion failure | **done** |
| 5b.5 | C++ emits `std::bit_cast<float>` or `std::bit_cast<double>` from exact bits; the driver appends `-fno-fast-math` and `-ffp-contract=off`, then defines the required `__gti_strict_ieee754=1` policy marker. Direct artifact consumers must apply the same policy and define the marker | **done** |

**Why this matters beyond tidiness.** `docs/architecture/optimization.md`
forbids "host-C++ behavior as a proof." A folded float constant computed in
host `double` is exactly that, and no amount of care fixes it without a
target-independent float implementation.

**Coverage.** Lexer/evaluator/compiler-pipeline and driver/CLI cases cover
halfway and boundary literal rounding, signed zero, overflow to infinity,
NaN-producing operations and comparisons, inexact arithmetic/conversion, and
float-to-integer truncation at the boundary. The language, backend,
optimization, build/driver, grammar, and roadmap documents record the same
contract.

---

## 9. Stage 6 — Node Identity And Dispatch Disposition

**Remaining goal.** Finish justified classification migration and define dense
snapshot/body identity where it simplifies storage, lookup and invalidation.
AST kinds and GTI-owned classification helpers are already delivered.

**Prerequisites.** Stage 1. Independent of Stages 3–5, so it can run in parallel
with them if staffing allows.

| # | Work | Owner | Source |
| --- | --- | --- | --- |
| 6.1 | **Delivered:** exact `ExprKind`/`StmtKind` queries on AST nodes | GTI | `A§4.6` |
| 6.2 | **Delivered:** GTI-owned `exprAs`/`stmtAs` classification helpers, without LLVM public-header dependency | GTI | `A§4.6` |
| 6.3 | **Substantially delivered:** the old 409-site count is historical; remaining RTTI sites can migrate under focused classification/recovery evidence | GTI | `A§4.6` |
| 6.4 | Replace `SourceSpan::source` and `Token::source` strings with `SourceUnitId` | GTI | `A§4.4` |
| 6.5 | Assign each `Expr`/`Stmt` a snapshot-local `NodeId` at parse time; convert the 26 hash side tables (`include/gti/semantic_analyzer.h:2071`–`:2112`) to dense vectors; `clear()` becomes one loop | GTI | `A§4.5` |
| 6.6 | Dense binding indices for flow state; replace whole-`ScopeStack` copies at the ~30 branch/loop/switch sites with a flat per-binding state vector | GTI | `A§4.7` |
| 6.7 | Start dense flow state with a GTI-owned container (`std::vector<bool>` is adequate); evaluate an LLVM bit vector only after a real sparse client is measured | GTI / measured option | `A§4.7`, `L§6.3` |

**Note on 6.1–6.2.** The cost here is the kind-tag design and call-site
migration, not the helpers. GTI-owned helpers preserve the familiar idiom
without making hundreds of frontend sites read as LLVM code or exposing LLVM
types in core headers.

**Note on 6.5.** This also turns the Stage 3a instance delta into a sparse
overlay over dense vectors, which is materially simpler than a map clone.

**Remaining gate.** Prove exact classification and parser/tooling recovery for
remaining sites, with deterministic behavior and current cost evidence.
Removed implementation headers and the old 409-site inventory do not define
an unfinished full migration; new dense identities have separate snapshot and
publication obligations.

---

## 10. Stage 7 — MIR shaped for the passes it is planned to host

**Current goal.** Extend the delivered analysis/editing/proof foundation with
useful verified transforms; do not repeat a pre-first-pass infrastructure gate.

**Status.** The dominance adapter and subsequent stable editing/invalidation,
proof replay, per-instance effects, block parameters, worklist dataflow/SCCP
and bounded loop/alias/capture clients are implemented. Broader representation,
memory/loop and interprocedural transforms remain goals with exact proof and
performance obligations. The original rows below remain traceability, not a
request to build a second framework.

| # | Work | Owner | Source |
| --- | --- | --- | --- |
| 7.1 | **Delivered, revised:** stable body/instruction IDs and canonical edits/replay, not the original block/index-addressed proposal | GTI | `A§5.4`, `L§7.1` |
| 7.2 | **Delivered editing contract:** canonical repair, invalidation and revision-keyed analyses preserve use/CFG coherence; finer incremental cost improvements remain measured work | GTI | `A§5.4` |
| 7.3 | **Delivered:** conservative concrete-instance effect summaries, propagated failure/dispatch/capture facts and optimizer queries | GTI | `A§5.4` |
| 7.4 | **Done:** copy one structurally valid `MirBody` CFG into a private pointer-stable snapshot and run LLVM generic dominator construction | **LLVM behind GTI API** | `L§6.1` |
| 7.5 | **Done:** expose fresh `MirDominanceInfo` reachability/immediate-dominator queries in GTI block IDs and use them to verify cross-block value availability | GTI | `A§5.4`, `L§6.1` |
| 7.6 | **Delivered MIR foundation:** worklist/lattice dataflow, scalar block parameters/promotion and SCCP. General semantic loan endpoint/flow-state redesign remains a separate authority problem. | GTI | `A§5.4` |
| 7.7 | Representation compaction remains a candidate goal; 776 B is the historical baseline, not a current size claim. State current payload/access/allocation costs before choosing an indirect or kind-specific representation. | GTI | `A§5.4` |

**`llvm::ilist` is not selected for this representation.** GTI's stable-ID,
place-based MIR with scalar block parameters and canonical editing already has
an ownership/invalidation contract. Replacing its storage with intrusive nodes
is not a missing prerequisite of those passes. Any later private container
choice must preserve GTI identity, deterministic address-free output and
editing lifetime while clearly improving the applicable costs; availability
alone does not justify the migration.

**7.4–7.5 are the strongest "do not rewrite this" item in LLVM, but the first
adoption is intentionally narrow.** `GenericDomTree.h` and
`GenericDomTreeConstruction.h` are templates over an arbitrary CFG with no
LLVM IR dependency. Semi-NCA dominator construction is subtle, and an
incorrect implementation produces silently wrong optimizations rather than a
clear failure.

The adapter remains in `src/compiler/`; LLVM node and tree types never enter a
GTI header. `MirBody` stores blocks in a `std::vector`, so inserting a block may
invalidate pointer node identities required by the generic tree. The initial
analysis therefore has snapshot lifetime only, performs no CFG mutation while
live, returns no body or snapshot pointers, and is fully recomputed for every
verification. `LoopInfoBase` and incremental
dominance updates are deferred until a concrete optimization needs them and
MIR editing, stable node identity, and analysis invalidation are established.

The private snapshot node implements `printAsOperand(llvm::raw_ostream&)`
because `GenericDomTree` requires that diagnostic hook. This is confined
adapter plumbing: `MirPrinter`, `CppEmitter`, and observable GTI output remain
on GTI-owned interfaces, so it does not reverse the declined `raw_ostream`
emitter migration in Stage 8.

**Completed-slice gate.** Focused optimizer tests cover joins, loops,
unreachable blocks, malformed CFG rejection, immediate-dominator queries, and
verifier checks for direct and indexed cross-block uses. The broader Stage 7
infrastructure is delivered. New transforms extend its independent proof
replay/verifier, invalidation and optimized-runtime acceptance instead.

---

## 11. Stage 8 — Remaining And Delivered Disposition

Ordered by value, not dependency. Each is independently schedulable once its
prerequisite lands.

| # | Work | Owner | Prerequisite | Source |
| --- | --- | --- | --- | --- |
| 8.1 | General AST recovery nodes remain a possible extension; error-tolerant current completion already has a delivered bounded profile | GTI | Exact recovery/type/publication contract | `A§6.3` |
| 8.2 | Independent linkage/content-stable mangling remains a goal; current source modules and whole-program caches are delivered | GTI | ABI/declaration identity contract; no automatic TypeContext dependency | `A§5.1` |
| 8.3 | **Delivered foundation:** generated `DiagnosticId`/descriptor registry and warning groups; further per-message migration/policy extends that registry | GTI | Exact diagnostic identity and safe actions | `A§4.8` |
| 8.4 | **Declined:** `raw_ostream` migration in `CppEmitter` and `MirPrinter`; current output is correct and native C++ compilation dominates | — | — | `L§6.4` |
| 8.5 | **Deferred:** reconsider `llvm::vfs` only if a source-loader restructuring gives overlays a concrete client and measurable simplification | measured option | M-Phase 2 | `L§8` |
| 8.6 | **Deferred:** reconsider `lit` + `FileCheck` only if the snapshot harness itself is restructured | measured option | — | `L§8` |
| 8.7 | **Delivered:** bounded lexed/parsed source-unit cache with content/configuration/dependency identities and retained immutable AST ownership | GTI | Finer semantic reuse/invalidation is separate | `A§6.2` |
| 8.8 | **Delivered execution authority:** concrete verified MIR instances feed the immutable lowered frontier; native template lookup/instantiation does not decide body meaning | GTI | Broader representation clients preserve exact identities | `A§5.2` |
| 8.9 | **Delivered:** exact artifact/site defined-failure records and checked cleanup/propagation; allocation exhaustion has its separate report-and-abort contract | GTI | New families preserve first/secondary failure and ownership | `A§7.2` |
| 8.10 | **Done:** `llvm::json` replaced json-c in the private LSP protocol translation unit | **LLVM** | Landed as a protocol-only value/RAII migration; document-state extraction remains independently deferred | `L§8`, ADR 006 |

**8.4 decision.** `raw_ostream` would touch hundreds of call sites and changes
float and pointer formatting. No measured emission bottleneck justifies that
surface, so it is declined rather than left as momentum-driven future work.
The private dominance snapshot's required
`printAsOperand(llvm::raw_ostream&)` compatibility hook is not an adoption for
GTI emission and does not weaken this decision.

**8.7 follow-on.** The old 1,103-line baseline and projected latency are
historical motivation. Lex/parse reuse exists; current editor costs include
whole-program semantic/concrete-instance work. Select scoped/demand-driven
reuse from current evidence while preserving identity, invalidation, recovery
and query completeness.

---

## 12. Recorded Measurement Obligations

One benchmark set, re-run at every gate. All inputs and expected shapes come
from `A§3`.

| Benchmark | Baseline (commit `d861d18`) | Gates it |
| --- | --- | --- |
| Phase timing, 25,600 lines | 989 ms total; sema 605, HIR 183, MIR 87 | 1.1, 1.3, 3a, 3b, 6.5 |
| Peak RSS, 25,600 lines | 802 MB historical; 512 MB after occurrence opt-out, with only ~13.5 MB attributed to expression types | measure 5a; do not assume a win |
| Generic scaling, 50→400 instances | 99 → 8,073 ms historical | 3a attribution; 3b regression guard |
| Base-size scaling, 100 instances, 0→1,600 extra fns | 203 → 1,115 ms | **3a** |
| Error-path, 25,600 lines / 12,800 errors | 10.17 s vs 0.93 s clean | 1.2 |
| Node sizes | `SemanticType` 120 B, `HirValue` 632 B, `MirInstruction` 776 B, `SemanticOccurrence` 872 B | 5a, 7.7 |
| TU compile time, `src/driver/compilation.cpp` | 19.7 s / 128,349 preprocessed lines | 2.1, 2.2 |
| Two-process output determinism | byte-identical | every stage from 0 on |

Once 1.6 lands, `TimeProfiler` supplies the phase timings directly and the
bespoke harness can be retired.

---

## 13. Standing rules

These apply from Stage 0 onward and should be added to the architecture docs,
not just this plan.

**13.1 Determinism.** A private LLVM hash container (`DenseMap`, `StringMap`,
`DenseSet`, `FoldingSet`) may be used as a **lookup index**. It must never be
iterated to produce diagnostics, printed IR, emitted C++, metadata, or any
other observable output. Keep a GTI-owned ordered sequence that assigns IDs
and use the hash container only for lookup. GTI keys 26 side tables on `const
Expr*` — precisely the address-dependent shape this rule guards.

**13.2 Exceptions.** No GTI callback that can throw may be passed into an LLVM
API — no throwing comparator to `llvm::sort`, no throwing `Profile()` on a
`FoldingSet` node. The parser throws at 54 sites
(`include/gti/parser.h`) and LLVM is conventionally built `-fno-exceptions`.

**13.3 Link surface.** Only `LLVMSupport`, `LLVMDemangle`, `LLVMTargetParser`.
Enforced in CI (0.3).

**13.4 RTTI.** Never `dynamic_cast` or `typeid` an LLVM type; never derive a
GTI class from a polymorphic LLVM class that will be cast.

**13.5 Probation with an expiry.** A swap may use a differential test against
the previous implementation while the choice is being validated. The released
compiler has one active implementation. Displaced code may remain temporarily
under `archive/` as non-built reference material, then is deleted when its
review and rollback value expires. Permanently maintaining two implementations
of checked integer arithmetic is worse than either choice alone.

**13.6 Interfaces first.** The interface that makes a swap reversible —
`TypeContext`, `parseTargetTriple`, a de-dup index behind `enqueueClass` — is
the interface you want regardless. Build it because it is right; reversibility
comes free. Do not build abstraction layers whose only purpose is keeping LLVM
optional.

**13.7 Representation authority.** LLVM may implement language-neutral
algorithms and private storage behind GTI-owned interfaces. GTI owns language
semantics, canonical identities, cross-phase representations, serialized
forms, and public APIs. LLVM types stay out of `include/gti/` and must not
become authoritative through hidden pointer identity, lifetime, or ordering
assumptions.

---

## 14. Traceability

Every finding from the architectural audit, and where it lands.

| Finding | Stage | Owner |
| --- | --- | --- |
| `A§4.1` instance analysis copies the analyzer | 3a | GTI |
| `A§4.2` de-dup linear scan | 3b | GTI index, private LLVM hashing |
| `A§4.3` `SemanticType` has no canonical identity | 5a | GTI; private storage measured later |
| `A§4.4` `SourceManager::locate()` linear | 1.2 | GTI |
| `A§4.4` span/token path strings | 6.4 | GTI |
| `A§4.5` 26 AST-pointer side tables | 1.3, 6.5 | GTI |
| `A§4.6` 409 `dynamic_cast` sites | 6.1–6.3 | GTI |
| `A§4.7` scope-stack copies | 6.6, 6.7 | GTI baseline; LLVM container deferred |
| `A§4.8` ad-hoc diagnostic codes | 8.3 | GTI |
| `A§5.1` counter-based symbol names | 8.2 | GTI |
| `A§5.2` generics as C++ templates | 8.8 | GTI |
| `A§5.3` emitter recognizes types by spelling | 1.4 | GTI |
| `A§5.4` MIR not shaped for passes | 7.1–7.7 | mixed |
| `A§5.5` `TargetInfo` has no layout | 1.7, 1.8 | **LLVM** + GTI |
| `A§6.1` LSP lowers unused HIR/MIR | 1.1 | GTI |
| `A§6.2` no parsed-unit cache | 8.7 | GTI |
| `A§6.3` no AST error node | 8.1 | GTI |
| `A§7.1` evaluation order left to host C++ | 1.5 | GTI |
| `A§7.2` checked failures abort without location | 8.9 | GTI |
| `A§7.3` documentation comments unreachable | deferred | — |

Fourteen of twenty are GTI-owned. That ratio is expected and correct: LLVM
supplies infrastructure, and most of what the audit found is GTI's own
representation and layering. **Adopting LLVM does not substitute for that
work.**

---

## 15. Risks to this plan

**The substitution risk.** The largest wins — 3a, 5a's interface, 6.5, 7.1 —
are architecture changes LLVM containers do not deliver. A `DenseMap` in place
of an `unordered_map` does not fix copying the whole semantic model per generic
instance. The gates in §12 are chosen so this cannot be papered over: 3a's gate
can only be passed by 3a.

**Boundary erosion.** Posture A is closed, but private LLVM machinery can still
quietly become authoritative if GTI starts exposing its node identities,
lifetime assumptions, or iteration order. Review each adoption against the
full ADR boundary, not only the absence of an `llvm/*` include in public
headers.

**The original Stage 2 long pole is complete.** Compiled-library and later
structural ownership boundaries supersede the 20,949-line-header baseline.
Remaining identity/storage migrations use those services and need their own
snapshot, invalidation and performance acceptance; they do not justify
restarting the old extraction or treating it as an unmet prerequisite.

**Oversized cross-cutting migrations.** Type handles, AST identities, and MIR
editing each touch central structures. Land independently testable slices and
avoid coupling them merely to follow the original stage numbering.

**The float work had to begin before constant evaluation.** The completed phase
removed host conversion at literal ingestion and established backend/runtime
parity in addition to changing constant storage. Replacing only the `double`
held by `ConstantValue` would have preserved the earliest precision loss and
created a false sense of completion.
