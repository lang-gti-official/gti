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

### 0. Make the crash boundary state-safe - implemented

`runIsolatedAnalysis` now guards only analysis and snapshot construction. It
catches all C++ exceptions inside the callback and returns them as
`GuardedAnalysis` state, so none crosses an LLVM frame, and it holds no lock
and touches no shared state, so a stack restore cannot strand `stateMutex`.
`publishAnalysis` performs generation checks, diagnostic publication, request
rejection, and every other shared-state mutation outside the crash-recovery
context. `lsp_protocol` covers the recovery path with
`test_worker_survives_failed_analysis`, which drives repeated failing analyses
through the worker and then requires ordinary analysis and a semantic request
to still succeed.

The two long-lived frontend workers also reserve 8 MiB stacks through
`llvm::thread`. This lets the compiler's 64-level parser budget publish
`GTI-P0005` before Darwin's smaller default secondary-thread stack is
exhausted, including completion's independent frontend analysis. The protocol
regression then edits and formats the same document, proving that both workers
and shared state remain usable.

What remains is the stronger guarantee, not the state-safety one: in-process
recovery cannot undo heap damage that occurred before a fatal signal. Moving
analysis to a subprocess and treating process termination as the recovery
boundary is the option if that is ever required. Do not broaden the current
guard around more protocol or stateful work.

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

Generated API documentation and broader standard library documentation remain
to consume the same value; they must not add a second comment parser or
documentation store.

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
PCH/preamble, background shards, or per-file workers.

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

### 5. Consume resolved project configuration - implemented for default host analysis

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
default-profile configuration and exact dependency source-root graph as
project compilation. Resolution never builds or fetches; locked-Git failures
remain structured driver diagnostics; standalone files retain their prior
defaults; and canonical dirty source overlays remain authoritative across the
resolved graph. Manifest and lockfile watcher events invalidate every open
snapshot.

Explicit client selection of a different workspace package, target,
non-default profile, or cross target remains a future client-configuration
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

## Deferred clangd Complexity

Preambles/PCH, compile-command borrowing, macro expansion
locations, per-file workers, AST LRUs, adaptive debounce, remote indexes, and
memory accounting solve C++/clang scale. Adopt none without a measured GTI
problem.

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
