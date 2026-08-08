# LSP Completion And Hover Architecture

Status: Phase 1 hover shipped in 0.42.0, Phase 3 visible-symbol completion in
0.43.0, and the first Phase 4 symbol/navigation layer in 0.44.0.
Documentation, completion-item resolve, and project-wide indexing remain
future work.

## Objective

GTI should provide semantic completion and hover information with the same
basic quality users expect from clangd:

- completion proposes names that are valid at the cursor, with useful type and
  signature details;
- hover shows the resolved GTI declaration, inferred or instantiated type, and
  source documentation;
- unsaved entry files and unsaved included files are analyzed coherently;
- results follow GTI visibility, access, overload, generic, mutability, and
  ownership rules exactly;
- the compiler frontend remains the only implementation of those rules.

The LSP process must not recreate scopes, resolve names, substitute generic
types, select overloads, or infer ownership. It converts protocol positions to
compiler positions, schedules compiler queries, and serializes the results.

In LazyVim, completion UI is supplied by the configured completion client.
Pressing `K` is a client key mapping that normally calls
`vim.lsp.buf.hover()`. GTI does not need a custom `K` mapping: `gti_lsp` needs
to advertise and correctly implement `textDocument/hover`.

## Current State

The architecture now has these foundations:

- both the CLI and LSP call `lang::Frontend`;
- `FrontendResult` owns the recovered `Program`, `SemanticModel`, typed HIR,
  structural MIR, `SourceGraph`, `SourceManager`, and diagnostics;
- every analysis snapshots all open buffers as source overrides, including
  unsaved included files;
- dependency generations prevent stale diagnostics from being published;
- source locations remain byte offsets until the LSP converts them to UTF-16;
- semantic analysis records resolved expression types, bindings, class and enum
  identities, selected constructors, selected calls, operators, generic
  substitutions, mutability, access, and ownership traits;
- `SemanticModel` retains snapshot-scoped symbol records and a source-unit
  occurrence index with declaration, definition, reference, read, write, call,
  and type-use roles;
- `SemanticTypePrinter`, `SignaturePrinter`, and `LanguageQueries::hover` are
  compiler-owned and render GTI source spellings;
- the LSP commits immutable `FrontendResult` snapshots only after dependency
  and generation checks, and `textDocument/hover` reads only the current
  generation;
- LSP positions are converted back to UTF-8 byte offsets with checked UTF-16
  handling, including rejection of positions inside surrogate pairs;
- a dedicated completion frontend inserts an internal marker at the cursor and
  records live unqualified, namespace, enum, and member candidates during real
  semantic lookup;
- completion runs on a bounded worker independently of diagnostics, rejects
  stale document generations, ranks candidates deterministically, and emits
  exact UTF-16 text edits and snippets when the client supports them;
- semantic tokens consume compiler-owned symbol kinds and occurrence roles for
  resolved identifiers; token-based identifier classification is used only as
  a degraded fallback before a semantic snapshot is available;
- `LanguageQueries::definition` and `textDocument/definition` navigate by exact
  symbol identity, selected overload, and selected constructor across the
  current frontend source graph.

The remaining gaps are:

1. Symbol identities are snapshot-local by design; explicit `ScopeId`, complete
   declaration extents, and project-stable identities remain deferred until a
   concrete query needs them.
2. Compiler-provided pseudo-members and intrinsic constraint names still need
   explicit tooling records if they are to receive semantic highlighting.
3. The lexer discards comments, so declaration documentation is not retained by
   the frontend.
4. completion still needs request cancellation, dedicated type/argument/include
   contexts, checked `->` receiver completion, documentation resolve, and
   latency benchmarks.

## Architectural Decision

Add a compiler-owned semantic query layer and retain its immutable result in
the LSP. Keep the JSON-RPC layer as a protocol adapter.

