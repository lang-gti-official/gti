# Frontend: Sources, Lexing, Parsing, And AST

Status: Current implementation.

The frontend converts one entry source and its dependency graph into a
syntax-preserving `Program`, then hands that owned tree to semantic analysis.

Typed `#macro` declarations remain parser-owned structured syntax, with an
implicit void or explicit typed result and a retained macro declaration identity. They are not
text replacement directives consumed by `SourceLoader`. Names, generic
parameters, exact typed value parameters, defaults, constraints, attributes and
braced bodies use the ordinary parsed syntax; semantics owns eligibility and
expansion. Ordinary call syntax carries no separate macro invocation sigil.

Expression parsing also retains a half-open byte extent for each complete
written assignment-expression and full comma-expression. This syntax-owned
`Expr::writtenOrigin` shares one immutable extent across ordinary clones and
includes interior trivia and closing syntax even when an AST node does not
otherwise need those tokens. Ordinary clones preserve that
origin. Whole-expression substitution preserves the replacement's origin;
cloning a parent whose children are substituted invalidates the parent's
origin, because the resulting composition was never written as one expression.
Synthetic syntax starts without an origin. Consumers require an available
matching `SourceManager` buffer instead of reconstructing source from AST names.

## Source Loading

`SourceLoader` is declared in `include/gti/source_loader.h`; its canonical path,
import-resolution, cycle, provenance, package, graph-recursion, and
source-manager algorithms are compiled into `gti_compiler` from
`src/compiler/source_loader.cpp`. The private `LoadedLexicalUnitLoader` owns
one unit's file/overlay byte acquisition, stable digest, lexical cache lookup
and publication, completion lexing, tokens, and exact lexical diagnostics. It
has no import or graph authority. The private incremental
`ConfigurationDirectiveProcessor` owns conditional frames, target/flag
selection, `#define`/`#undef`, inactive spans, configuration tokens, and their
existing diagnostics.
`SourceGraph` traversal/mutation and `SourceManager` location work compile in
`src/compiler/source_graph.cpp` and `src/compiler/diagnostic.cpp`. The loader
canonicalizes source identity, lexes each unit, rejects GTI `#include` with
`GTI-I0014`, resolves imports and target/configuration conditions in source
order, and builds `SourceGraph`. Rejected include directives do not load their
target. `SourceLoader` advances the directive processor one lexical step at a
time, retaining graph ownership. The initial valueless flag set comes from
`FrontendOptions`; active source-local `#define`/`#undef` directives update it
and are removed before parsing, without substituting tokens. Imported modules
start from compiler-supplied flags and restore the importer's flag state on
return. Definitions in an inactive branch do not leak. The loader records the
source-ordered value on each `defined(NAME)` token while retaining every branch
for the parser; semantic selection owns target comparisons and composition.

Quoted imports select relative GTI interfaces within the owning package.
Named package imports resolve only through the driver's immutable package graph
and direct dependency aliases. Package units retain stable identity and path
provenance but remain ordinary untrusted GTI source. Direct mode does not
discover manifests or synthesize package roots. Imports are load-once edges,
not textual substitution. The implicit prelude is an edge to every ordinary
unit. Source-written dependency edges retain their exact spelling and lexical
occurrence for deterministic, host-path-free failure metadata; these facts do
not provide an alternative resolution mechanism.

Ordinary `Frontend::analyze()` in `src/compiler/frontend.cpp` performs this load
itself. The compiled driver also exposes `loadCompilationInputs` for
project-cache orchestration. It still
uses `SourceLoader`, retains the resulting `SourceGraph`, `SourceManager`, and
source diagnostics as one snapshot, and either hashes that snapshot for a
verified cache hit or moves it into `Frontend::analyzeLoaded` on a miss. The
loaded entry point resumes the same parser/semantic/HIR/MIR pipeline; it is not
a second import resolver or frontend representation.

Source files are opened in binary mode. Before tokenization, the lexer validates
the complete source as strict UTF-8 and rejects a lone CR; this makes file,
overlay, direct-compilation, and LSP snapshots share one byte contract on every
host. An optional UTF-8 BOM is skipped only at byte zero. The source manager,
cache digest, token positions, and diagnostics still retain the exact written
bytes. LF and CRLF both advance one logical line, and display-line extraction
removes both bytes of a CRLF terminator. Invalid UTF-8 (`GTI-L0013`) or newline
spelling (`GTI-L0014`) produces one lexical diagnostic and an EOF-only token
stream so later phases cannot cascade. ASCII remains the complete identifier
and token alphabet; a valid non-ASCII scalar outside a comment or string is
grouped into one `GTI-L0015` span.

Each `SourceUnit` retains its path, stable exact-source digest, logical import
where applicable, tokens, token-granular inactive spans for editor
presentation, dependency edges, and final declaration range in the combined
transitional `Program`. Semantic
visibility uses the graph: a unit sees itself, the exported surface of its
imports and explicit re-exports, and the prelude, not arbitrary declarations in
the combined AST.

