# Concepts and bounded requirements

GTI concepts describe primitive, lifecycle, and exact structural properties
that generic type arguments must provide. They are a frontend validity
mechanism. They do not emit C++ concepts, rank overloads, or introduce a
separate compile-time metaprogramming language.

The source-defined `std::numeric<T>` contract includes bitwise relocation,
alongside copying, ordering and default construction. Its signed, integral and
floating refinements inherit that guarantee. Compiler-bound numeric, atomic
scalar and integral-enum capabilities also prove complete object layout and
absence of stored borrows for a dependent parameter, because every type they
admit has those properties. This permits numeric generic algorithms to use
ordinary source containers without restating their storage constraints.
Copyability or movement alone supplies neither structural guarantee; facts
available only in one disjunct do not escape that disjunction. Example 33
demonstrates the numeric-container composition.

The prelude's `std::integral_enum<T>` admits scoped enums with a fixed-width
integer representation and excludes payload enums. `std::enum_underlying<T,U>`
additionally requires `U` to be the exact declared representation, including
signedness and width. They do not grant numeric operators or implicit integer
conversion. The separate `std::to_underlying<U,T>(value)` function extracts that
exact integer at runtime. Concept applications are constraint syntax, not
boolean expressions; ordinary bounded member queries can inspect a constrained
member in a constant condition.

## Concept declarations

A namespace-scoped source concept declares one or more type parameters and
combines existing concepts and bounded member requirements with conjunction
and disjunction:

```gti
concept sortable<T> =
    std::totally_ordered<T> && std::movable<T>;

concept readable_as<Iterator, T> =
    std::input_iterator<Iterator> &&
    std::accumulates_into<Iterator, T>;

concept scalar<T> =
    std::integral<T> || std::floating_point<T>;
```

Every application argument is a visible non-pack type parameter, a closed exact
type, or a dependent compound type. An application must supply the referenced concept's
exact arity, but may map, reorder, or reuse enclosing parameters. A closed
argument such as `char` supplies an exact fact but does not participate in
generic inference. Its application remains an ordinary predicate branch, so a
false closed application may be bypassed by a satisfied `||` alternative.
Dependent compound arguments such as `Wrapper<T>` use ordinary type substitution
and exact specialization selection; they do not infer generic arguments.
Unexpanded packs, values, and expressions remain outside this grammar. Concept names
follow ordinary namespace and direct-source-unit visibility. They may be
declared after use within a visible source unit. Definitions may use
`&&`/`and` and `||`/`or`. Conjunction binds more tightly than disjunction, and
parentheses group a constraint expression. Cycles are rejected.

An inline generic-parameter constraint remains the compact unary form:

```gti
T lower<sortable T>(T left, T right) {
  if (left < right) { return left; }
  return right;
}
```

The named concept must have one type parameter. On function parameters it
must reduce to unary capabilities; structural function contracts use the
trailing clause below. Class and struct parameters also accept unary
structural concepts:

```gti
concept readable<T> = requires(T& source) { { source.read() } -> int; };
class Session<readable T> {
  T& source;
public:
  Session(T& source) : source(source) {}
  int read() { return this.source.read(); }
  ~Session() { int final_sample = this.source.read(); }
};
```

The contract's guaranteed operations apply throughout the class body,
including field initializers, constructors, methods and destructors. Unsafe
requirements still require a lexical `unsafe` block at each selected call.
Every concrete class argument must satisfy the exact concept even when no
member is called. A contract does not create a public member, change access,
or permit a destructor to call operations absent from the contract. Ordinary
constraint composition and disjunction retain only guaranteed common facts.
Relationships between two or more generic parameters use the trailing clause
below; this does not introduce class-level trailing `requires` syntax.

## Trailing `requires`

A generic free function, non-polymorphic method, or ordinary generic
constructor may append a bounded requirements clause after its parameter list
and receiver `mut` qualifier, when present. For a constructor the clause comes
before its initializer-list colon:

