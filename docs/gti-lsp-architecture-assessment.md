# GTI LSP architectural assessment

Assessed against the live worktree on 2026-08-08, including the in-progress compiler-owned hover and completion work. This is an architectural review only; it does not propose a clangd-shaped rewrite.

## Implementation update

GTI 0.44.0 implements the first bounded layer from recommendations 1 and 2:

- `SemanticDatabase` owns snapshot-scoped `SymbolId`, `SymbolKind`,
  `SymbolRecord`, and explicit occurrence roles;
- source declarations and resolved uses are connected for namespaces, aliases,
  classes, structs, enums, enumerators, overloads, constructors, destructors,
  fields, bindings, parameters, captures, generic parameters, and type uses;
- `LanguageQueries::definition` follows the exact resolved symbol and selected
  overload or constructor, including declarations in a directly included source
  unit;
- semantic tokens use compiler symbol facts whenever a current snapshot exists.
  The old token classifier remains only as an explicitly degraded fallback
  before analysis publishes a snapshot.

Snapshot-local IDs deliberately do not promise persistence across edits.
`ScopeId`, full declaration extents, retained documentation, references,
rename, and a project index remain follow-up layers.

## Overall assessment

GTI already has the right central direction: one compiler run produces an owned frontend result, the LSP retains that result as an immutable generation-checked snapshot, and hover/completion call compiler-owned queries. The most important correction is to extend that model to semantic highlighting and future navigation/refactoring features.

The current `SemanticDatabase` is a useful beginning, but it is an occurrence table rather than a complete tooling symbol model. It can answer “what semantic occurrence covers this byte?”, but it cannot yet reliably answer “which declaration does this use denote?”, “where is its definition?”, or “which other occurrences denote the same symbol?”. Building definition, references, or rename directly on the current representation would invite feature-specific workarounds.

## What GTI already does well

### Coherent compiler-owned snapshots

