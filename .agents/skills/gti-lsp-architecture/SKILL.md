---
name: gti-lsp-architecture
description: Design, review, or modify the GTI language server using the compiler frontend as the semantic source of truth. Use for document snapshots, AST lifetime, semantic tokens, hover, completion, definition, references, rename, diagnostics, source ranges, indexing, incomplete-source recovery, project or manifest configuration, LSP protocol boundaries, or request scheduling. Apply clangd's architectural lessons without importing C++-specific complexity.
---

# GTI LSP Architecture

## Core principle

Treat GTI's compiler frontend as the authoritative source of language semantics.

Resolve names, types, scopes, members, overloads, declaration/definition links, source ranges, and diagnostics once in the frontend. Make LSP features query those results. Never let semantic tokens, hover, completion, navigation, references, or rename grow independent interpretations of GTI syntax.

Keep the intended flow visible:

```text
LSP JSON-RPC
    -> protocol adapter
    -> document/snapshot service
    -> compiler-owned language queries
    -> FrontendResult + SemanticModel
                         + optional ProjectSymbolIndex
```

Do not require one GTI class for every clangd class. Preserve a simpler existing mechanism when it enforces the same ownership and dependency boundary.

## Working method

Before designing or changing a feature:

1. Run `git status --short` and preserve unrelated worktree changes.
2. Inspect the live GTI frontend, semantic model, source model, and LSP implementation.
3. Trace the semantic fact from parsing through analysis before adding an LSP representation.
4. Put reusable language knowledge in compiler-owned data or queries.
5. Keep LSP-specific encoding and capability negotiation in the protocol layer.
6. Add only the smallest boundary that solves a current GTI problem.
7. Test valid, invalid, incomplete, dependency-overlay, and stale-version cases in proportion to the feature.

## Architectural boundaries

| Boundary | Owns | Consumes | Allowed dependencies | Must not own |
|---|---|---|---|---|
| Protocol adapter | JSON-RPC IDs, LSP capabilities, position/range conversion, request/result serialization, diagnostic publication | LSP messages and compiler-query results | Document service and language-server API | GTI lookup, type inference, overload selection, declaration parsing |
| Language-server API/orchestration | Feature entry points and request routing independent of JSON | URI, version, internal position, query inputs | Document service, language queries, optional index | Parser rules or duplicated semantic tables |
| Document service | Open text, client version, internal generation, source overlays, dependency invalidation, current immutable snapshot | `didOpen`, `didChange`, `didClose`, frontend results | Frontend and scheduler | Feature-specific semantic state |
| Compiler frontend | Lexical tokens, recovered AST, semantic analysis, diagnostics, source graph | Exact source version, target, overlays, completion point | Lexer, parser, semantic analyser | LSP protocol types |
| Semantic model/database | Symbols, occurrences, types, scopes, resolved members/calls, locations, documentation | Results produced while analysing the AST | Compiler data structures and source model | JSON, Markdown envelopes, editor state |
| Language queries | Reusable hover, completion, navigation, reference, rename, and highlighting facts | Immutable semantic snapshot and optional index | Semantic model, compiler printers, source model | Document lifetime or protocol bookkeeping |
| Project symbol index | Durable symbol summaries and cross-file occurrences, when needed | Completed semantic results for files | Stable compiler-owned symbol schema | Raw AST pointers, open-document versions, LSP messages |

Dependencies flow downward in this table. The protocol layer may translate a compiler `SymbolKind` to an LSP token or completion kind; it may not decide the `SymbolKind` from identifier spelling.

## AST ownership and semantic snapshots

Use one coherent immutable snapshot for each analysed document version. GTI's current `FrontendResult` is a suitable snapshot payload because it owns the `Program`, `SemanticModel`, `SourceGraph`, `SourceManager`, and diagnostics together.

Maintain these rules:

- Publish a snapshot atomically only after its analysis finishes.
- Associate it with the exact open-document version and an internal monotonically increasing generation.
- Reject results from obsolete generations.
- Keep the exact source text used for analysis available for range conversion.
- Keep the entire `FrontendResult` alive while any semantic record contains AST pointers.
- Never cache raw AST or declaration pointers outside their owning snapshot.
- Return values, snapshot-scoped IDs, or execute queries while a shared snapshot handle is alive.
- Treat a snapshot as immutable after publication.

A conceptual document record may contain:

```text
DocumentState
  URI / canonical path
  current source
  client version
  analysis generation
  current shared immutable FrontendResult
  current diagnostics and dependency set
```

Do not duplicate tokens, AST, and semantic state beside `FrontendResult` merely to match this shape.

