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
- [Optimization](architecture/optimization.md) — HIR constant analysis,
  controlled MIR editing, and source-to-optimized coherence.
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
- [Verification](architecture/verification.md) — test ownership, broad checks,
  and the optional local language audit.

## Language

- [Language index](language/index.md) — status, authority, reading order, and
  specification maintenance.
- [Grammar](language/grammar.ebnf) — implemented source grammar with focused
  semantic notes.
- [Static semantics](language/static-semantics.md) and
  [execution semantics](language/execution.md) — high-level current rules and
  explicit specification gaps.
- [Ownership and lifetimes](language/ownership-and-lifetimes.md),
  [raw pointers](language/raw-pointers.md), and
  [native C interoperation](language/native-c-interop.md) — ownership, unsafe,
  ABI, and lifetime contracts.
- [Concepts](language/concepts.md), [ranges](language/ranges.md), and
  [expected values](language/expected.md) — focused feature contracts.
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

Decision records explain why a significant rule exists. They are not a second
description of the implementation.

## Plans

- [Dependency-ordered implementation sequence](plans/implementation-sequence.md)
  — the sole prompt-sized work queue, prerequisites, status, and exit gates
- [Concurrency and memory-model proposal](plans/concurrency-memory-model.md)
  — D-MEM-01 design evidence superseded normatively by accepted ADR 008
- [Callable ownership and escape contract](plans/callable-ownership-and-escape.md)
  — the completed D-CALL-01 identity/capability contract, implemented local
  move-capture environment, and remaining owned-escape boundary
- [Place identity and ownership-state authority](plans/place-and-ownership-state.md)
  — the completed M-OWN-01 key, relation, phase-ownership, and invalidation
  contract that precedes indexed-place implementation
- [Systems-readiness roadmap](plans/roadmap-to-1.0.md) and
  [current roadmap checkpoint](plans/compiler-roadmap-status.md) — capability
  outcomes and the soft, revisable path to a full-featured 1.0.
- [Compiler library migration](plans/compiler-library-migration.md)
- [Optimization architecture](plans/optimization.md)
- [Build and package system](plans/build-system.md)
- [Iterator and range completion](plans/iterators-and-ranges.md)
- [Mutable field groups](plans/mutable-field-groups.md) — records the bounded
  `mut { ... }` shorthand for consecutive mutable class and struct fields while
  preserving immutable defaults and existing receiver/write rules
- [Static assertions](plans/static-assertions.md) — records the implemented
  frontend-owned exact-`bool` `static_assert`, including per-concrete-instance
  generic evaluation and erasure before HIR
- [Mutable temporary receivers](plans/mutable-temporary-receivers.md) — records
  the implemented bounded ephemeral receiver capability for fresh,
  self-contained class-value temporaries without making temporaries general
  mutable places
- [LSP evolution](plans/lsp-evolution.md)
- [C interop target surface](plans/c-interop-target-surface.md) — the
  completed 124-function GLFW 3.4 acceptance surface for `S-FFI-02`
  F1/F2/F3 and the same-thread `S-CALL-01` callback boundary
- [Performance tooling](plans/performance-tooling.md)
- [Conditional compilation and configuration flags](plans/conditional-compilation.md)
  — implemented `#define`/`#ifdef` flag axis over target conditionals, its
  design rationale, and LSP inactive-region rendering
- [Language restriction ledger](plans/language-alignment.md) — current
  restrictions classified by reason, readiness role, owner, and evidence

Everything under `docs/plans/` is future or incomplete unless a section
explicitly identifies a verified current baseline. Implemented behavior must be
documented in `docs/architecture/` or `docs/language/` as it lands.

## Maintenance

When behavior changes, update the existing owning document, its tests, and any
affected plan status. Create an ADR only for rationale worth preserving. Remove
or merge superseded proposals instead of keeping `v2`, `final`, or
`implementation-notes` variants.