```gti
T consume<Iterator, Sentinel, T>(mut Iterator first, Sentinel last, mut T init)
  requires std::input_iterator<Iterator> &&
           std::sentinel_for<Sentinel, Iterator> &&
           std::accumulates_into<Iterator, T> {
  // body
}
```

The implemented clause has this bounded grammar:

```text
constraint-primary    = concept-application | member-requirement
                      | '(' constraint-expression ')'
constraint-and        = constraint-primary (('&&' | 'and') constraint-primary)*
constraint-expression = constraint-and (('||' | 'or') constraint-and)*
requires-clause       = 'requires' constraint-expression
```

Every argument must be a visible non-pack type parameter, closed exact type,
or dependent compound type,
and every application must have the declared arity. Unexpanded packs,
values, arbitrary expressions, general nested requires-expressions, boolean
predicates, and negation are not accepted in this clause.

The compiler forms all clause applications before validating dependent
compound arguments against the clause's guaranteed facts. For example,
`requires std::copyable<T> && boxed_int<Box<T>>` can admit `Box<T>` when the
class itself requires copyability. Reversing the conjuncts has the same
meaning. A fact supplied by only one disjunction branch cannot admit that
compound type; every concrete call still checks the selected clause.

At a concrete call, conjunction requires both operands and disjunction accepts
either complete operand. Facts are never mixed across alternatives. During
symbolic generic-body checking, conjunction contributes the union of its
facts, while disjunction contributes only facts guaranteed by every branch.
For example, `std::integral<T> || std::floating_point<T>` permits operations
proved by both numeric branches but does not prove integral-only remainder.
Callable capability implications are retained when computing the common fact:
read-only and mutable callable alternatives both prove the repeatable mutable
use site and the weaker once-use site. The original concept identity remains
part of concrete viability even when its alternatives have no common body
fact, so an empty intersection never accepts every type.

At a concrete call, the requirements are substituted and checked again
against the inferred or explicit types. During the symbolic check, the
value produced by a constrained dereference has no exact type unless a
`std::dereferences_to` requirement names one. An `auto` local may still bind
that value: each concrete generic instance owns the binding's exact inferred
type, while the shared source declaration remains instance-dependent. This is
valid even when the generic primary is never instantiated. Naming the element
type (`T value` under `std::dereferences_to<Iterator, T>`) remains necessary
when the type itself participates in a signature or constraint. A failed
requirement removes the
candidate from viability and produces a source-facing constraint diagnostic
when no call is viable. Requirements do not:

- distinguish otherwise identical overload declarations;
- rank viable overloads or prefer a more constrained candidate;
- provide SFINAE-style substitution recovery;
- select a specialization; or
- delegate validity to native C++ constraint evaluation.

The bounded first version does not permit a trailing clause on an operator or
on a virtual, pure, overriding, or interface method. Conditional operators
would require the backend protocol to encode the same condition, while a
polymorphic contract cannot be strengthened safely by an override. Both need a
separate design rather than silently weakening the declared requirement.

GTI therefore retains the exact unique-match rule from
[ADR 003](../decisions/003-exact-overload-resolution.md).

## Library contract diagnostics

An unsatisfied source-defined contract is a compile-time API error, not a
recoverable runtime result. Its diagnostic preserves the public concept's
concrete application, the nested requirement that failed, and its declaration
location. For example, an ordinary library can declare:

```gti
concept readable<T> = requires(T& value) { { value.read() } -> int; };
int inspect<T>(int label, T& value) requires readable<T> {
  return value.read();
}
```

When `inspect(1, item)` receives a type without `read()`, `GTI-S2029` names
argument 2 and `readable<Item>`, relates the requirement, and explains the
missing member. An existing inaccessible method, inexact result, or unsupported
receiver/parameter shape receives its corresponding explanation instead.
Concrete applications remain available through nested concepts, including
reordered parameters. The compiler follows resolved generic-parameter identity;
it does not guess the responsible argument by comparing equal concrete types.
When several arguments contribute the same parameter, a closed requirement
does not depend on an argument, or a disjunction has distinct causes, the
primary remains at the call rather than arbitrarily blaming one argument.
For an attributable argument, the primary highlights its representative source
token; it does not promise to underline an entire compound expression.
The compiler and LSP publish the same structured explanation and do not offer
an automatic edit that invents an implementation or changes ownership.

