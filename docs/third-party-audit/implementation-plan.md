# Merged Implementation Plan: Architecture Fixes And LLVM Adoption

> **Status:** External proposal. Not canonical architecture and not an accepted
> plan. Nothing here describes implemented behavior. If accepted, this schedule
> belongs under `docs/plans/` and the two decision gates (Stage 0 and Stage 4)
> belong in ADRs.

Reviewed commit: `d861d18` (checkpoint 0.88.0)
Inputs merged by this document:

- [`audit.md`](audit.md) — architectural audit, cited as `A§N`
- [`llvm-audit.md`](llvm-audit.md) — LLVM adoption audit, cited as `L§N`
- [`docs/plans/compiler-library-migration.md`](../plans/compiler-library-migration.md)
  — cited as `M-Phase N`

**Standing direction for this plan:** where the two audits offered a GTI-owned
implementation and an LLVM implementation as alternatives for the same job,
this schedule resolves the tie **toward LLVM**, and sequences the enabling work
early enough for LLVM to be usable. §2.2 lists every tie that was resolved this
way and records what the neutral call would have been, so a later reader can
tell a directed choice from a technical conclusion.

---

## 1. Shape of the plan

Nine stages. The measured wins land early: editor latency and the diagnostic
quadratic in **Stage 1**, the instantiation quadratic in **Stage 3**, the
memory profile in **Stage 5**.

```text
Stage 0  Foundations            LLVM vendored, safety rails, no behavior change
Stage 1  Free wins              Tier-1 audit fixes + TimeProfiler + Triple
Stage 2  Migration wave 1       M-Phase 3/4 groundwork + APInt
Stage 3  The quadratic          instance delta model -> FoldingSet de-dup
Stage 4  Posture B              ADR: LLVM types may enter public headers
Stage 5  Representation         TypeContext interning + APFloat
Stage 6  Identity and dispatch  Kind tags + Casting.h, SourceUnitId, NodeId
Stage 7  MIR for passes         patch protocol, dominance, effect summaries
Stage 8  Long tail              raw_ostream, vfs, FileCheck, diagnostics, ...
```

Dependency structure — the two hard gates are Stage 0 and Stage 4:

```text
Stage 0 ──┬─> Stage 1 ──┬─────────────────────────────> Stage 6
          │             │
          └─> Stage 2 ──┴─> Stage 3 ──> Stage 4 ──> Stage 5 ──> Stage 7
                                                                   │
              (float semantics decision) ──> Stage 5 APFloat       └─> Stage 8
```

Stage 1 and Stage 2 can run concurrently by different people. Everything from
Stage 3 onward is serial.

---

## 2. What changed when merging

### 2.1 Corrections to the source documents

**`L§10` Phase 3's gate was unachievable as written.** It required HIR lowering
to "stop growing at all with unrelated base-program size" — which is `A§3.3`,
caused by the per-instance `SemanticVisitor` copy (`A§4.1`), not by the
de-duplication scan (`A§4.2`) that `FoldingSet` replaces. The profile in
`A§4.1` is dominated by `Symbol`, `FunctionCandidate`, and `ClassInfo` copy
constructors, which scale with whole-program state regardless of how instances
are looked up.

Fixed here by splitting Stage 3 into **3a (delta model, GTI-owned) then 3b
(`FoldingSet`, LLVM)**, each with its own gate. 3a is also the larger win, so
this ordering is correct independent of the gate problem.

**`L§7.5` declined TableGen on a premise worth revisiting.** The stated cost was
"a separate build-time executable." But `llvm-tblgen` is built as part of any
LLVM build — LLVM needs it for its own `.td` files — so under `GTI_BUNDLE_LLVM`
it arrives free. See §2.2; the recommendation is still Python, but for a
different and weaker reason.

**`A§4.4` and `L§7.2` agree and this plan follows both:** `llvm::SourceMgr` is
declined, and the line-index fix uses GTI's own code moved down from
`src/lsp/main.cpp:160`. Favouring LLVM does not extend to importing a source
model that cannot represent GTI's include graph.

### 2.2 Ties resolved toward LLVM by direction

