# GTI Compiler Roadmap Status

Status: implementation checkpoint

Checkpoint version: 0.85.0

This document records where the compiler currently sits against
[`roadmap-to-1.0.md`](roadmap-to-1.0.md). The roadmap remains the dependency and
release plan; this checkpoint is the shorter implementation ledger that future
passes should update when they complete or materially unblock a milestone.
The grammar and semantic specification remain authoritative for shipped
language behavior.

## Current Position

The review found no new competing semantic authority or dependency-order
drift. Source loading, parsing, semantic selection, concrete HIR discovery, and
structural MIR lowering remain one directional. The C++ backend consumes
frontend facts instead of deciding overloads, ownership, dispatch, or language
validity.

The 0.85.0 checkpoint carries one read-only owner relationship
through resolved calls, concrete generic carrier instances, explicit moves,
checked returns, and drops. Instance methods derive that relationship from the
receiver; free functions and static methods may derive it from one eligible
read-only parameter. This unblocks ordinary helper/factory APIs for read-only
cursors and views without introducing explicit lifetime syntax. Mutable or
exclusive reborrows, more than one or nested origin, global/captured/storage
escape, dependency-changing assignment, and precise shared-alias endings
remain outside the slice.

The 0.84.0 checkpoint extends retained-loan flow for one unshared carrier across
bounded switch exits and same-path invalidations immediately followed by a
matching `break`. Semantic analysis selects the endpoint, HIR preserves it, and
MIR normalizes each relevant outgoing edge before the verifier checks
predecessor agreement. Precise shared-alias endpoints and general mutable
reborrow/exclusive-loan graphs remain explicitly deferred.

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
source keyword, attribute, or public compiler-known wrapper type.

## Layer Assessment

