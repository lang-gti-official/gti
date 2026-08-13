# 3. Static Semantics

Status: Current high-level semantic contract with explicit specification gaps.

This chapter records the major semantic categories. Focused documents in this
directory provide detailed ownership, pointer, generic, range, interop, and
library rules; source and tests decide current implementation when this draft
has drifted.

## 3.1 Type Categories

The implemented language includes:

- primitive integers, `float`, `double`, `bool`, and `char`;
- `void` in permitted return and `expected` positions;
- `nullptr_t`;
- integral scoped enumerations and closed payload-enum types;
- class, struct, interface, and passive union types;
- fixed arrays whose extents participate in type identity;
- non-null read-only and mutable reference types;
- nullable one-level `T*` and `const T*` raw-pointer types;
- `expected<T, E>`;
- compiler-private unique-owner and storage capability types;
- generic type and value parameters; and
- lexical lambda types.

Aliases are transparent names and do not introduce new nominal types.

## 3.2 Bindings And Access

Bindings and parameters are immutable unless declared `mut`. Fields are also
immutable unless declared `mut`. Methods have read-only receivers unless their
declaration has a trailing `mut` qualifier. The bounded trailing `&&` receiver
qualifier is available only on `operator()` and denotes consuming access, not a
general rvalue-reference type. A consuming call operator may mutate its
receiver while consuming it.

`T&` denotes a non-null read-only borrow of `T`. `mut T&` denotes a non-null
writable borrow. Reference binding and escape are subject to the ownership
rules in [Ownership And Lifetimes](ownership-and-lifetimes.md).

A local mutable reference may be reborrowed as a mutable or read-only child
when its source has a stable root with only named-field and checked-dereference
projections. The mutable parent is suspended while any child loan remains
active and fully reactivates only after the final active child's proven
endpoint. Access through a known-disjoint named-field projection remains
available, and multiple child loans may coexist only over such disjoint
places. Parent or owner access that overlaps an active child is ill-formed,
and a read-only loan cannot be upgraded to a mutable child. Indexed, raw, and
opaque sources do not receive this bounded precise-place treatment. This rule
does not permit mutable stored-reference fields, returning any local child
reborrow (mutable or read-only) directly or through a stored carrier, or
mutable owner-tied range iteration.
Existing receiver-tied mutable-reference returns and structural range writable
yields remain governed by their narrower rules.

A local borrowed-state carrier may retain an ordinary read-only owner
dependency, but it may not retain a mutable parent loan or a child reborrow.
The latter shape is limited to non-retained full-expression temporaries.

For a receiver- or argument-tied result, the stable place is the selected
origin expression. The compiler preserves a caller-visible field path such as
`parent.left`, but it does not infer an internal returned-field projection from
the callee body. A result tied to a whole receiver or parameter therefore
conservatively protects that whole origin.

Call and construction arguments are checked under strict left-to-right
evaluation. If an earlier argument produces a transient borrow, a later
mutation of an overlapping place is ill-formed because that loan remains active
through the full-expression. An earlier completed mutation may be followed by
a later borrow when no other loan conflicts. The current compiler still
rejects the pair in either written order until the matching ordered MIR/backend
family implements this distinction.

Local `auto` infers one exact complete value type from its initializer. It does
not infer globals, fields, parameters, returns, arrays, or untyped braced
initializers. The range-for forms `auto&` and `mut auto&` infer element borrows;
plain `auto` does not silently infer a reference.

For a raw pointer, leading `mut` controls whether the pointer binding may be
reseated. Pointee access is writable for `T*` and read-only for `const T*`.
A raw-pointer variable or field requires an explicit initializer. Raw pointers
are trivial non-owning values and create no semantic loan.

## 3.3 Raw Pointers And Lexical Unsafe

Carrying, copying, passing, returning, assigning, null comparison, compatible
equality, and adding pointee qualification from `T*` to `const T*` do not
require an unsafe block. Pointee qualification is not removed implicitly, and
GTI provides no implicit typed-pointer/`void*` conversion or raw-pointer
truthiness.

Address formation, raw dereference or store, raw indexing, raw arrow access,
pointer arithmetic, and a pointer-bearing C call are well-formed only within a
lexically enclosing `unsafe` block. A nested lambda starts a fresh safety
context. Unsafe context does not suppress type, access, initialization,
ownership, or control-flow checking.

Exactly one pointer level is supported. Pointer-to-pointer types, references to
raw pointers, function pointers, pointer-to-array types, implicit fixed-array
decay, casts, and source allocation/deallocation expressions are ill-formed.
`void*` is opaque and cannot be dereferenced, indexed, used for member access,
or used in pointer arithmetic.

