# GTI Compiler Roadmap Status

> **Plan status:** Implementation checkpoint and future-work ledger. It does
> not define current language semantics.

Status: implementation checkpoint

Checkpoint version: 0.98.0

This document records where the compiler currently sits against
[`roadmap-to-1.0.md`](roadmap-to-1.0.md). The roadmap remains the durable
capability and release plan; the operational dependency queue lives in
[`implementation-sequence.md`](implementation-sequence.md). This checkpoint is
the shorter evidence ledger that future passes should update when they complete
or materially unblock a milestone. The grammar and semantic specification
remain authoritative for shipped language behavior.

## Current Position

The review found no new competing semantic authority or dependency-order
drift. Source loading, parsing, semantic selection, concrete HIR discovery, and
structural MIR lowering remain one directional. The C++ backend consumes
frontend facts instead of deciding overloads, ownership, dispatch, or language
validity.

The 0.98.0 checkpoint adds multi-parameter source concepts and bounded,
validity-only trailing `requires` conjunctions. Exact input-iterator,
iterator/sentinel, and homogeneous accumulation relationships now support a
source-defined `std::accumulate`; concrete generic reanalysis retains the
selected operator identities through HIR, and formatter, Tree-sitter, LSP, and
editor surfaces share the frontend representation. General
requires-expressions, specialization, subsumption, and constraint-based
overload ranking remain outside this checkpoint under ADR 009.

The 0.98.0 checkpoint extends managed project-native compilation to declared,
package-contained `.cpp`, `.cc`, and `.cxx` sources. They use the already
resolved C++ compiler, project standard and optimization, shared include paths,
and ordered native `compile-args`; atomically published C++ objects follow C
objects and precede runtime/manifest operands in the final link. Metadata schema
4 reports both source categories, while failed C++ source compilation preserves
the prior object and executable.

The 0.97.0 checkpoint completes I-CAP-01. The source graph distinguishes
application, implicit prelude, and physical configured standard-library roles
without allowing an override-only path to mint trust. Root `gti_internal`
declarations, references, and alias targets are application errors under
`GTI-S2058`. Private owner, storage, and text-view types bind through their
exact trusted-prelude class identities; aliases and signatures retain privacy,
the C++ backend consumes semantic capability facts, and shared language queries
filter completion, hover, definition, and semantic classification for
application documents. Public `std` wrappers remain ordinary GTI source.

The 0.96.0 checkpoint makes `interface` the complete source-level abstraction
for pure behavior contracts. Interface methods are declaration-only signatures
ending in `;`; the semantic model supplies virtual/pure identity, and the C++
backend lowers that identity to `virtual ... = 0;`. The redundant C++ pure
specifier is now diagnosed inside an interface, while ordinary classes retain
their existing `virtual`, `override`, and `= 0;` syntax.

The 0.95.0 checkpoint extends project-native inputs with declared,
package-contained C sources. Project builds resolve the selected C compiler,
standard, include paths, and C-only arguments; compile each source to a staged,
atomically published object; and place those objects before the runtime and
manifest libraries in the existing final C++ link. Metadata schema 3 reports
the effective C inputs, while `check` remains compiler-free and output-free.

The 0.94.0 checkpoint adds the first transforming owned-MIR slice without
changing emitted behavior. At `-O1+`, exact primitive literals flowing only
through grouping identities are rewritten in shadow MIR and compared with the
existing HIR constant result. A narrow GTI-owned editor validates guarded
body/`{block,index}` patches as one batch, repairs value uses, records precise
invalidation, preserves instruction/result/provenance identity and CFG
dominance, and verifies a copied program before atomic commit. Strings,
arithmetic, conversions, dynamic values, general pass management, analysis
caches, and MIR-controlled emission remain outside this slice.

The 0.93.0 checkpoint adds the owned hosted entry form
`int main(int, std::vector<std::string>)` alongside `int main()`. Semantic
analysis validates canonical standard-library type identities and resolves the
source-defined append operation once; HIR and MIR preserve the entry kind and
that exact callable identity, and MIR verification guards them against drift.
The C++ backend emits a private native `argc`/`char**` adapter that copies every
argument into owned GTI values before invoking the source entry. This does not
add pointer-to-pointer source types, borrowed native storage, environment
access, or general compiler-provided vector behavior.

