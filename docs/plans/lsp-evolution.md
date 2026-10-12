# LSP Evolution Plan

Status: Non-canonical future work. Current behavior is documented in
[`docs/architecture/lsp.md`](../architecture/lsp.md).

Operational prerequisites and the current tooling queue are maintained in
[`implementation-sequence.md`](implementation-sequence.md), including the
client-gated status of project indexing and process-isolated analysis.

GTI already retains immutable compiler snapshots, compiler-owned symbol
occurrences, semantic tokens, hover, completion, definition, references,
document highlights, document symbols, fail-closed function-local rename,
diagnostics, and diagnostic quick fixes. The next work should extend that
shared model instead of building feature-specific semantic systems.

## Near-Term Priorities

### General missing-include actions — implemented

Project, direct-dependency and standard-library declarations now share compiler
discovery and speculative frontend validation. The fixed standard-type header
table is removed. Current scope, snapshot invalidation and conservative limits
are recorded in the canonical LSP architecture document. This does not provide
workspace-wide references/rename or a persistent project index.

### 0. Isolate protocol publication — implemented; shared-cache fault safety remains

`runIsolatedAnalysis` now guards only analysis and snapshot construction. It
catches all C++ exceptions inside the callback and returns them as
`GuardedAnalysis` state, so none crosses an LLVM frame. `stateMutex` is not held
by that guarded callback, so a stack restore there cannot strand the protocol
publication lock.
`publishAnalysis` performs generation checks, diagnostic publication, request
rejection, and protocol-state mutation outside the crash-recovery context.
`lsp_protocol` covers ordinary failed-input recovery with
`test_worker_survives_failed_analysis`, which drives repeated failing analyses
through the worker and then requires ordinary analysis and a semantic request
to still succeed.

The long-lived frontend workers also reserve 8 MiB stacks through
`llvm::thread`. This lets the compiler's 64-level parser budget publish
`GTI-P0005` before Darwin's smaller default secondary-thread stack is
exhausted, including completion's independent frontend analysis. The protocol
regression then edits and formats the same document, proving that the
analysis and completion workers and shared state remain usable.

The 2026-10-11 source audit found that guarded analysis still calls the shared
frontend cache, whose lookup/store operations own a mutex. Recovery from a
signal during those operations can skip unlocking or leave an incomplete cache
update. The existing missing-include and depth-limit tests do not establish
signal-fault safety. The current boundary is best-effort, not generally
state-safe, and in-process recovery also cannot undo heap damage.

The next recovery slice must choose a lock/state-safe cache seam or worker
process isolation before promising continued operation after a compiler fault.
Acceptance needs isolated fault-injection cases at cache access/update and
ordinary analysis, proving no stranded worker/lock, no partial snapshot
publication, stale-request rejection and subsequent healthy queries. Measure
latency/memory impact against current parsed-unit reuse; process isolation is
not permission to duplicate semantics or discard cache identity contracts.
Do not broaden the guard around protocol or additional stateful work.

### 1. Retain documentation comments and declaration extents - hover, completion, and signature help implemented

The lexer now normalizes contiguous `///` blocks as leading trivia, the parser
transfers them onto declarations, and semantic analysis attaches the Markdown
to exact compiler symbols. Hover consumes that value for declarations and
resolved uses, including selected calls/constructors, included source, and
dirty dependency overlays. Completion candidates and signature help now
publish the same symbol-attached value: each function overload resolves its
own declaration symbol rather than the shared overload-set scope entry, and
the protocol adapter follows the client's declared `documentationFormat`
exactly as hover follows `contentFormat`.

Compiler-generated API documentation now consumes the same symbol-attached
value through `DocumentationIndex`; its JSON/Markdown projection is delivered.
Broader documentation coverage remains library/content work, not a second
comment parser or documentation store. See
[the documentation architecture](../architecture/reference-documentation.md).

Statement-level declaration extents are implemented: the parser stamps every
statement funnel result with its first-through-last-token span, and document
symbols consume them. Exact name ranges stay distinct from full extents.
Expression-level extents remain future work gated on a concrete feature.

### 2. Complete compiler-owned semantic queries

Current-snapshot references by `SymbolId` and occurrence role, document
highlights over the same query, and fail-closed rename (function-local
identities, lambda-capture closure, reserved-name and visibility-collision
validation, source-verified edits) are implemented in `LanguageQueries`.

