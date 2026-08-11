# LLVM Support Adoption: Audit And Implementation Plan

> **Status:** Historical external review and proposal. It is not canonical
> architecture. Some early recommendations were later implemented, but current
> policy belongs to ADR 006 and `docs/architecture/`. In particular, GTI now
> has one mandatory LLVM-backed build and does not preserve selectable
> LLVM/non-LLVM implementations.

Reviewed commit: `d861d18` (checkpoint 0.88.0)
Review type: read-only. No source file was modified.
Companion document: [`audit.md`](audit.md) — the architectural audit this plan
repeatedly refers to. Findings are cited as `audit §N`.

## 0. What this plan is and is not

**In scope.** LLVM's *compiler-engineering support libraries*: `llvm/ADT`,
`llvm/Support`, and `llvm/TargetParser`. These are language-agnostic
infrastructure — arbitrary-precision arithmetic, IEEE-754 floating point,
target triple parsing, generic dominator-tree construction over an arbitrary
CFG, allocators, uniquing containers, and telemetry.

**Explicitly out of scope**, per the request and per
`docs/architecture/backend.md`: LLVM IR, `llvm/IR`, `llvm/MC`, `llvm/Target`,
`llvm/CodeGen`, ORC, LTO, and anything that would make LLVM a backend. GTI's
C++ backend and the reasons LLVM emission is premature are unchanged by this
document.

**The governing principle.** LLVM is adopted where GTI would otherwise
reimplement a *specification-defined, language-neutral* algorithm, confined
behind a GTI-owned interface, and never where it would encode another
language's model inside GTI's semantics.

The later decision sharpens that principle: **LLVM should be selected only
when it clearly provides the best implementation—not merely because it is
available.** The original candidate verdicts below are historical proposals,
not automatic approvals for unfinished migrations.

---

## 1. Summary and recommendation

**Recommendation: adopt, narrowly, in five stages, gated behind an explicit
posture decision about public headers.**

There are three genuinely strong candidates, two moderate ones, and a
surprisingly long list of things that look attractive and should be declined.

| Area | Verdict | Why | Links |
| --- | --- | --- | --- |
| `APInt`/`APSInt` for checked integers | **adopt** | GTI's model is capped at 64 bits by construction | §5.1 |
| `APFloat` for float constants | **adopt** | Closes a Milestone 0 release blocker that host `double` cannot | §5.2 |
| `Triple`/`TargetParser` for `TargetInfo` | **adopt** | Replaces three unvalidated strings | §5.3, audit §5.5 |
| `FoldingSet`/`DenseMap` for HIR instance de-dup | **adopt** | Removes a measured O(n^2) | §5.4, audit §4.2 |
| `TimeProfiler` for compile-time telemetry | **adopt** | The performance plan wants exactly this | §5.5 |
| `GenericDomTree` + `GraphTraits` for MIR | **prepare for** | Best "do not rewrite this" item in LLVM | §6.1, audit §5.4 |
| `BumpPtrAllocator` + `FoldingSet` for type interning | **prepare for** | Blocked on the public-header posture | §6.2, audit §4.3 |
| `BitVector`/`SparseBitVector` for dataflow | **prepare for** | Waits for the dataflow framework | §6.3, audit §4.7 |
| `raw_ostream` in emitter/printer | **prepare for** | Waits for those files to leave headers | §6.4 |
| `Casting.h` (`isa`/`dyn_cast`) | **adopt if present, do not vendor for** | The cost is the `Kind` tags, not the helpers | §6.5, audit §4.6 |
| `llvm::ilist` for MIR instructions | **decline** | Encodes an SSA/instruction-list model GTI does not have | §7.1, audit §5.4 |
| `llvm::SourceMgr` | **decline** | No include graph, no source-unit identity | §7.2, audit §4.4 |
| `llvm::Error`/`Expected` as error model | **decline** | GTI already has a better one, and ADR 001 owns it | §7.3 |
| `llvm::cl` | **decline** | The CLI contract is frozen and tested | §7.4 |
| TableGen for diagnostics | **decline** | A Python generator gives 90% at 5% of the cost | §7.5, audit §4.8 |
| `llvm::json`, `llvm::vfs`, `lit`/`FileCheck` | **evaluate later** | Real but not compiler-engineering wins | §8 |

The single most important constraint is not technical merit — it is §3.

---

## 2. Why LLVM is a reasonable fit here

Three of the audit's findings are cases where GTI has written a simplified
version of a problem that has a canonical, heavily tested solution:

- `CheckedIntegerValue` is `{bool negative; uint64_t magnitude}`
  (`include/gti/checked_integer.h:10`) with a hard `domain.width <= 64` guard
  (`:54`). That is a narrow special case of `llvm::APInt`.
- `ConstantValue` stores floats as a host `double`
  (`include/gti/constant_evaluator.h:28`). `docs/architecture/optimization.md`
  forbids "host-C++ behavior as a proof," and a host `double` is exactly that.
- `TargetInfo` is `{std::string os, vendor, arch}` (`include/gti/target.h:14`)
  populated by `#if` ladders, with no parsing, no validation, and no derived
  facts.

None of these is a GTI language decision. All three are places where the
compiler is *smaller than the problem*, and where LLVM's version is not merely
equivalent but strictly more capable in exactly the direction the roadmap
already says GTI must go.

Equally, the repository already has a working vendoring practice to build on:
checked-in header drops (`vendor/expected_lite`, `vendor/tomlplusplus`) and an
optional `FetchContent` bundle gated by a CMake option with a pkg-config
fallback (`GTI_BUNDLE_JSON_C`, `CMakeLists.txt:145`–`:184`). LLVM fits the
second pattern. License installation into `share/licenses/gti` is already a
convention (`CMakeLists.txt:219`–`:235`).

---

## 3. The governing constraint: GTI's public header surface

This decides the shape of the entire plan, so it comes before the candidates.

