# 3. Static Semantics

Status: Current high-level semantic contract with explicit specification gaps.

This chapter records the major semantic categories. Focused documents in this
directory provide detailed ownership, pointer, generic, range, interop, and
library rules; source and tests decide current implementation when this draft
has drifted.

## 3.1 Type Categories

The implemented language includes:

- primitive integers, `float`, `bool`, and `char`;
- `void` in permitted return and `expected` positions;
- `nullptr_t`;
- scoped nominal enumeration types;
- class, struct, and interface types;
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
declaration has a trailing `mut` qualifier.

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

Call and construction arguments are also checked as one evaluation boundary.
If one argument produces a transient borrow and another may mutate an
overlapping place, the call is ill-formed in either written order until the
backend provides an explicitly ordered argument lowering.

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

`Type name{arguments};` directly constructs a declared class or struct. It is
not C++ aggregate initialization, list conversion, initializer-list preference,
copy-list initialization, or CTAD.

Fixed arrays require complete initialization. Empty braces value-initialize all
elements; a non-empty initializer supplies exactly one value per element.

## 3.6 Calls And Overloads

An overload set is resolved to one unique candidate whose parameter types match
exactly after generic substitution, apart from the bounded raw-pointer
qualification and null compatibility defined above. Return types, parameter
names, and by-value parameter mutability do not distinguish overloads. GTI does
not otherwise perform conversion ranking, return-type overloading, ADL, or a
concrete-over-generic preference.

Receiver mutability may distinguish method overloads. A read-only receiver can
select only a read-only method; a mutable receiver prefers the otherwise exact
mutable overload when both exist.

The selected callable, constructor, or operator identity is part of the
program's semantics and must not be re-selected by a backend.

## 3.7 Generics

Named generic arguments are inferred exactly from value arguments or supplied
explicitly. Generic classes and structs provide all arguments explicitly.

The current standard constraints describe frontend-owned primitive numeric
capabilities. Constraints validate arguments but do not rank overloads or
distinguish otherwise identical signatures. User-defined concepts,
specialization, `requires`, forwarding references, and unrestricted
metaprogramming are not part of the implemented language.

Class and struct type parameters may be followed by immutable `uint64_t` value
parameters. Their arguments and expression contexts are restricted by the
incorporated grammar. Concrete generic bodies are rechecked after substitution;
generic validity is not delegated to C++ template instantiation.

## 3.8 Classes, Interfaces, And Lifecycle

A class or struct has at most one state-bearing public base and may additionally
implement interfaces. Interfaces contain public behaviour contracts and no
instance state. Duplicate bases, cycles, diamonds, private inheritance, and
multiple state-bearing bases are ill-formed.

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
flow rules. GTI does not have implicit switch fallthrough.

Every non-`void` call result is used unless intentionally suppressed with the
specified discard form.

## 3.11 Bounded Constant Evaluation

A `constexpr` variable is immutable, has an initializer, and has a supported
scalar type. A `constexpr` class or struct field is also `static`. Its value is
computed by the GTI frontend and retained as typed semantic and HIR data; the
C++ backend is not an authority for whether an expression is constant.

The evaluator accepts fixed-width integer, `bool`, `char`,
`std::string_view`, and `nullptr_t` literals; earlier constexpr bindings;
grouping; supported scalar unary, binary, comparison, and short-circuit
logical operations; lazy conditional expressions; and explicit integer
conversions. Integer operations use the language's checked domains. Evaluation
has a shared 4096-step budget and a 64-call-depth limit and reports overflow,
zero divisors, invalid shifts, out-of-range conversions, non-constant
references, unsupported operations, and resource exhaustion at source
locations.

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
polymorphic constexpr functions, references, floating-point evaluation,
allocation, arrays, and runtime/intrinsic/C calls are rejected until they have
compiler-owned evaluation rules.

## 3.12 Static-Semantic Gaps

The following require later normative sections rather than inference from the
current implementation:

- general user-defined capabilities and their relationship to interfaces;
- complete lifetime relationships for borrowed aggregate values;
- general place movement, partial initialization, and reinitialization;
- escaping callable types and captures;
- generic and aggregate constexpr evaluation plus compile-time assertions;
- audited expansion beyond the bounded scalar, counted-text-input, and
  one-level raw-pointer C call surface, including native records, callbacks,
  casts, and ownership transfer; and
- layout guarantees for native records other than the explicit
  `gti_c_string_view` input record.