Concrete generic-instance diagnostics are now published by stopping editor
frontend snapshots after HIR. This deliberately reuses canonical instance
discovery and reanalysis rather than reproducing it in the protocol layer;
failure metadata and MIR remain skipped.

Still extend `LanguageQueries` and `SemanticDatabase` rather than adding LSP
tables:

- preferred declaration/definition and overload-set queries;
- rename beyond function-local scope once reverse-dependency coverage exists
  (a project index or workspace-wide analysis), keeping the fail-closed rule;
- source-unit/default-library/internal visibility as semantic properties.

References and rename must fail closed when identity or coverage is incomplete.

### 3. Make repeated frontend analysis measurable and reusable - implemented

The end-to-end `scripts/lsp_benchmark.py` workload now measures cold diagnostics,
root-only edit diagnostics, dirty dependency/dependant analysis, semantic
tokens, hover, completion, cancellation, and superseded generations. Its
opt-in frontend trace records exact load, lex, parse, semantic-analysis, and
cache-hit unit lists, phase times, and bounded-cache occupancy. Schema 2 also
asserts that a root-only edit reuses unchanged dependencies and the prelude,
that dirty dependency invalidation remains scoped, and that the configured
memory limits hold.

The measured implementation reuses exact-content lexical rows and immutable
parsed statement trees through a 256-row/64-MiB LRU. Source generation and
digest, post-directive token identity, transitive dependency generations and
content identities, target data layout, configuration flags, and completion
context participate in identity; dirty lexical overlays compare exact bytes.
Semantic models, symbol IDs, HIR, and other snapshot-local records are always
rebuilt.

On the reference arm64 macOS machine and identical RelWithDebInfo/16-dependency
configuration, the final campaign alternated baseline/current binaries for 30
measured pairs after 5 warmup pairs. Repeated edit-to-diagnostics fell from
5.140 ms to 3.132 ms, a 39.1% reduction. Cold open fell from 5.886 ms to
4.478 ms, a 23.9% improvement rather than a regression.

| Interaction | Baseline median | Current median | Reduction |
| --- | ---: | ---: | ---: |
| cold diagnostics | 5.886 ms | 4.478 ms | 23.9% |
| repeated-edit diagnostics | 5.140 ms | 3.132 ms | 39.1% |
| semantic tokens | 0.148 ms | 0.163 ms | -10.3% |
| hover | 0.065 ms | 0.071 ms | -9.1% |
| completion | 4.647 ms | 2.565 ms | 44.8% |
| dirty-dependency diagnostics | 8.085 ms | 4.504 ms | 44.3% |
| cancellation | 4.493 ms | 2.526 ms | 43.8% |
| superseded diagnostics | 9.698 ms | 5.410 ms | 44.2% |

Semantic tokens and hover operate on the already-published immutable snapshot;
their measured increases were 0.015 ms and 0.006 ms respectively and both
remain below 0.2 ms. The root-only trace lexed and parsed one root while
reusing all 16 dependencies and the prelude. A dirty dependency loaded, lexed,
and parsed that unit; its dependant reused every lexical row, reparsed the root,
and did not reparse the prelude or 15 unrelated dependencies. Maximum observed
cache occupancy was 51 rows and 4.7 MB estimated against the 256-row/64-MiB
policy. The dominant remaining repeated-edit work is whole-program semantic
analysis, followed by HIR; the evidence did not justify a project index,
PCH/preamble, background shards, or per-file workers. The benchmark's
synthetic project loads no standard library, so it understates real editing
cost by two orders of magnitude; see "Per-Keystroke Analysis Cost" below.

### 4. Extract document/snapshot service boundaries - frontend seam implemented

`LanguageServer` currently combines protocol, document overlays, scheduling,
diagnostics, formatting, and feature handlers. Exact frontend cache ownership,
source generations, learned digests, worker capture, and stale identity
publication now live behind the independently tested `FrontendSnapshotState`.
This is the ownership seam required by the measured reuse work.

Keep extracting open-source versions, dependency scheduling, immutable result
publication, and request freshness only when a concrete feature or testability
problem warrants it. Preserve the existing worker model during that
extraction.

### 5. Consume resolved project configuration - implemented for selected-profile host analysis

