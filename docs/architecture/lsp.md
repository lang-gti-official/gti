# GTI Language Server Architecture

Status: Current implementation, with future symbol/index work explicitly
separated below.

The LSP is an adapter over compiler-owned semantics:

```text
JSON-RPC/LSP
  -> protocol conversion in src/lsp/main.cpp
  -> open-document state and generation checks
  -> exact frontend source snapshot / bounded parsed-unit cache
  -> immutable FrontendResult snapshot
  -> LanguageQueries / SemanticDatabase
```

## Semantic Source Of Truth

`FrontendResult` retains the recovered AST, `SemanticModel`, `SemanticDatabase`,
HIR/MIR, source graph, source text, and diagnostics together. The LSP retains a
`shared_ptr<const FrontendResult>` in each `AnalysisSnapshot`. This keeps AST
pointers and snapshot-scoped symbol IDs valid while a request reads them.
Compatible snapshots may share immutable parsed statement trees through the
frontend cache, but every snapshot constructs a fresh semantic model and
database. Symbol IDs, semantic records, and pointers into one semantic model
are never cache values.

`LanguageQueries` is declared in `include/gti/language_queries.h`; reusable
hover, completion, definition, and signature rendering compile in
`src/compiler/language_queries.cpp`. Semantic tokens use compiler symbol and
occurrence facts for identifiers and lexical facts for keywords, literals,
operators, and comments. Line and block comments are split into per-line LSP
tokens with UTF-16 lengths; delimiters inside literals are excluded. The
protocol layer must not repeat name lookup, overload selection, type rendering,
or declaration parsing.

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

The checked integer functions are ordinary `<std/numeric>` declarations.
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

`constexpr` constructors and read-only aggregate methods use the same compiler
signature printer as ordinary calls, so declaration and selected-call hover
retain the qualifier and canonical concrete signature. Unsupported aggregate
evaluation publishes compiler-owned `GTI-S2057` detail, hints, and related
locations. The protocol adapter has no aggregate evaluator or diagnostic
allowlist, and a corrected document snapshot clears the stale failure.

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
it only after a newer compiler snapshot accepts the receiver. Hover and
signature help render the exact selected trailing-`mut` overload; the protocol
layer does not infer freshness from constructor-like punctuation.

Fresh temporary arguments use the same compiler snapshot. An ineligible
read-only-reference argument publishes the compiler-owned `GTI-S2017` reason
and related declaration; a newer valid snapshot clears it. Signature help
renders the exact selected borrowed overload when the fallback tier is active
and the exact by-value overload when the ordinary tier wins. The protocol
layer does not classify producers, rank candidates, or extend lifetimes.

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
LSP reads the default project profile as described below and exposes only the
source-defined `std::thread`, `std::thread_result<Result>`, `std::atomic<T>`
(including integer fetch operations), `std::mutex<T>`, and
`std::mutex_guard<T>` declarations in hover/definition/completion. It filters
the identity-bound `gti_internal` thread handles, atomic/mutex storage,
lock tokens, and operations. It does not derive task transfer, callable entry
selection, result ownership, atomic order legality, protected-value provenance,
lock lifetime, or concurrency from public spelling.

## Document State

`LanguageServer` still owns protocol handling, open overlays, client versions,
analysis generations, dependency relationships, immutable result snapshots,
diagnostic sets, and semantic-token caches. Exact reusable-frontend input state
is extracted behind `FrontendSnapshotState` in `src/lsp/`. That component owns
the shared `FrontendCache`, per-canonical-source generations, and exact digests
learned from successfully published analyses. Workers receive a value snapshot
of those identities; the JSON-RPC adapter does not expose its mutable maps to
compiler work.

- `didOpen`/`didChange` replace the full synchronized source, advance the
  document and canonical-source generations, erase the prior learned digest,
  invalidate affected dependants, and schedule analysis over all open-buffer
  overlays. A stale result snapshot may remain allocated until its replacement
  is published, but generation checks make it inaccessible to requests.
- `didClose` removes the overlay and snapshot, clears diagnostics, invalidates
  the overlay source identity and pending work, and reanalyzes dependants whose
  view changes. The next analysis must read the disk source before learning its
  new digest.
- Completed analysis is published only if the document and dependency
  generations still match. Only then may its source digests be learned. Stale
  semantic requests and stale digest publications are rejected.
- The dynamic file watcher covers both `**/*.gti` and `**/gti.toml`. A manifest
  change invalidates semantic-token caches and reanalyses every open document,
  so changing `[build].defines` flips the selected/inactive branches.

### Reusable frontend work

The process-local frontend cache is a thread-safe LRU bounded by both 256 rows
and 64 MiB of conservatively estimated input/tree storage. Lexical rows retain
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

This is deliberately not a semantic snapshot cache. Semantic analysis and HIR
still run over the complete compatible parsed program, producing new
snapshot-local identities and records. The benchmark exposes exact loaded,
lexed, parsed, semantically analysed, and cache-hit unit lists plus phase times
through the opt-in `GTI_LSP_FRONTEND_TRACE` stream. The same report records the
configured cache limits and observed estimated occupancy.

Analysis and completion have separate bounded worker queues. This keeps the
JSON loop responsive and lets newer completion/analysis work supersede older
requests without adopting clangd's per-translation-unit scheduler complexity.
Both queues use one long-lived LLVM thread with an explicit 8 MiB stack. That
avoids Darwin's 512 KiB secondary-thread default and ensures the compiler's
bounded-depth recovery runs before native stack exhaustion in debug builds,
without creating a new thread for every edit or completion request.

