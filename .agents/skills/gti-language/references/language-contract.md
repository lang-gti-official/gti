# GTI Language Contract

Use this reference for language design, semantic behavior, and feature review.
Read only the sections relevant to the task, plus the matching productions in
`docs/grammar.ebnf`. The grammar is the implemented syntax surface; this file
records cross-feature intent and constraints that grammar alone cannot express.

## Contents

- [Core Safety Rules](#core-safety-rules)
- [Bindings And Primitive Values](#bindings-and-primitive-values)
- [Classes, Construction, And Lifecycle](#classes-construction-and-lifecycle)
- [Functions, Calls, Operators, And Lambdas](#functions-calls-operators-and-lambdas)
- [Generics And Aliases](#generics-and-aliases)
- [Arrays, Control Flow, And Failure](#arrays-control-flow-and-failure)
- [Ownership, References, And Internal Capabilities](#ownership-references-and-internal-capabilities)
- [Source Units, Runtime, And Non-Goals](#source-units-runtime-and-non-goals)

## Core Safety Rules

- Prefer familiar C++ spelling and precedence unless GTI deliberately adopts a
  safer or simpler rule.
- Keep ownership, lifetime, nullability, and conversions explicit. Do not
  inherit unsafe C++ defaults, textual macros, hidden conversions, accidental
  undefined behavior, or order-dependent semantics by omission.
- Reject invalid GTI in semantic analysis instead of depending on generated C++
  errors. Keep the C++ backend replaceable and treat generated C++ as an
  implementation artifact rather than the specification.
- Reserve the `__gti_` identifier prefix for compiler-generated backend names
  and reject source identifiers using it before lowering.
- Define integer edge cases at the GTI level. Keep modulo-by-zero and invalid
  shifts checked; do not lower them to raw undefined C++ operations.
- Require every non-`void` call result to be used. Permit intentional call-site
  suppression only through `[[discard]]`.

## Bindings And Primitive Values

- Keep bindings and parameters immutable by default. Require `mut` for state
  that can change.
- Keep `static` explicit and scope-owned. Namespace-scope static has source-unit
  internal linkage. Class or struct static data and methods belong to the type
  and use `Type::member`; static data stays outside instance layout and
  lifecycle and requires an in-class initializer. Static methods have no
  receiver or `this`. Reject block-scope static, static operators, and
  generic-class statics until their missing semantic surfaces are designed.
- Keep `int8_t` through `int64_t` and `uint8_t` through `uint64_t` as canonical
  fixed-width spellings. Retain suffix-less forms as exact lexer aliases and
  normalize them to `_t` in the formatter. Neither spelling family is a prelude
  alias or requires an include.
- Decode decimal, hexadecimal `0x`, and binary `0b` integer literals to one
  uint64_t-bounded magnitude before semantic type selection. Do not inherit C++
  octal literals or suffix-driven type selection.
- Keep numeric conversions explicit with `Type(value)`. Preserve checked
  narrowing behavior in every backend instead of emitting unchecked casts.
- Keep `char` an exact unsigned 8-bit code unit distinct from `uint8_t`.
- Give string literals the trivial counted `std::string_view` type over static
  storage. Preserve embedded zero bytes, read-only checked traversal, and no
  allocation. Do not reintroduce an unqualified `string` primitive.
- Keep owning text in the standard library. `std::string` is a source-defined
  move-only owner over `gti_internal::storage<char>` with explicit allocating
  `clone()` and read-only structural iteration. Its source-defined iterator
  retains a checked borrow rather than exposing a pointer. Do not expose a
  dynamic owner-backed string view until it can carry an owner-tied lifetime.
- Keep `auto` initializer-driven and local to variable declarations, including
  loop initializers. Infer one exact complete value type in semantics, retain
  its access and ownership traits in HIR, and require `mut auto` for mutation.
  Confine `auto&` and `mut auto&` reference inference to range-for element
  declarations. Reject inference for globals, fields, parameters, returns,
  ordinary references, arrays, and untyped braced initializers. Reject invalid
  move-only copies before the backend.

## Classes, Construction, And Lifecycle

- Keep construction explicit and select one exact constructor parameter list.
  Permit `Type name{arguments};` only for direct construction of a declared
  class or struct. It is not C++ list, aggregate, initializer-list, copy-list,
  CTAD, or parenthesized declaration behavior.
- Generate default/copy/move construction, assignment, and destruction from
  frontend lifecycle metadata instead of inheriting C++ special-member
  suppression rules.
- Permit one public `Type(Type&) = default|delete;` copy policy and one public
  `Type(Type&&) = default|delete;` move policy. Keep `&&` confined to that exact
  move declaration, reject a structurally impossible `= default`, and keep
  assignment lifecycle independent. Do not accept custom copy/move bodies until
  place-aware field movement and partial initialization are represented.
- Keep fields immutable by default in semantics. Keep methods read-only by
  default and use trailing `mut` for a mutable receiver. Permit read-only and
  mutable receiver overloads with otherwise exact signatures; a mutable
  receiver prefers the mutable twin.
- Keep inheritance explicit and public. A class or struct may have at most one
  state-bearing class or struct base plus interface bases. Reject private
  inheritance, duplicate bases, cycles, diamonds, and multiple state-bearing
  bases; use composition for private implementation reuse. Do not infer C++
  `protected`, `final`, or open-class behavior.
- Treat `interface` as a pure public behavior contract. Its methods end in
  `= 0;`, are implicitly virtual, and introduce no instance state. Class and
  struct virtual roots require `virtual`; inherited implementations require
  `override` and must match parameters, receiver access, operator identity, and
  return type exactly. Do not add covariant returns or method generics.
- Keep abstract types unconstructible and prohibit value slicing. Derived-to-
  base conversion is available only where the program explicitly asks for a
  base reference or return; ordinary calls continue to use exact types. Record
  override roots, overload-lookup owner, explicit receiver, and static versus
  virtual dispatch in semantic metadata, HIR, and MIR.
- Construct the state-bearing base before fields by selecting one exact,
  accessible base constructor. Destruction of polymorphic objects is compiler
  generated and virtual where required; do not expose user-defined virtual
  lifecycle members.
- Spell the current-object expression `this`. Treat it as a non-null object
  receiver with `this.member` access, not as a source-level raw pointer. Keep
  `self` as an ordinary identifier.
- Treat `~Type()` as automatic, public, non-throwing cleanup with an implicitly
  mutable receiver. Run it only for an active value before reverse-order field
  destruction. Cleanup-owning classes are noncopyable. Generated moves transfer
  active-drop state; move assignment cleans the active target before replacing
  it. Do not expose manual destructor calls.
- Keep enums scoped and nominal. Accept `enum class`, default the backing type
  to `int32_t`, require `Enum::value`, and add no implicit integer or boolean
  conversions or injected enumerator names.

## Functions, Calls, Operators, And Lambdas

- Resolve overloads by one unique exact parameter-type match after generic
  substitution. Do not add implicit call conversions, conversion ranking,
  return-type overloading, ADL, or a concrete-over-generic preference. Receiver
  mutability may distinguish methods but not free functions. Record the
  selected callable identity in semantics.
- Restrict operator overloading to member `operator*`, `operator->`, prefix
  `operator++`, `operator[]`, `operator()`, `operator==`, `operator!=`, and
  contextual `operator bool`. Prefix increment has no parameters, returns
  `void`, and requires a mutable receiver; postfix overloads remain absent.
  `operator()` may have arbitrary arity but is non-generic. Resolve exact
  operands and receiver access in semantics and lower a direct selected method
  call. Do not synthesize equality candidates, add ADL, delegate selection to
  C++, or recursively resolve arrow proxies.
- Normalize `&&` and `and` to one logical token identity, and likewise `||` and
  `or`, so precedence, contextual boolean conversion, short-circuiting,
  optimization, and lowering cannot diverge.
- Keep lambdas lexical and explicit. Require named immutable value captures,
  explicit parameter and return types, and exact calls. Reject capture defaults,
  reference or init captures, `this`, mutable closure state, noncopyable
  captures, and escape. Permit a lambda or class function object to bind only
  to a direct by-value generic parameter with a visible body when that parameter
  is used as a non-escaping void-returning operation or exact bool predicate.
  Infer a predicate requirement only from a direct bool condition, explicit
  initializer or assignment, logical operand, or return. Recheck every
  invocation against the concrete lambda or exact `operator()` target during
  generic instantiation, and retain the contract in HIR and MIR. Permit a
  callable parameter to pass through another direct by-value generic parameter
  only when the selected callee parameter already has a proven non-escaping
  callable contract. Resolve forwarding chains independent of declaration
  order and retain every edge in semantics, HIR, and MIR. Reject ordinary
  generic forwarding, callable references, arbitrary and auto-deduced callable
  result types, and owning type erasure until their lifetime and result
  capabilities are explicit.
- Reject every non-`void` function, method, operator, or lambda that can reach
  the end of its body. Keep this a semantic control-flow guarantee. Only
  top-level `main` has an implicit zero return; require a defined `int main()`
  with no parameters until a typed argument surface exists.

## Generics And Aliases

- Keep named generics predictable. Infer function type arguments exactly from
  value arguments; do not add conversion-driven deduction, specialization, or
  unconstrained compile-time metaprogramming.
- Allow one frontend-owned standard constraint per type parameter. Keep the
  supported identities confined to `std::ordered`, `std::numeric`,
  `std::signed_numeric`, `std::integral`, `std::signed_integral`,
  `std::unsigned_integral`, and `std::floating_point`. Check concrete arguments,
  symbolic implication, and every constrained pack element. Do not add
  constraint-based overload ranking, signature distinction, user concepts, or
  `requires` clauses. Permit checked `T(value)` only for `std::numeric` or a
  stronger constraint.
- Let classes and structs follow type parameters with immutable `uint64_t`
  value parameters. Limit arguments to integer literals or an in-scope value
  parameter, and uses to fixed-array extents and nested class arguments. Keep
  value parameters out of functions, packs, defaults, and arbitrary constant
  expressions until those semantics are represented.
- Keep `using Name = Type;` namespace-scoped, transparent, and independent of
  declaration order. Canonicalize aliases before overload, ownership, HIR, and
  backend decisions. Reject cycles, generic aliases, and reference targets.
  Define `std::size_t` as `uint64_t` and `std::ptrdiff_t` as `int64_t` in the
  prelude rather than as compiler primitives.
- Confine variadic generics to one final function or method type pack, one
  matching final immutable by-value parameter pack, and expansion as the final
  argument to another variadic callable. Preserve exact element types. Consume
  a concrete pack as one unit on its first expansion when any element is
  move-only; copyable packs may expand repeatedly.
- Reject arbitrary expansion contexts, class packs, folds, indexing, multiple
  packs, and forwarding-reference deduction. Do not add per-element pack access
  before HIR can track independently owned pack places.

## Arrays, Control Flow, And Failure

- Treat fixed arrays as inline bounded values. Keep C++ declarator spelling,
  compile-time length identity, complete initialization, checked indexing, and
  no pointer decay or public raw-data escape.
- Permit checked `+`, `-`, `*`, `/`, and `%` arithmetic over literal extents.
  Keep one `uint64_t` value parameter as the complete extent until symbolic
  arithmetic participates in type identity. Preserve bounds checks unless an
  optimization proves them unnecessary.
- Keep range-for member-based and structural. The implemented groundwork
  accepts stable lvalue ranges with self-contained iterator/sentinel values and
  resolves `begin`, `end`, read-only-reference sentinel `operator!=`,
  checked-reference `operator*`, and prefix `operator++` before HIR. Follow
  [`docs/iterator-range-proposal.md`](../../../../docs/iterator-range-proposal.md)
  for fixed arrays, temporary ownership, owner-tied iterators, iteration loans,
  and invalidation rules; do not infer those guarantees from emitted C++.
- Keep `switch` exact and non-fallthrough. Permit concrete integers, `char`, and
  scoped enums. Require same-type compile-time labels, reject duplicates, and
  require every executable arm to terminate explicitly. Adjacent labels share
  an arm; every arm has its own lexical scope. `break` may exit a loop or
  switch, while `continue` remains loop-only.
- Model recoverable failure with built-in `expected<T, E>` and explicit
  `unexpected(error)`. Do not add exceptions or implicit propagation syntax.

## Ownership, References, And Internal Capabilities

Follow `docs/ownership.md` for the staged ownership design.

- Treat `std::move(value)` as an explicit unary move operation, not a library
  hint. Permit named movable locals and by-value parameters, including copyable
  and generic values. Consume the source until valid plain assignment
  reinitializes a `mut` binding.
- Reject moves from references, globals, fields, captures, temporaries, and
  partial places until their lifetime or initialization state has a sound
  model. Reject direct self-move assignment. Retain movement in binding metadata,
  HIR, and MIR so a backend cannot silently copy it.
- Permit method reference returns only when the borrow is proven to originate
  from `this`. Require a trailing mutable receiver and writable returned place
  for leading `mut T&`. Record the receiver or intrinsic argument that owns a
  borrowed call result and reject retained borrows from temporary storage.
- Permit one direct read-only reference field in a class or struct as the
  confined stored-borrow carrier. Require every constructor to bind it directly
  from one exact reference parameter. Make the carrier move-constructible,
  noncopyable, and nonassignable. Permit an instance method to return it only
  when its origin is derived from `this`. Reject mutable or multiple reference
  fields, nested/inherited borrowed state, user cleanup, global/static storage,
  and free-function escape until a broader lifetime model exists.
- Permit that exact carrier contract to retain one read-only
  `gti_internal::storage<T>&` only when the carrier is declared in an implicit
  or imported standard-library source unit. Limit the exception to the field
  and its read-only storage-reference constructor parameters. Continue to
  reject storage references in application code and in library locals, method
  parameters, returns, static storage, or mutable form.
- Conservatively reject invalidating operations on a borrowed move-only root or
  on any root retained by a stored-borrow carrier until lexical loan analysis
  can prove the borrow has ended. Do not generalize receiver-tied method
  returns into free-function reference returns without an explicit lifetime
  model.
- Derive class and struct ownership traits recursively from substituted field
  types. Reject aggregate copies and use after move in semantics; backends must
  consume recorded binding traits rather than nominal spelling.
- Keep raw pointers, pointer arithmetic, `new`, and `delete` out of ordinary
  safe GTI. Implement public ownership and containers as nominal classes under
  `std` over restricted `gti_internal` capabilities. A future opt-in dangerous
  surface requires a separate audited design.
- Bind internal capabilities by trusted semantic declaration identity, never by
  the public wrapper name. Adding an intrinsic does not make it a stable public
  API.
- Keep each intrinsic irreducible. It may enforce allocation, bounds,
  initialization, borrow, and drop invariants, but it must not expose wrapper
  policy such as logical size, capacity, engagement, or per-slot state. Keep
  public factories such as `std::make_unique` on the ordinary generic call path.
- Treat C++ smart pointers and storage helpers as backend representations, not
  the GTI ABI or runtime binding. Preserve ownership, transfer, and drop
  semantics in frontend metadata, HIR, and MIR.

## Source Units, Runtime, And Non-Goals

- Treat `#include "path.gti"` and `#include <std/name>` as dependency loading,
  never textual substitution. Keep includes top-level, canonicalized,
  load-once, and cycle-checked. Resolve standard-library imports only beneath
  configured GTI standard-library roots.
- Parse source units independently. Expose only the current unit, its direct
  includes, and the implicit prelude; do not leak transitive or sibling
  declarations. Only the entry unit may declare top-level `main`.
- Keep `#if` restricted to target selection. Parse every branch and do not grow
  it into a macro processor. Permit `#error "message"` as the only unconditional
  directive action; diagnose it only when its containing target branch is
  active.
- Do not infer target capabilities such as threading from `target.os`. Add a
  capability condition only after the target/runtime contract owns its value
  for both host and cross compilation.
- Keep services out of the parser. Expose ordinary portable APIs in `stdlib/`
  and cross to the host only through validated runtime bindings selected by
  semantic identity rather than public spelling.
- Do not assume support for user-defined or combined constraints, `requires`,
  specialization, value generic functions or packs, arbitrary compile-time
  evaluation, raw pointers, arbitrary reference escape or stored-reference
  graphs beyond the confined one-owner carrier, escaping or stored lambdas,
  multiple state-bearing inheritance, inheritance diamonds, covariant returns,
  user-defined virtual lifecycle members, exceptions, textual macros, implicit
  error propagation, named modules, exports, separate compilation, or a stable
  ABI unless the implementation and grammar explicitly add them.