Driver-owned `ProjectFacts` subsume the earlier bounded nearest-manifest
integration for `[build].defines` and the default profile's execution model.
The managed-thread tooling gate proves that a concurrent default-profile
project receives valid diagnostics, hover, definition, completion, and
semantic tokens without exposing its private capability surface. Standalone
analysis still accepts the same explicit execution-profile selection as direct
compilation. The immutable analysis configuration owns capability derivation;
the protocol adapter does not duplicate it.

The LSP now consumes the driver's read-only `ProjectFacts` for the package
containing each entry document. Analysis and completion receive the same
default/explicitly selected profile configuration and exact dependency source-root graph as
project compilation. Resolution never builds or fetches; locked-Git failures
remain structured driver diagnostics; standalone files retain their prior
defaults; and canonical dirty source overlays remain authoritative across the
resolved graph. Manifest and lockfile watcher events invalidate every open
snapshot.

Named profile selection is implemented through initialization options and
`settings.gti.profile`, with generation-bound analysis/completion, token refresh,
dirty overlay preservation, malformed-setting rejection and unknown-profile
diagnostics. The bundled Neovim plugin exposes `:GTIProfile` and supplies
theme-safe, high-priority inactive-code styling.

Explicit client selection of a different workspace package, target,
or cross target remains a future client-configuration
contract rather than part of this row.

## Project Index Boundary

Add a `ProjectSymbolIndex` only when unopened-file completion, cross-file
definition/references, or project rename demonstrates the need. Records must be
independent of AST lifetime and use a deliberately designed durable identity.
Fresh open-document facts override possibly stale index data.

A first implementation can be an in-memory index of successfully analyzed
files. Do not begin with background shards, remote indexes, persistent caches,
or distributed infrastructure.

## Incomplete Source

Continue parser recovery and completion-marker analysis. Represent unknown/error
facts honestly. Do not use an AST with changed source offsets, and do not grow a
second declaration parser in semantic-token or completion handlers. Bounded
lexical fallbacks are acceptable only when clearly less authoritative.

## Per-Keystroke Analysis Cost

Status: measured 2026-10-10; editor-scoped HIR and background raw-pointer audit
are implemented. Lazy trusted-body analysis and broader semantic reuse remain
proposed. This is the compiler-side half of editor responsiveness. The
protocol-side half (edit-time publication, adaptive debounce, refresh policy)
is implemented and described in the architecture document.

### Measured Baseline Before Scope Reduction

Release `gti_lsp` on arm64 macOS, driven by a client-like message flow (open,
`didChange`, tokens and hover requests) with `GTI_LSP_FRONTEND_TRACE`. Every
edit in that baseline re-ran semantic analysis and HIR for every loaded unit;
the parse cache removed parsing from the picture but nothing else. These
figures establish the motivation, not current per-edit timings after step 1.

| Workload | Units analysed per edit | Per-edit frontend time |
| --- | ---: | ---: |
| `001-basics.gti`, no includes | 2 | 4 ms |
| `transit-planner` file, one `<std/cstdio>` include | 11-14 | 190-290 ms |
| `089-library-workflow.gti`, eight standard-library includes | 21 | 580 ms |

Phase breakdown for the eight-include file (one edit, parse fully cached):

| Phase | Time |
| --- | ---: |
| Compiler-trusted (standard-library) function bodies | 247 ms |
| HIR lowering | 250 ms |
| Declaration registration, all units | 37 ms |
| Semantic finalization | 20 ms |
| Application (user-file) function bodies | 9 ms |

About ninety percent of each keystroke re-analyses and re-lowers library
bodies whose text did not change. The reference benchmark did not show this
because its synthetic project loads no standard library.

### Recommendation

Keep the LSP a consumer of `FrontendOptions`. Each step below is a compiler
change that `gti check` can also benefit from, measured with the same message
flow before and after, and none adds a second semantic path in the protocol
layer.

1. **Editor-scoped HIR — implemented in 0.397.4.**
   `FrontendOptions::lowerEntryUnitOnly` seeds the program roots and the
   entry unit's declarations and lowers only what they reach; the LSP sets
   it. Measured: HIR 250 ms to 155 ms on the eight-include file (it reaches
   much of what it includes) and 45 ms to 5 ms per transit-planner file.
   Diagnostics inside library bodies that the open file instantiates are
   still produced because those instances are reached.

   Artifact builds gained a different trusted-library HIR scope in 0.399.3
   (`lowerLibraryReachableOnly`, commit `514b7554`): application/package units
   are lowered completely and unreached compiler-trusted HIR declarations are
   skipped. This is delivered compiler work, not a remaining editor-scope
   task. Neither option makes trusted semantic body analysis lazy; `gti check`
   retains complete lowering.

   The same measurement found a far larger cost next to it: with
   `raw-pointer-audit = "warn"` in the project profile, one analysis of the
   eight-include file took 41 s (trusted bodies 19 s, HIR 17 s). The audit is
   now never part of edit-path analysis; it runs as a background audit of a
   settled generation (see the architecture document), so the figures above
   are the edit-path cost in every project.