This facility is independent of any public library name. It does not by itself
reject a supported function's dynamic input at compile time. The source-defined
formatter protocol uses the exact member signatures below, including mutable
output parameters. Concrete comma-pack folds check their target's requirements
for every element before HIR publication. Literal-format validation separately
needs general call-site constant validation and must reuse the canonical
constant-evaluation direction, not an optimizer-dependent check or a second
format grammar embedded in the compiler.

## Exact member signature requirements

Library authors can describe a public, nongeneric instance method with
an exact owned result type:

```gti
concept snapshot_as<T, R> =
    requires (T& value) { { value.snapshot() } -> R; };

T save_snapshot<T>(T& value) requires snapshot_as<T, T> {
  return value.snapshot();
}
```

The receiver name is local to the requirement. The requirement inspects a
signature, not an expression body: it never calls the method, constructs a
receiver, acquires a loan, or changes runtime ownership. Missing, inaccessible,
static, consuming, ambiguous or result-mismatched methods do
not satisfy it. A method's own requirements must be satisfied. Cyclic member
requirements fail closed; nested member-requirement checking is bounded to 64
active checks. The result cannot be a reference or contain tracked
borrowed state. The method body remains subject to ordinary analysis when used;
signature satisfaction alone does not prove that every body instantiation is
valid.

Accessibility follows a call written where the requirement is spelled. A
compiler-private member, meaning any member of a `gti_internal` type or a
public member whose signature names one, satisfies a direct query or a trailing
`requires requires` leaf only in compiler-trusted source, which could call it
there. Trusted source can therefore probe a private helper type whose method is
constrained directly by a raw `@compiler_constraint` capability. A named concept
is a public predicate. Its member requirements never admit a compiler-private
member, even when trusted source declares or applies the concept, and the
failure reports that the member is compiler-private rather than nonpublic.

Each query admits one checked reference receiver and one nongeneric member.
An immutable `T&` receiver requires a read-only method; `mut T&` requires a
mutable method. Additional typed parameters describe the exact method signature,
including reference access. The call must use every declared parameter once in
declaration order; it cannot contain arbitrary expressions or transfers:

```gti
concept accumulates<C, V> =
    requires(mut C& target, V& input, mut V& previous) {
      { target.add(input, previous) } -> V;
    };
```

Value parameters require exactly that value type, `V&` requires a read-only
reference, and `mut V&` requires a mutable reference. There is no conversion,
default-argument omission or generic method deduction in signature matching.
Requirements neither borrow nor execute their synthetic arguments. Actual calls
still apply ordinary ownership, receiver access and lifetime rules, and each
concrete instance is reanalyzed before HIR/MIR. A mutable contract never grants
permission to call through a read-only receiver. Combine separate queries using
`&&` and `||`; only facts common to all alternatives enter a generic body.
Receiver, parameter and result types may contain dependent compound types.
Function inline generic-parameter constraints still exclude structural
concepts; use a trailing clause. Class parameters accept unary structural
annotations as described above. Borrowed-result contracts remain unsupported.

An ordinary requirement proves a safe call and rejects an unsafe member.
Prefix the compound signature with `unsafe` to describe a member whose caller
may have an additional obligation:

```gti
concept writes<T> = requires(mut T& target, int value) {
  unsafe { target.write(value) } -> void;
};
unsafe void forward<T>(mut T& target, int value) requires writes<T> {
  unsafe { target.write(value); }
}
```

