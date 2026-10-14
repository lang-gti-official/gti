# ADR 028: Structural Propagation Protocol For Prefix `?`

Status: Proposed

## Context

Prefix `?` propagates the failure of a fallible value out of the enclosing
callable. Until this decision it accepted exactly one operand family, the
built-in `expected<T, E>`: semantics checked the `SemanticType::Expected`
kind and an `expected<U, E>` return with the identical `E`, HIR built a
dedicated `ExpectedPropagation` plan from compiler intrinsics
(`ExpectedValue`, `ExpectedError`, `Unexpected`), and MIR lowered that plan to
a state branch whose error edge ends the full expression early and returns
`unexpected(error)`.

`std::optional` is ordinary GTI policy over a compiler-owned storage
capability, and user result types are ordinary classes. Neither could use
`?`: callers wrote `if (!x.has_value()) { return std::optional<U>(); }` by
hand. A future source-defined `expected` would also lose `?` unless the
operator stops recognizing one built-in type.

Rust makes `?` a desugaring through compiler-known traits (`Try` and
`FromResidual`): `Result` and `Option` are ordinary enums, and the operator
needs only a branch, an output and a residual. GTI has no trait impls, but it
already defines structural protocols over ordinary members: range-`for` over
`begin()`/`end()`, the owned collection protocol, and contextual
`operator bool`. Repository rules forbid recognizing a public standard-library
type by name, require the rule to be proven with an unrelated user type, and
require new executable behaviour to extend verified MIR rather than add an
AST/HIR-only emission path.

## Decision