Editor analysis requests `FrontendOptions::stopAfter = Hir`. HIR is the
compiler-owned discovery point for concrete generic instances, so stopping at
symbolic semantics made valid editor snapshots omit diagnostics that
`gti check` reports for instantiated generic bodies and dependent
`static_assert` declarations. The LSP does not query HIR directly, and it still
does not build failure metadata or MIR; retaining the HIR result is the current
cost of publishing the compiler's complete source-level semantic diagnostic
set. Code generation remains disabled. Before analysis, the LSP uses the
driver's canonical nearest-manifest discovery and parser plus the immutable
driver-owned `AnalysisConfiguration`. It seeds configuration flags from
`[build].defines`, selects the default `dev` execution profile, and derives
runtime capabilities from the installed hosted runtime profile; it does not
reimplement TOML, target, or capability policy. A startup
`--execution-profile single-threaded|concurrent` option overrides the manifest
profile and also configures standalone documents. Omission selects the
manifest `dev` profile when present and otherwise remains single-threaded.
This keeps standalone and concurrent-project diagnostics and the public
`<std/thread>`, `<std/atomic>`, and `<std/mutex>` surfaces aligned with
compiler-driver workflows. Analysis work
additionally runs inside `lang::runGuarded`, and that boundary is deliberately
narrow. Only compiler work runs under the guard: `runIsolatedAnalysis` builds
an isolated `DocumentAnalysis` and its immutable frontend snapshot, and
`publishAnalysis` writes to shared LSP state afterwards, on the normal path.

Two properties make the boundary sound:

- **No exception crosses LLVM.** LLVM's `CrashRecoveryContext` is built
  without C++ exception support, so the guarded callback catches its own
  exceptions and returns the outcome as data (`GuardedAnalysis`). The worker
  inspects that status instead of relying on an exception unwinding through
  an LLVM frame.
- **No lock is held under the guard.** The guarded callback touches no shared
  state and acquires no mutex, so a signal-recovery stack restore that skips
  destructors cannot leave `stateMutex` owned. Publication acquires the lock
  on a normally unwound path, where `lock_guard` behaves normally.

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

`src/lsp/main.cpp` owns JSON-RPC IDs, capabilities, URIs, UTF-8 byte to LSP
position conversion, request/result serialization, diagnostic publication,
workspace edits, cancellation, and semantic-token wire encoding. It advertises
full document sync, formatting, semantic tokens, hover, completion,
definition, references, document highlights, document symbols, and rename
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

Diagnostic serialization preserves compiler codes, severities, phases, hints,
related locations, and fix-its while respecting the client's
`publishDiagnostics` capabilities. Hints stay in the plain-text message for
universal display and are also exposed as structured data when supported.
When several open roots diagnose the same dependency span, publication merges
their related information instead of selecting one root arbitrarily; distinct
include and instantiation locations therefore remain visible on the one
deduplicated diagnostic. Diagnostics with different hints or fixes remain
distinct.
Quick fixes come only from compiler `Diagnostic::fixes` associated with the
current source/generation, an intersecting request range, and an exact
client-provided diagnostic. Position conversion is tested at the protocol
edge; compiler source spans remain half-open UTF-8 byte ranges.

## Incomplete Source

Editor analysis enables parser recovery and semantic analysis of recovered
declarations. Completion runs a dedicated frontend analysis with a cursor
marker so the parser/analyzer can capture real scope, receiver, visibility, and
overload candidates in fragments such as `object.` or `foo(`. Features may
return partial or no semantic results; they must not confidently invent missing
meaning from punctuation.

## Current Limits

- Retained documentation is consumed by hover, completion, and signature
  help. Generated API reference output does not yet publish the same
  compiler-owned Markdown.
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
- Semantic analysis and HIR remain whole-program work on every accepted edit.
  They are the largest remaining repeated-edit cost; no semantic checkpoint or
  durable symbol identity is implemented.
- `Frontend`/`LanguageQueries` can consume the same explicit
  `PackageSourceRoot` graph as compilation. The LSP now consumes the nearest
  manifest's configuration flags and default `dev` execution profile, with an
  explicit standalone execution-profile startup override, but does not yet
  resolve its workspace graph, selected package, or a named non-default
  manifest profile. Consequently, package angle includes are not resolved in
  editor snapshots yet. A project whose `dev` profile selects `concurrent`, or
  a standalone server launched with `--execution-profile concurrent`, receives
  the same managed-thread and concurrent-global semantic checks as the matching
  compiler-driver workflow; richer project selection still awaits the shared
  project-facts model.
- Crash containment is in-process. It keeps the worker and shared state
  usable after a contained fault, but it is not a process-isolation
  guarantee: heap damage done before a fault is not undone.

Those items are tracked in [`docs/plans/lsp-evolution.md`](../plans/lsp-evolution.md).
[ADR 005](../decisions/005-lsp-compiler-semantics.md) records why language
features share compiler semantics.

## Do Not Copy From clangd Yet

GTI does not need C++ preambles/PCH, compile-command borrowing, macro
spelling/expansion locations, background index shards, remote indexes,
clangd-style full semantic translation-unit AST caches, or one worker per
translation unit. Its bounded exact-content parsed-unit reuse is narrower and
contains no semantic records. Preserve the boundaries that allow measured
evolution without importing those systems preemptively.
