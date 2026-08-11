# 005: LSP Features Query Compiler Semantics

Status: Accepted

## Context

Semantic tokens, hover, completion, definition, references, rename, and
diagnostics all need the same scopes, symbols, types, overloads, and source
ranges as compilation. Re-parsing those rules in the protocol server causes
feature drift.

## Decision

The LSP retains immutable `FrontendResult` snapshots and calls compiler-owned
`LanguageQueries`/`SemanticDatabase`. The protocol layer owns JSON-RPC,
capabilities, positions, serialization, publication, and edits—not GTI
semantics. One resolved member or call should power every applicable feature.

## Alternatives

- Maintain an LSP-specific semantic database: rejected because it duplicates
  the frontend and diverges on broken or evolving syntax.
- Use text and punctuation heuristics for semantic identifiers: permitted only
  as explicitly degraded lexical behavior where no semantic result exists.

## Consequences

Tooling requirements influence compiler source ranges, symbols, occurrences,
and documentation retention. Snapshot lifetimes and document generations are
explicit. Project indexes, additional scheduling, and cross-file identities
are added only when a concrete feature requires them.