| Job | Neutral call | This plan | Cost of the directed choice |
| --- | --- | --- | --- |
| HIR instance de-dup (`A§4.2`) | Either; hand-written map has no prerequisite | **`FoldingSet`** | Requires M-Phase 4 first — pulls Stage 2 forward |
| `isa`/`dyn_cast` helpers (`A§4.6`) | GTI's own; ~200 lines, stays in GTI's namespace | **`llvm/Support/Casting.h`** | Readers must know LLVM idiom; `Kind` tag work is unchanged |
| Type/identifier interning (`A§4.3`) | Either; `std::deque` + `unordered_map` is adequate | **`BumpPtrAllocator` + `FoldingSet` + `UniqueStringSaver`** | Forces the Posture B decision (Stage 4) earlier than `L§3` proposed |
| Dense flow-state sets (`A§4.7`) | `std::vector<bool>` is adequate | **`BitVector` / `SparseBitVector`** | Negligible |
| Emitter/printer output | `std::ostringstream` works | **`raw_ostream`** | 468 call sites; float formatting differs — snapshot-gated |
| Editor buffer overlays | `sourceOverrides` map works | **`llvm::vfs` overlay** | Waits for M-Phase 2 |
| Snapshot tests | Python/C++ substring checks work | **`lit` + `FileCheck`** for `--emit-cpp` and MIR only | New harness alongside CTest |
| Diagnostic table (`A§4.8`) | Python generator | **Python generator** — tie *not* flipped | See below |

**Why the diagnostic table stays Python.** TableGen would be free under bundled
LLVM, but it would make diagnostics — the most central compiler component —
unbuildable without `llvm-tblgen` on the host. That breaks the
`find_package(LLVM CONFIG)` path on any system whose LLVM package omits the
tool, and diagnostics are the wrong place to accept a build-fragility risk.
Revisit only if GTI standardizes on bundled-only LLVM.

**Posture B is pulled forward.** `L§3` recommended Posture A for phases 1–4 with
Posture B as a distant, separate ADR. Favouring LLVM adoption makes that
sequencing self-defeating: the ADT containers that deliver the memory win
(`A§4.3`) cannot be used from behind Posture A at all, because `SemanticType`
is a public header type. The Posture B decision therefore becomes **Stage 4**,
a scheduled gate rather than an open question — but it stays a real gate, taken
after Stages 1–3 have proven the vendoring, the handlers, and the determinism
rule in production.

---

## 3. Stage 0 — Foundations

**Goal.** LLVM is vendored, linked, and safe to use. No compiler behavior
changes.

**Prerequisites.** None.

| # | Work | Owner | Source |
| --- | --- | --- | --- |
| 0.1 | ADR: license posture (Apache-2.0-with-exception alongside MIT), adoption rubric, determinism rule, exception rule, Posture A→B trajectory | decision | `L§4`, `L§9.1` |
| 0.2 | `find_package(LLVM CONFIG)` with a pinned supported range; `GTI_BUNDLE_LLVM` via `FetchContent`; `GTI_RELEASE_BUILD` forces it on, mirroring `GTI_BUNDLE_JSON_C` at `CMakeLists.txt:145` | LLVM | `L§9.5` |
| 0.3 | CI link-surface assertion: only `LLVMSupport`, `LLVMDemangle`, `LLVMTargetParser` may be linked; anything under `llvm/IR`, `MC`, `Target`, `CodeGen` fails the build | LLVM | `L§9.5` |
| 0.4 | `install_fatal_error_handler` + `install_bad_alloc_error_handler` in `gti`, `gti_lsp`, and every test binary; `CrashRecoveryContext` around LSP analysis | LLVM | `L§9.3` |
| 0.5 | `llvm-LICENSE.txt` installed into `share/licenses/gti` beside the existing entries (`CMakeLists.txt:219`) | LLVM | `L§9.1` |
| 0.6 | Determinism test: same input, two separate processes, byte-identical `--emit-cpp` and MIR-print output | GTI | `L§9.2` |
| 0.7 | LLVM include dirs applied `SYSTEM PRIVATE`; LLVM's `-fno-rtti` must not propagate to GTI targets | LLVM | `L§9.6` |
| 0.8 | Update `README.md` build requirements and the release workflow for the new dependency | GTI | — |

**Gate.** Full `ctest` suite green with LLVM linked and **zero LLVM symbols
used**. This proves build, packaging, licensing, and crash-safety before any
behavior depends on them.

**Revert.** Delete the CMake option. Nothing else references LLVM.

**Why 0.4 comes before any LLVM use.** `gti_lsp` currently survives internal
failures through top-level `catch (...)` handlers (`src/lsp/main.cpp:1306`,
`:2472`). `llvm::report_fatal_error` bypasses those and calls `abort()`.
Introducing LLVM into the analysis path without handlers converts a recoverable
condition into a hard language-server crash.