The requirement's keyword is a signature fact, not an executing block. It
retains the unsafe-call obligation during symbolic generic analysis, including
forwarding to another constrained function. A safe implementation also
satisfies an unsafe requirement because it needs fewer caller guarantees;
using only the unsafe requirement still requires lexical `unsafe` in the body.
A safe requirement never accepts an unsafe implementation, even when queried
inside an unsafe block. Conjunction retains an independently proved safe fact;
disjunction retains only the weaker unsafe fact when either alternative has
that obligation. The actual concrete overload is rechecked and its declaration
effect is preserved through HIR/MIR. These rules use no library type or member
name. Example 117 exercises an unrelated sensor abstraction.

The same requirement is a boolean static query in expressions, including
`if constexpr` and `static_assert`. Unlike C++'s compound requirement syntax,
GTI's `-> R` names the **exact result type**, not a result constraint concept.
A successful conjunction supplies each member contract to symbolic body
analysis; a disjunction supplies only contracts shared by every alternative,
including their member names and exact receiver/result relationships.

`<std/utility>` defines `std::member_cloneable<T>` with this syntax and
`std::cloneable<T>` as member cloning or `std::copy_constructible<T>`.
`std::clone(value)` prefers the exact member operation and otherwise performs
ordinary copy construction. This is explicit duplication, not automatic deep
copying: strings duplicate their bytes, shared owners retain shared ownership,
and user types define their own cloning policy. Noncopyable types without a
compatible clone member remain rejected. `std::vector<T>::clone()` requires
`std::cloneable<T>` and duplicates each element through that helper, including
strings and nested vectors. `std::optional<T>::clone()` applies the same helper
only when engaged, preserving empty state otherwise. Both operations are
ordinary constrained GTI methods. No public clone name is recognized by the
compiler.

## Ownership boundary

The compiler must evaluate facts that current source concept composition cannot
itself express, such as whether a primitive is integral or whether an iterator
declaration exposes an exact dereference operation. It must not understand
public library names or library policy.

The boundary has three layers:

1. compiler-owned capability records contain irreducible semantic facts;
2. trusted declarations in `gti_internal` bind one fact or exact structural
   relationship by declaration identity; and
3. public `std` concepts and user concepts compose those declarations in GTI.

For example, `std::totally_ordered` is not a compiler enum value. The prelude
defines it as equality plus the irreducible relational-operator fact. The
iterator units likewise define public concepts over private structural facts:

```gti
namespace std {
concept copy_constructible<T> =
    gti_internal::copy_constructible_capability<T>;

concept input_iterator<I> = gti_internal::input_iterator_capability<I>;

concept sentinel_for<Sentinel, Iterator> =
    gti_internal::sentinel_for_capability<Sentinel, Iterator>;

concept dereferences_to<Iterator, T> =
    gti_internal::dereferences_to_capability<Iterator, T>;

concept forward_iterator<Iterator> =
    std::input_iterator<Iterator> &&
    std::copy_constructible<Iterator> &&
    std::sentinel_for<Iterator, Iterator>;
}
```

The current structural capability set is intentionally small:

- `copy_constructible<T>` requires an ordinary GTI copy construction to be
  available; unlike `copyable<T>`, it does not require copy assignment;
- `move_constructible<T>` requires ordinary GTI move construction, without
  requiring move assignment. `movable<T>` still requires both operations.
  Construction-only owners can therefore nest without acquiring replacement
  or borrow-reseating permission; the ordinary ownership checks still apply;
- `relocatable<T>` requires structurally movable, borrow-free state, excluding
  address-stable private host state; it does not require assignment;