## Document lifecycle

### `didOpen`

Store the supplied source and version, advance the generation, invalidate cached feature results, analyse using all open-buffer overlays, and publish the snapshot and diagnostics only if the generation is still current.

### `didChange`

Apply the advertised synchronization mode exactly, advance the generation, invalidate the previous snapshot for queries against new byte offsets, coalesce obsolete queued analyses, invalidate affected dependants, and schedule a replacement analysis. Never combine new source text with an AST from an older version.

### `didClose`

Remove the overlay and snapshot, cancel or invalidate pending work, clear that document's published diagnostics, and reanalyse open dependants if their view changes. Release the snapshot after outstanding readers finish.

Use client versions for protocol publication and internal generations for race-free scheduling. Do not assume clients always supply a version.

## Project configuration boundary

When `gti.toml` project support lands, consume one resolved compiler/driver-owned
project configuration. Do not create an LSP-specific manifest parser or infer
package roots from editor workspace folders independently.

- Treat the project configuration as an immutable, versioned analysis input
  containing target information, declared source roots, and relevant language
  settings.
- Advance a configuration generation when a manifest, lockfile, selected
  target, or dependency root changes; invalidate affected snapshots just as a
  dependency edit does.
- Preserve standalone analysis for `.gti` files outside a project.
- Keep dirty document overlays authoritative over on-disk project sources.
- Do not fetch dependencies, compile targets, run programs or hooks, clean
  outputs, or rewrite manifests and lockfiles as a side effect of opening or
  analysing a document.
- Prefer a reusable in-process project model. If a metadata protocol is used,
  keep it stable and read-only and never parse human CLI output.

Use `$gti-build-architecture` and `docs/build-system-proposal.md` when project
configuration changes both the build driver and the LSP.

## Semantic model required by tooling

Grow a shared compiler-owned query surface as features require it. Prefer records conceptually equivalent to:

- `SymbolId`: unambiguous within a snapshot; define project-stable identity only when indexing requires it.
- `SymbolRecord`: name, qualified name, kind, declared type/signature, containing scope, access, mutability, generic parameters/constraints, declaration location, optional definition location, documentation, members or overload membership.
- `OccurrenceRecord`: source range, resolved `SymbolId`, role such as declaration/definition/read/write/call/type-reference, and resolved expression type where relevant.
- `ScopeRecord`: parent, owner symbol, visible declarations, and receiver/member information where useful.

Support reusable queries such as:

- symbol or expression at a source position;
- symbol kind and canonical signature;
- declared and resolved type;
- declaration and preferred definition;
- containing scope and visible names;
- resolved member for `object.member`;
- selected overload for a call;
- members of a resolved receiver type;
- occurrences of a symbol in the current snapshot;
- documentation attached to the declaration.

Resolve `object.member` once. Reuse that resolution for highlighting, hover, definition, completion, references, and rename.

## Source locations and ranges

Preserve tooling-grade locations through lexer, parser, AST, and semantic analysis:

- source-unit identity or canonical source path;
- half-open UTF-8 byte offsets `[start, end)`;
- precise name/token range;
- full node or declaration extent where a feature needs it;
- declaration and definition locations as distinct facts;
- occurrence role and any related diagnostic/fix-it ranges.

Derive line and column from the exact snapshot source. Convert UTF-8 byte offsets to negotiated LSP positions, normally UTF-16, only at the protocol boundary. Centralize and test this conversion. Do not store LSP `Position` inside compiler semantics.

GTI does not need Clang's separate macro spelling and expansion locations unless GTI gains macros with that behaviour.

## Feature implementation rules

### Semantic tokens

- Obtain identifier roles from semantic occurrences and symbols: `Occurrence -> SymbolId -> SymbolKind -> internal highlight kind -> LSP token type`.
- Use the lexer directly for intrinsically lexical categories such as keyword, literal, operator, and retained comments.
- Mark declaration, definition, readonly, static, and similar modifiers from semantic facts.
- Keep the LSP legend and wire encoding in the protocol adapter.
- If recovery leaves an identifier unresolved, omit it or use an explicitly degraded lexical fallback. Do not parse declaration grammar or guess a member's role from punctuation in the LSP.

### Hover

- Resolve the semantic occurrence at the cursor.
- Prefer the selected overload for a resolved call and the overload set for an unresolved callable name.
- Format canonical GTI syntax with a compiler-owned declaration/signature printer.
- Attach compiler-retained documentation for standard and third-party source uniformly.
- Let the protocol adapter wrap the signature and documentation in `MarkupContent`.

