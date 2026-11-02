# GTI Language Server Architecture

Status: Current implementation, with future symbol/index work explicitly
separated below.

Associated type families use compiler-owned protocol member and input-binder
symbols. Declaration, conformance definition and projected member uses share
the exact contract identity; renamed family inputs retain their own independent
declarations. Hover uses the application occurrence's semantic type, definition
targets the protocol contract, and type completion after `C<P>::` offers its
declared members with their parameter lists. Alias validation publishes these
occurrences after conformance registration, avoiding stale early projections.
Tree-sitter marks each family declaration/definition as its input scope; the
formatter recognizes the parameter clause before a `using` definition's `=`.
No protocol-side family lookup or substitution is introduced.

Generic payload enum variant hover and completion use exact occurrence and
candidate types with the retained semantic instance signatures. Defaults and
transparent aliases print the same canonical owner and substituted payload
types. Definition and documentation still target the primary declaration.
Generic binder identities, optional `class` spelling and dirty dependency
overlays follow the existing snapshot/navigation protocol; the LSP does not
perform generic substitution itself.

Since 0.449.0 the source-defined global `expected` class and `unexpected`
function use ordinary type/function symbols, generic syntax, and call
occurrences. Neither spelling is an editor keyword or builtin-type exception.
Tree-sitter uses `user_type` and `call_expression`, while semantic highlighting
and navigation continue to follow the compiler-selected library declaration.
Formatting uses the same generic-type rules as unrelated imported classes; it
does not reserve these identifiers.

### Toolchain selection in Neovim

The opt-in `vim.g.gti_toolchain = "gtiup"` integration delegates installation
and channel selection to the toolchain manager. Its release prefix contains
matching compiler, LSP, standard library, runtime and Tree-sitter parser; the
editor never downloads the LSP separately. `gtiup which gti_lsp` runs from each
client's project root before the RPC process starts. Existing `gti.toml`, `.git`
and `CMakeLists.txt` root-marker priority is preserved; `gti-toolchain` is a
fallback marker, so an inherited workspace selection does not collapse nested
projects into one LSP root. The real selected binary,
rather than a proxy's location, determines the matching parser path. Explicit
`GTI_*_PATH` overrides continue to take precedence. A failed managed selection
never falls back to a plugin-local or arbitrary PATH compiler.

The legacy self-installing mode remains compatible. Managed mode disables its
build hook and PATH prepend, and does not compare the compiler's semantic
version to the plugin checkout's VERSION. `GTIInfo` reports the manager and
selected binaries; compiler/LSP version disagreement remains visible.
Tree-sitter parsers are process-global in Neovim: after one selected parser is
loaded, buffers selecting another parser fall back to regex syntax with a
warning. Query compatibility is checked before enabling structural highlights.
Separate editor instances can use different grammars; separate LSP clients
can already use different toolchains within one instance.

The LSP is an adapter over compiler-owned semantics:

Typed macro declarations and ordinary-looking calls retain compiler-selected
declaration identities. Hover/signature text prints `#macro`, preserving an
explicit result type (including written `void`) and omitting only the generated
implicit `void` type. Definition
navigation and exact overload selection come from the same frontend records;
the protocol does not infer expansion semantics from `#` spelling or names.
Incomplete signatures, legacy-attribute migration and mixed macro/function
overloads publish ordinary compiler diagnostics through current snapshot and
UTF-16 conversion rules.

### Read-only macro expansion inspection

`LanguageQueries::macroExpansion` selects a retained checked macro invocation
by its parser-owned source range and reads `SemanticModel::macroExpansionSites`.
It renders substituted argument expressions, nested expansions, hygienic local
bindings and control flow as explanatory GTI. Display-only local labels
`name__macro_local_N` follow checked binding identities, not equal identifier
text. The display is not executable source, an ownership proof or a backend
input. It never triggers concrete semantic analysis or changes the snapshot.
Calls with no retained expansion, or several distinct concrete expansions for
one written invocation, return no result instead of guessing an instance.
Less-common argument syntax not represented by the display printer is marked
with an explicit omission comment. Output is deterministic and capped at
16 KiB, 4096 visited nodes and 64 levels; truncation is explicit and UTF-8 safe.

The server advertises
`experimental.macroExpansionProvider = { method: "gti/inspectMacroExpansion",
readOnly: true }`. The custom request takes normal `textDocument` and UTF-16
`position` fields, with an optional `textDocument.version`. Its result is
`null`, or owned `uri`, `version`, invocation `range`, declaration location,
`languageId: "gti"`, `readOnly: true`, `text` and `truncated` fields. It shares
the existing semantic snapshot queue, dirty dependency overlays, cancellation,
close/reopen and generation rejection. A supplied stale version produces LSP
`Content modified` (`-32801`); no source edits or executable commands are sent.

Neovim's `:GTIExpandMacro` requests this view at the cursor and opens an
unmodifiable, read-only `nofile` scratch split; `q` closes it. The adapter
discards responses when a source/dependency buffer changed while waiting.
There is no automatic replacement of a macro call with its displayed text.
Definition navigation targets the written binder of a macro-local variable;
references and rename join only exact written declaration identities across
retained expansions, without including same-spelled caller or shadowed names.

Concrete static methods on interfaces use the same compiler-owned method
symbols, selected calls and qualified completion as static class methods.
Hover and definition retain the exact declaration identity; semantic tokens
mark the function static rather than treating it as a virtual requirement.
The LSP publishes the frontend's interface/static restrictions, including pure
or virtual static declarations and instance method bodies, without
reconstructing those rules in the protocol layer. Bodyless static declarations
follow the ordinary declaration-snapshot contract rather than becoming abstract
requirements: editor snapshots can retain them, while closed artifact requests
diagnose a selected missing implementation with `GTI-S2106`.

Local `auto&`/`mut auto&` declarations publish the compiler's concrete reference
type and access in inferred-type hover. Uses retain the reference binding's
definition identity. Switching between value/reference spelling, incomplete
initializers and unsaved owner declarations is handled by the existing snapshot
generation and dependency invalidation, not protocol-side inference.

```text
JSON-RPC/LSP
  -> protocol conversion in src/lsp/main.cpp
  -> open-document state and generation checks
  -> exact frontend source snapshot / bounded parsed-unit cache
  -> immutable FrontendResult snapshot
  -> LanguageQueries / SemanticDatabase
```

Missing-definition diagnostics from concrete generic instances retain the
frontend's outer requesting call as primary, including calls through an included
library or a constructor. Related locations still identify the selected internal
call and its bodyless declaration. The LSP performs only source URI and UTF-16
range conversion; it does not infer caller attribution from public type names.

Protected sections use the lexical keyword token and ordinary formatter/Tree-sitter
syntax. Compiler-owned completion filters fields and each selected overload with
the same declaring-class and static receiver rules as source analysis; base and
sibling receivers do not advertise inaccessible protected members. Hover and
navigation retain the original declaration identity, and `GTI-S2007` retains
use/declaration spans through ordinary snapshot and UTF-16 conversion.

## Code Snippet Completions

`LanguageQueries::complete` combines semantic candidates with a small static
syntax-template catalog in `src/compiler/language_snippets.cpp`. The real parser
records the completion marker's declaration, ordinary class/struct member, or
statement boundary and its enclosing type name. The frontend retains this
editor-only value, including on parsed-unit cache hits; it contains no semantic
IDs or AST pointers. The catalog does not infer scopes from text, resolve names,
or add syntax. No additional parse or dependency load is needed for snippets.

Declaration templates cover classes (including a generic skeleton), structs,
interfaces, scoped enums, namespaces, aliases and free functions. Ordinary
class/struct member templates add owner-named constructors and cleanup bodies,
read-only/mutable methods and initialized fields. Statement templates cover
conditionals, indexed/borrowed-range loops, switches and unsafe blocks. Labels
are prefix-filtered, and exact semantic candidates rank ahead of templates.

The lexer recognizes complete keyword triggers only in completion-marker
scans. Ordinary lexing/parsing is unchanged, and completion markers are excluded
from comments, strings and character literals, including unterminated forms.
Expressions, member/qualified-name lookups, inactive code and unresolvable
source configurations do not receive syntax templates. The edit replaces the
whole current word, including a suffix after the cursor; the protocol layer
converts that compiler byte range to UTF-16.

