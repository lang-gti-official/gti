---
name: lsp-synchronization
description: Synchronize or implement GTI LSP and editor behavior after compiler syntax, semantic, diagnostic, source-range, token, symbol, type, completion, hover, definition, reference, rename, formatting, or project-configuration changes. Use for gti_lsp protocol features, snapshots, semantic tokens, language queries, incomplete-source recovery, stale-version handling, Tree-sitter/Neovim alignment, or compiler/LSP divergence.
---

# Synchronize The GTI LSP

Keep the compiler frontend as the semantic source of truth and the LSP as a
versioned protocol adapter.

## Workflow

1. Run `git status --short`. Read
   [`docs/architecture/lsp.md`](../../../docs/architecture/lsp.md),
   [ADR 005](../../../docs/decisions/005-lsp-compiler-semantics.md), and the
   language/compiler docs for the changed fact.
2. Trace the fact from lexer/parser through `SemanticModel`/
   `SemanticDatabase`. Add reusable data or a query in compiler code when the
   frontend does not expose it. Do not infer semantic identifiers from spelling
   or punctuation in `src/lsp/main.cpp`.
3. Query an immutable `FrontendResult` for the exact document/dependency
   generation. Keep AST pointers and snapshot-scoped IDs inside that lifetime.
4. Keep JSON-RPC IDs, capabilities, URI/position conversion, serialization,
   diagnostic publication, and workspace edits in the protocol layer.
5. Test valid source, incomplete/broken source, unsaved dependency overlays,
   stale document generations, cancellation/supersession, close/reopen, and
   non-ASCII UTF-16 positions as applicable.

## Feature Checks

- **Semantic tokens:** map semantic occurrence -> symbol kind -> LSP type;
  lexical categories may come from the lexer/fallback.
- **Hover/signature:** use compiler-selected declarations/overloads and
  compiler-owned GTI printers; attach retained documentation when implemented.
- **Completion:** use completion-marker frontend analysis and actual scopes,
  receiver types, access, constraints, and substitutions.
- **Definition/references/rename:** start from exact symbol identity; never
  search equal identifier text. Fail closed if coverage is incomplete.
- **Diagnostics/code actions:** publish the current snapshot's structured
  diagnostics and derive edits only from current compiler fix-its.
- **Formatting/Tree-sitter/editor queries:** update only when syntax/token
  structure changes; keep structural and semantic highlighting roles distinct.

Do not add a project index, document-service extraction, or richer concurrency
unless the feature has a concrete need. Record future work in
[`docs/plans/lsp-evolution.md`](../../../docs/plans/lsp-evolution.md) and update
the current architecture doc when behavior lands.

After local validation, use `$finish-release` when the completed LSP/editor
change is ready to commit or release. Finish after the push or workflow
dispatch succeeds; do not wait for asynchronous GitHub CI/CD.
