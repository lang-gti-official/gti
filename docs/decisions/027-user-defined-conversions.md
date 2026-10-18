# ADR 027: Explicit-By-Default User-Defined Conversions

Status: Accepted

## Context

GTI originally required exact types at calls, returns, and assignment. Before this
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
initialization, `Target name = value;`, and explicitly declared value returns. GTI has no
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
- **No other contexts.** Calls, assignment, operator operands, `auto`
  inference, and brace construction are unchanged and never consult
  conversion operators. Contextual `operator bool` keeps its contexts;
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

## Revision: Declared return initialization (2026-10-18)

The declaration-only restriction is revised: `return expression;` initializes
an explicitly declared value return type with the same single implicit
constructor/operator selection. A return has one exact destination and no
call-site overload ranking, so requiring a temporary binding adds verbosity
without strengthening ownership. The source keeps constructor/member-call
copy, move, borrowing, access and failure checks; a named move-only conversion
argument still requires explicit transfer. Reference and borrow-carrying target
exclusions remain, and there are no conversion chains. Concrete generic return
destinations select and verify their calls during ordinary instance reanalysis.
The source-defined `optional(T)` constructor opts in through `implicit`, without
compiler recognition of optional. Example 166 demonstrates both optional and
an unrelated user-defined result.

## Consequences

Conversions are visible where they happen. An explicit conversion names its
target, and an implicit one is limited to a declaration or return whose
destination type is explicitly declared. Adding an implicit operator cannot change which function
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