---

## 4. Stage 1 — Zero-prerequisite wins

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
| 1.8 | Add GTI-owned layout fields to `TargetInfo` (pointer width/alignment, endianness, scalar alignments); derive `std::size_t`/`ptrdiff_t` from the selected target instead of `stdlib/prelude.gti:121` | GTI | `A§5.5` | closes a portability hole |

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

## 5. Stage 2 — Migration wave 1, and the first LLVM swap

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

## 6. Stage 3 — The instantiation quadratic

**Goal.** Remove the largest measured cost in the compiler. Two ordered
sub-stages with independent gates.

**Prerequisites.** Stage 2 (2.1 for 3a, 2.2 for 3b).

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

### 3b — `FoldingSet` instance de-duplication (LLVM)

Replace the linear scans in `enqueueClass` and `enqueueFunction`
(`include/gti/hir.h:528`, `:562`) with a `FoldingSet` keyed on
`(declaration id, argument list)` via a `Profile()` method.

**Instance storage stays a `std::vector`** — it is what assigns stable
`HirClassInstanceId`s and what every downstream consumer iterates. Only the
lookup index becomes an LLVM container. This is the `§8.1` determinism rule
applied.

**Gate.** The `A§3.2` benchmark: HIR lowering must be linear in distinct
instance count. Today 50 → 400 instances costs 99 ms → 8,073 ms.

**Revert.** 3b reverts to the linear scan without touching 3a. 3a is a
self-contained change to four functions plus the delta type.

---

## 7. Stage 4 — Posture B decision gate

**Goal.** Decide, once and explicitly, whether LLVM types may appear in
`include/gti/`.

**Prerequisites.** Stages 1–3 complete and soaked. Do not take this decision on
theory; take it after the vendoring, the fatal-error handlers, and the
determinism rule have run in production for a release cycle.

**What is being decided.** Today GTI installs `libgti_compiler.a` and
`include/gti/` as a consumable pair (`CMakeLists.txt:194`, `:203`), verified by
`compiler_library_boundary`. Posture B means installing LLVM headers alongside
`include/gti` and requiring consumers to have LLVM on their include path.

**What it unlocks.** Everything in Stage 5 — `BumpPtrAllocator`-owned interned
types in `SemanticType`, `APFloat` in `ConstantValue`, ADT containers in
`SemanticModel`. Under Posture A none of these is reachable, because all three
types are public.

**What it costs.** GTI's public API gains a dependency with no stable
cross-version ABI. This is less severe than it sounds — `docs/architecture/build-and-driver.md:30`
already states the archive and headers are "an exact-version pair without a
stable cross-version compiler ABI promise" — but it extends that constraint to
anyone compiling against GTI, not just linking.

**Deliverables.**

| # | Work | Owner |
| --- | --- | --- |
| 4.1 | ADR recording the decision and its blast radius | decision |
| 4.2 | If yes: install LLVM headers with `include/gti`; update `compiler_library_boundary` to link LLVM and to include a header that transitively includes `llvm/*.h` | LLVM |
| 4.3 | If no: Stage 5 falls back to GTI-owned interning (`std::deque` + `unordered_map` behind `TypeContext`) and an opaque `ConstantFloat`; the rest of the plan is unaffected | GTI |

**This gate is genuinely two-way.** The plan is written so a "no" here costs
Stage 5 its LLVM implementation and nothing else. Do not let the momentum of
Stages 0–3 pre-commit the answer.

---

## 8. Stage 5 — Representation: interning and float semantics

**Goal.** Fix the root cause identified in `A§4.3` and close a Milestone 0
release blocker.

**Prerequisites.** Stage 4. The `APFloat` item additionally requires a language
decision (see below).

### 5a — `TypeContext` interning

| # | Work | Owner |
| --- | --- | --- |
| 5a.1 | Design `TypeContext` as a **GTI-owned interface** handing out `TypeId` handles; equality becomes identity comparison; structural comparison stays private to the uniquing table | GTI |
| 5a.2 | Implement over `BumpPtrAllocator` + `FoldingSet` | **LLVM** |
| 5a.3 | Intern identifiers with `UniqueStringSaver`; name lookup keys on a handle rather than hashing `std::string` characters | **LLVM** |
| 5a.4 | Migrate storage sites incrementally: `ExpressionInfo` and `BindingInfo` first (highest count), then `HirValue`, `MirInstruction`, `Symbol` | GTI |

