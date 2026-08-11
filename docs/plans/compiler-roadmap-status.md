# GTI Compiler Roadmap Status

> **Plan status:** Implementation checkpoint and future-work ledger. It does
> not define current language semantics.

Status: implementation checkpoint

Checkpoint version: 0.90.0

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
source keyword, attribute, or public compiler-known wrapper type.

## Layer Assessment

| Layer | Position | Concrete boundary |
| --- | --- | --- |
| Source graph and parser | Implemented foundation | Per-unit parsing, direct visibility, recovery, source provenance, and target directives are shared by CLI and LSP. The external Tree-sitter grammar now has a CI gate that parses every shipped standard-library and example source in addition to focused corpus fixtures. |
| Semantic analysis | Broad but transitional | Exact types, overloads, concepts, lifecycle, ownership, dispatch, bounded constexpr values/functions/branches, and current borrow restrictions are authoritative. Constexpr evaluation is compiler-owned, checked, step/depth bounded, and recorded independently of C++ emission. One-level raw-pointer operations and pointer-bearing C calls are classified against lexical unsafe context before lowering; raw pointers create no semantic loans. Trusted intrinsics bind by declaration identity, variadic storage construction selects exact element constructors, and bounded C linkage retains exact external symbols. Named-field move state is path-sensitive and checked on reachable loop backedges. Borrowed-return summaries select one read-only receiver or parameter origin and concrete generic carrier instances preserve it through calls, moves, returns, and drops. Retained local loans have owner/carrier provenance and frontend-selected straight-line, nested conditional, loop-exit, switch-exit, and proven break-path endings. Every carrier of a shared read-only loan contributes to that same path-aware plan. Bounded exclusive reborrows create distinct mutable or read-only child loans over stable root/field/checked-dereference places, suspend the mutable parent, validate prefix-overlap conflicts, permit known-disjoint sibling children and projected access, and fully reactivate the parent only after its final active child endpoint. General indexed, raw, opaque, stored, or escaping exclusive-loan graphs remain deferred. |
| Typed HIR | Implemented foundation | Owns concrete generic/class/callable instances, resolved call edges, typed values including frontend-computed constants, structured construction, source provenance, selected C linkage/external symbols, unsafe block markers, and classified unsafe expressions. Inherited generic calls consume the exact semantic dispatch owner instead of reconstructing base arguments from the derived receiver. Intrinsic calls retain their operation and declaration identity without enqueuing a bodyless function target. In-place storage construction keeps its storage/index/pack operands alongside the selected nested element-constructor identity. Exclusive reborrows retain child/parent identity, stable source place, access, and the semantic endpoint plan selected for reactivation. HIR remains immutable. |
| MIR | Structural foundation | Owns body CFG, values, places, calls, moves, loans, lexical drops, cleanup edges, raw address/arithmetic operations, raw memory projections, and selected C linkage/external symbols. Raw-memory effects are conservative and raw pointers do not create loans. Moves retain receiver/binding, dereference-or-loan, and field projections; concrete pack expansion no longer confuses source arguments with the callee. Storage-construction calls preserve their nested constructor target for verification and later lowering. Borrowed-returning functions retain the selected receiver or formal-parameter summary; entry, call-result, carrier, and escaping return loans preserve the same source identity across calls. One loan can carry multiple unique read-only bindings while retaining one producer and one path-sensitive state. Exclusive child loans preserve their mutable parent and drive verified suspended/reactivated transitions. Proven endpoints lower after statements, nested `if` merges, conditional branch entries, or normalized loop, switch, and break predecessors. Verification checks loan production, carrier and parent identity, selected call/return sources, path-sensitive active/suspended state, and predecessor agreement in addition to structural identities, reachability, and use indexes. General temporaries, indexed partial initialization, complete active-drop state, and a general ABI model remain missing. |
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
implemented. Bounded scalar constexpr bindings, free functions, static
methods, recursion, structured control flow, and frontend-selected
`if constexpr` are also implemented. Arbitrary callable results, capture
ownership, exact callable/range concepts, and generic or aggregate constexpr
evaluation remain.

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

Complete these in order unless a focused proposal records a dependency change.
The former first slice, bounded exclusive reborrows over stable places, is now
complete:

1. Extend the named-field move slice to indexed places, generalized place
   aliasing, and MIR-owned partial-move/reinitialization state.
2. Make temporary lifetime and active-drop transitions explicit on every MIR
   edge.
3. Use the implemented single-origin read-only owner dependency for focused
   cursor/span/view library APIs; design multi-origin, nested, mutable, or
   stored dependency graphs separately instead of widening the first slice by
   implication.
4. In parallel, finish the MIR pass framework and shadow constant folding; do
   not make optimized MIR control C++ emission until one complete body family
   is supported.

Every future pass that changes one of these positions should update this file,
the detailed plan that owns the work, and the affected canonical documents
under `docs/architecture/` or `docs/language/` in the same change.