GTI installs `libgti_compiler.a` **and** `include/gti/` as a consumable pair
(`CMakeLists.txt:194`, `:203`), and `compiler_library_boundary` links a smoke
program against exactly that (`tests/compiler_library_smoke.cpp`). Meanwhile
the compiler is still header-dominant: `include/gti/semantic_analyzer.h` is
20,949 lines of implementation, and `hir.h`, `mir.h`, `parser.h`, and
`cpp_emitter.h` are the same.

Therefore:

> **Any `llvm/*.h` included from `include/gti/` becomes a mandatory transitive
> dependency of every consumer of the installed GTI compiler library.**

`docs/plans/compiler-library-migration.md` already names this exact failure
mode as one of its motivations: *"private implementation dependencies become
transitive consumer dependencies."* LLVM adoption is the concrete case that
motivation was describing.

Two postures follow, and the plan should not blur them.

### Posture A — LLVM is private to compiled translation units

`llvm/*.h` appears only in `src/**/*.cpp`. GTI's public types keep their
current definitions; LLVM appears only in implementations. Consumers of
`libgti_compiler.a` need nothing new at compile time. They do need LLVM
Support at *link* time, which is already the situation for any static archive
with dependencies and is consistent with GTI's existing statement that the
archive and headers are "an exact-version pair" with no cross-version ABI
promise (`docs/architecture/build-and-driver.md:30`).

Everything in §5 is achievable under Posture A. Some of it requires the
relevant subsystem to move to `src/compiler/` first — which is the
compiler-library-migration plan's own work, not new work invented here.

### Posture B — LLVM types enter GTI's public headers

`SemanticType` holds an `llvm::SmallVector`; `SemanticModel` is built from
`llvm::DenseMap`; interned types are `BumpPtrAllocator`-owned. This is what
audit §4.3 and §4.7 ultimately want, and it is where the large performance
wins live.

It requires a deliberate decision to install LLVM headers alongside
`include/gti` — precedent exists (`install(DIRECTORY vendor/expected_lite/include/nonstd ...)`,
`CMakeLists.txt:207`) but LLVM's header set is thousands of files, not one.

**Recommendation: commit to Posture A for phases 1–4. Treat Posture B as a
separate, explicit ADR decision taken only after the migration plan has moved
the affected subsystem behind a declaration boundary.** The two plans then
reinforce each other instead of competing: each subsystem that migrates to
`.cpp` becomes eligible for private LLVM use, and the desire for private LLVM
use gives the migration a concrete forcing function.

---

## 4. Adoption rubric

The request asks that future development be able to weigh LLVM against a
custom GTI implementation case by case. This is the proposed test. It should
live in the ADR, not in this audit.

**Adopt LLVM when all five hold:**

1. The problem is **specification-defined and language-neutral** — IEEE-754,
   arbitrary-precision integer arithmetic, dominator construction, triple
   grammar. There is a right answer that is not GTI's to choose.
2. GTI's current version is a **known simplification** that a `docs/plans/`
   document already lists as insufficient.
3. The API lives in **`ADT`, `Support`, or `TargetParser`** — no dependency on
   `llvm/IR`, `MC`, `Target`, or `CodeGen`.
4. It can be **confined behind a GTI-owned type or function** so that the
   LLVM type is an implementation detail, not the interface.
5. It does not **enter GTI's semantic model or public headers** unless Posture
   B has been separately decided.

**Roll GTI's own when any one holds:**

1. The behavior **is GTI language semantics** — ownership, loans, effects,
   checked-operation policy, visibility, overload resolution.
2. LLVM's version **encodes another language's model**. `SourceMgr` assumes a
   flat buffer set with no include graph; `ilist` assumes an instruction-list
   IR. Adopting these imports assumptions GTI has deliberately rejected.
3. The abstraction would **leak an LLVM concept into a GTI document** — the
   language spec, the diagnostics contract, the MIR contract. If explaining
   GTI to a user requires explaining an LLVM type, the abstraction is wrong.
4. **Determinism or diagnostic quality would regress.** See §9.2.
5. GTI's version is **already correct and tested**, and the change is churn.

**The sharpest single question** is not "LLVM or custom?" It is: *does the
LLVM type become load-bearing in GTI's semantic model?* `llvm::APInt` inside
`CheckedIntegerValue` is fine — `CheckedIntegerValue` remains GTI's type and
GTI's contract. `llvm::DenseMap` **as the definition of** `SemanticModel`'s
storage is a materially larger commitment, and should be treated as one.

---

## 5. Adopt: well-scoped, high-value, Posture A

### 5.1 `llvm::APInt` / `APSInt` for checked integer arithmetic

**Current.** `include/gti/checked_integer.h` models an integer as
`{bool negative; uint64_t magnitude}` (`:10`) plus a `{width, signedValue}`
domain (`:18`), with `validCheckedIntegerDomain` rejecting anything above
64 bits (`:52`–`:55`).
`ConstantInteger` (`include/gti/constant_evaluator.h:15`) carries the same
representation. Overflow detection is hand-written per operation.

**What LLVM gives.** `APInt` is arbitrary-width with exact two's-complement
semantics and dedicated overflow-reporting operations
(`sadd_ov`, `ssub_ov`, `smul_ov`, `sdiv_ov`, `ushl_ov`, and unsigned
equivalents), plus correct division/remainder semantics, bit counting, and
sign extension. `APSInt` adds signedness tracking. This is the single most
exhaustively tested arithmetic implementation available.

**Why it matters beyond tidiness.** The 64-bit cap is not a limitation the
language chose — it is a limitation of the representation. Any future
`int128`, any bit-width-parameterized integer, and any target whose pointer
width the constant evaluator must reason about (audit §5.5) runs into it.

**Confinement.** `CheckedIntegerValue`, `CheckedIntegerDomain`,
`CheckedIntegerOutcome`, and `ConstantInteger` keep their current public
definitions. Only the *operations* change implementation. Those operations are
currently `inline` in the header, so this requires moving
`checked_integer` and `constant_evaluator` implementations to
`src/compiler/` — a migration-plan step that is independently desirable.

