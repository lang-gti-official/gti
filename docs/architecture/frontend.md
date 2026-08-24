# Frontend: Sources, Lexing, Parsing, And AST

Status: Current implementation.

The frontend converts one entry source and its dependency graph into a
syntax-preserving `Program`, then hands that owned tree to semantic analysis.

## Source Loading

`SourceLoader` is declared in `include/gti/source_loader.h`; its canonical path,
filesystem, include-resolution, cycle, provenance, and source-manager algorithms
are compiled into `gti_compiler` from `src/compiler/source_loader.cpp`.
`SourceGraph` traversal/mutation and `SourceManager` location work compile in
`src/compiler/source_graph.cpp` and `src/compiler/diagnostic.cpp`. The loader
canonicalizes source identity, lexes each unit, resolves `#include` directives,
resolves target/configuration conditions in source order, and builds
`SourceGraph`. The initial valueless flag set comes from `FrontendOptions`.
Active `#define`/`#undef` directives update that set and are removed before
parsing; they never substitute tokens. A definition before an include enters
that unit, and active definitions made there remain visible when loading
resumes. Definitions in an inactive branch do not leak. The loader records the
source-ordered value on each `defined(NAME)` flag token while retaining every
branch for the parser; semantic selection still owns target comparisons and
boolean composition.
Quoted paths are relative GTI files; `<std/name>` resolves only beneath the
configured GTI standard-library roots. When the driver supplies an immutable
package graph, `<alias/name>` resolves only through the including package's
direct dependency aliases and declared source root. These package units retain
stable package identity/path provenance but remain ordinary untrusted GTI
source. A quoted include cannot cross an owning package boundary. Direct mode
does not discover manifests or synthesize package roots. Includes are
load-once dependency edges, not textual substitution. The implicit prelude is
an edge to every ordinary unit.
Successful source-written include edges also retain the exact path spelling and
that directive's zero-based lexical occurrence. Those facts are not another
resolution mechanism: the failure-metadata builder uses them only to derive a
deterministic, host-path-free identity for a source outside the artifact's
logical root.

Ordinary `Frontend::analyze()` in `src/compiler/frontend.cpp` performs this load
itself. The compiled driver also exposes `loadCompilationInputs` for
project-cache orchestration. It still
uses `SourceLoader`, retains the resulting `SourceGraph`, `SourceManager`, and
source diagnostics as one snapshot, and either hashes that snapshot for a
verified cache hit or moves it into `Frontend::analyzeLoaded` on a miss. The
loaded entry point resumes the same parser/semantic/HIR/MIR pipeline; it is not
a second include resolver or frontend representation.

Each `SourceUnit` retains its path, stable exact-source digest, logical import
where applicable, tokens, token-granular inactive spans for editor
presentation, dependency edges, and final declaration range in the combined
transitional `Program`. Semantic
visibility uses the graph: a unit sees itself, direct includes, and the
prelude, not arbitrary declarations in the combined AST.

### Reusable lexical and parsed units

`FrontendCache` is an optional frontend input owned by a coordinator such as
the LSP. It reuses only source text/tokens or immutable parsed statement trees;
it never retains semantic records, snapshot-local symbol IDs, HIR, MIR, or
backend state. A unit key contains its canonical path, coordinator-supplied
source generation, stable exact-content digest, normalized target data layout
and configuration flags, and an entry completion offset when present. Parsed
rows additionally key the exact post-directive token-stream digest, including
resolved configuration-flag values and source geometry, plus a deterministic
digest of that unit's transitive dependency inputs. The dependency digest
includes graph order, canonical identities, generations, exact source digests,
and processed token digests. A dependency that changes the active flag state
therefore invalidates an otherwise unchanged dependant's parsed row without
invalidating unrelated graphs. A missing source, token-stream, or dependency
digest forces a miss. Dirty lexical overlays also require an exact
byte-for-byte source match.