Templates use LSP snippet items (`kind = 15`, `insertTextFormat = 2`) with tab
stops and linked placeholders, and are offered only when the client advertises
`completionItem.snippetSupport`. The compiler query opts into templates only for
these clients, preserving the plain client's full semantic candidate budget and
ranking. Existing semantic-call completions keep their
plain-text fallback. Generation rejection, cancellation, dirty overlays and
close/reopen use the existing completion worker; snippets introduce no separate
cache, worker, edit command or client-specific snippet format.

## Semantic Source Of Truth

Source modules use the same manifest module maps, source graph, export
publication, and re-export traversal as compilation. Symbol completion only presents
the candidates supplied by semantic lookup; hover and definition use resolved
occurrences. An inferred value of a private type can still present its ordinary
public members without exposing that type's name as a namespace candidate.
Dirty interface overlays invalidate consumers through the existing dependency
generation machinery, and closing an overlay restores the on-disk surface.
Registered implementation files are retained source dependencies as well.
Project resolution also supplies the default discovered module map. Saved `.gti`
creation, removal and header changes refresh project snapshots even when no
previous import edge named that file. A normal `textDocument/didSave` also
refreshes membership when the compiler's saved leading module declaration has
changed, including a newly saved file; this does not require watcher support.
Body-only saves retain their existing analysis instead of scheduling it again.
Membership is discovered from saved files;
unsaved contents of existing members still use ordinary frontend overlays and
dependency generations. An unsaved new/renamed module identity becomes available
to named imports after saving. Discovery performs no builds or dependency fetches.
Out-of-class bodies, in-class declarations and calls share one semantic method
symbol; definition navigation points at the body, while references include
both declaration and definition. Body parameter names belong to that body and
can be renamed locally. The existing conservative restriction on renaming
public/non-local symbols remains in force. The protocol does not independently
match signatures or load implementation files.
Unrelated semantic errors do not suppress module completion. Named-import
completion uses the source owner’s public module map and direct aliases,
including dotted logical names; it never scans dependency-private files. The
loader retains resolved, unresolved, and incomplete import facts with exact
name spans and a token-derived completion prefix. Hover, definition, and
references consume those facts and registered implementation membership.
Unresolved imports have no invented definition target. Semantic tokens classify
retained module-name segments as namespaces, including incomplete names; only a
resolved compiler-trusted standard target receives the default-library modifier.
Interface header names additionally carry the declaration modifier. These tokens
consume the loader's ranges rather than reparsing module syntax. Authorized lookup paths
and separate authored lookup paths remain notification dependencies even when an
interface is absent. Incomplete recovery retains only the last valid dependency
snapshot for unchanged root text plus current incomplete facts, bounding repeated
invalid retargets without retaining stale semantic output. Opening an unsaved
interface or creating it on disk refreshes consumers. Publication checks their
captured source generations to reject stale creation/deletion results. Protocol ranges convert
retained byte spans to UTF-16, including comments and non-ASCII text before names.
Module rename remains conservative; no textual path-rewrite fallback exists.

`FrontendResult` retains the recovered AST, `SemanticModel`, `SemanticDatabase`,
HIR/MIR, source graph, source text, and diagnostics together. The LSP retains a
`shared_ptr<const FrontendResult>` in each `AnalysisSnapshot`. This keeps AST
pointers and snapshot-scoped symbol IDs valid while a request reads them.
Compatible snapshots may share immutable parsed statement trees through the
frontend cache, and every snapshot constructs a fresh semantic model and
database. Symbol IDs, semantic records, and pointers into one semantic model
are never cache values, with one bounded exception: concrete instance deltas
of compiler-trusted declarations with library-only arguments, which a later
snapshot reuses only after validating every identity they carry (see
"Reusable frontend work").

`LanguageQueries` is declared in `include/gti/language_queries.h`; reusable
hover, completion, definition, and signature rendering compile in
`src/compiler/language_queries.cpp`. Semantic tokens use compiler symbol and
occurrence facts for identifiers and lexical facts for keywords, literals,
operators, and comments. Line and block comments are split into per-line LSP
tokens with UTF-16 lengths; delimiters inside literals are excluded. The
protocol layer must not repeat name lookup, overload selection, type rendering,
or declaration parsing.

JSON-RPC admits only valid UTF-8 document text. Compiler byte offsets are
converted through one source-position index at the protocol boundary; Unicode
supplementary scalars count as two UTF-16 code units, CRLF counts as one line
break, and an explicit leading BOM occupies one UTF-16 position before the
first token. The compiler remains the authority for BOM placement, ASCII token
spelling, and `GTI-L0013` through `GTI-L0015`; the LSP publishes those exact
diagnostics and clears them on the next valid generation.

Contiguous `///` blocks are normalized by the lexer as leading trivia on the
following declaration token. The parser transfers that Markdown onto the
declaration, and semantic analysis attaches the same value to the exact
`SymbolRecord`. Hover reads documentation through the occurrence's selected
symbol, so declaration, type, field, constructor, and overload-selected call
sites agree across entry files, included sources, and dirty dependency
overlays. The protocol adapter places the documentation after the canonical GTI
signature and does not search source text for comments.

Completion and signature help publish the same symbol-attached value.
Completion candidates carry each resolved symbol's documentation — a function
overload resolves its own declaration's symbol rather than the shared
overload-set scope entry — and signature help attaches the selected
callable's documentation through the call-site occurrence hover already uses.
The protocol adapter emits LSP `MarkupContent`, following the client's
declared `documentationFormat` for completion items and signature information
exactly as hover follows `contentFormat`; an undocumented symbol omits the
field. No consumer parses comments or stores documentation independently.

Compile-time conditions retain loader-resolved inactive token spans in each
`SourceUnit`. Semantic tokens apply the `inactiveCode` modifier to those spans;
the loader also retains the exact `defined` operator and configuration-flag
name roles it resolved. Flag names receive the LSP `macro` type even in an
inactive condition, while `#define` and `defined` remain owned by the more
precise Tree-sitter captures. Other directive keywords retain their existing
LSP `macro` classification. Other unresolved identifiers in inactive branches
receive lexical variable classification only, so an inactive branch can fade
without being semantically analysed or fabricating hover/definition results.
Clients that do not consume semantic-token modifiers remain unaffected.
Inactive punctuation also receives an operator token carrying `inactiveCode`;
active punctuation remains syntax-owned. The Neovim plugin supplies a
language-specific default `@lsp.mod.inactiveCode.gti` link to `Comment` and
reinstates that default after colorscheme changes without replacing explicit
theme/user styling. Its `LspTokenUpdate` overlay uses the server's modifier at
semantic-token priority + 3, above ordinary type/modifier colors. The native
semantic-token engine removes those overlays when a new result arrives.
The Neovim integration links GTI's language-specific macro token to the
client's generic `@lsp.type.macro` group, matching other LSP languages and
leaving the selected colorscheme in control of its color.

The `sizeof` and `alignof` reserved words use the compiler's operator token
classification, while invalid operands publish the shared `GTI-S2063`
diagnostic. The LSP does not evaluate layout or maintain a second supported-type
list.

Restricted arithmetic, remainder, bitwise, shift, compound-assignment, and
prefix-decrement overloads follow the same boundary. Tree-sitter and the LSP
classify their written punctuation as operators, while declaration contracts,
exact member selection, candidate locations, and no-synthesis diagnostics come
from the compiler snapshot. The protocol layer does not emulate C++ overload
lookup or maintain its own overloadable-operator list.

C++ core keyword spellings likewise use compiler lexical classification. An
invalid identifier use is published as the shared parser diagnostic
`GTI-P0002`, and the spelling remains a semantic `keyword` token while the
document is invalid. The protocol layer does not maintain its own reserved-word
list.

Canonical concept and callable rendering follows the same rule. Compiler
queries format multi-parameter concept applications and a selected function's
resolved trailing requirements from AST/semantic records; hover and completion
must not reconstruct `requires` syntax or capability meaning in the protocol
adapter.

Bounded member requirements render their compiler-owned
`requires(T& value) { { value.method() } -> R; }` signature in concept hover.
An explicitly unsafe compound requirement retains `unsafe { ... } -> R` in
that signature. Ordinary unsafe methods use the existing resolved function
contract for hover and unsafe-call diagnostics; no protocol-owned effect
inference is involved.
The synthetic receiver declaration and use share a `RequirementParameter`
symbol for definition, references, rename, and read-only parameter tokens.
That symbol is source-only: it does not declare runtime parameter storage or
relax the lowered verifier's rules for ordinary parameters. Malformed queries
and failed contracts use the shared parser/semantic diagnostics, and formatting
must preserve their meaning through an edit/reanalysis round trip.

