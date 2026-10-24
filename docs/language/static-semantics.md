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
- nullable recursive raw-pointer types with per-layer qualification;
- `expected<T, E>`;
- narrow compiler-private owner, uninitialized-object and host capabilities;
- generic type and value parameters; and
- lexical lambda types and nominal namespace function-item types;
- function-signature metadata types, which have no object storage themselves.

Aliases are transparent names and do not introduce new nominal types.

## 3.2 Bindings And Access

Bindings and parameters are immutable unless declared `mut`. Fields are also
immutable unless declared `mut`. A class or struct may group consecutive direct
instance fields inside `mut { ... }`; each enclosed field is semantically the
same ordinary mutable field as if it had its own leading `mut`. The group does
not change binding or receiver mutability, access, type identity, layout,
lifecycle, initialization order, or the immutable default outside its braces.
Methods have read-only receivers unless their declaration has a trailing `mut`
or `&&` qualifier. Trailing `&&` denotes consuming access, not a general
rvalue-reference type. It is available on ordinary instance methods,
`operator()`, and arithmetic/bitwise value operators. A consuming member may
mutate its writable fields while consuming its receiver; free and static
functions have no receiver and cannot use this qualifier.

An explicit `mut` on an ordinary local value that is never required for a
write, mutable-reference argument, mutable raw address, mutable receiver,
explicit move out of one of its fields or constant-index elements, or reflected
`get_mut` field access is well-formed but produces warning `GTI-S2096`. Moving
the whole binding does not require `mut`. Removing that token is a
semantics-preserving quick fix. The compiler is conservative at boundaries it
cannot prove: generated range/reference bindings and compiler-trusted library
implementation locals do not receive this advisory warning.
Mutable operator receivers count like ordinary method receivers. A fixed
mutable-reference argument in a pack fold also counts during primary generic
analysis. Passing a mutable place to an unresolved callable conservatively
retains `mut`, because its concrete parameter may require writable access.
Advice is declaration-wide: one concrete `if constexpr` branch does not make
shared source unreachable for other specializations. Independent unused
mutable locals and unconditional unreachable tails still receive warnings.

`T&` denotes a non-null read-only borrow of `T`. `mut T&` denotes a non-null
writable borrow. Reference binding and escape are subject to the ownership
rules in [Ownership And Lifetimes](ownership-and-lifetimes.md).

A local mutable reference may be reborrowed as a mutable or read-only child
when its source has a stable root with named-field, constant fixed-array-index
or checked-dereference projections. The mutable parent is suspended while any
child loan remains active and fully reactivates only after the final active child's proven
endpoint. Access through a known-disjoint named-field projection remains
available, and multiple child loans may coexist only over such disjoint
places. Parent or owner access that overlaps an active child is ill-formed,
and a read-only loan cannot be upgraded to a mutable child. Dynamic indices
remain potentially overlapping; raw and opaque sources do not gain this precise
place proof. Returning a child must satisfy the independently checked
return-origin contract. Stored carriers and range iteration have the explicit
contracts below; neither bypasses parent suspension or owner lifetime.

A nominal carrier may retain multiple shared/exclusive owner dependencies,
including recursively composed nominal fields. Copy construction is available
only when all dependencies are shared and ordinary fields are copyable; a copy
aliases the same owner loans, not the source shell. An exclusive carrier moves
its existing dependency set and is noncopyable. Neither form supports assignment
that would reseat established dependencies. A single-path shared carrier may
retain a read-only child of an exclusive parent, keeping that parent suspended
until every child alias ends. Local mutable cursors are ordinary clients of
this rule, not a library-name or hidden-range exception.