**Risk: low.** The behavior contract is already pinned by
`optimizer_foundation` and `compiler_pipeline` tests. A differential harness
(§10) can prove equivalence exhaustively for 8- and 16-bit domains and by
random sampling for 32/64.

### 5.2 `llvm::APFloat` for floating-point constants

**This is the highest-value item in the plan, and it is a semantics item, not
a performance item.**

**Current.** `ConstantValue` holds a host `double`
(`include/gti/constant_evaluator.h:28`). `SemanticType::Float` is a single
kind (`include/gti/semantic_analyzer.h:197`); the grammar has one `float`
(`docs/language/grammar.ebnf:581`). Constant folding therefore computes with
whatever the compiler that built GTI does.

**The documented gap.** `docs/plans/compiler-roadmap-status.md`, Milestone 0,
"Still required": *"floating-point behavior for NaN, signed zero, contraction,
conversion, and supported rounding environment."*
`docs/language/execution.md` §4.3 repeats it. It is a stated 1.0 release
blocker.

**Why host `double` cannot close it.** `docs/architecture/optimization.md`
states the rule directly: optimization must not use "host-C++ behavior as a
proof." A folded float constant computed in host `double` is precisely that.
The compiler cannot even *express* a target float semantic different from its
own host, cannot control rounding mode, and cannot distinguish quiet from
signaling NaN payloads.

**What LLVM gives.** `APFloat` implements IEEE-754 for a chosen semantics
(`IEEEsingle`, `IEEEdouble`, `IEEEhalf`, `x87DoubleExtended`, and others)
independently of the host, with explicit `roundingMode` on every operation, an
`opStatus` result reporting inexact/overflow/underflow/invalid, correct
signed-zero and NaN handling, and exact decimal string conversion in both
directions.

`APFloat` does not *decide* GTI's float semantics — that remains a language
decision the specification must make. It makes the decision **expressible and
enforceable**, which is what unblocks the milestone.

**Confinement.** Replace `double` in the `ConstantValue` variant with a GTI
type (`ConstantFloat`) that holds an `APFloat` and its semantics tag. Under
Posture A that requires `constant_evaluator.h` to expose `ConstantFloat`
opaquely or move behind a compiled boundary; it is the one adopt-tier item
with real public-header pressure, and it is worth paying for.

**Risk: medium.** It forces the language to answer questions it has been
deferring. That is the point, but it means this item should be sequenced
*with* a specification decision, not ahead of one.

### 5.3 `llvm::Triple` and `TargetParser` for `TargetInfo`

**Current.** `TargetInfo` is three strings filled by preprocessor detection
(`include/gti/target.h:31`–`:58`). There is no parsing, no normalization, no
validation, and no derived fact. `CppEmitter` accepts a `TargetInfo` and never
reads it (audit §5.5). The prelude hardcodes `size_t = uint64_t`
(`stdlib/prelude.gti:121`).

**What LLVM gives.** `llvm::Triple` parses and *normalizes* an
`arch-vendor-os-environment` string, canonicalizes aliases
(`aarch64` / `arm64`, `darwin` / `macosx`), and answers derived questions:
`isArch64Bit()`, `getArchPointerBitWidth()`, `isOSDarwin()`,
`isLittleEndian()`, `isOSWindows()`. `llvm::TargetParser` adds CPU and feature
tables.

**Scope discipline.** Use `Triple` to **parse and normalize into** GTI's
`TargetInfo`. Do not put a `Triple` in `TargetInfo`, and do not let target
conditionals in GTI source become Triple queries — `#if` predicates are GTI
language surface owned by semantics, and their vocabulary is a GTI decision.

**What this unblocks.** With pointer width available, `size_t` and `ptrdiff_t`
can derive from the selected target instead of being fixed in the prelude, and
`gti build --target` gains a validated triple instead of an unchecked string.
That is audit §5.5's actionable change with the parsing already written.

**Note.** `Triple` moved from `LLVMSupport` to `LLVMTargetParser` in LLVM 17.
The pinned version determines which library to link. Verify against the pin.

**Risk: low.** `TargetInfo::host()` keeps its current preprocessor path as the
default; Triple parsing applies to explicitly requested targets first.

### 5.4 `llvm::FoldingSet` / `DenseMap` for HIR instance de-duplication

**Current.** `HirLowerer::enqueueClass` and `enqueueFunction`
(`include/gti/hir.h:528`, `:562`) linearly scan every instance discovered so
far, deep-comparing `std::vector<SemanticType>` per candidate. Audit §3.2
measured the result: 400 distinct generic instances take 8,073 ms in HIR
against 44 ms in semantics, growing ~4.5x per doubling.

**What LLVM gives.** `FoldingSet` exists for exactly this: a node provides a
`Profile(FoldingSetNodeID&)` method that hashes its structural identity, and
the set uniques on it. It is the mechanism clang uses for template
specialization lookup. `DenseMap` with a hand-written key is the simpler
alternative and may be sufficient.

**Why this one is attractive.** It is entirely inside `HirLowerer`. Nothing
about it touches a public type, a diagnostic, or a language rule. Once
`HirLowerer` moves to `src/compiler/hir.cpp` (migration plan), this is a
strictly private change with a measured, reproducible before/after.

**Caveat that generalizes.** `FoldingSet`, `DenseMap`, and `StringMap`
iteration order is bucket order, not insertion order, and for pointer keys it
depends on addresses. See §9.2 — this is the most likely way to break GTI's
determinism guarantees, and it applies to every container in this plan.

**Risk: low**, provided the de-dup structure is never iterated to produce
output. Instance *storage* stays the existing `std::vector`, which is what
assigns stable `HirClassInstanceId`s and what everything downstream iterates.
Only the *lookup index* becomes an LLVM container.