Bounded constraint disjunction follows that same compiler-owned path.
Signatures render canonical `&&`/`||` precedence and only necessary
parentheses, selected calls substitute every application leaf, and a failed
disjunction publishes the shared `GTI-S2029` primary diagnostic plus related
alternative locations. The protocol adapter neither evaluates branches nor
reconstructs their common guarantees.

Class-scoped type aliases also stay on this compiler-owned path. Their
declaration and every resolved type use share one `TypeAlias` symbol identity,
so hover renders `using Owner::Name = Type`, definition targets the member
declaration, semantic tokens classify both declaration and use as a type, and a
hierarchical document outline nests the alias below its owner. Completion in a
type position asks semantic analysis for the accessible direct aliases of the
qualified class; the protocol adapter does not infer class membership from the
written `::` path. Tree-sitter accepts the alias as a class member and supplies
the syntax-level type-definition capture while semantic tokens retain the exact
class-versus-namespace meaning. Specialized owners such as `Box<int>::value_type`
retain their type arguments in the compiler AST. Completion uses that resolved
owner for alias substitution, and hover/definition/references/tokens keep the
alias declaration identity at every qualifier step. Incomplete specialized
paths use the existing completion-marker and parser recovery paths.

Conditional unsafe class capability assertions use the same generic-parameter
symbol as their declaration. Navigation, references and rename therefore cover
the attribute argument without protocol-side name matching. Class hover labels
the policy as conditional instead of suggesting an unconditional assertion;
invalid, misplaced and conflicting policies publish the shared `GTI-S2059`
diagnostic. Neither CLI nor LSP offers a fix that accepts an unsafe promise.

Function-declaration and selected-call signatures retain `[[noreturn]]`,
`[[track_caller]]`, `[[discardable]]`, explicit `[[borrowed_from(owner)]]` / `[[borrowed_from(this)]]`,
and `unsafe` from the resolved `FunctionInfo`. The borrow-origin parameter name
comes from the resolved formal parameter index, not from reparsing the attribute;
inferred borrow origins do not invent explicit annotations. Contract
attributes precede the `unsafe` modifier and then `static`/`constexpr`, when
present. Signature printers preserve that canonical order.
The LSP publishes `GTI-S2055` and its
related declaration span from the same frontend; it neither infers safety
requirements nor offers a fix-it accepting an unsafe obligation.
The keyword is highlighted as a keyword, not a decorator. The compiler's
obsolete-spelling migration and `unused-unsafe` diagnostics supply the same
current-version edits to CLI and LSP; the latter removes only a proven-unused
keyword while keeping its lexical scope.

C-style create/join, manual mutexes, conditions, and once initialization use that same public declaration path.
Their `GTI-S2084` profile requirement is derived by semantics from the resolved
private runtime call graph, and `GTI-S2055` retains the ordinary unsafe caller contract. The
editor publishes both without recognizing public `std::thrd_*`, `std::mtx_*`, or `std::cnd_*`
spellings or `std::call_once`; hover and definition select the ordinary wrapper in `std.cthread`.

Function-declaration and selected-call hover also append compiler-owned
callable-parameter contracts from `FunctionInfo`. Confined notes identify
read-only or mutable access and the deduplicated exact invocation signatures
with their read-callable, mut-callable, or once-callable requirement. Owned
notes instead identify the explicit ownership move and exact generic return or
owner-field destination. All types use the canonical semantic type printer.
Declaration hover retains the symbolic generic
contract, while
selected-call hover applies the compiler-recorded generic type and value
arguments so the same contract is rendered with concrete types and inferred
fixed-array extents. Declaration signatures print generic type/value defaults
from their semantic/AST records, including dependent value expressions;
selected-call signatures print the complete concrete substitution regardless
of whether each argument was written, inferred, or defaulted. Generic
signature-pattern class declarations print their public pattern, such as
`class Callback<R(Args...)>`, rather than flattening its extracted binders.
Binder occurrences retain their own symbol identities even when an outer
alias has the same name. Generic
constructor declaration hover retains its symbolic
local type/value parameters and requirements, while selected construction
hover substitutes the exact inferred arguments, requirements, and parameter
types from `ResolvedConstructionInfo`. Definition, references, and signature
help keep the generic constructor declaration's semantic identity. Method
calls also apply the concrete receiver specialization retained in
`ResolvedCallInfo::dispatchOwner`, including generic class type arguments.
Forwarding-only parameters are identified without inventing a signature. The
protocol adapter only serializes these notes. Hovering the parameter binding
itself remains ordinary binding hover until semantic occurrences retain its
owning function and parameter contract directly.

Checked-reference callable parameters use that same contract rendering: a
selected call retains its concrete `F&`/`mut F&` argument type. Deferred
conditional arm results are resolved by concrete frontend analysis, not the
LSP. An incompatible concrete join publishes `GTI-S2050`, and correcting the
document clears it without retaining a spurious `void` result or backend error.

A namespace name selected contextually as a GTI function item records the
exact function declaration symbol as an ordinary `Function` semantic
occurrence. Hover therefore shows the canonical selected function signature,
definition jumps to that declaration even when it appears later in source, and
semantic tokens classify the name as a function. Ambiguous or inexact item
formation publishes the shared `GTI-S2090` diagnostic. A unique eligible bare
name may also form a retained item without a required signature; an ineligible
function-value use retains `GTI-S2076`. The query and protocol layers do
not infer a function-item type, choose among overloads, or expose the backend's
private nominal tag.

Consuming public callback signatures likewise use ordinary semantic records:
binding hover retains `R(Args...) &&`, and missing explicit moves or reuse after
consumption publish compiler ownership diagnostics. The protocol regression
changes a valid wrapper invocation to each invalid form and back, checking that
the newest valid snapshot clears the errors. No wrapper-specific LSP policy is
needed.

Defaulted default constructors and destructors use those same compiler-owned
declaration identities. Declaration hover and document-symbol detail include
`= default`, selected construction hover presents the callable `Type()`
signature, and definition from the call reaches the written constructor. The
protocol layer does not infer lifecycle policy from punctuation.

Lambda capture targets and sources remain separate semantic symbols. Hover on
an environment binding appends the compiler-owned `immutable copy-snapshot
capture` or `owned move capture` note; definition from the initializer source
returns the enclosing declaration, while definition from the lambda body
returns the capture target. Semantic tokens and formatting consume those same
frontend records and written syntax rather than inferring capture mode from
punctuation in the protocol layer.

References, document highlights, and rename consume the same exact symbol
identity as definition: the occurrence under the cursor resolves to a
`SymbolId`, and `SemanticDatabase` occurrence records for that identity —
never equal spelling — produce the result ranges with their read/write roles.
Rename fails closed in the compiler query: only function-local identities
(locals, parameters, generic type/value parameters, lambda captures) are
renamable, because every reference to them provably lies inside the current
snapshot; copy-snapshot lambda captures rename together with their source
binding through recorded capture links; the new name must lex as a single
non-reserved identifier; a conservative visibility check rejects any new name
already spelled in the touched source units; and every edited range is
verified against the snapshot source before edits are produced. The protocol
layer only serializes the resulting spans.

Document symbols walk the recovered AST for the requested source unit —
namespaces, types, members, enums and enumerators, functions, globals,
aliases, and concepts — using the parser-recorded statement extents for
enclosing ranges and exact name tokens for selection ranges.
Compile-time-conditional declarations contribute the semantically selected
branch.
Each node's detail reuses the hover signature query, so the outline and hover
always present identical compiler-owned renderings.

Signature help reads the semantic database's call occurrences: the analyzer
records each selected call's and construction's byte-exact argument-list
geometry — the `(` and `)` offsets and every argument-separating `,` — from
parser tokens, and the query picks the innermost geometry containing the
offset, prints the compiler-selected signature with per-parameter labels,
and derives the active parameter from the recorded separators. Synthesized
calls carry no geometry and offer no help; a call whose occurrence cannot
be presented in loaded source is skipped fail-closed.

Signature rendering appends ` = <default>` to parameters whose declarations
contain a default expression. The placeholder deliberately avoids reprinting
source expressions while exposing optionality consistently in hover, outline,
and signature help; the protocol layer does not infer defaults from arity.

