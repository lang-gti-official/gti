# Frontend: Sources, Lexing, Parsing, And AST

Status: Current implementation.

The procedural macro frontend recognizes `#macro(proc)` and the explicit
`name!({...})` invocation. This spelling was selected by the owner during RFC
0001 implementation. All macro invocations now require the marker; typed generic
calls spell `name<T>!(...)`, while ordinary functions keep `name(...)`.
`SyntaxCapture` retains immutable tokens, exact input text, source spans and
text-relative token ranges. Brackets, braces and parentheses balance using GTI
lexical rules; embedded-language structure is not a frontend grammar.

Before directive processing or processed-unit replay, the source loader rejects
directives/imports inside any explicit macro invocation (`GTI-I0015`). A
rejected unit publishes no directive effects or dependency edges. Ordinary
braced calls and ordinary configuration directives keep their existing behavior.
The parser retains capture ownership in `Call`, and AST cloning remaps token
expansion identities without losing source/trivia. The formatter preserves
opaque groups, including incomplete editor input. Semantics executes completed
imported bodies through verified MIR and checks their sealed expression output.
The external HTML package exercises this path with transitive helper imports.
Processed-unit reuse preserves exact source/configuration identity; semantic
expansion reuse never crosses a new target/configuration snapshot. The project
cache includes the library and transitive helper source graph, so body, input,
trivia and helper edits cannot reuse a stale executable. See
[RFC 0001](https://github.com/lang-gti-official/rfcs/pull/1).

The frontend converts one entry source and its dependency graph into a
syntax-preserving `Program`, then hands that owned tree to semantic analysis.

Typed `#macro` declarations remain parser-owned structured syntax, with an
implicit void or explicit typed result and a retained macro declaration identity. They are not
text replacement directives consumed by `SourceLoader`. Names, generic
parameters, exact typed value parameters, defaults, constraints, attributes and
braced bodies use the ordinary parsed syntax; semantics owns eligibility and
expansion. A marked `Call` retains the `!` token and both raw capture and parsed
expression arguments. The parser attempts the typed argument grammar under its
existing nesting budget, retaining any errors on that alternative. Semantic name
resolution selects typed arguments or opaque procedural input before argument
analysis; raw input never executes parser or loader directives. This keeps typed
macro substitution structural even when macros invoke other macros, without
requiring parser knowledge of imported declaration kinds. An unmarked call to a
resolved macro is rejected with a targeted marker-insertion fix.

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

Canonical interface paths remain the owner registry's authorization and identity
facts. Separate authored lookup paths retain the declared spelling through
symlinks. Import records expose both paths: the authored path is only a filesystem
notification key, including when a target is missing or rejected. Loading always
revalidates canonical ownership; notification metadata grants no source access.
Editors retain prior observed dependency keys only for unchanged root text during
incomplete recovery, so deleting and recreating a registered interface refreshes
diagnostics without retaining stale semantic output.

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

Semantic catalog copies own their callable records and rebind every overload
candidate to those copied records, including visible ambiguity alternatives,
class members, protocol operations and enum symbols. Copied body-flow scopes
and indexes use the receiving catalog as well. Completing an interface prototype
in a resumed request therefore updates only that request; it cannot mutate the
cached preamble or retain a candidate pointing at the preamble's incomplete
record. After registration resumes, declaration indexes refresh from the complete
catalog while existing variable flow facts retain their declaration identity.
Namespace globals established during prefix body analysis may be absent from
that catalog; their owned entries and visible ambiguity candidates are retained
explicitly. The ordinary namespace declaration checks validate these prefix
globals against newly registered names without replaying initializers or moving
mutable value facts into the catalog. If a new declaration invalidates a prefix
global, the visitor discards the complete resumed request and analyzes afresh;
no initializer, effect, value or journal facts survive that fallback. The cached
prefix remains immutable and reusable by later valid edits. This keeps added
overloads, declaration collisions and ambiguities equivalent to uncached analysis.
Resuming also starts fresh raw-result and complete-return query journals: their
mutable summaries, budgets and pooled worker catalogs belong to one request,
so a prefix-only worker cannot be reused after the declaration catalog expands.
A prefix that produces semantic errors is not cached: it may be incomplete
without a later implementation body, as with a separate constexpr definition.
The frontend then analyzes the complete program and publishes only those
diagnostics, rather than preserving provisional prefix errors.

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

Executable entry validation, library-export selection and test-dispatch
synthesis run after parsing, so their policies do not partition lexical or
parsed cache rows. Opening an executable root and an imported source fragment
can reuse the same unchanged library syntax. Semantic reuse identities still
include these policies, and each analysis applies its own program request.

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

Lifetime syntax follows that same division. `GenericParameter::lifetimeKeyword`
retains contextual `lifetime` in function, record, alias and concept binders;
the lexer continues to classify its spelling as an ordinary identifier.
`ConceptDecl::genericParameters()` preserves parameter kinds, while its
`typeParameters()` compatibility projection contains only ordinary type names.
`TypeRef::referenceLifetime` holds the `<`, argument and `>` tokens of
`T&<a>`, separately from the referent's generic arguments. Names stay unresolved
in generic argument syntax; `_`, `static` and `this` have an explicit lifetime
argument discriminator. Mutable reference type arguments preserve their `mut`
token in the existing reference-access field. No syntax node decides whether a
region is in scope or whether a borrow may escape.

`for<lifetime a, lifetime b> R(Args...)` retains a `LifetimeBinder` on the
function-signature `TypeRef`, with each declaration token and both delimiters.
It scopes over the whole signature, including its result. The parser requires
plain named lifetime parameters and a function-signature target; semantics
owns quantification and compatibility. `this` is admitted as a reference or
generic lifetime argument, including in `outlives<this, a>` requirements, and
semantic method context gives it its receiver meaning. Ordinary `this`
expressions, `for` statements and `for<...> concept` conformance declarations
keep their existing syntax. AST cloning remaps all lifetime and binder tokens;
debug printing preserves their written relationship. Malformed lifetime
suffixes and binders recover through the ordinary declaration boundaries.

Semantic registration normalizes nominal lifetime arity before resolving
generic defaults, aliases and callable signatures. Omitted regions in stored
fields, inherited bases and enum payloads become independent declaration
parameters in source order. `GenericParameterInfo::implicit` distinguishes
these slots from positional source arguments, including around type packs.
Aliases preserve their binder identities across projection refresh. The
catalog retains normalized stored types; body analysis substitutes those
types rather than reinterpreting their omitted annotations. Recursive borrowed
declarations that would expand anonymous arity require explicit lifetime
parameters and arguments. Callable type elision has its own higher-ranked
scope and does not add parameters to the containing record.

Class instantiation scheduling includes projected lifetime binders and argument
patterns, even when a declaration has no ordinary type or value binder. This
classification propagates through body/effect reanalysis, HIR discovery and
lowered declaration/cleanup metadata. It does not add public generic arguments:
`Fixed<int&<a>>` still projects `a` from its single reference-type argument.

Constructor initializer summaries connect inputs to anonymous stored regions
through exact field and enum-payload identities. Explicit variant construction
retains its guard at each nested enum subobject; only a contradictory selected
guard excludes an inactive dependency path. A missing source dependency does
not prove an alternative inactive. These declaration summaries supply input
relationships to the existing lifetime normalizer, while ordinary body
analysis still proves initialization, access, movement and conversion. Named
owner lifetimes continue to require declared relationships.
Constructor signature normalization allocates its declaration binders once.
Initializer-derived relationships for anonymous stored regions are added after
the class field identities are reserved, before contract validation and
publication. This pass preserves explicit bounds and exact class/constructor
generic scopes; it does not allocate symbols from read-only path queries or
recreate normalized input lifetimes.
Conditional nominal wrappers publish each initialized field's exact paths and
alternative domains, including fields nested through other ordinary records.
The shared coverage check requires a selected alternative for every active
typed enum subobject and every independent sibling. Empty variants contribute
domain evidence without an invented origin. Concrete constructor body queries
still prove all active dependencies; unresolved generic recipes alone cannot
certify a complete result. Exclusive sources cannot be duplicated into fields
whose guards can hold together.

Enum reference patterns form an inferred binding region. After validating the
exact loan, a physical payload borrow records the subject storage's relationship
to that region, just as a reference local does. Reborrowing a stored reference
instead records its external referent's declared region; the enum shell does
not supply that lifetime. Both paths preserve nested carrier regions and their
existing access, guard, parent-loan and cleanup obligations.

Stored-dependency records share `BorrowPath`: a rootless sequence of field,
direct-base, and constant-index projections. A base records its declaring owner
and the full direct-base ordinal, including interface entries. Shape validation
rejects body-only dereferences/dynamic indices and mixed selector metadata; the
exact instantiated type walk must separately prove direct field identity, base
storage and index bounds. It never recovers a missing base step by searching
inherited fields. The structural overlap query preserves ancestor overlap and
treats unequal bases conservatively. Raw-pointer audit places preserve the same
base steps while retaining their separate body-only dereference and dynamic
index operations.

Reference and alternative inventories share one bounded work budget per query
(4096 entries, with a depth limit of 256). A failed or exhausted query is unknown,
not an empty dependency set. Independent enum subobjects keep separate guarded
domains without forming a Cartesian product of all sibling alternatives.
Selected base-initializer facts capture complete producer evidence immediately
after argument checking and lifetime application. Enclosing constructor summaries
consume that snapshot; later body flow cannot reconstruct or revive moved inputs.
Inherited storage requires exact constructor publication, independent
program-aware MIR replay and cleanup validation before code generation; the
structural path itself grants no lifetime, access or escape permission.

A shared reference copied from an inline field of a local cleanup-bound carrier
can retain that field's independent external suppliers. The return check uses
the evaluated expression's exact place and the direct typed storage inventory,
then checks every selected supplier's original shared parent chain. It rejects
owned/local referents, exclusive parents, reference crossings, unknown paths and
shortened fields that cannot satisfy the declared result region. The ordinary
field read still names the carrier's Stored group; neither that group nor its
other fields lose their cleanup obligations. MIR must independently capture the
selected reference before retiring the carrier. This rule does not open the
contents of a referenced object or make a lifetime-owner place an address.
A fixed `static` reference result can obtain its exact global supplier from an
available nonvirtual GTI source definition. Declaration analysis defers only that
missing supplier metadata: every return still proves its region and access,
and body admission requires a complete summary naming one exact global place.
Concrete generic reanalysis retains that checked global summary when structural
signature derivation finds no formal or receiver supplier; its compact origin
and complete supplier identity remain consistent.
An unavailable definition, differing global suppliers, or an unknown/cyclic
summary cannot acquire origin authority merely from the `static` result type.

A method call through a checked reference applies that reference's outer region
to the receiver contract, including retained references and reference-returning
expressions. External lifetime suppliers do not replace the receiver's shell.
Fresh owned receivers retain a separate evaluated materialization witness, and
synthetic calls with an explicit receiver place retain that exact place. This
region application does not establish the referent's stored contents.

Checked-reference contents remain staged. One explicit semantic adapter reuses
an entry reference's separately proved Stored fact or records an evaluated
reference, its full type and active formation read loans. Direct Get projection
and receiver/argument summary inputs share that adapter; query consumers never
create authority. New openings currently require a complete unconditional
shared inventory, retain tagged sources instead of lifetime-owner places, and
keep formation reads separate from lasting external parents. Writes, joins and
loops invalidate reuse of the current contents without revoking copied shared
snapshots. The common call-effect boundary also invalidates current contents
after selected input and result facts have been captured, including mutable
receivers and unknown, global or raw writes. This conservatively includes
readonly calls until a complete no-write effect proof can preserve their
snapshots. Expression reanalysis clears only the old opening index; already
captured shared facts remain immutable, and expressions without openings add no
index entries. Opening indices belong to the body's loan table: concrete instances
start fresh, while speculative analyses copy both together. Production
formation hooks remain disconnected and HIR/MIR reject
opening records until independent instruction-level replay is implemented.
Call-result address selectors, exclusive/conditional opening, and compact
recursive supplier closure remain prerequisites; an unknown or exhausted
query never establishes empty contents.

The shared callable contract derivation obtains each nominal application's
formation formula from the owning phase. Semantics supplies the instantiated
declaration contract; MIR supplies its verified nominal view. Inputs and
results retain the complete Boolean formula, including requirements not
implied by stored fields. A nested callable validates its own invocation
scope without adding those premises to its enclosing signature. Fixed carrier
regions are separate from invocation quantifiers, and `static` remains a
constant. Missing nominal metadata, foreign region identities and malformed
formula nodes make derivation fail rather than weaken the contract.
Mutable and consuming function-signature qualifiers remain part of the exact
callable type during formation. GTI invocation pointers independently require
a read-only signature; native ABI pointers cannot carry receiver qualifiers.

Generic callable patterns derive transient scope captures from their stored
`genericArgumentPatterns` recipe and the exact actual argument. A projection
containing an invocation binder stays in `GenericSubstitution::callableScopes`,
separate from ordinary type substitutions. Reconstructing a callable closes
those binders; a method or local lambda opens the selected scope with fresh
universal identities. Field and alias uses cannot expose a captured binder
outside that scope. Raw substitution keys retain complete scope identities,
while proof comparisons alpha-rename lexical binders and keep fixed environment
and imported invocation choices exact. Declaration refresh restores the owning
pattern context; concrete reconstruction still must resolve elision and prove
nominal formation before publication.

Native function-pointer signatures use this same invocation scope. Elided
regions in their inputs become invocation quantifiers, rather than stored
borrows of the pointer value. GTI target adaptation records the requested
callable contract and proves that it implies the target's requirements;
indirect calls instantiate and check that contract through the ordinary
lifetime application. The native type, ABI, unsafe-call rule and thread
requirements remain authoritative throughout adaptation and invocation.

Private body-derived lifetime contracts use a declaration dependency worklist.
Every eligible body is initially probed with the ordinary semantic analyzer;
changed bounds requeue the declaration and the callers that read its contract.
After the initial probes, dirty strongly connected components run in dependency
order, with callees before callers and deterministic order inside a cycle.
Ranks refresh when the read graph grows; transitive query reads remain in the
graph rather than being discarded to reduce probe counts.
The read set includes unsuccessful overload and concept trials, function-item
formation, and inherited model lookups. Nested probes share only that read
trace, and reads are conservatively retained across iterations. This reaches
the same finite fixed point without rechecking unrelated bodies after each
change. Provisional body facts remain isolated, and ordinary strict body
analysis validates the finalized contracts before IR publication. Frontend
work metrics count candidates, body probes, changed summaries and dependency
edges separately from elapsed time. An unjournaled cached semantic proof makes
its consumer depend conservatively on all mutable private contracts; metrics
count those opaque probes so lost precision remains observable. Primitive-only
expression proofs contain no function reads. No trusted-body validation is
bypassed, and cached outcomes keep their existing admission rules.
Staged constant and procedural-expansion keys retain complete generic lifetime
substitutions and the inferred-contract generation, so a changed declaration
contract cannot reuse a previously checked execution closure or expansion.
An early private probe can demand an earlier constexpr definition before the
primary lexical body pass reaches it. A bounded prerequisite query checks that
definition strictly in its own source context, with lifetime collection off;
only a successful checked definition becomes available in the isolated probe.
Its selected constexpr closure still executes through the existing staged
HIR/MIR service. Staging-only source-availability callbacks authorize fresh
concrete semantic models, including for nongeneric callees; HIR never reads
missing body facts from the primary registration model. Lazy discoveries use
the original required-constant source boundary, which also participates in
the staging result and worker identities. Prerequisite proofs retain dependency reads and the exact
contract generation, preserve source declaration order, and cannot publish a
provisional recursive proof or complete the primary program's body pass.
Successful immediate value or void substitution withdraws that expression's
runtime lifetime facts with its existing runtime effect slice. The snapshot
belongs to the exact enclosing body and preserves every earlier origin and
constraint; semantic argument checks and checked constant execution precede
withdrawal. HIR therefore does not need origins for erased temporary receivers.

Nominal variance is derived from verified fields and bases by a finite fixed
point over the variance lattice. Shared references and owned storage preserve
variance; mutable reference referents and unproven raw storage are invariant.
Compiler-owned storage relaxations use trusted declaration capability identity.
Ordinary mutation through an exclusive receiver does not imply interior
mutation. `NominalVariance` records compact type and lifetime descriptors, and
the semantic instance provider expands a trailing type-pack descriptor for
each concrete argument. HIR and MIR preserve this result rather than deriving
variance from layout or emitted code.

The same declaration normalizer collects nominal well-formedness contracts
from finalized storage: a stored `T&<a>` contributes `outlives<T, a>`, while
opaque projections remain type obligations. This stage cannot add lifetime
arity. Exact specializations inherit the primary's normalized region shape;
selection uses structural type/value identity without region identity, and the
selected semantic type retains its actual lifetime arguments. A specialization
cannot add anonymous region slots or strengthen formation obligations. Public
lifetime variance is conservative across all definitions in the family, so an
open primary cannot promise a conversion that a selected layout invalidates.

Defaulted copy/move policies are checked after nominal contracts settle. The
exact-self declaration context first resolves omitted owner regions to the
current owner; written regions remain exact and the source shell is excluded.
Explicit and elided declaration provenance carry the same obligation proof,
while body-inferred contracts cannot authorize a structural policy. The
shared structural-policy predicate derives the source-reference signature
from the exact owner declaration, retaining its fixed regions and adding one
independent shell binder. It compares the complete normalized `All`/`Any`
alternatives, including formation of nested nominal type arguments. Missing or
stronger facts, correlated/fixed shell regions and unsupported predicates fail
the declaration check. Accepted policies select the existing structural
copy/read or move/value operation; their declaration identity is retained, but
no fictitious callable target or invocation application is published. Whole
value lifetime, liveness and cleanup checks continue to govern the transfer.

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

Function optimization annotations `[[inline]]`, `[[noinline]]` and `[[cold]]`
reuse the function-attribute AST list. The parser accepts the otherwise reserved
`inline` token only as an annotation name, and rejects attribute arguments.
Semantics validates eligible source-defined function/method declarations,
duplicates and conflicting requests, then publishes `FunctionCodegenAttributes`
independently of return-flow, ownership and ABI contracts. Concrete HIR and MIR
instances preserve that record; the LLVM backend consumes verified policy.

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
Ordinary construction and direct-base initialization share constructor candidate
selection, including generic inference, lifetime application, constraints and
conversion ranking. Forwarded concrete packs contribute exact flattened input
lanes to a fixed-arity target. Selected base records retain constructor type,
value and lifetime arguments; omitted defaults and program effects use the same
flattened count. Plain defaults remain checked in their declaration scope, and
S2077 continues to require borrowed boundaries explicitly. A checked base
producer can be normalized back to the enclosing constructor's exact formal
binding paths without repeating declaration-only overload selection.

Semantic, HIR and MIR formal inventories derive flat argument ordinals from
grouped parameter bindings through one bounded structural helper. Only concrete
pack nodes expand; a reference to a pack remains one reference. Repeated element
types retain distinct constant-index paths. The inventory proves correspondence
only: active entry loans, permitted access, argument exclusivity and complete
result dependencies remain separate checks.

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

Compile-time recursion is not bounded by written syntax. Nested
constant-evaluation calls (staged MIR and the compatibility interpreter),
nested staged constexpr analysis, macro expansion, and the HIR and MIR
lowering of expanded macro bodies recurse once per level up to the configured
[compile-time limits](../language/static-semantics.md#3112-implementation-limits).
Each of those recursion boundaries checks the remaining native stack
(`nativeStackNearlyExhausted` in `include/gti/support.h`). Below a 1 MiB
headroom (4 MiB in sanitized or unoptimized builds) the work continues on a
fresh 8 MiB (32 MiB) guard-paged stack segment of the same thread and returns
to the original stack when it completes. A small switch routine for x86-64 and
AArch64 moves only the stack pointer, so thread-local state, crash recovery,
time tracing and cancellation are unaffected, and exceptions thrown on a
segment propagate to the caller. Depth is therefore a language limit, never a
property of the host thread: one level of nested staging or macro expansion
measured 64 KiB of stack in an optimized arm64 build (160 KiB under
AddressSanitizer), so the default limits alone would need tens of MiB.
`testNativeStackSegmentsAreInvisible` forces a switch at every boundary and
requires byte-identical MIR and diagnostics.

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