- `bitwise_relocatable<T>` additionally requires passive scalar/array state or
  a class with only eligible fields, no bases, polymorphism, or declared
  cleanup. It implies `relocatable<T>`; backend representation checks may still
  select generated elementwise transfer. See [structural
  relocation](ownership-and-lifetimes.md#structural-relocation);
- `input_iterator<I>` requires public read-only `operator*()` returning a
  checked reference and public `void operator++() mut`; transferring an
  iterator into an algorithm is checked separately by ordinary parameter
  ownership rules;
- `sentinel_for<S, I>` requires an exact public read-only
  `bool I::operator!=(S&)`; and
- `dereferences_to<I, T>` requires public read-only `I::operator*()` to return
  a checked reference whose referent is exactly `T`;
- `writable_iterator<I>` requires public `mut I::operator*() mut` returning a
  mutable checked reference plus public `void operator++() mut`; the public
  spelling is `std::output_iterator<I>`;
- `writes_to<I, T>` refines the mutable dereference referent to exactly `T`;
- `bidirectional_iterator<I>` requires public `void operator--() mut`;
- `random_access_iterator<I>` requires public
  `void operator+=(std::ptrdiff_t) mut` and
  `void operator-=(std::ptrdiff_t) mut`;
- `subscripts_to_mut<I, T>` requires public
  `mut T& I::operator[](std::ptrdiff_t) mut` with exactly `T`;
- `sized_sentinel_for<S, I>` requires public read-only
  `std::ptrdiff_t I::operator-(S&)`, the signed distance from the iterator to
  the sentinel;
- `input_range<R>` derives exact public read-only `begin()`/`end()` endpoints
  and requires their iterator/sentinel pair to satisfy the input protocol;
- `range_of<R, T>` refines that range to an exact read-only `T&` dereference,
  while `mutable_range_of<R, T>` requires exact mutable `begin_mut()` and
  `mut T&` dereference;
- `sized_range<R>` requires exact public read-only
  `std::size_t R::size()`; fixed arrays satisfy it from their exact extent;
- `forward_iterator<I>` is the source-defined conjunction of input iteration,
  copy construction, and use of `I` as its own sentinel; the public
  bidirectional and random-access concepts compose their movement facts over
  it, so only copyable read cursors satisfy them; and
- `accumulates_into<I, T>` is the numeric-library composition of the same
  exact dereference relationship;
- `adds<T>`, `subtracts<T>`, and `negates<T>` each require one exact public
  read-only member — `T operator+(T&)`, `T operator-(T&)`, and unary
  `T operator-()` — returning an *owned* value of the domain type itself; and
- `scales_by<T, Scalar>` requires public read-only `T operator*(Scalar)`
  returning an owned domain value for that exact scalar type;
- `read_callable<F, A, R>`, `mut_callable<F, A, R>`, and
  `once_callable<F, A, R>` require one exact public unary `operator()` whose
  result is the owned, non-borrowed `R` and whose readable argument is either
  `A` or `A&`. The receiver must respectively be read-only, repeatable
  read-only/mutable, or usable once through owned access;
- the corresponding `read_binary_callable`, `mut_binary_callable`, and
  `once_binary_callable` relationships name two exact readable arguments;
- `once_action<F>` is the nullary exact-`void` consuming-use relation;
- `indirect_predicate<F, I>` derives the exact referent of `I::operator*()` and
  requires a repeatable `bool` callable for it; and
- `indirect_operation<F, I, R>` derives the same referent and requires a
  repeatable exact owned `R` operation; and
- `range_predicate<F, R>` and `range_operation<F, R, Result>` derive the exact
  read-only element of a complete range and apply the same repeatable callable
  rules without exposing an associated iterator type.

In a generic body the writable capability supplies the mutable-dereference
form of `*output = value`, and `subscripts_to_mut` supplies
`cursor[offset] = value`; a mutable receiver prefers the mutable structural
form when the active requirements prove both a read-only and a mutable one,
mirroring concrete member selection.

Complete-range requirements additionally supply private symbolic
iterator/sentinel carriers for a range-based `for`, plus the exact zero-argument
`size()` result under `sized_range`. These carriers are compiler proof objects,
not source-visible associated types. Concrete generic reanalysis must replace
them with the selected nominal endpoint declarations or fixed-array strategy
before HIR and verified MIR are built.

The four domain-value capabilities are spelled publicly as `std::addable`,
`std::subtractable`, `std::negatable`, and `std::scalable_by`, and compose into
`std::vector_space<T, Scalar>` together with `std::copyable<T>`.

The callable facts are exposed from `<std/functional>`. Public
`std::predicate`, `std::binary_predicate`, `std::operation`,
`std::binary_operation`, and `std::comparator` are ordinary source concepts
over the repeatable forms; `std::once_action` exposes the nullary consuming
form. Read-only targets satisfy repeatable and once-use sites, mutable targets
satisfy repeatable and once-use sites, and `operator() &&` satisfies only a
once-use site. This implication is capability inclusion, not overload ranking:
each relation still requires one exact candidate after receiver capability is
selected.

An exact callable argument describes a readable source value. A concrete
target may copy it by value or observe it through an exact read-only reference.
Library implementations therefore present readable lvalues at these sites;
they do not use native reference binding or implicit conversion to widen the
contract. Results may not be references, lambdas, or values carrying tracked
borrowed state because the bounded relation has no result-owner parameter.

A borrowed operator result does not satisfy these capabilities: the produced
value would be tied to a receiver whose lifetime the requirement cannot name.
The operand may be declared by read-only reference or by value, which lets a
domain type take a peer by reference and a scalar by value without either
choice becoming part of the contract.

This family is deliberately **member-only**, matching the language's
member-only user-operator rule. A built-in numeric type therefore does not
satisfy it — built-in arithmetic keeps its own checked semantics and its
`std::numeric` contract, and admitting it here would resolve its operators as
symbolic member calls and drop the canonical local failure outcomes that
checked integer arithmetic must retain. Library functions that want to serve
both worlds provide an exact built-in overload alongside the generic form, as
`std::lerp` does.

Copy construction is deliberately a separate lifecycle fact. A read-only
owner-tied cursor may be copied into another independently positioned cursor
while both values retain the same owner dependency. It is not thereby
copy-assignable: replacing a live borrow-carrying carrier would require a
separate reseating and invalidation design. Mutable owner-tied cursors remain
non-copyable and therefore do not satisfy `std::forward_iterator`.

The exact-referent rule is a deliberate first version. It supports homogeneous numeric
accumulation but not an iterator element type that merely converts to, or can
otherwise be added into, a different accumulator type. A later heterogeneous
contract requires a separately specified exact operation relationship; native
C++ conversion or expression probing must not fill that gap implicitly.

## Trusted bindings

The prelude and selected canonical standard-library units use reserved syntax
to connect an internal declaration to one compiler fact. For example,
`<std/iterator>` owns its exact sentinel relationship:

```gti
namespace gti_internal {
@compiler_constraint("sentinel_for")
concept sentinel_for_capability<S, I>;
}
```

`@compiler_constraint` is accepted only inside `gti_internal` in the implicit
prelude or in the exact trusted unit assigned to that binding. Atomic scalar
facts belong to `<std/atomic>`, iterator relationships to `<std/iterator>`, and
callable relationships to `<std/functional>`; core type and operator facts
remain in the prelude. Each unit composes its private declarations into public
concepts. Application source cannot forge a binding, name the private
capability, or expose it through a public signature. Binding names and bound
declarations are compiler/standard-library integration details, not a public
language API.

## Frontend and backend contract

Semantic analysis resolves each concept by declaration identity. Unary facts
remain compact capability sets; structural facts retain their ordered type-
parameter relationships. During symbolic body checking, an active requirement
can prove only the exact operator shapes represented by those facts. Concrete
generic reanalysis substitutes the actual types, validates their declarations,
and records the selected operator function identities used by HIR.

Concept declarations and trailing clauses emit no C++. HIR and MIR receive
only validated concrete instances and resolved operations. The C++ backend may
use templates as a transitional representation, but C++ lookup, concepts,
SFINAE, implicit conversion, and overload ranking do not decide GTI validity.

## Standard-library client

`<std/iterator>` and `<std/numeric>` use the bounded model to expose
`std::dereferences_to` and implement `std::accumulate` in ordinary GTI source:

```gti
T accumulate<Iterator, Sentinel, std::numeric T>(
    mut Iterator first, Sentinel last, mut T init)
  requires std::input_iterator<Iterator> &&
           std::sentinel_for<Sentinel, Iterator> &&
           std::accumulates_into<Iterator, T> {
  for (; first != last; ++first) {
    init = T(std::move(init) + *first);
  }

  return init;
}
```

The explicit `T(...)` preserves GTI's checked numeric-conversion and
assignment rules for narrow integer types. The compiler recognizes none of the
public names `std::accumulate`, `std::input_iterator`, `std::sentinel_for`, or
`std::accumulates_into`.

`<std/string>` uses the closed exact argument in
`std::dereferences_to<Iterator, char>` for its constructor-local generic
iterator/sentinel constructor. `std::string(source.begin(), source.end())`
copies characters into independent owning storage and retains neither the
iterators nor the source loan after construction.

`<std/algorithm>` supplies the first multi-pass client,
`std::binary_search`. Its iterator must satisfy `std::forward_iterator`, its
possibly distinct end value must satisfy `std::sentinel_for`, and dereference
must produce the exact ordered value type. The implementation copies a cursor
and advances that copy to each midpoint; it neither subtracts iterators nor
mutates the end sentinel. This yields logarithmic comparisons and linear total
iterator increments for forward iterators.

The input-iterator client cluster also includes ordinary source-defined
`std::find`, `std::count`, and `std::for_each` overloads. `find` and `count`
use the same exact relationships:

```gti
Iterator find<Iterator, Sentinel, T>(mut Iterator first, Sentinel last,
                                     T& value)
  requires std::input_iterator<Iterator> &&
           std::sentinel_for<Sentinel, Iterator> &&
           std::dereferences_to<Iterator, T> &&
           std::equality_comparable<T>;
```

The `dereferences_to` and equality requirements do not admit a convertible
element or a backend-selected comparison. A distinct sentinel remains an
independent exact type. Returning `find`'s iterator is an ordinary move of
`first`: when every reachable return moves that same parameter and its exact
concrete carrier type holds borrowed state, that parameter is the return's
owner dependency. This rule does not select an owner when different return
paths move unrelated parameters; the ordinary ambiguous-dependency diagnostic
still applies.

`for_each` states that contract publicly:

```gti
Function for_each<Iterator, Sentinel, Function>(mut Iterator first,
                                                Sentinel last,
                                                mut Function function)
  requires std::input_iterator<Iterator> &&
           std::sentinel_for<Sentinel, Iterator> &&
           std::indirect_operation<Function, Iterator, void>;
```

The indirect relation derives the checked-reference referent selected for the
concrete iterator and rejects an inexact argument, non-`void` result, private
target, ambiguity, or consuming-only target during call viability. Concrete
body reanalysis then records the same exact invocation and selected
read/mutable receiver in HIR and MIR. The contract also carries one independent
exact moved-return proof, so the callable may be invoked repeatedly and then
returned without changing the confined boundary. Callers must transfer a named
callable with `std::move`, and its stored state must not contain references,
tracked borrows, or raw pointers. Public algorithm names and emitted C++
overloads grant no capability.

`std::invoke_once` is the consuming counterpart:

```gti
void invoke_once<Callable>(Callable callable)
  requires std::once_action<Callable> {
  std::move(callable)();
}
```

It accepts an exact reusable `void()` target or `operator() &&`, consumes the
local once, and retains that choice through concrete HIR/MIR. It does not
create escaping callable storage.

## Deliberate exclusions and extension rule

The implemented language does not provide general C++ requires-expressions,
arbitrary expression requirements, negation, associated types, value
constraints, concept or partial specialization, constraint subsumption,
constraint-based overload ordering, conditionally available operators,
constrained polymorphic contracts, or unrestricted compile-time reflection.
Variadic callable relations, generic `operator()` requirements, and function
items or function pointers in public callable relations are also outside the
current bounded surface.

Add a new source concept when existing facts can express the contract. Add a
new private compiler capability only when semantic analysis must answer a
genuinely new irreducible question for a demonstrated language or library
client. Do not add a capability for a public class, algorithm, container state,
naming convention, or convenience alias.
