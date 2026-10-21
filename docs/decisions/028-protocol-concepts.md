# ADR 028: Protocol Concepts And Conformance Blocks

Status: Accepted (2026-10-14). Phase 1 is implemented, and concepts with
several parameters followed in 0.435.0 (see the revision below). Later phases
are listed under Consequences.

## Context

GTI concepts are structural predicates
([ADR 009](009-bounded-requires-contracts.md)). A concept cannot name a type
related to its argument, so generic code passes related types as extra
parameters: `std::range_of<Range, T>` and `std::dereferences_to<Iterator, T>`
carry the element type separately, and every caller must supply it.

Protocols that the compiler or library consume are found by member names on
the participating type: the brace-list hooks `collection_init` and
`collection_insert`, range-for's `begin()` and `end()`, and, in the prototype
of a general `?` operator, a `residual()` member. Each protocol therefore adds
members to the public API of every type that takes part in it, and only a
type's own author can make it take part.

The motivating client is a `?` protocol that serves `std::optional`,
`expected` and user result types, without the compiler naming any of them and
without protocol-only members on them. Iterator value types are the second
client.

## Decision

1. **A concept may have a body.** Instead of `= constraint;`, a concept may
   declare `{ members };`. A `typename name;` member declares an associated
   type, and a function prototype declares an operation. A concept with a
   body takes one type parameter in this phase. `concept C<T> = ...;` keeps
   its meaning.

2. **Conformance blocks.** A type satisfies a concept with a body only
   through a block at namespace scope:

   ```gti
   for <T> concept measurable<Crate<T>> {
     using unit_type = int;
     unit_type measure(Crate<T>& item) { return item.count; }
   };
   ```

   The block defines every associated type with `using` and every operation
   with a function of exactly the concept's signature, after substituting
   the pattern for the concept's parameter and the block's types for the
   associated types. It defines nothing else.

3. **Exact selection.** The pattern is a concrete type, or one class template
   applied to each block parameter exactly once. A concrete type matches at
   most one block: overlapping blocks are rejected, and there is no
   specialization or ranking between blocks.

4. **Coherence.** As for exact class specializations
   ([ADR 020](020-exact-qualified-class-specializations.md)), the declaring
   package must own the concept or the pattern's top-level nominal type. All
   blocks register before any body is analyzed, so selection does not depend
   on include order. A foreign type that neither side owns is wrapped.

5. **Use.** `C<X>::name` is a type and `C<X>::operation(arguments)` is a call.
   For a concrete `X` they resolve to the selected block. In generic code
   they require `C<T>` as a conjunct of the enclosing `requires` clause, not
   inside a disjunction, and each concrete instance resolves them.
   `typename` becomes a keyword.

6. **No remapping.** A block function is reachable only through its concept.
   It never takes part in member lookup or ordinary name lookup, so a block
   cannot change what `x.member()` means. Inside a block, the block's own
   associated types are transparent and each operation requires the block's
   own application.

7. **Execution.** Block functions are ordinary generic functions over the
   block's parameters. The parser places them in a namespace whose generated
   name no source can spell, and calls to them are ordinary calls through
   HIR and MIR. There is no new MIR operation and no backend path.

## Alternatives

- **Traits classes specialized per type**, as in C++'s `std::hash<T>` or the
  P2561 `try_traits`. They keep hooks off the type, but need partial
  specialization, check nothing declared, and let libraries conflict.
- **Structural concepts with inferred outputs.** They need a rule for
  choosing among overloaded members, such as read-only, mutable and
  consuming `value()`, and they still put every hook on the type.
- **C++0x concept maps with remapping.** A map could rename operations, so
  constrained code saw a different interface from ordinary code. That
  interaction with unconstrained templates is the part GTI does not adopt.
- **Protocol-named members**, as the collection hooks use. They need no new
  mechanism, but every hook stays on the type's public API.

## Consequences

- ADR 009's exclusion of associated types is revised for concepts with a
  body. Concept specialization, constraint ranking, negation and general
  requires-expressions remain excluded.
- An inline constraint such as `<measurable T>` does not accept a concept
  with a body, because its atomic facts cannot express a block. A trailing
  `requires` clause does.
- Phase 1 boundaries:
  - one concept parameter (lifted by the 2026-10-15 revision);
  - type parameters only, without value parameters or packs;
  - patterns over class templates or concrete types, not `expected`, fixed
    arrays or pointers;
  - no default operations and no constraints on associated types;
  - no access through a composite concept;
  - `?` is not yet bound to a protocol concept (bound to `std::tryable` by
    [ADR 029](029-tryable-propagation.md)).
- Next phases: bind `?` to a standard protocol concept, and move existing
  member-name protocols (collection hooks, iterator value types) onto
  protocol concepts where coherent.
- Violations are reported as `GTI-S2123`.

## Revision: several parameters (2026-10-15)

A concept with a body may take several type parameters, such as
`concept adds_to<Left, Right> { typename sum_type; ... };`. Decisions 3 and 4
generalize as follows:

- A block gives one pattern per parameter. Each is a concrete type or one
  class template applied to block parameters. A bare parameter is not a
  pattern, so there are no blanket blocks. Every block parameter appears
  somewhere. A parameter may repeat, and its occurrences must bind the same
  type.
- Two blocks overlap when their patterns unify position by position. They are
  then rejected, so selection stays exact without ranking.
- The declaring package must own the concept or the top-level nominal type
  of at least one pattern. Without bare parameters, overlapping blocks in two
  packages would need each package to depend on the other, so this keeps the
  single-parameter rule's guarantee. Rust's orphan rule needs its ordering
  clause only because it admits blanket implementations.
- Generic code requires the exact application, arguments in order, as a
  conjunct.

## Revision: enum templates (2026-10-15)

A pattern may apply an enum template to block parameters, as it applies a
class template: `for <T, E> concept C<result<T, E>>`. Selection, overlap and
coherence are unchanged, and the enum template's package owns the pattern as
a class template's package does.

## Revision: built-in type patterns and block interfaces (2026-10-21)

A pattern may also apply a raw pointer or a fixed-length array to block
parameters: `for <T> concept C<T*>` or `for <T> concept C<T[3]>`. Selection
and overlap treat the built-in type as a template whose arguments are its type
arguments. A pointer's access and an array's length belong to its identity,
so `T*` and `const T*` neither overlap nor match each other's instances. An
array length cannot be a block parameter until blocks take value parameters.
No package owns a built-in type, so only the concept's package declares a
block over one. `expected` (a source class template since 0.449.0),
`std::unique_ptr` and `std::shared_ptr` are standard-library class templates
and were already patterns.

Two interface rules remove differences between a concrete use and a use
through a requirement:
- a block function cannot declare default arguments, which a concept's
  operations never have;
- a concrete associated type `C<X>::name` checks the selected block's
  parameter constraints, as `requires C<X>` and a concrete operation call
  already did.