### 5.5 `llvm::TimeProfiler` for compile-time telemetry

**Current.** Nothing. Audit §3.1 had to build a bespoke harness linking
`libgti_compiler.a` to get per-phase timings.

**What LLVM gives.** `TimeProfiler` is clang's `-ftime-trace`: scoped
`TimeTraceScope` regions, hierarchical, emitted as Chrome Trace Format JSON
that opens in any browser trace viewer or Perfetto. Instrumenting
`Frontend::analyze`'s six phases plus `HirLowerer`'s per-instance work is a
few dozen lines.

**Fit with the existing plan.** `docs/plans/performance-tooling.md` asks for
"compiler-owned telemetry [that] explains compile time, optimization
decisions, emitted safety operations, and the native backend boundary." That
document also says the *benchmark harness* "must not add a mandatory benchmark
library, profiler, package-manager dependency, or network fetch." TimeProfiler
is compiler telemetry, not the benchmark harness, and it is not a network
fetch once LLVM is vendored — but the owners of that plan should confirm the
reading rather than have it assumed.

**Confinement.** `PRIVATE` to `gti_compiler` and the CLI. Emission gated on a
flag; zero cost when off.

**Risk: very low.** Additive, removable, no behavior change.

---

## 6. Prepare for: real value, blocked on prerequisite work

### 6.1 `GenericDomTree` + `GraphTraits` for MIR analysis

**This is the strongest "do not rewrite this" argument in the whole plan.**

`llvm/Support/GenericDomTree.h` and `GenericDomTreeConstruction.h` are
**templates over an arbitrary CFG**, reached through a `GraphTraits`
specialization. They have no dependency on LLVM IR. Writing
`GraphTraits<MirBody*>` — successors from `MirTerminator`, predecessors from a
precomputed map — makes available, for free:

- Semi-NCA dominator and post-dominator construction, with incremental update
  (`DomTreeUpdater`) so a transforming pass does not have to rebuild;
- `df_iterator`, `po_iterator`, `ReversePostOrderTraversal`, `scc_iterator`
  for the traversal orders every dataflow solver needs;
- `LoopInfoBase`, also templated, for natural loop identification.

Dominator construction is genuinely subtle, and an incorrect one produces
silently wrong optimizations rather than crashes. GTI currently has none —
`rebuildMirReachability` (`src/compiler/mir.cpp:325`) is a plain reachability
marking, and there is no post-order, RPO, or dominance anywhere in the tree.

**When.** The roadmap's Milestone 1 lists as still required: *"a general
fixed-point transfer authority for repeated loop headers and arbitrary CFG
joins, replacing the current bounded semantic snapshots."* That work needs RPO
and loop headers on day one. Write `GraphTraits<MirBody*>` when it starts, not
before.

**Note.** These headers are template-only, so this is one of the few items
that could in principle be used without linking `libLLVMSupport`. Do not rely
on that — see §9.5 on partial vendoring.

### 6.2 `BumpPtrAllocator` + `FoldingSet` + `StringSaver` for interning

Audit §4.3 identifies uninterned `SemanticType` as the root cause of the
compiler's memory and instantiation profile, and §4.7 identifies
`std::string`-keyed name lookup as a related cost. The canonical solution is
clang's: a `BumpPtrAllocator` owning all types, a `FoldingSet` uniquing them,
and a `UniqueStringSaver` interning identifiers into stable `StringRef`s.

`BumpPtrAllocator` is a well-tuned slab allocator with `SpecificBumpPtrAllocator`
for types needing destruction. `StringSaver` and `UniqueStringSaver` are
exactly the identifier-interning primitive.

**Why this is "prepare for" and not "adopt."** It is squarely Posture B:
`SemanticType` is in a public header, is copied by value across the entire
compiler, and appears in `ExpressionInfo`, `BindingInfo`, `HirValue`,
`MirInstruction`, and `Symbol`. Introducing an allocator-owned interned type
is the largest single representation change available and should follow, not
precede, the `TypeContext` design in audit §4.3.3.

The sequencing that works: design `TypeContext` as a GTI-owned interface with
`TypeId` handles first; implement it over `std::deque` + `std::unordered_map`;
prove the interface; *then* swap the implementation to
`BumpPtrAllocator` + `FoldingSet` as a private change. That order means the
LLVM decision is reversible and measurable rather than structural.

### 6.3 `BitVector` / `SparseBitVector` / `IndexedMap` for dataflow state

Audit §4.7 found roughly thirty sites that deep-copy the entire
`ScopeStack` — a `std::vector<std::unordered_map<std::string, Symbol>>` — at
every `if`, loop, switch, and short-circuit operator, and recommended dense
binding indices with flow state in a flat vector.

Once flow state is dense-indexed, `BitVector` (dense) and `SparseBitVector`
(sparse, for large sparse sets) are drop-in and are what LLVM's own dataflow
uses. `IndexedMap` covers the non-boolean case.

Small, mechanical, PRIVATE-able — but it is downstream of the dense-index
refactor, which is the actual work.

### 6.4 `raw_ostream` for the emitter and MIR printer

`CppEmitter` writes through a `std::ostringstream` (`include/gti/cpp_emitter.h:3764`)
across 468 `output <<` sites; `MirPrinter` does the same
(`src/compiler/mir_printer.cpp:475`). `llvm::raw_ostream` is meaningfully
faster — no locale machinery, no `sentry` construction per insertion, explicit
buffering.

Two reasons this is not adopt-tier now:

- 468 call sites is a large mechanical diff for a pure throughput win, and
  audit §3.6 shows native C++ compilation, not emission, dominates end-to-end
  build time. The payoff is small today.
- `raw_ostream`'s default float and pointer formatting differs from iostreams.
  Every numeric literal path (`include/gti/cpp_emitter.h:3673` builds literals
  through a nested `ostringstream`) must be checked against the emitted-C++
  snapshots.

