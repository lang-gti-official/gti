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
combines existing concepts, unevaluated expression requirements and constexpr
boolean-call predicates with conjunction and disjunction:

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
Unexpanded packs remain outside concept-application arguments. A constexpr
boolean call can additionally test reflected type facts, as described below. Concept names
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

The named concept must have one type parameter. Functions, constructors,
classes and structs accept source-defined unary concepts, including structural
queries and predicates. The full concept contract is preserved, not just its
classification facts:

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
and receiver `mut` or `&&` qualifier, when present. For a constructor the clause comes
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
constraint-primary    = concept-application | expression-requirement
                      | constexpr-boolean-call
                      | '(' constraint-expression ')'
constraint-and        = constraint-primary (('&&' | 'and') constraint-primary)*
constraint-expression = constraint-and (('||' | 'or') constraint-and)*
requires-clause       = 'requires' constraint-expression
```

Every argument must be a visible non-pack type parameter, closed exact type,
or dependent compound type,
and every application must have the declared arity. Predicate leaves use an
ordinary explicitly type-argumentized call such as
`std::meta::is_integral<T>()`; they must evaluate to exact `bool` in the existing
constant evaluator. General Boolean expressions belong inside such a constexpr
function, not directly in the clause. Unexpanded packs and constraint-level
negation are not accepted. These additions do not introduce overload ranking.

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

A concept's author can state what satisfies it with
`[[unsatisfied("...")]]` before the declaration:

```gti
[[unsatisfied("A readable type has 'int read()'.")]]
concept readable<T> = requires(T& value) { { value.read() } -> int; };
```

Every unsatisfied requirement on `readable` then shows that text as the first
`help:` line of its `GTI-S2029`. The text adds to the structured explanation
above; the failed requirement, concrete application and related locations
remain. For a concept with a body, it replaces the generic hint about
`for concept` blocks, which say how a type satisfies the concept rather than
what a caller must provide. When concepts nest, the concept that the failed
requirement names explains it. Concepts it is defined through explain the
failure only when it has no text of its own. A `///` documentation comment
never changes a diagnostic.

The text is one nonempty string literal and the declaration's only attribute.
Compiler-bound concepts take none. Hover shows it with the declaration.

This facility is independent of any public library name. It does not by itself
reject a supported function's dynamic input at compile time. The source-defined
formatter protocol uses the exact member signatures below, including mutable
output parameters. Concrete comma-pack folds check their target's requirements
for every element before HIR publication. Literal-format validation separately
needs general call-site constant validation and must reuse the canonical
constant-evaluation direction, not an optimizer-dependent check or a second
format grammar embedded in the compiler.

## Unevaluated expression requirements

A requirement can check an ordinary expression without executing it or changing
the surrounding ownership state:

```gti
concept adds_to<L, R, V> = requires(L& left, R& right) {
  { left + right } -> V;
};
concept builds<T, A> = requires(A& argument) {
  { T(argument) } -> T;
};
concept defaults<T> = requires { { T() } -> T; };
```

The parameter list is optional. Each binding has an exact type; `mut` is
permitted only on a reference parameter. A block contains one or more checks,
all of which must hold. An unannotated `-> R` names an exact owned, non-borrowed
result type, including `void`; it is not a result concept or an implicit
conversion. A checked reference result instead names its borrowed input:

```gti
concept readable<C, T> = requires(C& owner) {
  { *owner } -> T& [[borrowed_from(owner)]];
};
concept editable<C, T> = requires(mut C& owner) {
  { owner.edit() } -> mut T& [[borrowed_from(owner)]];
};
```

The result's referent and access must match exactly, and every possible checked
origin must belong to the named synthetic parameter. That parameter must itself
be borrowed; a mutable result additionally needs a mutable input. Merely
returning the correct reference type from a different input does not satisfy
the contract. The annotation cannot authorize a temporary to escape, make an
owned local a borrowed input, or turn read-only access into mutable access.
Omitting the origin does not admit reference results. This slice describes
checked references, not conditional reference-bearing wrapper results.

A simple requirement omits the result contract: `{ expression };` checks
whether the expression is valid but promises no particular result type,
ownership category or borrowed origin. In an open generic body, a direct
operation has a deferred result, not an invented owned value or reference;
concrete instantiation must resolve it before executable lowering.