Builtin counted-view members implemented by trusted source factories preserve
one compiler-selected call identity. `ResolvedCallInfo::argumentReceiver`
normalizes the written receiver into argument zero for semantic effects and
HIR's ordinary ordered source call; it does not duplicate the receiver or
invent a backend member operation. The compiler's shared argument-access
queries fail closed for a foreign AST receiver identity. Hover, completion and
signature help present the public member name and written arguments, omitting
the private source factory's injected receiver. Normalization remains inside
the immutable frontend snapshot; the LSP protocol adapter performs no
name-based dispatch or argument-index repair.

The checked integer functions are ordinary `std.numeric` declarations.
Hover, definition, and completion therefore consume the same selected overload
and source-unit records as other standard-library functions; the protocol layer
does not recognize `checked_add/sub/mul` by spelling.

Out-of-range signed contextual integer operands likewise publish the shared
semantic `GTI-S2004` diagnostic and its numeric-token range. The protocol layer
does not infer signed literal values or repeat integer range policy.

Class-declaration hover also presents the effective transfer/share facts and
labels explicit opt-out, interface-requirement, or unsafe-assertion policy from
`ClassTypeInfo`. The protocol layer only serializes those compiler notes.
Capability attribute identifiers are syntax-owned decorator tokens in both
Tree-sitter and LSP highlighting.

Exact qualified class specializations use their distinct compiler-owned class
and symbol identities. Hover and outline render the canonical primary plus
arguments; definition from a matching application reaches the specialization,
while a nonmatching application reaches the primary. Semantic tokens use the
ordinary nominal type classes, and invalid declarations publish the shared
`GTI-S2078` diagnostic with related primary or duplicate locations. The
protocol adapter performs no specialization matching.

For a valid `[[c_abi]]` declaration, the same compiler query renders the
attribute and reports the selected record size and ABI alignment. `c_abi` is a
decorator token, and invalid declarations publish the shared `GTI-S2064`
diagnostic; the protocol layer neither computes layout nor keeps a second field
allowlist.

`[[link_name("symbol")]]` follows the same boundary for C-linkage functions.
The attribute is a syntax-owned decorator, hover renders it through the
compiler's canonical function signature, and invalid placement, spelling, or
program-global symbol collisions publish `GTI-S2054`. The LSP never substitutes
the link symbol for source lookup or maintains a second native-symbol table.

`mut { ... }` field groups have no document symbol or protocol-owned semantic
rule. The compiler query flattens their enclosed fields into the containing
class/struct outline, hover renders each field's effective mutable binding, and
definition/reference/rename use the ordinary field symbols. Tree-sitter alone
retains the group node for structural highlighting, indentation, folds, and
delimiter pairing.

`static_assert` likewise has no document symbol or protocol-owned evaluator.
The LSP publishes compiler-owned parser, exact-`bool`, constexpr, and failed-
assertion diagnostics; lexically classifies the spelling as a keyword; and
uses ordinary semantic occurrences for hover, definition, and references in
the condition. A newer passing document snapshot clears the prior diagnostic.

`constexpr` constructors and read-only or non-consuming mutable aggregate methods use the same compiler
signature printer as ordinary calls, so declaration and selected-call hover
retain the qualifier and canonical concrete signature. Unsupported aggregate
evaluation publishes compiler-owned `GTI-S2057` detail, hints, and related
locations. The protocol adapter has no aggregate evaluator or diagnostic
allowlist, and a corrected document snapshot clears the stale failure.
Enum-valued mutable constexpr helpers use that same frontend execution path;
the editor does not maintain an older scalar-only eligibility list.

For `[[c_opaque]] struct Name;`, hover renders the incomplete declaration and
the compiler-owned address-only contract instead of value/concurrency
capability notes. Semantic tokens classify the attribute as a decorator.
Invalid bodies, kinds, direct values, and pointee operations publish
compiler-owned `GTI-S2065` ranges without an LSP ownership model or generated
fix-it. Definitions treat the bodyless form as a declaration, not as a source
definition.

`union` uses the compiler keyword and nominal-type facts and Tree-sitter's
type-declaration capture. Payload alternatives remain enum-member semantic
tokens, while hover and completion render their resolved constructor-like
field signatures. Payload field declarations and case bindings consume
compiler-owned symbol/type occurrences; the protocol layer neither recognizes
patterns nor recomputes exhaustiveness. Shared `GTI-S2066`/`GTI-S2067`
diagnostics are published unchanged.

Mutable reference results backed by global or static storage likewise use the
compiler's exact borrow-origin place and loan endpoints. The LSP publishes
overlapping retained-borrow diagnostics and related origin ranges from the
shared semantic model; it does not infer storage duration or temporary-loan
boundaries from call syntax.

Fresh temporary receiver diagnostics and selected read/mutable overloads use
the same compiler records. The LSP publishes `GTI-S2081` unchanged and clears
it only after a newer compiler snapshot accepts the receiver. Regression
coverage checks the reference-escape reason and exact closing-parenthesis
range separately from the read-only named receiver's mutable-call rejection.
Hover and signature help render the exact selected trailing-`mut` overload; the protocol
layer does not infer freshness from constructor-like punctuation.

Fresh temporary arguments use the same compiler snapshot. An ineligible
read-only-reference argument publishes the compiler-owned `GTI-S2017` reason
and related declaration; a newer valid snapshot clears it. Signature help
renders the exact selected borrowed overload when the fallback tier is active
and the exact by-value overload when the ordinary tier wins. The protocol
layer does not classify producers, rank candidates, or extend lifetimes.

Ordered-call borrow conflicts likewise publish the compiler's later-mutation
primary span and earlier-borrow related span, including UTF-16 conversion of
non-ASCII prefixes. Reordering to a valid mutation-before-lookup expression
clears the error on the next snapshot. The LSP does not reorder operands or
offer a fix-it that could change observable effects.

Moving an owner-dependent text view retains its compiler-owned dependency.
The LSP therefore publishes `GTI-S2045` when that moved expression is stored
in an unsupported field, with the same source token and non-mechanical hint
as compilation. A local view transfer remains valid. The borrowing-composition
protocol regression checks the UTF-16 range after a supplementary Unicode
comment, structured hints, and clearing after valid or repaired edits.
It also checks `GTI-S2019` for a borrowed payload in optional storage. When
concrete instantiation diagnoses the library's private storage declaration,
the published related information retains the exact requesting user binding.
Replacing that payload with an eligible value clears the dependency diagnostic
as well as publishing the new valid document generation.

Mixed shared/exclusive stored-reference aggregates use those same snapshots.
The composition regression covers four independent fields, selected mutable
accessor hover/definition, an overlapping owner mutation after a supplementary
Unicode comment, diagnostic clearing and incomplete-source recovery. Neither
the LSP nor its fixes reconstructs field permissions or return origins; those
remain compiler-owned dependency facts.

Range-for diagnostics follow the same boundary. An immutable owner for a
mutable element reference, a missing exact `begin_mut()`/mutable-dereference
path, or an unsupported range publishes compiler-owned `GTI-S2083`; changing
to a valid read-only or mutable fixed-array/structural range clears it on the
next document version. Incomplete range syntax publishes recovered parser
diagnostics without retaining stale semantic range errors. The protocol layer
does not infer a range protocol, access mode, parent/child loan lifetime,
array strategy, invalidation rule, or temporary ownership from punctuation.
The bounded Expected-payload range follows the same snapshot rule: an owner
mutation publishes the compiler-owned `GTI-S2017`, and replacing the document
with an exact named read-only `value()` range clears it. The LSP does not
recognize `value()`, infer an active alternative, or extend the receiver
lifetime independently.

Stored-carrier return diagnostics remain compiler authority as well. A
by-value carrier return that selects a shell projection, a non-exact referent,
or an incompatible dependency type publishes `GTI-S2085` with its exact return
range and related carrier parameter; replacing it with the exact stored
referent clears the diagnostic on the next document version. Hover continues
to render `std::copyable` and `std::copy_constructible` as distinct declared
concepts. The protocol layer does not infer owner provenance from member names
or lifecycle capability from spelling.

Managed-thread, atomic, and mutex diagnostics also remain shared frontend
authority. An invalid task transfer, single-threaded profile, or target without
thread support publishes `GTI-S2084`; an escaped protected loan or overlapping
live guard publishes the ordinary ownership diagnostic `GTI-S2017`. Both
retain their semantic related spans and hints. Typed-result diagnostics use the
same compiler fact, including exact result matching and transfer denials. The
LSP reads the selected project profile as described below and exposes only the
source-defined `std::thread`, `std::thread_result<Result>`, `std::atomic<T>`
(including integer fetch operations), `std::mutex<T>`, and
`std::mutex_guard<T>` declarations in hover/definition/completion. It filters
the identity-bound `gti_internal` thread handles, atomic/mutex storage,
lock tokens, and operations. It does not derive task transfer, callable entry
selection, result ownership, atomic order legality, protected-value provenance,
lock lifetime, or concurrency from public spelling.