The cache uses one process-local, thread-safe least-recently-used policy across
lexical and parsed rows. The default limits are 256 rows and 64 MiB of
conservatively estimated storage. Coordinators advance only changed source
generations; older rows become unreachable and are reclaimed by normal
eviction. This keeps invalidation explicit without allowing a protocol thread
to synchronously destroy a large cached tree. Source loading still rebuilds
the dependency graph and processes include/configuration directives for every
snapshot, so the cache cannot bypass path, overlay, visibility, or active-branch
authority.

`SourceGraph::compilationOrder()` supplies the current dependency-first parsing
and combined-AST assembly order. It is not runtime program-initialization
authority. `SourceGraph` separately retains configured prelude roots in their
exact input order; `clear()`, copying a loaded snapshot, and
`Frontend::analyzeLoaded` preserve or reset that vector together with the
entry, units, and dependency edges. Invalid or stale root identities fail
closed rather than silently changing order.

Semantics consumes those facts to build the separate immutable
`ProgramInitializationPlan`: ordered prelude roots precede the entry walk,
explicit include edges are followed in lexical dependency-first order with
first-visit deduplication, and only declarations in selected compile-time
branches are admitted in source order. Source-unit IDs, loader worklist order,
`compilationOrder()`, and the combined declaration vector are therefore not
execution authority.

## Lexer

Token identity, source spelling, and small `constexpr` classifications live in
`include/gti/token.h`; the keyword table is defined once in
`src/compiler/token.cpp`. `Lexer` declarations live in `include/gti/lexer.h`;
scanning implementation is compiled in `src/compiler/lexer.cpp`. Tokens retain
source identity, one-based line, and
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

The lexer discards ordinary `//` line comments and non-nesting `/* ... */`
block comments while preserving following token offsets and line numbers. A
contiguous `///` block is instead normalized as Markdown leading trivia on the
next declaration token; the parser transfers it to the declaration and
semantics attaches it to the exact tooling symbol. An unterminated block
comment produces `GTI-L0011` at its opening delimiter and stops later phases
without parser cascades. The formatter and lexical editor highlighting still
scan written comment ranges separately.

## Parser And AST

`Parser` in `include/gti/parser.h` owns grammar, precedence, AST construction,
recovery, and synchronization through a narrow facade. Its algorithms and
request-local parser state are compiled once in `src/compiler/parser.cpp`;
consumers depend only on the facade, AST, token, and diagnostic contracts.
`include/gti/ast.h` owns syntax node and visitor contracts.
AST children own their subtrees. Top-level and nested statement handles use
shared ownership so an exactly compatible cache row and one or more immutable
frontend snapshots can retain the same parsed tree; expression children remain
uniquely owned inside that tree. Syntax nodes preserve written forms and source
tokens; they do not own resolved types or selected declarations, and compiler
passes treat cached trees as immutable.

Every final expression and statement node exposes one closed `ExprKind` or
`StmtKind` tag in addition to the visitor interface. `exprAs<T>`/`stmtAs<T>`
check that tag before a static cast. This avoids repeated RTTI name comparisons
in semantic and HIR classification while leaving syntax authority and visitor
exhaustiveness unchanged.

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

`ClassDecl` also distinguishes an ordinary generic declaration from an exact
qualified specialization. The latter retains one complete target `TypeRef`,
including the written namespace path and concrete arguments, while its name
token remains the final primary-name segment for diagnostics and tooling.
The parser recognizes this form only through a qualified target such as
`class std::hash<Widget> { ... };`; canonical argument identity, primary
lookup, package coherence, and selection belong to semantics.

The parser also records `union` as a distinct `ClassKind` while reusing the
recoverable class-member tree; semantic analysis owns the narrower passive
shape. `EnumeratorDecl` retains an optional ordered `Parameter` payload. The
AST does not infer a tag representation, validate payload storage, or decide
whether a switch pattern is exhaustive.

`MutableFieldGroupDecl` preserves a class/struct member written as
`mut { ... }`, including its braces and conditional/configuration children.
The parser restricts its contents to direct non-static field declarations and
records each child `VariableDecl` with effective mutable semantics. The group
itself has no symbol identity. Semantics flattens its active children in the
surrounding access context before HIR, while the retained node remains
available to formatting and source tooling.