Prefix `?` on an operand of class type uses a structural **propagation
protocol** made of ordinary member functions and one implicit constructor.
The built-in `expected<T, E>` keeps its dedicated path for now (see
[Relationship to `expected`](#relationship-to-expected)).

### The protocol

For `?operand` where `operand` has class type `S`, ordinary member lookup on
`S` finds three zero-argument, non-static, non-generic, non-virtual member
functions with GTI bodies (not `unsafe`, `[[track_caller]]` or
non-returning):

1. **State test** `bool has_value()` with a read-only receiver. `true`
   continues; `false` propagates.
2. **Output** `value()`. A named operand selects a read-only overload. A
   consumed operand (a temporary or an explicit `std::move(name)`) prefers a
   consuming `&&` overload and otherwise uses the read-only one, exactly as
   ordinary receiver selection treats an owned temporary. The selected
   overload returns either an owned non-`void` value without borrowed state,
   which becomes the result of `?operand`, or a read-only reference, whose
   referent is copied (requiring a copyable output type).
3. **Residual** `residual()`, selected the same way, returning an owned,
   movable residual value `Res` without borrowed state.

Mutable (`mut`) receivers are never part of the protocol, so a named operand
is only observed and is never consumed implicitly.

The **return side** is the immediately enclosing function or lambda's return
type `R`. `R` must declare exactly one `implicit` constructor whose sole
parameter is exactly `Res` by value. The residual exit constructs `R` through
it, exactly as the declaration `R result = residual;` would (section 3.5's
implicit-construction rule), then returns that value. No other conversion is
considered.

The `?operand` expression has the output type `T` (a value, never a place).

### Residual families in the first slice

The first slice admits one exact family: `R` and `S` are instances of the
same class declaration (an exact specialization counts as its primary). So
`optional<T>` propagates into `optional<U>` for any `U`, and a user
`Parsed<T>` into `Parsed<U>`, but neither into the other. The residual type
is the family's token and the implicit constructor's exact parameter type is
the check, so no cross-family conversion exists.

The residual-type design already has the shape that a later cross-family
rule would use: a return type that declares an implicit constructor from
another family's residual (as Rust's `FromResidual<R>` impls do). Admitting
that is a separate decision, not a relaxation of this one.

### Evaluation order and cleanup

`?operand` behaves as:

```text
S&& s = operand;                 // evaluated exactly once
if (!s.has_value()) {
  R result = s.residual();       // residual(), then R's implicit constructor
  // end the full expression's temporaries created so far, newest first,
  // then the ordinary lexical return cleanup
  return result;
}
s.value()                        // the result; copied when a reference
```

A named operand is the place itself; a temporary operand is one
full-expression temporary that every call borrows or consumes in place,
destroyed at the end of its full expression on the continuing path; an
explicitly moved place is described under [Ownership](#ownership). Only one of `value()` and
`residual()` runs per evaluation. Values that would be created after the
operator are never evaluated on the residual exit. Several `?` in one
expression run left to right and each exit cleans only the obligations active
at that point, as for `expected`. A defined runtime failure inside any
protocol call or the residual constructor follows that call's ordinary MIR
failure edge and cleanup; it is not a propagation.

### Ownership

This mirrors `expected` propagation:

- A **named place** is observed: the read-only overloads run on a read-only
  borrow of the place, which stays available afterwards. A reference output
  is copied. The ordinary loan rules reject a named operand that a live
  mutable borrow overlaps.
- A **temporary operand** is one full-expression temporary. Consuming
  overloads transfer its contents; its shell is still destroyed by the
  full-expression cleanup, and a consuming member owns leaving that shell
  destructible (moved-out fields are not destroyed twice).
- An **explicitly moved place**, `?std::move(name)`, stays in its place:
  read-only members borrow it and a consuming member receives it by move.
  On a path where no selected member consumes it (for example the residual
  exit when `residual()` is read-only), `name` keeps its ordinary lexical
  cleanup; semantics still treats `name` as moved afterwards. This mirrors
  what `std::move(name).member()` already means and needs no new temporary
  materialization in MIR.
- The residual is a fresh value moved into the implicit constructor. The
  constructed return value is published by the return, never dropped.

### `std::optional` as the first client

`<std/optional>` adds an empty residual type and the return-side constructor:

```gti
struct nullopt_t {};
class optional<T> {
  implicit optional(nullopt_t empty) {}   // empty optional<U>
  nullopt_t residual() { return nullopt_t(); }
  // bool has_value() and T& value() are the existing observers
};
```

`?` on an empty optional therefore returns an empty `optional<U>` of the
enclosing type. On success it copies the payload through the existing
read-only `T& value()`, so this slice requires a copyable payload.

A consumed optional should move its payload out instead. That needs a
consuming `T value() &&`, which in turn needs a compiler-owned
`optional_storage_take` capability (move the payload out and retire
engagement, failing with `empty_owner_access` when empty). That capability is
a storage-lifetime fact ordinary GTI cannot express, so it is justified as a
private capability bound by trusted declaration identity. It needs backend
storage support and is the first follow-up; adding the overload then changes
no protocol rule, because receiver selection already prefers it for consumed
operands.

### Relationship to `expected`

`expected<T, E>` stays on its dedicated semantic check, HIR
`ExpectedPropagation` plan and MIR lowering in this slice. The two paths are
deliberately parallel only until `expected` becomes a client:

| Protocol step | `expected<T, E>` as a client |
| --- | --- |
| `has_value()` | its existing state test |
| consuming `value() &&` | moves `T` (its `std::move(r).value()` today) |
| read-only `value()` | copies `T` for a named carrier |
| `residual()` | returns `unexpected<E>`, copying or moving `E` |
| implicit `expected(unexpected<E>)` | the existing `unexpected(error)` return |

The implicit constructor's exact parameter `unexpected<E>` reproduces today's
exact-`E` rule, and the receiver rules reproduce today's copy/move transport.
The runtime fixture already exercises that owning shape with an application
class: a consuming `value() &&` moves the payload and a consuming
`residual() &&` moves an owning cause out of temporary and explicitly moved
operands, with exact cleanup on both exits.
Two `expected` behaviours need protocol extensions first: `expected<void, E>`
needs a `void` output, and `expected<T&, E>` needs a *borrowed output* that
forwards the read-only loan with its receiver-origin authority instead of
copying the referent. When `expected` is source-defined (or its built-in
members are exposed as these protocol members), `?` on it lowers through the
protocol plan and the dedicated semantic check, `HirExpectedPropagationPlan`
and MIR `emitExpectedPropagation` are deleted in the same change; their
existing tests become protocol evidence. That deletion is the exit criterion,
so GTI never keeps two permanent execution rules for `?`.

### Diagnostics

`GTI-S2086` keeps ownership of invalid `?` expressions and now also
distinguishes a class that lacks a protocol member or declares it with the
wrong receiver, parameters or result; a named operand whose overloads only
consume (hint: `?std::move(name)`); an output that cannot be copied (hint:
declare a consuming `value() &&`); an enclosing return type outside the
operand's family; and a return type without exactly one implicit constructor
taking the residual. Inaccessible members or constructors report `GTI-S2007`
and unsatisfied member constraints report their ordinary constraint
diagnostic. No fix-it is offered: consuming a named operand, adding members
or changing a return type changes program intent.

### Layers

- **Syntax, formatter, Tree-sitter:** unchanged; `?` already parses.
- **Semantics** selects the three members with ordinary receiver rules, the
  return construction with the implicit-construction rule, checks the family,
  ownership and loans, records a `ResolvedPropagationInfo` on the `Unary`
  node, and adds the calls and construction to program effects.
- **HIR** lowers a `Propagation` value whose `HirPropagationPlan` names the
  operand, three ordinary `Call` values with selected function instances and
  temporary-borrow or borrow receivers, and a `DirectInitializer` of the
  selected constructor instance. HIR plan verification ties each call to the
  semantic declaration, the operand, its receiver kind and the full
  expression.
- **MIR** branches on the `has_value()` result and lowers the residual exit
  as the existing early-exit return (early full-expression boundary, reverse
  temporary drops, scope exit, returned value transfer). On the success path
  a result without a cleanup obligation is the `value()` call's own result,
  or the copy read through its call-result loan; a result with an obligation
  moves through a result temporary that takes over the call's obligation, as
  expected propagation does. No new MIR operation exists; the existing
  verifier checks calls, loans, lifecycle, cleanup and boundaries.
- **Backend:** no change; the C++ is emitted from verified MIR as for any
  call, construction, branch and return.
- **Tooling:** the LSP publishes the shared diagnostics; no editor surface
  changes.

## Consequences

`?` becomes a general language facility: an unrelated user class gains it by
declaring three members and an implicit constructor, and `std::optional`
gains it without any compiler knowledge of `optional`. Executable authority
stays in verified MIR made of ordinary calls.

The protocol reuses the observer names `has_value()` and `value()`, so a class
is not accidentally propagatable: `residual()` and the family's implicit
constructor are the explicit opt-in.

Deferred work, each with its named prerequisite:

- consuming optional extraction (the `optional_storage_take` capability);
- `void` outputs (needed for `expected<void, E>` and status-like types);
- borrowed outputs that forward a read-only loan (needed for
  `expected<T&, E>`);
- cross-family residuals through another family's residual constructor;
- a constraint naming the protocol, so a generic body can apply `?` to a
  type-parameter operand;
- constant evaluation of protocol propagation through the MIR constant
  evaluator;
- inherited protocol members and residual constructors with defaults,
  read-only reference parameters or constraints.
