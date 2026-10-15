# ADR 029: Prefix `?` Through `std::tryable`

Status: Accepted (2026-10-15); decision 5 revised 2026-10-15 (see below)

## Context

Prefix `?` propagated only built-in `expected<T, E>`. Making other types
propagate raised the question of how a type opts in.

A prototype (GitHub PR #144, never merged) found its protocol through member
names: `has_value()`, `value()` and a protocol-only `residual()`, plus an
implicit constructor on the return type. That put a hook on every
participating type's public API, and only that type's author could add it.
The owner's direction was to keep hooks off the type, keep C++ familiarity,
and avoid a `.propagate_residual()`-style member.
[ADR 028](028-protocol-concepts.md) supplied the mechanism: a concept with a
body, satisfied through `for concept` blocks outside the type.

## Decision

1. **The protocol.** The prelude declares:

   ```gti
   concept tryable<T> {
     typename value_type;
     typename residual_type;
     bool has_value(T& value);
     value_type take_value(T value);
     residual_type take_residual(T value);
   };
   ```

   The compiler binds `?` to this concept by trusted declaration identity: a
   concept with a body named `std::tryable` in the prelude. It knows nothing
   about the types that conform.

2. **Which rule applies.** An operand of built-in `expected<T, E>` keeps its
   built-in rule. Every other operand must satisfy `std::tryable`, through a
   block for a concrete type or through a `requires std::tryable<T>`
   conjunct in generic code.

3. **Operand transport.** The operand is evaluated once and passed like a
   by-value argument. A temporary or `std::move(place)` is consumed, and a
   named place is copied, so it needs a copyable type and stays usable.
   `has_value` borrows the operand. `take_value` or `take_residual` then
   consumes it, exactly once on each path.

4. **Success.** `?operand` evaluates to `take_value`'s result, of type
   `value_type`. When `value_type` is `void`, `?operand` produces nothing and
   is a statement.

5. **Failure.** `take_residual` produces the residual. The enclosing function
   or lambda returns it as an ordinary `return residual;` of that fresh value
   would, initializing its declared return type `R` as `R result = residual;`
   would:
   - the residual already has type `R`; or
   - ADR 027's one shared selection converts it: one `implicit` constructor
     of `R`, or one `implicit` conversion operator of the residual's class.

   The return first destroys the full expression's earlier temporaries,
   newest first, and then performs ordinary lexical cleanup, as for
   `expected`. (Revised; see below.)

6. **Execution.** Each step is an ordinary call of the selected block
   function. HIR records the steps as one propagation plan, and MIR lowers it
   to a branch with an early return. There is no new MIR operation and no
   backend path.

## Alternatives

- **Member names on the type** (the PR #144 prototype). Every hook stays on
  the public API, and a foreign type cannot join.
- **A two-shape structural rule.** Propagate `std::nullopt` or
  `unexpected(x.error())`, chosen by the operand's shape. It needs no blocks,
  but it fixes the kinds of failure and still reads members of the type.
- **Moving `expected` onto the protocol now.** Its borrowed form
  `expected<T&, E>` would need a block operation that returns a reference
  derived from a by-value parameter. That needs a signature-level origin
  contract, which blocks cannot declare yet. Blocks also cannot yet match
  built-in type constructors. Moving the borrowed form alone would split one
  type across two rules.

## Consequences

- `expected` and `std::tryable` are two rules for one operator until the
  migration's prerequisites exist:
  - patterns over built-in type constructors;
  - signature-level borrow-origin contracts for block operations.

  The two rules agree on evaluation order, single evaluation, consumption of
  temporaries and explicit moves, and cleanup order. They differ for a named
  place: `expected` copies only the active alternative, while a tryable
  operand is copied as a whole.
- `std::optional` gains a block only once a source-defined optional can move
  its value out. Its private storage slot has no move-out operation, and the
  owner preferred not to add one as a workaround. The source-defined
  `std::experimental::optional`, over a payload enum and consuming payload
  matches, has the block, and is the planned replacement.
- `?` through `std::tryable` is not available in constant evaluation yet.
- The operand must be a class or struct type in this phase. A payload enum
  can have a block, but `?` on it is rejected until the propagation plan can
  stage enum temporaries and places. Wrapping the enum in a class works, and
  is how `std::experimental::optional` is meant to be built.
- Violations are reported as `GTI-S2086`. A residual conversion that the
  shared selection rejects keeps that selection's own diagnostic, such as
  `GTI-S2122` for an ambiguity.

## Revision: the residual is an ordinary return (2026-10-15)

Decision 5 originally searched the return type for exactly one `implicit`
constructor taking the residual: a second conversion rule beside
declarations, which never considered conversion operators. When returns
gained ADR 027's conversions, the owner directed that the residual be
consolidated with ordinary initialization rather than keep its own resolver.

The residual is now an ordinary return of the residual. Semantics passes the
fresh residual, through a synthesized stand-in for the `take_residual`
result, to the same selection that `return residual;` and
`R result = residual;` use, and the hand-written constructor search is
removed. A residual therefore also converts through an `implicit` conversion
operator, an `implicit` constructor with defaulted trailing parameters, or a
constrained `implicit` constructor whose requirements hold; an ambiguity is
`GTI-S2122`, and an access or ownership failure is the selection's own
diagnostic. HIR represents the residual return as the same contextual
construction, or receiver-only conversion call, that a declaration or return
lowers to; MIR still branches to the early return and adds no operation.

The built-in scalar and `expected` initialization conversions that an
ordinary return also accepts, such as an integer widening, do not apply to a
residual yet: the propagation exit returns either the residual itself or a
selected call, and it does not share the ordinary return's built-in
conversion. Supporting them needs that exit to return through the ordinary
return lowering. They remain `GTI-S2086`.