## Document State

`LanguageServer` still owns protocol handling, open overlays, client versions,
analysis and content generations, dependency relationships, immutable result snapshots,
diagnostic sets, and semantic-token caches. Exact reusable-frontend input state
is extracted behind `FrontendSnapshotState` in `src/lsp/`. That component owns
the shared `FrontendCache`, per-canonical-source generations, and exact digests
learned from successfully published analyses. Workers receive a value snapshot
of those identities; the JSON-RPC adapter does not expose its mutable maps to
compiler work.

- `didOpen`/`didChange` replace the full synchronized source, advance the
  document and canonical-source generations, erase the prior learned digest,
  invalidate affected dependants, and schedule analysis over all open-buffer
  overlays. They publish nothing themselves: the client keeps the diagnostics
  and semantic tokens it holds until a newer result replaces them, exactly as
  clangd clients do while a rebuild runs. Publishing an edit-time subset made
  every diagnostic in the edited file vanish on each keystroke and return
  with the next result. Generation checks make a stale result snapshot
  inaccessible to requests, so advancing a root's analysis generation also
  releases that snapshot's frontend result instead of keeping it until the
  replacement publishes. A dedicated reclamation worker destroys released results, so
  neither the protocol thread nor `stateMutex` pays for tearing down a large
  frontend graph, and a request that already holds a result keeps it alive.
  The snapshot keeps its generation and include-discovery flag for
  diagnostic aggregation and dependency invalidation. Before this, a
  shared-dependency edit held a stale and a new whole-program result for
  every dependant at once.
- Each published analysis records, for every dependency it loaded, the
  digest of the exact text it read (`lang::sourceTextDigest`, the identity the
  frontend cache already uses for loaded text). Opening a document is not a
  change for a dependant that read exactly the opened text from disk, so that
  dependant keeps its result; going to a definition in another project file
  then costs one analysis, not one per open dependant. A dependant that read
  different text is reanalysed as before.
- `didClose` removes the overlay and snapshot, clears diagnostics, invalidates
  the overlay source identity and pending work, and reanalyzes dependants whose
  view changes. The next analysis must read the disk source before learning its
  new digest.
- Each open document has two generations. Its analysis generation advances
  whenever its own analysis is rescheduled, including when only a dependency
  changed, and identifies the result a request may use. Its content generation
  advances only when its own overlay text or presence changes (`didOpen`,
  `didChange`, `didClose`). A request captures the content generations of
  every open document.
- Completed analysis is published only if the root's analysis generation and
  the content generation of each open dependency still match. A dependency
  that opened after the request was made is also current when its overlay has
  the digest of the text the analysis read. Only then may its
  source digests be learned. Stale semantic requests and stale digest
  publications are rejected. Dependency staleness never compares analysis
  generations: re-analysing an unchanged dependant then invalidated every
  dependant above it in an include chain, so one shared-file edit re-analysed
  a five-file chain up to eleven times instead of five.
- A root's published diagnostics stay in force until its next result replaces
  them, including while a dependency edit has queued its re-analysis. When a
  publication for an open file is assembled, each diagnostic is included only
  if the text it was computed from is exactly that file's current text, so a
  publication labelled with the file's current version never carries a range
  from a superseded version. A dependant's diagnostics located in an edited
  file therefore disappear with the edit and return with the dependant's new
  result; its diagnostics in unchanged files stay visible throughout.
- The dynamic file watcher covers `**/*.gti`, `**/gti.toml`, `**/gti.lock`,
  and `**/module-map.json`, plus the installed standard-library map’s absolute
  path. A manifest, lockfile, or public module map change invalidates snapshots and
  reanalyses every open document, so configuration, package aliases, and
  materialized locked dependencies cannot leave a stale project snapshot.

Entry-point validation follows the uniquely selected manifest target at its
actual root. Executables require a hosted `main`; `kind = "test"` passes the
compiler's test-suite request, accepting validated `[[test]]` functions or a
legacy `main` and rejecting empty suites and conflicting entries. Other source
files and ambiguous target roots remain fragments; the server does not invent
an executable target from a filename. Watched target-kind changes reanalyse
open roots. Editor requests disable `FrontendOptions::synthesizeTestEntry`:
the compiler still validates and discovers tests, but does not append the
artifact-only dispatcher to the document or expose it through editor queries.
Written source digests, ranges, diagnostics and generations remain unchanged.

### Reusable frontend work

The process-local frontend cache is a thread-safe LRU. The compiler's generic
bound is 256 rows and 64 MiB of conservatively estimated input/tree storage;
the editor's `FrontendSnapshotState` uses 1,024 rows and 512 MiB
(`editorFrontendCacheLimits`), because the lexical and parsed rows of one
eight-include standard-library graph already exceed 64 MiB by that estimate.
With the generic bound such a file evicted its own rows on every edit, reloaded,
re-lexed and re-parsed the whole library, and could reuse no instance
analysis. Lexical rows retain
exact source text, tokens, and lexer diagnostics. Parsed rows retain immutable
statement trees and parser diagnostics. A key includes canonical source path,
that source's generation, stable exact-content digest, exact post-directive
token-stream digest, a transitive dependency-input digest, normalized target
data layout and configuration flags, and the completion offset where
applicable. The dependency identity includes each visible input's canonical
path, generation, exact source digest, and processed token digest. A
dependency-provided flag that selects different syntax therefore invalidates
an otherwise unchanged dependant parse. Dirty overlays additionally compare
their exact bytes after lexical lookup, so they override disk state and a
digest collision cannot select different text there.

A generation change makes old rows unreachable immediately. Physical removal
is left to the LRU, avoiding large AST destruction while the protocol thread is
processing a change. A root-only edit can therefore reload, lex, and parse the
root while reusing unchanged dependency and standard-library inputs. A dirty
dependency advances only that source identity; existing dependency tracking
reanalyzes its open dependants, whose affected parsed rows miss through the
dependency digest, without advancing or invalidating unrelated source
identities.

This is deliberately not a semantic snapshot cache. Semantic analysis runs over
the compatible loaded declarations; HIR performs the root/entry-scoped concrete
discovery described below. Both produce fresh snapshot-local identities and
records, except that `FrontendSnapshotState` also owns a process-wide
`InstanceAnalysisCache` passed as `FrontendOptions::instanceCache`. Concrete
instance analyses of compiler-trusted declarations whose arguments name only
compiler-trusted types are stored once HIR is final and reused by later
snapshots of the same library generation: the same trusted parsed syntax,
analysis configuration and trusted registration identities. A reused delta's
journaled base identities, consulted functions and identity range are
validated first, and a failure simply analyses the instance again (see
"Instance Reuse Across Snapshots" in
[semantic-analysis.md](semantic-analysis.md)). Editing a library unit, or
reparsing it, selects another generation, which supersedes the old one. The
cache is bounded by 8,192 entries, 192 MiB of estimated delta storage and four
generations. Completion stops before HIR and does not use it. The benchmark exposes exact loaded,
lexed, parsed, semantically analysed, and cache-hit unit lists plus phase times
through the opt-in `GTI_LSP_FRONTEND_TRACE` stream. Each record also carries a
monotonic `finishedMicroseconds` time since server start, so concurrent work
can be ordered, and analysis records carry `retainedFrontends`, the number of
published snapshots still holding a whole-program result. The same report records the configured cache limits and
observed estimated occupancy. Analysis records also carry
`hirFunctionInstances`, the instance-reuse counts (`instanceReuseHits`,
`instanceReuseMisses`, `instanceReuseRejected`, `instanceReuseIneligible`,
`instanceReuseStores`), `instanceReuseMicroseconds`, and the instance cache's
occupancy (`instanceCache`).

Analysis and completion have separate bounded worker queues. This keeps the
JSON loop responsive and lets newer completion/analysis work supersede older
requests without adopting clangd's per-translation-unit scheduler complexity.
The analysis queue is first-in first-out, except that a root with a queued
semantic request (hover, definition, references, and similar) runs before
dependants queued ahead of it, so the document under the cursor is not
answered last after a shared-dependency edit.

