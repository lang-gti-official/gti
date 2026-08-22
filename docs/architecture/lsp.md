# GTI Language Server Architecture

Status: Current implementation, with future symbol/index work explicitly
separated below.

The LSP is an adapter over compiler-owned semantics:

```text
JSON-RPC/LSP
  -> protocol conversion in src/lsp/main.cpp
  -> open-document state and generation checks
  -> immutable FrontendResult snapshot
  -> LanguageQueries / SemanticDatabase
```

## Semantic Source Of Truth

`FrontendResult` owns the recovered AST, `SemanticModel`, `SemanticDatabase`,
HIR/MIR, source graph, source text, and diagnostics together. The LSP retains a
`shared_ptr<const FrontendResult>` in each `AnalysisSnapshot`. This keeps AST
pointers and snapshot-scoped symbol IDs valid while a request reads them.

`LanguageQueries` is declared in `include/gti/language_queries.h`; reusable
hover, completion, definition, and signature rendering compile in
`src/compiler/language_queries.cpp`. Semantic tokens use compiler symbol and
occurrence facts for identifiers and lexical facts for keywords, literals,
operators, and comments. Line and block comments are split into per-line LSP
tokens with UTF-16 lengths; delimiters inside literals are excluded. The
protocol layer must not repeat name lookup, overload selection, type rendering,
or declaration parsing.

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

The `sizeof` and `alignof` reserved words use the compiler's operator token
classification, while invalid operands publish the shared `GTI-S2063`
diagnostic. The LSP does not evaluate layout or maintain a second supported-type
list.

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
fixed-array extents. Generic-constructor declaration hover retains its
constructor-local `uint64_t` parameter, while selected construction hover uses
the exact value and parameter array type from `ResolvedConstructionInfo`. Method
calls also apply the concrete receiver specialization retained in
`ResolvedCallInfo::dispatchOwner`, including generic class type arguments.
Forwarding-only parameters are identified without inventing a signature. The
protocol adapter only serializes these notes. Hovering the parameter binding
itself remains ordinary binding hover until semantic occurrences retain its
owning function and parameter contract directly.

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

## Document State

`LanguageServer` currently owns protocol handling and document lifecycle in one
large class. It stores unsaved source overlays, client versions, internal
analysis generations, dependency generations, immutable snapshots, diagnostic
sets, and semantic-token caches.

- `didOpen`/`didChange` replace the full synchronized source, advance the
  generation, invalidate the prior snapshot/cache and affected dependants, and
  schedule analysis over all open-buffer overlays.
- `didClose` removes the overlay and snapshot, clears diagnostics, invalidates
  pending work, and reanalyzes dependants whose view changes.
- Completed analysis is published only if the document and dependency
  generations still match. Stale semantic requests are rejected.
- The dynamic file watcher covers both `**/*.gti` and `**/gti.toml`. A manifest
  change invalidates semantic-token caches and reanalyses every open document,
  so changing `[build].defines` flips the selected/inactive branches.

Analysis and completion have separate bounded worker queues. This keeps the
JSON loop responsive and lets newer completion/analysis work supersede older
requests without adopting clangd's per-translation-unit scheduler complexity.

Editor analysis requests `FrontendOptions::stopAfter = Semantics`: no LSP
feature reads HIR or MIR, so those phases are not lowered per change. The
validity flags of skipped phases stay false and code generation remains
disabled, which is already the LSP contract. Before analysis, the LSP uses the
driver's canonical nearest-manifest discovery and parser to seed
`FrontendOptions::configurationFlags` from `[build].defines`; it does not
reimplement TOML or manifest validation. Analysis work additionally runs
inside `lang::runGuarded`, and that boundary is deliberately narrow. Only
compiler work runs under the guard: `runIsolatedAnalysis` builds an isolated
`DocumentAnalysis` and its immutable frontend snapshot, and `publishAnalysis`
writes to shared LSP state afterwards, on the normal path.

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

- Documentation comments are not retained by the compiler, so hover's optional
  Markdown documentation field is normally empty.
- Editor snapshots stop after symbolic semantic analysis. Diagnostics that
  require concrete generic instantiation during HIR lowering are therefore
  reported by a full compile, not by the current LSP snapshot. Moving concrete
  instance checking into an editor-safe compiler query is future work; the LSP
  must not reproduce it independently.
- Definition, references, document highlights, and rename are implemented
  from exact resolved symbols. Rename is deliberately limited to
  function-local names: identities visible outside the entry document's
  source graph may have references in unopened dependants the snapshot cannot
  see, so the compiler query rejects them instead of producing an incomplete
  edit. Signature help and a project symbol index are not implemented.
- `SymbolId` is snapshot-local. There is no durable cross-analysis identity.
- Document/scheduling state is not yet extracted from the protocol class.
- `Frontend`/`LanguageQueries` can consume the same explicit
  `PackageSourceRoot` graph as compilation. The LSP now consumes the nearest
  manifest's configuration flags, but does not yet resolve its workspace graph,
  selected package/profile, command-line overrides, or execution profile.
  Consequently, package angle includes are not resolved in editor snapshots
  yet, and editor analysis currently uses the default single-threaded execution
  profile; project-selected concurrent-global diagnostics remain a project
  CLI/build check until that configuration is shared.
- Crash containment is in-process. It keeps the worker and shared state
  usable after a contained fault, but it is not a process-isolation
  guarantee: heap damage done before a fault is not undone.

Those items are tracked in [`docs/plans/lsp-evolution.md`](../plans/lsp-evolution.md).
[ADR 005](../decisions/005-lsp-compiler-semantics.md) records why language
features share compiler semantics.

## Do Not Copy From clangd Yet

GTI does not need C++ preambles/PCH, compile-command borrowing, macro
spelling/expansion locations, background index shards, remote indexes, AST LRU
caches, or one worker per translation unit. Preserve the boundaries that allow
measured evolution without importing those systems preemptively.
