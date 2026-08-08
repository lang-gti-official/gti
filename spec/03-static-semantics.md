# 3. Static Semantics

Status: Partial normative draft

The detailed current contract is incorporated from the
[`GTI Language Contract`](../.agents/skills/gti-language/references/language-contract.md).
This chapter records its major semantic categories and the intended shape of
the completed normative text.

## 3.1 Type Categories

The implemented language includes:

- primitive integers, `float`, `bool`, and `char`;
- `void` in permitted return and `expected` positions;
- `nullptr_t`;
- scoped nominal enumeration types;
- class, struct, and interface types;
- fixed arrays whose extents participate in type identity;
- non-null read-only and mutable reference types;
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
rules in [Ownership And Lifetimes](05-ownership-and-lifetimes.md).

Local `auto` infers one exact complete value type from its initializer. It does
not infer globals, fields, parameters, returns, arrays, or untyped braced
initializers. The range-for forms `auto&` and `mut auto&` infer element borrows;
plain `auto` does not silently infer a reference.

## 3.3 Names, Scopes, And Visibility

Namespaces, namespace aliases, and qualified `::` names use lexical declaration
and source-unit visibility. Namespace-scoped `using Name = Type;` aliases are
declaration-order independent after cycle validation.

Class members follow their declared access and the default associated with
`class` or `struct`. `interface` members are public contracts. Namespace
`static` declarations have source-unit internal linkage. Type-owned static
members do not participate in instance layout or lifecycle.

A combined backend translation unit shall not make a declaration visible where
the GTI source graph does not.

## 3.4 Initialization And Conversion

Initialization requires an exact type unless a specific rule permits a
conversion. Numeric conversions use `Type(value)` and checked GTI semantics.
Constructors are selected by one exact parameter list and do not define implicit
conversions.

`Type name{arguments};` directly constructs a declared class or struct. It is
not C++ aggregate initialization, list conversion, initializer-list preference,
copy-list initialization, or CTAD.

Fixed arrays require complete initialization. Empty braces value-initialize all
elements; a non-empty initializer supplies exactly one value per element.

## 3.5 Calls And Overloads

An overload set is resolved to one unique candidate whose parameter types match
exactly after generic substitution. Return types, parameter names, and by-value
parameter mutability do not distinguish overloads. GTI does not perform
conversion ranking, return-type overloading, ADL, or a concrete-over-generic
preference.

Receiver mutability may distinguish method overloads. A read-only receiver can
select only a read-only method; a mutable receiver prefers the otherwise exact
mutable overload when both exist.

The selected callable, constructor, or operator identity is part of the
program's semantics and must not be re-selected by a backend.

## 3.6 Generics

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

## 3.7 Classes, Interfaces, And Lifecycle

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

## 3.8 Operators And Contextual Conversion

The overloadable operator set and arity rules are defined by the incorporated
grammar. Operators are member-only and are selected through ordinary exact
semantic resolution. The backend does not perform GTI operator lookup.

Contextual `operator bool` participates only in the contexts enumerated by the
language contract. Logical `and`/`or` and `&&`/`||` are equivalent spellings
with identical short-circuit semantics.

## 3.9 Well-Formed Control Flow

Every reachable path through a non-`void` function, method, operator, or lambda
returns an appropriate value. The permitted top-level `main` definition may
reach its closing brace, which returns zero.

Every executable `switch` arm terminates explicitly as defined by the control
flow rules. GTI does not have implicit switch fallthrough.

Every non-`void` call result is used unless intentionally suppressed with the
specified discard form.

## 3.10 Static-Semantic Gaps

The following require later normative sections rather than inference from the
current implementation:

- general user-defined capabilities and their relationship to interfaces;
- complete lifetime relationships for borrowed aggregate values;
- general place movement, partial initialization, and reinitialization;
- escaping callable types and captures;
- bounded constant evaluation and compile-time assertions;
- an audited native FFI and unsafe type surface; and
- layout guarantees required for native interoperation.
