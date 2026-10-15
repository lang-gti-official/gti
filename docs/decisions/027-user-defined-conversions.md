# ADR 027: Explicit-By-Default User-Defined Conversions

Status: Accepted; revised 2026-10-15 for returns (see the revision below)

## Context

GTI requires exact types at calls, returns, and assignment. Before this
decision, a class could convert to another type only through a constructor
of the target or a named method, apart from the contextual `operator bool`
of conditions. Declarations gained a bounded exception in 0.426.0: a
constructor declared `implicit` constructs a declared class from one value
of another type, as `T name{value}` would.

That left a gap in the opposite direction. When the target's author cannot
or should not know the source, as with a strongly typed identifier wrapping
a database key or a builder producing a `std::string`, the conversion
belongs to the source. C++ answers with conversion operators that are
implicit unless marked `explicit`, and that participate in overload
resolution, user-defined conversion sequences, and ranking against
converting constructors. That model makes a type's conversions part of every
call site's overload set and is a frequent source of surprising selections.

## Decision

A class may declare conversion operators, `operator Target()`. They are
explicit by default. `Target(value)` calls one, including the keyword forms
`int(value)` and `bool(value)` and an alias of the target. Writing `implicit`
first, as for constructors, additionally admits the operator in declaration
initialization, `Target name = value;`, and nowhere else. GTI has no
`explicit` keyword.

- **One shared selection.** A conversion from a class `U` to `Target` has two
  possible paths: the constructors of `Target` that take the value, and the
  conversion operators of `U` to exactly `Target`. Each path counts every
  candidate whose types match, whatever access, receiver, or ownership it
  would then require, so a rejected candidate never lets a conversion fall
  back to the other path. Exactly one path may exist. There is no preference
  rule, and a constructor and an operator together are ambiguous. A
  declaration considers only implicit constructors and implicit operators.
- **One step.** No conversion chains: a conversion operator never composes
  with a constructor, another conversion operator, or a numeric conversion.
- **No other contexts.** Calls, returns, assignment, operator operands, `auto`
  inference, and brace construction are unchanged and never consult
  conversion operators. (Revised: returns now convert as declarations do;
  see below.) Contextual `operator bool` keeps its contexts;
  `bool(value)` calls it explicitly and `bool flag = value;` needs
  `implicit operator bool()`.
- **Ordinary member calls.** A conversion is a call of the selected operator
  with the converted value as its receiver. Read-only, `mut`, and consuming
  (`&&`) receivers follow the member-call rules, so a named owner is never
  consumed implicitly. Ownership, borrowing, failure, cleanup, unsafe, and
  program-effect obligations are the call's.
- **Owned results.** The target is an owned value type. References, text
  views, borrow-carrying targets, `void`, and the owner class are rejected.
- **Identity.** A conversion operator's target is part of its overload
  identity. Aliases denote their resolved target, and `implicit` does not
  distinguish declarations.

Semantics owns selection and records the operator on an explicit
conversion's node, or as a contextual conversion of a declaration's
initializer. HIR lowers both as a receiver-only call of the selected
function. MIR and the backend add no conversion rule; they execute the
selected call.

## Consequences

Conversions are visible where they happen. An explicit conversion names its
target, and an implicit one is limited to a declaration whose type is
written beside it. Adding an implicit operator cannot change which function
a call selects, because calls never convert. Adding an operator or
constructor that creates a second path makes existing conversions ambiguous
instead of silently selecting one; `Target{value}` still selects a
constructor explicitly.

Two capabilities are deferred, each with a named prerequisite:

- **Borrow-carrying targets.** A conversion to a view would need the
  stored-borrow provenance of a construction expression at every conversion
  site, including the receiver's lifetime for a declaration's temporary
  receiver.
- **Virtual conversion operators.** Every target shares one member name, so
  virtual dispatch needs a dispatch slot per target. The current C++
  representation names virtual members by source name, so two targets
  collide, while target-derived names cannot follow overrides of a dependent
  `operator T()` or overrides of several interface roots. A class can declare
  a virtual named method and a conversion operator that calls it.

A future general implicit-conversion model, conversion ranking, or a
conversion in another context must replace this decision rather than extend
it locally.

## Revision: returns (2026-10-15)

The owner directed: "Allow implicit constructors in returns." A return
initializes one owned value of a written type, as a declaration does, yet
`R result = value; return result;` was valid where `return value;` was not.
A function returning a source-defined optional had to write
`return optional<T>();` rather than `return nullopt;`, while `?` already
returned its residual through an implicit constructor (ADR 029).

1. **Returns convert as declarations do.** In a function, method or lambda
   whose declared return type `R` is an owned value type, `return value;`
   with a value of another type `U` converts exactly as `R result = value;`
   does: the one shared selection above, considering only `implicit`
   constructors of `R` and `implicit` operators of `U`, with one path, no
   ranking and no chains. A return the existing rules already accept, such
   as an exact type, an integer widening or an `expected` success, is
   unchanged. The exclusions are the declaration's: reference returns, text
   views, borrow-carrying targets and `void` keep their rules and
   diagnostics. GTI has no deduced return types (`auto` is not a return
   type, and a lambda declares `-> R`), so every converting return names its
   target.
2. **Implicit movement.** ADR 023 still decides which returned operands may
   move: after grouping, a whole named local or by-value parameter whose
   concrete type is movable and not copyable. A conversion that takes its
   operand by value, an `implicit` constructor whose parameter is not a
   reference or a consuming (`&&`) `implicit` operator, consumes such an
   operand by move and then selects exactly as `R result = std::move(value);`
   would. A conversion that borrows it, through a reference parameter or a
   read-only or `mut` receiver, uses the place as the declaration does, and
   the operand lives until lexical cleanup. Any other named place is copied,
   or rejected when it cannot be, as in a declaration. Moving into a
   borrowing conversion was rejected: a reference parameter accepts only a
   place, so `return value;` would have failed where `R result = value;`
   succeeds, and the move would have bought nothing. A generic body is
   checked before its instances, with a type parameter treated as copyable,
   so there a named operand of a consuming operator still needs `std::move`;
   each concrete instance decides the move from its substituted type.
3. **Propagation.** Prefix `?` returns its residual as an ordinary
   `return residual;` of that fresh value (ADR 029's revised decision 5).
4. **Constant evaluation.** A converted return in a `constexpr` function
   evaluates at compile time exactly where the declaration form does: the
   selected constructor or operator must itself be `constexpr`, and the
   staged verified-MIR evaluator executes it as the ordinary call it is. The
   legacy AST evaluator has no conversion rule; it rejects a result whose
   type differs from the declared one instead of converting it.
5. **Execution.** Semantics records the selection on the returned
   expression, in the same contextual construction or conversion record a
   declaration's initializer uses, and an implicit move as ADR 023's fact.
   HIR lowers the return as the selected construction, or receiver-only
   operator call, whose operand is the returned value or the one `Move` of
   it. MIR returns that fresh value like any other: the operand's
   temporaries end with the full expression, the result is published, and
   lexical cleanup follows. No HIR value kind, MIR operation or backend rule
   was added.

Consequences: `return std::experimental::nullopt;` works, and a move-only
local returns through a consuming conversion without `std::move`. Calls,
assignment, operator operands, `auto` inference and brace construction still
never convert, so adding an implicit constructor or operator can make a
rejected return valid but never changes which function a call selects. This
revision replaces the treatment of returns in the "No other contexts" rule;
the rest of the decision stands.