| Layer | Position | Concrete boundary |
| --- | --- | --- |
| Source graph and parser | Implemented foundation | Per-unit parsing, direct visibility, recovery, source provenance, and target directives are shared by CLI and LSP. The external Tree-sitter grammar now has a CI gate that parses every shipped standard-library and example source in addition to focused corpus fixtures. |
| Semantic analysis | Broad but transitional | Exact types, overloads, concepts, lifecycle, ownership, dispatch, and current borrow restrictions are authoritative. One-level raw-pointer types retain binding versus pointee access, and every gated address, access, arithmetic, or pointer-bearing C call is classified against lexical unsafe context before lowering. Raw pointers create no semantic loans. Raw-pointer overload preference is per-argument dominance, including concrete variadic construction. Trusted intrinsic declarations carry a closed operation identity from the prelude into resolved calls; call-site spelling grants no behavior. Variadic storage construction expands concrete packs, selects one exact element constructor, rejects borrowed-state elements, and requires movable relocation before backend entry. Bounded C-linkage declarations retain exact external symbols and are validated against the fixed scalar, counted-input, and scalar/`void` pointer ABI before backend entry. Named writable field moves carry path-sensitive moved state and require definite reinitialization before a receiver returns or its local owner is transferred. Move-state joins now exclude terminating and unreachable edges, model short-circuit reachability, and conservatively require outer values and projected fields to be reinitialized on every reachable loop backedge. Borrowed-return summaries select one read-only origin from an instance receiver or one eligible free/static parameter, and concrete generic carrier instances preserve that origin through calls, moves, returns, and drops. Retained local borrows have semantic loan identities, owner/carrier provenance, precise straight-line and conditional-join endpoints, recursive path-specific endings through nested `if` arms, loop-exit endpoints, unified switch-exit endpoints, and proven early endings after the final use before a same-path invalidation immediately followed by a matching `break`, for one unshared carrier. Loop-carried loans remain active through conditions, bodies, increments, `continue`, and backedges. Switch/loop nesting outside the proven shapes remains conservative. Read-only aliases retain lexical extent; precise shared-alias endpoints and general mutable reborrow/exclusive-loan graphs remain deferred. |
| Typed HIR | Implemented foundation | Owns concrete generic/class/callable instances, resolved call edges, typed values, structured construction, source provenance, selected C linkage/external symbols, unsafe block markers, and classified unsafe expressions. Inherited generic calls consume the exact semantic dispatch owner instead of reconstructing base arguments from the derived receiver. Intrinsic calls retain their operation and declaration identity without enqueuing a bodyless function target. In-place storage construction keeps its storage/index/pack operands alongside the selected nested element-constructor identity. HIR remains immutable. |
| MIR | Structural foundation | Owns body CFG, values, places, calls, moves, loans, lexical drops, cleanup edges, raw address/arithmetic operations, raw memory projections, and selected C linkage/external symbols. Raw-memory effects are conservative and raw pointers do not create loans. Moves retain receiver/binding, dereference-or-loan, and field projections; concrete pack expansion no longer confuses source arguments with the callee. Storage-construction calls preserve their nested constructor target for verification and later lowering. Borrowed-returning functions retain the selected receiver or formal-parameter summary; entry, call-result, carrier, and escaping return loans preserve the same source identity across calls. MIR loans retain their originating semantic loan identity and every carrier binding; proven endpoints lower after statements, after reachable nested `if` merges, at conditional branch entries, or on the relevant predecessor edges of semantic loop exits, switch exits, and proven same-path immediate-break routes. Switch and break lowering normalize every relevant outgoing edge before it reaches a shared join. Verification checks loan production, carrier uniqueness, selected call/return sources, path-sensitive active state, and predecessor agreement in addition to structural identities, reachability, and use indexes. General temporaries, indexed partial initialization, complete active-drop state, and a general ABI model remain missing. |
| Optimizer | Stage A transition | Backend-neutral integer evaluation and safe HIR folding are implemented. The owned MIR path verifies an identity snapshot; controlled editors, pass management, analyses, shadow MIR folding, and MIR-controlled emission remain outstanding. |
| C++ backend | Correct transitional backend | Consumes semantic and HIR decisions and implements checked runtime behavior, but still emits from AST structure. It is not evidence that MIR is ready for LLVM. |
| Compiler library boundary | Partial migration | Lexer, MIR repair/verification/printing, effects, and optimizer entry points are compiled. The semantic analyzer, HIR lowerer, MIR lowerer, and C++ emitter remain large implementation headers under the accepted migration proposal. |
| Build and tooling | Parallel foundations | Direct and manifest workflows share driver requests; `build`, `check`, `run`, `clean`, and schema-2 `metadata` are implemented. Package/profile/target native inputs are target-selected, package-contained, ordered, and passed through the shared native request. Project tests, caching, dependencies, and lockfiles remain staged. LSP queries share frontend snapshots, while broader project awareness and symbol operations remain incomplete. |

## Roadmap Milestones

### Milestone 0: design boundaries - partial

Implemented:

- fixed-width integer domains, checked arithmetic failures, shifts, modulo,
  conversions, and backend-neutral constant evaluation;
- centralized MIR instruction, operation, and intrinsic effect tables;
- trusted declaration-bound intrinsic registration with no call-site spelling
  recognition;
- target selection and compiler-owned target conditionals;
- documented ownership, range, optimizer, build, and runtime boundaries.

Still required:

- floating-point behavior for NaN, signed zero, contraction, conversion, and
  supported rounding environment;
- one complete evaluation-order and full-expression contract, followed by C++
  lowering that cannot inherit host argument ordering;
- an explicit ledger separating safety restrictions from temporary compiler
  limitations;
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
- exact last-use endings for one unshared local carrier whose uses remain in a
  single straight-line statement region;
- path-specific endings for one unshared carrier across linear `if` arms,
  including branch-entry endings for paths with no carrier use;
- recursive path-specific endings through nested `if` trees, including
  reachable nested merges and ordinary cleanup on terminating arms;
- loop-carried last-use projection for a pre-existing unshared local carrier
  across `while`, body-first `do`/`while`, and classic `for`, with one endpoint
  after condition-false and `break` paths converge and no endpoint on a
  backedge or `continue`;
- unified switch-exit endpoints for one pre-existing unshared local carrier,
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

- shared read-only alias endpoints plus general mutable reborrow,
  exclusive-loan, and conflict validation over places;