**Sequencing rule.** Build 5a.1 before 5a.2 and prove the interface against a
trivial implementation first. That order makes the LLVM choice reversible and
measurable rather than structural — and it is the interface you want regardless
(`L§11`).

**Gate.** Peak RSS on the `A§3.1` 25,600-line benchmark drops materially from
802 MB. Node sizes from `A§3.5` shrink: `SemanticType` from 120 B to a handle;
`HirValue` and `MirInstruction` correspondingly.

### 5b — `APFloat`

**Hard prerequisite: a language decision.** GTI must choose its float semantics
and record them in `docs/language/execution.md` §4.3. `APFloat` implements the
decision; it cannot make it. `docs/plans/compiler-roadmap-status.md`
Milestone 0 lists this as still required, and it is a 1.0 release blocker.

| # | Work | Owner |
| --- | --- | --- |
| 5b.1 | Language decision: NaN behavior, signed zero, contraction, conversion, rounding environment | decision |
| 5b.2 | Replace `double` in `ConstantValue` (`include/gti/constant_evaluator.h:28`) with a `ConstantFloat` holding an `APFloat` and its semantics tag | **LLVM** |
| 5b.3 | Route float constant folding through `APFloat` with explicit rounding mode and `opStatus` checking | **LLVM** |

**Why this matters beyond tidiness.** `docs/architecture/optimization.md`
forbids "host-C++ behavior as a proof." A folded float constant computed in
host `double` is exactly that, and no amount of care fixes it without a
target-independent float implementation.

**Gate.** New `compiler_pipeline` cases for NaN, signed zero, overflow to
infinity, inexact conversion, and float-to-integer truncation at the boundary.
`docs/plans/compiler-roadmap-status.md` Milestone 0 updated in the same change.

---

## 9. Stage 6 — Node identity and dispatch

**Goal.** Remove the RTTI dispatch mechanism and give the AST a dense identity.

**Prerequisites.** Stage 1. Independent of Stages 3–5, so it can run in parallel
with them if staffing allows.

| # | Work | Owner | Source |
| --- | --- | --- | --- |
| 6.1 | Add a `Kind` enum to `Expr`/`Stmt` (`include/gti/ast.h:330`, `:379`), set in each constructor, with `classof` per node | GTI | `A§4.6` |
| 6.2 | Use `llvm/Support/Casting.h` for `isa`/`cast`/`dyn_cast`/`dyn_cast_if_present` | **LLVM** | `L§6.5` |
| 6.3 | Migrate the 409 `dynamic_cast` sites, heaviest clusters first: `cpp_emitter.h` (94), `hir.h` (56), then `semantic_analyzer.h` (251) | GTI | `A§4.6` |
| 6.4 | Replace `SourceSpan::source` and `Token::source` strings with `SourceUnitId` | GTI | `A§4.4` |
| 6.5 | Assign each `Expr`/`Stmt` a snapshot-local `NodeId` at parse time; convert the 26 hash side tables (`include/gti/semantic_analyzer.h:2071`–`:2112`) to dense vectors; `clear()` becomes one loop | GTI | `A§4.5` |
| 6.6 | Dense binding indices for flow state; replace whole-`ScopeStack` copies at the ~30 branch/loop/switch sites with a flat per-binding state vector | GTI | `A§4.7` |
| 6.7 | `BitVector` / `SparseBitVector` / `IndexedMap` for that flow state | **LLVM** | `L§6.3` |

**Note on 6.1–6.2.** The cost here is 6.1 and 6.3, not the cast helpers.
`Casting.h` saves perhaps 200 lines. Do not block 6.1 on the LLVM decision —
it is independently valuable and can land against temporary GTI-owned helpers.

**Note on 6.5.** This also turns the Stage 3a instance delta into a sparse
overlay over dense vectors, which is materially simpler than a map clone.

**Gate.** `dynamic_cast` count reaches zero in `cpp_emitter.h` and `hir.h`;
`compiler_pipeline` green; `A§3.1` semantic-analysis time improves; the
determinism test still passes.

---

## 10. Stage 7 — MIR shaped for the passes it is planned to host

**Goal.** Establish the seams the roadmap's dataflow work needs, before the
first transforming pass is written.

**Prerequisites.** Stage 2 (M-Phase 4), Stage 6.5 for dense indices.