`StaticAssertDecl` preserves the keyword, delimiters, condition, optional
message separator and string token, semicolon, and complete source extent. It
is admitted as a declaration at namespace, complete class-like, and block
scope, while the parser excludes parameter lists, initializer lists, concept
requirements, `extern "C"`, and mutable field groups. The node introduces no
symbol; exact typing, constant evaluation, and generic deferral belong to
semantics.

`ConceptDecl` retains every written type parameter and concept application.
`FunctionDecl` and `ConstructorDecl` optionally retain a `RequiresClause`
containing the `requires` token and its conjunction of applications. Concept
applications retain `TypeRef` arguments so semantics can distinguish visible
generic parameters from closed exact types. The parser enforces only the
bounded application grammar and rejects disjunction; concept identity, arity,
argument closure, capability composition, and call viability belong to
semantic analysis.

`ArrayInitializer` is reused for fixed-array declarations and for braces in a
call argument list. The parser records only the written elements; it does not
invent an element type, an extent type, or an initializer-list object.
`ConstructorDecl` retains constructor-local generic parameters after the
constructor name and its optional trailing requirements before an initializer
list. Semantics owns their nested scope, exact inference, constraint
validation, and whether a constructor value parameter is inferable from one
fixed-array context.

`FunctionDecl` also retains one receiver mode. Ordinary members are read-only
or use trailing `mut`; `operator()` alone may use trailing `&&` for the
consuming mode. The parser recognizes `operator()() &&` structurally rather
than treating `&&` as a general reference type, and rejects that qualifier on
free functions, ordinary methods, other operators, and conflicting receiver
qualifiers before semantics.

`LambdaCapture` retains the capture target plus an owned initializer subtree.
A bare `[value]` capture uses a generated source-variable initializer so copy
and move forms share ordered lowering. The parser accepts the C++-familiar
`[target = expression]` shape for recovery, but semantics admits only the exact
`[target = std::move(local)]` owned form; arbitrary init-capture meaning is not
implemented in syntax handling.

`Parser::parse()` recovers at declaration/statement boundaries and retains later
valid declarations. `Frontend::analyze()` parses each source unit independently
in dependency order, appends declarations to the transitional `Program`, and
records each unit's declaration range. Syntax errors normally stop later
phases; `FrontendOptions::analyzeRecoveredProgram` lets the LSP semantically
analyze the safe recovered subset while keeping code generation disabled.

Recursive declaration, statement, type, compile-condition, assignment, call,
grouping, and unary parsing share a structural depth budget of 64. Crossing
that budget produces parser-owned `GTI-P0005` at the first token beyond the
limit and unwinds through ordinary recovery; it never relies on a process
signal or native stack exhaustion. The bound also keeps recovered syntax safe
for downstream AST visitors. LSP frontend workers have an explicit stack budget
large enough to reach this source-level limit even in unoptimized builds. The
diagnostic suggests splitting the source into shallower parts and has no fix-it
because no single rewrite preserves intent.

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
`Semantics`, `Hir`, or the default `Mir`. Completion returns after semantic
context capture. Ordinary editor snapshots request `Hir` so concrete generic
instance reanalysis publishes the same source-level diagnostics as `gti
check`, but the LSP does not query HIR directly and never builds MIR. Skipped
phases keep their validity flags false, so code generation stays disabled. The
semantic model is moved out of the analyzer once its last reader (HIR instance
reanalysis) has finished rather than deep-copied.

## Boundaries

- Change include behavior in `SourceLoader`/`SourceGraph`, not the parser or
  emitter.
- Change grammar and recovery in `Parser`/AST, not semantic lookup.
- Preserve source-unit identity and half-open byte spans for downstream
  diagnostics and tooling.
- Resolve source-ordered flag references in `SourceLoader`, parse every branch,
  and keep target comparison and boolean selection in semantics.
- Add each new AST node to every applicable visitor, HIR path, formatter,
  Tree-sitter grammar, and tooling surface rather than leaving a silent default.

The accepted syntax is summarized by
[`docs/language/grammar.ebnf`](../language/grammar.ebnf). Source-unit rationale
is recorded in [ADR 002](../decisions/002-source-units-and-includes.md).