### Completion

- Use a completion-specific compiler entry point because the cursor often interrupts otherwise invalid syntax.
- Capture parser context and query real semantic scopes, receiver types, members, visibility, access, constraints, and substitutions.
- Let a future project index supplement candidates outside the current semantic snapshot; do not let it replace local scope analysis.
- Keep ranking and snippet synthesis in a shared tooling query, not scattered through protocol handlers.
- Reject results whose source generation is obsolete.

### Go-to-definition

- Resolve an exact symbol at the cursor.
- Return its preferred definition, falling back to its declaration.
- Use the current semantic snapshot first and an index only for locations absent from it.
- Never navigate by searching for matching identifier text.

### References

- Start from an exact `SymbolId`.
- Collect fresh current-document occurrences from the snapshot.
- Add external occurrences from an optional index, then deduplicate.
- Preserve reference roles when known.

### Rename

- Require an unambiguous renameable symbol and exact occurrence ranges.
- Validate the new name and scope conflicts through compiler semantics.
- Require complete-enough reference coverage for the requested scope.
- Verify indexed ranges against current file contents and fail closed when stale or uncertain.
- Never implement rename as textual replacement of equal spellings.

### Diagnostics

- Publish structured diagnostics produced by the same frontend snapshot used for queries.
- Preserve severity, code, primary range, related ranges, fix-its, and source identity.
- Publish only if the analysed generation is current, include the client version when available, and clear diagnostics on close.
- Keep editor publication policy separate from diagnostic production.

## Incomplete and broken source

Expect editor buffers such as `object.`, `object.mem`, `foo(`, missing delimiters, and temporarily inconsistent dependencies.

- Make parser recovery produce the largest safe partial AST rather than abandoning the whole file.
- Retain accurate ranges and explicit error/unknown states for recovered nodes.
- Let semantic analysis continue over valid recovered declarations and expressions while representing unavailable types as unknown/error.
- Insert a dedicated completion marker at the cursor and preserve the parser/semantic context reached there.
- Keep diagnostics recoverable and avoid cascades where practical.
- Permit features to return partial results, but distinguish semantic results from a documented lexical fallback.
- Never use a stale AST with changed source offsets. A stale snapshot is usable only with the source version it represents.

Clangd's stale-preamble completion is a C++ latency optimization, not a general requirement for GTI.

## Current document versus project index

Keep the future composition boundary explicit:

```text
CurrentDocumentSemanticState (fresh, exact open buffer)
                     +
ProjectSymbolIndex (broader, possibly stale)
                     |
                     v
               language queries
```

Current-document facts win on conflicts. Keep index records independent of AST lifetime and expose a small query interface such as lookup by ID, fuzzy symbol search, and references by ID.

Do not build a background index until unopened-file completion, project-wide definition/references, or rename creates a demonstrated need. Begin with current snapshots or a simple in-memory index of successfully analysed open files if that solves the immediate problem.

## Scheduling and concurrency

Understand why clangd has `TUScheduler` and `ASTWorker`: Clang ASTs are expensive and mutable during construction; per-file serialization makes reads observe preceding writes, keeps AST lifetimes safe, prevents one file from blocking all requests, and allows obsolete work to be cancelled or skipped.

For GTI now:

- Keep request handling responsive and analysis off the protocol read loop if analysis is observably blocking.
- Serialize publication through versions/generations and atomically swap snapshots.
- Coalesce obsolete document analyses.
- Bound completion work and discard stale results.
- Encapsulate scheduling behind document/query boundaries so cancellation or more workers can be added later.

Do not add a thread per document, preamble workers, AST LRUs, adaptive debounce, memory profiling, or speculative index requests without measured GTI latency or memory pressure. A simple worker model is preferable while it remains correct.

## Classification of clangd lessons

### ADOPT

- **Compiler-backed semantics:** prevents divergent meanings across compiler and editor features.
- **Coherent immutable snapshots:** makes AST-pointer lifetime and version correctness explicit.
- **Shared symbol/occurrence queries:** lets one resolution power many features.
- **Protocol/semantics separation:** keeps JSON and LSP evolution from contaminating the language model.
- **Precise source identity and ranges:** makes navigation, edits, diagnostics, and UTF-16 conversion reliable.
- **Recovery plus completion-specific parsing:** incomplete source is normal editor input.
- **Versioned publication and stale-result rejection:** asynchronous results must not overwrite newer edits.
- **Fresh current state plus optional index:** local accuracy and project breadth have different lifetimes.

### PREPARE FOR

