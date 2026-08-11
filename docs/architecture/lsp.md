# GTI Language Server Architecture

Status: Current implementation, with future symbol/index work explicitly
separated below.

The LSP is an adapter over compiler-owned semantics:

```text
JSON-RPC/LSP
  -> protocol conversion in src/lsp/main.cpp
  -> open-document state and generation checks
  -> immutable FrontendResult snapshot
  -> LanguageQueries / SemanticDatabase
```

## Semantic Source Of Truth

`FrontendResult` owns the recovered AST, `SemanticModel`, `SemanticDatabase`,
HIR/MIR, source graph, source text, and diagnostics together. The LSP retains a
`shared_ptr<const FrontendResult>` in each `AnalysisSnapshot`. This keeps AST
pointers and snapshot-scoped symbol IDs valid while a request reads them.

`LanguageQueries` in `include/gti/language_queries.h` owns reusable hover,
completion, and definition logic. Semantic tokens use compiler symbol and
occurrence facts for identifiers and lexical facts for keywords, literals,
operators, and comments. The protocol layer must not repeat name lookup,
overload selection, type rendering, or declaration parsing.

## Document State

`LanguageServer` currently owns protocol handling and document lifecycle in one
large class. It stores unsaved source overlays, client versions, internal
analysis generations, dependency generations, immutable snapshots, diagnostic
sets, and semantic-token caches.

- `didOpen`/`didChange` replace the full synchronized source, advance the
  generation, invalidate the prior snapshot/cache and affected dependants, and
  schedule analysis over all open-buffer overlays.
- `didClose` removes the overlay and snapshot, clears diagnostics, invalidates
  pending work, and reanalyzes dependants whose view changes.
- Completed analysis is published only if the document and dependency
  generations still match. Stale semantic requests are rejected.

Analysis and completion have separate bounded worker queues. This keeps the
JSON loop responsive and lets newer completion/analysis work supersede older
requests without adopting clangd's per-translation-unit scheduler complexity.

Editor analysis requests `FrontendOptions::stopAfter = Semantics`: no LSP
feature reads HIR or MIR, so those phases are not lowered per change. The
validity flags of skipped phases stay false and code generation remains
disabled, which is already the LSP contract. Analysis work additionally runs
inside `lang::runGuarded`, but that boundary is only best-effort and must not
be treated as complete server isolation. The guarded callback calls
`analyzeAndPublish`, so it covers both compiler analysis and publication into
shared LSP state.

There are two known hazards:

- LLVM's `CrashRecoveryContext` is built without C++ exception support. A GTI
  exception escaping the callback would cross an LLVM frame before reaching
  the worker's outer `catch`, which is not a valid recovery design.
- Crash recovery may bypass C++ stack unwinding. If a fatal signal occurs
  while `analyzeAndPublish` owns `stateMutex`, the `lock_guard` destructor may
  not run and the worker's recovery path can deadlock while trying to use the
  same state.

The safe future boundary is narrower: build an isolated `DocumentAnalysis`
and immutable frontend snapshot under the crash guard, catch and retain C++
exceptions inside the callback, return normally through LLVM, and publish to
shared state only after successful guarded completion. Process isolation is
the stronger future option if in-process signal recovery cannot provide the
required guarantees. Until that work lands, normal C++ exceptions remain
handled by the worker, but recovery from memory faults or LLVM fatal failures
is not guaranteed to leave the LSP usable.

## Protocol Boundary

`src/lsp/main.cpp` owns JSON-RPC IDs, capabilities, URIs, UTF-8 byte to LSP
position conversion, request/result serialization, diagnostic publication,
workspace edits, cancellation, and semantic-token wire encoding. It advertises
full document sync, formatting, semantic tokens, hover, completion, and
definition. It advertises quick-fix code actions when the client supports code
action literals.

Diagnostic serialization preserves compiler codes, severities, phases, hints,
related locations, and fix-its while respecting the client's
`publishDiagnostics` capabilities. Hints stay in the plain-text message for
universal display and are also exposed as structured data when supported.
Quick fixes come only from compiler `Diagnostic::fixes` associated with the
current source/generation, an intersecting request range, and an exact
client-provided diagnostic. Position conversion is tested at the protocol
edge; compiler source spans remain half-open UTF-8 byte ranges.

## Incomplete Source

Editor analysis enables parser recovery and semantic analysis of recovered
declarations. Completion runs a dedicated frontend analysis with a cursor
marker so the parser/analyzer can capture real scope, receiver, visibility, and
overload candidates in fragments such as `object.` or `foo(`. Features may
return partial or no semantic results; they must not confidently invent missing
meaning from punctuation.

## Current Limits

- Documentation comments are not retained by the compiler, so hover's optional
  Markdown documentation field is normally empty.
- Definition is implemented from exact resolved symbols. References, rename,
  signature help, and a project symbol index are not implemented.
- `SymbolId` is snapshot-local. There is no durable cross-analysis identity.
- Document/scheduling state is not yet extracted from the protocol class.
- Project manifest/source-root configuration is not yet a shared resolved LSP
  input.
- The current in-process `runGuarded(analyzeAndPublish)` boundary has the
  exception-crossing and lock-unwinding hazards described above. It is crash
  reporting and best-effort recovery, not a process-isolation guarantee.

Those items are tracked in [`docs/plans/lsp-evolution.md`](../plans/lsp-evolution.md).
[ADR 005](../decisions/005-lsp-compiler-semantics.md) records why language
features share compiler semantics.

## Do Not Copy From clangd Yet

GTI does not need C++ preambles/PCH, compile-command borrowing, macro
spelling/expansion locations, background index shards, remote indexes, AST LRU
caches, or one worker per translation unit. Preserve the boundaries that allow
measured evolution without importing those systems preemptively.