Do it when `cpp_emitter` moves to `src/compiler/`, where the diff is
contained and the snapshots gate it.

### 6.5 `llvm/Support/Casting.h` — adopt if present, do not vendor for

Audit §4.6 found 409 `dynamic_cast` sites and recommended LLVM-style RTTI.
`Casting.h` provides `isa`, `cast`, `dyn_cast`, `dyn_cast_if_present`, and the
smart-pointer overloads, and is effectively header-only.

The honest accounting: **the cost of that migration is adding a `Kind` enum to
`Expr`/`Stmt` and a `classof` to every node — not writing the cast helpers.**
The helpers are a few hundred lines GTI could own outright, with the advantage
that they would appear in GTI's namespace and GTI's documentation rather than
requiring readers to know LLVM.

Verdict: if LLVM is vendored for §5, use `Casting.h` and save the effort. Do
not let this item justify vendoring LLVM on its own, and do not block the
`Kind`-tag work on the vendoring decision — that work is independently
valuable and can land against GTI-owned helpers first.

---

## 7. Decline, with reasons

These are the cases where adopting LLVM would work *against* GTI's goals. Each
is a worked example of the §4 rubric.

### 7.1 `llvm::ilist` for MIR instruction lists

Audit §5.4 flagged that `MirBlock::instructions` is a `std::vector<MirInstruction>`
(`include/gti/mir.h:242`), so insertion and erasure are O(block) and invalidate
references. LLVM's answer is `ilist` — the intrusive list `BasicBlock` uses.

**Decline, on rubric rule 2.** `ilist` presupposes LLVM's model: heap-allocated
instruction nodes with intrusive links, identity by address, and an SSA value
graph threaded through use-lists. GTI's MIR is deliberately none of those — it
is non-SSA, place-based, with body-local integer IDs, and
`docs/architecture/mir.md` requires the printer to remain "deterministic and
address-free." Adopting `ilist` would make `MirInstruction` address-identified,
which is in direct tension with that requirement.

**The better model is rustc's, and it needs no dependency.** rustc's MIR is
also non-SSA and place-based; it keeps statements in vectors and provides an
explicit `Location { block, statement_index }` addressing scheme plus a
`MirPatch` accumulate-then-apply protocol. That is audit §5.4.1's
recommendation, it preserves determinism, and it is perhaps two hundred lines
of GTI code.

This is the clearest case in the plan where the LLVM tool is battle-tested for
a problem GTI does not have.

### 7.2 `llvm::SourceMgr`

Audit §4.4 found `SourceManager::locate()` scanning from byte 0 per diagnostic,
producing a measured 11x penalty on the error path. `llvm::SourceMgr` has a
cached line-number lookup and would fix it.

**Decline, on rubric rule 2.** `SourceMgr` models a flat set of memory
buffers. GTI's `SourceManager` is paired with `SourceGraph`, which owns
source-unit identity, dependency edges, per-unit visibility, and the
load-once include semantics recorded in ADR 002. `SourceMgr` has no concept of
any of that, and its diagnostic rendering is LLVM-shaped rather than GTI's.

The fix audit §4.4 recommends is a `lineStarts` table and a binary search. The
project has already written that code — `SourcePositionIndex` in
`src/lsp/main.cpp:160`. Moving it down into `SourceManager` is strictly less
work than adopting `SourceMgr` and keeps GTI's source model intact.

### 7.3 `llvm::Error` / `llvm::Expected` as an error model

**Decline, on rubric rules 1 and 3.** GTI's `Diagnostic`
(`include/gti/diagnostic.h:50`) carries a stable code, phase, severity, primary
span, related spans, fix-its, and hints. It is a *better* error model for a
compiler than `llvm::Error`, which is designed for tool-level failures.
Introducing `Expected<T>` would create a second error channel alongside the
diagnostic list, and ADR 001 assigns semantic authority — including error
reporting — to the frontend.

`llvm::Error`'s must-be-checked discipline is genuinely valuable, but it is
valuable for *infrastructure* failures. If it is used anywhere, restrict it to
`src/driver/` process and filesystem paths, and never let it reach a GTI
language diagnostic.

### 7.4 `llvm::cl` for command-line parsing

**Decline, on rubric rule 5.** `src/cli/main.cpp` owns a frozen argument
contract exercised by `cli_workflow` and `project_cli_workflow`, including
exact help text, `--` passthrough of native compiler argv, and exit-status
policy. `cl::opt` would change help formatting, response-file handling, and
error text for no benefit, and it installs global state that a library
consumer of `gti_compiler` should never inherit.

### 7.5 TableGen for the diagnostic table

Audit §4.8 recommends a diagnostic table with per-code severity and groups.
clang generates that from `.td` files via `llvm-tblgen`.

**Decline, on rubric rule 5 and pragmatics.** TableGen is a separate
build-time executable, a separate language to learn, and a build-graph
dependency for every developer. The value GTI needs — one entry per
diagnostic, with a code, default severity, group, and format string — is a
table. A YAML or TOML file plus a ~100-line Python generator (the repository
already requires Python for `format`, `release_version_policy`, and three
smoke tests) delivers it with no new toolchain.

Revisit only if GTI later needs the other things TableGen is good at
(instruction descriptions, target tables), which is a codegen concern this
plan excludes.

---

## 8. Evaluate later

Not recommended now, not ruled out. Each should be reconsidered when its
prerequisite appears.

**`llvm::json` for the LSP.** `src/lsp/main.cpp` has 368 `json_object`
references against json-c's manual reference counting. `llvm::json::Value` is
substantially safer and has correct Unicode handling. But it is a
368-site mechanical change with zero language benefit and real regression risk
in the one component whose failure is most user-visible. Revisit only if the
LSP is being restructured anyway — for example when document/scheduling state
is extracted from `LanguageServer`, which `docs/architecture/lsp.md` already
lists as a current limit.

