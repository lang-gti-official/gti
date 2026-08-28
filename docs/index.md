# GTI Documentation Map

Canonical GTI knowledge lives here. Source code and tests remain authoritative
for the current implementation when a document is stale or ambiguous.

## Compiler Architecture

- [Overview](architecture/overview.md) — pipeline, ownership boundaries, and
  current transition points.
- [Frontend](architecture/frontend.md) — source graph, lexing, parsing, AST
  ownership, recovery, and `FrontendResult` lifecycle.
- [Semantic analysis](architecture/semantic-analysis.md) — name/type
  resolution, semantic side tables, symbols, tooling records, and concrete
  generic reanalysis.
- [HIR](architecture/hir.md) — concrete instance discovery and executable typed
  values.
- [MIR](architecture/mir.md) — control-flow graphs, values, places, ownership
  effects, cleanup, and verification.
- [Optimization](architecture/optimization.md) — verified local MIR passes,
  reusable dataflow/SCCP, bounded loop and alias facts, stable proof replay,
  per-instance effects, and optimizer observability.
- [Backend](architecture/backend.md) — the verified-MIR body contract, sealed
  C++ representation planning, and native compilation handoff.
- [Diagnostics](architecture/diagnostics.md) — structured diagnostics, spans,
  fix-its, recovery, and test expectations.
- [Standard library and runtime](architecture/standard-library-and-runtime.md)
  — ordinary GTI policy, compiler-private capabilities, C ABI, and host code.
- [Build and driver](architecture/build-and-driver.md) — direct/project
  orchestration, workspaces/local package graphs, verified whole-program
  caching, and the
  `gti_compiler`/`gti_driver` boundary.
- [LSP](architecture/lsp.md) — immutable frontend snapshots, compiler-owned
  queries, document state, and protocol separation.
- [Formatting](architecture/formatting.md) — current formatter contract and
  syntax-tooling boundary.
- [Reference documentation](architecture/reference-documentation.md) —
  compiler-owned API projection, authored MDX, page-owned examples, and static
  site validation.
- [Verification](architecture/verification.md) — test ownership, broad checks,
  and the optional local language audit.

## Language

- [Language index](language/index.md) — status, authority, reading order, and
  specification maintenance.
- [Scope and conformance](language/scope-and-conformance.md),
  [lexical and syntax rules](language/lexical-and-syntax.md), and
  [programs and targets](language/programs-and-targets.md) — source contract,
  compilation units, entry points, target selection, and configuration.
- [Grammar](language/grammar.ebnf) — implemented source grammar with focused
  semantic notes.
- [Static semantics](language/static-semantics.md) and
  [execution semantics](language/execution.md) — high-level current rules and
  explicit specification gaps.
- [Ownership and lifetimes](language/ownership-and-lifetimes.md),
  [raw pointers](language/raw-pointers.md), and
  [native C interoperation](language/native-c-interop.md) — ownership, unsafe,
  ABI, and lifetime contracts.
- [Concepts](language/concepts.md), [ranges](language/ranges.md),
  [expected values](language/expected.md), and
  [concurrency](language/concurrency.md) — focused feature contracts.
- [I/O](language/io.md), [TCP](language/tcp.md), and
  [standard library](language/standard-library.md) — current library-facing
  semantics and boundaries.

## Decisions

- [001: Frontend semantic authority](decisions/001-frontend-semantic-authority.md)
- [002: Source units and includes](decisions/002-source-units-and-includes.md)
- [003: Exact overload resolution](decisions/003-exact-overload-resolution.md)
- [004: Standard-library/runtime boundary](decisions/004-standard-library-runtime-boundary.md)
- [005: LSP compiler-semantic authority](decisions/005-lsp-compiler-semantics.md)
- [006: LLVM support-library adoption](decisions/006-llvm-support-adoption.md)
- [007: Defined runtime failure](decisions/007-defined-runtime-failure.md)
- [008: Safe concurrency memory model](decisions/008-safe-concurrency-memory-model.md)
- [009: Bounded `requires` contracts](decisions/009-bounded-requires-contracts.md)
- [010: Deterministic evaluation and full expressions](decisions/010-deterministic-evaluation-and-full-expressions.md)
- [011: Language compatibility and editions](decisions/011-language-compatibility-and-editions.md)
- [012: Outcome-first systems readiness](decisions/012-outcome-first-systems-readiness.md)
  — prioritizes coherent user capabilities and defines 1.0 as a soft,
  revisable full-systems-readiness goal rather than a scheduling cutoff.