An edit-triggered analysis (the edited document's own, and each dependant's)
does not start at once. It waits for the document's median recent analysis
time, bounded to 50-150 ms, and every further edit restarts the wait, so a
typing burst is analysed once from its final text. A semantic request waiting
on the document, shutdown, opening a document, and project invalidation
(manifest, lockfile, profile) run immediately. This is clangd's
update-debounce policy, adopted after measuring that every keystroke re-runs
semantic analysis and concrete HIR discovery; the old timings are not current
performance guarantees. `GTI_LSP_UPDATE_DEBOUNCE_MS` pins a fixed wait, and
`0` disables it, for benchmarks and tests.

An edit that arrives while its document is being analysed also cancels that
analysis (`FrontendOptions::cancellation`), so the wait can stay short: the
newest text never queues behind an obsolete analysis. Semantic analysis checks
the flag at safe points (after registration, before finalization, at each
top-level statement and each concrete-effect owner) and stops by unwinding to
`SemanticVisitor::check`; the frontend then returns a result marked
`cancelled` before HIR, so the instance cache receives nothing from it. A
preamble built before the stop is complete and stays cached. The stopped run
is not published, does not count toward the debounce median, and is traced as
`kind: "cancelled"` under `GTI_LSP_FRONTEND_TRACE`. Raw-pointer audits are not
cancelled; they run only when the pool is idle.

`workspace/semanticTokens/refresh` is not sent per publication. A client
re-requests tokens for a document it edited or opened itself. Only an analysis
the document did not ask for (a dependency or project change) can change
tokens the client holds, so after such a publication the server recomputes the
document's tokens off the protocol thread, compares them with the ones last
answered, and requests one refresh for the whole wave of dependant
re-analyses once the pool is idle, and only if some document's tokens actually
differ. Neovim answers a refresh by clearing and re-requesting the tokens of
every attached buffer, so a refresh per result blanked all highlighting
several times per keystroke.

Every open document is its own whole-program root, so opening a project or
editing a shared dependency queues one independent analysis per affected
document. A small pool of interchangeable analysis workers runs those
concurrently: half the host's hardware threads, at most four, or 1-16 through
`GTI_LSP_ANALYSIS_WORKERS`. The bound exists because each in-flight analysis
holds a complete frontend result until it is published. A document is never
analysed on two workers at once. Its newer request waits for the running
analysis to finish, so rapid edits to one file cannot fill the pool with
results that generation checks would discard. The workers share no
compiler state beyond the thread-safe frontend cache; publication still
happens under `stateMutex`, after the guarded compiler work.

Completion keeps one worker. Every worker is a long-lived LLVM thread with an
explicit 8 MiB stack. That avoids Darwin's 512 KiB secondary-thread default
and ensures the compiler's bounded-depth recovery runs before native stack
exhaustion in debug builds, without creating a new thread for every edit or
completion request.

Editor analysis requests `FrontendOptions::stopAfter = Hir`. HIR is the
compiler-owned discovery point for concrete generic instances, so stopping at
symbolic semantics made valid editor snapshots omit diagnostics that
`gti check` reports for instantiated generic bodies and dependent
`static_assert` declarations. It also sets `lowerEntryUnitOnly`: lowering
seeds the program roots and the open file's own declarations and discovers
only what they reach, instead of every declaration in every loaded unit.
Instance diagnostics for the open file's code, including the library
instances it requests, are therefore identical to `gti check`; an instance
that only unreached library code requests is not reported in the editor,
because it is that library's own `gti check` concern and the complete
lowering cost about a third of every keystroke. The LSP does not query HIR,
and it does not build failure metadata or MIR, so it also clears
`lowerLibraryOnlyInstanceBodies`: a library-only instance (a compiler-trusted
declaration whose arguments name only compiler-trusted types) is analysed and
its own diagnostics are reported, but its body is not lowered. Instances that
only such a body would request, such as a library class instantiated inside
a library-only function, are left to the library's check by the same rule
(see "Library-only instance bodies" in [hir.md](hir.md)). An instance of the
open file's own types is lowered completely and reaches what `gti check`
reaches. It releases the HIR program as soon as the analysis returns, once
HIR's diagnostics are in the result, so retained snapshots hold syntax,
semantics, and source identity only. Code generation remains disabled.

Editor analysis and completion also set `trustedBodies = Reachable`:
standard-library (compiler-trusted) function bodies are analysed only when
something analysed demands them. A body that only concrete generic discovery
selects is completed during HIR lowering by late demand, inside the same
analysis, so an edit runs registration, body analysis and HIR once (see "Body
Order And Demand" in [semantic-analysis.md](semantic-analysis.md)). The
bounded repeated analysis remains only for the cases late demand refuses. The
frontend trace reports `semanticLateDemandBodies`,
`semanticLateDemandMicroseconds`, `semanticDemandRounds` and
`semanticDemandFallbacks` for every analysis.

The LSP also owns a `SemanticPreambleCache` beside its parse and instance
caches and passes it through `FrontendOptions::preambleCache`: the open
file's standard-library prefix is analysed once per import graph and every
later snapshot resumes from a copy of that analysis, registering and
analysing only the file's own declarations (see "Trusted Prefix Preamble" in
[semantic-analysis.md](semantic-analysis.md)). The first snapshot of a new
import graph pays for the build; a changed or re-parsed library unit selects
a new preamble. The trace reports `preambleBuilds`,
`preambleBuildMicroseconds`, `preambleAdoptions`,
`preambleAdoptMicroseconds`, `preambleBodies`, `preambleReachedBodies`, the
cache's occupancy, `conceptProofReuses` and `conceptProofRecords` (concept
proofs the preamble shares across snapshots; see "Shared concept proofs"),
`effectQueryReuses` and `effectQueryRecords` (initializer-effect results it
shares; see "Shared initializer-effect results") and,
for every analysis, `includeSuggestionsRequested`. The background raw-pointer audit never uses a
preamble.

Before analysis and completion, the LSP asks the driver for immutable
`ProjectFacts` for the package containing the entry document. The facts select
the nearest workspace member/package, the selected profile's analysis
configuration, and the exact `PackageSourceRoot` closure used by `gti check`.
The profile defaults to the driver's normal default (normally `dev`). Clients
may select a named project profile with `initializationOptions.profile` or
`workspace/didChangeConfiguration` settings `{ "gti": { "profile": "release" } }`.
A null/omitted `profile` in a GTI settings object restores the project default;
notifications without a `gti` object leave the choice unchanged. Selection is
per server instance, not a build invocation or a process-global environment
variable. Neovim sends its configured `settings.gti.profile` after startup;
`:GTIProfile <name>` changes it for servers attached to the current buffer,
`:GTIProfile!` resets it, and `:GTIProfile`/`:GTIInfo` report the requested choice.

The configuration includes package and selected-profile defines. Driver facts
infer target defines for a unique matching entry root, or any document in a
single-target package. Shared files and ambiguous entry roots in multi-target
packages receive no target defines: unrelated target sets are never combined.
Inactive consumer target/profile flag names remain known for typo diagnostics,
and both active and declared sets enter the frontend cache context. Watching a
manifest refreshes these facts for diagnostics and completion together.
Profile changes also advance every open document's analysis generation,
invalidate token caches and queued semantic requests, and capture the exact
profile into each analysis/completion request. In-flight old-profile results
cannot publish under the new generation. Dirty source overlays remain
authoritative, and compatible frontend-cache rows retain the ordinary bounded
configuration/content identity rules.

Invalid setting types or non-portable profile names produce a
`window/showMessage` error and preserve the previous choice. An undeclared
profile produces the driver's existing `GTI-B1202` meaning at the entry
document's first source position, with available-profile guidance. That
generation has only lexical/empty query recovery, not fabricated default-profile
semantics or inactive spans. Selecting a valid/default profile clears it.
Standalone documents retain their prior analysis defaults; named project
profiles do not invent a manifest for them.
Named package imports therefore use only declared direct aliases, including
already-materialized checksum-verified Git dependencies. Resolution runs no
process, performs no fetch or build, and creates no project state. Registry
dependencies use the same verified cached closure, logical source identities
and exact lock edges; no version-discovery or archive request is made by editor
analysis. A missing
manifest preserves standalone analysis; a malformed, unlocked, unmaterialized,
or checksum-mismatched project publishes the driver's structured diagnostics
and never invents a partial package graph.