```text
open document versions and source overlays
                    |
                    v
          compiler query scheduler
                    |
        +-----------+------------+
        |                        |
        v                        v
normal Frontend analysis   completion Frontend analysis
        |                  with a synthetic cursor token
        v                        |
FrontendResult                  v
  + SemanticDatabase      CompletionContext
  + SourceGraph             + semantic candidates
  + SourceManager                 |
        |                         |
        +------------+------------+
                     v
          compiler LanguageQueries
             hover / completion / definition
                     |
                     v
       thin LSP conversion and JSON output
```

The names in this document are proposed API names, not commitments to exact
header or class names.

### Authoritative hover invariant

`gti_lsp` must never maintain its own descriptions of GTI functions, classes,
fields, overloads, generic constraints, or standard-library APIs. Its retained
`AnalysisSnapshot` is ownership of compiler output, not a competing semantic
database.

The authoritative hover path is:

```text
GTI application or library source
              |
              v
     source loader / lexer / parser
              |
              v
documentation attached to its declaration
              |
              v
 semantic symbol and resolved occurrences
              |
              v
 compiler-owned signature and hover query
              |
              v
       gti_lsp protocol adapter
              |
              v
 textDocument/hover with MarkupContent
              |
              v
       Neovim renders the popup
```

The current `SemanticModel` already has much of the necessary truth: canonical
types, `FunctionInfo`, overload identities, and `ResolvedCallInfo` containing
the exact function selected for a call. What it lacks is a complete public map
from source occurrences to semantic symbols, declaration documentation, and a
reusable source-signature printer. Those belong in the frontend. Filling those
gaps must not be implemented by name lookup or signature reconstruction in
`src/lsp/main.cpp`.

After receiving compiler-owned `HoverInfo`, the LSP is allowed to do only
protocol work: map a source byte span to an LSP UTF-16 range, wrap the signature
in a fenced `gti` block, append the supplied Markdown, and serialize the
response.

### Compiler-owned layers

The compiler library should own:

- `SemanticDatabase`: symbols, scopes, occurrences, documentation, and typed
  facts for one immutable frontend analysis;
- `LanguageQueries`: backend-neutral hover and completion queries over compiler
  data;
- `SemanticTypePrinter` and `SignaturePrinter`: canonical GTI source spelling;
- completion-point parsing and candidate collection;
- candidate viability and ranking inputs derived from GTI semantics.

The LSP executable should own:

- open document text and versions;
- UTF-16 position conversion;
- request scheduling, cancellation, and stale-generation checks;
- client capability negotiation;
- conversion from compiler result kinds into LSP kinds;
- Markdown serialization into `MarkupContent`;
- bounded caching of immutable compiler snapshots.

This boundary allows a future command-line documentation tool, IDE integration,
or test harness to use the same queries without speaking LSP.

## The Semantic Database

“Database” here means an immutable, queryable semantic snapshot. It should
begin as normal in-memory C++ data owned by `FrontendResult`, not SQLite and not
an LSP-specific file format.

The database must be populated as the semantic analyzer performs real lookup.
A later AST walk may compact recorded facts, but it must not infer what a name
means for a second time.

### Symbol identity

Introduce an opaque `SymbolId` covering all source-facing declarations:

```cpp
enum class SymbolKind {
  Namespace,
  NamespaceAlias,
  TypeAlias,
  Class,
  Struct,
  Enum,
  Enumerator,
  Constructor,
  Destructor,
  Function,
  Method,
  Operator,
  Field,
  GlobalVariable,
  LocalVariable,
  Parameter,
  TypeParameter,
  ValueParameter,
  Lambda,
  Builtin,
};

struct SymbolRecord {
  SymbolId id;
  SymbolKind kind;
  std::string name;
  std::string qualifiedName;
  SourceUnitId sourceUnit;
  SourceSpan nameSpan;
  SourceSpan declarationSpan;
  ScopeId declaringScope;
  AccessModifier access;
  SemanticType type;
  std::optional<CallableSignature> callable;
  SemanticTypeTraits traits;
  std::optional<Documentation> documentation;
};
```