**`llvm::vfs::FileSystem`.** `SourceLoader::load` threads an
`unordered_map<string,string> sourceOverrides` through the whole loader
(`include/gti/source_loader.h:29`, `:167`) to model unsaved editor buffers.
`vfs::InMemoryFileSystem` over `vfs::OverlayFileSystem` is exactly that
problem, solved, and it would additionally make the loader testable without
touching the disk. Genuine merit. Blocked because `SourceLoader` is entirely
header-defined and uses `std::filesystem` throughout; revisit when the loader
migrates to `src/compiler/`.

**`lit` + `FileCheck` for snapshot tests.** GTI's `--emit-cpp` output and
`MirPrinter` output are exactly what FileCheck was built to check, and today
they are asserted with substring searches in hand-written C++ and Python.
FileCheck's `CHECK-NEXT` / `CHECK-SAME` / variable capture would make those
tests both stronger and more readable. The cost is a `lit` harness alongside
CTest. Worth a focused proposal for the two snapshot families specifically —
not a wholesale test migration.

---

## 9. Risk register

These are the failure modes that would make this plan a regression rather than
an improvement. Each has a concrete mitigation and each should be a gate.

### 9.1 License posture (decide before any code)

LLVM is **Apache-2.0 WITH LLVM-exception**. GTI is MIT (`LICENSE`).

Combination is permitted — Apache-2.0 is permissive and the LLVM exception
specifically removes the copyleft-style obligations on compiled output. But
Apache-2.0 imposes obligations MIT does not: retention of the license and any
NOTICE, a statement of modification if LLVM sources are changed, and an
express patent grant with a termination clause on patent litigation.

Practically: install `llvm-LICENSE.txt` into `share/licenses/gti` alongside
the existing entries (`CMakeLists.txt:219`–`:235`), and do not modify vendored
LLVM sources.

**This is a project-ownership decision, not a technical one.** It should be
settled in the ADR before phase 1, not discovered during release packaging.

### 9.2 Determinism — the most likely way to break GTI

`docs/architecture/mir.md` requires `MirPrinter` to be "deterministic and
address-free." `docs/architecture/diagnostics.md` and the CLI/LSP tests depend
on stable diagnostic ordering. `gti metadata` promises "deterministic
schema-versioned JSON."

`DenseMap`, `StringMap`, `DenseSet`, and `FoldingSet` **do not iterate in
insertion order**, and for pointer keys their order depends on addresses,
which vary run to run under ASLR. GTI already keys 26 side tables on
`const Expr*` (audit §4.5) — precisely the dangerous shape.

**Rule, to be written into the architecture docs and enforced in review:**

> An LLVM hash container may be used as a *lookup index*. It must never be
> iterated to produce diagnostics, printed IR, emitted C++, metadata, or any
> other observable output. Where iteration order is observable, use
> `llvm::MapVector` / `llvm::SetVector`, or keep the ordered `std::vector`
> that assigns IDs and use the hash container only for lookup.

The §5.4 design already follows this: instances stay in a `std::vector`;
only the lookup becomes a `FoldingSet`.

**Gate:** a test that runs the same input twice in separate processes and
asserts byte-identical `--emit-cpp` and MIR-print output. This is cheap and
would catch the entire class.

### 9.3 Error model collision — a crash risk specific to `gti_lsp`

`gti_lsp` currently survives internal failures: there is a top-level
`try` / `catch (const std::exception&)` / `catch (...)` around request dispatch
(`src/lsp/main.cpp:1306`–`:1317`) and around worker analysis (`:2389`, `:2472`).
Its resilience strategy is "catch everything and keep serving."

LLVM Support **bypasses that entirely**. `llvm::report_fatal_error` and
`llvm::report_bad_alloc_error` call the installed handler and then `abort()`.
`SmallVector` growth failure routes through the latter. An
assertions-enabled LLVM build additionally aborts on API misuse.

Introducing LLVM into the analysis path without addressing this converts
today's recoverable condition into a hard language-server crash — a clear
regression.

**Mitigation, required before phase 1 ships in the LSP:**

- Install `llvm::install_fatal_error_handler` and
  `llvm::install_bad_alloc_error_handler` in every entry point (`gti`,
  `gti_lsp`, each test binary) before any LLVM call.
- Wrap LSP analysis in `llvm::CrashRecoveryContext`, which LLVM built for this
  purpose and clangd uses for the same reason.
- Build release LLVM with `LLVM_ENABLE_ASSERTIONS=OFF`; enable assertions only
  in developer builds, and know that they abort.

There is a bonus in the other direction: `llvm::sys::PrintStackTraceOnErrorSignal`
and `llvm::PrettyStackTraceProgram` would give GTI crash reports it does not
have today.

### 9.4 Exceptions through LLVM frames

`include/gti/parser.h` throws `ParseError` at **54 sites** and catches at
**10** recovery boundaries. LLVM is conventionally built with
`LLVM_ENABLE_EH=OFF` (`-fno-exceptions`). Throwing an exception through an
LLVM stack frame is undefined behavior.

**Rule:** no GTI callback that can throw may be passed into an LLVM API —
no throwing comparator to `llvm::sort`, no throwing `Profile()` on a
`FoldingSet` node, no throwing predicate to an LLVM algorithm.

Today the parser is the only thrower and it does not use LLVM, so this is
manageable. It should still be a written rule, and it is one more argument for
eventually moving parser recovery off exceptions onto explicit result types —
which audit §6.3's `RecoveryExpr` recommendation would largely accomplish
anyway.

### 9.5 Vendoring cost, reproducibility, and version pinning

LLVM has **no stable C++ ABI across versions** and no support for partial
source vendoring. Three options, evaluated against the existing json-c
precedent:

| Option | Pros | Cons |
| --- | --- | --- |
| Check LLVM headers into `vendor/` | Matches `expected_lite` precedent; no fetch | Thousands of files; unsupported by LLVM; `SmallVector`/`APInt`/`APFloat` need compiled sources anyway; drifts silently |
| `find_package(LLVM CONFIG)` | Fast; no build cost; distro-friendly | Version drift across developer machines; not self-contained for `GTI_RELEASE_BUILD` |
| `FetchContent` on a pinned release tarball | Exactly mirrors `GTI_BUNDLE_JSON_C`; reproducible; self-contained | Large download; multi-minute first build |

**Recommendation: mirror the json-c pattern exactly.** `find_package(LLVM CONFIG)`
with a pinned supported version range as the default, and a
`GTI_BUNDLE_LLVM` option using `FetchContent` for `GTI_RELEASE_BUILD` and CI.
`GTI_RELEASE_BUILD` already forces `GTI_BUNDLE_JSON_C` (`CMakeLists.txt:45`–`:47`);
it should force `GTI_BUNDLE_LLVM` the same way.

Configure the bundled build for the minimum surface and for reproducibility:

```
LLVM_ENABLE_PROJECTS=""      LLVM_TARGETS_TO_BUILD=""
LLVM_ENABLE_ZLIB=OFF         LLVM_ENABLE_ZSTD=OFF
LLVM_ENABLE_TERMINFO=OFF     LLVM_ENABLE_LIBXML2=OFF
LLVM_ENABLE_LIBEDIT=OFF      LLVM_INCLUDE_TESTS=OFF
LLVM_INCLUDE_BENCHMARKS=OFF  LLVM_INCLUDE_EXAMPLES=OFF
LLVM_ENABLE_ASSERTIONS=OFF   (release)
```

Link only `LLVMSupport`, `LLVMDemangle`, and `LLVMTargetParser`. Assert that
set in CI — a link against anything under `llvm/IR` or `llvm/CodeGen` should
fail the build, since that is the boundary this whole plan rests on.

**Also record:** LLVM's build requires CMake and a host compiler newer than
GTI's current floor; the pin determines both. `README.md`'s stated build
requirements need updating in the same change.

### 9.6 RTTI flag propagation

Stock LLVM is built with `LLVM_ENABLE_RTTI=OFF`. That is compatible with GTI's
409 `dynamic_cast` sites, because RTTI is emitted per class by whichever
compiler compiles that class — GTI's own types keep their type info.

Two rules:

- Never `dynamic_cast` or `typeid` an LLVM type, and never derive a GTI class
  from a polymorphic LLVM class that will be cast.
- Ensure LLVM's exported CMake flags do not propagate `-fno-rtti` onto GTI
  targets. Some LLVM installations export it through `LLVM_DEFINITIONS` /
  `LLVM_CXXFLAGS`. Apply LLVM include directories as `SYSTEM PRIVATE`
  (matching the existing tomlplusplus treatment at `CMakeLists.txt:104`) and
  do not consume its flag variables wholesale.

### 9.7 Build time and developer friction

The current full build is 50 s wall / 232 s CPU. A bundled LLVM Support build
adds a multi-minute one-time cost, and CI caching becomes load-bearing.

Mitigation: default to `find_package`, bundle only for release and CI, cache
the bundled build in CI, and document the system-LLVM path as the ordinary
developer setup.

---

## 10. Phased implementation plan

Each phase is independently landable, independently revertible, and gated. No
phase begins before its gate passes.

### Phase 0 — Decision and skeleton (no compiler code changes)

1. ADR recording: the license posture (§9.1), Posture A vs B (§3), the
   adoption rubric (§4), the determinism rule (§9.2), and the exception rule
   (§9.4).
2. `find_package(LLVM CONFIG)` with a pinned supported range, plus
   `GTI_BUNDLE_LLVM` FetchContent mirroring `GTI_BUNDLE_JSON_C`;
   `GTI_RELEASE_BUILD` forces it on.
3. Link-surface assertion in CI: only `LLVMSupport`, `LLVMDemangle`,
   `LLVMTargetParser` may be linked.
4. Fatal-error and bad-alloc handlers installed in `gti`, `gti_lsp`, and every
   test binary (§9.3). This lands *before* any LLVM use, not alongside it.
5. `llvm-LICENSE.txt` installed into `share/licenses/gti`.
6. A determinism test: same input, two processes, byte-identical `--emit-cpp`
   and MIR-print output (§9.2).

**Gate to phase 1:** the full suite passes with LLVM linked and zero LLVM
symbols used. This proves the build and packaging story before any behavior
depends on it.

### Phase 1 — `TimeProfiler` (§5.5)

Purely additive telemetry, flag-gated. Chosen first deliberately: it exercises
the vendoring, the handlers, and the CI link assertion under zero behavioral
risk, and it produces the measurements every later phase needs.

**Verify:** `cli_workflow` unchanged with the flag off; a trace file is
produced and parses as valid Chrome Trace Format with the flag on.

### Phase 2 — `Triple` for target parsing (§5.3)

`TargetInfo` keeps its definition. `TargetInfo::host()` keeps its preprocessor
path. Add a `parseTargetTriple(std::string_view) -> std::optional<TargetInfo>`
in a compiled source; route explicit target selection through it.

**Verify:** `project_model` and `driver_pipeline`. Add cases for alias
normalization (`arm64` / `aarch64`) and for rejection of a malformed triple
with a GTI diagnostic — not an LLVM error.

**Follow-on, separate change:** derive `size_t`/`ptrdiff_t` from the parsed
target (audit §5.5). This is a language-visible change and deserves its own
review.

### Phase 3 — `FoldingSet` for HIR instance de-duplication (§5.4)

**Prerequisite:** `HirLowerer` moves to `src/compiler/hir.cpp`
(compiler-library-migration). This phase is the concrete justification for
that migration step.

Instance storage stays in `std::vector`. Only the lookup index changes.

**Verify:** `compiler_pipeline`; the determinism test from phase 0; and the
audit §3.2 / §3.3 benchmarks re-run. Success criterion is explicit: HIR
lowering time must stop growing superlinearly with instance count, and must
stop growing at all with unrelated base-program size.