- **Stable symbol and scope IDs:** add snapshot-scoped IDs as definition/references/rename need them; add cross-snapshot identity only for indexing.
- **A document-state service boundary:** extract it when protocol handlers or tests can no longer manage lifecycle clearly.
- **A `ProjectSymbolIndex` interface:** define it when the first cross-file feature needs broader knowledge.
- **Current-file overlay over project data:** retain the boundary so dirty buffers can supersede stale indexed data.
- **Cancellation and per-document scheduling:** keep generation checks and query APIs compatible, but implement richer scheduling only when latency warrants it.

### DO NOT COPY

- **Preambles, PCH, header command borrowing, and compile-command machinery:** these solve C/C++ headers and translation-unit configuration.
- **Macro spelling/expansion location machinery:** unnecessary unless GTI adopts expansion semantics that create dual locations.
- **Multiple dynamic/background/static/remote index layers, persisted shards, and distributed services:** these solve enormous C++ projects and resource costs.
- **Per-translation-unit threads, AST LRU caches, adaptive debounce, speculative completion, and detailed memory accounting:** these are clangd performance engineering, not baseline correctness.
- **Clang `CompilerInstance`/`FrontendAction` lifetime workarounds:** preserve GTI's simpler owned `FrontendResult` instead.
- **Heuristic resolution for dependent C++ templates, Objective-C, modules, include insertion/cleanup, and clang-tidy integration:** adopt only an analogous feature after GTI itself creates the need.

## Architecture checklist

Before implementing or modifying an LSP feature, check:

1. Is this information already available from the compiler frontend?
2. Am I duplicating semantic analysis inside the LSP?
3. Can another LSP feature reuse this information?
4. Does the relevant AST or symbol retain an accurate source range?
5. Does this work on incomplete source where reasonably possible?
6. Am I introducing state into a feature that should instead be a query over document state?
7. Am I prematurely introducing clangd complexity?
8. Is the protocol layer being kept separate from GTI semantics?

Also check snapshot ownership, source-version freshness, UTF-16 conversion, and whether uncertainty should produce no result rather than an unsafe guess or edit.

## clangd architectural map

| clangd concept | Responsibility and data boundary | GTI equivalent or lesson |
|---|---|---|
| `ClangdLSPServer` | Owns client capabilities, request IDs, protocol caches, JSON handlers, and result conversion; consumes `ClangdServer` callbacks | **ADOPT:** thin JSON-RPC/LSP protocol adapter |
| `ClangdServer` | Owns drafts and composes scheduler, feature functions, and indexes; consumes internal request values | **ADOPT:** language-server API and orchestration, independent of JSON |
| `TUScheduler` | Owns open-file workers, task ordering, versioned parse inputs, AST/preamble caching, and publication sequencing; feature code receives bounded AST access | **ADOPT** lifecycle/version boundary; **DO NOT COPY** its C++ caching sophistication |
| `ASTWorker` | Owns one file's latest mutable inputs and serialized queue; consumes updates and AST read actions | **PREPARE FOR:** serialized document work only when GTI needs more scheduling |
| `ParsedAST` | Keeps Clang AST, Sema, source manager, preprocessor, tokens, diagnostics, and their lifetime owners coherent; consumes exact parse inputs and a preamble | **ADOPT:** compiler-owned snapshot, currently close to `FrontendResult`; do not expose pointers beyond it |
| `SymbolIndex` | Defines lookup, fuzzy-find, references, and relations over durable symbol IDs; consumers do not depend on an implementation | **PREPARE FOR:** a much smaller `ProjectSymbolIndex` query interface |
| `FileIndex` | Snapshots freshly parsed open-file symbols/references and overlays them over broader data | **PREPARE FOR:** current/dirty knowledge wins over stale project data |
| `BackgroundIndex` | Owns project crawling, queues, persisted shards, and a broad eventually consistent symbol view | **DO NOT COPY** until GTI has a measured project-wide need |
| `SemanticHighlighting` | Consumes resolved AST references and declaration kinds, then emits feature-neutral highlight facts | **ADOPT:** semantic occurrence/symbol to highlighting mapping |
| `CodeComplete` | Runs Clang parser/Sema at a completion point and merges its context-sensitive results with optional index candidates | **ADOPT:** dedicated compiler completion mode; **DO NOT COPY** speculative/preamble optimization |
| `Hover` | Resolves the selected AST node/reference and renders structured semantic facts, optionally supplemented by the index | **ADOPT:** compiler-owned hover query and signature/documentation data |
| `XRefs` and `Rename` | Resolve exact declarations/IDs, combine fresh AST occurrences with indexed external occurrences, and reject unsafe edits | **ADOPT:** shared symbol identity and fail-closed refactoring; **PREPARE FOR** project coverage |