Existing `FunctionId`, `ClassId`, `EnumId`, and constructor identities can
remain semantic identities and be referenced by `SymbolRecord`. They are not
enough alone because tooling also needs locals, parameters, fields,
enumerators, aliases, and namespaces.

Each function overload has its own `SymbolId`. A name occurrence may initially
refer to an overload set, while a resolved call occurrence refers to the one
selected overload. Hovering a call must therefore show the instantiated
selected signature, not whichever declaration happens to be visited first.

Snapshot-local numeric IDs are sufficient for live queries. A later persistent
workspace index needs a separate stable key derived from source-unit identity,
symbol kind, qualified name, normalized parameter types, and generic arity.
AST addresses and per-analysis numeric IDs must never be serialized.

### Scopes and visibility

Record lexical and namespace scopes explicitly:

```cpp
struct ScopeRecord {
  ScopeId id;
  ScopeKind kind;
  std::optional<ScopeId> parent;
  std::optional<SymbolId> owner;
  SourceUnitId sourceUnit;
  SourceSpan extent;
  std::vector<SymbolId> declarations;
};
```

The semantic analyzer should record a completion snapshot when it reaches a
completion point. This preserves facts that cannot be reconstructed reliably
after analysis, including:

- the active lexical and namespace scope chain;
- declarations visible before the cursor where declaration order matters;
- direct-include and prelude visibility from `SourceGraph`;
- the current class and private-access context;
- the receiver type and receiver access for member completion;
- active generic type and value parameters;
- expected type, when one exists;
- flow-sensitive facts such as moved values or unavailable mutable access.

Completion must not expose transitive includes or sibling units. An inactive
target-conditional branch is parsed but does not contribute semantic
candidates for the selected target.

### Occurrences and position lookup

Every declaration and resolved use should produce a `SymbolOccurrence`:

```cpp
enum class OccurrenceRole {
  Declaration,
  Definition,
  Reference,
  Read,
  Write,
  Call,
  TypeUse,
};

struct SymbolOccurrence {
  SourceSpan span;
  OccurrenceRole role;
  std::vector<SymbolId> targets;
  std::optional<ResolvedCallable> selectedCallable;
};
```

Occurrences should be sorted by source and byte range so hover can find the
narrowest occurrence containing the cursor in logarithmic time. The selected
callable stores the same declaration and concrete substitution already chosen
by semantic analysis.

The database also needs typed spans that are not ordinary symbol references.
Examples include `auto`, `this`, a literal, and a compound expression. A
`TypedOccurrence` can associate those spans with `ExpressionInfo` or
`BindingInfo`, allowing hover to show inferred types without inventing fake
symbols.

### Documentation

LSP provides fields for Markdown; it does not provide or maintain a symbol
database. GTI should make source declarations the canonical home of public
documentation:

```gti
/// Opens `path` for read-only access.
///
/// Returns an error when the file cannot be opened.
expected<File, IoError> open(std::string_view path);
```

The first documentation syntax should be consecutive `///` lines immediately
before a declaration. Their contents are normalized to Markdown and stored in
the declaration's `SymbolRecord`. Starting with one syntax avoids implementing
a Doxygen language accidentally.

Association should be deterministic:

- consecutive `///` lines form one documentation block;
- the marker and one optional following space are removed from each line;
- paragraph breaks, headings, inline code, lists, and fenced code blocks are
  preserved;
- the block attaches only to the next documentable declaration in the same
  source unit;
- a blank non-documentation line breaks the association;
- each overload owns its own documentation;
- documentation may attach to namespaces, aliases, classes, enums,
  enumerators, constructors, functions, methods, fields, and globals;
- local bindings and parameters do not need declaration documentation in the
  first implementation.

The lexer currently discards comments. The correct change is for the frontend
to retain documentation trivia and associate it with declarations. The LSP
must not scan comments independently and guess which declaration they belong
to. Ordinary comments can continue to have no semantic meaning.

