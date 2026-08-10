# GTI Language Specification

Status: Working Draft

This directory is the scaffold for the normative GTI language and standard
library specification. It describes GTI as a language independently of the
current C++ backend. It is not an ISO publication, does not claim ISO
affiliation, and is not yet a complete conformance standard.

The immediate objective is to give every shipped feature one precise home for
its syntax, static semantics, runtime semantics, ownership effects, and failure
conditions. A future GTI 1.0 specification can become a compatibility boundary
once the gaps listed in this draft have been resolved and covered by
conformance tests.

## Status And Authority

This working draft uses three status levels:

- **Normative draft** text states the intended GTI rule using the vocabulary in
  [Scope And Conformance](01-scope-and-conformance.md). A conflicting compiler
  result should be investigated as either an implementation defect or a draft
  defect.
- **Incorporated reference** text points to an existing repository document
  that remains the detailed authority until its rules are migrated here.
- **Specification gap** identifies behaviour that is deliberately not assigned
  a rule yet. An implementation accident, emitted C++ behaviour, or optimizer
  choice must not silently fill such a gap.

For the current implementation, syntax remains defined in
[`docs/grammar.ebnf`](../docs/grammar.ebnf), while cross-feature semantic intent
remains defined in the
[`GTI Language Contract`](../.agents/skills/gti-language/references/language-contract.md).
The ownership and lifetime subset is further defined in
[`docs/ownership.md`](../docs/ownership.md). This specification incorporates
those documents during migration; where they disagree, the disagreement must
be recorded and resolved rather than guessed.

Generated C++, C++ standard-library behaviour, and the current shape of a
backend helper are never normative GTI definitions.

## Reading Order

| Part | Subject | Current status |
| --- | --- | --- |
| [1](01-scope-and-conformance.md) | Scope, terminology, conformance, and compatibility | Normative scaffold |
| [2](02-lexical-and-syntactic-structure.md) | Source text, tokens, grammar, and source units | Grammar incorporated by reference |
| [3](03-static-semantics.md) | Types, declarations, calls, generics, objects, and well-formedness | Normative summary with recorded gaps |
| [4](04-execution-and-runtime-semantics.md) | Evaluation, control flow, numeric behaviour, failure, and cleanup | Partial normative draft |
| [5](05-ownership-and-lifetimes.md) | Places, values, borrows, moves, owners, and destruction | Ownership contract incorporated and summarized |
| [6](06-programs-implementations-and-targets.md) | Programs, targets, implementations, diagnostics, and backends | Normative scaffold |
| [7](07-standard-library.md) | Prelude, public library, internal capabilities, and runtime boundary | Partial normative draft |

## Normative Vocabulary

The key words **must**, **must not**, **required**, **shall**, and **shall not**
state normative requirements. **May** grants permission. **Should** states a
recommendation and is not by itself a conformance requirement.

Code marked as an *example* is non-normative unless the surrounding text says
otherwise. Notes, rationale, implementation guidance, roadmap items, and the
discussion at the bottom of this document are non-normative.

## Design Principles

The specification is developed under these principles:

1. GTI remains immediately readable to a C++ programmer where familiar syntax
   can have a complete and simpler rule.
2. Ownership, lifetime, nullability, conversions, failure, and evaluation are
   language semantics rather than backend consequences.
3. Value semantics, deterministic cleanup, explicit control, native
   interoperability, and predictable performance remain first-class goals.
4. Invalid GTI is rejected by the GTI frontend; native compiler rejection is
   not a substitute for a language rule.
5. Undefined behaviour is not introduced by omission. Any future unsafe
   surface must enumerate the obligations transferred to the programmer.
6. The standard library is ordinary GTI source wherever the language can
   express its invariant. Compiler-private capabilities remain irreducible and
   unavailable as accidental public APIs.
7. A C++ backend and a future non-C++ backend must implement the same observable
   GTI behaviour.

## Specification Maintenance

Every user-visible language change should eventually update:

1. the applicable normative section in this directory;
2. the grammar when syntax changes;
3. positive and negative conformance tests;
4. ownership, runtime, and target rules when affected;
5. the standard-library contract when public APIs change; and
6. the compatibility record when an accepted program changes meaning.

Before 1.0, incompatible draft changes are permitted but must be recorded in
release notes. GTI 1.0 should publish a compatibility and deprecation policy and
define whether later breaking changes use editions or another explicit opt-in
mechanism.

## Incorporated Repository Sources

The following documents supplied the initial scaffold:

- [`docs/grammar.ebnf`](../docs/grammar.ebnf): implemented syntax and embedded
  semantic notes;
- [`GTI Language Contract`](../.agents/skills/gti-language/references/language-contract.md):
  cross-feature semantic invariants;
- [`docs/ownership.md`](../docs/ownership.md): ownership, moves, borrows,
  storage, and destruction;
- [`docs/ranges.md`](../docs/ranges.md) and
  [`docs/iterator-range-proposal.md`](../docs/iterator-range-proposal.md): the
  implemented range subset and later lifetime layers;
- [`docs/compiler-architecture.md`](../docs/compiler-architecture.md): current
  compiler representation and backend transition;
