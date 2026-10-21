# ADR 029: Prefix `?` Through `std::tryable`

Status: Accepted (2026-10-15)

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
     [[borrowed_from(value)]] value_type take_value(T value);
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
   or lambda returns it, initializing its declared return type `R` as
   `R result = residual;` would:
   - the residual already has type `R`; or
   - `R` declares exactly one `implicit` constructor whose sole parameter is
     the residual's type.

   The return first destroys the full expression's earlier temporaries,
   newest first, and then performs ordinary lexical cleanup, as for
   `expected`.

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
  contract, now supplied by `borrowed_from` on protocol operations. Blocks still cannot match
  built-in type constructors. Moving the borrowed form alone would split one
  type across two rules.

## Consequences

- `expected` and `std::tryable` are two rules for one operator until the
  migration's prerequisites exist:
  - patterns over built-in type constructors;
  - full borrowed alternative storage and extraction (signature-level
    borrow-origin contracts for block operations are now available).

  The two rules agree on evaluation order, single evaluation, consumption of
  temporaries and explicit moves, and cleanup order. They differ for a named
  place: `expected` copies only the active alternative, while a tryable
  operand is copied as a whole.
- `std::optional` gains a block only once a source-defined optional can move
  its value out. Its private storage slot has no move-out operation, and the
  owner preferred not to add one as a workaround. The source-defined
  `std::experimental::optional`, over a payload enum and consuming payload
  matches, has the block, and is the planned replacement.
- At initial implementation, `?` through `std::tryable` was unavailable in
  constant evaluation; the MIR-execution revision below supersedes that bound.
- The operand must be a class, struct or payload-enum type. Payload enums
  joined in the 2026-10-15 revision below; a scalar-backed enum remains
  outside this phase.
- Violations are reported as `GTI-S2086`.

## Revision: payload-enum operands (2026-10-15)

A payload enum, or an instance of an enum template that a block's pattern
names, propagates like a class. No rule is specific to `?`:

- an ordinary call may borrow a fresh payload-enum value for one read-only
  `T&` argument, exactly as it borrows a fresh class value, so `has_value`
  borrows a temporary operand that way;
- a named place is copied and `std::move(place)` is consumed, as for a class;
- a conformance block's pattern may apply an enum template, such as
  `for <T, E> concept std::tryable<result<T, E>>`.

An enum declares no constructors, so a residual returned from a function
whose return type is a payload enum has that enum type, which is how a
`result<T, E>`-style enum returns its own failure.

## Borrowed protocol results (2026-10-15)

The original owned-result restriction is revised: protocol signatures may
declare the existing `borrowed_from(parameter)` contract, and `take_value`
uses it when its associated result is a reference. A consumed carrier with
one exact stored dependency may relay its external owner. The implementation
must not return the carrier's own storage or substitute another origin. HIR
retains the selected call contract; MIR verifies the same call and preserves
the result loan across the success edge. The residual remains owned. General
variant-conditional dependencies are separate required work, not an implicit
promise made by permitting this protocol contract.

## Constant execution through the same protocol (2026-10-18)

Prefix `?` in a constexpr function uses the ordinary selected protocol calls,
branch, residual conversion and early-return cleanup. The staged HIR query
requires available constexpr definitions for its call closure, then executes
verified MIR under the existing resource budgets. A runtime-only protocol
operation cannot enter that closure. No AST protocol interpreter or special
result-type execution path is introduced. This applies to unrelated source
carriers as well as source-defined library results; persistent constants still
must satisfy the constant profile's ordinary storage restrictions.