- [013: Bounded native C records](decisions/013-bounded-native-c-records.md)
  — defines the passive `[[c_abi]]` record family, compiler-owned layout,
  by-value/pointer passage, ownership exclusions, and the follow-on
  pointer-only `[[c_opaque]]` identity used by C/C++ adapters.
- [014: Native unions and payload enums](decisions/014-native-unions-and-payload-enums.md)
  — separates unsafe passive overlapping storage from safe closed tagged sums
  and defines the bounded first payload family.
- [015: Contextual fixed-array arguments](decisions/015-contextual-fixed-array-arguments.md)
  — gives call-site braces an exact generic `T[N]` meaning without importing
  `std::initializer_list`, list-preferred overloads, common-type inference, or
  CTAD.
- [016: Single lowered backend input](decisions/016-single-lowered-backend-input.md)
- [017: Per-body defined-failure boundary](decisions/017-per-body-defined-failure-boundary.md)
- [018: Loan erasure representation](decisions/018-loan-erasure-representation.md)
  — converges executable C++ generation on one immutable lowered-program
  input (verified MIR bodies plus deterministic representation tables) and
  ends per-body AST/HIR consultation at the backend boundary.
- [019: Generic-owner transformed-member boundary](decisions/019-generic-owner-transformed-members.md)
- [020: Exact qualified class specializations](decisions/020-exact-qualified-class-specializations.md)
  — defines exact source syntax, canonical semantic selection, package
  coherence, and backend/tooling preservation without partial specialization.
- [021: libclang C import adoption](decisions/021-libclang-c-import.md)
  — makes libclang the mandatory private parser behind `gti bindgen`'s
  reader seam, bounds the Clang link surface to exactly that library, and
  defines its acquisition, packaging, and recovery rules.
- [022: Safe typed allocation and storage provenance](decisions/022-safe-typed-allocation.md)
  — adopts the direct `std::allocator`/`std::storage<T>` boundary, typed
  provenance and slot lifetimes, recoverable allocation, alignment and
  zero-size rules, and the arena/pool composition boundary.

Decision records explain why a significant rule exists. They are not a second
description of the implementation.

## Plans

The active plans below describe incomplete work or maintain cross-cutting
delivery state. Completed delivery records are retained in the archive rather
than mixed into the active queue.

- [Dependency-ordered implementation sequence](plans/implementation-sequence.md)
  — the sole prompt-sized work queue, prerequisites, status, and exit gates;
  retains the completed runtime-performance campaign baseline and owns the
  resumed queue
- [Systems-readiness roadmap](plans/roadmap-to-1.0.md) — capability outcomes
  and the soft, revisable path to a full-featured 1.0
- [Language restriction ledger](plans/language-alignment.md) — current
  restrictions classified by reason, readiness role, owner, and evidence
- [Concurrency and memory-model proposal](plans/concurrency-memory-model.md)
  — remaining implementation work under the policy accepted by ADR 008
- [Callable ownership and escape contract](plans/callable-ownership-and-escape.md)
  — implemented local move capture and the remaining owned-escape boundary
- [Place identity and ownership-state authority](plans/place-and-ownership-state.md)
  — implemented identity/state foundations and remaining aggregate, failure,
  and provenance work
- [Optimization architecture](plans/optimization.md)
- [Build and package system](plans/build-system.md)
- [Iterator and range completion](plans/iterators-and-ranges.md)
- [Native bindings import](plans/native-bindings-import.md) — the
  C-first `gti bindgen` plan: current bounded C17/LP64/LLP64 import, its
  scalable libclang migration, and the later C++/build-integration gates
- [LSP evolution](plans/lsp-evolution.md)
- [Performance tooling](plans/performance-tooling.md)
- [Archived plan records](plans/archive/README.md) — completed delivery plans,
  migrations, and historical checkpoints retained without presenting them as
  active work

Active plans are proposals or work ledgers, not evidence that behavior exists.
Implemented behavior must be documented in `docs/architecture/` or
`docs/language/` as it lands.

## Historical Audits

- [Historical third-party audits](third-party-audit/README.md) — a preserved
  2026-08-22 evidence snapshot from before the current LLVM support-library
  boundary; intentionally non-canonical for current implementation status

## Maintenance

When behavior changes, update the existing owning document, its tests, and any
affected plan status. Create an ADR only for rationale worth preserving. Remove
or merge superseded proposals instead of keeping `v2`, `final`, or
`implementation-notes` variants.