Documentation for source-defined standard-library APIs belongs next to the
declarations under `stdlib/`. Compiler-only primitives without source
declarations may use a small compiler-owned documentation table keyed by
`BuiltinSymbolId`. An index cache may duplicate normalized Markdown, but that
cache remains rebuildable rather than authoritative.

Third-party libraries use exactly the same path. For example:

````gti
namespace graphics {
/// Loads an image from disk.
///
/// Returns an error if the image cannot be decoded.
///
/// # Example
///
/// ```gti
/// expected<Image, DecodeError> image =
///     graphics::load_image("player.png");
/// ```
expected<Image, DecodeError>
load_image(std::string_view path);
}
````

Once that source unit has been loaded by the frontend, or later appears in the
workspace index, `graphics::load_image` carries its own signature and Markdown
like any `std` declaration. There must be no `std::pow` or
`graphics::load_image` documentation table in `gti_lsp`. Public standard
library APIs are ordinary source-defined symbols; only true compiler builtins
without declarations need compiler-owned fallback text.

When emitted as LSP Markdown, source documentation should not allow executable
`command:` links or arbitrary raw HTML. Normal Markdown code, lists, and
ordinary web links are sufficient.

## Shared GTI Rendering

Hover and completion must display GTI, never generated C++ spellings or
backend-mangled names.

Extract semantic type spelling from the semantic analyzer into a reusable
`SemanticTypePrinter`. Add a `SignaturePrinter` that consumes resolved compiler
records rather than rebuilding signatures in `src/lsp/main.cpp`.

It should be able to render both declaration and instantiated forms:

```gti
T std::minimum<std::ordered T>(T left, T right)
```

```gti
int32_t std::minimum<int32_t>(int32_t left, int32_t right)
```

The printer must preserve GTI concepts such as `mut`, read-only and mutable
references, trailing mutable receivers, constraints, type packs, value generic
parameters, fixed-array extents, aliases, and exact namespace names. It must
not consult the C++ emitter.

For an alias, hover may show both the surface alias and canonical type:

```gti
using UserId = uint64_t
```

`UserId` should remain the primary spelling; `aka uint64_t` can be secondary
information when it helps explain type checking.

## Hover Query

The compiler-facing API should resemble:

```cpp
std::optional<HoverInfo>
LanguageQueries::hover(const FrontendResult &snapshot,
                       SourceUnitId unit,
                       std::size_t byteOffset) const;
```

`HoverInfo` is protocol-neutral:

```cpp
struct HoverInfo {
  SourceSpan range;
  std::string signature;
  std::optional<std::string> documentationMarkdown;
  std::vector<SemanticNote> notes;
};
```

The query proceeds as follows:

1. Find the narrowest symbol or typed occurrence at the byte offset.
2. Follow the semantic identity recorded for that occurrence.
3. If it is a call or construction, prefer the selected instantiated
   signature.
4. Render the source-level GTI signature with the shared printer.
5. Attach normalized declaration documentation and concise semantic notes.
6. Return the exact identifier or expression range that produced the hover.

Useful first-wave hover results include:

- functions, methods, operators, and constructors: selected signature and
  documentation;
- classes and structs: qualified declaration, generic parameters, and docs;
- enums and enumerators: enum type, backing type, and evaluated value;
- fields, globals, locals, and parameters: exact type, mutability, and whether
  the value is a read-only borrow or move-only owner;
- aliases: declared target and canonical type;
- `auto`: the inferred complete type;
- expressions: resolved type and value/place access when no more specific
  symbol is available.

### Overload-aware hover

Hover selection must use the semantic occurrence and its enclosing expression,
not only the token spelling.

For a function name with no uniquely selected call, the occurrence refers to
an overload set and hover may show every visible declaration:

```gti
uint64_t std::pow(uint64_t base, uint64_t exponent)
float std::pow(float base, float exponent)
```

For an analyzed call such as:

```gti
uint64_t base = 2;
uint64_t exponent = 8;
uint64_t result = std::pow(base, exponent);
```

