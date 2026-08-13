# Frontend: Sources, Lexing, Parsing, And AST

Status: Current implementation.

The frontend converts one entry source and its dependency graph into a
syntax-preserving `Program`, then hands that owned tree to semantic analysis.

## Source Loading

`SourceLoader` in `include/gti/source_loader.h` canonicalizes source identity,
lexes each unit, resolves `#include` directives, and builds `SourceGraph`.
Quoted paths are relative GTI files; `<std/name>` resolves only beneath the
configured GTI standard-library roots. When the driver supplies an immutable
package graph, `<alias/name>` resolves only through the including package's
direct dependency aliases and declared source root. These package units retain
stable package identity/path provenance but remain ordinary untrusted GTI
source. A quoted include cannot cross an owning package boundary. Direct mode
does not discover manifests or synthesize package roots. Includes are
load-once dependency edges, not textual substitution. The implicit prelude is
an edge to every ordinary unit.

Ordinary `Frontend::analyze()` performs this load itself. The compiled driver
also exposes `loadCompilationInputs` for project-cache orchestration. It still
uses `SourceLoader`, retains the resulting `SourceGraph`, `SourceManager`, and
source diagnostics as one snapshot, and either hashes that snapshot for a
verified cache hit or moves it into `Frontend::analyzeLoaded` on a miss. The
loaded entry point resumes the same parser/semantic/HIR/MIR pipeline; it is not
a second include resolver or frontend representation.

Each `SourceUnit` retains its path, logical import where applicable, tokens,
dependency edges, and final declaration range in the combined transitional
`Program`. Semantic visibility uses the graph: a unit sees itself, direct
includes, and the prelude, not arbitrary declarations in the combined AST.

`SourceGraph::compilationOrder()` supplies the current dependency-first parsing
and combined-AST assembly order. It is not runtime program-initialization
authority. The accepted D-EXEC-01 contract requires semantics to build a
separate immutable initialization plan from prelude order, lexical include
directive spans, active declarations, and source positions. Source-unit IDs,
loader worklist order, and the combined declaration vector cannot stand in for
that future plan.

## Lexer

Token identity and source spelling live in `include/gti/token.h`. `Lexer`
declarations live in `include/gti/lexer.h`; scanning implementation is compiled
in `src/compiler/lexer.cpp`. Tokens retain source identity, one-based line, and
UTF-8 byte offset. Fixed-width integer aliases normalize to shared token kinds,
and reserved `__gti_` identifiers are rejected here. Every C++20/C++23 core
keyword that has no GTI meaning is classified as `CPP_RESERVED`; `delete` has
a dedicated token because the parser accepts it only in special-member policy.
Identifier uses of either form receive `GTI-P0002` before semantic analysis or
native C++ emission. `sizeof` and `alignof` have dedicated reserved token kinds
and are classified as word operators, not ordinary identifiers or declaration
keywords. Source loading still recognizes reserved spellings as components of
`<std/...>` paths because those components are not identifiers.

Decimal scanning retains an exact GTI-owned `BinaryFloat`: unsuffixed
spellings select binary32 and `d`/`D` selects binary64. The compiled lexer
passes the original decimal digits directly to the private `APFloat`
implementation; it never converts through host `double`.

The lexer currently discards comments. The formatter and editor tooling scan
comments separately. Documentation-comment retention is therefore not yet a
compiler semantic capability.

## Parser And AST

`Parser` in `include/gti/parser.h` owns grammar, precedence, AST construction,
and synchronization. `include/gti/ast.h` owns syntax node and visitor contracts.
AST children own their subtrees. Syntax nodes preserve written forms and source
tokens; they do not own resolved types or selected declarations.

`LayoutQuery` preserves the written operator token and one parenthesized
`TypeRef`. The parser does not accept an expression operand, an unparenthesized
form, or a layout query directly inside the deliberately smaller array-extent
grammar. Supported layout categories and the resulting constant belong to
semantics, not this syntax node.

Class-declaration attributes are retained as written syntax before the class
kind. The bounded native-record form is `[[c_abi]] struct`; the parser records
the attribute and kind but does not decide whether the body is a valid passive
C record or compute its layout. `ClassDecl` also records whether the declaration
used the bodyless `struct Name;` form. Semantics admits that syntax only with
`[[c_opaque]]`; the parser keeps it recoverable so an ordinary or malformed
forward declaration receives one source diagnostic rather than a brace
cascade.

`ConceptDecl` retains every written type parameter and concept application.
`FunctionDecl` optionally retains a `RequiresClause` containing the `requires`
token and its conjunction of applications. The parser enforces only the
bounded application grammar and rejects disjunction; concept identity, arity,
visible type-parameter arguments, capability composition, and call viability
belong to semantic analysis.

`FunctionDecl` also retains one receiver mode. Ordinary members are read-only
or use trailing `mut`; `operator()` alone may use trailing `&&` for the
consuming mode. The parser recognizes `operator()() &&` structurally rather
than treating `&&` as a general reference type, and rejects that qualifier on
free functions, ordinary methods, other operators, and conflicting receiver
qualifiers before semantics.

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