- [`docs/native-c-interop.md`](../docs/native-c-interop.md): bounded C-linkage
  declaration, ABI, lifetime, and linking contract;
- [`docs/roadmap-to-1.0.md`](../docs/roadmap-to-1.0.md): stability gates and
  deferred features; and
- [`stdlib/README.md`](../stdlib/README.md): public library and runtime
  boundaries.

Implementation files and tests are evidence of current behaviour, but the
completed specification must be sufficient to implement GTI without reading
the existing compiler source.

## Discussion: Alignment With GTI's Goal

This final section is deliberately non-normative. It records design pressure
that should be discussed before the corresponding rules are frozen. It neither
deprecates current behaviour nor grants implementations permission to ignore
it.

GTI's stated goal is not source compatibility with C++. It is a C++-familiar
compiled systems language that retains explicit control, value semantics,
RAII, performance, and native interoperability while removing avoidable
hazards and accidental complexity. Several departures clearly serve that goal:
checked fixed-array indexing, scoped nominal enums, defined shift and modulo
edges, no textual macros, deterministic cleanup, non-fallthrough `switch`,
frontend-owned overload resolution, and the separation of public owners from
compiler-private allocation capabilities.

The following areas may represent overcorrection, misleading familiarity, or
temporary implementation restrictions at risk of becoming permanent design:

| Area | Current direction | Question before stabilization |
| --- | --- | --- |
| Exact calls | Calls perform no implicit argument conversion or conversion ranking. | Does rejecting every safe widening and derived-to-base call make ordinary C++-shaped APIs unnecessarily ceremonial? A small, explicitly specified conversion set may be possible without recreating C++ ranking. |
| Polymorphic substitution | A derived value can form an explicit base reference or base-reference return, but ordinary calls remain exact. | If `draw(Renderable&)` cannot directly accept a `Sprite`, has GTI made its familiar inheritance syntax materially less useful than users expect? |
| Reference spelling | `T&` is a read-only borrow and `mut T&` is writable. | This is simpler than C++ const propagation, but it reverses a deeply learned C++ meaning. Documentation and diagnostics must make the distinction unmistakable. |
| Move spelling | `std::move(value)` consumes the binding, including copyable values, rather than performing C++'s value-category cast. | Is the familiar spelling an advantage, or would a future spelling such as `take` communicate GTI's stronger state transition more honestly? |
| Result use | Every non-`void` call result must be consumed unless the call is marked `[[discard]]`. | Does universal enforcement create annotation noise for intentionally ignorable observations? A declaration-side discard policy may preserve safety with better ergonomics. |
| Operators | User operators are member-only and restricted to a small allowlist; GTI has no ADL or free operator lookup. | The restriction avoids major C++ complexity, but can generic numeric types, formatting, iterators, and domain values remain natural without a reusable capability-based customization mechanism? |
| Interfaces | GTI adds an `interface` keyword while retaining `= 0;`, explicit `public` bases, and C++-shaped virtual declarations. | Is this the right familiarity point, or does it combine modern intent with avoidable C++ ceremony? The distinction between interface contracts and state-bearing abstract bases should remain crisp. |
| Construction braces | `Type value{args};` is direct exact construction but deliberately not list initialization, aggregate initialization, CTAD, or initializer-list preference. | The safer rule is coherent, but identical punctuation with substantially narrower meaning needs precise teaching and diagnostics. |
| `include` | `include` loads one source dependency with direct visibility and no textual or transitive inclusion. | The semantics are healthier than headers, but the familiar word may promise C++ behaviour that GTI intentionally rejects. A future module vocabulary should be considered before compatibility freezes the spelling. |
| Generic capabilities | Built-in numeric constraints work, but user-defined interfaces do not yet form a general constraint system. | Without user-defined capabilities, is generic GTI sufficiently expressive for containers, algorithms, allocators, hashing, formatting, and callables? |
| Borrowed values | Stored references, escaping lambdas, dynamic views, and owner-tied iterators are intentionally confined while lifetime tracking matures. | These are sound staging limits, but they must not quietly become permanent restrictions that prevent zero-cost library abstractions. |
| Low-level control | Safe GTI has no public raw pointer, allocator, manual lifetime, `new`, or `delete` surface. | Omitting C++'s defaults is aligned with the safety goal; omitting any audited low-level memory and FFI model permanently would conflict with the systems-language and game-engine goals. |
| Standard-library intrinsics | Most public wrappers are ordinary GTI, while operations such as `std::move` are currently recognized directly by the compiler. | Public names should eventually bind to compiler semantics by trusted declarations rather than spelling, so user shadowing, tooling, and alternative backends share one rule. |
| Backend completeness | Some executable meaning still reaches the C++ emitter through checked AST plus semantic/HIR side data while MIR ownership is expanding. | Until evaluation order, temporaries, layout, ABI, and all operations are represented independently, generated C++ can still exert accidental pressure on the language design. |

The purpose of this discussion is not to move GTI back toward every C++ rule.
It is to distinguish intentional improvements from restrictions chosen because
they were initially easier to prove or lower. A departure should survive if it
has a clear safety, predictability, or simplicity benefit that outweighs the
loss of familiarity and expressiveness.