`ResolvedCallInfo` already identifies the exact overload. Hover anywhere on
the callee name should therefore show only:

```gti
uint64_t std::pow(uint64_t base, uint64_t exponent)
```

Each overload retains its own documentation. If every overload has identical
documentation, the renderer may show the signatures followed by one shared
documentation body. Otherwise, it should keep each documentation body with
its signature. Large overload sets should be capped with an explicit
“additional overloads omitted” note rather than making the popup unbounded.

Example LSP-visible Markdown:

````markdown
```gti
int32_t std::minimum<int32_t>(int32_t left, int32_t right)
```

Returns the smaller of `left` and `right`.
````

The LSP handler converts that to `Hover { contents: MarkupContent,
range: Range }`. If no semantic occurrence exists, it returns `null`, not a
JSON-RPC error. Recovered programs may return partial hover information when
the relevant declaration was successfully analyzed.

Conceptually, the serialized response is:

```json
{
  "contents": {
    "kind": "markdown",
    "value": "```gti\nuint64_t std::pow(uint64_t base, uint64_t exponent)\n```\n\nRaises `base` to `exponent`."
  },
  "range": {
    "start": { "line": 4, "character": 16 },
    "end": { "line": 4, "character": 24 }
  }
}
```

The fenced declaration and documentation originate in the compiler query. The
LSP supplies only the JSON representation.

## Completion Query

Completion differs from hover because the program is commonly incomplete at
the cursor. For example, neither `value.` nor `std::` followed by end-of-file
is necessarily represented by a useful ordinary AST.

The final architecture should follow clangd's strongest idea: perform a
dedicated compiler parse with a synthetic completion token inserted at the
cursor. The token is an internal parser event and is never part of GTI source
syntax.

The compiler-facing entry point should accept source overlays and a byte cursor
just like normal frontend analysis, then return protocol-neutral candidates:

```cpp
CompletionResult
LanguageQueries::complete(const CompletionInput &input,
                          SourceUnitId unit,
                          std::size_t byteOffset) const;
```

```text
source bytes + byte cursor
        |
        v
lexer emits internal COMPLETION_POINT
        |
        v
parser records syntactic CompletionKind
        |
        v
semantic analyzer reaches the point with live scopes and expected type
        |
        v
compiler returns viable CompletionCandidate records
```

Proposed completion contexts include:

- unqualified expression name;
- type name;
- namespace-qualified name after `::`;
- member after `.`;
- owned member after `->`;
- enum member after `EnumType::`;
- call argument with an expected parameter type;
- generic type or value argument;
- declaration or statement keyword;
- standard-library include path.

The parser owns syntactic context and keyword candidates. Semantics owns names,
types, visibility, access, substitutions, and viability. The LSP owns neither.

### Candidate model

```cpp
struct CompletionCandidate {
  SymbolId symbol;
  CompletionKind kind;
  std::string label;
  std::string signature;
  SourceSpan replacementRange;
  std::string insertion;
  std::optional<Snippet> snippet;
  std::optional<std::string> documentationMarkdown;
  CompletionScore score;
};
```

Candidate collection must apply GTI rules directly:

- filter symbols that are not visible through the source graph;
- filter inaccessible private members;
- substitute generic class arguments before presenting members;
- respect read-only versus mutable receivers;
- resolve `->` through GTI's single checked arrow step;
- use exact argument and expected-type relationships, never conversion ranking;
- exclude values that flow analysis knows have been moved;
- distinguish type contexts from value contexts;
- preserve shadowing and declaration-order rules;
- expose active target-conditional declarations only.

Initially, completion should propose only symbols already visible to the
current unit. A later workspace index may suggest an unavailable symbol with an
explicit direct `include` edit. It must never silently rely on a transitive
include.

### Ranking

Ranking should be deterministic and compiler-informed. In descending
importance:

1. semantic viability and accessibility;
2. exact prefix, then case-insensitive prefix, then conservative fuzzy match;
3. nearest lexical scope and member context;
4. exact expected-type match;
5. locals and members before namespace, direct-include, and prelude symbols;
6. non-deprecated and documented declarations;
7. stable lexical tie-breaking.

Do not use ranking to hide ambiguous semantics. GTI still resolves an actual
call by one exact overload; completion merely presents possible source edits.
Each overload may initially be a separate item with a full signature in
`detail`. Grouping overloads can be added later if clients present duplicates
poorly.

### Insertions

The server should always return a `textEdit` that replaces the identifier
prefix rather than relying on client word-boundary guesses.

When the client advertises snippet support, a function candidate may provide:

```text
minimum(${1:left}, ${2:right})
```

Without snippet support, insert only the name or a plain argument delimiter
according to a future GTI completion setting. The first implementation should
avoid automatic include insertion and additional edits.

## LSP Protocol Surface

During `initialize`, remember the client's completion and Markdown
capabilities. Advertise at least:

```json
{
  "hoverProvider": true,
  "completionProvider": {
    "triggerCharacters": [".", ">", ":"],
    "resolveProvider": true
  }
}
```

Target the common LSP 3.17/3.18 feature subset rather than requiring a custom
extension. Respect `hover.contentFormat`, completion documentation formats,
snippet support, label-detail support, insert/replace edit support, and the
properties the client permits `completionItem/resolve` to fill. Emit plaintext
when the client does not advertise Markdown.

`>` triggers owned-member completion after `->`, and `:` triggers qualified
completion after the second colon in `::`. Manual completion remains available
in every supported context. Include-path triggers may be added after include
completion exists.

Implement these methods:

- `textDocument/hover`;
- `textDocument/completion`;
- `completionItem/resolve`.

Return a `CompletionList`, not only a bare array, so `isIncomplete` can request
refiltering while a result was capped or a workspace index is still warming.

The initial completion item should contain cheap fields:

- `label` and, when supported, `labelDetails`;
- `kind` mapped from compiler `SymbolKind`;
- `detail` containing the GTI signature;
- `filterText`, `sortText`, and a precise `textEdit`;
- `insertTextFormat` only when using snippets;
- opaque `data` identifying the server session, analysis snapshot, and symbol.

`completionItem/resolve` lazily attaches Markdown documentation. If its
snapshot has been evicted, the server may resolve the stable global key in the
current snapshot; otherwise it should return the original item unchanged.

Typical kind mappings are:

| GTI symbol | LSP completion kind |
| --- | --- |
| function | Function |
| method or operator | Method |
| constructor | Constructor |
| field | Field |
| local, global, or parameter | Variable |
| class | Class |
| struct | Struct |
| enum | Enum |
| enumerator | EnumMember |
| namespace | Module |
| type alias | Reference |
| generic parameter | TypeParameter |

No GTI-specific protocol extension is needed for the first implementation.

## Snapshot And Scheduling Model

Extend the existing background analysis result rather than adding an unrelated
LSP parser:

```cpp
struct AnalysisSnapshot {
  std::uint64_t id;
  std::string rootUri;
  std::uint64_t rootGeneration;
  std::unordered_map<std::string, std::uint64_t> overlayGenerations;
  FrontendResult frontend;
  std::unordered_map<SourceUnitId, PositionIndex> positions;
};
```

The committed snapshot should be held through
`std::shared_ptr<const AnalysisSnapshot>`. This keeps the AST alive for every
pointer-backed semantic record and allows read requests to run without holding
the mutable server-state lock.

The current LSP position index converts bytes to UTF-16 positions only. Query
support also needs the checked inverse conversion from an LSP `(line,
character)` to a UTF-8 byte offset. Cache both directions per source unit and
reject positions that split a UTF-16 surrogate pair or lie beyond the line.

On a successful generation check, the analysis worker commits the whole
snapshot together with diagnostics and dependency information. Editing the
root or any open dependency invalidates that snapshot for exact live queries.

