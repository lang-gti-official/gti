# GTI Compiler Roadmap Status

Status: implementation checkpoint

Checkpoint version: 0.76.0

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

The compiler is nevertheless transitional rather than backend-independent:

- semantic analysis and typed HIR are the strongest authorities today;
- MIR is validated and contains CFG, value, place, call, move, loan, and drop
  structure, but it does not yet own every temporary and lifecycle rule;
- constant folding still controls C++ emission through the compatibility HIR
  replacement table;
- the C++ emitter still walks checked AST and HIR side data rather than
  emitting complete bodies from optimized MIR.

The standard-library critical path is therefore **Milestone 1: lifetimes,
places, and ownership flow**. Adding more surface syntax or teaching the
compiler public library type names would not remove the current blocker.

Compiler operations that ordinary GTI cannot yet express now enter semantics
through trusted bodyless declarations in the implicit prelude. Calls bind the
selected declaration and `FunctionId`; namespace aliases preserve that
identity, while an untrusted declaration with the same spelling remains an
ordinary function. This removes call-site name recognition without adding a
source keyword, attribute, or public compiler-known wrapper type.

## Layer Assessment

| Layer | Position | Concrete boundary |
| --- | --- | --- |
| Source graph and parser | Implemented foundation | Per-unit parsing, direct visibility, recovery, source provenance, and target directives are shared by CLI and LSP. |
| Semantic analysis | Broad but transitional | Exact types, overloads, concepts, lifecycle, ownership, dispatch, and current borrow restrictions are authoritative. Trusted intrinsic declarations carry a closed operation identity from the prelude into resolved calls; call-site spelling grants no behavior. Bounded C-linkage declarations retain exact external symbols and are validated against the fixed scalar and counted-input ABI before backend entry. Named writable field moves carry path-sensitive moved state and require definite reinitialization before a receiver returns or its local owner is transferred. Retained local borrows have semantic loan identities, owner/carrier provenance, precise straight-line endpoints, endpoints after an enclosing `if` join, and path-specific endings before invalidation in linear arms. Shared carriers, nested conditional flow, and loop endpoints remain conservative. |
| Typed HIR | Implemented foundation | Owns concrete generic/class/callable instances, resolved call edges, typed values, structured construction, source provenance, and selected C linkage/external symbols. Intrinsic calls retain their operation and declaration identity without enqueuing a bodyless function target. HIR remains immutable. |
| MIR | Structural foundation | Owns body CFG, values, places, calls, moves, loans, lexical drops, cleanup edges, and carries selected C linkage/external symbols on function instances. Moves retain receiver/binding, dereference-or-loan, and field projections; concrete pack expansion no longer confuses source arguments with the callee. MIR loans retain their originating semantic loan identity and every carrier binding; proven endpoints lower after statements, after an `if` merge, or at a conditional branch entry. Branch lowering preserves and reconciles each arm's active-loan and outer-carrier state. Non-retained call-result loans end at their full-expression boundary, including loop conditions. Verification checks loan production, carrier uniqueness, and path-sensitive active state in addition to structural identities, reachability, and use indexes. General temporaries, indexed partial initialization, complete active-drop state, and a general ABI model remain missing. |
| Optimizer | Stage A transition | Backend-neutral integer evaluation and safe HIR folding are implemented. The owned MIR path verifies an identity snapshot; controlled editors, pass management, analyses, shadow MIR folding, and MIR-controlled emission remain outstanding. |
| C++ backend | Correct transitional backend | Consumes semantic and HIR decisions and implements checked runtime behavior, but still emits from AST structure. It is not evidence that MIR is ready for LLVM. |
| Compiler library boundary | Partial migration | Lexer, MIR repair/verification/printing, effects, and optimizer entry points are compiled. The semantic analyzer, HIR lowerer, MIR lowerer, and C++ emitter remain large implementation headers under the accepted migration proposal. |
| Build and tooling | Parallel foundations | Direct and manifest workflows share driver requests; `build`, `check`, `run`, `clean`, and `metadata` are implemented. Project tests, caching, dependencies, and lockfiles remain staged. LSP queries share frontend snapshots, while broader project awareness and symbol operations remain incomplete. |

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
- path-sensitive MIR verification of one loan producer, represented active
  uses, balanced normal exits, and equal incoming loan state at CFG joins.
- explicit movement of named writable fields rooted in local values,
  parameters, checked owner dereferences, or mutable `this`, with
  flow-sensitive use checks and definite receiver reinitialization.

Still required:

- last-use analysis across nested branches, loops, and shared/reborrowed
  carriers;
- shared/exclusive conflict and reborrow validation over general places;
- indexed partial movement, generalized place aliasing, and MIR-owned
  initialization state;
- complete temporary, full-expression, active-drop, and unwind-free failure
  cleanup semantics;
- general owner dependencies carried by borrowed values through calls, moves,
  returns, and drops.

The MIR verifier remains a guardrail rather than the authority that chooses
loan endpoints. Semantic analysis chooses the implemented straight-line,
conditional-join, or linear-arm endpoints; HIR carries that decision; and MIR
materializes and verifies it. The verifier deliberately does not infer nested
branch or loop last use and does not prove place aliasing.

### Milestone 2: containers, iterators, and ranges - early partial

Structural range `for`, source-defined read-only iterators, and one confined
stored-reference owner dependency exist. Fixed-array range iteration, owned
temporary ranges, per-iteration element loans, mutable iteration, general
owner-tied cursors, and invalidation effects remain incomplete. Public
`std::vector` must wait for those language facts and must remain an ordinary
library class.

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
scalar allowlist, and non-retained counted text inputs; pointers, callbacks,
native layouts, ownership transfer, and project native-link settings remain
deferred. The public standard library has initial utility, ownership, array,
string, view, math, and I/O foundations, but it cannot yet claim the v1
container/view/algorithm surface because Milestone 1 is not complete.

## Parallel Tracks

- **Optimizer/backend:** Stage A is partial. The next optimizer architecture
  work remains controlled MIR editing and pass management, followed by MIR
  constant folding in shadow mode. No new optimization should extend the HIR
  replacement bridge.
- **Build system:** immutable compiler/driver requests, executable manifest
  targets, and `build`, `check`, `run`, `clean`, and `metadata` are complete.
  Project test targets are next, followed by deterministic caching.
- **Quality/tooling:** deterministic diagnostics, formatting, Tree-sitter,
  semantic tokens, completion, hover, and definition have foundations. Full
  symbol operations, project-aware analysis, fuzzing, and performance
  observability remain open.

## Next Compiler Slices

Complete these in order unless a focused proposal records a dependency change:

1. Extend edge-specific loan flow through nested conditionals and terminating
   arms, then support loop exits and backedges, using the MIR loan-flow verifier
   as the invariant gate.
2. Extend the named-field move slice to indexed places, generalized aliases,
   and MIR-owned partial-move/reinitialization state.
3. Make temporary lifetime and active-drop transitions explicit on every MIR
   edge.
4. Carry general owner dependencies in semantic types, HIR, and MIR, then use
   them for fixed arrays, iterators, spans, and dynamic views.
5. In parallel, finish the MIR pass framework and shadow constant folding; do
   not make optimized MIR control C++ emission until one complete body family
   is supported.

Every future pass that changes one of these positions should update this file,
the detailed proposal that owns the work, and the compiler-internals reference
in the same change.
