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

What remains is the stronger guarantee, not the state-safety one: in-process
recovery cannot undo heap damage that occurred before a fatal signal. Moving
analysis to a subprocess and treating process termination as the recovery
boundary is the option if that is ever required. Do not broaden the current
guard around more protocol or stateful work.

### 1. Retain documentation comments and declaration extents

Preserve `///` comments as lexer trivia or declaration-attached data, normalize
their Markdown once, and attach it to compiler symbols. Hover, completion,
signature help, and generated API documentation should consume the same value
for standard and third-party source.

Statement-level declaration extents are implemented: the parser stamps every
statement funnel result with its first-through-last-token span, and document
symbols consume them. Exact name ranges stay distinct from full extents.
Expression-level extents remain future work gated on a concrete feature.

### 2. Complete compiler-owned semantic queries

Current-snapshot references by `SymbolId` and occurrence role, document
highlights over the same query, and fail-closed rename (function-local
identities, lambda-capture closure, reserved-name and visibility-collision
validation, source-verified edits) are implemented in `LanguageQueries`.

Still extend `LanguageQueries` and `SemanticDatabase` rather than adding LSP
tables:

- expose concrete generic-instance diagnostics through an editor-safe compiler
  query without requiring the protocol layer to run or reproduce HIR lowering;
- preferred declaration/definition and overload-set queries;
- signature help from the selected overload and argument position;
- rename beyond function-local scope once reverse-dependency coverage exists
  (a project index or workspace-wide analysis), keeping the fail-closed rule;
- source-unit/default-library/internal visibility as semantic properties.

References and rename must fail closed when identity or coverage is incomplete.

### 3. Extract document/snapshot service boundaries

`LanguageServer` currently combines protocol, document overlays, scheduling,
diagnostics, formatting, and feature handlers. Extract open source, versions,
generations, dependency invalidation, immutable snapshot publication, and
request freshness behind a testable service when new features make the current
class difficult to evolve. Preserve the existing worker model during that
extraction.

### 4. Consume resolved project configuration

Use a read-only driver-owned project/metadata model for selected target and
source roots. Do not parse `gti.toml` semantics inside the LSP, build/fetch on
open, or let on-disk project state override dirty buffers.

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

Preambles/PCH, compile-command borrowing, header insertion, macro expansion
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