Hover normally reads the newest committed snapshot. If analysis for the
current generation is pending, schedule the read after that write rather than
returning semantic facts for different source bytes.

Completion is more latency-sensitive and requires its own completion-point
parse. Give it a high-priority bounded worker queue using the newest document
and overlay snapshot. A new edit or `$/cancelRequest` cancels obsolete work.
The JSON-RPC input loop must never block waiting for compilation.

The current single global analysis worker is sufficient for diagnostics today
but would let a large unrelated root delay completion. Evolve it toward
per-root serialization on a small bounded thread pool:

- writes for one root are coalesced;
- reads observe the writes queued before them;
- different roots may progress independently;
- completion work has a separate latency-sensitive path;
- CPU, memory, and retained snapshot counts remain bounded.

Warm targets should be approximately 50 ms or less for completion and 20 ms or
less for hover on ordinary files. These are engineering targets to benchmark,
not language guarantees.

## Workspace Index

A clangd-style workspace index is useful later, but it is not required for
correct first-wave hover and visible-name completion. The whole-program
frontend already loads the entry unit and its dependency graph.

When GTI needs completion for unopened or currently unimported declarations,
add a rebuildable `WorkspaceSymbolIndex` containing only durable records:

- stable symbol key;
- name, qualified name, and kind;
- declaration and definition locations;
- normalized callable/type summary;
- documentation Markdown;
- owning source unit or standard-library logical include;
- flags useful for completion ranking.

Layer the live index above the workspace index so unsaved source always wins:

```text
current immutable analysis snapshot
              |
              v
        live file index
              +------ merged query ------ workspace index/cache
```

Do not persist local variables, AST pointers, `SemanticType` object addresses,
or selected call occurrences. Cache entries must be versioned by compiler
version, target, canonical source identity, content hash, and standard-library
version. The cache can be added only after measurement demonstrates that
rebuilding an in-memory index is a real cost.

LSIF is a separate interchange/indexing format and SQLite is a storage choice;
neither provides GTI semantics automatically. The compiler records the facts
regardless of the eventual storage format.

## Failure And Recovery Behaviour

- An unknown URI or invalid position returns `null` hover or an empty
  completion list.
- A source-loading failure still permits lexical or keyword completion when the
  parser can establish the context, but must not fabricate imported symbols.
- Recovered declarations and successfully resolved occurrences remain usable
  after unrelated parse or semantic errors.
- A stale or cancelled request must never publish a result for a newer document
  version.
- Hover and completion failures must not crash the server or publish compiler
  diagnostics as JSON-RPC errors.
- No query may reach the C++ backend or native compiler.

## Implementation Phases

### Phase 1: semantic records and hover

The 0.42.0 first pass implemented shared type/signature rendering, an initial
compiler-owned occurrence database, immutable LSP frontend snapshots, checked
UTF-16 request positions, overload-aware `LanguageQueries::hover`, capability
advertising, and compiler/LSP protocol tests. GTI 0.44.0 replaces identity by
declaration pointer with snapshot-scoped symbol records and covers the current
source-facing declaration and resolved-use set. Explicit scope records, full
node extents, and queued hovers remain future work.

1. Add source ranges for declarations and scopes where the current AST does not
   retain enough extent information.
2. Add compiler-owned `SymbolId`, `ScopeId`, symbol records, and occurrence
   records during semantic resolution.
3. Extract shared semantic type and signature printers.
4. Retain `FrontendResult` in immutable LSP analysis snapshots.
5. Add `LanguageQueries::hover` and the LSP hover handler.
6. Advertise `hoverProvider` and add protocol tests.

This phase can ship useful signature/type hover before documentation comments
exist.

### Phase 2: documentation

1. Retain `///` documentation trivia in the frontend.
2. Associate documentation with declarations and symbol records.
3. Add documentation to hover.
4. Add reusable documentation lookup and Markdown sanitization.
5. Document public prelude and standard-library declarations in source.

### Phase 3: semantic completion

