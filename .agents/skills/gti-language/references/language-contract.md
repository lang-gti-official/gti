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
- [Source Units, Native C Interop, Runtime, And Non-Goals](#source-units-native-c-interop-runtime-and-non-goals)

## Core Safety Rules

- Prefer familiar C++ spelling and precedence unless GTI deliberately adopts a
  safer or simpler rule.
- Keep ownership, lifetime, nullability, and conversions explicit. Do not
  inherit unsafe C++ defaults, textual macros, hidden conversions, accidental
  undefined behavior, or order-dependent semantics by omission.
- Confine one-level raw address operations to lexical `unsafe {}` blocks.
  Unsafe code remains type checked and transfers only the documented pointer
  validity obligations to the programmer; it is not a general diagnostic
  suppression mechanism.
- Reject invalid GTI in semantic analysis instead of depending on generated C++
  errors. Keep the C++ backend replaceable and treat generated C++ as an
  implementation artifact rather than the specification.
- Reserve the `__gti_` identifier prefix for compiler-generated backend names
  and reject source identifiers using it before lowering.
- Define integer edge cases at the GTI level. Signed and unsigned addition,
  subtraction, and multiplication trap on overflow; integer division traps on
  zero and signed minimum divided by `-1`; unary negation and
  increment/decrement use the same checked rules. Keep modulo-by-zero and
  invalid shifts checked; do not lower integer operations to raw undefined C++
  behavior.
- Fold integer arithmetic only through the typed checked-integer evaluator. A
  proven in-range result may become an exact typed constant; a proven overflow,
  zero divisor, or invalid shift must retain the original runtime failure unless
  language control flow proves that the operation is not evaluated.
- Require every non-`void` call result to be used. Permit intentional call-site
  suppression only through `[[discard]]`.

## Bindings And Primitive Values

- Keep bindings and parameters immutable by default. Require `mut` for state
  that can change.
- Keep raw-pointer binding access separate from pointee access. `mut T*`
  reseats a writable-pointee pointer, while `const T*` has a read-only pointee.
  Require explicit initialization for raw-pointer variables and fields.
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
- Compound assignment evaluates its target place once, applies the matching
  checked arithmetic, bitwise, remainder, or shift rule, and checks conversion
  of the result back to the target type. An integer target rejects a floating
  right operand until the source explicitly converts it.
- Keep `char` an exact unsigned 8-bit code unit distinct from `uint8_t`.
- Give string literals the trivial counted `std::string_view` type over static
  storage. Preserve embedded zero bytes, read-only checked traversal, and no
  allocation. Do not reintroduce an unqualified `string` primitive.
- Keep owning text in the standard library. `std::string` is a source-defined
  move-only owner over `gti_internal::storage<char>` with explicit allocating
  `clone()` and read-only structural iteration. Its source-defined iterator
  retains a checked borrow rather than exposing a pointer. The single-origin
  lifetime model can now support a dynamic owner-backed string view, but do not
  expose that API until the public type adopts the carrier contract and its
  invalidation behavior is tested.
- Keep `auto` initializer-driven and local to variable declarations, including
  loop initializers. Infer one exact complete value type in semantics, retain
  its access and ownership traits in HIR, and require `mut auto` for mutation.
  Confine `auto&` and `mut auto&` reference inference to range-for element
  declarations. Reject inference for globals, fields, parameters, returns,
  ordinary references, arrays, and untyped braced initializers. Reject invalid
  move-only copies before the backend.
- Permit `auto [name, ...] = expression;` as one immutable local declaration.
  Evaluate the expression once into one hidden owned value, then expose each
  name as a read-only projected place. Support exact-arity fixed arrays and
  flat class/struct values whose direct instance fields are all public. Reject
  mutable or reference forms, nested patterns, inherited fields, stored
  references, untyped braces, and partial moves until MIR can prove their
  initialization and loan behavior. Keep decomposition facts in semantics,
  HIR, and MIR rather than deriving them from C++ structured-binding behavior.

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
  constructor-wide definite initialization and active-drop transitions are
  represented.
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
  substitution, apart from the bounded raw-pointer null and added-pointee-const
  compatibility. Do not add general implicit call conversions, conversion
  ranking, return-type overloading, ADL, or a concrete-over-generic preference.
  Receiver mutability may distinguish methods but not free functions. Record
  the selected callable identity in semantics.
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
- Allow one namespace-resolved unary concept per type parameter. Permit
  namespace-scoped source concepts to compose existing concepts with `&&` or
  `and`, and require each application to use the declaration's one type
  parameter. Public numeric identities in the implicit prelude are
  `std::numeric`, `std::signed_numeric`, `std::integral`,
  `std::signed_integral`, `std::unsigned_integral`, and
  `std::floating_point`; lifecycle identities are `std::copyable`,
  `std::movable`, and `std::default_initializable`; comparison identities are
  `std::equality_comparable` and `std::totally_ordered`. Retain
  `std::ordered` as a compatibility spelling for total ordering. Check concrete
  arguments, symbolic set inclusion, and every constrained pack element.
- Keep compiler knowledge beneath public concepts. Bind only trusted
  `gti_internal` prelude concepts to irreducible semantic facts; keep public
  names, aliases, composition, and implication in `stdlib/prelude.gti`. Reject
  `@compiler_constraint` outside that trusted namespace and source unit.
- Require exact public, read-only `bool` member contracts for nominal class
  comparison capabilities. Equality requires `operator==` and `operator!=`;
  total ordering additionally requires `operator<`, `operator<=`, `operator>`,
  and `operator>=`. Every comparison takes one read-only reference to the same
  concrete class type. Do not synthesize missing operators, use implicit
  conversions, or infer capabilities from the C++ backend.
- Express implications such as copyability plus movability, total ordering plus
  equality, and numeric lifecycle/comparison requirements by composing source
  concepts in the prelude. Permit checked `T(value)` only when the resolved
  capability set includes numeric, and permit `T()` only when it includes
  default initialization. Do not add constraint-based overload ranking,
  signature distinction, disjunction, expression requirements, specialization,
  or `requires` clauses.
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
  move-only; copyable packs may expand repeatedly. This permits bounded
  source-defined emplacement but is not C++ perfect forwarding: copyable
  arguments may be copied when entering a wrapper pack, and GTI has no
  forwarding references or reference collapsing.
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
  accepts stable lvalue ranges with self-contained iterator/sentinel values or
  the confined standard-library iterator that retains one read-only checked
  storage borrow. It resolves `begin`, `end`, read-only-reference sentinel
  `operator!=`, checked-reference `operator*`, and prefix `operator++` before
  HIR. Follow
  [`docs/iterator-range-proposal.md`](../../../../docs/iterator-range-proposal.md)
  for fixed arrays, temporary ownership, mutable/general owner-tied iterators,
  iteration loans, and invalidation rules; do not infer those guarantees from
  emitted C++.
- Keep `switch` exact and non-fallthrough. Permit concrete integers, `char`, and
  scoped enums. Require same-type compile-time labels, reject duplicates, and
  require every executable arm to terminate explicitly. Adjacent labels share
  an arm; every arm has its own lexical scope. `break` may exit a loop or
  switch, while `continue` remains loop-only.
- Keep `do { ... } while (condition);` body-first and explicit. Execute the
  body at least once, route `continue` through the condition, require the same
  exact contextual-bool conversion as other loops, and perform lexical cleanup
  before every `continue`, `break`, or return edge.
- Keep `condition ? true_value : false_value` lazy and value-producing. Require
  an exact contextual-bool condition and the same exact type in both arms.
  Materialize place arms by copy, require explicit `std::move` for move-only
  places, and merge moved-state facts from both runtime paths. Reject result
  types that contain borrowed state until MIR can represent branch-selected
  loan origins; do not inherit C++ conditional-lvalue or implicit-conversion
  rules.
- Model recoverable failure with built-in `expected<T, E>` and explicit
  `unexpected(error)`. Do not add exceptions or implicit propagation syntax.

## Ownership, References, And Internal Capabilities

Follow `docs/ownership.md` for the staged ownership design.

- Treat `std::move(value)` as an explicit unary move operation, not a library
  hint. Permit named movable locals, by-value parameters, and writable named
  field paths rooted in those values or mutable `this`, including fields
  reached through checked `operator->`. Consume the source until valid plain
  assignment reinitializes the binding or exact field. Require every moved
  receiver field to be reinitialized before a method returns, and reject
  whole-owner transfer while an owned field remains moved.
- Reject moves from references, globals, captures, temporaries, and indexed
  places until their lifetime or initialization state has a sound model.
  Reject direct self-move assignment. Retain movement in binding metadata,
  projected semantic state, HIR, and MIR so a backend cannot silently copy it.
- Permit method reference returns only when the borrow is proven to originate
  from `this`. Permit a read-only free-function or static-method borrowed
  return when every reachable return derives from one eligible read-only
  parameter. A `T&` parameter may introduce the dependency; a by-value direct
  borrowed-state carrier may only transfer its existing dependency, including
  through a concrete generic instance. The declaration must name that carrier
  shape; an unconstrained `T` parameter does not gain borrow propagation from
  its eventual substitution. Record the receiver or exact parameter
  index that owns a borrowed call result and reject retained borrows from
  temporary storage. Keep mutable free/static returns unavailable; require a
  trailing mutable receiver and writable returned place for a method returning
  `mut T&`.
- Permit one direct read-only reference field in a class or struct as the
  confined stored-borrow carrier. Require every constructor to bind it directly
  from one exact reference parameter. Make the carrier move-constructible,
  noncopyable, and nonassignable. Permit an instance method to return it only
  when its origin is derived from `this`; permit a free/static result derived
  from one eligible read-only parameter. Preserve that dependency through
  calls, concrete generic carrier relays, moves, returns, and drops. Reject
  mutable or multiple reference fields, nested/inherited borrowed state, user
  cleanup, global/static/captured escape, dependency-changing assignment, and
  multi-origin return paths until a broader lifetime model exists.
  Treat a projection through `expected<owner, E>.value()` as nested: immediate
  full-expression use is permitted, but retaining or returning that projection
  is not part of this contract.
- Permit that exact carrier contract to retain one read-only
  `gti_internal::storage<T>&` only when the carrier is declared in an implicit
  or imported standard-library source unit. Limit the exception to the field
  and its read-only storage-reference constructor parameters. Continue to
  reject storage references in application code and in library locals, method
  parameters, returns, static storage, or mutable form.
- Conservatively reject invalidating operations on a borrowed move-only root or
  on any root retained by a stored-borrow carrier until lexical loan analysis
  proves the borrow has ended. For one unshared local carrier, permit a proven
  endpoint after its final straight-line use, at a reachable nested `if` merge,
  or on a used/unused branch entry. A terminating arm leaves through ordinary
  cleanup and does not constrain the reachable merge. For ordinary `while`,
  body-first `do`/`while`, and classic `for`, a pre-existing unshared carrier
  may remain active through every backedge and `continue`, then end once after
  condition-false and `break` paths converge. The same bounded one-carrier
  analysis may end at a switch's unified exit, or after a final use before a
  same-path invalidation immediately followed by the matching `break`. MIR
  normalizes every relevant outgoing edge and requires predecessor loan states
  to agree at the join. Do not generalize
  this into arbitrary nested switch/loop flow. Keep shared read-only aliases
  and general mutable reborrow/exclusive-loan graphs conservative. Do not
  generalize the implemented single-origin read-only free/static return rule
  into nested, multiple, captured, global, stored, or dependency-changing
  lifetime graphs.
- Derive class and struct ownership traits recursively from substituted field
  types. Reject aggregate copies and use after move in semantics; backends must
  consume recorded binding traits rather than nominal spelling.
- Keep raw pointers non-owning and loan-free. Safe code may carry, copy, pass,
  return, compare, and null-initialize compatible one-level `T*`/`const T*`
  values. Require lexical unsafe for address formation, dereference, indexing,
  arrow access, pointer arithmetic, and pointer-bearing C calls. Reject decay,
  `T**`, pointer references, typed/`void*` conversions, casts, function
  pointers, `new`, and `delete`. Implement public ownership and containers as
  nominal classes under `std` over restricted `gti_internal` capabilities.
- Bind internal capabilities by trusted semantic declaration identity, never by
  call-site spelling or the public wrapper name. Declare them as ordinary
  bodyless functions in the implicit prelude and carry the selected function
  identity into the intrinsic semantic record. The same spelling outside the
  trusted prelude remains ordinary. Adding an intrinsic does not make it a
  stable public API.
- Keep each intrinsic irreducible. It may enforce allocation, bounds,
  initialization, borrow, and drop invariants, but it must not expose wrapper
  policy such as logical size, capacity, engagement, or per-slot state. Keep
  public factories such as `std::make_unique` on the ordinary generic call path.
- Keep `gti_internal::storage<T>` a move-only owner of partially initialized,
  non-borrowed elements. Variadic construction expands the
  concrete final pack and selects one exact accessible class constructor, or
  performs zero/one-argument exact primitive construction, directly in an empty
  slot. Preserve that nested constructor identity in HIR and MIR. Relocation
  requires a movable element and destroys the moved source slots.
- Keep `std::vector<T>` an ordinary source-defined, move-only class constrained
  to `std::movable T`. Its first surface has checked indexed access, capacity
  management, push/pop, clear, variadic in-place `emplace_back`, and read-only
  one-owner iteration. Do not infer full invalidation, mutable iteration,
  temporary-range, or C++ allocator semantics from that subset.
- Treat C++ smart pointers and storage helpers as backend representations, not
  the GTI ABI or native C ABI. Preserve ownership, transfer, and drop
  semantics in frontend metadata, HIR, and MIR.

## Source Units, Native C Interop, Runtime, And Non-Goals

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
  and put narrow native calls behind source-defined GTI wrappers.
- Accept `extern "C" { ... }` only as a namespace-scope block of bodyless free
  functions. Require the decoded linkage string to be exactly `C`. Reject
  methods, definitions, generics, overloads, redeclarations, runtime
  attributes, static/virtual/receiver qualifiers, and native variables.
- Use the exact function identifier as one program-global native C symbol.
  Preserve source namespaces for lookup without qualifying or mangling that
  symbol. Reserve `main` for the GTI entry point and reject collision with
  root-namespace GTI storage. Record `LanguageLinkage::C` and `externalSymbol`
  in semantics and carry them through concrete HIR and MIR instead of
  rediscovering linkage in a backend.
- Limit C ABI returns to `void`, fixed-width signed or unsigned integers,
  `float`, and one-level raw pointers to those scalars or `void`. Limit
  parameters to immutable by-value instances of those scalars and pointer
  shapes plus `std::string_view`. Permit `const` pointees. Resolve transparent
  aliases before applying the allowlist. Reject bool, char, enums, classes,
  expected, owners, references, arrays, packs, string-view returns,
  pointer-to-pointer types, and function pointers. Require unsafe only for a
  call whose source signature contains a raw pointer; scalar/counting-input
  calls remain safe.
- Lower a C-linkage `std::string_view` parameter to
  `gti_c_string_view { const char *data; uint64_t length; }` from
  `runtime/include/gti/c_abi.h`. Treat it as a counted, read-only input valid
  only for the call; the callee must not retain it, assume NUL termination, or
  read beyond its length. Do not infer ownership transfer or a general native
  record layout facility from this one explicit ABI type.
- Keep linking separate from declaration semantics. Direct mode may forward
  native library arguments after `--`; project mode may resolve structured
  package/profile/target native inputs under the build-system contract. Neither
  source form makes an unresolved C symbol available by declaration alone.
- Retain `@runtime("...")` only as a closed compiler-validated compatibility
  mechanism. The standard prelude's host entries use bounded `extern "C"`;
  neither surface grants native behavior from call-site spelling.
- Keep `<std/tcp>` bounded to an unconnected POSIX IPv4 stream-socket owner
  over scalar `socket`/`close` declarations. Its move-only lifetime and close
  errors are ordinary GTI policy. Do not add connect, address, or traffic APIs
  until the language owns reviewed address records and bounded byte buffers.
- Do not assume support for concept disjunction, expression requirements,
  `requires`, specialization, value generic functions or packs, arbitrary
  compile-time evaluation, pointer-to-pointer/function-pointer types, casts,
  source allocation or manual lifetime, arbitrary reference escape or
  stored-reference graphs beyond the confined one-owner carrier, escaping or
  stored lambdas, multiple state-bearing inheritance, inheritance diamonds,
  covariant returns, user-defined virtual lifecycle members, exceptions,
  textual macros, implicit error propagation, named modules, exports, separate
  compilation, or a stable ABI unless the implementation and grammar explicitly
  add them.