Explicit `export module net.http;` interfaces retain a canonical dotted module
name and an exact source name span. The loader consumes the header and resolves
unconditional imports, retaining `Import` and `ExportImport` dependency kinds.
Import tokens remain in the parser input so an import inside an unfinished
parameter or initializer cannot disappear into valid source. `SourceImport`
records preserve the written declaration/name spans, lexical segments, resolved
target, re-export flag, and unresolved or incomplete status for editor queries.

The immutable `SourceOwnerRegistry` separates public resolution and containment
from trusted compiler provenance. Packages, generated bindings, and configured
toolchain roots use the same owner-local public module maps and direct alias
edges. Quoted imports stay inside the same canonical owner, including private
helper modules; a path escape cannot bypass a dependency's public map. Direct
compilation remains manifest-independent and receives a direct source owner.
Exact local dotted names take precedence only when there is no competing direct
alias prefix; a collision is diagnosed rather than guessed.

The configured standard owner exposes the reserved implicit alias `std`. Its
installed `module-map.json` contains a `modules` name-to-relative-path object and
a `default` module name for `import std;`. Invalid names, absolute paths, and
canonical containment escapes are rejected; absent maps expose no public
modules. Embedding callers can supply a descriptor explicitly. Public names do
not confer compiler trust: canonical trusted standard paths still establish
`StandardLibrary` provenance independently. Logical owner identities and module
maps join build cache identity without embedding relocatable checkout roots.

An import target must declare an explicit module interface. The former
`ExportInclude` republication and per-module include-configuration replay paths
are removed; `#include` is recognized only to emit the removal diagnostic.

The parser retains an optional export token on declarations. Frontend assembly
collects their exact ranges, including inherited namespace exports in selected
configuration branches, from the fresh or cached AST. These are syntax requests,
not a separate symbol catalog.
Semantic analysis validates export eligibility and publishes each declaration
and overload through `SourceGraph::isDeclarationVisible`. Ordinary source
imports remain non-transitive; explicit re-export edges publish their imported
surface. There is no legacy unit republication path.
Definition-site generic context preserves private implementation lookup. A
public result can retain an inferred private nominal type without exposing its
name. Tooling consumes resolved semantic scope/occurrence facts rather than
filtering reachable members by module name. Initialization, failure provenance
and caches retain the ordered source graph, including import and re-export edge
kinds.
HIR/MIR and C++ emission use existing executable contracts; this is not binary
or separate compilation.

Registered `PackageSourceRoot::moduleImplementations` extend that closure after
ordinary imports finish. Each implementation must start with a matching
`module net.http;` header and be registered with that interface. It shares the
interface's canonical `ModuleId`, making its interface available implicitly
without granting access to sibling modules or nested-name prefixes. The graph
retains private implementation roots
for dependency-first compilation, without adding interface-to-implementation
visibility edges or an artificial import cycle. Only reached registered
interfaces activate their bodies; newly imported interfaces extend this
closure. Implementation bytes, configuration replay and overlays use the same
source snapshots as other inputs. No backend or LSP filesystem discovery is
needed.
After this closure is complete, the loader validates explicit imports with each
interface and its registered bodies grouped as one module. A dependency cycle
through different implementation files is rejected with `GTI-I0001`, even when
ordinary file loading has already finished both interfaces.

For `ReturnType Owner::method(...) { ... }`, the AST records the syntactic owner
path. Semantics matches one existing member and stores an immutable-syntax
declaration/definition association in its snapshot. `FunctionInfo::declaration`
remains the API/contract identity; `bodyDeclaration()` selects executable syntax,
body parameter bindings and definition-site lookup. Instance overlays resolve
the same association without mutating cached ASTs. HIR instances use that body
and the canonical FunctionId. LoweredProgram emits only the canonical member
declaration, seals its body association, and gives MIR the existing call,
ownership and cleanup contracts. The definition is not a second namespace
function. No generated C++ signature matching supplies GTI meaning.