The complete programmer obligations are incorporated from
[`raw-pointers.md`](raw-pointers.md).

## 3.4 Names, Scopes, And Visibility

Namespaces, namespace aliases, and qualified `::` names use lexical declaration
and source-unit visibility. Namespace-scoped `using Name = Type;` aliases are
declaration-order independent after cycle validation.

Class members follow their declared access and the default associated with
`class` or `struct`. `interface` members are public contracts. Namespace
`static` declarations have source-unit internal linkage. Type-owned static
members do not participate in instance layout or lifecycle.

A combined backend translation unit shall not make a declaration visible where
the GTI source graph does not.

## 3.5 Initialization And Conversion

Initialization requires an exact type unless a specific rule permits a
conversion. Numeric conversions use `Type(value)` and checked GTI semantics.
Constructors are selected by one exact parameter list and do not define implicit
conversions.

Value-assignment contexts, including initialization, assignment, and return,
accept an integer literal when its mathematical value fits the destination.
A non-literal integer may widen within the same signedness, and an unsigned
integer may widen to a strictly wider signed destination. An integer value is
also assignment-compatible with `float` or `double`, and `float` may widen to
`double`. `double` never implicitly narrows to `float`, including through a
compound assignment. These assignment compatibilities do not participate in
call or constructor overload selection.

The binary equality, relational, arithmetic, modulo, and bitwise operators add
one narrow operand context for integer literals. When exactly one operand is
an integer literal, optionally parenthesized and optionally preceded by one
unary `+` or `-`, and the other operand has a built-in integer type or a
type-parameter type whose constraints imply `std::integral`, the complete
literal expression adopts that exact type. Its mathematical value, including
the sign, must fit a concrete adopted type; generic declarations defer that
range check until concrete instance reanalysis. Shift counts are excluded
because their type is independent of the shifted value. Non-literal mixed
operands receive no such conversion and continue to require the operator's
exact type contract.