The checkpoint also completes D-MEM-01 and D-MEM-02. The
[concurrency and memory-model proposal](concurrency-memory-model.md) is retained
as design evidence, while
[ADR 008](../decisions/008-safe-concurrency-memory-model.md),
[Execution §4.9](../language/execution.md#49-concurrency-boundary), and
[ownership semantics](../language/ownership-and-lifetimes.md#concurrency-transfer-and-sharing)
now adopt safe data-race freedom, structural transfer/share facts, explicit
single-threaded/concurrent profiles, an owned-only automatic-join first thread
boundary, sequentially consistent scalar atomics, concurrent-global policy,
contained worker failure, and explicit synchronization effects. This changes
no current executable behavior and does not authorize public concurrency;
transfer/share and global policy remain pre-1.0 implementation work, while the
public profile remains post-1.0.

Design-only D-CALL-01 is also complete in the accepted
[callable ownership and escape contract](callable-ownership-and-escape.md).
Lexical closures, nominal callable objects, and future exact function items
share one GTI-owned concrete identity/signature model; read-callable,
mut-callable, and once-callable distinguish receiver access and invocation
count independently from copy/move/drop and future transfer/share facts. The
contract defines immutable-copy and explicit-owned-move capture, confined
versus exact generic owned transport, lifecycle/escape diagnostics, and one
cross-phase vocabulary for algorithms, consumed tasks, and native callbacks.
It changes no current lambda behavior: L-CALL-01 remains blocked on M-LIFE-01,
while C-CALL-01 and S-CALL-01 keep their failure, concurrency, and ABI gates.

Design-only D-FAIL-01 is complete in
[Execution §4.10](../language/execution.md#410-defined-runtime-failure), with
rationale in [ADR 007](../decisions/007-defined-runtime-failure.md). Defined failure now has
stable categories and artifact-qualified source sites, cleanup-preserving
non-resumable propagation, an exact hosted report and status 70, a constrained
observer, explicit program/embedding/task/callback boundaries, and a precise
`expected`/infallible split. ADR 008 incorporates that contained worker
failure and requires explicit or automatic join to re-raise the original
record. It does not change current execution: the emitter still aborts
without cleanup/location/category preservation and wrong-state expected access
still inherits native behavior until M-LIFE-01, the relevant M-EXEC-01 slices,
co-delivered M-FAIL-01/Q-FAIL-01, and complete M-BACK-02 body migration land.

D-LANG-01 is now complete in the maintained
[language restriction ledger](language-alignment.md). It classifies every
external language-audit finding, original alignment question, explicit
language-specification gap, and backend-visible restriction with one reason,
v1 horizon, owner, and evidence gate. Bounded layout queries, defined integer
modes, and binary64 are v1 work; public concurrency, broad native ABI/manual
allocation, sums, propagation syntax, and broader operators are post-1.0.

The 0.92.0 checkpoint closes the floating-point Milestone 0 contract with an
exact GTI-owned IEEE-754 binary32 representation. Decimal literals, constexpr
and optimizer arithmetic, comparisons, and numeric conversions use private
LLVM `APFloat` computation with explicit rounding; emitted constants preserve
their exact bits, and the native driver enforces the matching strict floating
flags. It also adds a private, full-recomputation LLVM generic-dominator
adapter whose GTI block-ID result lets MIR verification reject same-block
use-before-definition and reachable uses not dominated by their definitions.
Neither LLVM representation crosses a public header or becomes cross-phase
authority. Type interning, loop analysis, and incremental dominance remain
deferred until ownership, clients, and measurements justify them.

The 0.90.0 checkpoint consolidates compiler-engineering support onto one
mandatory LLVM-backed build. A compatible system LLVM and the pinned bundled
LLVM are dependency-acquisition choices, not separate implementations. The
portable checked-integer and HIR hash implementations displaced by the LLVM
versions remain temporarily under `archive/` as non-built reference material.
This checkpoint does not adopt LLVM as a backend or transfer GTI-specific
language semantics, HIR, MIR, diagnostics, or code generation to LLVM. It also
records that the current LSP crash guard is best-effort and still needs a
state-safe isolation boundary.

The 0.89.0 checkpoint adds bounded local exclusive reborrows without adding
syntax. A mutable loan may produce a distinct mutable or read-only child over
a stable symbol/receiver root with named-field and checked-dereference
projections. The parent is suspended while any overlapping child remains active
and fully reactivates only after its final active child reaches a frontend-
selected endpoint. Nested chains compose, while known-disjoint named-field
children may coexist and leave disjoint projected parent access available.
Indexed, raw, and opaque sources, mutable stored-reference fields, any local
child escape (mutable or read-only, direct or through a stored carrier), and
mutable owner-tied range-for iteration remain outside this slice.

The 0.88.0 checkpoint extends precise retained-loan endings to shared
read-only aliases. Every alias remains a carrier of one semantic loan; uses
from every carrier feed the existing path-aware planner, and HIR/MIR preserve
one frontend-selected endpoint set and active-state identity. Straight-line,
conditional, loop, switch, and proven break-path shapes therefore permit owner
mutation after all reachable alias uses. That checkpoint still rejected
mutable aliasing because it lacked the explicit child-loan and suspension
transitions added in 0.89.0.

The 0.87.0 checkpoint extends bounded compile-time programming with
non-generic constexpr free functions and static methods, scalar locals,
mutation, structured control flow, recursion, and nested constexpr calls.
`if constexpr` is selected by semantics and only its chosen branch reaches HIR,
MIR, and C++ emission. One compiler-owned evaluator enforces checked primitive
operations, a shared 4096-step budget, and a 64-call-depth limit. Generic and
instance-function execution, class values, references, floating point,
allocation, and runtime/C/intrinsic calls remain explicit later slices.

The 0.86.0 checkpoint introduced compiler-evaluated `constexpr` scalar
bindings and `static constexpr` class fields. Computed values remain in
semantic bindings and HIR, supply concrete fixed-array extents and uint64_t
value-generic arguments, and are serialized by the C++ backend even at `-O0`.

The 0.85.0 checkpoint carries one read-only owner relationship
through resolved calls, concrete generic carrier instances, explicit moves,
checked returns, and drops. Instance methods derive that relationship from the
receiver; free functions and static methods may derive it from one eligible
read-only parameter. This unblocks ordinary helper/factory APIs for read-only
cursors and views without introducing explicit lifetime syntax. Mutable or
exclusive reborrows, more than one or nested origin, global/captured/storage
escape, and dependency-changing assignment remain outside that slice.

The 0.84.0 checkpoint extends retained-loan flow for one unshared carrier across
bounded switch exits and same-path invalidations immediately followed by a
matching `break`. Semantic analysis selects the endpoint, HIR preserves it, and
MIR normalizes each relevant outgoing edge before the verifier checks
predecessor agreement. General mutable reborrow/exclusive-loan graphs remain
explicitly deferred.

The 0.83.0 completeness pass hardened feature composition rather than adding a
new language surface. It preserves resolved inherited-generic owners into HIR,
adds fallthrough- and backedge-aware value-state checks, aligns raw-pointer
qualification ranking through variadic construction, and adds shipped-source
parser and position-sensitive editor-query gates. Larger MIR dataflow and
verification work remains explicitly staged rather than being folded into
these corrections.

The compiler is nevertheless transitional rather than backend-independent:

- semantic analysis and typed HIR are the strongest authorities today;
- MIR is validated and contains CFG, value, place, call, move, loan, and drop
  structure, but it does not yet own every temporary and lifecycle rule;
- constant folding still controls C++ emission through the compatibility HIR
  replacement table;
- the C++ emitter still walks checked AST and HIR side data rather than
  emitting complete bodies from optimized MIR.

The standard-library critical path is therefore still **Milestone 1:
lifetimes, places, and ownership flow**. The first source-defined vector now
validates movable dynamic storage and exact in-place construction, but complete
iterator invalidation and mutable traversal still depend on that milestone.
The bounded raw-pointer/unsafe slice enables audited native wrappers but does
not create owner dependencies or safe container traversal, so it does not
remove that remaining blocker. Teaching the compiler public library type names
would not remove it either.

Compiler operations that ordinary GTI cannot yet express now enter semantics
through trusted bodyless declarations in the implicit prelude. Calls bind the
selected declaration and `FunctionId`; namespace aliases preserve that
identity, while an untrusted declaration with the same spelling remains an
ordinary function. This removes call-site name recognition without adding a
source keyword, attribute, or public compiler-known wrapper type. The same
identity rule now covers private type declarations, while source roles and
semantic publication prevent application access to the surrounding namespace.

## Layer Assessment

| Layer | Position | Concrete boundary |
| --- | --- | --- |
| Source graph and parser | Implemented foundation | Per-unit parsing, direct visibility, recovery, source provenance, explicit application/prelude/physical-standard-library roles, and target directives are shared by CLI and LSP. Override-only paths do not acquire compiler trust. The external Tree-sitter grammar has a CI gate that parses every shipped standard-library and example source in addition to focused corpus fixtures. |
| Semantic analysis | Broad but transitional | Exact types, overloads, concepts, lifecycle, ownership, dispatch, bounded constexpr values/functions/branches, and current borrow restrictions are authoritative. Constexpr evaluation is compiler-owned, checked, step/depth bounded, and recorded independently of C++ emission. One-level raw-pointer operations and pointer-bearing C calls are classified against lexical unsafe context before lowering; raw pointers create no semantic loans. Trusted intrinsics and compiler-private types bind by declaration identity; `GTI-S2058` rejects application access to root `gti_internal`, and source publication prevents aliases or exposed signature types from leaking its symbols. Variadic storage construction selects exact element constructors, bounded C linkage retains exact external symbols, and the owned hosted-entry signature records its canonical argument types plus resolved startup append callable. Named-field move state is path-sensitive and checked on reachable loop backedges. Borrowed-return summaries select one read-only receiver or parameter origin and concrete generic carrier instances preserve it through calls, moves, returns, and drops. Retained local loans have owner/carrier provenance and frontend-selected straight-line, nested conditional, loop-exit, switch-exit, and proven break-path endings. Every carrier of a shared read-only loan contributes to that same path-aware plan. Bounded exclusive reborrows create distinct mutable or read-only child loans over stable root/field/checked-dereference places, suspend the mutable parent, validate prefix-overlap conflicts, permit known-disjoint sibling children and projected access, and fully reactivate the parent only after its final active child endpoint. General indexed, raw, opaque, stored, or escaping exclusive-loan graphs remain deferred. |
| Typed HIR | Implemented foundation | Owns concrete generic/class/callable instances, resolved call edges, typed values including frontend-computed constants, structured construction, source provenance, selected C linkage/external symbols, unsafe block markers, and classified unsafe expressions. Inherited generic calls consume the exact semantic dispatch owner instead of reconstructing base arguments from the derived receiver. Intrinsic calls retain their operation and declaration identity without enqueuing a bodyless function target. Program-entry instances retain the semantic entry kind and exact startup append callable. In-place storage construction keeps its storage/index/pack operands alongside the selected nested element-constructor identity. Exclusive reborrows retain child/parent identity, stable source place, access, and the semantic endpoint plan selected for reactivation. HIR remains immutable. Explicit ADR-007 failure outcome/site records are not implemented. |
| MIR | Structural foundation | Owns body CFG, values, places, calls, moves, loans, lexical drops, cleanup edges, raw address/arithmetic operations, raw memory projections, selected C linkage/external symbols, and program-entry adapter metadata. Raw-memory effects are conservative and raw pointers do not create loans. Moves retain receiver/binding, dereference-or-loan, and field projections; concrete pack expansion no longer confuses source arguments with the callee. Storage-construction calls preserve their nested constructor target for verification and later lowering. Borrowed-returning functions retain the selected receiver or formal-parameter summary; entry, call-result, carrier, and escaping return loans preserve the same source identity across calls. One loan can carry multiple unique read-only bindings while retaining one producer and one path-sensitive state. Exclusive child loans preserve their mutable parent and drive verified suspended/reactivated transitions. Proven endpoints lower after statements, nested `if` merges, conditional branch entries, or normalized loop, switch, and break predecessors. Verification checks program-entry identity, loan production, carrier and parent identity, selected call/return sources, path-sensitive active/suspended state, and predecessor agreement in addition to structural identities, reachability, and use indexes. General temporaries, indexed partial initialization, complete active-drop state, ADR-007 failure propagation/containment, and a general ABI model remain missing. |
| Optimizer | Stage A complete; Stage B started | Backend-neutral checked-integer and exact binary32 evaluation and safe HIR folding are implemented. A private LLVM generic-dominator adapter computes fresh GTI-ID dominance facts and the MIR verifier consumes them; no pointers survive the snapshot. One atomic controlled editor client folds primitive grouping identities in verified shadow MIR and reports HIR agreement plus repair/invalidation. General pass management, cached analyses, broader folds, and MIR-controlled emission remain outstanding. |
| C++ backend | Transitional with documented failure gaps | Consumes semantic and HIR decisions, including compiler-capability type identity, emits exact binary32 constants, and isolates native `argc`/`char**` behind the owned-entry adapter, but still emits from AST structure. Checked values are detected, yet emitter-local abort helpers and native expected observers do not implement Execution §4.10's category/site, cleanup, embedding, or status contract. It is not evidence that MIR is ready for LLVM. |
| Compiler library boundary | Partial migration | Lexer, MIR repair/verification/printing, effects, and optimizer entry points are compiled. The semantic analyzer, HIR lowerer, MIR lowerer, and C++ emitter remain large implementation headers under the accepted migration proposal. |
| Build and tooling | Parallel foundations | Direct and manifest workflows share driver requests; `build`, `check`, `run`, `clean`, and schema-4 `metadata` are implemented. Package/profile/target native inputs are target-selected, package-contained, ordered, and passed through the shared native request; declared C and C++ sources compile atomically before the final C++ link. Project tests, caching, dependencies, and lockfiles remain staged. LSP queries share frontend snapshots and compiler-owned private-presentation checks for semantic tokens, completion, hover, and definition; broader project awareness and symbol operations remain incomplete. |

## Roadmap Milestones

### Milestone 0: design boundaries - partial

Implemented:

- fixed-width integer domains, checked arithmetic failures, shifts, modulo,
  conversions, and backend-neutral constant evaluation;
- IEEE-754 binary32 literals, arithmetic, comparisons, conversions, signed
  zero/NaN behavior, no-contraction execution, and compiler-owned constant
  evaluation through exact stored bits;
- centralized MIR instruction, operation, and intrinsic effect tables;
- trusted declaration-bound intrinsic registration with no call-site spelling
  recognition;
- trusted source roles, exact compiler-private type identity, application
  `gti_internal` rejection, private-signature publication, and shared LSP
  presentation filtering;
- target selection and compiler-owned target conditionals;
- an adopted concurrency and memory-model boundary covering safe data-race
  freedom, transfer/share capability derivation, explicit execution profiles,
  first-profile ownership and globals, atomic and automatic-join thread
  lifecycle semantics, native entry, and staged compiler/runtime verification;
- an accepted defined-failure contract covering stable category/site records,
  cleanup and double failure, hosted reporting/status, observation, embedding,
  worker containment, and recoverable API boundaries;
- a maintained restriction ledger distinguishing intentional v1 rules from
  proof, lowering, library, and undecided-choice work with explicit horizons
  and owners;
- documented ownership, range, optimizer, build, and runtime boundaries.

Still required:

- one complete evaluation-order and full-expression contract, followed by C++
  lowering that cannot inherit host argument ordering;
- pre-1.0 implementation of the adopted transfer/share facts and concurrent-
  global policy over the now-secured compiler-private capability boundary;
- the ledger-selected source-text, target/data-layout, bounded layout-query,
  integer-mode, and binary64 work;
- the pre-1.0 compatibility and future-edition policy.

### Milestone 1: lifetimes, places, and ownership flow - active

Implemented foundation:

- non-null read-only and mutable references;
- receiver/argument borrow origins through semantics, HIR, and MIR;
- local, call-result, stored, and escaping return loan identities;
- single-origin read-only owner dependencies selected from a method receiver
  or one eligible free/static parameter and preserved through ordinary calls,
  concrete generic carrier relays, moves, returns, and drops;
- places with field, index, and dereference projections;
- explicit moves, lexical drops, cleanup edges, and `EndBorrow` instructions;
- full-expression endings for non-retained call-result loans;
- semantic loan identities with owner, origin, carrier, access, and storage
  protection metadata;
- move transfer of one retained loan identity between borrowed-state carriers;
- read-only alias attachment to one retained loan identity, with loan-wide use
  collection and one endpoint plan across all carriers;
- distinct mutable or read-only child loans derived from a mutable local loan,
  including nested and known-disjoint sibling chains, parent suspension, and
  full reactivation after the final active child's frontend-selected endpoint;
- exact prefix-overlap conflict validation for stable symbol/receiver roots
  with named-field and checked-dereference projections, conservative unknown
  divergence, and independent known sibling fields;
- exact last-use endings for one supported local loan whose uses remain in a
  single straight-line statement region;
- path-specific endings for one loan across linear `if` arms,
  including branch-entry endings for paths with no carrier use;
- recursive path-specific endings through nested `if` trees, including
  reachable nested merges and ordinary cleanup on terminating arms;
- loop-carried last-use projection for a pre-existing local loan
  across `while`, body-first `do`/`while`, and classic `for`, with one endpoint
  after condition-false and `break` paths converge and no endpoint on a
  backedge or `continue`;
- unified switch-exit endpoints for one pre-existing local loan,
  without claiming general nested switch/loop flow;
- proven same-path early endings after a carrier's final use before an
  invalidation immediately followed by the matching `break`, with MIR
  normalization on every relevant outgoing edge before the shared join;
- per-iteration conditional endings for a carrier created inside a loop body,
  while loans first created in a `for` initializer retain lexical loop-scope
  cleanup;
- path-sensitive MIR verification of one loan producer, represented active
  uses, balanced normal exits, and equal incoming loan state at CFG joins,
  including the newly normalized switch and immediate-break edges;
- explicit movement of named writable fields rooted in local values,
  parameters, checked owner dereferences, or mutable `this`, with
  flow-sensitive use checks and definite receiver reinitialization;
- fallthrough-aware move-state joins for terminating `if` arms and unreachable
  tails, short-circuit move-state joins, and explicit `break`/`continue` value
  snapshots; and
- conservative whole-value and projected-field availability validation on
  every reachable `while`, `do`/`while`, and classic `for` backedge.

Still required:

- generalized exclusive-loan graphs for indexed, raw, opaque, stored, or
  escaping provenance beyond the bounded local stable-place slice;
- indexed partial movement, generalized place aliasing, and MIR-owned
  initialization state;
- a general fixed-point transfer authority for repeated loop headers and
  arbitrary CFG joins, replacing the current bounded semantic snapshots;
- complete temporary, full-expression, active-drop, and unwind-free failure
  cleanup semantics; and
- owner-dependency graphs beyond the implemented direct read-only
  single-origin case, including stored or escaping mutable dependencies,
  multiple or nested origins, and dependency-changing assignment.

The MIR verifier remains a guardrail rather than the authority that chooses
loan endpoints. Semantic analysis chooses the implemented straight-line,
conditional, nested-arm, unified loop/switch-exit, or proven same-path
immediate-break endpoint; HIR carries that decision; and MIR materializes
and normalizes it on the relevant outgoing edges. The verifier does not infer
endpoints or place aliasing, but it validates exclusive child/parent identity,
suspension and reactivation, and requires predecessor loan states to agree at
every reachable join.

### Milestone 2: containers, iterators, and ranges - first container slice

Structural range `for`, source-defined read-only iterators, and one confined
stored-reference owner dependency exist. The first ordinary source-defined
`std::vector<T>` now uses private checked storage for default/size construction,
observation, reserve, clear, push/pop, checked access, variadic exact in-place
`emplace_back`, movement, and conservative read-only iteration. Its element type
must be movable and cannot contain borrowed state. The vector name has no
compiler privilege.

This does not complete the milestone. Free/static factories can now return one
direct read-only owner-tied cursor or view, including through a concrete
generic carrier relay, but fixed-array range iteration, owned temporary ranges,
per-iteration element loans, mutable iteration, nested/multi-owner views, and
precise invalidation effects remain incomplete. Bounded local exclusive
reborrows provide a prerequisite for mutable access, but they do not create the
range-level or per-iteration loan protocol by themselves.

### Milestone 3: callables and generic capabilities - first layer complete

Typed lexical lambdas, direct non-escaping generic callable parameters,
declaration-order-independent confined forwarding, named concepts, lifecycle
and comparison capabilities, value generics, and restricted packs are
implemented. Concepts now include multi-parameter source composition, bounded
validity-only trailing `requires`, and exact input-iterator, iterator/sentinel,
and accumulation-referent structural capabilities used by source-defined
`std::accumulate`. Bounded scalar constexpr bindings, free functions, static
methods, recursion, structured control flow, and frontend-selected
`if constexpr` are also implemented. Arbitrary callable results, capture
ownership, complete-range/callable/hash capabilities, heterogeneous
accumulation, and generic or aggregate constexpr evaluation remain.

### Milestones 4 and 5 - selective groundwork

Several safe C++-familiar additions are complete, including owned conditional
expressions, arithmetic compound assignments, `do`/`while`, and the first
bounded `extern "C"` call layer. The latter owns exact C symbols, a fixed-width
scalar allowlist, non-retained counted text inputs, and one-level scalar/`void`
pointers whose calls are lexically unsafe. Native layouts, pointer-to-pointer
and callback types, casts, and ownership transfer remain deferred. Project
manifests can now provide structured target-aware native link inputs and
automatically compile declared package-contained C and C++ sources. The
public standard library has initial utility, ownership, array, string, vector,
view, math, and I/O foundations plus a bounded POSIX `std::tcp::socket` owner.
Owned process arguments are available through the typed hosted entry form;
environment access remains deferred. The library cannot yet claim connected
networking or the complete v1
container/view/algorithm surface because the address/buffer ABI and Milestone 1
lifetime work are incomplete.

## Parallel Tracks

- **Optimizer/backend:** the bounded Stage A editor client and first Stage B
  shadow fold are complete. The next optimizer work may expand existing safe
  folds one proof family at a time or implement `O-MIR-02` conservative
  per-function effects. General pass management must still follow a concrete
  client, and no new optimization should extend the HIR replacement bridge.
- **Build system:** immutable compiler/driver requests, executable manifest
  targets, and `build`, `check`, `run`, `clean`, and `metadata` are complete.
  Structured package/profile/target native inputs and declared C/C++ source
  compilation are also complete. Project test targets are next, followed by
  deterministic caching.
- **Quality/tooling:** deterministic diagnostics, formatting, Tree-sitter,
  semantic tokens, completion, hover, and definition have foundations.
  Shipped GTI sources are parsed by Tree-sitter in CI; interface/pack signature
  presentation and current rainbow-delimiter nodes have position-sensitive
  regressions. Full symbol operations, project-aware analysis, fuzzing, and
  performance observability remain open.

## Operational Queue

The sole maintained work queue is
[`implementation-sequence.md`](implementation-sequence.md). Its current first
unowned task is the evaluation/full-expression decision; I-CAP-01, the
memory-model decision, callable contract, and failure contract are complete.
`C-TYPE-01` is also ready on the pre-1.0 concurrency-policy lane. The
executable compiler critical path remains generalized indexed places and
definite initialization,
temporary/active-drop authority, ordered MIR expression lowering, the
co-delivered failure/runtime substrate, the first MIR-emitted body family, and
complete M-BACK-02 body-family migration.

This checkpoint records evidence rather than duplicating that queue. Every
future pass should update its row in the implementation sequence, this file's
implemented evidence, and the affected canonical architecture/language
documents in the same change.