Artifact builds set `FrontendOptions::lowerReachableOnly`. The complete program
is still analyzed, but MIR and everything after it receive only the instances
reachable from the program roots; see
[HIR](hir.md#reachable-executable-prefix). Artifact builds also set
`lowerLibraryReachableOnly`. HIR still lowers the entry unit and every
application or package unit completely, but lowers compiler-trusted
declarations only when the roots reach them. `gti check` keeps complete HIR
lowering. Unreached compiler-trusted bodies therefore avoid HIR lowering;
application and dependency-package bodies do not gain that exemption.
Optimization and executable emission use the reached artifact prefix, with
the retained declaration/physical-carrier dependencies documented in HIR and
backend contracts. Imported trusted units still cost semantic analysis of
every body they declare.

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
the dependency graph and resolves every import directive for every
snapshot, so the cache cannot bypass path, overlay, visibility, or active-branch
authority.

A lexical row whose unit has no configuration directive, lexical diagnostic, or
token-level loading diagnostic also carries that unit's processed form, which
replaces its raw tokens. No incoming flag set can change such a unit's selected
syntax. The processed form holds:

- the post-directive token stream, shared and immutable;
- its syntax digest and module declaration;
- each import directive's own tokens plus one token of lookahead,
  which is all directive resolution reads, and its brace depth.

Loading the unit again in a later snapshot re-resolves those directives in
source order. Their dependency edges and diagnostics, such as an unknown module,
are re-established. The unit's tokens are not copied or rescanned. Without this
form, every edit copied and rescanned the token stream of every loaded unit,
roughly 1.5 million tokens for `import std;`. Units with configuration
directives keep the full processing path.

`SourceGraph::compilationOrder()` supplies the current dependency-first parsing
and combined-AST assembly order. It is not runtime program-initialization
authority. `SourceGraph` separately retains configured prelude roots in their
exact input order; `clear()`, copying a loaded snapshot, and
`Frontend::analyzeLoaded` preserve or reset that vector together with the
entry, units, and dependency edges. Invalid or stale root identities fail
closed rather than silently changing order.

Semantics consumes those facts to build the separate immutable
`ProgramInitializationPlan`: ordered prelude roots precede the entry walk,
explicit import edges are followed in lexical dependency-first order with
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
keywords. Source loading validates module import components independently
from ordinary declaration identifiers.

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

`FunctionDecl::unsafeKeyword()` retains the written `unsafe` declaration
modifier separately from contract attributes. The parser admits it on GTI free
functions and ordinary non-polymorphic instance/static methods, in
`unsafe static constexpr` order before the return type. `MemberRequirement`
separately retains an optional unsafe keyword before its compound signature;
semantics propagates that effect through structural requirement facts.
The parser rejects the obsolete `[[unsafe]]` spelling with a
spelling-only migration diagnostic while retaining a recovered declaration
for tooling. Neither syntax form grants implicit permission to the body;
semantics owns the caller contract and each lexical unsafe operation.

Qualified namespace declarations (`namespace outer::inner { ... }`) expand
directly into the same nested `NamespaceDecl` nodes as explicit blocks. Each
component retains its original name token and a declaration extent ending at
the shared closing brace; the outer extent also includes `namespace`. Every
component consumes the existing nesting-depth budget, so shorthand cannot
bypass the parser's bounded recursion contract. Semantics, export visibility,
HIR/MIR, and tooling therefore use ordinary nested-namespace facts, without a
second namespace representation or backend interpretation of source spelling.

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

`ConceptDecl` retains every written type parameter and a precedence-resolved
constraint-expression tree over its concept applications. `FunctionDecl` and
`ConstructorDecl` optionally retain the same tree in a `RequiresClause`
together with the `requires` token. `&&`/`and` binds more tightly than
`||`/`or`, and parentheses change the tree shape. Concept applications retain
`TypeRef` arguments so semantics can distinguish visible generic parameters
from closed exact types. The parser owns only this bounded Boolean grammar;
concept identity, arity, argument closure, common-fact derivation, and call
viability belong to semantic analysis.

`ArrayInitializer` is reused for fixed-array declarations and for braces in a
call argument list. The parser records only the written elements; it does not
invent an element type, an extent type, or an initializer-list object.
`ConstructorDecl` retains constructor-local generic parameters after the
constructor name and its optional trailing requirements before an initializer
list. Semantics owns their nested scope, exact inference, constraint
validation, and whether a constructor value parameter is inferable from one
fixed-array context.

`FunctionDecl` also retains one receiver mode. Non-static members are read-only
by default, use trailing `mut` for mutable access, or trailing `&&` for owned
consumption. Consuming ordinary methods and admitted operators are general
source facilities, not an `operator()`-only exception. The parser records that
mode rather than treating `&&` as a general reference type, rejects it on
free/static functions and with a conflicting `mut`, and leaves structural
eligibility and escape rules to semantics. Constructors retain their separate
special-member move-policy syntax.

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
Recovery nested inside a compile-time branch propagates an unconsumed
`#elif`, `#else`, or `#endif` boundary to the nearest conditional parser. A
continuation directive with no open conditional is instead advanced as invalid
input, so every synchronization loop either consumes input or unwinds to its
owning grammar production.

Completion-marker parsing may retain a variable declaration when its
initializer consumed the marker before the user wrote the trailing semicolon.
That exception is local to the dedicated completion snapshot: ordinary parsing
still requires the terminator, and a marker consumed by the declaration's type
or name does not suppress the syntax error after its initializer.

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

- Change source dependency behavior in `SourceLoader`/`SourceGraph`, not the parser or
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