`float` is IEEE-754 binary32 and `double` is IEEE-754 binary64. An unsuffixed
floating literal has type `float`; `d` or `D` selects `double`. A built-in
numeric operation uses `double` if either operand is `double`, otherwise
`float` if either operand is `float`, and converts an integer operand into that
selected format. Conversion from either floating type to an integer requires
explicit `IntegerType(value)` syntax and the checked range/truncation rule in
[execution semantics](execution.md#43-numeric-execution). Explicit
`float(value)` and `double(value)` accept integers or either floating width;
converting to `float` is the required explicit narrowing operation.

`Type name{arguments};` directly constructs a declared class or struct. It is
not C++ aggregate initialization, list conversion, initializer-list preference,
copy-list initialization, or CTAD.

A union uses the same direct-brace spelling only for zero initialization or an
exact value for its first declared field. Selecting another active field is an
explicit write to a `mut` field through a mutable union value inside `unsafe`.

Fixed arrays require complete initialization. Empty braces value-initialize all
elements; a non-empty initializer supplies exactly one value per element.

Program-wide bindings initialize in the dependency/source order defined by
[Execution Section 4.2.4](execution.md#424-program-wide-initialization). A safe
initializer is well-formed only when the compiler proves that it cannot access,
directly or through a GTI call, a program-wide binding whose initialization
step has not completed. Declaration visibility does not provide an early value.
Substituting a frontend-computed `constexpr` value without forming or loading
its storage is not such an access.
The current compiler does not yet build that plan or call/access proof; its
acceptance of such an initializer is an implementation gap.

Execution-profile selection adds one declaration-wide process-storage rule.
In the concurrent profile, every namespace global and non-generic class static
field must be an immutable binding whose concrete resolved type is
share-capable. The rule applies equally through aliases, concrete generic
instances, internal linkage, raw-pointer or borrowed-state carriers, declared
cleanup, and nominal capability policy. The default single-threaded profile
continues to permit ordinary mutable globals. An immutable value with a
reviewed `[[unsafe_share]]` nominal assertion satisfies the capability rule;
the selection does not itself provide an atomic or mutex operation.

## 3.6 Calls And Overloads

An overload set is resolved to one unique candidate whose parameter types match
exactly after generic substitution, apart from the bounded raw-pointer
qualification and null compatibility defined above. Return types, parameter
names, and by-value parameter mutability do not distinguish overloads. GTI does
not otherwise perform conversion ranking, return-type overloading, ADL, or a
concrete-over-generic preference.

Receiver mutability may distinguish method overloads. A read-only receiver can
select only a read-only method; a mutable receiver prefers the otherwise exact
mutable overload when both exist. `operator()` additionally admits a consuming
overload with trailing `&&`. It is available only through an explicitly moved
receiver, and an otherwise exact consuming overload is preferred for that
form. Read-only, mutable, and consuming call-operator overloads may coexist.

A consuming call operator is non-virtual, has a body, and cannot implement an
interface contract. Its result cannot be a reference or contain tracked
borrowed state. Invocation consumes the exact source place under the ordinary
move-state rules; a later possible read, move, or
invocation is ill-formed. A confined generic call written as
`std::move(operation)()` requires at-most-once invocation and may select an
exact consuming target or a reusable read/mutable target. Reusable clients do
not accept a consuming-only target.

The consumed receiver must not structurally require active cleanup. This
includes cleanup inherited through a base, field, fixed array, owner/storage
component, concrete generic substitution, or callable capture. GTI does not
yet have a full-expression-owned receiver representation that can transfer and
clean up such state at the language-defined boundary, so both direct calls and
concrete generic once-call selections are rejected before HIR.

The selected callable, constructor, or operator identity is part of the
program's semantics and must not be re-selected by a backend.

## 3.7 Generics

Named generic arguments are inferred exactly from value arguments or supplied
explicitly. Generic classes and structs provide all arguments explicitly.

The current standard constraints describe frontend-owned primitive, lifecycle,
and exact structural capabilities. Namespace-scoped user concepts may declare
one or more type parameters and compose existing concepts with conjunction.
Each concept application supplies identifiers naming the enclosing concept's
type parameters and must match the referenced declaration's arity.

An inline constraint such as `std::numeric T` remains unary and
non-structural. A generic free function or non-polymorphic method may instead
use a trailing clause of the form
`requires Concept<Parameter...> && Concept<Parameter...>`. Clause arguments
must be visible non-pack generic type parameters. These requirements provide
exact facts while the symbolic body is checked and are substituted and
revalidated for every concrete call.

Operators and virtual, pure, overriding, or interface methods cannot carry a
trailing clause in the bounded first version. Operator availability and
polymorphic contract equivalence require separate representation rules.

Constraints affect candidate validity but do not rank overloads, distinguish
otherwise identical signatures, provide SFINAE, or select specializations.
Disjunction, negation, general requires-expressions, arbitrary expression
requirements, value constraints, constraint subsumption, forwarding
references, and unrestricted metaprogramming are not part of the implemented
language.

Class and struct type parameters may be followed by immutable `uint64_t` value
parameters. Their arguments and expression contexts are restricted by the
incorporated grammar. Concrete generic bodies are rechecked after substitution;
generic validity is not delegated to C++ template instantiation.

## 3.8 Classes, Interfaces, And Lifecycle

A class or struct has at most one state-bearing public base and may additionally
implement interfaces. Interfaces contain public behaviour contracts and no
instance state. Duplicate bases, cycles, diamonds, private inheritance, and
multiple state-bearing bases are ill-formed.

An interface method is written as a bodyless signature ending in `;`. The
enclosing `interface` makes that signature virtual and pure; spelling the C++
pure specifier `= 0;` inside an interface is redundant and ill-formed. A class
or struct continues to use `virtual`, `override`, and `= 0;` for its explicit
virtual roots, implementations, and pure methods.

```gti
interface Renderable {
  int render(int frame);
};

class Sprite : public Renderable {
public:
  int render(int frame) override { return frame; }
};
```

Virtual roots and overrides match parameter types, receiver access, operator
identity, and return type exactly. Abstract values cannot be directly
constructed. GTI does not permit object slicing. Polymorphic destruction is
derived by the compiler from lifecycle metadata.

Default, copy, and move construction; copy and move assignment; and destruction
are generated from structural field traits. Declaring an ordinary constructor
does not suppress otherwise valid generated lifecycle operations.

A class or struct may declare at most one public exact copy policy
`Type(Type&) = default|delete;` and one public exact move policy
`Type(Type&&) = default|delete;`. These declarations govern construction only;
copy and move assignment remain independently derived. `= default` preserves
the structural operation and is ill-formed when a base, field, stored borrow,
or cleanup policy makes that operation unavailable. `= delete` makes the
corresponding operation unavailable.

`T&&` is not a general GTI reference or forwarding-reference type. It is
permitted only in the exact move constructor policy. A custom copy or move
constructor body is ill-formed until field places, partial movement, and
partial initialization have complete language semantics.

`[[c_opaque]] struct Name;` is the only incomplete nominal type declaration.
It is nongeneric, baseless, has no body, layout, members, lifecycle, or
concurrency capability, and cannot be combined with `[[c_abi]]`. Its resolved
type may occur only behind one raw pointer, including in an admitted
`[[c_abi]]` record field or `extern "C"` signature. The pointer remains
nullable, non-owning, and subject to lexical unsafe rules. Direct values,
construction, inheritance, generic arguments, pointee layout queries, and
ordinary forward declarations are rejected with `GTI-S2065` before lowering.

### 3.8.1 Passive Unions And Payload Enums

`union Name { fields... };` declares untagged overlapping storage. A union is
nongeneric, complete, baseless, non-empty, and public. It contains only
instance fields and cannot declare attributes, access sections, methods,
constructors, a destructor, static fields, or field initializers. Fields may be
fixed-width scalars, raw pointers, integral scoped enums, concrete fixed arrays
of admitted fields, nested valid unions, or valid `[[c_abi]]` records. A
recursive by-value edge and every ownership-bearing or cleanup-bearing field
are ill-formed. The frontend computes the maximum field size and alignment and
rounds size to that alignment. `sizeof` and `alignof` expose those facts.

Every union member read or write requires an `unsafe` block because the
compiler does not track the active field. Ordinary mutability still applies: a
write also requires a mutable union place and a field declared with `mut`.
Union copies are passive byte-representation copies; unions do not acquire
hidden lifecycle behavior. `GTI-S2066` owns declaration-shape failures, while
the ordinary unsafe diagnostic owns safe member access.

An `enum class` with at least one parameterized alternative is a closed tagged
sum. It cannot declare an integral backing type or explicit enumerator values.
Every payload field is named, immutable, non-variadic, and currently limited to
primitive scalars, `std::string_view`, `nullptr_t`, one-level raw pointers, and
integral scoped enums. References, arrays, classes, nested payload enums, and
ownership-bearing values are rejected until move/borrow patterns and
variant-aware partial lifecycle state are implemented.

Payload alternatives use exact construction. A unit alternative is written
`Result::empty`; a payload alternative is written `Result::value(arguments)`.
A `switch` over a payload enum uses the same alternatives as patterns:

```gti
enum class Message {
  quit,
  move(int32_t x, int32_t y),
};

int32_t coordinate_sum(Message message) {
  switch (message) {
  case Message::quit:
    return 0;
  case Message::move(x, y):
    return x + y;
  }
}
```

Each binding is an immutable copied value scoped to its arm. Every alternative
must be handled exactly once unless a `default` arm is present. A complete
payload switch is exhaustive for return, ownership-state, loan, HIR, and MIR
control-flow analysis. `GTI-S2067` owns payload declaration, construction, and
pattern/exhaustiveness failures.

## 3.9 Operators And Contextual Conversion

The overloadable operator set and arity rules are defined by the incorporated
grammar. Operators are member-only and are selected through ordinary exact
semantic resolution. The backend does not perform GTI operator lookup.

Contextual `operator bool` participates only in the contexts enumerated by the
language contract. Logical `and`/`or` and `&&`/`||` are equivalent spellings
with identical short-circuit semantics.

## 3.10 Well-Formed Control Flow

Every reachable path through a non-`void` function, method, operator, or lambda
returns an appropriate value. The permitted top-level `main` definition may
reach its closing brace, which returns zero.

Every executable `switch` arm terminates explicitly as defined by the control
flow rules. GTI does not have implicit switch fallthrough. A payload-enum
switch without `default` covers every value only when all alternatives are
present; such a switch has no reachable unmatched path.

Every non-`void` call result is used unless intentionally suppressed with the
specified discard form.

## 3.11 Bounded Constant Evaluation

`sizeof(type)` and `alignof(type)` are type-only layout queries. Each
expression has exact type `uint64_t` and is evaluated by the frontend from the
selected target's GTI-owned data-layout facts. `alignof` returns ABI alignment,
not preferred alignment. A query does not evaluate a source expression, form
a borrow, access storage, or require `unsafe`. It is itself a scalar constant
expression and may participate in ordinary frontend constant evaluation, such
as a comparison used by an `if constexpr` condition.

After transparent alias resolution, the bounded operand set is:

- `bool`, `char`, `float`, `double`, `int`, `uint`, and every canonical or
  compatibility spelling of the fixed-width signed and unsigned integers;
- one-level `T*` or `const T*` raw pointers, including `void*` and pointers
  whose pointee does not itself have a queryable layout; and
- integral scoped enums, whose layout is the layout of their exact underlying
  fixed-width integer type;
- valid passive unions and concrete `[[c_abi]]` structs whose compiler-owned
  aggregate layout is available; and
- a fixed array whose element type is supported recursively and whose every
  extent is a concrete positive value.

For a fixed array, `sizeof(T[N])` is the checked product of `N` and
`sizeof(T)`, and `alignof(T[N])` is `alignof(T)`. The rule applies recursively
to multidimensional arrays. A zero or symbolic extent, or a product that does
not fit `uint64_t`, is ill-formed.

Bare `void`, `nullptr_t`, references, ordinary classes/structs, opaque-handle
pointees, interfaces, payload enums, `expected`, compiler-private types, and
symbolic type parameters have no source-queryable layout when queried
directly. Passive unions and `[[c_abi]]` records are the bounded nominal layout
opt-ins; integral scoped enums inherit their declared integer layout.
Unsupported complete operands are rejected as
`GTI-S2063` before HIR lowering; a direct opaque-handle operand retains its
focused incomplete-type `GTI-S2065`, and an unresolved name retains its
ordinary type-resolution diagnostic without a duplicate layout diagnostic.

The query itself is not an `array-extent-expression`. Its computed value may
still feed that restricted grammar through an earlier non-negative
`constexpr uint64_t` binding:

```gti
constexpr uint64_t word_bytes = sizeof(uint32_t);
uint8_t payload[word_bytes] = {};
```

A `constexpr` variable is immutable, has an initializer, and has a supported
scalar type. A `constexpr` class or struct field is also `static`. Its value is
computed by the GTI frontend and retained as typed semantic and HIR data; the
C++ backend is not an authority for whether an expression is constant.

The evaluator accepts fixed-width integer, `float`, `double`, `bool`, `char`,
`std::string_view`, and `nullptr_t` literals; layout-query constants; earlier
constexpr bindings; grouping; supported scalar unary, binary, comparison, and
short-circuit logical operations; lazy conditional expressions; and explicit
numeric conversions. Integer operations use the language's checked domains.
Floating literals, arithmetic, comparisons, integer conversions, signed zero,
infinities, and NaNs use the selected binary32 or binary64 rules in execution
semantics. Evaluation has a shared 4096-step budget and a 64-call-depth limit
and reports integer
overflow, integer zero divisors, invalid shifts, out-of-range conversions,
non-constant references, unsupported operations, and resource exhaustion at
source locations.

A non-generic free function or static method may be `constexpr` when its
parameters and return use those scalar domains. Its body may use scalar local
bindings, assignment and compound assignment, increment and decrement,
blocks, returns, ordinary and constexpr conditionals, loops, switch,
`break`/`continue`, target conditionals, recursion, and calls to available
constexpr definitions. Calling such a function at runtime remains an ordinary
GTI call.

`if constexpr (condition)` requires the frontend to compute one `bool`. Only
the selected branch is semantically analyzed and lowered; the discarded branch
cannot contribute diagnostics, symbols, calls, ownership effects, HIR, MIR, or
emitted C++. Native C++ constant evaluation is not used to choose the branch.

Concrete non-negative integer constexpr values may supply fixed-array extents
and `uint64_t` value-generic arguments. These uses refer to the declaration's
computed value rather than reinterpreting emitted C++.

Generic constexpr instantiation, instance methods and class values, operator or
polymorphic constexpr functions, references, allocation, arrays, and
runtime/intrinsic/C calls are rejected until they have compiler-owned
evaluation rules.

## 3.12 Static-Semantic Gaps

The following require later normative sections rather than inference from the
current implementation:

- the remaining callable, complete-range, heterogeneous accumulation, and hash
  capability families plus any general expression-requirement model;
- complete lifetime relationships for borrowed aggregate values;
- general place movement, partial initialization, and reinitialization;
- owned callable escape beyond exact same-type generic return, the bounded
  one-field generic owner, local closure movement, and explicit owned
  move-capture;
- generic and aggregate constexpr evaluation plus compile-time assertions;
- audited expansion beyond the bounded scalar, counted-text-input,
  `[[c_abi]]` record, pointer-only `[[c_opaque]]` handle, and one-level
  raw-pointer C call surface, including callbacks, out parameters, casts, and
  annotated ownership transfer; and
- native record field families beyond the closed scalar, nested-record, and
  one-level-pointer set, including fixed arrays, unions, packing, and
  bit-fields; record initialization policy remains in safe wrappers or native
  factory functions rather than in the representation declaration.