| # | Work | Owner | Source |
| --- | --- | --- | --- |
| 7.1 | Instruction addressing (`{block, index}`) and an accumulate-then-apply patch protocol. **GTI-owned**, modelled on rustc's `Location`/`MirPatch` | GTI | `A§5.4`, `L§7.1` |
| 7.2 | Make `rebuildMirValueUses` (`src/compiler/mir.cpp:332`) incremental at patch granularity, or add a dirty flag so consecutive passes rebuild once | GTI | `A§5.4` |
| 7.3 | Per-function conservative effect summaries from `MirFunctionInstance` bodies; consult them in `effects(const MirInstruction&)` (`src/compiler/optimization/effects.cpp:323`) for non-intrinsic calls | GTI | `A§5.4` |
| 7.4 | `GraphTraits<MirBody*>` specialization — successors from `MirTerminator`, predecessors precomputed | **LLVM** | `L§6.1` |
| 7.5 | Take `ReversePostOrderTraversal`, `df_iterator`, `po_iterator`, `scc_iterator`, `DominatorTreeBase`, `DomTreeUpdater`, and `LoopInfoBase` from that specialization | **LLVM** | `L§6.1` |
| 7.6 | Dataflow framework: lattice concept, transfer function over `MirInstruction`, worklist solver over `MirBody`. Second client should be the loan-flow analysis currently in `SemanticVisitor` (`:6530`–`:6669`) | GTI | `A§5.4` |
| 7.7 | Shrink `MirInstruction` from 776 B — move the rarely used call/construct payload behind a pointer, or make the representation kind-specific | GTI | `A§5.4` |

**`llvm::ilist` remains declined.** It presupposes address-identified nodes and
an SSA use-graph. GTI's MIR is non-SSA and place-based, and
`docs/architecture/mir.md` requires the printer to remain "deterministic and
address-free." This is the clearest case in the plan where an LLVM tool is
battle-tested for a problem GTI does not have — 7.1 is the correct answer and
it needs no dependency.

**7.4–7.5 are the strongest "do not rewrite this" item in LLVM.**
`GenericDomTree.h` and `GenericDomTreeConstruction.h` are templates over an
arbitrary CFG with no LLVM IR dependency. Semi-NCA dominator construction is
subtle, and an incorrect one produces silently wrong optimizations rather than
crashes.

**Gate.** The first analysis built on 7.6 reproduces the existing loan-flow
results exactly; MIR print output unchanged; `optimizer_foundation` green.

---

## 11. Stage 8 — Long tail

Ordered by value, not dependency. Each is independently schedulable once its
prerequisite lands.

| # | Work | Owner | Prerequisite | Source |
| --- | --- | --- | --- | --- |
| 8.1 | `RecoveryExpr` in the AST; return it instead of unwinding past parsed sub-expressions | GTI | — | `A§6.3` |
| 8.2 | Deterministic content-based symbol mangling replacing `__gti_fn_<counter>_` (`include/gti/cpp_emitter.h:2741`), applied uniformly including virtual methods | GTI | Stage 5a (canonical types) | `A§5.1` |
| 8.3 | Diagnostic table: `{code, default severity, group, format string}`; `report()` takes an enum. Python generator over a data file | GTI | — | `A§4.8` |
| 8.4 | `raw_ostream` in `CppEmitter` and `MirPrinter` | **LLVM** | M-Phase 6 | `L§6.4` |
| 8.5 | `llvm::vfs::InMemoryFileSystem` over `OverlayFileSystem` replacing the `sourceOverrides` map threading in `SourceLoader` | **LLVM** | M-Phase 2 | `L§8` |
| 8.6 | `lit` + `FileCheck` for the `--emit-cpp` and MIR-printer snapshot families only — not a wholesale test migration | **LLVM** | — | `L§8` |
| 8.7 | Parsed-unit cache keyed on `{path, content hash}` so an edit skips re-lexing and re-parsing the prelude and standard library | GTI | — | `A§6.2` |
| 8.8 | Concrete instance emission replacing C++ templates, one instance family at a time | GTI | Stage 3, 8.2 | `A§5.2` |
| 8.9 | Thread a location token into emitted checked-failure calls; single `gti_rt_fail(kind, location)` entry point replacing seven abort helpers | GTI | — | `A§7.2` |
| 8.10 | `llvm::json` replacing json-c in the LSP | **LLVM** | LSP state extraction | `L§8` |

**8.4 caveat.** `raw_ostream` float and pointer formatting differs from
iostreams. Every numeric literal path — including the nested `ostringstream` at
`include/gti/cpp_emitter.h:3673` — must be checked against emitted-C++
snapshots. 8.6 landing first would make this much safer.