[`FrontendResult`](../include/gti/frontend.h#L25) owns the `Program`, `SemanticModel`, HIR, source graph, source manager, diagnostics, and validity flags together. This is the important `ParsedAST` lesson without Clang's lifetime machinery: semantic side tables containing AST pointers remain valid while their owning `FrontendResult` remains alive.

The LSP stores a [`shared_ptr<const FrontendResult>`](../src/lsp/main.cpp#L1761) in an [`AnalysisSnapshot`](../src/lsp/main.cpp#L1768). [`analyzeAndPublish`](../src/lsp/main.cpp#L2475) commits it only when the document generation and dependency generations are still current. This is a sound ownership and stale-result boundary.

### Compiler-owned hover and completion

[`LanguageQueries::hover`](../include/gti/language_queries.h#L252) reads the compiler's `SemanticDatabase`, uses resolved call/construction records, and formats signatures through the compiler-owned `SignaturePrinter`. It does not redo overload resolution in the LSP.

[`LanguageQueries::complete`](../include/gti/language_queries.h#L358) runs the real frontend in a dedicated completion mode. [`Lexer::scanForCompletion`](../include/gti/lexer.h#L59) marks the cursor, while semantic completion capture uses the analyser's actual scopes, receiver/member lookup, access rules, and substituted callable types. This matches the useful clangd principle while remaining much simpler.

### Editor-time recovery

[`Parser::parse`](../include/gti/parser.h#L43) retains declarations around errors and [`Parser::synchronize`](../include/gti/parser.h#L1560) recovers at grammar boundaries. [`FrontendOptions::analyzeRecoveredProgram`](../include/gti/frontend.h#L19) allows semantic analysis of that partial program for editor use. GTI is therefore already treating invalid source as normal LSP input.

### Versioned document overlays and diagnostics

[`didOpen`](../src/lsp/main.cpp#L1923), [`didChange`](../src/lsp/main.cpp#L1951), and [`didClose`](../src/lsp/main.cpp#L1992) retain unsaved buffers, invalidate snapshots and dependent roots, and clear or republish diagnostics. Analysis requests include all open source overrides, so included GTI files use their unsaved editor contents.

### Source encoding boundary

Compiler [`SourceSpan`](../include/gti/diagnostic.h#L32) uses source identity and byte offsets. The LSP's [`SourcePositionIndex`](../src/lsp/main.cpp#L142) performs UTF-8 byte to UTF-16 position conversion at the protocol edge. That separation should be preserved.

## Duplicated or guessed semantics

### Semantic tokens contain a second declaration analyser

This is the largest current problem. [`basicSemanticType`](../src/lsp/main.cpp#L947) guesses roles from adjacent punctuation, including treating `.` plus `(` as a method and `::` as namespace/function context. [`classifyType`](../src/lsp/main.cpp#L1022), generic-parameter parsing, and [`classifyDeclarations`](../src/lsp/main.cpp#L1166) independently recognize classes, enums, aliases, functions, parameters, fields, generic constraints, and declaration extents.

That code duplicates parser and semantic rules and will drift as GTI evolves. [`applyResolvedBindingClassifications`](../src/lsp/main.cpp#L1545) overlays real compiler information only for binding occurrences, so the final token stream is a mixture of semantic truth and textual guesses.

Lexical classification of keywords, literals, operators, and comments is appropriate. Semantic classification of identifiers is not.

### Standard-library identity is inferred from spelling

`isDefaultLibraryReference` in [`src/lsp/main.cpp`](../src/lsp/main.cpp#L934) treats a path rooted at the text `std` as default-library code. Default-library status should eventually be a symbol/source-unit property supplied by the compiler, especially if aliases or re-exports are added.

### Comments are rescanned outside the lexer

[`collectCommentTokens`](../src/lsp/main.cpp#L1509) scans raw source because the lexer discards line comments at [`Lexer::scan`](../include/gti/lexer.h#L214). This is tolerable as a small lexical fallback, but it prevents one retained source of comment ranges and blocks compiler-owned documentation comments.

## Missing semantic and source information

### No stable symbol identity in tooling records

[`SemanticOccurrence`](../include/gti/semantic_analyzer.h#L576) contains a name, type, role-like flags, and several optional declaration pointers, but no `SymbolId`. Binding uses and declarations are not connected by a stable identity. The analyser's richer private [`Symbol`](../include/gti/semantic_analyzer.h#L3777) is transient and discarded after checking.

This blocks robust definition, references, rename, and unified semantic highlighting.

### Incomplete occurrence kinds and roles

[`SemanticOccurrenceKind`](../include/gti/semantic_analyzer.h#L562) lacks general records for namespaces, fields as symbols, enumerators, generic parameters, and many explicit type/name references. Occurrences also lack a complete role such as declaration, definition, read, write, call, or type-reference.

### No declaration/definition location query

The public [`SemanticDatabase`](../include/gti/semantic_analyzer.h#L597) exposes occurrences and `findOccurrence`, but no symbol record, declaration location, preferred definition, containing scope, or references-by-symbol query.

### AST nodes do not share a complete source extent

The base [`Expr`](../include/gti/ast.h#L240) and [`Stmt`](../include/gti/ast.h#L281) interfaces carry no common source range. Individual nodes retain useful tokens, but tooling cannot uniformly ask for a node's full half-open extent. Exact name ranges are already available; full declaration/expression extents should be added only where a concrete feature needs them.

### Documentation is not retained

`HoverInfo` has `documentationMarkdown`, but no compiler symbol currently owns parsed documentation and the lexer discards comments. Standard-library and third-party hover documentation therefore cannot yet flow from source declarations automatically.

## Problems likely to hurt future features

- Implementing definition by spelling search would be ambiguous for scopes, overloads, fields, and shadowed locals.
- Implementing references or rename without `SymbolId` would confuse unrelated equal spellings; cross-file rename would also lack a completeness boundary.
- Extending the current semantic-token parser for every new syntax feature would make `src/lsp/main.cpp` a second frontend.
- The 2,700-line [`LanguageServer`](../src/lsp/main.cpp#L1786) currently combines JSON-RPC, document state, scheduling, diagnostics, formatting, and features. This is still workable, but it will become difficult to test and evolve once definition/references/rename arrive.
- AST pointers are safe inside the retained `FrontendResult`, but future caches or indexes must not copy those pointers beyond the snapshot lifetime.

## Ranked improvements

### 1. Fix now — add a compiler-owned tooling symbol/occurrence schema

Introduce the smallest snapshot-scoped `SymbolId`, `SymbolKind`, `SymbolRecord`, and `OccurrenceRole` needed by current features. Connect every semantic identifier occurrence to its resolved symbol where resolution succeeds. Store exact name range, declaration location, optional definition location, type/signature, scope relationship, and relevant modifiers.

Extend the existing `SemanticDatabase`; do not create an LSP database or a parallel AST. Cross-snapshot/project-stable IDs can wait until an index needs them.

### 2. Fix now — make semantic tokens consume compiler facts

Record namespaces, types, generic parameters, functions/methods, parameters, bindings, fields, enums/enumerators, and resolved uses in the semantic database. Map `SymbolKind` and occurrence modifiers to LSP token types in `src/lsp/main.cpp`, then remove the declaration/generic/member classifiers there.

Keep direct lexer handling for keywords, literals, operators, and retained comments. When analysis is unavailable, provide only an explicitly lexical fallback rather than confident identifier guesses.

### 3. Improve soon — retain documentation and tooling-grade extents

Retain documentation comments as lexer trivia or parser-attached declaration data, normalize them once, and store Markdown on the compiler symbol. Continue using the existing compiler signature printer for hover and completion. Add full source extents selectively for declarations, expressions, and fix-producing nodes that need them.

### 4. Improve soon — separate document/snapshot service from protocol handling

Extract open text, versions/generations, overlays, dependency invalidation, analysis scheduling, and snapshot lookup behind a small internal service. Leave JSON decoding, capabilities, position conversion, and response serialization in the protocol adapter. Preserve the existing two-worker model; this recommendation does not require more concurrency.

### 5. Defer until needed — project index and clangd-scale scheduling

Define a `ProjectSymbolIndex` interface only when unopened-file completion, cross-file definition/references, or rename requires it. Start with a simple in-memory implementation if appropriate; do not add a background crawler, persistent shards, remote index, per-file workers, preambles, AST LRUs, or adaptive debounce now.

Add request cancellation or more parallelism when measurements show the current workers are causing editor latency, while retaining the current generation checks and immutable snapshot boundary.