This is the phase with the largest measurable payoff and the smallest semantic
surface. If the plan delivers only phases 0–3, it has already been worth it.

### Phase 4 — `APInt` for checked integers (§5.1)

**Prerequisite:** `checked_integer` and `constant_evaluator` operations move to
`src/compiler/`.

Public types unchanged. Implementation swapped behind them.

**Verify:** a differential harness comparing old and new implementations —
exhaustive over all 8- and 16-bit domain/operation/operand combinations,
randomized over 32- and 64-bit. This is the probation mechanism (§11) made
concrete.

### Phase 5 — `APFloat` and the float semantics decision (§5.2)

**Prerequisite:** a language decision on GTI's float semantics, recorded in
`docs/language/execution.md` §4.3. `APFloat` implements the decision; it does
not make it.

Replace `double` in `ConstantValue` with a GTI `ConstantFloat`.

**Verify:** new `compiler_pipeline` cases for NaN, signed zero, overflow to
infinity, inexact conversion, and float-to-integer truncation at the boundary.
This phase should close a documented Milestone 0 item, and the roadmap ledger
should be updated in the same change.

### Phase 6 (conditional) — `GraphTraits` + dominance (§6.1)

Begins only when the MIR dataflow framework begins. Write
`GraphTraits<MirBody*>` first; take `ReversePostOrderTraversal` and
`DominatorTreeBase` from it.

### Phase 7 (separate ADR) — Posture B

Only after phases 0–5 have soaked, and only as an explicit decision to install
LLVM headers with `include/gti`. Unlocks §6.2 type interning, which is audit
§4.3's endpoint.

---

## 11. How "future development decides" is made concrete

The request asks that later work be able to weigh LLVM against a GTI
implementation. Two mechanisms, and a warning.

**Mechanism 1 — the rubric (§4).** Applied to a specific component, with the
answer recorded. §7 contains five worked examples so the rubric has
precedent to reason from rather than being an abstract checklist.

**Mechanism 2 — probation with a deadline.** A swap may be validated against
the previous implementation during development, but the released compiler has
one active implementation. After selection, displaced code may be retained
temporarily under `archive/` as non-built reference material, then deleted when
its review and rollback value expires.

The deadline is not optional. Permanently maintaining two implementations of
checked integer arithmetic is worse than either choice alone: it doubles the
test matrix, and the unused path silently rots. Archival is not support,
selection, or continued maintenance. Probation is a decision procedure with
an expiry, not a hedge.

**The warning.** The interface that makes a swap reversible is the same
interface that makes the code good regardless — `TypeContext`, a
`parseTargetTriple` function, a de-dup index behind `enqueueClass`. Build the
interface because it is right, and reversibility comes for free. Do not build
abstraction layers whose only purpose is to keep LLVM optional; that is the
speculative complexity the repository's own review skill warns against.

---

## 12. Relationship to the architectural audit

| audit finding | This plan |
| --- | --- |
| §4.2 HIR instance de-dup is O(n^2) | Phase 3, `FoldingSet` — direct fix |
| §4.3 `SemanticType` has no interning | Phase 7 / §6.2 — `BumpPtrAllocator` + `FoldingSet`, but design `TypeContext` first |
| §4.4 `SourceManager::locate()` is linear | **Not** an LLVM item (§7.2). Move `SourcePositionIndex` down from the LSP |
| §4.5 26 AST-pointer side tables | Partially §6.3, but the dense-`NodeId` design is GTI's own work |
| §4.6 409 `dynamic_cast` sites | §6.5 — use `Casting.h` if LLVM is present; the `Kind` tags are the real work and are not blocked on this plan |
| §4.7 whole scope stack copied per branch | §6.3 `BitVector` after dense indices land |
| §4.8 ad-hoc diagnostic codes | **Not** an LLVM item (§7.5). Python generator over a data file |
| §5.1 counter-based symbol names | Not addressed here; content-based mangling is GTI's own |
| §5.4 MIR not shaped for passes | §6.1 dominance/traversal — yes. §7.1 `ilist` — explicitly no |
| §5.5 `TargetInfo` has no layout | Phase 2, `Triple` — direct fix |
| §6.1 LSP lowers unused HIR/MIR | Not an LLVM item; Phase 1 telemetry makes it visible |
| §7.1 evaluation order left to host C++ | Not an LLVM item |
| §7.2 checked failures abort without location | Not addressed here, but §9.3's handler work is adjacent |

Roughly half the audit's findings are **not** LLVM problems. That is the
expected and correct ratio: LLVM supplies infrastructure, and most of what the
audit found is GTI's own representation and layering. Adopting LLVM does not
substitute for that work, and this plan should not be read as an alternative
to it.

---

## 13. Assessment

Vendoring LLVM's support libraries is a sound move for GTI, but for a narrower
reason than "LLVM has good tools." The reason is that GTI has three components
— checked integers, float constants, target description — that are currently
*smaller than the problems they name*, in ways the roadmap already flags as
release blockers, and where the correct implementations are language-neutral
and fully specified elsewhere. Writing those from scratch would be months of
work to arrive at a worse `APFloat`.

Against that, the honest counterweights: the biggest performance findings in
the companion audit (type interning, dense node identity, instance-analysis
copying) are **architecture problems that LLVM containers do not solve**. A
`DenseMap` in place of an `unordered_map` does not fix copying the whole
semantic model per generic instance. If LLVM adoption becomes a substitute for
that work rather than a complement to it, this plan will have made things
worse while appearing to make them better.

The sequencing above is designed to make that failure mode hard: phase 3 is
gated on a measured benchmark that only an architectural fix can pass, and
§12 states plainly which findings LLVM does not address.

The two things to get right before writing any code are the license posture
(§9.1) and the fatal-error handlers (§9.3). The first is a decision that is
expensive to reverse after release packaging; the second is the difference
between a language server that recovers from bad input and one that crashes on
it.