The selected profile also supplies warning-group and dependency-warning policy.
It also supplies `raw-pointer-audit = "off"|"warn"|"strict"`. A process option
`gti_lsp --raw-pointer-audit=off|warn|strict` explicitly overrides that profile;
without either selection standalone documents use `off`. Duplicate, missing,
or unknown process values are rejected. Mandatory type/unsafe/checked-borrow
errors remain errors in every mode. Audit warning/error severity comes from
the same frontend boundary as CLI compilation; the LSP does not promote
findings or infer safety itself. Watched manifest changes invalidate these
snapshots even when source bytes are unchanged. No independent dynamic editor
policy is introduced.

The audit never runs on the edit path. It is whole-program provenance
analysis the compiler cannot interrupt, and it made one analysis of an
eight-include file take 41 s instead of 0.6 s, so with it on the edit path
every keystroke stalled the editor for the audit's duration. Edit-path
analysis and completion always pass `Off`. When the selected mode is `warn`
or `strict`, each published generation additionally schedules a background
audit: the same analysis of the same text with the profile's mode, on one
dedicated worker that starts only when no analysis or completion is pending
and the document has been quiet for two seconds (`GTI_LSP_AUDIT_QUIET_MS`
pins the period for tests), or at once on `didSave`. A newer generation
replaces a waiting audit; a finished audit publishes only if its generation
is still current. Its findings are the diagnostics the ordinary result did
not already contain, stored per root and published as additions to that
generation's set, so a document with the audit enabled publishes twice per
settled version: the edit-path result, then the audit's additions. Findings
belong to their generation: a dependency or project change retires them
until the next audit, even when the file's text is unchanged. Among several
waiting audits the most recently scheduled runs first. The protocol layer
holds no list of audit codes; the audit result is diffed against the ordinary
result. The frontend has already applied the profile's
`raw-pointer-audit-library-findings` policy, so by default the editor never
shows standard-library implementation findings for an application file. Because a running audit cannot be cancelled, `exit` ends the process
without waiting for it.

The protocol adapter applies the shared driver's primary-source package
classifier when publishing diagnostics, not a directory-name heuristic.
`dependency-warnings = false` removes dependency warnings without hiding
errors, consumer warnings or their related dependency notes. Filtering does
not discard the analyzed semantic snapshot: hover, completion and other queries
remain available. A dependency opened as its own project becomes the selected
package and retains its own warnings. Watched manifest changes invalidate
diagnostic snapshots through the existing project-input generation path.

An open document whose canonical path is a declared executable or test root
also requests the frontend's missing-entry check (`GTI-S2094`). Ordinary
included files, source-only packages, and standalone documents do not acquire
executable intent. Completion-marker recovery suppresses this check; normal
document analysis clears or replaces it through the existing versioned
diagnostic publication path. The LSP does not scan for `main` itself.

Every open GTI document is still passed to the frontend as a canonical-path
source override. An unsaved dependency buffer is therefore authoritative over
the verified on-disk package root for diagnostics, hover, completion,
definition, and semantic tokens, while generation checks prevent an analysis
captured before that edit from publishing. The protocol adapter does not parse
TOML, reconstruct workspace membership, or resolve dependency aliases itself.

The selected project configuration seeds package, selected-profile and resolved
target flags, uses that profile's execution model, and derives runtime capabilities from the installed
hosted runtime profile. A startup
`--execution-profile single-threaded|concurrent` option overrides the manifest
profile and also configures standalone documents. Omission selects the
selected project profile when present and otherwise remains
single-threaded.

This keeps standalone and concurrent-project diagnostics and the public
`std.thread`, `std.atomic`, and `std.mutex` surfaces aligned with
compiler-driver workflows. Analysis work
additionally runs inside `lang::runGuarded`, and that boundary is deliberately
narrow. Only compiler work runs under the guard: `runIsolatedAnalysis` builds
an isolated `DocumentAnalysis` and its immutable frontend snapshot, and
`publishAnalysis` writes to shared LSP state afterwards, on the normal path.

The boundary isolates protocol publication and exception unwinding:

- **No exception crosses LLVM.** LLVM's `CrashRecoveryContext` is built
  without C++ exception support, so the guarded callback catches its own
  exceptions and returns the outcome as data (`GuardedAnalysis`). The worker
  inspects that status instead of relying on an exception unwinding through
  an LLVM frame.
- **The protocol-state lock is outside the guard.** `stateMutex` protects
  publication on the normally unwound path, not the guarded compiler callback.
  This prevents a skipped publication destructor from stranding that mutex;
  it does not mean the entire frontend operation is lock-free.

**Remaining fault-state gap (2026-10-11 audit):** `analyzeDocument` supplies the
shared `FrontendCache` inside `runIsolatedAnalysis`, and its lookup/store methods
acquire their own mutex. A fatal signal during one of those critical sections
can skip the lock destructor or interrupt a cache update. In-process recovery
therefore cannot guarantee that the cache or future workers remain usable.
`test_worker_survives_failed_analysis` covers missing-dependency failures followed
by healthy analysis/hover; it does not inject a crash under a cache lock. The
parser-depth regression proves bounded diagnostic recovery, not arbitrary fault
recovery. A stronger promise needs a state/lock-safe recovery design and
dedicated fault-injection evidence, or process isolation.

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

Editor snapshots publish the compiler-owned `GTI-I0014` error for removed GTI
include syntax using the same source-loading rule as build/check requests. The
former optional `legacy-include` migration warning is removed. Module conversion
has no automatic fix because dependencies require explicit interfaces and
exports and conversion can change visibility.

`src/lsp/main.cpp` owns JSON-RPC IDs, capabilities, URIs, UTF-8 byte to LSP
position conversion, request/result serialization, diagnostic publication,
workspace edits, cancellation, and semantic-token wire encoding. It advertises
full document sync, formatting, semantic tokens, hover, completion,
definition, signature help, references, document highlights, document symbols, and rename
with prepare support. It advertises quick-fix code actions when the client
supports code action literals. Document symbols are hierarchical when the
client negotiates `hierarchicalDocumentSymbolSupport` and flatten to
`SymbolInformation` with container names otherwise; rename responses use
versioned document changes when the client supports them.

The protocol translation unit uses `llvm::json` as private parsing and encoding
machinery. Incoming payloads are parsed into RAII value objects and invalid
UTF-8 is rejected as a JSON-RPC parse error. IDs and params retained by queued
or superseded requests are ordinary value copies, so asynchronous request
lifetime does not depend on protocol-library retain/release calls. GTI still
owns the JSON-RPC/LSP object schema and validation; no LLVM JSON type crosses a
public header or enters compiler snapshots.

The shared compiler token contract classifies `double` as a built-in type and
the complete `d`/`D`-suffixed decimal as one numeric semantic token. The LSP
does not rescan the suffix or infer float width independently.