An inferred reference requirement instead spells
`{ expression } -> auto& [[borrowed_from(input)]];` (or `mut auto&`). It checks
the same exact access and input-origin rules as a typed borrowed requirement,
while leaving the referent inferred. A value result cannot satisfy this check,
and inference never changes a read-only input into a mutable borrowed origin.
Use a simple requirement, not `-> auto`, when no result contract is wanted.

The same query is a Boolean expression usable in `static_assert` and
`if constexpr`. Example [173](../../examples/173-general-requires.gti) demonstrates
operators, construction, composition and reflection on an unrelated user type.

An iteration requirement checks an ordinary range loop without executing it:

```gti
concept readable_range<R> = requires(R& range) {
  for (auto& element : range) {}
};
concept writable_int_range<R> = requires(mut R& range) {
  for (mut int& element : range) {}
};
```

Its body must be empty. `auto` infers the element type; an explicit element
type must match exactly, rather than becoming valid through a conversion.
The check uses the same range analysis as an executable loop, including fixed
arrays, endpoint selection, temporary lifetime, access and cleanup obligations.
It neither consumes the caller's range nor publishes an element loan. Direct
borrowed-parameter range queries preserve provisional iteration facts in an
open generic body; concrete instantiation rechecks the actual loop. This is a
general source-language query, not a binding to a standard-library concept.
An explicitly `unsafe for` query is currently a concrete filter only: it does
not grant a safe generic body permission to perform unsafe iteration operations.

Semantic analysis checks general expressions using ordinary GTI resolution,
access, ownership, mutability and lifetime rules in an isolated unevaluated
context. Missing or ambiguous operations, inaccessible constructors, invalid
transfers, or an incorrect result make the query false. It does not instantiate
a runtime receiver, execute a function or publish moves, loans or writes into
the caller. An owned synthetic parameter can be consumed; a checked reference
does not gain permission to move its referent. Prefixing a check with `unsafe`
permits unsafe operations in that query and retains the unsafe obligation when
the resulting operation is used in a generic body. An enclosing unsafe block
does not silently authorize an ordinary requirement.

A requirement expression supplied as a typed-macro argument keeps the caller's
lexical lookup domain. Synthetic query parameters are local to the query; they
do not expose intervening macro-local variables or leak into a separately
analysed function or lambda scope.
The current typed-macro body profile does not yet admit a directly written
requirement expression; a caller may supply the query as a Boolean argument.

For an open generic body, direct bound-operand operator/call requirements with
a reference receiver and direct `T(arguments...)` construction requirements
supply exact provisional
operation facts. These facts do not create executable call targets: concrete
instances resolve and validate ordinary operations again before HIR/MIR.
For a direct member call, dereference or subscript, the provisional fact retains
the named receiver/argument origin and reference access. Composition and
disjunction cannot erase that lifetime contract or substitute another input.
Owned receiver queries are concrete filters until an exact consuming-access
proof is available; they never manufacture a borrowed receiver capability.
Compound expressions may filter concrete types, but do not manufacture proofs
for arbitrary subexpressions in an open generic body. Conjunction unions
guaranteed operation facts; disjunction retains only common operation facts.
Constraint satisfaction also preserves the alternatives themselves: a source
concept or transparent predicate wrapper can prove an equivalent disjunction
regardless of alternative order. Every possible provided branch must imply
the required alternative; one successful branch is insufficient. This bounded
implication check does not execute or infer arbitrary predicate bodies. Recursive query
checking is bounded to 64 active checks.

### Constexpr reflection predicates

```gti
concept integer_value<T> = std::meta::is_integral<T>();
concept plain_integer<T> =
    integer_value<T> && std::meta::is_bitwise_relocatable<T>();
concept same_representation<E, U> = std::meta::is_enum_underlying<E, U>();
```

Predicates use the existing constexpr execution rules and limits. A nonconstant,
non-Boolean, unsafe or false call cannot satisfy a constraint. An arbitrary
constexpr function is a concrete validity filter, not a promise that the
compiler can deduce every consequence of its implementation. Direct reflected
facts, and unambiguous safe constexpr wrappers that directly forward such a
fact, additionally carry that fact into symbolic body checking. Public function
names are not special: the fact originates at a trusted declaration identity.
Lexical lookup belongs to the predicate declaration rather than the caller's
generic bindings. No separate constexpr interpreter or native template probing
is involved.