**8.7 urgency note.** This is cheap today only because the standard library is
1,103 lines. At 20,000 lines the fixed per-keystroke cost is ~30 ms before the
user's own code; at 100,000 lines it is several hundred. Schedule it before the
library grows, not after.

---

## 12. Measurement obligations

One benchmark set, re-run at every gate. All inputs and expected shapes come
from `A§3`.

| Benchmark | Baseline (commit `d861d18`) | Gates it |
| --- | --- | --- |
| Phase timing, 25,600 lines | 989 ms total; sema 605, HIR 183, MIR 87 | 1.1, 1.3, 3a, 3b, 6.5 |
| Peak RSS, 25,600 lines | 802 MB (~33 KB/line) | 5a |
| Generic scaling, 50→400 instances | 99 → 8,073 ms | 3b |
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

**13.1 Determinism.** An LLVM hash container (`DenseMap`, `StringMap`,
`DenseSet`, `FoldingSet`) may be used as a **lookup index**. It must never be
iterated to produce diagnostics, printed IR, emitted C++, metadata, or any
other observable output. Where iteration order is observable, use
`llvm::MapVector`/`SetVector`, or keep the ordered `std::vector` that assigns
IDs and use the hash container only for lookup. GTI keys 26 side tables on
`const Expr*` — precisely the address-dependent shape this rule guards.

**13.2 Exceptions.** No GTI callback that can throw may be passed into an LLVM
API — no throwing comparator to `llvm::sort`, no throwing `Profile()` on a
`FoldingSet` node. The parser throws at 54 sites
(`include/gti/parser.h`) and LLVM is conventionally built `-fno-exceptions`.

**13.3 Link surface.** Only `LLVMSupport`, `LLVMDemangle`, `LLVMTargetParser`.
Enforced in CI (0.3).

**13.4 RTTI.** Never `dynamic_cast` or `typeid` an LLVM type; never derive a
GTI class from a polymorphic LLVM class that will be cast.

**13.5 Probation with an expiry.** Each swap lands behind a GTI-owned interface
with a differential test proving identical observable behavior. After one
release cycle of soak, **one implementation is deleted.** Permanently
maintaining two implementations of checked integer arithmetic is worse than
either choice alone.

**13.6 Interfaces first.** The interface that makes a swap reversible —
`TypeContext`, `parseTargetTriple`, a de-dup index behind `enqueueClass` — is
the interface you want regardless. Build it because it is right; reversibility
comes free. Do not build abstraction layers whose only purpose is keeping LLVM
optional.

---

## 14. Traceability

Every finding from the architectural audit, and where it lands.

| Finding | Stage | Owner |
| --- | --- | --- |
| `A§4.1` instance analysis copies the analyzer | 3a | GTI |
| `A§4.2` de-dup linear scan | 3b | **LLVM** |
| `A§4.3` `SemanticType` has no canonical identity | 5a | **LLVM** (interface GTI) |
| `A§4.4` `SourceManager::locate()` linear | 1.2 | GTI |
| `A§4.4` span/token path strings | 6.4 | GTI |
| `A§4.5` 26 AST-pointer side tables | 1.3, 6.5 | GTI |
| `A§4.6` 409 `dynamic_cast` sites | 6.1–6.3 | **LLVM** helpers |
| `A§4.7` scope-stack copies | 6.6, 6.7 | **LLVM** containers |
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

**Stage 4 pre-commitment.** Stages 0–3 build real momentum toward Posture B.
The plan is structured so a "no" at Stage 4 costs Stage 5 its LLVM
implementation and nothing else — but that only holds if the decision is taken
on merit. Write 4.3's fallback design *before* taking the vote.

**Stage 2 is the long pole.** M-Phase 3 (splitting semantic data from semantic
algorithms across a 20,949-line header) is the single largest piece of work in
this schedule and it is a prerequisite for Stage 3. Under-scoping it will
stall everything downstream. It is also the change with the least visible
payoff, which makes it the most likely to be deferred.

**Serialization from Stage 3 on.** Stages 0–2 and Stage 6 parallelize; the rest
does not. If throughput matters, staff Stage 6 concurrently with Stages 3–5 —
it shares no files with them beyond `semantic_analyzer.h`, which Stage 2 will
have already split.

**The float decision (5b.1) is not a compiler task.** It is a language
specification decision that gates a release blocker. Schedule it as its own
work item with its own owner, well ahead of Stage 5, or `APFloat` will arrive
with nothing to implement.