- indexed partial movement, generalized place aliasing, and MIR-owned
  initialization state;
- a general fixed-point transfer authority for repeated loop headers and
  arbitrary CFG joins, replacing the current bounded semantic snapshots;
- complete temporary, full-expression, active-drop, and unwind-free failure
  cleanup semantics; and
- owner-dependency graphs beyond the implemented direct read-only
  single-origin case, including mutable/exclusive reborrows, multiple or
  nested origins, escaping storage, dependency-changing assignment, and
  precise shared aliases.

The MIR verifier remains a guardrail rather than the authority that chooses
loan endpoints. Semantic analysis chooses the implemented straight-line,
conditional, nested-arm, unified loop/switch-exit, or proven same-path
immediate-break endpoint; HIR carries that decision; and MIR materializes
and normalizes it on the relevant outgoing edges. The verifier does not infer
endpoints or place aliasing, but it requires predecessor loan states to agree
at every reachable join.

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
per-iteration element loans, mutable iteration, nested/multi-owner views,
precise shared readers, and precise invalidation effects remain incomplete.

### Milestone 3: callables and generic capabilities - first layer complete

Typed lexical lambdas, direct non-escaping generic callable parameters,
declaration-order-independent confined forwarding, named concepts, lifecycle
and comparison capabilities, value generics, and restricted packs are
implemented. Arbitrary callable results, capture ownership, exact callable
concepts, range concepts, bounded `constexpr`, and `if constexpr` remain.

### Milestones 4 and 5 - selective groundwork

Several safe C++-familiar additions are complete, including owned conditional
expressions, arithmetic compound assignments, `do`/`while`, and the first
bounded `extern "C"` call layer. The latter owns exact C symbols, a fixed-width
scalar allowlist, non-retained counted text inputs, and one-level scalar/`void`
pointers whose calls are lexically unsafe. Native layouts, pointer-to-pointer
and callback types, casts, and ownership transfer remain deferred. Project
manifests can now provide structured target-aware native link inputs. The
public standard library has initial utility, ownership, array, string, vector,
view, math, and I/O foundations plus a bounded POSIX `std::tcp::socket` owner.
It cannot yet claim connected networking or the complete v1
container/view/algorithm surface because the address/buffer ABI and Milestone 1
lifetime work are incomplete.

## Parallel Tracks

- **Optimizer/backend:** Stage A is partial. The next optimizer architecture
  work remains controlled MIR editing and pass management, followed by MIR
  constant folding in shadow mode. No new optimization should extend the HIR
  replacement bridge.
- **Build system:** immutable compiler/driver requests, executable manifest
  targets, and `build`, `check`, `run`, `clean`, and `metadata` are complete.
  Structured package/profile/target native inputs are also complete. Project
  test targets are next, followed by deterministic caching.
- **Quality/tooling:** deterministic diagnostics, formatting, Tree-sitter,
  semantic tokens, completion, hover, and definition have foundations.
  Shipped GTI sources are parsed by Tree-sitter in CI; interface/pack signature
  presentation and current rainbow-delimiter nodes have position-sensitive
  regressions. Full symbol operations, project-aware analysis, fuzzing, and
  performance observability remain open.

## Next Compiler Slices

Complete these in order unless a focused proposal records a dependency change:

1. Extend retained-loan flow to shared read-only aliases, then design general
   mutable reborrow and exclusive-loan graphs over places, using MIR
   predecessor agreement and loan-flow verification as invariant gates.
2. Extend the named-field move slice to indexed places, generalized aliases,
   and MIR-owned partial-move/reinitialization state.
3. Make temporary lifetime and active-drop transitions explicit on every MIR
   edge.
4. Use the implemented single-origin read-only owner dependency for focused
   cursor/span/view library APIs; design multi-origin, nested, mutable, or
   stored dependency graphs separately instead of widening the first slice by
   implication.
5. In parallel, finish the MIR pass framework and shadow constant folding; do
   not make optimized MIR control C++ emission until one complete body family
   is supported.

Every future pass that changes one of these positions should update this file,
the detailed proposal that owns the work, and the compiler-internals reference
in the same change.