Alternatives retain common consequences as well as identical predicates. For
example, `is_signed_integral<T>() || is_unsigned_integral<T>()` proves that `T`
is integral; it does not prove either particular signedness. Likewise,
`is_bool<T>() || is_integral<T>()` supplies the scalar-domain evidence needed
by atomic storage. Adding a floating-point alternative removes that evidence.
These implications depend on compiler-verified type categories, not concept or
wrapper names.

`std::meta::has_object_layout<T>()` answers whether a complete object
representation is available to the general object-lifetime operations.
`std::meta::is_borrow_free<T>()` answers whether a value retains no checked
external-owner borrow according to the type's structural ownership contract.
They are separate facts: a class containing a checked reference can have a
layout while retaining a borrow. The query is not a per-value lifetime check;
all ordinary provenance and escape checks still apply to view values. A raw
pointer can be borrow-free without being valid, owned,
or safe to dereference. Neither predicate relaxes unsafe obligations. Both
queries also have methods on `std::meta::type<T>`.

`std::meta::has_active_cleanup<T>()` reports whether destruction has active
work, including source destructor bodies and recursively owned state. A false
answer does not permit skipping object lifetime transitions.
`std::meta::is_polymorphic<T>()` reports resolved virtual dispatch, and
`std::meta::is_public_base_of<Base, Derived>()` reports the public base relation
used by ordinary GTI conversion. The latter is false for identical types and
non-class types. The corresponding type-metaobject methods are
`has_active_cleanup()`, `is_polymorphic()` and `has_public_base<Base>()`.
These are metadata queries, not casts or permissions to access an object.

`std::inline_polymorphic_payload<Model, Interface>` now composes these facts,
object layout, movable/relocatable, borrow-free and concurrency facts in source.
The trusted storage operation still independently validates its concrete
lifetime and erasure invariants; removing a private concept does not remove
those checks or make the storage operation a public escape hatch.

### Exact member signature compatibility

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

The existing exact member-signature shape admits one checked reference receiver
and one nongeneric member. It remains a compatible strict subset:
An immutable `T&` receiver requires a read-only method; `mut T&` requires a
mutable method. Additional typed parameters describe the exact method signature,
including reference access. The call must use every declared parameter once in
declaration order:

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
Inside a generic body, a dependent class receiver such as `Box<T>` proves a
requirement from its declared members after ordinary substitution, with the
same exact matching, even when the required result or parameters name the
enclosing parameters. A bare type parameter proves one only through an
enclosing requirement.
Unary source concepts may annotate function, constructor and class generic
parameters inline, including composed expression, signature and predicate
requirements. Their full contracts enter the generic scope and are checked
again for concrete arguments; they are not reduced to unary classification
bits. Relational concepts and protocol concepts with bodies still use a
trailing clause. Borrowed-result contracts remain unsupported.

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

The compiler must establish facts that source cannot safely derive, including
primitive classification, structural copying/movement/relocation, concurrency
eligibility and exact enum representation. Source composes public reflection
queries for these facts; it does not implement layout or ownership truth by
trying arbitrary operations. The compiler must not understand public library
names or library policy.

The boundary has three layers:

1. compiler-owned capability records contain irreducible semantic facts;
2. trusted declarations in `gti_internal` bind one fact or exact structural
   relationship by declaration identity; and
3. public `std::meta` predicates expose general facts, and `std` concepts and
   user concepts compose those predicates or expression requirements in GTI.

