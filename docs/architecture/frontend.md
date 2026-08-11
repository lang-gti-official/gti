# Frontend: Sources, Lexing, Parsing, And AST

Status: Current implementation.

The frontend converts one entry source and its dependency graph into a
syntax-preserving `Program`, then hands that owned tree to semantic analysis.

## Source Loading

`SourceLoader` in `include/gti/source_loader.h` canonicalizes source identity,
lexes each unit, resolves `#include` directives, and builds `SourceGraph`.
Quoted paths are relative GTI files; `<std/name>` resolves only beneath the
configured GTI standard-library roots. Includes are load-once dependency edges,
not textual substitution. The implicit prelude is an edge to every ordinary
unit.

Each `SourceUnit` retains its path, logical import where applicable, tokens,
dependency edges, and final declaration range in the combined transitional
`Program`. Semantic visibility uses the graph: a unit sees itself, direct
includes, and the prelude, not arbitrary declarations in the combined AST.

## Lexer

Token identity and source spelling live in `include/gti/token.h`. `Lexer`
declarations live in `include/gti/lexer.h`; scanning implementation is compiled
in `src/compiler/lexer.cpp`. Tokens retain source identity, one-based line, and
UTF-8 byte offset. Fixed-width integer aliases normalize to shared token kinds,
and reserved `__gti_` identifiers are rejected here.

The lexer currently discards comments. The formatter and editor tooling scan
comments separately. Documentation-comment retention is therefore not yet a
compiler semantic capability.

## Parser And AST

`Parser` in `include/gti/parser.h` owns grammar, precedence, AST construction,
and synchronization. `include/gti/ast.h` owns syntax node and visitor contracts.
AST children own their subtrees. Syntax nodes preserve written forms and source
tokens; they do not own resolved types or selected declarations.

`Parser::parse()` recovers at declaration/statement boundaries and retains later
valid declarations. `Frontend::analyze()` parses each source unit independently
in dependency order, appends declarations to the transitional `Program`, and
records each unit's declaration range. Syntax errors normally stop later
phases; `FrontendOptions::analyzeRecoveredProgram` lets the LSP semantically
analyze the safe recovered subset while keeping code generation disabled.

## Frontend Result And Gates

`FrontendResult` owns, in lifetime order:

- `Program`;
- `SemanticModel`;
- `HirProgram` and `MirProgram`;
- `SourceGraph` and `SourceManager`;
- structured diagnostics and phase validity flags.

`canGenerateCode()` requires source loading, syntax, semantics, HIR, and MIR to
all be valid. Completion uses a dedicated byte-offset marker and returns after
semantic capture. Backends must never run merely because parsing succeeded.

`FrontendOptions::toolingOccurrences` selects whether semantic analysis
records the editor occurrence table. Compilation leaves it off; the LSP
leaves it on.

`FrontendOptions::stopAfter` selects the last phase the pipeline runs:
`Semantics`, `Hir`, or the default `Mir`. A consumer that never reads HIR or
MIR (the LSP) requests `Semantics`; skipped phases keep their validity flags
false, so code generation stays disabled. The semantic model is moved out of
the analyzer once its last reader (HIR instance reanalysis) has finished
rather than deep-copied.

## Boundaries

- Change include behavior in `SourceLoader`/`SourceGraph`, not the parser or
  emitter.
- Change grammar and recovery in `Parser`/AST, not semantic lookup.
- Preserve source-unit identity and half-open byte spans for downstream
  diagnostics and tooling.
- Parse every target-conditional branch; semantics selects the active branch.
- Add each new AST node to every applicable visitor, HIR path, formatter,
  Tree-sitter grammar, and tooling surface rather than leaving a silent default.

The accepted syntax is summarized by
[`docs/language/grammar.ebnf`](../language/grammar.ebnf). Source-unit rationale
is recorded in [ADR 002](../decisions/002-source-units-and-includes.md).