The 0.43.0 first pass implements steps 1, 2, 4, 5, and 7 plus the first part of
step 3 for visible expression names, namespace members, scoped enumerators, and
`.` members. It uses one bounded completion worker, source overlays, generation
rejection, and compiler-owned candidate details. `resolveProvider` remains
false until Phase 2 supplies source documentation. Explicit `$/cancelRequest`,
richer parser contexts, checked `->` completion, and measured queue latency
remain before this phase is complete.

1. Add the internal completion-point lexer/parser path.
2. Record `CompletionContext` from semantic scope state.
3. Implement unqualified, qualified, enum, `.` member, and `->` member
   candidates.
4. Add deterministic ranking and exact replacement edits.
5. Negotiate snippets and completion item capabilities.
6. Add `completionItem/resolve` using the documentation lookup.
7. Advertise `completionProvider` and add protocol tests.
8. Add the high-priority cancellable query queue.

### Phase 4: broader tooling reuse

GTI 0.44.0 begins this phase with exact current-snapshot go-to-definition and
symbol-driven semantic-token resolution. Build signature help, document
symbols, references, and rename on the same symbol and occurrence data. This
phase is an important check that the database is compiler infrastructure rather
than a hover-specific cache.

### Phase 5: workspace indexing and import completion

Add live/workspace index merging, persistent cache only if needed, and explicit
direct-include edits for out-of-scope symbols. Keep these suggestions below
already-visible candidates.

## Verification Plan

Compiler-level tests should cover:

- symbol identities for declarations, references, overload sets, and selected
  calls;
- nested scope lookup, shadowing, and declaration order;
- direct include, prelude, transitive include, and sibling visibility;
- private access and read-only versus mutable receivers;
- class generic substitution, value generics, constraints, and exact overloads;
- alias display and canonical types;
- move-only, borrowed, and moved-from bindings at a completion point;
- `auto`, references, arrays, enums, constructors, operators, and lambdas;
- documentation attachment, blank-line boundaries, Markdown preservation, and
  command-link sanitization;
- incomplete input at every completion context;
- inactive target branches;
- multibyte UTF-8 source around the byte completion point.

LSP protocol tests should cover:

- advertised `hoverProvider` and `completionProvider` capabilities;
- UTF-16 request positions and response ranges;
- hover code fences, signatures, inferred types, documentation, and `null`;
- completion kinds, order, replacement edits, snippets, and `isIncomplete`;
- `completionItem/resolve` preserving the original item's `data`;
- unsaved direct includes and dependency invalidation;
- stale generations, rapid edits, cancellation, and snapshot eviction;
- malformed programs returning partial results without server failure;
- existing diagnostics, formatting, and semantic-token behaviour remaining
  responsive.

A Neovim smoke test only needs to establish that the attached client advertises
hover and completion support. LazyVim owns its UI and `K` key mapping; protocol
behaviour should remain editor-independent.

## Explicit Non-goals For The First Release

- no second semantic analyzer inside `src/lsp/main.cpp`;
- no SQLite or mandatory on-disk index;
- no automatic include insertion;
- no completion of invisible transitive declarations;
- no AI or probabilistic completion;
- no backend C++ names or native type information;
- no Doxygen parser or arbitrary documentation directives;
- no signature help, rename, or references in this layer;
- no LSP-specific key mappings in the GTI Neovim plugin.

## References

- [Language Server Protocol 3.18 completion request](https://github.com/microsoft/language-server-protocol/blob/gh-pages/_specifications/lsp/3.18/language/completion.md)
- [Language Server Protocol 3.18 hover request](https://github.com/microsoft/language-server-protocol/blob/gh-pages/_specifications/lsp/3.18/language/hover.md)
- [clangd code architecture](https://clangd.llvm.org/design/code)
- [clangd thread and request scheduling](https://clangd.llvm.org/design/threads)
- [clangd symbol index](https://clangd.llvm.org/design/indexing)
- [clangd user-facing completion and hover behaviour](https://clangd.llvm.org/features)