The prelude now defines numeric, enum, lifecycle and transfer/share concepts
through reflection rather than duplicate private concept declarations.
Their obsolete declaration-binding labels have been removed; the underlying
compiler fact kinds remain the authority for reflection and proof checking.
The exact enum-underlying relation likewise uses reflection rather than a
separate private concept binding.
`std::meta::reflectable` now directly uses `is_reflectable<T>()`.
`std::atomic_scalar` selects Boolean or integral types in source and composes
the lifecycle and concurrency concepts. `std::layout_queryable` and
`std::storage_element` compose object-layout and borrow-free facts. Their
former private concept binding labels are removed; actual reflection access,
native atomic operations and object lifetimes remain compiler-checked.
Arithmetic domain concepts use source requirements for the actual operations.
Their `[[unsatisfied(...)]]` explanations also live in source, so constraint
diagnostics retain useful guidance without recognizing those concept names.
The former `adds`, `subtracts`, `negates`, and `scales_by` compiler bindings
and dedicated proof branches have been removed. Their generic-body operation
facts now use the same general expression-requirement path as user concepts.
Equality and ordering policy now also lives in source. Ordinary safe expression
queries prove the operations; a source metadata scan retains the public,
read-only, nongeneric, requirement-free, exact-reference declaration policy for
classes. General method predicates expose consuming access, unsafe obligations,
generic declarations and method requirements. Primitive comparisons remain
value operations, not hypothetical reference-taking member calls. The old
comparison capability bindings and dedicated class-policy checker are removed.
The policies use `std::meta::lookup_methods_of<T>()` and
`lookup_operators_of<T>()` when inherited declarations matter. These return
the resolved member set, including inherited overloads and excluding
overridden base declarations. Each metaobject retains its exact instantiated
declaring owner and original declaration index. Names are sorted lexically;
overloads retain lookup order. Existing `methods_of<T>()`, `operators_of<T>()`
and `type<T>::method_count()` remain declaration-only. Metadata alone never
grants access or permission to invoke a method; expression queries still check
the actual use on `T`.
`std::meta::is_raw_pointer<T>()` and `type<T>::is_raw_pointer()` classify a
concrete type using the existing type-category metadata. They do not establish
pointer validity, lifetime, access rights, or permission to dereference it.
Iterator relations returning checked borrows, and callable relations carrying
the complete read/mutable/once-use and borrow/escape contract, retain their
existing compiler facts. A single owned-result expression query is not a
replacement for those complete relationships.

Prefix mutation and compound assignment can also supply general operation
proofs, for example `requires(mut T& value, int n) { { value += n } -> void; }`.
The proof preserves the receiver's mutable-access requirement; it does not let
a generic function modify a read-only reference. As with all expression
queries, checking this requirement does not mutate any caller object.
Iterator decrement and jump policies now use these ordinary source queries;
their former private category bindings are removed. Borrowing iterator and
range relationships still retain the facts described above.

For example, `std::totally_ordered` remains source composition, and the iterator
units still define public concepts over the necessary private structural facts:

```gti
namespace std {
concept copy_constructible<T> =
    std::meta::is_copy_constructible<T>();

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
- `sentinel_for<S, I>` is source-defined: declaration metadata requires one
  exact public safe read-only `bool I::operator!=(S&)` or copyable by-value
  `S` parameter, and an expression query proves the named-operand call; and
- `dereferences_to<I, T>` requires public read-only `I::operator*()` to return
  a checked reference whose referent is exactly `T`;
- `writable_iterator<I>` requires public `mut I::operator*() mut` returning a
  mutable checked reference plus public `void operator++() mut`; the public
  spelling is `std::output_iterator<I>`;
- `writes_to<I, T>` is a source-defined requirement for mutable dereference
  returning exactly `mut T&`, borrowed from the mutable cursor parameter;
- `bidirectional_iterator<I>` requires public `void operator--() mut`;
- `random_access_iterator<I>` requires public
  `void operator+=(std::ptrdiff_t) mut` and
  `void operator-=(std::ptrdiff_t) mut`;
- `subscripts_to_mut<I, T>` requires public
  `mut T& I::operator[](std::ptrdiff_t) mut` with exactly `T`. Source metadata
  checks the exact by-value offset declaration, and a borrowed expression
  requirement checks its result access and cursor origin;
- `sized_sentinel_for<S, I>` composes the sentinel policy with source
  metadata and a query for exact public read-only
  `std::ptrdiff_t I::operator-(S&)` or copyable by-value `S`, the signed
  distance from the iterator to the sentinel. Neither sentinel policy has
  a private compiler binding;
- `input_range<R>` is a source iteration requirement for
  `for (auto& element : range) {}`. `range_of<R, T>` additionally checks an
  exact `T` element, and `mutable_range_of<R, T>` composes that with
  `for (mut T& element : range) {}` on a mutable borrowed range. These policies
  use actual loop semantics, including fixed arrays and safe materialized
  value yields. This deliberately supersedes the old private declaration-only
  range checker: a viable ordinary loop, rather than a second hand-maintained
  overload filter, establishes the protocol;
- `sized_range<R>` requires exact public read-only
  `std::size_t R::size()`; fixed arrays satisfy it from their exact extent;
- `forward_iterator<I>` is the source-defined conjunction of input iteration,
  copy construction, and use of `I` as its own sentinel; the public
  bidirectional and random-access concepts compose their movement facts over
  it, so only copyable read cursors satisfy them; and
- `accumulates_into<I, T>` is the numeric-library composition of the same
  exact dereference relationship;
- `addable<T>`, `subtractable<T>`, and `negatable<T>` are source-defined
  requirements for readable addition, subtraction and negation returning an
  owned `T`; `scalable_by<T, Scalar>` checks readable multiplication by an owned
  exact `Scalar`. These domain concepts require a struct/class, not a primitive.
  Ordinary call viability applies: a by-value operand cannot copy a move-only
  borrowed value merely because a signature exists;
- `read_callable<F, A, R>`, `mut_callable<F, A, R>`, and
  `once_callable<F, A, R>` are source expression queries for a safe unary
  invocation returning exactly the owned, non-borrowed `R`, with either a
  borrowed `A&` argument or a consumed owned `A`. Nominal targets provide a
  public `operator()`; lambdas and GTI function items use ordinary resolution. The
  invocation capability is respectively read-only, repeatable
  read-only/mutable, or usable once through owned access;
- the corresponding `read_binary_callable`, `mut_binary_callable`, and
  `once_binary_callable` relationships test both arguments, including mixed
  borrowed/consumed forms;
- `once_action<F>` is the source-defined nullary exact-`void` consuming-use
  relation: `requires(F action) { { std::move(action)() } -> void; }`;
- `indirect_predicate<F, I>` queries the ordinary `function(*position)`
  composition in source and requires a safe repeatable `bool` invocation; and
- `indirect_operation<F, I, R>` uses that same composition with an exact owned
  `R` result; and
- `range_predicate<F, R>` and `range_operation<F, R, Result>` derive the exact
  read-only element of a complete range and apply the same repeatable callable
  rules without exposing an associated iterator type. These are source-defined
  queries over `*range.begin()`, or indexing for fixed arrays only; an unrelated
  indexing overload on a class cannot substitute for its iterated element.

The unary/binary source policies deliberately replace the former declaration
census with usable expression queries. A callable may therefore qualify through
a valid owned-argument form even when its borrowed form is ambiguous. The
concept does not choose the argument form for a caller: the actual invocation
must still satisfy exact overload selection, copying/movement, access and
lifetime checks. An ambiguous named-argument call remains an error at the call
site. Generic code that needs one specific form can state that expression
directly in a requirement. No unary/binary callable compiler bindings remain.

The iterator-aware queries likewise preserve the actual dereference expression's
value category rather than a private approximation based only on its referent
type. Owned yields, checked loans, callable access, and any copy or move must
satisfy ordinary expression rules. Their whole-range counterparts additionally
require the input-range contract. All callable concept policy is source-defined;
the compiler retains the general invocation and ownership rules these queries
ask about.

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
selected. Inside a generic body, a dependent target such as `less<T>`, or a
lambda whose signature names `T`, satisfies a relation through its substituted
`operator()` signature. A bare type parameter satisfies one only through an
enclosing requirement.

The nullary `once_action` policy no longer has a compiler concept binding.
Its ordinary expression requirement selects the call and checks public access,
safe invocation, ownership transfer and the exact `void` result. Ordinary call
viability decides this nullary relation; the query does not relax operator
declaration restrictions or make an otherwise unavailable overload callable.
A query neither invokes nor consumes a runtime value;
an actual `invoke_once` call still transfers ownership and performs deterministic
cleanup. Unary, binary, iterator-aware and range-aware callable policies use
the same ordinary query machinery rather than private signature checkers.

An exact callable argument describes a readable source value. A concrete
target may copy it by value or observe it through an exact read-only reference.
Library implementations select the borrowed or consumed form that their
algorithm needs; qualification alone does not make a different form viable.
They do not use native reference binding to widen the contract. These policies
require owned, non-borrowed results because they have no result-owner parameter;
callable-valued results also retain the ordinary callable escape checks.

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
`<std/iterator>` still owns its not-yet-migrated inferred input-iterator
relationship:

```gti
namespace gti_internal {
@compiler_constraint("input_iterator")
concept input_iterator_capability<I>;
}
```

`@compiler_constraint` is accepted only inside `gti_internal` in the implicit
prelude or in the exact trusted unit assigned to that binding. The remaining
four bindings belong to `<std/iterator>`: input iteration, dereference-to,
writable iteration and size availability. Migrated classification, lifecycle,
comparison, callable, sentinel, range and exact borrowed-result policies have
no private concept declarations. They compose general queries and irreducible
compiler facts in ordinary source. Application source cannot forge a binding, name the private
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
tracked borrows, or raw pointers. Eligible zero-state GTI function items also
satisfy the exact callable relationship and may be retained from the result;
their declaration identity and signature survive ordinary generic transport.
Public algorithm names and emitted C++ overloads grant no capability.

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

## Protocol concepts and conformance blocks

A concept may have a body instead of `= constraint;`. The body declares
associated types with `typename name;` and operations as function
prototypes:

```gti
concept measurable<T> {
  typename unit_type;
  unit_type measure(T& item);
  bool is_empty(T& item);
};
```

A type satisfies such a concept only through a conformance block, declared
at namespace scope:

```gti
for concept measurable<Rope> {
  using unit_type = Meters;
  Meters measure(Rope& item) { return Meters(item.length); }
  bool is_empty(Rope& item) { return item.length == 0; }
};