Use this map conceptually. Do not create matching classes merely for symmetry.

## clangd references

Architectural conclusions were checked against LLVM commit `2f9775ad49bf8eb0d4a54592d40652df3e4f38dd`:

- Protocol and orchestration: [`ClangdLSPServer.h`](https://github.com/llvm/llvm-project/blob/2f9775ad49bf8eb0d4a54592d40652df3e4f38dd/clang-tools-extra/clangd/ClangdLSPServer.h), [`ClangdLSPServer.cpp`](https://github.com/llvm/llvm-project/blob/2f9775ad49bf8eb0d4a54592d40652df3e4f38dd/clang-tools-extra/clangd/ClangdLSPServer.cpp), [`ClangdServer.h`](https://github.com/llvm/llvm-project/blob/2f9775ad49bf8eb0d4a54592d40652df3e4f38dd/clang-tools-extra/clangd/ClangdServer.h), and [`ClangdServer.cpp`](https://github.com/llvm/llvm-project/blob/2f9775ad49bf8eb0d4a54592d40652df3e4f38dd/clang-tools-extra/clangd/ClangdServer.cpp).
- State, scheduling, and ownership: [`TUScheduler.h`](https://github.com/llvm/llvm-project/blob/2f9775ad49bf8eb0d4a54592d40652df3e4f38dd/clang-tools-extra/clangd/TUScheduler.h), [`TUScheduler.cpp`](https://github.com/llvm/llvm-project/blob/2f9775ad49bf8eb0d4a54592d40652df3e4f38dd/clang-tools-extra/clangd/TUScheduler.cpp), [`ParsedAST.h`](https://github.com/llvm/llvm-project/blob/2f9775ad49bf8eb0d4a54592d40652df3e4f38dd/clang-tools-extra/clangd/ParsedAST.h), and [`ParsedAST.cpp`](https://github.com/llvm/llvm-project/blob/2f9775ad49bf8eb0d4a54592d40652df3e4f38dd/clang-tools-extra/clangd/ParsedAST.cpp).
- Semantic features: [`SemanticHighlighting.cpp`](https://github.com/llvm/llvm-project/blob/2f9775ad49bf8eb0d4a54592d40652df3e4f38dd/clang-tools-extra/clangd/SemanticHighlighting.cpp), [`Hover.cpp`](https://github.com/llvm/llvm-project/blob/2f9775ad49bf8eb0d4a54592d40652df3e4f38dd/clang-tools-extra/clangd/Hover.cpp), [`CodeComplete.cpp`](https://github.com/llvm/llvm-project/blob/2f9775ad49bf8eb0d4a54592d40652df3e4f38dd/clang-tools-extra/clangd/CodeComplete.cpp), [`XRefs.cpp`](https://github.com/llvm/llvm-project/blob/2f9775ad49bf8eb0d4a54592d40652df3e4f38dd/clang-tools-extra/clangd/XRefs.cpp), and [`refactor/Rename.cpp`](https://github.com/llvm/llvm-project/blob/2f9775ad49bf8eb0d4a54592d40652df3e4f38dd/clang-tools-extra/clangd/refactor/Rename.cpp).
- Indexing and locations: [`index/Index.h`](https://github.com/llvm/llvm-project/blob/2f9775ad49bf8eb0d4a54592d40652df3e4f38dd/clang-tools-extra/clangd/index/Index.h), [`index/FileIndex.h`](https://github.com/llvm/llvm-project/blob/2f9775ad49bf8eb0d4a54592d40652df3e4f38dd/clang-tools-extra/clangd/index/FileIndex.h), [`index/Background.h`](https://github.com/llvm/llvm-project/blob/2f9775ad49bf8eb0d4a54592d40652df3e4f38dd/clang-tools-extra/clangd/index/Background.h), [`SourceCode.h`](https://github.com/llvm/llvm-project/blob/2f9775ad49bf8eb0d4a54592d40652df3e4f38dd/clang-tools-extra/clangd/SourceCode.h), and Clang's [`SourceLocation.h`](https://github.com/llvm/llvm-project/blob/2f9775ad49bf8eb0d4a54592d40652df3e4f38dd/clang/include/clang/Basic/SourceLocation.h).
- Official intent and larger-system context: [code walkthrough](https://clangd.llvm.org/design/code), [threads and request handling](https://clangd.llvm.org/design/threads), and [index design](https://clangd.llvm.org/design/indexing).
