# ADR 023: Bounded Implicit Return Movement

Status: Accepted

## Context

GTI makes ownership consumption explicit with `std::move`, including at call,
initialization, assignment, capture, and return boundaries. That rule keeps
destructive reads visible, but an exact return of a move-only local or by-value
parameter is a uniquely constrained case: every successful path exits the
current callable, so the source cannot remain usable on that path. Requiring
`return std::move(value);` there adds frequent ceremony without communicating a
choice between preserving and consuming the same binding.

C++ applies implicit move rules during some return overload resolution. Rust
instead moves noncopyable values by default in all value-consuming contexts.
GTI does not adopt either model wholesale: C++'s conversion and overload rules
are broader than GTI's exact semantics, while Rust-style general implicit moves
would change calls, assignments, captures, and ordinary local initialization.

## Decision

GTI implicitly moves a return expression only when all of these conditions
hold:

- after transparent grouping, the expression is one whole named local or
  by-value parameter;
- the expression type exactly equals the callable's result type; and
- the concrete type is movable and not copyable.

The move consumes the source and uses the same availability, borrow-conflict,
partial-move, cleanup, and result-publication rules as explicit `std::move`.
Copyable values continue to copy on return. The rule does not apply to return
conversions, fields or other projections, globals, references, captures,
temporaries, or immovable values. It is not last-use inference and it does not
change any non-return consuming context.

Semantics owns the decision and records an instance-local fact because generic
substitution can change copyability. HIR materializes that fact as the existing
unary `Move` operation. MIR and the backend receive no new source-level rule;
they use the same verified move, pending-result cleanup, and `TransferOut`
publication path as explicit movement.

## Consequences

The common forwarding and local-result patterns become concise without making
destructive calls or assignments implicit. Generic code may write `return
value;`: a move-only concrete instance moves, while a copyable instance keeps
copy behavior. Converting success returns such as `T` into `expected<T, E>`
still require `std::move(value)` because the source and result types differ.

The rule intentionally differs from C++ when a copyable type also has a move
operation: GTI preserves a copy rather than silently preferring movement. A
future broader move-by-default model, implicit projections, or return
conversion movement must replace this decision with compatibility and
lifecycle evidence; it cannot be inferred by HIR, MIR, or a backend.