for <T> concept measurable<Crate<T>> {
  using unit_type = int;
  unit_type measure(Crate<T>& item) { return item.count; }
  bool is_empty(Crate<T>& item) { return item.count == 0; }
};
```

Users write `C<X>::name` for an associated type and `C<X>::operation(...)`
for an operation:

```gti
measurable<T>::unit_type total<T>(T& item) requires measurable<T> {
  return measurable<T>::measure(item);
}
```

### Declaring a concept with a body

A concept with a body takes one or more type parameters. Its member names
are distinct from each other, from the parameters and from the concept.

An operation is a free function signature. It has no body, generic
parameters, requires clause, defaults, packs, modifiers, attributes or
receiver qualifier. Its types may name the parameters, the concept's
associated names, and visible types.

A concept with several parameters relates types to each other. An
associated type then depends on all of them:

```gti
concept adds_to<Left, Right> {
  typename sum_type;
  sum_type add(Left& left, Right& right);
};

for concept adds_to<Meters, Centimeters> {
  using sum_type = Centimeters;
  Centimeters add(Meters& left, Centimeters& right) { ... }
};

for <T> concept adds_to<Tally<T>, Tally<T>> {
  using sum_type = Tally<T>;
  Tally<T> add(Tally<T>& left, Tally<T>& right) { ... }
};