2. **Lazy trusted-body analysis.** Determine which facts a caller's analysis
   takes from a callee's body (borrow summaries and inferred borrow origins,
   failure and effect results, lifecycle facts) and analyse a compiler-trusted
   body only when a caller demands one of them, memoising the result for the
   snapshot. Bodies no application code reaches are then never analysed in
   the editor. This removes most of the 247 ms and is a clean generalisation
   of the existing on-demand borrow-summary candidate inspection.
   Precondition, now met: body analysis publishes inferred facts onto
   callable declarations that callers consume, so those facts had to be
   independent of the order in which bodies are analysed. They were not: a
   generic callee returning a moved parameter was rejected (`GTI-S2045`) when
   declared after its caller. The fact is now demanded through
   `ensureBodyAnalyzed`, which advances the resumable lexical pass to the
   callee (see "Body Order And Demand" in
   [semantic-analysis.md](../architecture/semantic-analysis.md)). The
   mechanical gate exists as the `body_analysis_order` test: the complete pass
   and the `Deferred` test mode (trusted bodies after application bodies, in
   reverse order) must produce identical diagnostics over the examples and
   standard-library tests. Implemented in 0.409.0 using the deferred
   `codex/lsp-lazy-bodies` checkpoint as the starting point. Snapshots and
   completion now use reachable trusted bodies; generic HIR discovery requests
   missing primary facts before accepting usable HIR. Artifact builds use the
   same bounded policy, revising the earlier editor-only restriction; full
   checks and artifact raw-pointer audits remain complete. See ADR 024.
3. **Unit-local semantic reuse** (the true preamble analogue) only if 1 and 2
   leave the eight-include file above about 100 ms per edit. Trusted units'
   registration and body facts would be cached per unit, keyed by the unit's
   content digest and its dependency digests, with identities that survive
   across snapshots. This is the largest change because `SemanticModel`
   identities are snapshot-local today, and it should not be started until
   the cheaper steps have been measured.
4. **Completion scope.** Completion still runs its own frontend analysis per
   request, now with the same trusted-body scope reduction. Consider answering from
   the root's current snapshot when the completion marker does not change
   the visible declarations.

Exit measure: the eight-include file under 100 ms per edit with the probe,
semantic diagnostics for the open file identical to `gti check` on its
project, and the frontend trace measuring omitted library bodies (constant
definitions, virtual methods and lifecycle bodies remain conservative/eager).
Record the campaign in `implementation-sequence.md`
before starting; it touches semantic analysis and HIR ownership, so the
compiler-architecture review applies.

## Deferred clangd Complexity

Preambles/PCH, compile-command borrowing, macro expansion locations, per-file
workers, retained semantic-AST LRUs and remote indexes are not default GTI
requirements. The implemented bounded lexical/parsed-unit LRU already has
memory accounting; it does not retain a semantic snapshot. Broader cache or
index machinery needs an identified GTI latency/coverage problem and an
identity/invalidation contract. clangd's update debounce was adopted in 0.397.3 after the
measurement above showed per-keystroke whole-program re-analysis; it is
described in the architecture document.

## Acceptance Checks For Every Feature

1. The semantic fact comes from the compiler frontend.
2. The result is tied to the exact source generation and UTF-16 conversion is
   protocol-only.
3. Another feature can reuse the compiler query where appropriate.
4. Broken source produces a safe partial/no result rather than a confident
   guess.
5. Dependency overlays and stale results are tested.
6. Protocol handlers contain no independent GTI lookup or overload logic.

The clangd reference map and evidence that informed this direction were
previously embedded in the retired `gti-lsp-architecture` skill. For deeper
research, start with clangd's `ClangdLSPServer`, `ClangdServer`, `TUScheduler`,
`ASTWorker`, `ParsedAST`, `FileIndex`, `BackgroundIndex`, `Hover.cpp`,
`CodeComplete.cpp`, `FindTarget.cpp`, and semantic-highlighting implementation.
