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
inside `lang::runGuarded`, and that boundary is deliberately narrow. Only
compiler work runs under the guard: `runIsolatedAnalysis` builds an isolated
`DocumentAnalysis` and its immutable frontend snapshot, and `publishAnalysis`
writes to shared LSP state afterwards, on the normal path.

Two properties make the boundary sound:

- **No exception crosses LLVM.** LLVM's `CrashRecoveryContext` is built
  without C++ exception support, so the guarded callback catches its own
  exceptions and returns the outcome as data (`GuardedAnalysis`). The worker
  inspects that status instead of relying on an exception unwinding through
  an LLVM frame.
- **No lock is held under the guard.** The guarded callback touches no shared
  state and acquires no mutex, so a signal-recovery stack restore that skips
  destructors cannot leave `stateMutex` owned. Publication acquires the lock
  on a normally unwound path, where `lock_guard` behaves normally.

After a contained crash the partially built analysis is deliberately leaked
rather than destroyed, because running destructors over an abandoned frame
risks a second fault. The document is skipped and its pending semantic
requests are rejected.

This is crash containment, not process isolation. In-process recovery cannot
undo heap corruption that occurred before a fault, so a sufficiently damaging
crash may still leave the server unhealthy even though the worker survives.
Process isolation remains the stronger option if that guarantee is ever
required; it is tracked in
[`docs/plans/lsp-evolution.md`](../plans/lsp-evolution.md).

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
- Crash containment is in-process. It keeps the worker and shared state
  usable after a contained fault, but it is not a process-isolation
  guarantee: heap damage done before a fault is not undone.

Those items are tracked in [`docs/plans/lsp-evolution.md`](../plans/lsp-evolution.md).
[ADR 005](../decisions/005-lsp-compiler-semantics.md) records why language
features share compiler semantics.

## Do Not Copy From clangd Yet

GTI does not need C++ preambles/PCH, compile-command borrowing, macro
spelling/expansion locations, background index shards, remote indexes, AST LRU
caches, or one worker per translation unit. Preserve the boundaries that allow
measured evolution without importing those systems preemptively.