Diagnostic serialization preserves compiler codes, severities, groups, phases,
hints, related locations, and fix-its while respecting the client's
`publishDiagnostics` capabilities. Hints stay in the plain-text message for
universal display and are also exposed as structured data when supported.
Ownership explanations use this same serialization: move-site notes,
borrow-origin/later-statement notes, and read-only declaration notes remain
compiler-owned half-open byte spans until protocol conversion. Their intentional
repair options are plain hints, not automatic moves, clones, mutability changes,
or effect reorders. Correcting an overlay clears the former explanation on the
next document generation; a diagnostic from the prior version cannot authorize
an action on the replacement source. Terminal display-cell expansion never
enters LSP messages or UTF-16 ranges.
Compiler diagnostic tags are published only when the client's negotiated
`tagSupport.valueSet` admits that exact LSP tag; the current unreachable-code,
unnecessary-`mut` and unused-unsafe warnings use `Unnecessary`. Quick-fix
applicability is compiler-owned and distinct from preference. Unannotated fixes,
including unused-`mut` removal and `[[discard]]` insertion, require review:
they remain discoverable and manually invocable but are not preferred.
An LSP `Automatic` trigger requests action discovery (for example, a lightbulb
after a selection change), not automatic application, so it does not hide
review-required alternatives. Only a producer-proven `MachineApplicable` group
may retain preference. See the [LSP code-action contract](https://github.com/microsoft/language-server-protocol/blob/gh-pages/_specifications/lsp/3.18/language/codeAction.md).
Selected mutable-reference constructor arguments count as mutable uses in the
compiler. Changing a constructor from read-only to mutable-reference removes
the obsolete warning and its quick fix; an old diagnostic cannot remove a
now-required `mut` in the new document generation.
When the client supports literal quick-fix actions, a diagnostic carrying at
least one structurally valid repair group appends `(fix available)` to its LSP-facing
message. The compiler diagnostic text remains unchanged, and clients without
that action capability are not promised an unavailable editor operation.

Equal nonempty repair IDs group edits atomically; different IDs and legacy
singletons remain alternatives. This first profile supports one open target
document per group. It rejects unknown or inconsistent source text, cross-file
groups, invalid/reversed ranges, UTF-8/CRLF splits, overlaps, ambiguous touching
insertions, no-op members, incomplete suggestions and mixed applicability.
No fix-all operation is offered. Full captured text must match the target's
current overlay; versioned `documentChanges` carry the live document version.
The originating root and requested document must both have current snapshots.

For clients negotiating diagnostic `dataSupport`, repair-bearing diagnostics
carry `repairSnapshot` (root URI plus analysis generation), `repairId` and
applicability. Echoed stale generations cannot authorize an edit, even if the
text changed back to identical bytes. Clients without diagnostic data retain
the current-source manual path; clients without `documentChanges` cannot enforce
a version at application time. Neither fallback promises closed-file or
cross-document transactions. Structural group validity is not a proof that an
intent-changing suggestion should be applied automatically.
When several open roots diagnose the same dependency span, publication merges
their related information instead of selecting one root arbitrarily; distinct
include and instantiation locations therefore remain visible on the one
deduplicated diagnostic. Diagnostics with different hints or fixes remain
distinct. The same grouping applies to identical primary errors from multiple
concrete instantiations in one root; every substitution note and requesting
location is retained. This presentation grouping does not deduplicate the
compiler's instance-owned diagnostic records.
Quick fixes come only from compiler `Diagnostic::fixes` associated with the
current source/generation, an intersecting request range, and an exact
client-provided diagnostic. Position conversion is tested at the protocol
edge; compiler source spans remain half-open UTF-8 byte ranges.
Unresolved declarations publish compiler-validated missing-import fixes through
this same path. This covers project functions such as `example::test()`, types,
aliases, values, scoped enumerators, static members, and concepts, not a catalog
of standard-library type spellings. Semantics marks the unresolved lookup on
the diagnostic; the protocol adapter never extracts names from its message.

The driver supplies search roots for the selected package and its declared
direct dependency aliases. Standalone files search their parent directory;
dependency and standard-library candidates come only from explicit public
module maps. Same-owner discovery recurses through `.gti` files, excluding hidden/build directories, nested
packages without their own search authority, and symlink escapes. Unsaved
files and dirty overlays participate. Candidates must be explicit module
interfaces. Dependency fixes use the direct alias and declared module name;
standard fixes use `std.name`; same-package fixes use a relative quoted import.
Public facades contribute their explicit re-export closure, including declarations
whose bodies live in registered implementations. Candidate discovery remains a
syntax filter; a speculative compile validates the selected canonical exported
symbol and the actual requesting program before offering an edit.

`include_suggestions.cpp` uses the lexer as a cheap candidate filter and the
existing source loader/parser to discover declaration names. `FrontendPhase::Parsing`
stops the canonical frontend before semantic analysis; it is not another
declaration parser or semantic index. A candidate edit is then checked in a
separate frontend snapshot of the actual requesting program. The existing
definition query must resolve the original occurrence to that candidate file,
and the edit must introduce no new errors. Wrong namespace, access, overload,
configuration, package, and module guesses therefore fail closed. Existing
unrelated errors may remain; a candidate that reveals additional errors is
conservatively withheld until those errors are addressed. No discovery runs
for a snapshot without unresolved-name requests, with source-loading or parser
errors, or for completion-marker analysis. Parser recovery still supplies its
ordinary diagnostics and semantic queries, but an import edit cannot repair
syntax in the frozen source graph. Skipping discovery in that case avoids a
workspace-wide scan whose speculative edits would all be rejected; fixing the
syntax restores import suggestions on the next snapshot.

The service shares the bounded parsed-unit cache but retains no semantic IDs
across snapshots. Edits preserve BOM/CRLF and follow a module header when the
requesting file has one. Multiple valid files produce separate, deterministic,
non-preferred alternatives rather than silently choosing a declaration.

Open/change/close and watched-file events refresh snapshots that requested
discovery, including negative searches and newly created files outside the
loaded graph. A discovery-generation check rejects an in-flight result after
any source change; ordinary loaded-source generation checks remain unchanged.
Code actions still require the current source and diagnostic. This deliberately
uses conservative invalidation for unresolved snapshots rather than adding a
persistent background project index.

## Incomplete Source

Editor analysis opts into source-graph recovery as well as parser recovery and
semantic analysis of recovered declarations. A missing import retains the
available units for independent diagnostics and queries; it does not make the
source graph valid. Such snapshots never produce HIR/MIR, cache semantic
preambles/instances, or synthesize a test entry. Ordinary compilation remains
fail-closed at source loading. Completion runs a dedicated frontend analysis with a cursor
marker so the parser/analyzer can capture real scope, receiver, visibility, and
overload candidates in fragments such as `object.` or `foo(`. A variable
initializer that consumes this marker remains available to semantic completion
even when the user has not yet typed its trailing semicolon; ordinary compiler
snapshots continue to reject that source. Published diagnostics do not gate
completion when the marker's scope and receiver remain recoverable: an
unrelated error such as a missing return in the enclosing function remains
visible while candidates are returned. Features may return partial or no
semantic results; they must not confidently invent missing meaning from
punctuation.

## Current Limits

- Retained documentation is consumed by hover, completion, signature help and
  the compiler-owned documentation index used by the separate reference site.
  Website projection/publishing has its own [reference-documentation contract](reference-documentation.md).
- Definition, references, document highlights, and rename are implemented
  from exact resolved symbols. Rename is deliberately limited to
function-local names: identities visible outside the entry document's
  source graph may have references in unopened dependants the snapshot cannot
  see, so the compiler query rejects them instead of producing an incomplete
  edit. A project symbol index is not implemented.
- `SymbolId` is snapshot-local. There is no durable cross-analysis identity.
- Document overlays, dependency scheduling, diagnostic aggregation, and most
  request freshness rules remain in the protocol class. The exact reusable
  frontend-input identity/cache seam is extracted; broader state extraction
  should follow a concrete testability or feature need.
- Semantic analysis of loaded declarations and root/entry-scoped HIR discovery
  repeat on every accepted edit. Only library-only concrete instance analyses
  are reused across snapshots; registration and primary body analysis are not
  a reusable semantic checkpoint, and durable symbol identity is not
  implemented. Trace evidence, not the old complete-HIR baseline, should select
  the next repeated-edit optimization.
- Project analysis follows the package containing the opened document and the
  driver's default or explicitly selected profile on the host platform. Unique entry roots and
  single-target packages infer their target defines; shared/ambiguous sources
  in multi-target packages receive only package/profile flags. The protocol has no explicit
  workspace-package, target, or cross-target selection
  request yet; adding one requires a client-facing configuration contract,
  not another manifest parser.
- Crash containment is best-effort and in-process. Protocol publication stays
  outside the guard, but the shared cache's lock/update recovery gap and heap
  damage prevent a general post-fault worker/state usability guarantee.

Those items are tracked in [`docs/plans/lsp-evolution.md`](../plans/lsp-evolution.md).
[ADR 005](../decisions/005-lsp-compiler-semantics.md) records why language
features share compiler semantics.

## Do Not Copy From clangd Yet

GTI does not need C++ preambles/PCH, compile-command borrowing, macro
spelling/expansion locations, background index shards, remote indexes,
clangd-style full semantic translation-unit AST caches, or one worker per
translation unit. The analysis pool is a fixed, small set of interchangeable
workers, not a per-document scheduler. Its bounded exact-content parsed-unit reuse is narrower and
contains no semantic records; the bounded instance reuse keeps only validated
library-only instance deltas, never a translation unit's semantic model. Preserve the boundaries that allow measured
evolution without importing those systems preemptively.

### Foreign storage queries

Foreign-variable hover uses the compiler's resolved external-storage identity
to display C linkage, source access, exact renamed native symbol and unsafe
load/store obligations at both declaration and use sites. Definition, references
and semantic tokens retain the ordinary global symbol identity. The protocol
does not infer ownership from an external declaration or add a fix accepting an
unsafe access. Incremental diagnostic and hover tests cover edited link names,
UTF-16 positions and clearing stale unsafe-access errors.