Checked carrier returns must preserve the complete summarized origin set. The
separately escaping reference-return rule for a by-value carrier still requires
its one exact externally borrowed reference path. A shell field, unrelated
referent or unsupported projection cannot substitute for that origin
(`GTI-S2085`). The [stored-reference contract](ownership-and-lifetimes.md#references)
states constructor, nested dependency, child-loan and escape boundaries.

For a receiver- or argument-tied result, the stable place is the selected
origin expression. The compiler preserves a caller-visible field path such as
`parent.left`, but it does not infer an internal returned-field projection from
the callee body. A result tied to a whole receiver or parameter therefore
conservatively protects that whole origin.

Call and construction arguments are checked under strict left-to-right
evaluation. If an earlier argument produces a transient borrow, a later
mutation of an overlapping place is ill-formed because that loan remains active
through the full-expression. An earlier completed mutation may be followed by
a later borrow when no other loan conflicts. Ordered ordinary-call and
constructor schedules implement this distinction; reversed order does not
shorten the earlier borrow, and a later read under an exclusive argument is
also rejected.

Ordinary local value bindings require initialization at their declaration,
including mutable scalar bindings. `mut` grants write access; it does not
permit an uninitialized local followed by a later assignment. The compiler
diagnoses a missing initializer before executable lowering and does not invent
a zero value. Constructor-initialized instance fields and program-wide scalar
data initialization retain their separate rules.

Local `auto` infers one exact complete value type from its initializer. It does
not infer globals, fields, parameters, returns, arrays, or untyped braced
initializers. Ordinary initialized locals may use `auto&` for a read-only
reference or `mut auto&` for a mutable reference. They infer the initializer's
exact referent type and apply the same addressability, access, stable-storage,
reborrow and lifetime checks as explicit `T&`/`mut T&`. Inference creates no
copy, movement, owner or lifetime extension; a checked reference returned through
a temporary carrier is tied to its established external owner. Whole fixed-array
and raw-pointer references remain rejected just as in explicit declarations;
references to supported array elements are valid. This also applies in concretely
analyzed generic bodies. `auto&&` and API-visible reference inference are not
supported. The range-for forms infer element borrows;
plain `auto` does not silently infer a reference. A fixed array is traversed
with an exact indexed strategy. A read-only loop receives a shared owner loan.
A `mut T&` or `mut auto&` loop over an addressable range requires a mutable
owner and receives an exclusive owner loan; an owned nominal temporary is
moved into mutable loop storage. A mutable nominal range must resolve exact
`begin_mut()`, `end()`, comparison, mutable dereference, and prefix-increment
operations. Every reference element loan ends before increment or loop exit
and may not escape its iteration. A read-only element cannot mutate; a mutable
element is one child of the exclusive range loan and is the only overlapping
mutation path until it ends.

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

Inert address formation from an admitted checked place is safe and creates no
loan or lifetime extension. A raw projection beneath that formation, raw
dereference or store, raw indexing, raw arrow access, pointer arithmetic, and a
pointer-bearing C call require a lexically enclosing `unsafe` block. A nested
lambda starts a fresh safety context. Unsafe context does not suppress type, access, initialization,
ownership, or control-flow checking.

A GTI free function or ordinary non-polymorphic instance/static method may
carry `unsafe` to require lexical unsafe permission at every call. Exact member
requirements preserve this effect explicitly. Concrete generic instances retain this
requirement. It grants no permission inside the declaration's body and cannot
be erased through a native callback or safe function-item conversion. Hosted
`main` cannot carry it. See [the caller contract](raw-pointers.md#functions-with-a-caller-obligation)
for the admitted declaration forms and proof obligations.

The ordinary type surface supports recursively nested pointer levels, including
locals, parameters, fields, aliases and native out-pointers, with exact
per-layer qualification and native-call obligations. References to raw
pointers, anonymous/general function-pointer declarators, pointer-to-array
types, implicit fixed-array
decay, integer/pointer casts, unrelated typed-pointer reinterpretation, and
source allocation/deallocation expressions are ill-formed. Explicit
const-preserving typed/`void*` conversions use pointer-alias type calls (or
concrete dependent target type calls), as specified in
[Raw pointers](raw-pointers.md#void).
An exact `[[c_array(count)]] extern "C"` return additionally records its counted
native array contract; it is not the source of general nested-pointer
permission. `void*` is opaque and cannot be
dereferenced, indexed, used for member access, or used in pointer arithmetic.
Namespace-scope aliases may name the distinct bounded native callback type
specified by [`native-c-interop.md`](native-c-interop.md#native-callback-types).

The complete programmer obligations are incorporated from
[`raw-pointers.md`](raw-pointers.md).

## 3.3.1 Unreachable Statements

After a statement that cannot fall through, the first statement of the
consecutive unreachable tail produces warning `GTI-S2095`. Unreachable source
is still parsed and semantically analyzed in isolated flow state so names,
hover/completion facts, and independent errors remain available without
repairing the reachable ownership state. The compiler does not offer an
automatic deletion because removing an unreachable region can still discard
intentional declarations or documentation.

## 3.4 Names, Scopes, And Visibility

An explicit module interface publishes only its exported namespace-scope
declarations and its explicit re-exports. Publication is per declaration and
overload, not per spelling: exporting one overload does not export the others.
Private implementation declarations and private imports remain usable while
checking their defining bodies and concrete generic instances. Consumers cannot
name a private declaration through qualification or an include bypass. A public
function can return a private nominal type; an inferred value retains that exact
type and ordinary member access, without making its private type name visible.
Modules do not introduce namespaces or change existing program-wide
qualified-name uniqueness, ownership, borrowing, or lifecycle rules. The
[source syntax](lexical-and-syntax.md#explicit-module-interfaces) specifies
import resolution and the initial single-interface boundary.

Function-signature types use `R(Args...)` syntax, for example `void()` and
`bool(int)`. They may be alias targets and generic type arguments, including
components dependent on generic type parameters. Their exact result and ordered
parameter types and receiver qualifier form their identity; transparent aliases
preserve that identity. An absent qualifier describes read-only invocation,
trailing `mut` describes mutable invocation, and trailing `&&` describes
consuming invocation. For example, `int(int)`, `int(int) mut`, and `int(int) &&`
are three distinct metadata types. Exact deduction and class-pattern matching
preserve the qualifier; they do not convert one signature type to another.
This metadata does not itself implement invocation: an owning source-defined
class must still declare an operator with the appropriate receiver access.
The empty parameter list is `()`, not `(void)`. Parameters are value or reference
types; results may also be `void`, but cannot be arrays or signatures. Reference
components follow the existing reference-placement rules. A parameter `T&`
describes read-only access, while `mut T&` describes mutable access; for example,
`void(mut int&, int&)`. The prefix requires a reference and is distinct from
the trailing qualifier governing access to the callable itself.

A signature is type metadata, with no object storage, copy/move operations, or
implicit function-pointer conversion. It cannot itself be a variable, field,
parameter, return value, array element, pointer pointee, or reference referent.
A generic class may use a signature as metadata without storing it as a field.
The native callback alias `(Args...) -> R` remains a distinct C ABI type.
Ordinary exact generic deduction can decompose fixed-arity signatures in a
parameter type: `Tag<R(A)>&` can infer `R = void` and `A = int` from a
`Tag<void(int)>` argument. Reference components retain their exact layers;
matching `R&(A&)` against `int&(int&)` infers `int` for both parameters.
Repeated deductions must agree, and this adds no conversion or overload ranking.
A final type pack can also be deduced from signature metadata:
`int inspect<R, Args...>(Tag<R(Args...)>& tag)` extracts the result and exact
ordered argument list, including an empty list or reference elements. A fixed
prefix such as `R(int, Args...)` retains its exact required prefix. The same
rule applies to the final pack of a nominal type (`Types<Args...>`). Repeated
patterns, explicitly supplied pack elements, and a matching named value pack
must all agree; inline pack constraints apply to every inferred element.
Metadata references remain types. When used in a concrete named parameter pack,
read-only reference elements borrow the caller's storage rather than own values.
Headerless primary declarations can decompose a signature:
`class Callback<R(Args...)> { ... };` accepts one public signature argument
and binds `R` and the ordered list `Args`. Bare unqualified type leaves in the
head declare parameters, even when an outer type has the same spelling;
builtins and qualified names refer to existing types. Repeated names must
agree exactly. For example, `class Identity<R(R)>` accepts `int(int)` but not
`int(bool)`. A fixed prefix such as `R(int, Args...)` is exact, and a pack may
be empty. At least one binder is required, with at most one type pack and no
value parameters or defaults in a pattern head. A name may have disjoint
single-signature definitions for the read-only, `mut`, and `&&` receiver
qualifiers. Each qualifier admits at most one definition, even when result or
argument patterns differ. All definitions must have the same class/struct kind
and belong to the same package ownership domain. Each has a distinct class
identity and member scope; selection requires that definition to be visible.
The qualifier selects the definition before exact component matching, including
after generic substitution. There is no ranking, fallback between qualifiers,
or overlap with an ordinary `class Name<T>` primary. Exact qualified concrete
specializations remain available against the selected definition.
Public arguments and aliases retain their canonical identity through generic
forwarding and concrete reanalysis; a non-signature does not become a signature
by conversion. Read-only and mutable reference elements can flow through named
parameter packs. Mutable references retain exclusive access to each caller
argument, and overlapping mutable/read or mutable/mutable arguments are rejected.
Source-defined owning wrappers implement read, mutable and consuming invocation
under the separate callable ownership/cleanup contract. The declaration syntax
alone does not supply storage, erasure or `std::function` policy.

An argument-profile family can also combine an ordinary type-parameter definition
with void, checked-reference and raw-pointer profiles, for example
`class Packet<T, E>`, `class Packet<void, E>`, `class Packet<T&, E>`, and
`class Packet<const T*, E>`. Each has its own members and layout. A profile head
contains bare type binders, `void`, reference binders, or one raw-pointer layer
over a binder (`T*`, `const T*`); it has no packs, defaults, value parameters,
deeper pointer patterns or nested patterns.
All definitions have the same public arity, class/struct kind and package owner.
Non-fallback profiles must be mutually disjoint: `Packet<T&, E>` and
`Packet<T, E&>` overlap and cannot form a family. At most one ordinary definition
is the fallback. Selection is independent of declaration order and precedes
member lookup; constraints on a selected definition do not cause fallback to a
different one. Read-only and mutable references are distinct profiles, as are
pointers to read-only and mutable pointees; a pointer binder binds the pointee,
so `T*` matching `int**` binds `T` to `int*`.
Generic deduction uses the family's public arguments, then concrete checking
uses the selected definition's exact identity. This does not allow conversions
between definitions or erase checked references' origins/access/lifetimes.
[Source value profiles](../../examples/168-source-value-profiles.gti) demonstrates
ordinary owned, empty and borrowed classes without a library-specific capability.

A lambda or nominal `operator()` call can expand a concrete parameter pack. Concrete
reanalysis checks the flattened arity and each exact argument type, including
an empty expansion. It does not pass a pack object as one lambda argument.
Declaration-wide symbolic packs defer target publication until that check.
Nominal operator overloads compare the expanded arguments in exact order;
this does not expand operands of non-call operators or add implicit conversions.

Concrete fixed-array values also retain their exact element type and every
extent through by-value parameter-pack forwarding and constructor emplacement.
The implemented array staging family is copyable, contains no tracked borrowed
state, and requires no active element cleanup. Each forwarded array is a value
snapshot: it does not decay to a pointer, acquire an implicit reference, or erase
an element lifetime dependency. Fixed-array arguments before a final pack follow
the same rule. Owning or borrow-carrying array transport is not enabled by this
passive-array staging rule.
Read-only reference elements forward to the same exact read-only reference
parameter type. They are not copied, moved, or given callee cleanup obligations;
the ordinary caller-side borrow rules apply for the invocation. Exact mutable
reference elements likewise preserve exclusive caller access and may be
forwarded sequentially without consuming the referent. Overlapping arguments
remain rejected; this is not implicit reference deduction from a value pack.

Namespaces, namespace aliases, and qualified `::` names use lexical declaration
and source-unit visibility. `namespace outer::inner { ... }` is shorthand for
`namespace outer { namespace inner { ... } }`; it can reopen existing
namespaces and nest inside another namespace. Every component denotes an
ordinary namespace, not a namespace alias. Alias declarations retain a single
declared name (`namespace short_name = outer::inner;`). An exported shorthand
namespace exports its nested declarations under the ordinary module rules.
[The namespaces example](../../examples/006-namespaces.gti) demonstrates the
shorthand, qualification, and alias spelling. Namespace- and class-scoped
`using Name = Type;` aliases are declaration-order independent within their
direct declaring scope after cycle validation. Every class-like body (`class`,
`struct`, `interface`, and `union`) admits aliases as type members. An alias
follows the owner's current `public:`, `protected:`, or `private:` section and
the class key's default access, and its name cannot collide with a value, callable, or another
type alias in that owner. A public alias of a non-generic type may be named with
`Owner::Name` outside the class.

A generic class alias may name the owner's type or value parameters and is
substituted for each concrete member-body and backend instance. The alias is
usable unqualified within that owner. A type alias naming a specialization may
qualify its public type members: `using IntBox = Box<int>;` permits
`IntBox::value_type`. Each alias qualifier retains its access checks and its
owning specialization's generic substitution, including chained type members.
Specialized owners may also appear directly in type paths:
`Box<int>::value_type` and `Outer<int>::nested_type::value_type` resolve the
same types as their named-alias equivalents. Owner arguments (including
defaults and value generics) are resolved and validated
before member selection. Every qualifier retains source visibility and access
checks; a public final member cannot bypass a private intermediate alias.
Each selected member alias is substituted under its actual owning instance.
An enclosing generic must establish the selected owner's constraints: for
example, `CursorTraits<std::move_constructible T>` may define
`using iterator = std::vector<T>::iterator;`; unconstrained `T` does not gain
vector's move-construction guarantee merely by naming its alias.
Named classes and structs can be declared directly inside a non-generic class
or struct. The enclosing access section controls visibility of the nested
type; a struct defaults to public and a class to private. `Config::Network`
names a distinct nominal type, not a namespace and not an implicit field.
Nested types and their enclosing type may use these names unqualified within
their lexical class scopes. A private nested type is accessible inside its
declaring owner and lexically nested types, but a public child cannot bypass a
private intermediate owner. Lexically nested types may access their enclosing
types' private aliases and members; this privilege does not extend in the
opposite direction or to unrelated types. Instance-member access still needs
an explicit enclosing object, except that an explicitly inherited base is
already part of the current receiver and uses ordinary inherited access.
A nested type has no enclosing object, hidden
parent pointer, or implicit access to an outer instance's fields. Its values
use ordinary construction, access, ownership, cleanup, reflection and
transfer/share rules. Nesting adds no runtime state.
As with existing transparent aliases, an explicitly public alias may expose a
private type selected inside its owner; that does not make the original private
name accessible or allow a private alias as a path qualifier.

Nested types may have their own generic parameters. Definitions inside a
generic enclosing type or its exact specialization are currently rejected with
`GTI-S2030`: retaining the
enclosing specialization in the child identity and substitution is a remaining
prerequisite. Anonymous records, field-name injection, nested interfaces/unions
and inherited nested-type lookup are not part of this slice. The
[nested configuration example](../../examples/148-nested-configuration.gti)
demonstrates nested records and a class with reflected JSON.

This form extends type contexts, not direct specialized-alias constructor
expressions; introduce a named alias or use direct brace initialization at the
typed declaration. Existing restrictions on aliases in exact class definitions
remain in force. Direct type aliases and named nested classes/structs can follow
a class-like owner;
fields, methods, namespace members, alias templates and inherited aliases do
not become type members. A malformed or missing member is diagnosed by the
frontend before HIR/MIR/backend entry. `Box::value_type` remains rejected
outside generic `Box` as an unspecialized type. Class aliases are direct members
rather than inherited members; a derived class does not acquire a base alias.
Alias templates,
C++ `using Base::member` exposure, constructor inheritance, and access
reshaping are not part of this declaration form. Native callback aliases remain
namespace-only under the native C interoperation contract.

Aliases preserve exact type identity during registration, but constraints on
their target applications are checked only after class members and lifecycle
facts are complete. This applies to unused aliases too; aliases cannot bypass
the target generic's constraints. A class argument therefore has the same
constraint meaning in an alias and in an ordinary declaration.

Concrete generic discovery has deterministic compilation-resource bounds: 64
new-instance levels in a request chain and 16,384 distinct generic class, function,
constructor and destructor instances per HIR request (including generic-owner
members and empty packs, not ordinary declaration roots). Exact recursion reuses
one instance. Type- or value-changing recursion must terminate within those
bounds through a selected compile-time branch or exact specialization; a
runtime `if` does not prevent instantiation of the other branch. Exceeding a
bound is diagnosed as `GTI-S2105`, not a hang or backend failure. These are
compiler resource bounds rather than a termination proof for generic programs.

Class members follow their declared access and the default associated with
`class` or `struct`. `interface` instance members are public contracts;
its static methods follow ordinary access rules and are public by default. Namespace
`static` declarations have source-unit internal linkage. Type-owned static
members do not participate in instance layout or lifecycle.

A combined backend translation unit shall not make a declaration visible where
the GTI source graph does not.

## 3.5 Initialization And Conversion

Initialization requires an exact type unless a specific rule permits a
conversion. Numeric conversions use `Type(value)` and checked GTI semantics.
Constructors are selected by one exact parameter list, and a class may declare
conversion operators to other types (section 3.9.2). Neither defines an
implicit conversion for calls or assignment. The initialization rule below
selects one, declared `implicit`, for a declaration initializer or an explicitly
declared value return type.

The source-defined `std::upcast_unique<Base, Derived>` operation is an explicit
ownership conversion, not an overload conversion. Its trusted private step is
well-formed only when `Base` is a public base of `Derived` and has polymorphic
destruction. Calls never convert `unique_ptr<Derived>` to `unique_ptr<Base>`
implicitly.

One bounded target-typed rule applies to an explicitly declared class or
struct binding, including a field's constructor initializer: when its
initializer is exactly `nullptr`, the destination may select one accessible
constructor whose sole parameter is exactly `nullptr_t`. The same conversion
is not considered for function arguments, constructor arguments, or returns.
Assignment does not use this construction rule; a class assignment from
`nullptr` requires an exact member `operator=(nullptr_t)`.

The same declarations also construct from an initializer of another type, but
only through a constructor its class declares `implicit`. In
`T name = initializer;`, where `T` is the declared class or struct type and
the initializer has a type `U` that no other rule admits, the declaration
constructs `T` exactly as `T name{initializer}` would, considering only `T`'s
implicit constructors. It selects the one whose exact parameter list accepts
the initializer as its sole explicit argument, by value or by read-only
reference, with any trailing defaults. Overload selection, access, argument
ownership, failure, and cleanup are those of the direct initializer. So
`std::string text = "Sampling...";` uses `std::string`'s
`implicit string(std::string_view)`, and `Celsius warm = 21;` calls a declared
`implicit Celsius(int)`. A constructor without `implicit`, such as a
container's element-count constructor, is only ever called explicitly. A named
move-only initializer still requires `std::move`, and an ambiguous or
unavailable selection is diagnosed as for direct initialization. When no
implicit constructor accepts `U`, the declaration keeps its exact-type
initialization error, with a hint naming the explicit form. The rule does not
apply to `constexpr` bindings, range-`for` element bindings, reference fields,
unions, abstract classes, a declared type that carries a borrow, or an
initializer whose type comes from its destination: a brace list, a
contextually typed call, or a function name. Function arguments and assignment never use it.

The same one-step selection initializes an explicitly declared value return
type from a return operand. For example, `Celsius read() { return 21; }` uses
`implicit Celsius(int)`. This includes functions, methods and explicitly typed
lambdas; destinations containing open type parameters, including computed
type projections, are checked during concrete generic reanalysis. The
destination is a fresh result, while the constructor argument
keeps its ordinary ownership checks: returning a named move-only source of a
different type requires `std::move(source)`. Existing exact-type named return
movement is unchanged. Reference returns do not construct temporaries, and
borrow-carrying target exclusions remain in force. An implicit constructor
whose parameter is exactly `nullptr_t` may initialize a return; the separate
non-implicit nullable-declaration exception above does not apply to returns.
No constructor/operator or numeric conversion chain is added. A class assignment from another type requires an
exact member `operator=`, such as `std::string`'s
`operator=(std::string_view)`.

A declaration whose initializer has a class type `U` also converts it
through an implicit conversion operator of `U` to exactly the declared type,
as `T(initializer)` would (section 3.9.2). This applies to every declared
value type, including `bool` and numeric types, under the same exclusions,
except that a declared class type need not be a constructible class. So
`UserId id = raw;` calls `RawId`'s `implicit operator UserId()`, and
`bool ready = flag;` calls an `implicit operator bool()`. When the declared
class also has an implicit constructor that takes the initializer, the
declaration is ambiguous (`GTI-S2122`): GTI never ranks a constructor against
a conversion operator, and `T name{initializer}` still selects the
constructor. A conversion operator without `implicit` converts only
explicitly; the exact-type error's hint then names `T(...)`.

`implicit` is written first in an ordinary constructor declaration, before
`constexpr`. It requires a source body and a constructor callable with zero or
one explicit arguments, so any further parameters must have defaults. It is
invalid on a copy or move constructor or a constructor with a parameter pack
(`GTI-S2121`), and cannot be combined with `unsafe`: a construction a
declaration performs implicitly carries no unsafe caller obligation.
Misplaced spellings, such as `constexpr implicit`, are syntax errors.

A bare `return;` in a class-valued function selects an accessible `implicit`
constructor callable with no explicit arguments. Omitted arguments use the
constructor's ordinary defaults. A generated default constructor or an explicit
zero-argument constructor does not opt in. Ambiguity, inaccessible constructors,
failed construction and cleanup follow ordinary direct construction rules.
The concrete return type owns selection, including after generic substitution;
this is not a rule tied to a library type's name. Semantics records the exact
return statement and selected constructor. HIR preserves that proof and a full
expression for construction/default-argument cleanup; no synthetic source
expression is invented. Borrowed dependencies from temporary default arguments
cannot escape an empty return. Raw-address fields remain unresolved in optional
pointer auditing until their construction origin is established.

Value-assignment contexts, including initialization, assignment, and return,
accept an integer literal when its mathematical value fits the destination.
A non-literal integer may widen within the same signedness, and an unsigned
integer may widen to a strictly wider signed destination. An integer value is
also assignment-compatible with `float` or `double`, and `float` may widen to
`double`. `double` never implicitly narrows to `float`, including through a
compound assignment. These assignment compatibilities do not participate in
call or constructor overload selection.

Integer-to-floating assignment conversion is a rounding conversion, not a
value-preserving widening guarantee. It uses the destination's IEEE binary32
or binary64 precision and round-to-nearest, ties-to-even, at compile time and
runtime. For example, `float value = 16777217` produces `16777216`, and a
`uint64_t` value of `9007199254740993` assigned to `double` produces
`9007199254740992`. All supported fixed-width integers fit the floating
formats' finite exponent range; precision loss is not an out-of-range failure.
An explicit conversion has the same rounding behavior. A call still requires
its exact argument type: pass `float(integer)` to a `float` parameter. The
existing checked floating-to-integer conversion does not restore lost bits;
it truncates toward zero and fails for non-finite or out-of-range results.

The binary equality, relational, arithmetic, modulo, and bitwise operators add
one narrow operand context for integer literals. When exactly one operand is
an integer literal, optionally parenthesized and optionally preceded by one
unary `+` or `-`, and the other operand has a built-in integer type or a
type-parameter type whose constraints imply `std::numeric`, the complete
literal expression adopts that exact type during checking. Its mathematical
value, including the sign, must fit a concrete adopted type; generic
declarations defer that range check until concrete instance reanalysis. A
floating-point instance uses the ordinary integer-to-floating operand
conversion instead of retyping an
integer constant as a floating constant. Thus `value == 0` and `value / 2`
work uniformly in numeric generics; integer promotions and explicit narrowing
of arithmetic results remain unchanged. The rule does not contextualize two
literal operands, floating literals, named constants, or arbitrary expression
trees, and does not participate in call or constructor overload selection.
Shift counts are excluded because their type is independent of the shifted
value. Non-literal operands receive no literal context: concrete built-in
operands use the existing numeric common-type rules, while mixed generic
operands must satisfy the operator's exact type contract.

`float` is IEEE-754 binary32 and `double` is IEEE-754 binary64. An unsuffixed
floating literal has type `float`; `d` or `D` selects `double`. A built-in
numeric operation uses `double` if either operand is `double`, otherwise
`float` if either operand is `float`, and converts an integer operand into that
selected format. Conversion from either floating type to an integer requires
explicit `IntegerType(value)` syntax and the checked range/truncation rule in
[execution semantics](execution.md#43-numeric-execution). Explicit
`float(value)` and `double(value)` accept integers or either floating width;
converting to `float` is the required explicit narrowing operation.

`char` remains a distinct nonnumeric unsigned eight-bit code-unit type. Two
explicit lossless conversions bridge text and byte-oriented code:
`uint8_t(character)` extracts the code-unit bits and `char(byte)` constructs the
same code unit from one `uint8_t`. Neither direction requires a range check.
The conversions are not implicit, do not admit any other integer type, and do
not make `char` satisfy an integer or numeric constraint.

`Type name{arguments};` directly constructs a declared class or struct. It is
not C++ aggregate initialization, list conversion, initializer-list preference,
copy-list initialization, or CTAD.

Structs additionally support **designated construction**:

```gti
struct Point { int x = 0; int y = 0; };
Point point = {.x = 3, .y = 4};
Point other{.y = 8}; // x uses its declaration default
```

The destination supplies the exact struct type. Names refer to public instance
fields, must occur at most once, and must follow declaration order. Positional
and designated entries cannot be mixed. Omitted fields require declaration
initializers; there is no implicit zero initialization of missing fields.
Nested designated lists receive their destination field's type.

This form is limited to structs without declared constructors or inheritance,
and public value fields. Reference, fixed-array, borrowed-state, and native-layout
records are excluded from this first slice. Classes retain constructor-based
initialization. Normal ownership rules still apply: a fresh owning value is
accepted, while transferring a named move-only value requires `std::move`.
Nonliteral numeric values require the exact field type in this initial slice;
use an explicit numeric conversion when the types differ. Integer literals
retain the usual representable-value initialization rules.
Field inputs are evaluated in declaration order, including omitted defaults;
prepared owning inputs have the ordinary constructor-argument rollback rules.
Existing backend limits on declaration-time field initializers still apply;
in particular, arbitrary function calls in declaration defaults are not newly
supported by this syntax. Such calls can be supplied explicitly in a
designated entry instead.

An ordinary brace list in a class-typed initialization context uses the
**owned collection protocol**. This includes `C values = {...}` and a
designated field such as `.subcommands = {{.name = ...}, ...}`. Each nested
element receives the exact element type, so collection lists and designated
struct construction compose recursively. `std::vector<T>` implements this
protocol; application-defined containers can implement it too. There is no
`std::initializer_list` object or hidden const backing array.

A participating class or struct `C` supplies these two public, non-overloaded,
static GTI functions (names are protocol names, not standard-library identity):

```gti
static C collection_init(uint64_t count);
static C collection_insert(C collection, T element);
```

Both functions must have GTI bodies, return exactly `C`, and have no
method-local generic parameters, default arguments, or unsafe contract. The
containing class may be generic. Ordinary `requires` constraints are checked
after substituting its concrete arguments, using the same rules as explicit
calls. Both hooks must satisfy their constraints, even for an empty list;
an unsatisfied unused hook does not forbid other explicit construction of the
container. The second insertion parameter
defines the exact contextual element type `T`. The collection must be movable;
collection and element values cannot carry stored borrow dependencies.
Reference and fixed-array element parameters are excluded from this slice.
Expected-value and confined-callable elements are also excluded until their
ordered owned-call transport is supported by this lowering path.
Normal ownership rules apply: named move-only elements require `std::move`,
while fresh owners transfer directly. Numeric values require the exact element
type; integer literals adopt the usual representable contextual type. Owning
strings still require `std::string("text")`; this adds no string conversion.

Execution first calls `collection_init` with the written count, then evaluates
each element left to right and calls `collection_insert`, transferring the
previous collection and the new element. Each insertion completes before the
next element begins. Empty braces call only `collection_init(0)`. Allocation,
ordering, duplicate-key policy, and storage are library responsibilities;
vector reserves once and appends each element. Failure uses ordinary call
argument and local cleanup, including the already-built collection and any
prepared element. No collection-specific backend execution path exists.

Direct braces `C values{arguments}` retain constructor-call meaning, and call
arguments retain the contextual fixed-array rules below. This slice does not
add untyped lists, list-preferred overload selection, or collection type
deduction. Other standard containers must explicitly implement the protocol;
being container-shaped does not silently opt a type in.

A union uses the same direct-brace spelling only for zero initialization or an
exact value for its first declared field. Selecting another active field is an
explicit write to a `mut` field through a mutable union value inside `unsafe`.

Fixed arrays require complete initialization. Empty braces value-initialize all
elements; a non-empty initializer supplies exactly one value per element.
Representable signed integer literals, including a parenthesized single unary
`+` or `-`, receive the exact element type. Passive records and unions can be
copied from named values into arrays under their ordinary copyability rules;
this does not introduce extra default-constructor calls. Union member accesses
still require `unsafe`.

Inside an ordinary named function, method, or constructor call, the same brace
syntax may initialize one exact by-value fixed-array parameter. It is a
contextual owned `T[N]` value, not a `std::initializer_list` or a general
untyped expression. The selected parameter supplies both `T` and `N`; elements
are checked under `T`, and a non-empty brace count must equal `N`. `{}`
value-initializes the selected extent. Braces do not infer a common element
type, prefer an overload, perform CTAD, or add implicit conversions.

Program-wide bindings initialize in the dependency/source order defined by
[Execution Section 4.2.4](execution.md#424-program-wide-initialization). A safe
initializer is well-formed only when the compiler proves that it cannot access,
directly or through a GTI call, a program-wide binding whose initialization
step has not completed. Declaration visibility does not provide an early value.
Substituting a frontend-computed `constexpr` value without forming or loading
its storage is not such an access.
The compiler builds this ordered plan, closes initializer effects through exact
calls and lifecycle operations, and rejects a later-storage access or unknown
transitive effect with `GTI-S2068` before HIR. The merged verified `Module/0`
body then executes the accepted plan through hosted startup or the no-entry
initialization guard.

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

An ordinary free function, non-polymorphic ordinary or static method, or
ordinary constructor may give a trailing suffix of parameters default
arguments. A candidate is arity-viable when the supplied argument count is
between its required and total parameter counts. Exact matching examines only
the supplied arguments. A default does not change overload identity and does
not rank one viable exact candidate over another; multiple viable exact
candidates are ambiguous.

Each default expression is analyzed in the declaration's surrounding scope,
not the function body scope. It cannot reference any ordinary callable
parameter, `this`, or an instance member. Defaulted parameters must be owned or
plain by-value values; references, raw pointers, C strings, and types
containing tracked borrowed state remain explicit. Operators, lambdas,
payload-enum fields, parameter
packs, runtime/intrinsic and `extern "C"` declarations, virtual/override/
interface instance methods, copy/move constructor policies, and `main` do not admit
defaults. The expression must exactly initialize the declared parameter type,
subject only to the ordinary bounded assignment compatibility. Recursive
omitted-default expansion is ill-formed.

An immutable `uint64_t` function, method, or constructor value parameter may be
inferred when it is the complete extent of a by-value fixed-array parameter.
A named array contributes its exact extent; a contextual brace argument
contributes its written count. Other type parameters must be inferred from
ordinary typed arguments. The inferred value is part of concrete HIR instance
identity, but it does not distinguish alpha-equivalent overload declarations.
An ordinary function or non-polymorphic method call may instead write type and
value arguments explicitly, for example `extent<4>()`, `first<int, 2>(values)`,
or `extent<2 + 2>()`. A written value is a closed `uint64_t` expression over
integer literals, an in-scope `constexpr uint64_t` binding, or an enclosing
`uint64_t` value parameter, with parentheses and checked `+`, `-`, `*`, `/`,
and `%`. It is resolved by the target parameter kind rather than treated as a
type. Exact inference runs before declaration defaults fill any still-unbound
trailing parameters; there is no result-context inference. A written suffix
must supply every non-defaulted type parameter; once it supplies a value, it
must also supply every non-defaulted value parameter. An omitted value may
still be inferred from a fixed-array argument when the suffix writes only type
arguments. The full concrete values are retained by semantic selection,
concrete reanalysis, HIR, MIR, and the detached lowered instance inventory.
Constructor-local value parameters remain inference-only, cannot declare a
default, and cannot shadow an enclosing class or struct generic parameter.
Unrestricted value-generic expressions and value packs are not implemented.

Generic type arguments are type metadata and may be `void`, including written,
inferred, and defaulted arguments. That does not give `void` an object
representation: concrete variables, fields, array elements, reference
referents, and value parameters still require valid object types. Constraints
remain in force. Concrete class alias targets are revalidated under the
selected substitution, so `using signature = int(T)` rejects `T = void` even
if no field uses the alias. `void*` remains an opaque pointer value.
Ordinary nominal generic arguments may also carry reference types as metadata,
without introducing stored borrows into that owner. Reference referents retain
their normal restrictions (including no void, array, raw-pointer, or nested
reference referents). Actual reference bindings still require an explicit
reference declarator; a type argument cannot silently turn an ordinary value
field into a stored-reference carrier. Concrete field/lifetime validation
continues to reject unsupported borrowed storage.

Generic declarations may give their type and value parameter list a trailing
suffix of defaults. A type default must resolve to an exact type, may
reference only visible types and earlier generic parameters, and must satisfy
its inline constraint. Constraint checks within a dependent type default wait
until its earlier parameters are substituted and the default is selected.
An explicitly supplied argument does not instantiate the unused default;
closed invalid defaults still fail at declaration time. Selecting a default
validates nested class constraints as well as the defaulted parameter's own
constraint before HIR. This does not defer constraints in ordinary generic
fields or other non-default type uses. A value default uses the closed expression domain above
and may reference only earlier visible namespace `constexpr uint64_t` bindings
whose initializers are themselves closed `uint64_t` arithmetic and earlier
value parameters. Arithmetic is checked in the `uint64_t` domain at the
earliest concrete substitution; overflow, underflow, division by zero, and
modulo by zero are ill-formed. Defaults are applied in declaration order after
inference and therefore may depend on an earlier resolved parameter. A
non-defaulted parameter cannot follow a defaulted parameter, and a generic
parameter list containing a pack cannot contain defaults.

An ordinary constructor may also declare constructor-local type parameters.
They are inferred only from supplied, already typed call arguments using the
same exact structural inference as a generic function; there is no written
constructor-local argument list. The result context, omitted defaults,
requirements, and untyped braces do not supply deductions. Repeated
occurrences must infer one canonical type, and every local type parameter must
occur in an inferable parameter position. A local parameter is visible only in
that constructor's parameters, requirements, defaults, initializer list, and
body and cannot shadow an owner parameter. A final constructor-local type pack
and final by-value parameter-pack expansion are implemented within the ordinary
owned pack/selected-constructor contract, including final-field construction.
Concrete pack elements retain exact copy/move/borrow roles, source order and
cleanup; this does not add explicit constructor-local arguments, local generic
defaults, CTAD, parameter reference packs or native variadic forwarding.

The selected constructor declaration, concrete owner, and ordered local
type/value arguments form one concrete constructor identity. Distinct local
arguments therefore remain distinct even when their native representation or
substituted parameter layout happens to match. Constructor generics do not add
CTAD, partial specialization, implicit conversion, or a concrete-over-generic
preference.

Omitted ordinary callable-parameter defaults do not participate in generic
inference. Generic declaration defaults are different: after written generic
arguments and exact call-argument inference have produced a substitution, they
fill its unresolved trailing positions in declaration order. Semantic analysis
then rechecks the concrete generic instance and every used ordinary parameter
default under that complete substitution.

Contextual braces participate only after candidate parameter shapes are known.
Fixed extents may remove candidates with a different element count; if more
than one exact contextual array type remains, the call is ambiguous. GTI has no
initializer-list preference or C++ list-overload ranking. Generic `operator()`
dispatch does not yet admit brace arguments because its callable contract has
no contextual-array representation.

A direct generic callable invocation used as an argument, such as
`combine(callback(n), other)`, obtains its exact result requirement from the
selected outer parameter. This also applies to explicit consuming invocation
and stored generic callable fields. Argument evaluation remains left-to-right;
the callback and following arguments are checked once after selecting the
outer signature. Selection must be possible without inferring the callback's
concrete result or using that result to distinguish overloads. An unresolved
outer generic parameter cannot supply an exact result context. A typed local
can supply the context explicitly when selection otherwise remains ambiguous.

When the outer receiver is itself an open generic callable, a direct nested
invocation such as `outer(inner(value))` records a deferred result obligation
instead. Concrete semantic instantiation resolves `inner` first, then checks
that exact result against `outer`; it does not assume the intermediate has the
enclosing function's return type. Both selected signatures must be concrete
before HIR. The intermediate must be a non-void, non-reference value without
tracked borrowed state or lambda identity. This does not enable general `auto`
inference through an open callable, overload selection from unknown results,
or implicit callable escape. Existing receiver capabilities, ownership and
left-to-right evaluation rules remain unchanged.

A generic callable parameter may be a checked reference: `F& f` permits
read-only invocation and `mut F& f` permits read-only or mutable invocation.
The argument must satisfy ordinary reference binding and exclusivity rules.
Its environment remains caller-owned, and a mutable invocation changes that
same object. Direct forwarding to another proven confined parameter is
supported, including reference reborrows; this does not permit access upgrades,
consuming a borrowed object, reference packs, capture, storage or escape of a
confined callable. No `requires` clause is necessary; callable concepts can
add an explicit signature requirement.

A conditional in an `auto` value initializer can defer a direct open callable
arm's result until concrete semantic instantiation. Either or both arms may
be callable invocations, including parenthesized invocations and nested
conditionals. Each concrete instance must satisfy the existing arm-compatibility
rules: exact types apart from the established raw-pointer joins. This adds no
common-type conversion or overload ranking from an unknown result.
The inferred callable results must be non-void, non-reference values without
tracked borrowed state or lambda identity. An explicitly typed initializer or
enclosing return still supplies an exact result requirement immediately.
An unresolved result is not `void`; all executable signatures and bindings
must be concrete before HIR/MIR. Direct `auto result = f(...);` remains outside
this conditional-join slice. A discarded conditional retains the ordinary
void-operation contract.

Receiver mutability may distinguish method overloads. A read-only receiver can
select only a read-only method; a mutable receiver prefers the otherwise exact
mutable overload when both exist. Ordinary methods and `operator()` also admit
a consuming overload with trailing `&&`. It is available through an owned
movable temporary or an explicitly moved receiver, and an otherwise exact
consuming overload is preferred for those forms. A named value is never
implicitly consumed. Read-only, mutable, and consuming overloads may coexist;
arithmetic/bitwise value operators admit read-only and consuming forms.

An accessor pair is the exception to the mutable preference: a read-only
overload returning `T&` and an otherwise exact `mut` overload returning
`mut T&` to the same referent, as a method, `operator[]` or `operator*`. On a
mutable receiver the pair selects its `mut` form only when the use of its
result may need mutable access; otherwise it selects the read-only form, which
borrows the receiver as shared. A use is read-only when the result, or a field
or fixed-array element reached from it, is only read:

- it initializes a read-only reference, including a read-only reference
  parameter or return;
- it is copied into a value of its own type, or converted between built-in
  types, as a by-value variable, parameter or return;
- it is a condition, an operand of `!`, `&&` or `||`, or an operand of a
  built-in operator;
- it is the receiver of a member or operator whose overloads all have
  read-only receivers, or the argument of an operator whose parameters all
  read it.

A use through another accessor pair is that pair's own use, and a range-for
loop uses its range as its element binding requests. Every other use selects
the `mut` form, including assignment, increment, `mut` bindings and arguments,
mutating members, and uses the compiler does not classify. While a read-only
result is live, other reads of the same receiver remain valid, and a mutable
use conflicts with it as with any shared borrow. The rule is structural: a
user-defined class, `std::vector` and `expected` behave the same way.

A concrete self-contained class or struct value produced directly by an
ordinary constructor or by-value GTI function call is a fresh temporary
receiver. For one direct ordinary non-static method call, including reusable
`operator()`, it may be borrowed read-only or mutably; transparent parentheses
do not change that classification. The method may be virtual, an override, an
interface implementation, or inherited from a base class, and the call keeps
the member's own dispatch. A fresh receiver prefers an
otherwise exact consuming overload, then a trailing-`mut` overload when no
consuming overload matches. Receiver-only materialization additionally admits
owned movable conditional and operator results, lambda results, copy/move
construction, and owned Expected extraction. This permission is receiver-only: the
value does not become a general mutable place, and cannot bind a mutable
reference, have its address taken, or be assigned through as storage.
`std::move(named)` is existing storage rather than a fresh receiver. The
selected method must return `void`, an ordinary scalar, or an owned value with
no tracked borrowed state, except that a read-only member may provide a
nonescaping receiver-tied read observation through a read-only reference or
string view. Such a borrow cannot outlive the temporary's full expression.
A read-only member that returns one text field of a temporary that holds a
local owner's text is not such a borrow: its result is a view of that owner
(see [owner-tracked view fields](ownership-and-lifetimes.md#owner-tracked-view-fields)).
Consuming members cannot return any reference or tracked borrowed state.

For an ordinary GTI function or non-operator method call, a direct fresh
construction, GTI function call, or GTI value-operator result may initialize an
exact read-only `T&` parameter for that invocation
only. An explicit `std::move(local)` of a whole local or parameter may do the
same, as an explicitly moved receiver does: the move transfers the local into a
temporary that the enclosing full expression owns and destroys, and the local
is moved on every later path.
Overload selection first considers every otherwise-exact candidate that needs
no temporary argument borrow. Only when that tier is empty does it consider
otherwise-exact candidates whose mismatch consists solely of one or more such
fresh values targeting read-only references; selection still requires one
unique candidate within the active tier. The selected callable must have a
body, use GTI linkage, and return `void`, a scalar, or an owned value without
tracked borrowed state. Mutable references, moves of a projection, native or
callback targets, compound producers, borrowed-state values, and results that
could retain the argument loan remain ill-formed. Transparent parentheses do
not change eligibility. A temporary borrowed inside a conditionally evaluated
operand, such as the right operand of `||`, lives to the end of the enclosing
full expression only on the path that created it.

The same call-scoped read-only parameter boundary admits an exact fresh lambda,
an explicitly moved lambda local, or a selected GTI function item. The selected
generic parameter must still have a proven confined callable contract, and
invocation must respect the callable's read-only capability. A function item
may select a target whose contract is proven only after its body is analyzed;
the program is then rejected unless that parameter is proven confined. A lambda
may own captures or retain checked shared capture dependencies: those exact
origins remain live and protected through the call and the temporary's
full-expression cleanup. No closure copy is introduced, and named callables
continue to bind by their ordinary reference. This does not admit mutable
temporary parameters, returned references to the temporary, or conflicting
mutation of a capture while the call remains live.

A consuming method or call operator may be virtual or an interface requirement. An
override must preserve its exact `&&` receiver capability, parameters, and
result. Its result cannot be a reference or contain tracked
borrowed state. Invocation consumes the exact source place under the ordinary
move-state rules; a later possible read, move, or
invocation is ill-formed. A confined generic call written as
`std::move(operation)()` requires at-most-once invocation and may select an
exact consuming target or a reusable read/mutable target. Reusable clients do
not accept a consuming-only target.

Moved nominal and lambda receivers may own active cleanup. Their MIR value
obligation remains in the caller through the enclosing full expression and
drains on normal or defined-failure cleanup. This includes an owned concrete
receiver invoking a virtual consuming operator. It does not make a borrowed
reference or pointer dereference an owned receiver; erased-pointee consumption
still requires an ownership contract beyond ordinary virtual dispatch.

The selected callable, constructor, or operator identity is part of the
program's semantics and must not be re-selected by a backend.

### Namespace Function Items

An eligible namespace free-function name can be used as a stateless callable
value. Without a required signature, exactly one eligible visible declaration
must identify the value; an overload set is not itself a value. Existing
confined generic-callable parameters and managed-thread tasks can supply an
exact required signature for contextual selection. The
selected declaration must be a non-generic, body-bearing GTI function at
namespace scope. Entry functions, members, operators, native declarations,
intrinsics, and compiler-private functions are not eligible.

Selection considers all visible declarations independently of source order.
Where a contract supplies a signature, the result type must be exactly its
result, and each written contract argument must bind under the ordinary exact
call-argument rules. There is no
conversion ranking, overload-set value, ADL, or backend overload resolution.
Exactly one viable declaration forms the item; zero candidates are a signature
mismatch and multiple candidates are ambiguous.

For a confined call such as `invoke_with(handler, args...)`, fixed arguments
and the final value pack are deduced before matching the handler's expanded
invocation signature. Pack elements use the existing value/read-only-reference
binding rules; they do not acquire mutable-reference access. An open forwarding
pack cannot distinguish overloads by its unknown arity. A unique eligible item
may be retained symbolically, and every concrete invocation is checked again.
Likewise, a contract forwarded to a member of a class with several
definitions, such as `mapping<R, E>` and `mapping<void, E>`, constrains only
the definition its concrete arguments select. When the candidate's own result
selects another definition, that edge supplies no signature, and the selected
member's concrete invocation is checked instead.

The resulting semantic type contains the selected nominal function identity,
exact parameter types, and exact result type. It is a zero-state read-callable
and is transfer- and share-capable, so the existing exact `void()` and owned-
result managed-thread transports can consume it. Its use is
subject to ordinary exact value rules: `auto item = handler;` retains its
identity, generic classes may store it, lambdas may copy-capture it, and generic
functions may return that exact type. Value packs preserve the nominal identity
without creating cleanup obligations. Function-item invocation can expand
concrete value/read-only-reference argument packs, with exact arity and type
checking. Evaluating the callee expression precedes its arguments even when
the selected target is statically known. A confined client returning its exact
callable parameter may likewise produce a retained item. Different named
functions remain different types even when their signatures match; source-defined
owning wrappers provide erasure. A named native callback alias
uses the separate exact native-callback conversion and never exposes the GTI
function item as a C/C++ function pointer.

Ordinary direct calls to the same function names retain their existing exact
overload behavior. Function-item contextualization neither changes their
candidate set nor creates a value when the name is called directly.

## 3.7 Generics

Named generic arguments are inferred exactly from value arguments, supplied
explicitly, or filled from a declaration-owned trailing default suffix. Generic
classes and structs provide every non-defaulted argument explicitly; a bare
generic class name is concrete only when every parameter has a default.

The current standard constraints describe frontend-owned primitive, lifecycle,
and exact structural capabilities. Namespace-scoped user concepts may declare
one or more type parameters and compose existing concepts with `&&`/`and`,
`||`/`or`, and parentheses. Conjunction binds more tightly than disjunction.
Each concept application supplies visible non-pack type parameters or closed
exact types containing no unresolved generic parameter and must match the
referenced declaration's arity. A dependent compound type such as
`Wrapper<T>` is not a closed exact type in this bounded form.

An inline constraint such as `std::numeric T` remains unary and
non-structural. A generic free function, non-polymorphic method, or ordinary
generic constructor may instead use a trailing bounded constraint expression,
for example `requires Concept<Parameter...> &&
(Relation<Parameter...> || Alternate<Parameter...>)`. Clause arguments
must be visible non-pack generic type parameters or closed exact types. These
requirements provide exact facts while the symbolic body is checked and are
substituted and revalidated for every concrete call.

A concrete disjunction succeeds when either complete branch succeeds. During
symbolic body checking it exposes only the intersection of the alternatives'
guaranteed atomic and structural facts; conjunction exposes their union. This
prevents facts from separate alternatives from being combined into a proof.
The named concept predicate remains attached to an inline generic parameter
for concrete viability even when the common fact set is empty.

Operators and virtual, pure, overriding, or interface instance methods cannot carry a
trailing clause in the bounded first version. Operator availability and
polymorphic contract equivalence require separate representation rules.

Constraints affect candidate validity but do not rank overloads, distinguish
otherwise identical signatures, provide SFINAE, or select specializations.
Exact member signatures can be declared with
`requires (T& x) { { x.snapshot() } -> R; }`, either in concepts or as a
boolean static query. `R` is an exact owned result type; the query does not
execute the method. The current profile additionally admits argument-bearing
read/mut signatures, checked reference parameters, dependent compound types and
explicit unsafe requirements. See
[concepts](concepts.md#exact-member-signature-requirements) for the bounded
syntax, composition and ownership rules.
Negation, general requires-expressions, arbitrary expression requirements,
value constraints, constraint subsumption, forwarding
references, and unrestricted metaprogramming are not part of the implemented
language.

Class and struct type parameters may be followed by immutable `uint64_t` value
parameters. Their defaults, arguments, and expression contexts are restricted
by the incorporated grammar. Defaults produce the same complete canonical
type/value substitution as written or inferred arguments. Concrete generic
bodies are rechecked after substitution; generic validity and default selection
are not delegated to C++ template instantiation.

A function or method may own one final type pack, bound either through a final
named immutable by-value parameter pack or an explicit type-list expansion in
a formal parameter's type metadata. A metadata-bound pack does not add runtime
parameters and may also expand into returned nominal metadata. Bare pack types
remain invalid as return values or object storage. A named value pack uses the
same deduced list if both forms are present. Whole-pack forwarding remains available
only as the final argument of another variadic call. The bounded comma-pack
fold `(call(fixed, pack), ...)` additionally applies one call pattern to each
concrete pack element. It is well-formed only when:

- the bare parameter-pack name occurs exactly once and is the pattern call's
  final argument;
- every other argument is a named place, so the fold does not conceal an
  effectful expression that would be reevaluated per element;
- the target is one directly named, non-overloaded ordinary free function with
  `void` result;
- that target has one generic type parameter supplied by the element and an
  exact final read-only reference parameter to that element type; and
- the source pack's constraint facts imply the target element parameter's
  constraints while the generic body is checked.

Semantics selects that one declaration before expansion. Concrete generic
reanalysis then substitutes each source-pack element in order, validates its
constraints and exact parameter types, and records one resolved call identity
per element. It does not reopen an overload set for each type. The expression
has type `void`, including for an empty pack. This rule adds neither a general
fold-expression algebra nor pack indexing, multiple packs, mutable element
iteration, partial movement, member-call patterns, or backend-selected
template behavior.

A class or struct may also declare one final type pack, for example
`class Signature<R, Args...>`. This form cannot combine packs with value
parameters or generic defaults. A bare `Args...` type-list expansion is allowed
only as the final parameter of a signature (`R(Args...)`) or as the final
argument feeding another class's final pack (`Signature<R, Args...>`). A pack
cannot fill a fixed generic slot or stand alone as a field type. Decorated
expansions such as `Args&...` are not yet supported. Empty packs contribute no
types; `Types<>` and `Types<>()` are valid for an all-pack owner. Concrete
elements preserve order and exact identity, and each must satisfy the pack's
constraint and its eventual use (so `void` metadata cannot become a signature
parameter).

A method may bind the enclosing class's final type pack with one final,
immutable, by-value parameter pack (`void send(Args... args)`). Its argument
types are prescribed by the receiver's class instantiation, not inferred anew
at the call. Ordinary methods, `operator()`, and interface/virtual methods use
the same exact argument and ownership rules. A method may also have its own
fixed generic parameters. Empty packs require no arguments; forwarding uses
the existing final `args...` expression, with per-element deterministic cleanup
and no repeated consumption of move-only elements. Concrete value-pack elements
must be non-void object types or references; bare signatures are rejected even
if the method is never called. Mutable reference elements require exact mutable
reference forwarding and do not consume the referent, so sequential forwarding
is allowed. The grouped pack binding itself remains immutable.
This does not admit reference-pack declarators (`Args&...`) or forwarding
references, nor implicit borrowing during unconstrained pack deduction.

## 3.8 Classes, Interfaces, And Lifecycle

A class or struct has at most one state-bearing public base and may additionally
implement interfaces. Interfaces contain public behaviour contracts and no
instance state. Duplicate bases, cycles, diamonds, private inheritance, and
multiple state-bearing bases are ill-formed.

An interface instance method is written as a bodyless signature ending in `;`.
The enclosing `interface` makes that signature virtual and pure; spelling the
C++ pure specifier `= 0;` inside an interface is redundant and ill-formed. A
class or struct continues to use `virtual`, `override`, and `= 0;` for its
explicit virtual roots, implementations, and pure methods.

An interface may also contain concrete `static` methods.
These are ordinary type-owned functions, not requirements on implementing
types: they have no receiver, do not participate in virtual dispatch, and do
not occupy a dispatch-table slot. A factory such as `Window::Create(...)` can
therefore select and return an owned implementation before any window exists.
Each call may create a separate owner; `static` does not imply a singleton or
shared instance. Use `expected<std::unique_ptr<Window>, window_errc>` when
selection can fail, and explicitly upcast a concrete owner with
`std::upcast_unique<Window, ConcreteWindow>(...)`.

Static interface methods reuse ordinary static-method qualification, overload,
generic, access and concurrent-profile rules. They are public by default, but
may use ordinary access modifiers for implementation helpers. They cannot use
`this`, access instance state, declare a receiver qualifier, or be `virtual`,
`override` or pure. A bodyless static declaration is an ordinary declaration,
not a requirement on implementing types; selecting a missing implementation
reports `GTI-S2106` under ordinary definition-closure rules. Static abstract
requirements are not supported. This does not allow instance method bodies,
fields, constructors or non-public instance requirements. The runnable
[`149-interface-static-factories.gti`](../../examples/149-interface-static-factories.gti)
demonstrates a recoverable factory selecting two implementations and ordinary
instance dispatch through the returned owner.

An interface remains abstract and has polymorphic destruction even when it has
no instance signatures. Its implementing class is concrete once all inherited
instance requirements are implemented; static declarations do not add any.
Inherited static functions use ordinary member lookup. Identically named static
functions from separate bases need not be unified with an override: a call
selecting indistinguishable candidates is ambiguous and can instead qualify
the intended base. The ordinary prohibition on accidentally hiding a same-signature
non-virtual base member still applies.

```gti
interface Renderable {
  static int initial_frame() { return 1; }
  int render(int frame);
};

class Sprite : public Renderable {
public:
  int render(int frame) override { return frame; }
};
```

### Member visibility

Class members default to `private`; struct members default to `public`.
`public:`, `protected:`, and `private:` sections apply to following members,
including instance/static fields, methods, constructors, aliases and nested
types. Visibility is checked by GTI semantic analysis after lookup and overload
selection, before executable lowering. A public overload or virtual override
keeps its own declared access; a public API may use protected implementation
members without exposing them to its callers.

A protected member is accessible in its declaring class and lexically nested
classes, and in classes that publicly derive from that declaring class,
transitively, including their lexically nested classes. For instance members,
access granted through a derived class `D` additionally requires the receiver's
static type to be `D` or a descendant of `D`. A `Base&` or sibling receiver is
insufficient even if its dynamic object might be a `D`. The declaring class
itself may access the member on other objects of its own type. Implicit receiver
access follows the same rule. Concrete generic owner/receiver types matter:
deriving from `Base<int>` grants no access to `Base<bool>`'s protected members.

Static members and member types have no instance-receiver restriction, but
retain the declaring-class/derivation requirement. Ordinary static members
declared in generic owners remain unsupported under GTI's existing qualified
generic value-member path restriction; protected access does not expand it.
Qualification and public aliases cannot bypass an inaccessible intermediate
alias or nested type.
Protected constructors may initialize a derived object's direct base, including
implicit base default construction; this permission does not permit derived
code to construct arbitrary complete base objects. An `implicit` constructor
retains the same access requirement during declaration initialization. Copy/move
policies and destructors remain public under the existing ownership rules. Interface instance
requirements remain public; interface static helpers may use protected access.
Unions and native C records retain their public-storage restrictions.

Protected visibility changes no mutation, borrowing, initialization, lifetime,
unsafe, transfer/share or compiler-private capability obligations. Inheritance
must still be explicitly `public`; protected inheritance and friendship are
not introduced. No runtime access check, allocation or extra object state is
required. [`150-protected-events.gti`](../../examples/150-protected-events.gti)
demonstrates event-handler state and an unrelated generic counter.

### Optional out-of-class method definitions

An ordinary named method may keep its signature in a non-generic class, struct
or interface and put its body at namespace scope:

```gti
interface Factory { static int create(int value = 7); };
int Factory::create(int result) { return result; }
```

The definition completes that exact method; it cannot introduce another member.
Parameter types, return type, receiver qualifier (`mut` or `&&`), `constexpr`
and `unsafe` must match. Parameter names may differ. Access, `static`, `virtual`,
`override`, attributes, constraints and default arguments stay on the original
declaration. A definition must be in the owner's namespace or an enclosing
namespace and in the same package ownership domain. Duplicate/mismatched
definitions and foreign-package definitions are errors (`GTI-S2120`).

A `borrowed_from` parameter name is resolved on that original declaration and
retained by parameter position. Renaming parameters in the definition does not
change the origin; declaration indexing order cannot change contract acceptance.
An invalid declared origin remains an error rather than falling back to inference
from the separate body.

The body has ordinary member access, including private members and an implicit
receiver for instance methods. Pure virtual/interface instance requirements
cannot acquire bodies. Selected declarations still require definitions before
artifact generation; non-pure virtual declarations also require bodies. A
constexpr body must be available before a constant-evaluated call. Borrowed
results, consuming receivers, unsafe obligations and deterministic cleanup
retain their ordinary semantics, independent of the body's file.

Generic owners/methods, constructors, destructors and operators keep their
existing inline spelling for now. This is an optional source organization
facility, not a separate-compilation or native linker contract. See
[module registration](../architecture/build-and-driver.md#source-module-registration)
for automatically loading a private factory implementation when its interface
is imported. Example 150 and `examples/window-factory` demonstrate both forms.

Virtual roots and overrides match parameter types, receiver access, operator
identity, and return type exactly. A concretely bound owner pack is expanded
before comparing parameter lists, so an ordinary zero-argument or multi-argument
override can implement the corresponding instantiated interface pack. Symbolic
packs remain distinct until substitution. Abstract values cannot be directly
constructed. GTI does not permit object slicing. Polymorphic destruction is
derived by the compiler from lifecycle metadata.

An ordinary class or struct cannot contain itself transitively by value. Fixed
arrays and payload-enum alternatives preserve the by-value edge, including an
alternative that is not active, such as an error. References, raw pointers and
wrappers whose actual storage is indirect form indirection boundaries.
`GTI-S2082` diagnoses the field that would make the containing object layout
infinite before lifecycle inference or constructor-body analysis.

Default, copy, and move construction; copy and move assignment; and destruction
are generated from structural field traits. Declaring an ordinary constructor
does not suppress otherwise valid generated lifecycle operations.

The exact declaration `Type() = default;` explicitly selects the generated
zero-argument construction operation. It is valid only when every field has a
declaration initializer or is a nominal class/struct field whose exact type has
an accessible zero-argument construction operation, and the state-bearing base has an accessible
zero-argument constructor, which are the same conditions used when the
declaration is omitted. It introduces no executable body, initializer list,
generic parameters, or requirements. Its source access is the written member
access. Ordinary-constructor `= delete` is not implemented.

An omitted nominal field is default-constructed in declaration order, unless
an explicit constructor initializer overrides it. This is a selected GTI
construction operation, not implicit zeroing or a nullable empty state. Scalar,
array, reference, raw-pointer, and native-function fields retain their existing
explicit-initialization rules. Generic availability is checked again after
substitution: `Layer<N - 1>` is a distinct concrete field type from `Layer<N>`,
and a finite chain may end at an exact specialization. Exact cycles remain
invalid; concrete layout/lifecycle exploration has a deterministic depth budget
and reports `GTI-S2105` at the source when exhausted.

A class or struct may declare at most one public exact copy policy
`Type(Type&) = default|delete;` and one public exact move policy
`Type(Type&&) = default|delete;`. These declarations govern construction only;
copy and move assignment remain independently derived. `= default` preserves
the structural operation and is ill-formed when a base, field, stored borrow,
or cleanup policy makes that operation unavailable. `= delete` makes the
corresponding operation unavailable.

`T&&` is not a general GTI reference or forwarding-reference type. It is
permitted only in the exact move constructor policy. A custom copy or move
constructor body is not supported. GTI currently uses structural movement and
relocation; adding arbitrary user move bodies would require a separate language
decision, not merely completion of the existing field-place and object-lifetime
facilities.

A class or struct may declare one public destructor as either `~Type() { ... }`
or `~Type() = default;`. The defaulted form selects structural destruction and
does not create a source cleanup effect, active-drop state, or the conservative
copy/transfer restrictions caused by a cleanup body. Destructor `= delete` is
not implemented because every initialized GTI value must remain destructible
under the current ownership model.

`[[c_opaque]] struct Name;` is the only incomplete nominal type declaration.
It is nongeneric, baseless, has no body, layout, members, lifecycle, or
concurrency capability, and cannot be combined with `[[c_abi]]`. Its resolved
type may occur only behind raw-pointer indirection, including in an admitted
`[[c_abi]]` record field or `extern "C"` signature. The pointer remains
nullable, non-owning, and subject to lexical unsafe rules. Direct values,
construction, inheritance, generic arguments, pointee layout queries, and
ordinary forward declarations are rejected with `GTI-S2065` before lowering.

### 3.8.1 Exact Qualified Class Specializations

A generic class or struct may have a complete exact specialization. The
declaration is written at package scope, outside every namespace, with a
qualified primary name and every concrete type or `uint64_t` value argument:

```gti
namespace std {
class hash<T> {
public:
  size_t operator()(T& value);
};
}

class Widget {};

class std::hash<Widget> {
public:
  size_t operator()(Widget& value) { return 0; }
};
```

The specialization uses the primary's `class` or `struct` kind, declares no
generic parameters, and provides a body. It may use ordinary class capability
attributes but not native-record representation attributes. Interfaces,
unions, bodyless specializations, and unqualified specialization targets are
ill-formed.

Every argument is resolved and canonicalized before matching. Transparent
aliases therefore do not create distinct specializations, and compatibility
spellings such as `int` and `int32_t` denote the same key. Value arguments are
concrete `uint64_t` values. A symbolic argument, partial pattern, duplicate
canonical key, or wrong arity is ill-formed. Declaration order has no effect:
semantic analysis registers all valid exact keys before checking bodies and
selects the distinct specialization class identity whenever a resolved primary
application has that key, including after generic substitution. A nonmatching
application keeps the primary class identity. Requirements do not rank or
select specializations.

Package coherence permits a specialization only when its declaring package
owns the primary or owns at least one top-level nominal class, struct, or enum
type argument. A nominal hidden inside another generic argument does not grant
ownership, and value arguments never grant ownership. This admits an
application's `std::hash<Widget>` while rejecting its `std::hash<int>`, and it
prevents two unrelated packages from defining the same foreign/foreign key.
Canonical duplicates and coherence failures use `GTI-S2078`.

Exact specialization is not partial specialization, specialization ranking,
SFINAE, or a general template metaprogramming facility.

### 3.8.2 Passive Unions And Payload Enums

`union Name { fields... };` declares untagged overlapping storage. A union is
nongeneric, complete, baseless, non-empty, and public. It contains only
instance fields and cannot declare attributes other than the explicit `[[c_abi]]`
representation opt-in, access sections, methods,
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
hidden lifecycle behavior. `[[c_abi]] union` restricts fields to the native C
record allowlist and admits C ABI passage and generated-header/import support;
see [native C interoperation](native-c-interop.md#passive-native-records).
`GTI-S2066` owns ordinary union declaration-shape failures, while
the ordinary unsafe diagnostic owns safe member access.

The declaration spellings `enum Name` and `enum class Name` have identical
meaning. Both declare scoped nominal enums; neither injects enumerators into
the enclosing namespace or permits implicit conversion to integers.

A payload enum may declare ordinary nominal generic parameters:

```gti
enum Message<T, uint64_t Channel = 3> {
  idle,
  received(T payload),
  stopped(int32_t reason)
};
using TextMessage = Message<std::string>;
```

Type parameters, a suffix of immutable `uint64_t` parameters, inline
constraints, defaults and one final type pack use the existing nominal generic
argument rules. A pack may expand in nominal type metadata, not into a variable
number of alternatives or payload fields; it cannot coexist with value
parameters or defaults. Every argument contributes to exact enum identity,
including metadata unused by a payload. Transparent aliases, defaulted
arguments and equivalent primitive spellings denote the same instance.
Generic integral-only declarations remain unsupported.

The owner must be concrete at a value use: `Message<int, 7>::received(4)` or
`TextMessage::received(std::string("hello"))`. A bare owner is accepted only
when all required arguments are defaulted. Arguments cannot be attached to the
alternative, and construction does not infer owner arguments from payloads.
Exact nominal argument deduction through ordinary generic function parameters
can decompose enum type/value arguments and a final metadata pack, just as it
does other nominal types. Patterns must name the subject's exact enum instance;
sharing a primary declaration is not enough.

Semantic substitution validates actual payload signatures, constraints,
copy/move/cleanup and concurrency capabilities before executable lowering.
An unused generic primary is not a concrete runtime representation. Forward
class/enum type-default dependencies are resolved against complete nominal
identities without declaration-order-dependent acceptance; cyclic defaults are
ill-formed. Value defaults still require earlier visible constant bindings.
Aliases do not bypass instantiated
payload validation. Generic arguments are metadata, not ownership edges: an
unused `void` or reference type argument does not become a payload. An actual
field substituted to an unsupported type is still ill-formed, including when
constructing another alternative.

By-value layout checks distinguish concrete generic instances when traversing
class fields and enum alternatives. A cycle cannot be hidden by visiting the
same generic primary with different arguments. Finite nested class and enum payloads
remain valid; direct recursive layouts and structurally growing by-value
recurrence remain ill-formed.

An enum with at least one parameterized alternative is a closed tagged
sum. It cannot declare an integral backing type or explicit enumerator values.
Every payload field is named, immutable, and non-variadic. The admitted field
types include primitive scalars, `std::string_view`, `nullptr_t`, recursive raw
pointers, native function pointers, integral scoped enums, concrete payload
enums, fixed arrays, `expected` result carriers, and ordinary concrete class
instances, including generic instances. The array element or result
alternatives must themselves be admitted field types. Ownership and cleanup
fold from actual fields, not from whether their outer type is a class:
`Message<Message<std::string>>`, `Message<Resource[2]>`, and
`Message<expected<Resource, Error>>` use the same ordered field publication
and recursive cleanup contract. A `void` success alternative in a result
carrier contains no object. Owning indirection may break a layout cycle;
an inline enum/class/array cycle is still ill-formed. Conditional stored
borrows follow the separate checked-reference rules below; direct lexical
closure payloads remain outside this owning slice.
The temporary `unexpected(...)` error-construction adapter is not itself an
admitted field; construct an `expected` error alternative before storing it.
Closure fields need a concrete closure-field representation before admission;
unsupported fields are rejected with `GTI-S2067`, not left to native emission.

A payload argument may be a fresh prvalue construction, a copy of a
named value, or an explicit `std::move` of a named value; the payload enters
the alternative before any later sibling field expression evaluates, matching
the ordinary left-to-right argument order. An enum folds its copyability,
movability, assignment, and ownership traits from
those payload types, so a move-only payload makes the enum value move-only.
An admitted owning enum supports whole-value move construction. Move
assignment between direct local bindings additionally requires the folded
assignment capability of its payloads; field construction does not require
assignment. Move assignment first drops the destination's old active
alternative, transfers the source alternative, and
leaves the moved-from source without the transferred cleanup obligation.

The bounded owning family admits direct local values and top-level read-only
checked-reference observation. It also admits the enum itself by value across
an ordinary source-defined free function, non-polymorphic method or ordinary
constructor, and an exact typed captureless lambda. Ordinary argument analysis
selects copy only when the enum's folded traits permit it; otherwise an explicit
move or fresh value is required.

An ordinary non-polymorphic class or struct, including a concrete generic
instance, may contain an owning payload enum as a direct field. The field may
be initialized by an in-class initializer or an ordinary constructor
initializer. Its containing value
inherits the enum's structural copy, move, assignment, and cleanup traits and
may use the existing direct class-value call and return transport. A mutable
direct field may be replaced from an explicit move; the old active alternative
is dropped before the replacement obligation is published. Recursive owning
class containment is permitted when its by-value layout is finite, including
an alternative holding a vector of enclosing nodes. A direct enum/class
by-value cycle remains ill-formed. This owning containment rule does not itself
authorize stored-reference classes,
compiler-private capabilities, polymorphic bases, or native-layout
declarations.

A payload enum may contain `T&`, `mut T&`, and nested reference-bearing enum
values. Construction retains the exact owner, access and field path for each
active alternative, including nested guards. Earlier argument borrows remain
protected while later field expressions evaluate. Reference-free alternatives
have a known empty dependency set, not an invented owner. Shared dependencies
permit whole-value copying when every ordinary payload is copyable; any
exclusive reference path makes the enum non-copyable. Movement transports the
dependency authority without moving the referent.

Pattern selection proves the active alternative before exposing its reference
fields. A direct reference selected from a consumed enum may retain its exact
external owner after the enum shell ends. A function returning that reference
must establish its declared `borrowed_from` contract from the checked selection;
a boolean predicate does not establish a tag proof. Nested enum construction
and complete dependency transport do not yet authorize consuming an outer
alternative into a new *borrowed-value* pattern binding and then consuming that
inner value. That handoff remains a structural `GTI-S2067` diagnostic until it
has its own exact transfer/retirement proof; it must not reach an internal
compiler error. Reference-field selection and borrowed-value extraction are
distinct operations.

Concrete generic free functions, methods, and constructors use that same
ordered transport contract. A type argument mentioning the enum does not by
itself create an ownership edge; concrete fields and declaration-bound storage
capabilities determine containment and cleanup. This permits ordinary owning
collections and inline optional storage of the enum without public-library
name recognition. Consuming methods may return an enum field while retaining
the containing receiver through the caller's full expression.

Polymorphic dispatch, native/runtime declarations, operators,
and direct enum lambda captures retain `GTI-S2067` at their declaration or capture
boundary. Unsupported containment retains `GTI-S2067` at the field or
aggregate boundary, before HIR/MIR/backend emission. A direct checked
reference or raw pointer does not itself contain the referent's ownership
state. Fixed arrays of direct owning enum elements and direct enum captures
remain outside this transport slice.

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

Each binding is an immutable value scoped to its arm. How a by-value binding
obtains its field follows the value category of the switch subject, by the
same transport rule as `?` on a result:

- A named place is observed. A by-value binding is a copy, so a class-typed
  binding requires a copyable payload type, and its copy drops at the arm's
  end exactly like an ordinary local. A move-only active payload can still be
  matched exhaustively by using its tag-only form, such as
  `case Message::owned:`. A place reached through a reference, a reference
  local or a call such as `switch (holder.shapeRef())` that returns one, is
  observed the same way: the reference stays live until the selected arm has
  bound its payloads, or for as long as an `auto&` binding derived from it.
  The place needs the storage a reference bound to it would, so a subject
  derived from a temporary, such as `switch (Holder(3).shapeRef())`, is
  rejected with `GTI-S2017`; bind the owner to a local first.
- A consumed subject, a prvalue or an explicit `std::move(place)`, is consumed
  by the switch. By-value bindings take ownership of their fields by move,
  which admits move-only payloads, and drop at the arm's end unless moved or
  returned. A payload type that can be neither moved nor copied cannot be
  bound by value. `std::move(place)` leaves the place moved after the switch
  on every path, including tag-only and `default` arms, which drop the
  consumed enum at their entry.

```gti
enum class Envelope {
  empty,
  owned(Resource item),
};

Resource take(Envelope envelope) {
  switch (std::move(envelope)) {
  case Envelope::owned(item):
    return item;
  case Envelope::empty:
    return Resource();
  }
}
```

`case Message::owned(auto& item)` instead binds an arm-scoped shared reference
to the original payload; `mut auto& item` binds an exclusive mutable reference
and requires mutable enum storage. A consumed subject provides neither
reference. The entire matched enum is the conservative alias-conflict root,
including for sibling fields: two mutable payload bindings in the same arm
conflict. Owner replacement, movement, or conflicting access is forbidden
while the loan lives. Existing checked return summaries can relay
receiver-derived payload references, but cannot let local or temporary
payloads escape. Every alternative must be handled exactly once unless a
`default` arm is present. A complete payload switch is exhaustive for return,
ownership-state, loan, HIR, and MIR control-flow analysis. `GTI-S2067` owns
payload declaration, construction, and pattern/exhaustiveness, local-value
containment, and callable-boundary failures.

## 3.9 Operators And Contextual Conversion

The overloadable operator set and arity rules are defined by the incorporated
grammar. Operators are member-only and are selected through ordinary exact
semantic resolution. The backend does not perform GTI operator lookup.
`operator=` takes one exact argument and requires a mutable receiver. A
same-type assignment with no exact user operator continues to use the class's
generated copy or move assignment policy.

Unary `+`, `-`, and `~`, and binary arithmetic, remainder, bitwise, and shift
operators are read-only or explicitly consuming member calls returning owned
values. Binary forms take
one exact argument. Compound arithmetic, remainder, bitwise, and shift
operators take one exact argument, return `void`, and require trailing `mut`.
Prefix `--` has the same `void` and mutable-receiver contract as prefix `++`;
postfix overloading is unavailable. Related operators are never synthesized,
operands are not reversed, and ADL or conversion ranking cannot add a
candidate. Receiver/projection evaluation precedes the right operand exactly
once under the ordinary left-to-right and target-first execution rules.

Contextual `operator bool` participates only in the contexts enumerated by the
language contract. Logical `and`/`or` and `&&`/`||` are equivalent spellings
with identical short-circuit semantics.

### 3.9.1 Expected Propagation

For a prefix expression `?operand` whose `operand` has type `expected<T, E>`,
the immediately enclosing function or lambda must return
`expected<U, E>`. The error types are identical semantic types; propagation
does not consider conversions or overloads. The expression has type `T`, or
`void` when `T` is `void`.

For the bounded `expected<T&, E>` checked-borrow specialization, `?` yields a
read-only place of type `T`, retaining the original owner dependency without
copying or moving the referent. Only the error alternative uses the operand's
copy/move transport policy. That specialization accepts only a read-only
reference success payload, one non-borrowing error type, and the single receiver,
argument, or global owner origin described by the ownership rules. It may be a
local or function return, but not a field, global variable, parameter, or
lambda return; `value_or` and direct local `unexpected` initialization are not
available in the initial surface.

### 3.9.2 Propagation Through `std::tryable`

For `?operand` with any other operand type `X`, `std::tryable<X>` must hold
([ADR 029](../decisions/029-tryable-propagation.md)):
- for a concrete `X`, through a `for concept std::tryable<...>` block that
  matches `X`, whose parameter constraints hold for the binding;
- for a dependent `X`, through a `std::tryable<X>` conjunct of the enclosing
  `requires` clause.

`X` is a class, struct or payload-enum type, possibly an instance of a class
or enum template that a block's pattern names; a scalar-backed enum is
rejected in this phase. The operand is transported like a by-value argument of
type `X`, and `has_value` borrows a temporary as an ordinary call borrows a
fresh value of `X`. A payload enum declares no constructors, so a residual
initializes a payload-enum return type only when it has that type. A
temporary
or `std::move(place)` operand needs a movable `X`. A named place needs a
copyable `X` and remains available. A borrowed `X` must currently have one
exact stored-dependency path. A reference `value_type` is permitted when
`take_value`'s `[[borrowed_from(value)]]` contract relays that path's external
owner. The expression is then a place with the declared reference access;
consuming the operand does not terminate the returned external borrow. Other
value types and the residual contain no stored borrows; the residual is not a
reference. The expression otherwise has type `std::tryable<X>::value_type`,
or `void` when that type is `void`.

The immediately enclosing function or lambda has a declared return type `R`,
which is not `void`. The residual type is `R` itself, or `R` declares exactly
one unconstrained, safe `implicit` constructor whose single parameter is the
residual type. In a generic body these checks happen for each concrete
instance. Propagation through `std::tryable` is not available in constant
evaluation. Violations are `GTI-S2086`.

The early return of a residual counts as a return of the converted value.
When `R` carries borrowed dependencies, as `expected<T&, E>` does, that return
contributes the selected conversion's complete dependency set, which names no
dependency for an owned residual. A function using `?` therefore keeps a
complete borrowed-return contract; it is unknown only when the conversion has
no complete dependency contract.

An owning operand whose expression category is a value selects move transport
for both alternatives. A named owning place selects copy transport for both
alternatives.
The selected move or copy must be available for every non-`void` alternative.
Consequently a named expected with a move-only value or error is rejected;
`?std::move(result)` is the explicit consuming form. `GTI-S2086` owns invalid
operand types, enclosing return contexts, mismatched error types, and
unavailable alternative transport.

### 3.9.2 Conversion Operators

A class declares a conversion to another type with a conversion operator,
named by its target:

```gti
struct RawId {
  int64_t raw;
  RawId(int64_t value) : raw(value) {}
  operator UserId() { return UserId(this.raw); }
  implicit operator int64_t() { return this.raw; }
};

UserId user = UserId(raw);  // explicit: calls RawId's operator UserId()
int64_t bits = raw;         // implicit: a declaration may use it
```

The target is the operator's result type. A conversion operator is a
non-static member with no parameters and a read-only, `mut`, or consuming
(`&&`) receiver, and it may be `constexpr` or virtual. Its target is an owned
value type: not `void`, a reference, a text view (`std::string_view`,
`c_string`), a type carrying a borrow, or its own class. The boolean
conversion is spelled `operator bool()`; another spelling of `bool`, such as an
alias, is rejected. A class declares at most one conversion operator per target
and receiver. Aliases denote their resolved target, and `implicit` does not
distinguish declarations, so `operator Count()` and `operator int()` collide
when `Count` aliases `int`. A conversion operator is inherited and overridden
like any member; an override has the same target and receiver.

A conversion operator is explicit by default. Only `Target(value)` calls it,
including the keyword forms `int(value)`, `double(value)`, `char(value)`, and
`bool(value)`, and an alias of a numeric, raw pointer, or class target.
`implicit operator Target()` also admits the declaration and declared-value
return conversions of section 3.5. No other context converts: function arguments,
assignment, operator operands, and `auto` inference keep exact types, and brace
construction `Target{value}` considers only constructors.

**Selection.** In `Target(value)`, where `value` has a class type `U` that
declares a conversion operator to exactly `Target`, the conversion has two
possible paths: a constructor of `Target` that takes `value` as its one
explicit argument, and a conversion operator of `U` to `Target`. Each path
counts every declaration of its kind whose types match, whatever access,
receiver, or ownership it would then require. Exactly one path may exist: a
constructor and an operator together are ambiguous (`GTI-S2122`), with no
preference rule. When `U` declares no conversion operator to `Target`,
`Target(value)` is ordinary construction. On the operator path, ordinary
receiver selection chooses among `U`'s operators to `Target`: a read-only
receiver selects only a read-only operator, a mutable place prefers a `mut`
operator, and an owned temporary or `std::move(value)` prefers a consuming
operator. A consuming operator never consumes a named owner implicitly, and a
`mut` operator requires a mutable place. Access is checked after selection,
so a private operator is an access error, never a reason to use another path.
A declaration considers only implicit constructors and implicit operators,
under the same rule.

At most one conversion applies. GTI never chains a conversion operator with a
constructor, another conversion operator, or a numeric conversion: a
conversion to `C` through an intermediate `B` must be written in two steps.

**Execution.** A conversion calls the selected operator with the converted
value as its receiver, exactly as a member call would: the receiver's
ownership and borrow rules, failure propagation, and the cleanup of a
temporary receiver at the end of its full expression are those of the call.
Its result is a fresh value of the target type; a declaration initialized by
it owns that value. A consuming conversion consumes its receiver and cannot
return borrowed state.

**Contextual `operator bool`.** Conditions, `!`, `&&`, `||`, and the
conditional operator's condition use any `operator bool`, implicit or not.
`bool(value)` calls one explicitly, and `bool flag = value;` requires
`implicit operator bool()`.

**Generics.** A conversion whose source is a class, including a class
parameterized by type parameters, is resolved where it is written. Within
`Box<T>`, `operator T()` converts to the type parameter, and in a generic body
`T value = box;` uses it when it is implicit. A type-parameter target `T(value)`
remains a numeric conversion, because a generic body is checked before its
instances: GTI has no constraint that names a conversion operator.

## 3.10 Well-Formed Control Flow

Every reachable path through a non-`void` function, method, operator, or lambda
returns an appropriate value. The permitted top-level `main` definition may
reach its closing brace, which returns zero.

A by-value `return name;`, including transparent parentheses around `name`,
implicitly selects move transport only when `name` denotes a whole local or
by-value parameter, the source and result types are exact, and the concrete
type is movable but not copyable. The source becomes unavailable on that path.
A copyable source still selects copy transport. Return conversions and fields,
indexed or dereferenced places, globals, references, captures, temporaries, and
immovable values do not gain implicit movement; their existing copy,
conversion, or explicit `std::move` requirements remain in force. Concrete
generic reanalysis makes this decision from the substituted type traits.

A void-returning function, method, operator, or lambda accepts a bare `return`
or `return expression;` with an exactly void-typed expression. Non-void values
cannot be discarded implicitly by returning them from a void body. The same
check runs after concrete generic substitution; constructors and destructors
retain their separate prohibition on return statements.

Every executable `switch` arm terminates explicitly as defined by the control
flow rules. GTI does not have implicit switch fallthrough. A payload-enum
switch without `default` covers every value only when all alternatives are
present; such a switch has no reachable unmatched path.

Non-`void` calls are non-discardable by default. `[[discard]]` permits one
ignored result; a selected `[[discardable]]` declaration permits omission under
its function/method contract. Neither suppresses the call's other checks. The
complete selected-target/virtual-contract rule is in
[execution semantics](execution.md#optional-api-results).

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
- recursive raw pointers, including `void*`, qualified pointer slots and pointers
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
value type. A `constexpr` class or struct field is also `static`. Its value is
computed by the GTI frontend and retained with its exact semantic type; the
C++ backend is not an authority for whether an expression is constant.

The evaluator accepts fixed-width integer, `float`, `double`, `bool`, `char`,
`std::string_view`, and `nullptr_t` literals; exact scoped-enum constants;
concrete fixed arrays; structural class and struct values; layout-query
constants; earlier constexpr bindings; grouping; supported scalar unary,
binary, comparison, and short-circuit logical operations; lazy conditional
expressions; read-only field and fixed-array indexing; literal-backed
string-view `size()`, `empty()`, and checked indexing; and explicit numeric
conversions plus lossless `uint8_t(char)`/`char(uint8_t)` code-unit conversion.
Integer operations use the language's checked domains.
Floating literals, arithmetic, comparisons, integer conversions, signed zero,
infinities, and NaNs use the selected binary32 or binary64 rules in execution
semantics. Evaluation has one shared 4096-step budget, including recursively
synthesized default array elements, and a 64-call-depth limit. It reports
integer overflow, integer zero divisors, invalid shifts, out-of-range
conversions, non-constant references, unsupported operations, and resource
exhaustion at source locations.

Constant string views retain their complete byte sequence, including embedded
NULs. Indexing produces one unsigned `char` code unit, not a Unicode code point;
negative indices and indices greater than or equal to `size()` fail constant
evaluation. These observers also work on constant parameters, returned views,
and structural fields. A runtime-owned view cannot become a constant merely
because its type supports these operations. Text inspection uses the same
shared evaluation budget and can form array extents, value-generic arguments,
static assertions, and compile-time branch conditions.

A free function or method may be `constexpr` when its declaration has
potentially supported value parameters, including concrete expanded packs (by value or read-only
or mutable reference) and a potentially supported value, checked reference,
or `void` result.
A generic type parameter can stand for a supported type at the
declaration. A concrete instantiation with runtime-only types remains callable
at runtime; for example, a generic by-value transfer helper can accept an owning
string without becoming eligible to allocate or destroy one at compile time.
Required constant calls must still have supported concrete types and executed
operations. Ordinary exact typing, ownership transfer, borrow checks, and
source-written reference/receiver restrictions apply to every instantiation.
Concrete type and `uint64_t` value-generic arguments retain canonical declaration and
type/value identity throughout evaluation. A non-static instance method must
have a read-only, mutable, or consuming receiver of a supported structural
class or struct. Its body
may use supported automatic value bindings, exact assignment, scalar compound
assignment, increment and decrement, blocks, returns, ordinary and constexpr
conditionals, loops, switch, `break`/`continue`, compile-time conditionals,
recursion, and calls to available constexpr definitions. Calling the same
function at runtime remains an ordinary GTI call with the same language
semantics.

Read-only, mutable, or supported consuming arithmetic, bitwise, comparison, subscript, dereference, and call
operators may also be `constexpr` under those same signature and structural
receiver rules. Ordinary overload resolution selects the exact target; no
reversed operands, new conversions, or alternate lookup is introduced. Both
direct operator expressions and calls inside completed constexpr bodies use
verified MIR execution, not an AST operator-body fallback. Generic owners retain
their concrete type/value arguments. For example, a source-defined math record
can implement constexpr addition, and a read-only `std::array` can be indexed
in a required constant expression. Reference results are observed values, not
persistable constant references. Arrow and contextual-bool operators may also
be constexpr; consuming operators remain outside this profile.

Completed free constexpr functions also support local fixed arrays of supported
scalar elements (including nested arrays): default or explicit construction,
by-value copies and parameters, `size()`, checked indexing, element assignment,
compound assignment, and increment/decrement. Copies remain independent after
mutation. These operations execute canonical verified MIR during semantic
constant evaluation; no runtime C++ execution or optimization is required.
Array materialization and updates share the evaluation budget.

Completed constexpr functions may bind, pass, forward, and return checked
read-only or mutable references to supported scalar, fixed-array, and passive structural
values. The ordinary loan rules still govern the exact source owner, projection,
access, and escape lifetime; returning a local reference or mutating an owner
while a reference remains live is invalid. Required constant calls must observe
constant data: a reference to a runtime variable cannot supply it. Reference
results can be read into constant values, but references themselves are not
persistable constexpr bindings or fields. This capability requires the verified
MIR executor; it does not extend the legacy AST evaluator with reference rules.
Mutable references preserve the live origin rather than a snapshot: edits by
a `constexpr void` helper or mutable method are visible to the caller. An indexed
reference retains the index chosen when it was formed, even if a separate index
binding later changes. A method may return a checked field/element reference;
the ordinary receiver or single mutable-parameter origin rule still controls
its permitted escape. A mutable reference parameter still cannot bind to a
temporary. A mutable method on an otherwise eligible fresh temporary uses its
ordinary full-expression lifetime. `void` completion is not a constant value,
and copying an aggregate does not create a mutable alias.

The source-defined mutable `std::array::at`, `front`, `back`, subscript, and
`fill` members now use these same rules. Existing reflection's `constexpr for`
and `get_mut` can compose with such helpers; reflection continues to expand and
resolve fields before constant execution. No public library spelling grants
permission to mutate. Stored-reference wrappers, raw addresses,
and host/runtime calls remain outside this constant-execution
profile. Calls requiring these new mutation rules cannot retry an unsupported
MIR operation in the compatibility AST evaluator.

Ordinary payload-free scoped enums participate in this MIR constant profile:
enumerator construction, exact-type comparisons, enum `switch`, local/field/
array assignment, value copies and checked references compose with mutable
builders. Constants retain their enum declaration identity and exact signed or
unsigned underlying domain; equal discriminants do not erase distinct types.
This does not introduce integer-to-enum conversions or payload-enum execution.
`expected<T, E>` and `unexpected<E>` compose when their concrete alternatives
are supported passive constant types; `T` may also be `void`. Success and error
construction, state tests, `.value()`, `.error()`, `.value_or()`, prefix `?`,
value copies, assignment, fields and fixed arrays use the verified MIR profile.
Only the active alternative is present. Inactive extraction is a constant
evaluation diagnostic, while propagation preserves the exact error value and
type. `value_or` evaluates its fallback eagerly, as at runtime. Checked-integer
helpers use this same representation rather than a separate result evaluator.
Owning alternatives may be transient within a constant request, with extraction
and cleanup retaining the exact payload lifetime. Borrowed expected payloads
remain outside the profile; this is not a general exception or panic interpreter.
Checked enum references in this MIR profile designate local/parameter/receiver
storage. It does not add general constant execution of references to global
objects or change the existing compatibility evaluator's read-only calls.

Completed free constexpr functions may also transform fully initialized passive
structural records supplied by value: copy into a mutable local, mutate fields
(including nested records and fixed arrays), and return the independent value.
The same no-base, no-polymorphism, no-stored-borrow, no-active-cleanup boundaries
apply. Field selection uses exact declaration identity, not field spelling or
native layout. Completed function closures also execute explicitly constexpr
constructors with selected field initializers and read-only or mutable constexpr
methods through MIR. Declaration defaults and nested source constructions use
the same initialization schedule; eligibility is never inferred from a generated
native constructor.

An ordinary constructor with a source body may be `constexpr`. The constructed
class or struct must have no base, polymorphism, union state or stored reference,
and every recursively reachable field must have a
supported constexpr value type for compile-time construction. Generic
constructors and methods can also be instantiated with runtime-only
field types for ordinary runtime use, under the same declaration-versus-call
distinction. Each field must be fully initialized by its
declaration or one exact constructor initializer before the body runs. The
verified-MIR profile permits local computation and checked mutation of mutable
fields after initialization, including loops and pure helper calls. An explicit
member initializer overrides its declaration default: the unused default is
not executed. Generated default constructors are eligible when the ordinary
selected field schedule is itself eligible. Constructor parameters may be
checked read-only or mutable references, and default arguments retain ordinary
call semantics. Mutable reference writes affect the caller's live origin; they
cannot be implemented by copying argument values. A control transfer out of the constructor remains
invalid. A constexpr instance method may subsequently observe or, with a
mutable receiver, edit the exact stored fields. New special-member policies,
generic packs, and polymorphic dispatch remain outside this subset.

`if constexpr (condition)` requires the frontend to compute one `bool`. Only
the selected branch is semantically analyzed and lowered; the discarded branch
cannot contribute diagnostics, symbols, calls, ownership effects, HIR, MIR, or
emitted C++. Native C++ constant evaluation is not used to choose the branch.
When a condition depends on an unresolved generic type or value parameter,
primary analysis records a deferred selection and inspects neither arm.
Ordinary name/type checks and independent static assertions outside those arms
still run. Branch-dependent return, movement, receiver-restoration, and loan
flow must be checked again for each concrete instance; the symbolic primary is
not an executable flow proof. A concrete instance must select every branch
before HIR lowering. Nested constexpr predicates may themselves require staged
concrete analysis; their execution shares the enclosing constant request's
step budget and a bounded staging/call depth.

Concrete non-negative integer constexpr values may supply fixed-array extents
and `uint64_t` value-generic arguments. These uses refer to the declaration's
computed value rather than reinterpreting emitted C++.

Aggregate constants retain their complete concrete array or class type,
including generic type and value arguments. Scoped-enum constants retain their
nominal enum identity as well as their underlying value; equal discriminants
from unrelated enums are not interchangeable. The backend may materialize a
frontend-computed structural value, but it may not replay constructors or use
native C++ constant evaluation to decide that value.

### Transient allocation and destruction

Completed constexpr bodies may create exclusive owners, transfer them between
local bindings, parameters, fields and supported result alternatives, borrow
their pointees, and replace or destroy them. `std::make_unique`, `unique_ptr`
dereference/arrow, reset, swap, null comparison and contextual bool use the same
source definitions and declaration-bound capabilities as runtime. An unrelated
user-defined wrapper with owning fields follows the same rules.

Source destructor bodies execute implicitly during canonical MIR cleanup;
destructors do not need a new constexpr spelling. Executed operations must be
constant-eligible, and ordinary function calls must select available constexpr
definitions. Destruction follows verified reverse-order recipes and active
ownership transfers, including early returns and moved result payloads. A
defined failure rejects constant evaluation at the executing source operation;
it does not resume, run native C++ unwinding, or produce a constant.

Allocation identities are symbolic and confined to one evaluation request.
All allocations must be reclaimed before the outer request completes. A helper
may return an owner to another helper, but an allocation, ownership handle or
active-cleanup object cannot become a persistent constexpr binding. Return a
passive summary instead. The existing step/depth limits also cover destruction
and heap operations. Each request allows at most 1024 allocation identities and
65536 live logical storage units (one per value/aggregate node plus each text
byte), independent of target layout. Exhaustion is a semantic diagnostic.

Shared ownership, virtual/recursive owning destruction, raw-memory/provider
allocation protocols, stored-reference carriers, unions and
inherited objects require further constant-execution profiles. They are not
enabled by their public library names. These new lifetime operations never
fall back to AST execution. Example 153 demonstrates transient scratch storage
and runtime parity.

Payload enums also execute in constant contexts when every possible field has
a supported concrete type. Their exact active alternative survives construction,
matching, copying, assignment and checked borrowed observation, including when
nested in ordinary source classes. Consuming methods and enum matches transfer
their active fields using the normal ownership rules. Transient exclusive owner
payloads are permitted only within the evaluation request; persistent constants
remain passive and cannot retain references. The evaluator uses verified MIR,
including active-variant cleanup, rather than a second enum execution model.

## 3.12 Compile-Time Iteration And Reflection

### 3.12.1 `constexpr for`

```gti
constexpr for ( [ "mut" ] type IDENTIFIER ":" sequence ) statement
```

`constexpr for` iterates a finite sequence of types at compile time. The
sequence expression's type must be a class whose generic arguments are exactly
one type pack; the pack's elements, in order, are the elements of the
sequence. The sequence expression is analyzed, and its type decides the
expansion, but it is never evaluated at run time. Any such class is a
sequence; `std::meta::list<Elements...>` is the library's spelling, and a
value of a user class with a type-pack argument list iterates the same way.

For every element `E`, the body is analyzed once more as a fresh copy inside a
scope of its own, with the binding initialized to `E()`. Each copy is an
ordinary block: its locals, loans, diagnostics and `if constexpr` selections
are its own, and copies run in element order as consecutive statements. When
`E()` is a constant the bounded evaluator can produce (a scalar, or a class
with a public `constexpr` default constructor) and the binding is not `mut`,
the binding is `constexpr`, so `if constexpr` and `static_assert` inside the
body may read it; otherwise it is a plain value. `break` and `continue` do not
apply to the expansion itself; they refer to an enclosing run-time loop as
usual. `if constexpr` is the way to select elements.

A sequence type that depends on an open generic parameter defers the
expansion in primary analysis exactly like a dependent `if constexpr`
condition: no copy is analyzed, and each concrete instance expands the
statement with the substituted sequence. A declaration of a function, class,
enum, concept, alias or namespace inside the body is rejected (`GTI-S2109`);
a sequence that is not such a class is rejected (`GTI-S2107`). Expansion is
bounded: at most 1024 elements per statement and 4096 expanded copies per
declaration (`GTI-S2108`), counted separately from the constant-evaluation
step budget. A diagnostic produced inside a copy carries related information
naming the element; a diagnostic that every copy produces identically is
reported once.

Nothing of the loop survives semantic analysis except the expanded copies;
there is no run-time loop and no run-time sequence value.

### 3.12.2 Reflection Metaobjects

`<std/meta>` describes GTI declarations with ordinary values. A metaobject is
an empty value whose generic arguments carry the canonical identity of what it
describes: `std::meta::type<T>` for a type and `std::meta::field<T, I>` for
instance field `I` of `T` in declaration order. `std::meta::fields_of<T>()`
is a `std::meta::list` of the field metaobjects of a struct or class, in
declaration order, direct instance fields only; static fields and inherited
fields are not listed. Queries (`name()`, `index()`, `is_public()`,
`type()`, `type_of<T>()`, `name_of<T>()`, the `is_*` predicates and
`field_count()`) are `constexpr` methods and functions whose results are
constants of their generic arguments; a query reached with concrete arguments
is a literal at run time and a constant in constant evaluation. `name()` is
the declared identifier; `name_of<T>()` is the qualified spelling the
compiler uses for `T`.

Lifecycle and safety predicates are available both as free functions and as
methods on `type<T>`: `is_copy_constructible`, `is_move_constructible`,
`is_copyable`, `is_movable`, `is_relocatable`, `is_bitwise_relocatable`,
`is_transferable`, `is_shareable` and `is_default_initializable`. These report
the same GTI facts used to admit the corresponding operations; in particular,
construction does not imply assignment, relocation is not permission to move a
borrowed value, and transfer/share queries do not override an object's live
loans or thread-affinity rules. `is_copyable` reports copy construction and
copy assignment; the public `std::copyable` concept additionally requires
movement. Classification also includes
`is_unsigned_integral`, `is_numeric`, `is_signed_numeric` and
`is_integral_enum` (which excludes payload enums).

`is_same<T, U>()` tests exact type identity, not conversion or inheritance.
`is_enum_underlying<E, U>()` is true only for a payload-free enum whose declared
integer representation is exactly `U`; non-enums and payload enums return
false. Their metaobject forms are `type<T>().is_same<U>()` and
`type<E>().has_underlying_type<U>()`. All these answers are frontend-owned
constants, with no native type-trait evaluation or runtime dispatch.

`field.has_default()` is true when the field's declaration has an
initializer, and `field.default_value()` is a fresh value of that initializer
evaluated where the call is, under the field's type exactly as the
declaration evaluates it (GTI-S2119 when there is none). It is the only way
to reach a declared default without an instance, so a decoder can fill an
absent member from the declaration.

`field.get(owner)` denotes the place `owner.<field>`, and `field.get_mut(owner)`
the same place for writing. They are not calls at run time: semantics analyzes
the projection the call denotes, at the call site, under the ordinary field
access rules. Visibility follows the call site, so a private field is
accessible through reflection exactly where `owner.<field>` is (`GTI-S2007`
otherwise); a field that is not writable through the given owner rejects
`get_mut` (`GTI-S2112`); borrowing, mutability, moves and loan conflicts are
those of the direct access. `std::meta::field_type<T, I>` is the exact type of
the field and may be spelled in any type position; `field.type()` yields a
`std::meta::type` of it, which a generic parameter deduces from
(`decode(field.type(), input)`). A field index that is out of range is
rejected (`GTI-S2111`).

Reflection consumed at compile time leaves no run-time representation:
metaobjects are empty values, queries fold to constants, and reflected access
lowers to ordinary projections. A name used at run time is an ordinary string
literal. `@compiler_type("...")` and the `gti_internal::meta_*` operations
that `<std/meta>` is built on are compiler-bound declarations of that trusted
unit and are unavailable to application code (`GTI-S2049`, `GTI-S2058`).

### 3.12.3 Declared Attributes

A struct declared `[[attribute]]` is an **attribute type**: a non-generic
struct with public value fields, declaration defaults and no declared
constructors or bases (`GTI-S2113` otherwise). `[[ns::name(arguments)]]`
before a class-like declaration or an instance field **applies** it: the
arguments initialize the attribute struct's fields positionally, or by
designator (`[[ns::name(.key = "x")]]`), with constant expressions; a field
without an argument takes its declaration default, which must then be a
literal. Positional and designated arguments do not mix. Several applications
share one group (`[[a::x, b::y(1)]]`), a declared group precedes any compiler
attribute group, and one declaration applies a given attribute type at most
once (`GTI-S2115`). Too many arguments, an unknown designator, a non-constant
or mistyped argument, or a missing default are rejected (`GTI-S2114`). The
compiler gives an application no meaning: it records the constant object the
arguments denote and nothing else, so attribute semantics live in the library
that declares the attribute type. Declared attributes are not allowed on
local variables, functions or enumerators in this slice.

Reflection reads them: `field.attribute_count()`, `field.has_attribute<A>()`
and `field.attributes()` (a `std::meta::list` of the applied attribute
*types*, in application order), and the same three on `std::meta::type<T>`
for the declaration's own attributes. `field.attribute<A>()` and
`type.attribute<A>()` denote the applied constant: in constant evaluation they
are that object, so `field.attribute<json::name>().key` folds in `if constexpr`
and `static_assert`; at run time a member read of the result is the constant
itself (a literal of the field's type) and a use of the whole object constructs
the attribute struct again from the application's arguments by designated
construction. Reading an attribute the declaration does not apply is rejected
(`GTI-S2116`); test with `has_attribute<A>()` first.

A class or struct declared `[[reflect_private]]` lets reflected access
(`field.get`, `get_mut`) reach its private instance fields from anywhere;
direct access rules are unchanged. `std::meta::reflectable<T>` is the
compiler-owned concept satisfied by a struct or class whose instance fields are
all public, or that declares `[[reflect_private]]`; `std::meta::is_reflectable<T>()`
is the same fact as a constant. A generic that reads every field through
reflection constrains on it so a caller with private state fails at the call
(`GTI-S2029`) rather than inside the expansion.

### 3.12.4 Enumerators, Bases And Generic Arguments

`std::meta::enumerators_of<E>()` (and `alternatives_of<E>()`, the same list
read as a payload enum's alternatives) is a `std::meta::list` of
`std::meta::enumerator<E, I>` metaobjects in declaration order, for integral
and payload enums alike, generic instances substituted. An enumerator has
`name()`, `index()`, `payload_count()` and `payload_fields()`, a list of
`std::meta::payload_field<E, I, P>` with `name()`, `index()` and `type()`
(`std::meta::payload_type<E, I, P>` is the exact payload type, spellable in
type positions). `value()` is the enumerator as a constant of `E`; a payload
enum's alternatives are not integer constants, so `value()` is rejected for
them (`GTI-S2117`). `type<E>::enumerator_count()`, `is_payload_enum()` and
`std::meta::is_payload_enum<E>()` report the shape; an out-of-range enumerator
or payload index is rejected like a field index (`GTI-S2111`).

`std::meta::bases_of<T>()` is a list of `std::meta::type<B>` over the direct
bases of a class in declaration order (`type<T>::base_count()`, `bases()`);
inherited fields are not part of `fields_of<T>()`.
`std::meta::arguments_of<T>()` is a list of `std::meta::type<Arg>` over the
type arguments of a generic class or enum instance (`argument_count()`,
`arguments()`; value arguments are not listed), and
`std::meta::is_generic_instance<T>()` tells an instance from a non-generic
type. A primary template is not a type and has no metaobject, but
`std::meta::primary_name_of<T>()` (`type<T>::primary_name()`) is the qualified
name of an instance's primary without its arguments (`std::vector` for
`std::vector<int>`), and the type's own name for a non-generic type, so a
generic client can dispatch on the template a field type instantiates.

### 3.12.5 Structural Construction

`std::meta::construct<T, E, Producer>(producer)` builds one `T` from one
value per instance field, in declaration order, without a constructor written
for `T`: for every field `F` of `T` it calls `producer.produce(F())` (a
generic method of the producer taking the field metaobject), which yields
`expected<std::meta::field_type<T, I>, E>`; the first failure ends
production and is the result, and the values produced so far are destroyed by
the ordinary cleanup of the values a call holds. When every field has a
value, `T` is initialized through the same generated constructor that
designated construction uses, so the eligibility is designated
construction's: a struct with public value fields and no declared
constructors or bases (`GTI-S2118` otherwise). Constructors and invariants are
never bypassed, nothing is default-constructed and then assigned, and no
storage is written before its object exists. The compiler-private step
`gti_internal::meta_construct<T>(values...)` is the selection of that
generated constructor for a pack of values; it is unavailable to application
code (`GTI-S2058`). A call `std::meta::field_type<T, I>(value)` (or any
computed type that denotes a scalar once its arguments are concrete) is the
explicit numeric conversion that scalar's own name spells, so a producer can
form the exact field value generically.

### 3.12.6 Callables, Parameters And Source Locations

`std::meta::methods_of<T>()` is a list of `std::meta::method<T, I>`
metaobjects over the methods `T` declares, in declaration order, operators and
static methods included and constructors and destructors excluded;
`operators_of<T>()` is the operator subset and `constructors_of<T>()` the
declared constructors as `std::meta::constructor<T, I>`. A method reports
`name()` (an operator as its spelling, `operator==`), `index()`,
`is_static()`, `is_operator()`, `is_constexpr()`, `is_virtual()`,
`is_mutating()` (non-read-only receiver), `is_public()`,
`is_consuming()` (an `&&` receiver), `is_unsafe()` (caller obligation),
`is_generic()` (method-owned generic parameters), `is_constrained()`
(method requirements), `parameter_count()`,
`parameters()` (a list of `std::meta::parameter<T, I, P>` with `name()`,
`index()` and `type()`, backed by the computed `parameter_type<T, I, P>`)
and `result()` (a type metaobject of `return_type<T, I>`); a constructor
reports `index()`, `parameter_count()` and `parameters()` likewise. This is
metadata only: invoking a method or constructor through a metaobject is a
separate capability that this slice does not provide. Indices out of range
are rejected like field indices (`GTI-S2111`).

`std::meta::callable_return_type<F>` is a computed type projecting the exact
declared result of a function signature, a selected GTI function item, a native
function pointer, or a lambda type. It preserves `void` and reference results.
In a generic declaration the query remains dependent until `F` is concrete;
for example a source wrapper can declare
`Box<std::meta::callable_return_type<F>> map<F>(F& mapper) &&`.
The operand of this query is metadata, not a stored or returned callback.
It does not select an overloaded `operator()` on a class: use the declaration
reflection above for that purpose. Unsupported concrete operands receive
`GTI-S2023`.

The query neither invokes the callable nor changes its calling contract.
Native invocations still need `unsafe`, and projected reference results still
need the same origin, access and lifetime proof as an explicitly spelled
reference. It does not provide a reference origin or authorize capture escape.
See `examples/167-callable-result-types.gti` for an unrelated source-defined
container using the projection.

Where the frontend retains a source position, metaobjects expose it as a
constant: `line()` on type, field, enumerator, method and constructor
metaobjects, and `file()` on a type (the path the compiler names the unit
by). These are the diagnostics' locations; no stronger guarantee is made.

## 3.13 Static Assertions

`static_assert(condition);` and `static_assert(condition, "message");` are
compile-time declarations admitted at namespace, complete class-like, and block
scope. The condition must resolve to exact `bool`; GTI does not apply integer
truthiness or contextual conversion. The optional message is one ordinary
quoted string literal.

An active assertion is evaluated by the same checked, step-bounded and
depth-bounded constexpr evaluator used by constexpr bindings and
`if constexpr`. A `true` result records a passed semantic fact. A `false`
result emits `GTI-S2079` at the condition, with the optional decoded message.
An unsupported constant operation or runtime dependency uses the existing
constexpr-evaluation diagnostic family. Assertions in inactive configuration
branches and discarded `if constexpr` branches are not evaluated; ordinary
runtime reachability does not suppress them.

An assertion that depends on an enclosing generic type or `uint64_t` value
parameter is deferred during symbolic analysis. It is reanalyzed and evaluated
once for each canonical concrete class, function, method, or constructor
instance. Repeated uses of one instance do not duplicate a failure. A concrete
failure retains the generic declaration as its primary location and reports
the substituted arguments plus the first requiring use as related information.
Concrete evaluation sees the completed program: a dependent assertion may call
a constexpr definition declared after it, in a class exactly as in a function.
An assertion may require the instance that owns it, as when a class assertion
or a destructor reflects on the class's fields, and is still evaluated once
rather than recursively.
The same holds wherever the instance is formed, including inside a constant
evaluation or a global initializer.
A failed dependent assertion states why its concrete instance is invalid, so
that instance reports only static assertions, not the errors that follow from
the broken precondition. For a class assertion this covers the instance's
fields, constructors, destructor and methods, such as a `void` field; for an
assertion in a function, constructor or destructor body, the rest of that
body. A library can therefore reject a type argument with one message, as
`expected` does for a `void` error type.
An independent assertion inside a generic declaration is still evaluated
immediately, and an unused genuinely dependent assertion does not fail solely
because it remains symbolic.

Assertions introduce no binding or scope. Names in their conditions create
ordinary semantic occurrences for tooling. A successful assertion is erased
before HIR, so MIR, optimization, runtime behavior, native headers, and the C++
backend contain no translated source assertion. Compiler-generated backend
integrity guards remain separate representation checks and are not GTI
language evaluation.

## 3.14 Static-Semantic Gaps

The following require later normative sections rather than inference from the
current implementation:

- additional associated-type/range and heterogeneous accumulation contracts,
  plus a general expression-requirement model; existing complete-range and hash
  capabilities are not missing foundations;
- broader lifetime/return-place relationships beyond the implemented nominal
  multi-origin aggregate and explicit borrow-summary profiles;
- general place movement, partial initialization, and reinitialization;
- full owned-callable composition beyond direct by-value generic parameters
  and nominal generic owning-carrier parameters,
  per-element by-value pack transport, exact concrete field ownership, local
  closure movement, and explicit owned move-capture, including general pack
  template-shape composition, broader erased allocation, and general function-pointer values;
- constexpr execution beyond the bounded concrete-generic, scoped-enum,
  fixed-array, result composition, checked reference/receiver mutation and
  transient exclusive-allocation profiles: persistent allocation, raw/provider
  storage, shared ownership, payload enums and polymorphism remain outside them;
- audited expansion beyond the bounded scalar, counted-text-input,
  `[[c_abi]]` record, pointer-only `[[c_opaque]]` handle, recursive raw-pointer,
  pointer-plus-count, and exact registered callback C surface, including
  array parameters, foreign-thread callbacks, and annotated ownership
  transfer; and
- native record field families beyond the closed scalar, nested-record,
  recursive-pointer, fixed-array, and callback-field set, including C
  ABI unions, packing, and bit-fields; record initialization policy remains in
  safe wrappers or native factory functions rather than in the representation
  declaration.