adds_to<L, R>::sum_type total<L, R>(L& left, R& right)
  requires adds_to<L, R> {
  return adds_to<L, R>::add(left, right);
}
```

### Conformance blocks

The block applies the concept to one pattern per concept parameter. Each
pattern is either a concrete type, or one class or enum template, pointer or
fixed-length array type applied to block type parameters, in any order, such
as `Box<T>`, `Pair<K, V>`, an enum `result<T, E>`, `expected<T, E>`, `T*` or
`T[3]`. A pointer pattern keeps its access and an
array pattern its length, so `T*`, `const T*`, `T[3]` and `T[4]` are four
different patterns; an array length cannot be a block parameter. A bare
parameter, such as `T` on its own, and a nested pattern, such as
`Box<Box<T>>` or `T**`, are not patterns. Every block parameter appears in
some pattern. A parameter may appear more than once, within one pattern or
across several, and then every occurrence must bind the same type:
`Tally<T>, Tally<T>` matches two tallies of the same kind only.

The block defines every associated type with one `using`, and every
operation with one function. Each function has exactly the concept's
signature after substituting the patterns for the parameters and the block's
types for the associated types. A block defines nothing else.

A block function is an ordinary function definition. The block's parameters
apply to it, and it cannot declare its own generic parameters or a requires
clause. Like the operation it defines, it declares no default arguments, so
a call accepts the same arguments whether it names a concrete type or goes
through a requirement. Inside the block:
- the block's associated types are transparent, whether written as
  `unit_type` or as `measurable<Crate<T>>::unit_type`;
- each operation requires the block's own application;
- the block's functions can call each other by name.

### Selection and coherence

Selection is exact:
- concrete arguments match at most one block of a concept, and two blocks
  that some arguments could both match are rejected as overlapping;
- there is no specialization or ranking between blocks;
- the block's type-parameter constraints must hold for the bound types,
  for `requires C<X>` and equally for a concrete `C<X>::name` or
  `C<X>::operation(...)`.

The declaring package must own the concept or the top-level nominal type of
at least one pattern, as for
[exact class specializations](../decisions/020-exact-qualified-class-specializations.md).
No package owns a pointer, an array or another built-in type, so a block
whose patterns are all built-in types belongs in the concept's package.
Because no pattern is a bare parameter, overlapping blocks in two different
packages would require each package to depend on the other, which package
dependencies rule out.
All blocks register before bodies are analyzed, so the block that applies
does not depend on include order.

### Uses in generic code

A declared class/enum family conformance also proves an open application whose
complete head matches that family. For example, a block for `C<Wrapper<T>>`
can establish `C<Wrapper<U>>` inside a generic body. Its binder constraints
must still be satisfied; repeated binders must agree exactly. A block for only
`Wrapper<int>` does not establish the family, and a bare unknown type still
needs an explicit requirement. Associated types reduce by the same checked
substitution. If a result can become a reference, its required borrow contract
must already be declared; concrete instantiation verifies the exact dependency.

In generic code, `C<T>::name` and `C<T>::operation(...)` require `C<T>` as a
conjunct of the enclosing `requires` clause, not inside a disjunction. With
several parameters the conjunct must name the same arguments in the same
order: `requires adds_to<L, R>` does not provide `adds_to<R, L>`. The
generic body sees the associated type as an opaque type and checks calls
against the declared signature. Each concrete instance then resolves both to
the selected block.

A concept with a body may be composed into an ordinary concept for concrete
checks. Its associated types are reachable only through a direct conjunct.
An inline constraint such as `<measurable T>` is rejected; write a trailing
`requires` clause.

### No remapping

A block function is reachable only through its concept. It never takes part
in member or ordinary name lookup, so a block cannot change what
`value.member()` means. Calls to block functions are ordinary calls,
including evaluation order, ownership transfer and failure cleanup.

An associated type definition can name a checked reference: `using element =
int&;` is shared and `using element = mut int&;` is exclusive. The modifier
belongs to the reference type, not mutable storage for the alias. `mut` on a
non-reference associated type is rejected; this does not add general reference
aliases outside conformance blocks.

An operation may declare `[[borrowed_from(parameter)]]`. This names the
parameter position that supplies a checked reference result; an implementation
may rename that parameter but must preserve its position and contract. For an
associated or dependent result type the contract applies only to reference
instances, and is inert for owned results. A generic implementation need not
annotate an owned-returning instance merely because its uninstantiated return
type is open. Selecting a concrete reference-returning conformance, including
through a generic requirement, checks the implementation's exact parameter
contract before permitting the call. A by-value argument may supply the
reference through one exact stored external dependency. It never supplies a
reference to the consumed argument's own storage. Each implementation body is
checked against that declaration contract, including in generic instances.
Unsafe implementations cannot satisfy ordinary safe operation declarations.

Violations are reported as `GTI-S2123`; an unsatisfied requirement at a call
is `GTI-S2029`. See [ADR 028](../decisions/028-protocol-concepts.md).

The prelude's `std::tryable<T>` is the protocol of prefix `?`, including
source-defined `expected`. A type propagates with `?` once a block supplies
its `value_type`, `residual_type`, `has_value`, `take_value` and
`take_residual` ([ADR 029](../decisions/029-tryable-propagation.md)).
Constant invocation uses the same protocol and MIR control flow when the
selected operations and residual construction belong to the supported constexpr
call closure. A runtime-only operation remains ineligible for constant execution.

## Deliberate exclusions and extension rule

The implemented language does not provide general C++ requires-expressions,
arbitrary expression requirements, negation, associated types outside a concept
with a body, value constraints, concept or partial specialization, constraint
subsumption, constraint-based overload ordering, conditionally available
operators, constrained polymorphic contracts, or unrestricted compile-time
reflection. Variadic callable relations, generic `operator()` requirements and
native function pointers in these public callable relations remain outside the
current bounded surface. Eligible namespace-scope GTI function items are
supported; an unresolved overload set still needs one exact context, and
unsafe/native/generic declarations do not automatically become stored items.

Add a new source concept when existing facts can express the contract. Add a
new private compiler capability only when semantic analysis must answer a
genuinely new irreducible question for a demonstrated language or library
client. Do not add a capability for a public class, algorithm, container state,
naming convention, or convenience alias.
