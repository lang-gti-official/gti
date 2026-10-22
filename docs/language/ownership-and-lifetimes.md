# Ownership, References, And Allocation

Status: Current implemented ownership and lifetime contract. Remaining work is
tracked in [`docs/plans/roadmap-to-1.0.md`](../plans/roadmap-to-1.0.md).

GTI exposes explicit ownership separately from its bounded raw-pointer escape
hatch. Raw pointers exist for native interoperation and audited low-level
wrappers, but they own nothing and never replace the language's move, borrow,
or deterministic cleanup rules. Source-level `new` and `delete` remain absent.

## Public Ownership Surface

An initialized local `auto& name = expression;` requests a read-only checked
reference with an inferred exact referent type. `mut auto&` requests mutable
access without upgrading a read-only initializer. Both use the explicit
reference contract: the same owner/place dependencies, reborrow restrictions,
last-use endpoints, escape checks and cleanup obligations apply. Only the
written type is inferred; ownership is not. Fresh owned temporaries cannot
initialize a retained reference, while a temporary `expected<T&, E>` may
publish a borrow of its stable external owner through `?` or `.expect()`.
Plain `auto` remains a value binding and can therefore reject a move-only
copy that `auto&` would validly borrow. Whole-array and raw-pointer referents
remain outside ordinary local reference support in either spelling.

The implemented unique ownership surface is:

```gti
#include <std/memory>

std::unique_ptr<Entity> entity = std::make_unique<Entity>(arguments);
std::unique_ptr<Base> erased = std::upcast_unique<Base, Entity>(
    std::move(entity));
```

These names are standard-library API, not keywords. The current implementation
defines `std::unique_ptr<T>` as a nominal GTI class in
`stdlib/std/memory.gti`; application source imports it with
`#include <std/memory>`.
Its private field is a compiler-defined owner capability, while dereference,
member access, boolean conversion, null comparison, movement, and destruction
use the ordinary class, operator, and lifecycle systems.

`std::make_unique<T>(arguments)` is a variadic GTI standard-library function
that forwards into the private allocation capability and returns the nominal
wrapper. It uses ordinary generic overload resolution and is not a compiler
intrinsic. Typed HIR monomorphizes its concrete callable instance and validates
the nested allocation and constructor call, reporting both the stdlib body and
the application instantiation site when that use is invalid. The backend invokes
the resolved stdlib function rather than replacing the public API with a backend
allocation call. `std::make_shared` is also implemented, with explicit strong
cloning and read-only shared pointee access as described below.

Scalar allocation admits the fixed-width integers, `float`, `double`, `bool`,
`char`, plain enums, raw pointers, `nullptr_t`, and named native callbacks.
One initializer must have the exact pointee type; allocation introduces no
implicit numeric or callback conversion. With no initializer, integers,
floating values, booleans, and characters become zero, and raw pointers become
null. Plain enums, `nullptr_t`, and callbacks require an explicit value under
their existing GTI initialization rules. Allocating a raw pointer owns only the
stored pointer value, never its pointee, and does not relax raw-pointer or
reference restrictions. Arrays and other compound scalar-like values are not
admitted by this allocation slice.

`std::upcast_unique<Base, Derived>(owner)` is an explicit, consuming ordinary
stdlib operation. Semantics admits it only when `Base` is a public base of
`Derived` and has polymorphic destruction. The private owner representation is
converted without exposing an address or creating a second owner. Ordinary
calls still perform exact matching: passing `unique_ptr<Derived>` where
`unique_ptr<Base>` is required does not trigger this conversion implicitly.

The safe ownership surface does not provide:

- ownership inferred from a raw `T*` value;
- integer-to-pointer or pointer-to-integer conversions;
- construction of an owner from an address;
- unchecked ownership casts;
- `unique_ptr::release()`;
- source-level `new` or `delete`.

Recursively nested `T*` and `const T*` values and their lexically gated operations are
specified separately in [`raw-pointers.md`](raw-pointers.md). They are
non-owning address values, not another ownership category.

## Capability Layers

GTI's ownership and storage capability model is designed in three layers. The
adopted concurrency model adds transfer/share facts to this same semantic
authority; it does not create a second ownership or borrow checker. The
language boundary is summarized in
[Concurrency Transfer And Sharing](#concurrency-transfer-and-sharing), with
rationale in
[ADR 008](../decisions/008-safe-concurrency-memory-model.md):

1. Ordinary application code uses safe standard-library classes such as
   `std::unique_ptr<T>`, `std::storage<T>`, `std::allocator`, and the first
   source-defined `std::vector<T>`.
2. Public policy is implemented in ordinary GTI over general layout, lifetime,
   checked borrowing and narrow irreducible owner/text capabilities. Sparse and
   prefix slot bookkeeping and allocation policy are source-defined; their
   former private storage engines are retired.
3. Lexical `unsafe {}` permits recursively nested raw-pointer operations
   needed by native wrappers and low-level code without making private
   `gti_internal` capabilities public.

The safe allocation surface is typed and owner-returning. The separate public
unsafe resource-binding and manual-lifetime surfaces define explicit validity,
aliasing, initialization and matching-release obligations; they do not weaken
checked references. C++-style `new` and `delete` are not GTI syntax. See
[raw pointers](raw-pointers.md) for these public unsafe contracts.

Intrinsic capabilities are ordinary bodyless functions declared either in the
implicit prelude when shared across modules or in the one canonical
standard-library unit that owns them. Unique ownership and object-lifetime
operations live in `<std/memory>`; the public logical-bounds check remains a
shared prelude facility. Prefix allocation and live-length policy are ordinary
source classes, not prelude capabilities. Semantic registration marks only those exact
trusted declarations, and a call receives intrinsic behavior from its selected
`FunctionId`. Reusing the same spelling in application source creates an
ordinary function and grants no compiler behavior. No source attribute,
keyword, or public wrapper name selects an intrinsic.

The private owner, uninitialized-storage, and text-view types
have ordinary nominal declarations in those same owning units. Their selected `ClassId`
receives a compiler-capability kind only for the exact trusted declaration.
Application source cannot enter or alias `gti_internal`; doing so is
`GTI-S2058`, and a same-spelled application declaration never acquires the
private semantic type.

The C++ backend may implement one through C++ RAII or aligned allocation while
a future LLVM backend lowers the same operation differently. HIR retains the
operation without treating its bodyless declaration as a source function to
instantiate. Neither backend representation is part of the source language.

An intrinsic must represent an operation or invariant that ordinary GTI cannot
yet express. It may retain private bookkeeping needed to validate that
operation, but it must not expose library policy. Logical container size,
growth, free-list order, and invalidation belong to nominal stdlib types. The
source-defined sparse-storage owner keeps allocation extent and slot-live state
in ordinary fields and metadata bytes. Its safe methods expose these facts
without exposing raw mutation or allocation addresses. General unsafe typed
lifetime operations and the owner-witness bridge provide the compiler-owned
facts; a bitmap or a particular container's bookkeeping is not an irreducible
language capability.

The unique-owner capabilities used by the wrapper are:

```gti
gti_internal::unique_owner<T>
gti_internal::allocate_unique_owner<T>(arguments...)
gti_internal::unique_owner_borrow(owner)
gti_internal::unique_owner_borrow_mut(owner)
gti_internal::unique_owner_is_null(owner)
gti_internal::unique_owner_upcast<Base>(owner)
```

They provide allocation, checked receiver-tied borrows, and observation of the
handle's irreducible null representation. The upcast operation consumes one
private owner only after semantics proves a public base relationship and safe
polymorphic destruction. The source-defined `std::unique_ptr` operators decide
what null state means for boolean conversion and comparison. These capabilities
do not expose an address, manual deallocation, release, or unchecked
dereference operation.

## Concurrency Transfer And Sharing

The default executable profile is single-threaded. GTI also implements the
ownership facts, process-wide storage policy, owned managed threads, and
ordered scalar atomics with sequentially consistent defaults in its opt-in
concurrent profile. Owner-tied `std::mutex<T>` guards provide scoped
mutex-protected borrows. The unsafe `<std/cthread>` surface separately provides
registered manual workers, once initialization, manual mutexes, and manual
condition variables. Scoped cross-thread borrows, detach, thread-specific
storage, and arbitrary foreign-thread entry remain unavailable.

For a concrete type `T`:

- **transfer-capable** means that exclusive ownership or access to a `T`,
  including responsibility for its eventual cleanup, may move to another
  thread without violating memory safety or a thread-affinity contract; and
- **share-capable** means that multiple threads may concurrently hold and use
  read-only shared access to the same live `T` while its lifetime is protected.

These properties are independent of copying, movement, ownership kind,
triviality, native layout, and backend traits. A value crossing a thread
boundary must satisfy both the required concurrency capability and its ordinary
construction or move rule. Semantic analysis computes the facts for each
concrete generic instance; HIR preserves them and later operations consume
them. A public wrapper name or emitted C++ type never grants either fact.

Ordinary classes and aggregates derive both facts structurally through every
state-bearing base, field, active alternative, capture, and lifecycle rule.
Recursive nominal values use a cycle-aware fixed point. Interface-erased
values require an explicit interface capability and proof by every
implementation. Thread affinity is not visible in an integer or opaque-pointer
representation, so a nominal type may safely opt out. Declaring a cleanup body
conservatively denies automatic transfer and sharing; compiler-owned owner and
storage types use explicit generic rules instead. A positive assertion that
overrides a raw-pointer field, cleanup body, structural denial, or native-
resource restriction is an explicit unsafe promise whose author proves all
movement, access, and cleanup behavior.

GTI spells that nominal policy with C++-familiar declaration attributes:

```gti
[[no_transfer, no_share]] class ThreadAffineHandle {
  int descriptor = -1;
};

[[unsafe_transfer]] class ReviewedOwner {
  int* native = nullptr;
public:
  ~ReviewedOwner() {}
};

[[transfer, share]] interface ConcurrentValue {
  int read();
};
```

`[[no_transfer]]` and `[[no_share]]` are safe class/struct opt-outs.
`[[unsafe_transfer]]` and `[[unsafe_share]]` are positive class/struct
assertions and constitute the explicit unsafe proof; they do not require a
surrounding lexical `unsafe` block. `[[transfer]]` and `[[share]]` are valid
only on interfaces and declare requirements inherited by derived interfaces.
Every implementing dynamic type proves each inherited requirement from its
structural facts or an explicit unsafe assertion. A declaration may select at
most one policy for each capability; conflicting, misplaced, or unknown
capability attributes produce `GTI-S2059`.

An unsafe assertion can instead be conditional on one non-pack type parameter
of the same declaration:

```gti
[[unsafe_transfer(T), unsafe_share(T)]] class ReviewedOwner<T> {
  // Audited ownership, access and cleanup implementation.
};
```

`unsafe_transfer(T)` grants transfer exactly when the concrete `T` is
transfer-capable; `unsafe_share(T)` analogously requires sharing. A false
condition denies that capability, rather than falling back to structural
inference. The attributes do not constrain which payloads can instantiate the
owner: an owner of a thread-affine payload remains usable on its original
thread. Generic uses need the corresponding capability constraint before
assuming the conditional promise holds. The two capabilities are independent.

The author still audits the entire wrapper, including state not named in the
condition, native pointers, movement and cleanup. This is not a compiler proof
of the unsafe implementation. The bounded condition names a type parameter,
not a value parameter, parameter pack, arbitrary type expression, or boolean
expression. Existing unconditional assertions and explicit opt-outs retain
their meaning; duplicate policies remain errors.

The prelude exposes `std::transferable<T>` and `std::shareable<T>` as ordinary
public concepts backed by these compiler-owned facts. They constrain generic
code without making transfer imply movement, sharing imply copying, or either
fact imply the other.

The adopted initial rules are:

| Type category | Transfer | Share | Boundary |
| --- | --- | --- | --- |
| fixed-width integer, `bool`, `char`, `float`, `double`, enum, `nullptr_t` | yes | yes | Ordinary scalar semantics still apply. |
| fixed array or value aggregate | structural | structural | Every contained value and lifecycle rule participates. |
| `expected<T, E>` | structural | structural | Both alternatives participate. |
| ordinary class or struct | structural | structural | State-bearing bases, fields, captures, and lifecycle participate. |
| interface-erased value or owner | declared and proved | declared and proved | Method signatures alone grant neither fact. |
| `std::unique_ptr<T>` | structural over pointee and cleanup state | structural read-only observation only | Sharing never permits mutation of the handle. |
| callable value | structural | structural | Capture state, lifecycle, and invocation access all participate. |
| `T&`, `mut T&`, or stored borrowed-state carrier | no | no | The first thread model is owned-only. |
| `T*`, `const T*`, or `void*` | no | no | A reviewed nominal unsafe wrapper may assert a stronger contract. |
| compiler-defined atomic scalar | yes | yes | Its language operation, not representation, provides synchronization. |

`mut` grants mutation through one local access path. It does not mean atomic,
volatile, synchronized, transfer-capable, share-capable, or safe for concurrent
use. Concurrent mutation must be mediated by an operation with an explicit
synchronization contract, such as the implemented `std::atomic<T>` or
`std::mutex<T>` guard.

The executable managed-thread model consumes one exact owned `void()` task
through `std::thread` or one exact owned `Result()` task through
`std::thread_result<Result>`. The callable object, every capture, and any
result returned to the joining thread must be transfer-capable and cross the
boundary exactly once. References, borrowed-state iterators/views, raw
pointers, and borrowed captures or results cannot cross even when a native
lifetime looks long enough. A later scoped-borrow extension requires an
explicit structured join proof, a child loan and suspended parent, cleanup on
every normal and failure exit, and verified reactivation only after child
completion.

In the concurrent profile, namespace globals and static fields must be
immutable and share-capable. Ordinary mutable globals/static fields are
ill-formed; synchronized mutation uses an immutable global atomic or future
process-wide mutex value with a complete shutdown contract. The implemented
`std::mutex<T>` remains a local cleanup-owning owner and therefore does not
fill that global role. Borrowed-state and raw-pointer globals do not qualify
merely by being immutable, and cleanup-owning process-wide values wait for a
complete shutdown/foreign-thread contract. These restrictions do not change
current single-threaded mutable globals. The reference compiler reports
`GTI-S2060` at the process-wide binding when concurrent selection rejects
mutability or a non-share-capable concrete type. Structural aliases and
concrete generic instances use the same resolved type fact; explicit nominal
opt-outs are
reported as related causes when available. The profile-independent `GTI-S2061`
cleanup restriction runs first, so a cleanup-owning global/static does not also
receive a concurrent-profile diagnostic.

## Semantic Foundation

The checked AST retains an `ExpressionInfo` for every analyzed expression. It
records:

- its resolved `SemanticType`;
- whether it is a temporary value or an addressable place;
- whether access through that place is read-only or mutable;
- whether the type is a value, borrow, unique owner, or shared owner;
- whether values of the type may be copied or moved;
- whether the type requires lexical destruction.

Variables, fields, and parameters retain equivalent `BindingInfo`. These side
tables preserve source AST identities. Typed HIR copies these facts into stable
concrete value and binding instances without encoding C++ representation
choices in the frontend.

Class and struct traits are structural over their fields after generic type
arguments are substituted. An aggregate containing `storage<T>` or another
move-only aggregate is therefore itself a move-only lexical owner. Its copies
are rejected by GTI semantics, `std::move` transfers it explicitly, and its
use-after-move state is tracked through control flow. Copyable aggregates remain
ordinary copyable values.

Borrowed references, unique owners and strong shared owners are source-reachable.
Weak ownership is a separate incomplete API; it is not a prerequisite for the
implemented strong-owner cloning, movement, checked access and final cleanup.

## References

Reference syntax is:

```gti
void render(Entity& entity);
void update(mut Entity& entity);
```

`T&` is a non-null, read-only borrow. `mut T&` is a non-null, writable borrow.
A reference does not own its referent and cannot be reseated. References cannot
generally bind to temporaries or outlive the storage they borrow. The bounded
call-only read borrow below is not a local or stored reference binding and does
not extend the temporary's lifetime.

Assignment targets are checked by their resolved place category and access,
not by whether they are spelled as a variable, subscript, or call. A stable
mutable-reference result supports `owner.slot() = value` and compound updates
such as `(owner.slot()) += value`. The target, including accessor calls, is
evaluated exactly once before the right operand. Ordinary accessor calls
complete at that point: their exclusive result loan remains live through the
store, so an overlapping owner read or mutation in the right operand is
rejected. This does not grant ordinary calls the two-phase reservation used
by checked operator projections. Read-only results, owned value results,
immutable owners, and temporary mutable receivers are not writable targets.
This expression-target slice admits calls (including grouped calls) and
grouping of existing variable, field, index and dereference targets. It does
not add comma/conditional targets or whole-`this` replacement.

References may be parameters and non-escaping local bindings. They require an
addressable initializer, and mutable references require a mutable place.
Method returns may use `T&` when the returned place is derived from `this`.
`mut T&` is also available from a method with a trailing `mut` receiver when
the returned place is writable. The call result is a borrow tied to its
receiver, so it cannot be retained from a temporary receiver:

```gti
T& at(std::size_t index) {
  return this.data.get(index);
}
```

A direct ordinary constructor or by-value GTI call may instead produce a
self-contained class or struct value that is borrowed transiently as one
read-only or mutable method receiver. The compiler materializes one mutable
temporary place, begins the selected receiver borrow after successful
production, ends that borrow after invocation, and retains ownership of the
value until the enclosing full-expression cleanup. This ephemeral capability
does not authorize a stored reference, address, field-assignment target, or
mutable-reference argument. The selected method may be virtual, an override,
the implementation of an interface requirement, or inherited from a base
class: dynamic dispatch still selects the body that runs, the borrow covers the
whole temporary, and a failing override leaves the temporary to the same
full-expression cleanup. An explicit move from named storage does not gain
this fresh-temporary capability. A read-only ordinary
method on a self-contained temporary may return a read-only reference or
`std::string_view` tied to that temporary receiver for immediate nonescaping
use, such as `read(make_owner().get())` or `parse(make_text().view())`.
The returned loan ends before the temporary is destroyed, including when a
later argument or the consuming call fails. Binding that result beyond the
full expression or returning it remains rejected; no temporary lifetime is
extended. Mutable borrowed results remain outside this capability.

A fresh class or struct containing a statically proven set of stored owner
dependencies may also supply one immediate ordinary read-only or mutable method
receiver, for example `api.auth().create_access_token(...)`. Its producer must
be a directly resolved ordinary GTI constructor or function with an exact
borrow summary. Every owner must have stable storage that survives the complete
expression; the receiver method must return `void`, a non-pointer scalar, or a
fully owned value. References, string views, C strings, raw pointers and native
function values are excluded as results. This is a structural rule for
user-defined services and views, not a special case for any library type.

The compiler retains two distinct kinds of borrows: the carrier's stored
dependencies on its external owners, and the immediate receiver borrow of its own
storage. The receiver borrow ends after invocation. The carrier is destroyed
at the full-expression boundary, then its stored owner dependencies end. The
same ordering applies to argument failure, method failure, and propagated
errors. Consequently, a later argument or operand cannot read an exclusively
borrowed owner merely because the immediate service method already returned.
A mutable method on a carrier with a shared owner dependency may mutate its
own fields; it does not upgrade that shared dependency to exclusive access.

This permission does not allow retaining a reference or address to the
temporary, exporting a borrowed result from its method, moving its owner while
active, or passing the carrier as a temporary reference argument. Temporary
owners and unresolved/native/virtual producers
remain excluded. A named carrier continues to use the ordinary stored-borrow
rules below. A factory called on a complete object protects that complete
receiver under its existing borrow summary; this feature does not infer a
narrower private-field origin from the factory body.

Shared, mixed, mutable and cleanup-owning carriers use this same materialized
receiver rule. A read-only call does not upgrade any shared dependency; owned
fields are destroyed before the carrier's external loans end.

A direct ordinary constructor or by-value GTI call may also produce a fresh,
self-contained class or struct value for one exact read-only `T&` argument of
an ordinary GTI function or non-operator method. A payload enum's alternative,
with or without fields, such as `state::engaged(1)` or `state::empty`, and a
by-value GTI call returning a payload enum produce such a value too, so
`observe(make_state())` borrows the fresh enum for that call. A scalar-backed
enum is not an aggregate value and stays excluded. The compiler materializes the
owned value at its written argument position, begins one read loan, evaluates
later arguments, invokes the selected callable, ends the loan immediately
afterward, and retains the value's drop obligation until the enclosing
full-expression boundary. A by-value exact overload remains preferred because
this borrowed form is a fallback viability tier. An explicit
`std::move(local)` of a whole local or parameter is admitted the same way: the
moved value becomes a temporary owned by the enclosing full expression, and the
local is moved on every later path. The permission does not apply to `mut T&`,
local or stored bindings, moves of a projection, compound or native producers,
borrowed-state values, or a callable result that can retain the loan.

A concrete payload-enum construction with copyable, movable, trivial-drop
value traits and no stored borrows supports the same ordinary call-only
read borrow. Its selected alternative remains in the existing temporary
value storage, and its exact full-expression lifetime is checked even though
destruction does no runtime work. Enum factories, owning alternatives and
borrowed alternatives do not yet use this temporary-argument family. This is
an ordinary argument rule; protocol-generated observations use it too.

An unqualified non-static field denotes the same receiver-rooted place as an
explicit `this` projection. Consequently, both `return field;` and
`return this.field;` preserve receiver-tied reference provenance, and a
receiver-tied carrier may be returned from either `field.begin()` or
`this.field.begin()`. Normal lexical shadowing still applies: a local or
parameter named `field` is an independent place and cannot be returned under
the receiver rule.

A free function or static method may return a read-only borrowed result when
every reachable return derives from one eligible read-only parameter. A `T&`
parameter can introduce that dependency. A by-value parameter whose concrete
type is a direct one-owner borrowed-state carrier can only transfer its
existing dependency; it does not create a new runtime borrow. Concrete generic
instances are checked by the same rule, but the declaration must name the
carrier shape: an unconstrained `T` parameter does not gain this capability
from an eventual view substitution. A carrier value return must transfer that
same carrier dependency. A reference return may name only the carrier's one
designated stored referent, with the exact referent type. An ordinary field of
the by-value carrier, a projection rooted in the carrier shell, a projection
beyond the stored referent, or a referent of another type is not evidence of
the external owner and is rejected with `GTI-S2085`. The selected parameter
index is part of resolved call metadata, so callers attach the result to the
argument's owner instead of treating the returned value as independent:

```gti
T& identity<T>(T& value) {
  return value;
}
```

This is implicit single-origin lifetime inference, not a general lifetime
parameter system. Other parameters may contribute ordinary values, but a
returned borrow may not choose between owners or combine dependencies.
Explicit complete dependency contracts for reference-bearing payload enums,
described below, extend this beyond the implicit single-origin form; they do
not infer an unspecified lifetime relation from an ambiguous signature.

`expected<T&, E>` applies this same inference to a checked read-only borrow.
The success path publishes the referent together with the receiver, argument,
or global owner dependency; an `unexpected(E)` path publishes no loan. A local
complete result is itself a carrier of that dependency, and `value()` creates
a child read loan rather than copying `T`. Copying, moving, or forwarding the
complete carrier preserves the one dependency, including a by-value
parameter. Fields, globals, lambda returns, mutable reference payloads, and
multiple-owner results remain outside this bounded form. See [Expected values](expected.md#fallible-borrows).

Under the default single-threaded execution profile, a free function or static
method may instead return `T&` or `mut T&` from one exact namespace-global or
non-generic static-field place. Every reachable return must name the same
symbol root and the same field, fixed-index, or dereference projection path.
The function summary retains that place, and each call creates a checked loan
against it. Unsafe raw-pointer dereference remains lexical to the accessor body;
it does not make a successfully checked call site unsafe:

```gti
class Application {
public:
  static mut Application& Get() {
    unsafe {
      return *Application::app;
    }
  }

private:
  static mut Application* app = nullptr;
};

Application::Get().Update();
```

An unretained call-result loan ends at its enclosing full-expression. When its
only use is as the receiver of a member call whose result does not borrow from
that receiver, it ends immediately after that call instead. Independent scalar
reads such as `rows[0].first() == rows[1].second()` therefore execute in source
order without retaining the first subscript loan across the second. A result
retained in a local reference remains live to its lexical scope boundary, so a
second overlapping mutable borrow is rejected even when the first carrier is
otherwise unused. A nested block is the explicit way to end such a retained
global borrow earlier. The concurrent execution profile does not admit this
unsynchronized mutable process-wide storage: its existing `GTI-S2060` rule
requires immutable, share-capable globals/statics until an atomic, mutex, or
guard contract provides synchronization.

The same exact global or static place may be the origin of a
reference-bearing source carrier, such as a class with a reference field or a
payload enum with a reference alternative. A free or static function may return
a carrier whose complete dependency set names that place: for example, a
`lookup<int32_t&> find_global()` that returns `global_value` through an
`implicit` constructor, or `slot::item(global_value)`. Each call creates a
checked loan against that storage rather than against a parameter or local. A
reference-free alternative still carries no dependency, and the single-threaded
profile and same-path rules above still apply.

Retaining a borrow from a move-only receiver prevents moving or replacing that
receiver and calling its mutable methods while the semantic loan remains live.
A read-only loan may have multiple local aliases. Every alias is a carrier of
the same semantic loan rather than an independent hidden borrow, and every
carrier use contributes to one path-aware lifetime. The compiler may end that
loan only after the final reachable use across all carriers.

A mutable local loan may be reborrowed into a distinct mutable or read-only
child loan. The child may itself be reborrowed, and a read-only child may gain
ordinary read-only aliases of its own loan identity. A read-only loan cannot be
upgraded into a mutable child. While any child remains active, its mutable
parent remains live but suspended: exclusive access, moving, or replacing
an overlapping part of the parent or owner place is rejected. An ordinary
reference without a stored-carrier dependency can still provide read-only
access and further shared children while every overlapping child access is
read-only. This revises the earlier blanket suspended-parent rejection;
an exclusive child still rejects overlapping reads. Reading a borrowed
carrier's physical shell does not itself authorize operations on its hidden
exclusive dependencies. Known-
disjoint named-field projections remain usable through that parent, and
multiple children may coexist when their accesses are shared or their stable
places are known to be disjoint. Ending a child reactivates exclusive access
through its parent only when no other active child
remains, so the whole parent may be used again after the final child's proven
endpoint. Nested children end from the inside out.

This exclusive-reborrow slice applies only when the compiler can retain a
stable place: a local or parameter root (including the method receiver), named
field projections, constant fixed-array indices, and checked nominal
dereference projections such as `*owner`. Two such places conflict when they
have the same root and one projection path is a prefix of the other. Different
named fields and different in-range constant elements are disjoint. Dynamic
index selections remain potentially overlapping with every element of their
array. The whole root therefore conflicts with every descendant, while exact
sibling fields or elements do not conflict merely because they share an owner.
Raw pointer provenance, opaque storage sources, and captured storage do not
receive this precise treatment. Namespace globals and non-generic static data
members do have stable symbol-rooted places; a free/static return may expose
one only through the exact-origin rule above.

A receiver- or argument-tied call result preserves the stable place of the
selected source expression, so calls on `parent.left` and `parent.right` remain
disjoint. The current function summary does not infer a narrower field path
from the callee body: a result tied to a call on the whole `parent` protects the
whole parent, even when that particular method returns one field internally.
Return-place transforms through callee-internal projections remain a separate
future contract.

Within one call or construction, receivers and arguments evaluate strictly
left to right under [Execution Section 4.2](execution.md#42-evaluation-order).
A transient borrow produced by an earlier argument remains active through the
invocation and its enclosing full-expression, so a later overlapping mutation
is invalid. When the earlier transient borrow is exclusive, a later operand of
the same call may not read the borrowed owner either, even through an
ephemeral receiver access whose result carries no borrow, such as
`values.end()` or `values.size()`. The classic
`f(values.begin_mut(), values.end())` spelling is therefore a semantic
rejection pointing at the reading argument, with a note at the exclusive
argument and a hint that whole-container operations take the container
itself. A read that completes in an earlier operand does not conflict with a
later exclusive borrow, so the reversed order remains valid. A mutation that
completes in an earlier argument may be followed by a later borrow when no
other active loan conflicts. Known-disjoint origin places remain valid in
either order.

A plain overloaded subscript or dereference store over a local, parameter,
receiver, or existing loan uses one non-escaping two-phase mutable call-result
loan. Target evaluation reserves the exact stable place while leaving its
mutable parent active, so the right operand may create temporary read-only
children. The reservation itself cannot be used as a reference, stored,
returned, synchronized across, or exposed to raw memory. After every
overlapping right-operand child ends, one explicit activation suspends the
mutable parent and must be followed immediately by the committing assignment.
Failure cleanup may end an unactivated reservation without ever publishing
write authority. Ordinary retained references, symbol-rooted globals, and
calls outside this compiler-generated store schedule do not gain two-phase
behavior.

For a nested target, the reservation includes its exact unretained mutable
call-result receiver ancestry. This lets `grid[i][j] = grid[k][l] + delta`
complete read-only RHS access without making the intermediate row exclusive
too soon. Each selected target accessor still runs once, target-first. The
terminal activation requires overlapping RHS loans to end, activates the
whole chain, and commits through the exact terminal referent. Normal and
failure cleanup retire children before their receiver ancestors. Retained,
escaping, stored-dependency and uncertain receiver loans are not converted
into reservations. In a dereference-store RHS, read-only operator access is
preferred under the same rule as subscript-store RHS access; an explicit
mutable-reference context is not downgraded.

The ordered ordinary-call and constructor schedules implement this rule:
`inspect(tree.append(node), tree.lookup(index))` may complete the append before
the lookup creates its loan, including when append reallocates owned storage.
Resolved free/generic helpers preserve the same origin and order. A borrowed
carrier passed by value retains its owner dependency; moving its shell into a
parameter does not shorten the owner's loan. The reverse order remains an
error, as do mutation under an already-live lexical loan, overlapping mutable
parameters, and a later read under an earlier exclusive loan. This does not
introduce last-use ending for full-expression temporaries or borrowed results.

Reborrowing adds no syntax or explicit lifetime parameter. It uses the existing
`T&` and `mut T&` reference declarations:

```gti
mut Widget& parent = widget;
mut Widget& child = parent;
int& observed = child.value;

inspect(observed);
child.update();  // the read-only child ended after inspect
parent.reset();  // the mutable child ended after update
```

These local loan transitions do not by themselves authorize arbitrary stored
or escaping lifetime graphs. Reference fields and owner-dependent carrier
values use the bounded stored-reference contracts below; returning a local
child must satisfy the independently checked return-origin rule, not merely
the fact that it is a valid local reborrow. L-RANGE-02 separately defines
range-level and per-element reborrows: an exclusive parent covers the whole
traversal, its one mutable element child is confined to the current iteration,
and the parent fully reactivates only after that child ends.

For a supported local loan, the compiler can end it after its final
straight-line use, at a reachable `if` merge, or on a branch entry that does
not use any carrier. The planner recurses through nested `if` trees, so
invalidation inside a nested arm can receive an endpoint after that path's
final use while sibling paths end independently. A terminating arm leaves
through ordinary loan cleanup; every reachable fallthrough arm must still
agree at its merge. When a carrier predates an ordinary `while`, body-first
`do`/`while`, or classic `for`, any carrier use in that loop projects the
loan's last use to the loop exit. The loan stays active through zero or more
iterations, all backedges, and `continue`, then ends once after
condition-false and `break` paths converge. A loan created inside the body
remains per-iteration, and a loan first created by a `for` initializer ends
with the loop scope. A pre-existing loan may also end at a switch's unified
exit. When an invalidation is immediately followed by the matching `break` on
the same path, the compiler may end the loan after that path's final carrier
use and before the invalidation. MIR normalizes the other relevant outgoing
edges before they join, and its verifier requires their loan states to agree.
This is not general nested switch/loop flow, and unproven nesting remains
conservative. A nested block can always provide an explicit earlier lexical
end.

A receiver-, argument-, or global-tied call result that is consumed without
being stored ends its MIR loan at the enclosing full-expression boundary. This
includes a borrow used to compute an `if`, loop, or switch condition. Retaining
the result in a reference or borrowed-state value still uses the applicable
retained-loan rule above.

A plain assignment to a field projected from an element reached through a
mutable checked subscript confines the element's exclusive loan to the
commit. The right operand is evaluated first and its transient element loans
end before the checked access call that begins the write loan, so
`points[i].x = points[i].x + delta` on a container of class elements is valid
under
[Execution Section 4.2.1](execution.md#421-operands-calls-and-assignments)
and behaves like its cross-statement read-then-store form. A retained
overlapping borrow still rejects the write through the ordinary loan rules
above.

Nested checked unique-owner arrow chains such as `root->next->next` support
field access, replacement, and calls, retaining each intermediate borrow until
its dependent child ends. Empty intermediate owners retain checked failure
and ordinary cleanup, including an already staged replacement value.

A borrow reached through an owner stored inside a result follows the same
rules. `value()` returns a reference tied to its receiver, and an owning
handle's checked pointee is storage the receiver contains, so
`DirectPayload& payload = *opened.value();` may be retained in a local, or
returned from a function whose result borrows from a read-only
`expected<std::unique_ptr<DirectPayload>, E>& opened` parameter. Replacing,
moving or mutating the `expected` while that reference is live is rejected.
Ranges over `value()` and `error()` use the general range rules; see
[ranges over `expected` payloads](ranges.md#ranges-over-expected-payloads).

Reference globals, nested references, and references over fixed arrays or
compiler-private owner handles remain unavailable to ordinary source. A
free/static reference return is accepted only under the exact global/static
origin or one compatible reference-parameter rule: a read-only return selects
one read-only origin and a mutable return selects one mutable origin. A
non-escaping local reference may borrow the public `std::unique_ptr<T>` class
itself, but conservatively prevents transfer or mutation of that owner for the
rest of the function.

Stored-reference carriers support owner-tied values such as iterators, views,
and application service contexts. A class or struct may contain any number of
direct `T&` and `mut T&` fields, or nominal fields recursively containing such
references. Each reference-bearing field path retains its originating place
and permission. There is no shared/exclusive cardinality limit: independent
owners and proven disjoint fields may coexist. Overlapping shared dependencies
are permitted; an overlap involving an exclusive dependency is rejected.
Repeated shared origins retain distinct field identities rather than being
collapsed into one fabricated field. A direct reference must
bind from one exact reference parameter with the same access mode. A nested
carrier field initializes from one exact carrier parameter, or an explicit
tree of nominal constructor calls whose arguments terminate in exact enclosing
parameters. Each dependency keeps its original parameter, nested field path and
access mode even when constructors reorder arguments. Factories, default
arguments, argument packs and arbitrary expressions do not acquire a guessed
constructor-origin summary. An exclusive dependency uses explicit movement
when transporting an existing carrier. Neither form supports
copy or move assignment because assignment would reseat an established
dependency.

A read-only carrier permits copy construction when its ordinary fields are
copy-constructible, and move construction when they are movable. A copy
constructs a new carrier shell, copies its ordinary fields independently, and
adds aliases to the same shared owner dependencies. A carrier with any exclusive
path is move-constructible but not copy-constructible: its complete dependency
set is transferred, never duplicated. An instance method may return a carrier
whose dependency is derived from `this`; a free function or static method may return
one when compatible reference or carrier parameters supply its checked origin
set. Multi-origin returns must preserve all summarized parameter dependencies;
they cannot substitute local storage or select a different dependency set on
another return path. Passing an exact carrier by value with an explicit move
transfers its existing loans through an ordinary function just as through a
constructor. This does not exempt a live child reborrow or another overlapping
loan, and a reference parameter does not authorize such a transfer:

```gti
class Iterator<T> {
  T& current;

public:
  Iterator(T& value) : current(value) {}
  T& operator*() { return this.current; }
};

class WriteView {
  mut int& current;

public:
  WriteView(mut int& value) : current(value) {}
  mut int& get() mut { return this.current; }
};
```

That dependency set survives ordinary calls, concrete generic substitution,
explicit moves, checked returns, and lexical destruction. A helper may
therefore construct a cursor or view from one owner, pass the carrier through
a concrete move relay, and return it to the caller without losing the owner
loan. For a mutable carrier, construction suspends direct access to the exact
owner, a move transfers that suspension, and the final carrier end reactivates
the owner. A mutable method may create a call-local or retained child reborrow;
that child must end before the carrier dependency can end. Dependency
propagation follows the selected argument and is verified before backend
entry; it is not inferred from an emitted C++ reference field. See
[`041-owner-dependencies.gti`](../../examples/041-owner-dependencies.gti) for a
runnable shared factory/generic relay and an exclusive write view.

A single-path shared carrier may retain a read-only child of an exclusive
carrier or mutable reference. This downgrades access for that child, not for
the parent: the exclusive parent remains suspended until every shared copy of
the child has ended. Moving the parent or changing the original owner while
any such copy remains live is rejected. A mutable range traversing a borrowed
carrier additionally retains its receiver's physical lifetime and underlying
stored dependency; neither can end while the iterator remains live.

Nesting uses the same rule, rather than an exception for a particular library
type. For example, `std::pair<Iterator<int>, bool>` may carry one iterator and
an insertion flag, and another nominal wrapper may contain that pair. The
compiler tracks the complete field path to the reference; copying a shared
wrapper aliases its owner, while moving an exclusive wrapper transfers its
dependency. Extracting an exclusive nested field also requires a move. A
by-value carrier may return its exact externally borrowed reference field,
but cannot return a reference to its own shell or an ordinary owned field.
A read-only method may expose a shared reference through a mutable reference
field without granting the caller mutable access.

Constructors may compose these carriers directly as fresh arguments, such as
`bundle(Bundle(View(right), View(left)))`. Each result field retains the exact
external owner selected by its constructor, including reordered inputs and
shared/exclusive permissions; intermediate temporary shells do not become the
owners. A cleanup-owning fresh argument transfers its object and dependency
into the next constructor. If that construction fails, normal parameter and
subobject rollback still run exactly once before the external loans retire.
This does not permit copying an exclusive carrier, aliasing two exclusive
fields, or using an owner while a retained loan remains live. The bounded
constructor-origin proof follows ordinary constructor producers and their
prepared inputs; this change does not introduce a new escaping-call or
lifetime-annotation rule.

Composed shared carriers conservatively retain their complete dependency set
when a method exposes a reference or a subview. Such a projection does not yet
release an unrelated owner independently. `std::paired_span<L, R>` is an ordinary
source-defined client: it holds two spans, uses the shorter input length, and
offers indexed `first`, `second`, and `subspan` operations. It has no special
compiler recognition: its checked iterators and reference proxies use the same
general stored-dependency and range protocols as application-defined carriers.
Whole-carrier by-value forwarding is supported. The separately escaping
reference-return rule for a by-value carrier parameter still requires its one
exact reference path; selecting an individual path from a multi-path by-value
parameter is not part of this slice. Receiver-based accessors remain available
and conservatively retain the receiver's complete owner set.

An ordinary local, parameter, or returned reference may refer to one of these
borrowed-state objects. It protects two distinct things: the object's physical
storage and the external owners retained by its fields. For example, an
accessor returning a reference to a contained service keeps both that service
and the dependencies it borrows alive. Moving or replacing the service while
the reference remains live is rejected even if all external owners survive.
The same rule applies to unrelated user-defined wrappers and concrete generic
or lambda reference parameters; it does not recognize library names.
Constructors storing such a reference retain both dependencies too. Adapters
may be nested: borrowing an adapter retains every intermediate object's
lifetime as well as its external owners. Shared access at any layer cannot be
upgraded by a mutable outer adapter. Constructor failure releases these loans
only after rolling back already-initialized fields.

An observing enum pattern that borrows an ordinary owned payload (for example,
an error string beside a reference-valued success alternative) protects the
enum's physical storage. The enum's external dependency group is not its
parent authority. A reference to the enum itself remains the normal parent
when matching through that reference. The live payload borrow prevents moving
or replacing the enum; selecting a stored reference instead retains its exact
external owners under the separate checked selection rule.

Reference access and stored-field access are independent. A mutable reference
to a shared view can mutate its own eligible fields, but cannot turn a stored
read-only reference into a mutable one. A read-only reference to an exclusive
view cannot call its mutable methods. Copying a copyable shared view through a
reference creates a distinct object retaining the original external
dependencies, not a reference to the source object's shell. Such a copy may be
returned by value under the existing return-origin rules. Returning a reference
to a local copy or by-value parameter remains invalid; `GTI-S2017` or the
existing by-value-carrier `GTI-S2085` diagnoses the escaping storage.

No source lifetime parameters are introduced. Constructor parameter bindings
and unambiguous direct return expressions establish field-wise origin maps.
A factory may reorder its parameters or bind several shared fields to one
parameter; unused parameters do not become result dependencies. Ordinary
local carriers may compose shared and exclusive references from independent
reference parameters, including through nested carrier constructors. Those
parameters provide the initial permissions, not competing loans; the resulting
carrier still excludes conflicting use of each referent until its own borrow
ends. A live overlapping child borrow is not exempted by this rule. Ordinary
accessors and operators preserve the selected stored field's permission and
origin rather than using the carrier's first dependency. Caller metadata is
checked against the callee's actual returned origins before backend entry.
A scalar reference result that can select between several external owners
retains every possible owner, even though the reference itself has no stored
field layout. Its local aliases preserve that full origin set. Conflicting
writes to either owner are rejected while the reference is live; ordinary
last-use analysis releases those protections after its final use.
See [`109-borrowed-aggregates.gti`](../../examples/109-borrowed-aggregates.gti)
for an application context borrowing four different service types and moving
through a generic relay, then composing the same dependencies through fresh
nested constructor arguments.

Local copies made inside a borrowed receiver/reference body currently require
the same complete carrier type as that entry; a differently typed nested field
copy still needs per-copy dependency-path representation. Direct supported
field access and reference-returning accessors do not require that copy.
A direct by-value accessor may return a nested copyable shared view using its
exact field-origin contract. Calling that accessor creates an independent
view retaining the external resource, rather than a reference to the enclosing
object's shell. It does not grant mutable access to the resource. An accessor
returning a reference to an owned field or owned storage still protects the
enclosing owner; an external resource remaining alive cannot make an element
reference survive destruction or movement of its own storage owner.
This also applies when a reference is returned through a local alias or an
identity wrapper. A borrowed operator may expose a fresh temporary's inline
storage for an immediate read within the full expression; that reference
cannot be retained in a local or returned from the function. An exact
stored-field result instead retains the external referent and does not borrow
the temporary carrier's inline storage.

Inherited borrowed state, arrays of borrowed carriers,
global/static or captured borrowed storage, dependency-changing returns, and
assignment that would replace a carrier's dependency remain rejected. Mutable
dependencies are limited to an exact
stable local, parameter, receiver, or existing-loan origin. They do not admit
raw or opaque origins, dynamic-index identity, arbitrary lifetime parameters,
or an independently escaping child reference.

An ordinary borrowed class may declare a destructor. A borrowed class whose
source destructor or nested borrowed field/base can use or release a dependency
retains that dependency until cleanup finishes, not just its last explicit read. Moving
it transfers the cleanup obligation and the same dependency protection. It may
be passed by value or moved into another direct field under the existing
constructor origin contract. Failed construction rolls back initialized fields
before releasing their dependencies. These rules do not grant mutable access
through a shared reference, permit reseating reference fields, or make a
resource thread-safe. Intermediate borrowed carrier shells and their external
owners must also outlive that cleanup. Existing bounded structured last-use
rules still apply; a narrower scope can make the complete dependency endpoint
explicit. See [cleanup-bound guards](../../examples/118-cleanup-bound-guards.gti).

Generated cleanup of an unrelated owning field is not a dependency-release
operation. A record containing `int& value` and an owning error string may
relay `value` after consuming the record; destroying the string does not end
the external integer's lifetime. A source destructor, a declaration-bound
resource capability, or a nested cleanup-bound borrowed field/base still
retains its dependencies until destruction. Unknown or recursive release
shapes are conservative. Semantics records this structural distinction and MIR
independently verifies it; it is not a special case for result or view names.

The compiler-trusted mutex guard retains its separate direct exclusive-access
capability. Wrapping it in another carrier remains outside the supported
field-origin contract: its effective exclusive permission is stronger than its
declared shared resource field. General cleanup retention does not manufacture
or downgrade that synchronization authority.

Retaining one of these values creates a semantic owner loan, so the owner
cannot be moved, replaced, or used through a mutable method while that loan
remains live. Moving the carrier transfers the same loan identity; creating a
read-only alias, including copy construction of a shared carrier, adds a
carrier to that identity. Dropping one shared copy ends only that shell's
lifetime; owner protection remains until the final reachable use of every
alias. An exclusive carrier has only one transferable shell.
The same alias rule applies to a named local shared payload-enum copy when
the complete origin, access, field-path and alternative-tag set is unchanged.
It preserves every external owner, including additional owners, after the
original carrier's last use; it does not permit assignment or copying an
exclusive or cleanup-bound capability.
This does not remove child-loan ancestry: an enum carrying a class constructed
from a local child reborrow still cannot return that dependency under a
conditional borrowed-result contract, with or without an intervening copy.
Direct reference alternatives can retain an eligible caller-owned reference
through a copied return under their verified result contract.
HIR carries
proven straight-line, nested-merge, and conditional branch-entry endpoints,
along with bounded switch-exit and same-path immediate-break endpoints. MIR
records every carrier on one loan and emits an explicit borrow ending only
after the frontend-selected aggregate endpoint, or after cleanup for a
cleanup-bound carrier. These are GTI lifetime rules;
the emitted C++ reference field is only a backend representation.

Vector, string and span iterators retain checked references to ordinary
source-defined owners through the same stored-reference contract available to
application classes. A mutable range iterator carries one exact writable owner
dependency from `begin_mut()` until range cleanup. Its shell is noncopyable:
duplicating it would duplicate an exclusive capability. No special permission
to reference a private prefix-storage type is needed. Applications still cannot
name compiler-private unique-owner or primitive text capabilities directly;
their separately checked lifecycle/representation contracts are unaffected.

Restricted member `operator*`, `operator->`, and `operator[]` declarations may
return these receiver-tied references. A wrapper can provide paired read-only
and mutable receiver overloads, while semantic analysis records the selected
method and returned access mode. `operator->` performs exactly one checked
reference step; recursive C++ proxy behavior is not exposed. These checked
nominal operators remain distinct from built-in raw-pointer `*`, `->`, and
`[]`, which require an unsafe block and create no borrow or loan.

Range-based `for` uses `operator*` through the same receiver-tied rules. A
read-only declaration records a shared range loan and a loop-bounded read-only
element loan. A `mut T&` or `mut auto&` declaration records one exclusive range
loan plus one mutable child element loan per iteration. The child ends before
increment or every loop exit; while the parent remains active it rejects every
overlapping owner read, mutation, move, drop, or second traversal. Both
self-contained iterators and confined owner-tied iterators are supported.
Fixed arrays use an indexed place rather than a pointer; an owned nominal
temporary is transferred into loop storage and destroyed once after all range
and element loans end, including failure cleanup. The compiler does not expose
a raw pointer or public intrinsic for iteration; see [`ranges.md`](ranges.md).
For exact `named_expected.value()` read-only sources, the shared range loan is
instead rooted in the complete named `expected` and begins only after the
checked extraction succeeds.

## Raw Pointers And Ownership

A raw pointer is a nullable, trivial, non-owning value. It does not keep the
pointee alive, destroy it, transfer an allocation, or create a semantic loan.
The compiler therefore cannot use a raw-pointer value to reject a later move or
destruction of the pointee. Establishing that the pointee remains live and
valid is an unsafe-code proof obligation.

Binding mutability and pointee access are separate. In `mut T*`, `mut` allows
the pointer binding to be reseated; `T` remains writable. In
`mut const T*`, the pointer may be reseated but the pointee is read-only.
Raw-pointer bindings and fields require explicit initialization so an absent
address is represented visibly with `nullptr`.

Safe code may form an inert address of an admitted checked place and carry,
compare, pass, return or copy compatible raw pointers. Raw projections beneath
address formation, dereference, indexing, member access, arithmetic, and calls
through pointer-bearing C declarations require `unsafe {}`. The address alone
extends no lifetime and creates no checked loan. This gate transfers
the validity, lifetime, alignment, initialization, bounds, provenance, aliasing,
and native-call obligations listed in
[`raw-pointers.md`](raw-pointers.md) to the programmer.

The intended ownership pattern is to keep a raw handle in a nominal class,
perform native operations inside small reviewed unsafe blocks, and use the
ordinary generated move and cleanup machinery to guarantee exactly-once
release. The class owns the resource; the raw pointer field still does not.

## Ownership Transfer

An ordinary constructor creates a fresh object; initializing a binding from
that construction does not require the type to permit movement of an existing
object. Explicitly deleted copy and move constructors still reject copying or
moving a live binding. This permits stable-identity values such as `once_flag`
to be constructed locally or in permitted global storage without making them
relocatable. Existing global cleanup and concurrency restrictions still apply.

`std::move(value)` is a compiler-defined explicit move operation with familiar
C++ spelling. It accepts a named movable local value, by-value parameter, or
writable field place rooted in one of those values or in a mutable `this`.
Checked field access through `operator->` is also supported. A directly owned
fixed-array element is movable when its index is an in-range compile-time
integer value and its containing local, parameter, or writable field has an
exact ownership place. Move-only values require `std::move` at consuming calls,
initializers, and assignments. One bounded return rule is implicit: `return
name;`, with optional grouping, consumes `name` when it is a whole local or
by-value parameter, its type exactly equals the function result type, and that
type is movable but not copyable. Copyable named returns remain copies.
Conversions, fields and other projections, globals, references, captures,
temporaries, and immovable values do not receive this rule. Copyable values may
still be moved explicitly so generic code does not need an ownership-specific
spelling.

A move consumes the source binding. Any later read is a semantic error, even
for a copyable type, rather than observing a C++-style unspecified moved-from
state. A `mut` binding becomes available again after a valid plain `=`
reinitialization. A moved field becomes available after valid plain assignment
to that exact field. Sibling fields remain usable, but the moved field and its
containing value cannot be read or transferred until reinitialization. A method
cannot return while a field of `this` remains moved on any reachable path.
Immutable bindings may be consumed but cannot be reinitialized. Branches merge
value and field state and report a later read when any reachable path consumed
the place; loops conservatively account for zero or more iterations.

Ordinary instance methods and value-producing arithmetic/bitwise operators
may declare a consuming receiver with trailing `&&`, as may `operator()` and
conversion operators (`operator Target() &&`).
This is a receiver capability, not a general `T&&` type. Free and static
functions have no receiver and cannot use this qualifier. Assignment,
compound assignment, and increment/decrement retain their mutable contracts;
comparisons and `operator bool` retain their read-only contracts. Dereference,
arrow, and subscript do not gain consuming forms. A conversion, whether
`Target(value)` or a declaration's implicit conversion, selects its receiver
exactly as a member call does: `std::string text = make_builder();` consumes the
temporary, `std::string text = std::move(builder);` the moved owner, and
`std::string text = builder;` is rejected rather than consuming `builder`.

A named value never selects a consuming member implicitly. An owned movable
temporary, including a returned intermediate or conditional result, or an
explicitly moved receiver prefers an otherwise-exact consuming overload.
Examples are `make_builder().append(part)`, `base / first / second`, and
`std::move(base).append(part)`. The first `/` on named `base` selects its
read-only overload; later owned intermediates may select consuming `/`.
If no consuming overload matches, ordinary receiver-qualified fallback remains
available. Other arguments keep their existing copy/explicit-move rules.
Receiver selection adds no implicit conversions or argument ranking.
The [consuming builder example](../../examples/088-consuming-builders.gti)
demonstrates preserving a named root while reusing owned path intermediates.

The receiver may
mutate or move from its fields because the whole exact receiver place is being
consumed, and it may finish with moved fields rather than restoring a value
that is no longer available. It cannot return a reference or tracked
borrowed-state carrier. Mutually exclusive control-flow paths may each consume
the callable, but any path permitting a second invocation or later use is
rejected by the same availability analysis as an ordinary explicit move.
Inside a consuming method, `std::move(this)` explicitly transfers the whole
receiver, for example in `return std::move(this);`. It requires the complete
receiver to be available, movable, and free of tracked borrowed state or a
conflicting live borrow. Subsequent use of `this`, its fields, or an implicit
instance-method call is rejected. Moving only a field does not authorize
transferring a partially moved whole receiver. Whole-receiver transfer is not
an implicit return rule.

Consuming methods and call operators may be virtual, pure, or interface requirements.
An override must preserve the exact consuming receiver capability along with
its parameter and result types; a read-only or mutable operator is not an
override of an `&&` requirement. A concrete owned receiver may invoke the
selected virtual member after an explicit move or on an owned temporary. This does not grant ownership
to an interface reference or to a borrowed pointer dereference.
An explicitly moved nominal receiver with active cleanup is materialized as a
caller-owned temporary. Invocation aliases that value; it does not transfer it
into a callee parameter. The temporary remains alive through later sibling
evaluation until the enclosing full expression ends. Failure before or during
invocation, or during a later sibling, drains the same active obligations in
reverse construction order. A consuming operator may move from a `mut` field;
the extracted value owns its cleanup and the receiver retains only its remaining
fields. Read-only and mutable fallback overloads selected for an explicit move
operate on this same materialized value.

Inside a consuming method, immediate invocation of an owned nominal
or stored-lambda field, `std::move(this.field)(arguments)`, instead operates in
place. Its
remaining resources stay in the outer receiver through that receiver's full
expression, including later arguments and failure cleanup. Grouping the moved
callee does not change this rule. The field becomes unavailable for subsequent
use; retaining its storage does not permit another invocation. The selected
call must return an owned value or void, not a reference or tracked borrow.
This rule covers writable, ordinary field-only paths rooted in consuming
`this`, not reference fields. It also permits immediate invocation through a
transparent mutable unique-owner accessor, as in
`std::move(*this.target)(arguments)`. The compiler must prove that the exact
nonvirtual, zero-argument accessor returns the checked private unique-owner
borrow of one direct field. The owner wrapper must itself be a writable field
of consuming `this`. Its availability is consumed while its physical cleanup
stays with the outer receiver. Only the accessor's temporary result loan is
used for invocation; an unrelated live alias still conflicts. Ordinary
references, raw-pointer dereferences, local owners, and extraction of the
pointee do not gain an ownership-transfer capability from this rule.
Stored lambdas keep their exact closure identity and read-invocation policy;
fixed arguments and expanded argument packs use the same retained checkpoint.
Explicit extraction into a local remains an actual ownership transfer and
retains that local's ordinary cleanup scope.

A destructor does not acquire consuming-receiver permission. It may observe
or mutate its available fields, but cannot move a receiver field into a local
or invoke it through `std::move(this.field)`. Cleanup retains responsibility
for those fields. Moving a separate local owned value inside the destructor
remains an ordinary permitted ownership transfer.

An explicitly moved lambda with owned captures uses the same full-expression
lifetime. Its environment is moved before argument evaluation, read invocation
aliases that environment, and capture cleanup runs exactly once at the normal
or failure boundary. This also applies to an exact lambda moved into a generic
by-value parameter and invoked there: that callee's full expression owns the
receiver lifetime, not the caller's surrounding expression. Capture fields
remain immutable; consuming the environment does not make its fields mutable.

Source-defined `std::function<R(Args...) &&>` and
`std::move_only_function<R(Args...) &&>` use this rule for erased invocation.
The wrapper is invoked through an explicit move, and its remaining captures
survive until the caller's enclosing full expression ends, including later
sibling evaluation and failure cleanup. This does not permit consuming through
an arbitrary borrow. The owning wrapper profile is integrated; subsequent
inline/erasure breadth must retain the same ordered invocation and cleanup
contract rather than reopening the original L-CALL-02 implementation.

Fixed-array ownership state is tracked per constant element. Moving element
`i` leaves a different constant element `j` available, but makes the containing
array partially unavailable. Plain assignment to the same element restores it;
the whole array becomes available again only when every moved element is
restored. Branch joins and loop backedges preserve `available`, `moved`, or
`maybe moved` state for each tracked element. A dynamic index may select any
element, so it cannot be a move source and cannot read or mutate through a
possibly moved constant element. This is a safety fallback, not a backend
array limitation.

References are borrows and cannot be consumed. Globals require interprocedural
state, dynamic indexes lack an exact element identity, and an already-created
lambda capture is an immutable environment field, so those places remain
rejected as direct `std::move` sources. The explicit
`[owned = std::move(source)]` initializer instead moves one enclosing local or
by-value parameter while constructing the closure; it is not movement from a
capture field.
Fields reached through a borrowed reference are also rejected because the
owner state cannot be updated. Passing a temporary to `std::move` is rejected
because the temporary is already a value.
Direct self-move assignment such as `value = std::move(value)` is rejected
rather than inheriting backend-specific self-move behavior.

### Exact Generic Callable Transport

A free function's direct immutable by-value generic parameter may carry one
exact lexical lambda across either of these bounded ownership destinations:

```gti
T relay<T>(T value) {
  return value;
}

class callable_owner<T> {
  T value;

public:
  callable_owner(T value) : value(std::move(value)) {}
};

callable_owner<T> own<T>(T value) {
  return callable_owner<T>(std::move(value));
}
```

The caller must still pass a named lambda with `std::move`. The first form uses
the exact-return rule to consume the noncopyable concrete parameter and
preserves the exact concrete lambda as the declared generic result. A copyable
instantiation keeps ordinary copy-return behavior. The second form
constructs a concrete generic owner whose sole field has that exact substituted
type and whose constructor moves its matching parameter into the field. The
closure's move-only captures and cleanup obligation move with it; the source is
unavailable and the final owner cleans the environment exactly once.

This is not lambda-result inference or type erasure. A transported lambda must
be movable. Checked borrowed captures retain their dependency places and access
through concrete generic transport and field storage, as described below;
unreviewed raw pointer captures remain unavailable. A class's effective
`unsafe_transfer` assertion is an
explicit unsafe boundary for its internal raw representation, including when
that class is nested inside another owned capture. Its author must guarantee
that movement preserves the represented ownership and pointer validity through
final cleanup; this is not inferred from a destructor or a library name.
A false conditional assertion, `unsafe_share` alone, or a tracked borrowed
field cannot provide that permission. Direct raw-pointer captures remain
rejected. This revises the original blanket recursive raw-field rejection so
source-defined owning types retain the same transport as private owners.
Example 55 demonstrates a source-defined string capture; unrelated reviewed
owners have the same rule. The examples above describe the original
exact-transport minimum, not a restriction to one-field owners: ordinary direct by-value generic
parameters, concrete field storage, final value packs, and explicit interface
erasure also support source-defined owning callback wrappers. Each concrete
body must retain its exact callable identity, ownership, and cleanup schedule.
Fresh-lambda inferred returns, implicit ownership copies, and namespace/static
closure storage remain outside this contract. Checked borrowing and signature
erasure are separate general facilities, not implicit owning transport.

A raw pointer transports an address, not the closure object it may name. It
holds no capture and invokes nothing, so passing `holder<Callable>*` to a
generic helper creates no callable argument boundary, and a raw pointer
member of a type argument does not either. General object-lifetime helpers
such as `std::construct_at`, `std::assume_ref`, `std::destroy_at` and
`std::layout_of` therefore accept closure-bearing objects. Reaching the object
again keeps its explicit unsafe obligation, and the checked reference that
`assume_ref`/`assume_mut` return borrows a closure-free owner witness. A
reference to a closure-bearing object is still a callable boundary. Direct
`F&`/`mut F&` parameters now admit visible, proven confined invocation and
forwarding, using ordinary checked loans and retaining caller ownership.
An otherwise unclassified `int peek<T>(T& value) { return 1; }` still rejects
a lambda argument with GTI-S2046 because it establishes no callable contract.
An exact checked-reference relay may instead preserve the parameter's borrow
without invoking or owning the callable. Its checked return-origin contract,
not a public helper name, establishes that boundary.
[`136-inline-closure-cells.gti`](../../examples/136-inline-closure-cells.gti)
keeps a closure in an application-defined inline cell this way.

The semantic model records which immutable bindings are explicitly moved. The
C++ backend must not equate those bindings with physical C++ `const`, while HIR
represents the operation directly as `Move` rather than an ordinary function
call. These are backend contracts for a source-level rule, not C++ semantics.

The nominal wrapper may be used as a local binding, parameter, return value,
class field, fixed-array element, or non-escaping local reference. A class or
array containing it becomes move-only through structural field traits. Unique
owners remain unavailable as globals. Fixed generic functions, methods,
classes, and constructors may use move-only type arguments because typed HIR
rechecks each concrete body after substitution. Copies remain errors; calls
and initializations still require `std::move` for named move-only values,
while exact whole locals and by-value parameters may move implicitly on
return. Typed HIR also retains concrete variadic
pack elements. A pack containing any move-only element is consumed as one unit
by its first `values...` expansion, so forwarding it again is a use-after-move
error. Copyable packs may still be forwarded repeatedly; individual pack
elements cannot yet be named or moved separately.
Exact generated copy/move construction through a generic helper such as
`T construct<T, Args...>(Args... args) { return T(args...); }` follows the same
rule: a single move-only `T` is moved from its pack once, while a copyable `T`
is copied and may be constructed again from that pack. This is ordinary
generated transfer, not a user-defined move constructor or forwarding-reference
facility.
Mutable-reference elements likewise permit repeated exact forwarding: each
element retains an exclusive entry capability over its caller-owned referent,
not ownership of that referent. Ordinary call-site exclusivity rules reject
overlapping mutable/mutable and mutable/read-only inputs before the pack is
formed. Forwarding does not make the immutable grouped binding reseatable.

### Borrowing Captures And Callable Views

An explicit `[&value]` capture takes a shared/read-only borrow of a named,
addressable local or parameter. `[mut &value]` takes an exclusive mutable
borrow and requires a mutable binding and mutable invocation of the closure:

```gti
mut int count = 0;
mut auto increment = [mut &count](int amount) -> void { count += amount; };
increment(2);
```

These are checked references, not raw pointer captures. Each environment field
retains its origin place, access mode and parent loan. Multiple captures and
disjoint fields use the same place-overlap rules as ordinary references. Copy
and move captures retain any checked dependencies already present in the
captured value. Shared environments can be copied when all their other fields
can; an exclusive borrowed capture makes the environment noncopyable. Moving
the environment transfers its dependencies without moving their referents.
Borrowed environments cannot be assigned or relocated as independent objects.

The restrictions last through the last proven use, or through cleanup when
the stored type's destructor needs the dependency. An ordinary nested block
can be the final-use endpoint after its cleanup. Loop backedges and later uses
keep the dependency live. Concrete generic parameters and user-defined fields
can store these environments, but cannot let them escape their owners. The
owning `std::function` family does not acquire permission to erase borrowed
state, nor can a borrowed environment cross a managed-thread boundary.

Reentrancy follows exclusive access rather than a special callback flag. An
invocation borrowing a mutable environment conflicts with another live mutable
access to the same environment or captured origin, including another argument
of that invocation. Shared/read-only calls may overlap when all dependencies
are shared. Native callback retention and thread-confinement obligations are
unchanged; a scoped callback is not an automatically safe native registration.

`<std/functional>` provides source-defined, non-owning
`std::function_ref<R(Args...)>` and `std::function_ref<R(Args...) mut>`.
Construct a view from a named callable of the exact signature. A read view
borrows it read-only; a mutable view borrows it exclusively and requires a
mutable view binding for invocation. Neither construction allocates or copies
the callable. There is no empty/null/default view. The callable and everything
it borrows must remain valid for all uses of the view.

A signature whose result is a read-only reference carries a result-origin
contract inferred like a free function's: exactly one read-only reference
parameter of the signature owns the result, and a reference to an erased
object is never an origin (a typed referent is reached through it only by
exact restoration). The callable a view or invocation pointer is built from
must publish the same origin, and each invocation's result borrows the
argument at that position:

```gti
int& head(std::vector<int>& values) { return values[0u]; }
std::function_ptr<int&(std::vector<int>&)> first{head};
int& front = first(values);   // borrows `values`, not the pointer
```

An ambiguous signature (two eligible parameters), a mutable reference result
and a borrowed-state class result are rejected before the view exists.

#### Lambda Reference Results

A lambda may return a read-only reference. Its origin is inferred by the same
rule: exactly one read-only reference parameter owns the result, every
`return` must derive from that parameter, and captures are never result
origins. A result that derives from a mutable parameter, an ambiguous
parameter list and a return that derives from a local, a copy or a capture are
rejected; a lambda's explicit return type cannot spell a mutable reference. A direct
invocation, a generic `F`/`F&` relay and a signature-erased view all give the
result the borrow of that argument:

```gti
auto same = [](int& value) -> int& { return value; };
int& view = same(cell);              // borrows `cell`

[[borrowed_from(value)]]
int& relay<F>(F& callback, int& value) { return callback(value); }
```

A generic relay names the parameter its result borrows with `borrowed_from`;
each concrete instance verifies that the selected callable's own origin
reaches exactly that parameter. On a declaration whose result type is a
generic parameter, `borrowed_from` binds only the instances whose result is a
checked reference and may name a final parameter pack, in which case the one
reference element of the expanded pack in that instance owns the result.
`std::function_ref` and `std::function_ptr` declare their call operators this
way.

A return type obtained from an associated type may itself be a reference.
With an explicit, verified `borrowed_from` contract, that exact reference type
does not require appending a second `&` to the projection spelling. The same
instance checks prove its origin and access; generic locals and parameters
retain their existing explicit reference-declarator rules.

The named parameter may also be a by-value carrier with exactly one stored
checked dependency. In that case the result must relay that external owner:
returning the carrier's own fields, a different parameter's owner, or a local
is rejected. Consuming or destroying the carrier does not destroy its external
owner. This same declaration-relative contract is available to protocol
operations; it is not special treatment of any standard-library wrapper.

For a reference-bearing payload enum, the named parameter may carry several
checked external dependencies. Its complete contract preserves every active
field path, alternative guard and access mode. Selecting one reference may
leave several possible external origins at the call site; all remain protected
until that scalar reference's endpoint. A consuming match retires the old
carrier group only after the exact selected references acquire their successor
authority. This does not authorize a reference to the consumed shell or an
ordinary owned payload, nor does it infer a lambda lifetime contract beyond
the lambda rules above.

Two general facilities support this ordinary source wrapper:

- `std::erase_ref(object)` and `std::erase_mut(object)` return checked
  `std::erased_object&` and `mut std::erased_object&` respectively. Erasure
  preserves lifetime/access dependencies. The referent cannot be constructed,
  read as a value, or used to acquire transfer/share capability.
- `std::restore_ref<T>(erased)` and `std::restore_mut<T>(erased)` require
  `unsafe`: the caller must supply precisely the original type. Shared erasure
  cannot be restored with mutable access. Restoration retains checked lifetime
  dependencies; `unsafe` does not make them disappear.
  When the restored object itself contains borrows, those field dependencies
  remain conservatively bounded by the erased reference's checked witness.
  Ordinary helper functions can forward that restoration contract when every
  return is proven to restore the same opaque input. A matching signature
  alone does not grant restoration authority.
  A named restored-reference local may be returned only while retaining the
  exact selected reference-parameter witness and access permission, without
  another local-owned or cleanup-bound dependency. This does not authorize
  general local child-reborrow escapes or returning a local object's storage.
- `std::function_ptr<R(Args...)>` is a non-null, typed GTI invocation pointer
  constructed from an exact namespace-function item or captureless lambda.
  It preserves GTI's failure and argument/result ownership convention. It is
  not a C ABI pointer and cannot turn unsafe, receiver-dependent or
  caller-context-dependent functions into ordinary safe targets. In this
  slice an invocation pointer accepts arguments of any value type, including
  owning class values, and returns scalar, reference, view and `void`
  results; an owning class value returned through the pointer is not yet
  representable in the backend and is rejected before native compilation.

The reference projection and invocation pointer capabilities are bound to
trusted declarations; no public wrapper spelling has compiler meaning.
Captureless thunks may expand a surrounding generic parameter pack. Their
concrete signature and each forwarded reference/value are checked normally.
[`156-borrowing-callbacks.gti`](../../examples/156-borrowing-callbacks.gti)
demonstrates direct view construction and unrelated user-defined callback
storage.

### Stateless Namespace Function Items

A selected namespace free-function item owns no capture
environment, storage address, or native resource. Its nominal identity and
exact signature are compile-time state, so it is trivially copyable, movable,
destructible, transfer-capable, and share-capable. An unambiguous eligible name
can initialize a local, pass through generic values and fields, be copy-captured,
and be returned with its exact type. Copies are independent zero-state values;
explicit moves still obey the ordinary moved-binding rules. Signature reference
types describe invocation arguments, not stored borrowed state. The existing
managed-thread boundary retains its once-task contract. None of this exposes a
native address or allows an unsafe function to become a safe callable.

Contextual function names can also fill individual positions in a generic
parameter pack, including after a fixed parameter or another pack element.
Each name retains its own exact nominal identity. An unconstrained pack still
requires one eligible unambiguous function; its position does not provide an
overload preference or permit an unsafe function to become safe.

### Shared Ownership

`std::shared_ptr<T>` uses explicit `clone()` to add a strong owner of the same
object and compiler-controlled movement to transfer one handle. It is not
implicitly copyable and does not require custom copy/move constructor bodies.
Default/null construction produces an empty handle. Reset, null assignment,
move assignment, and swap change a mutable handle, not the shared object.
Dereference and member access return read-only borrows, even from a mutable
handle. Such a borrow protects the specific handle that supplied it; resetting
a different alias is allowed because the borrowed handle still owns the
object. `use_count()` is an observational snapshot, not exclusive-access proof.

`std::make_shared<T>(args...)` uses the existing unique-allocation family:
classes, structs, and scalars without stored borrowed state. Direct arrays,
Expected values, and payload enums are not in this allocation family. The
current representation allocates the object and its control block separately.
Failure to allocate the
block raises an aborting `allocation_failure` report in `shared_owner`; no
pointee, argument, local or managed-worker cleanup runs before process abort.
Successful publication still transfers exactly one ownership obligation. Last-owner
release executes the ordinary verified cleanup recipe, including defined
failure propagation. Empty dereference/member access raises `empty_owner_access`.

`shared_ptr<Derived>::upcast<Base>()` and the equivalent
`std::upcast_shared<Base, Derived>(owner)` explicitly consume one derived-typed
handle and return a base-typed handle. The target must be a public polymorphic
base. Existing aliases retain their original static type, all handles retain
one control block and one strong count, and the conversion itself neither
allocates nor introduces a failure edge. This is the ownership mechanism for
heterogeneous shared interfaces such as logger sink collections; a public
`sink_ptr`-style name is an alias or spelling of `shared_ptr<sink>`, not a
second reference-counted pointer type. Ordinary calls do not perform this
conversion implicitly.

Shared handles can cross threads only when `T` is both transfer-capable and
share-capable: another thread can perform the last release. Atomic reference
counts do not make the pointee's ordinary mutable methods safe. Synchronized
read-receiver APIs such as `atomic<T>` retain their own checked permission.
A mutex derives both transfer and sharing from its payload's transfer
capability: the lock serializes payload access, and the payload may ultimately
be destroyed on another thread. Consequently a shared mutex owner can cross
threads when its payload transfers. A lock guard remains non-transferable;
neither allocation nor shared reference counting overrides thread affinity.

Shared ownership does not solve cycles. Weak observation remains incomplete;
strong ownership cycles must be avoided.

## Nullability And Allocation Failure

Owning pointers may be empty, including after movement. Boolean and `nullptr`
comparisons inspect that state. Dereference and member access on an empty owner
produce a defined GTI runtime failure rather than undefined behavior.
For `std::unique_ptr`, both read-only and mutable access now follow explicit
MIR failure edges: initialized live values are cleaned before status 70 and
`GTI-R0008` are reported. The original trusted `<std/memory>` operation owns
the source site and `dereference` or `member_access` detail. A failed checked
destination also cleans a completed, unpublished right-hand value.

An owner may be initialized directly from the exact `nullptr_t` literal and
may be reset or assigned `nullptr` later:

```gti
mut std::unique_ptr<Entity> entity = nullptr;
entity.reset();
entity = nullptr;
```

Initialization selects the source-defined
`std::unique_ptr<T>(nullptr_t)` constructor. Assignment selects the
source-defined exact `operator=(nullptr_t)` overload. `reset()` and null
assignment destroy a present pointee exactly once and leave the owner empty;
repeating either operation on an empty owner is valid. The owner remains
move-only and its ordinary scope cleanup remains deterministic.
An explicit field constructor initializer such as `Node() : child(nullptr) {}`
uses the same target-typed construction rule as `child = nullptr` on the field
declaration. It does not turn an ordinary call's `nullptr` argument into an
implicit owner conversion.

This is not a general implicit class conversion. In particular, an ordinary
call such as `consume(nullptr)` does not construct a `unique_ptr` argument;
call arguments still require an exact parameter type.

GTI implements the Rust-style allocation-failure model adopted by
[ADR 022](../decisions/022-safe-typed-allocation.md#revision-rust-allocation-failure-model-2026-10-09).
Exhaustion in an infallible process allocation such as `make_unique`,
`make_shared` or infallible vector growth produces a best-effort,
allocation-free `GTI-R0011` report and aborts the entire process at the detector.
It performs no cleanup, stream flushing, observer invocation or worker-to-join
handoff. Allocation exhaustion during existing failure cleanup also aborts;
it is not a contained secondary `GTI-R0014` failure. The optimizer may remove
unobserved process allocation/release pairs under the recorded allocation
non-observability contract; calls to application resources retain their
observable policy/effects.

Recoverable allocation remains an explicit `try_` operation's `expected`
result. Typed storage and custom resource factories supply such operations;
`try_make_unique` and `try_make_shared` remain separate completion work.
Native allocator exceptions do not escape into source.

Class/struct allocation with a selected ordinary constructor separates byte
allocation from object construction. The owner is published only on success.
Exhaustion aborts without cleaning prepared arguments. Once construction begins,
its parameters belong to the constructor: an ordinary **constructor failure**
is contained, frees unconstructed storage, never publishes an owner, and
preserves the constructor's original failure record. This includes the currently proved
constructor rollback family, including generated-cleanup owning fields such
as strings. Completed fields are cleaned before earlier-created constructor
parameters, in reverse field order; an enclosing source destructor remains
inactive if construction fails. The selected zero-argument owning-base family
also propagates base-constructor failure before the derived body runs. A
completed base is cleaned after later derived construction fails. A generated
field/base/pointee aggregate ending in lifecycle-guarded leaf destructors
now preserves the original failure if a leaf destructor also fails, reporting
`GTI-R0014` with both sites. Empty owners perform no pointee cleanup. This
includes multiple resource owners and scalar/string ownership in the same
generated aggregate: cleanup runs in reverse field order, and a first failure
drains the remaining fields before propagation. A second failure stops cleanup
immediately. Destruction of vector/prefix storage and sparse storage now
applies this protocol in reverse live-element order, including source-leaf
class elements, unique owners, nested collections, and generated aggregates.
An empty or moved-from collection performs no element cleanup. The allocation
is released after all live elements have been drained. Explicit vector
`pop_back()`, sparse-storage `destroy()`, and their source-defined `clear()`
loops use the same contained element cleanup. A failed removal retires that
element exactly once and propagates its original failure through caller
cleanup; a later destructor failure reports both records and stops cleanup.
Vector `erase(index)` also uses contained cleanup for these element families.
Successful removal shifts survivors into the hole through generated moves and
keeps the collection reusable. If the selected destructor fails, later elements
are drained in reverse order before caller cleanup handles the earlier prefix.
A second failure stops that sequence immediately, preserving both records.
An invalid index leaves the collection unchanged until caller cleanup.
Source-destructor classes with represented owning fields now use this same
protocol: their body runs while the fields are live, followed by reverse field
cleanup. A first body failure still drains fields; a failure during an existing
failure stops immediately and retains both records. This composes through direct
and unique-owned values, nested fields, and vector/storage cleanup and removal.
Partial construction runs only completed fields, never the enclosing source
destructor. Recursive ownership types, including `forward_list` nodes and a
source-destructor class owning another instance of itself, use finite MIR
cleanup recipes. Empty owners end the traversal; live children retain the same
first/secondary failure rules. This does not permit cyclic exclusive ownership
values or promise constant stack usage for arbitrary recursive destruction.
`forward_list` continues to detach and release nodes iteratively in its source
implementation. Other unrepresented element/payload families still need their
contained failure protocols.

`unique_ptr::reset()` detaches the old owner into a local source-defined owner
and leaves the receiver empty before that local is destroyed. Its cleanup uses
the same MIR failure protocol as scope destruction. Plain assignment over a
represented owning value now holds the replacement while explicitly cleaning
the old contents. Only successful cleanup commits the replacement. A first
cleanup failure drains the old value's remaining subobjects, then ordinary
caller cleanup destroys the uncommitted replacement and remaining locals. A
second failure terminates with both records. This covers represented direct
objects and unique owners, including moved and fresh replacements and checked
owner-field access and built-in fixed-array elements. Array replacement checks
every destination dimension before starting the old value's cleanup. Invalid
bounds leave the array unchanged and destroy the staged replacement through
ordinary caller cleanup. This includes fresh and explicitly moved values,
direct source-destructor objects, and arrays of unique owners. Unrepresented
cleanup families remain separate completion work.

Argument-bearing base construction, derived owning fields and source cleanup
have represented direct-construction schedules; generated base defaults and
selected unique-allocation rollback have their own focused tests. Do not infer
all allocation/constructor compositions from either one matrix. Generated
default/copy/move construction is also represented for admitted allocation
families. There is no terminal compatibility-body fallback: any further
profile needs exact selected construction, publication and rollback in verified
MIR before the backend may emit it.

## Destruction

Successfully initialized owned bindings are destroyed at the end of their
lexical scope in reverse successful-initialization order. Parameters are
initialized left to right and destroyed right to left after body locals.
Arrays destroy live elements in decreasing index order. A live class or struct
runs its cleanup body, destroys fields in reverse field declaration order, and
then destroys its state-bearing base. Closure captures are destroyed in reverse
capture-initialization order. An owned move capture transfers the source's
active cleanup obligation into its environment field; moving the closure
reparents that environment obligation and cleanup runs exactly once.

Temporaries and transient loans use the full-expression obligation stack in
[Execution Section 4.2.3](execution.md#423-full-expressions-and-cleanup). An
obligation that remains in the expression is discharged in reverse start
order; ownership transferred into a destination is reparented to that
destination instead. A partially initialized array, object, closure, call
frame, or result cleans only successfully initialized children, also in reverse
obligation order. The enclosing cleanup body does not run before the enclosing
lifetime begins. Movement transfers active resource/cleanup state while the
moved-from source retains any structural destruction required by its fields.

Every class and struct now has explicit frontend lifecycle metadata. Declared
constructors form exact-match overload sets. The compiler independently derives
default construction, copy/move construction, copy/move assignment, and
destruction from field traits; adding an ordinary constructor does not suppress
movement as it can in C++.

The exact declarations `Type() = default;` and `~Type() = default;` make the
compiler-derived default construction and destruction policy explicit. They do
not create empty source bodies: declaration initializers, checked default
construction of omitted nominal fields, state-bearing base
construction, reverse field/base destruction, and nested cleanup remain the
same as when the declarations are omitted. A defaulted destructor therefore
does not make a type noncopyable or require an active-drop flag.

A class or struct may explicitly default or delete copy and move construction:

```gti
Value(Value& other) = default;
Value(Value&& other) = delete;
```

These are public policy declarations, not ordinary overloads. `T&` is the
read-only copy source and `T&&` is confined to the exact move policy; general
rvalue and forwarding references are not introduced. Defaulting preserves the
structural result and is rejected when a base, field, stored reference, or
cleanup policy makes the operation unavailable. Deleting construction updates
the type's frontend traits, including concrete generic instances and enclosing
aggregates. Copy and move assignment remain independently derived.

A class or struct may instead declare one public `~Type()` cleanup body. The
body has an implicitly mutable receiver, is non-throwing, cannot return, and is
invoked automatically before fields are destroyed in reverse declaration
order. It is not a callable method. Declaring cleanup makes the type
noncopyable because the compiler cannot prove that duplicating copyable field
representations also duplicates the external cleanup obligation safely.

Cleanup-owning types remain movable when their fields are movable. Movement
transfers an active-drop state: moved-from values still destroy their fields but
skip the source cleanup body. Move assignment first runs cleanup for the active
target, then replaces its fields and transfers active state. This avoids C++'s
rule-of-five and moved-from destructor traps while preserving explicit GTI
transfer semantics. Custom copy and move constructor bodies remain unsupported.
Movement is a compiler-controlled ownership transfer, not a user-code hook.
Use explicit `clone()` methods for user-defined duplication and consuming
methods or factories for custom transformations. The custom-copy experiment
was withdrawn from the active campaign for further design consideration;
neither form of custom constructor body is required by shared ownership.

A type requires active cleanup when its own declared cleanup runs or a base,
field, or fixed-array element owns a resource/active cleanup obligation. Such a
type cannot currently have namespace-global or static-field storage. Global/static
shutdown remains absent, while program-wide initialization and initializer-
temporary cleanup use the deterministic cross-unit order in Execution Section
4.2.4. This restriction is structural and applies through aliases, enclosing
aggregates, and concrete generic fields; ordinary cleanup-free value globals
remain governed by the separate global-storage and concurrency rules.

A closed payload enum also requires active cleanup when any alternative owns
an active-cleanup class. The enum carries one obligation whose concrete drop
dispatches only to the active alternative. Construction and whole-enum movement
transfer that obligation exactly once; tag-only matching borrows the enum for
the duration of the switch and does not move its payload. Move assignment
replaces and drops the destination's old alternative before transferring the
source obligation. In the bounded callable slice, the same obligation may move
through ordered argument staging into an ordinary callable's lexical parameter
slot and from its return into a caller-owned result slot. The result obligation
is inactive until successful publication, so failure cleans parameters without
dropping an uninitialized result. An ordinary non-polymorphic
class or struct may also own the enum directly in a field. Construction
rollback arms that field only after successful publication, generated movement
transfers its obligation with the containing value, and replacement of a
mutable field drops the old active alternative before accepting the new one.
Concrete generic class payloads and finite recursive owning-class containment
retain this same contract. MIR records active-variant/field cleanup selectors
and finite recursion through concrete class cleanup; a failing destructor does
not skip remaining sibling cleanup. Borrowed payload patterns introduce
arm-scoped shared (`auto&`) or exclusive (`mut auto&`) loans into stable enum
storage. Their conflict root is conservatively the whole enum, and a checked
receiver-derived return retains its original owner dependency. A switch over
a consumed enum, a prvalue or an explicit `std::move`, instead owns the enum
until the selected arm's entry: by-value bindings take its fields by move and
release the drained enum without a second drop, while any other arm drops it
before its first statement.
Concrete generic callable instances and owning wrappers retain the same
obligations, including enum elements in storage-backed collections and inline
optional storage. No ownership rule follows merely from a generic argument's
spelling. Finite nested enum payloads and fixed arrays of supported payload
elements retain these construction and cleanup obligations. Passive native
records and unions may be payloads when their C ABI layout is validated.
Polymorphic enum callable transport, direct owning-enum lambda capture, arrays
whose elements are directly owning enums, payloads that are themselves
stored-reference class owners, unsupported
compiler-private payloads, native-layout owners containing owning enum fields,
and polymorphic bases remain diagnosed rather than inventing indirect cleanup
state.

Reference-bearing payload enums retain a complete dependency set rather than
one unconditional owner: each entry records an external origin, access mode,
field path and active-alternative guards. Nested enum values preserve those
paths during construction, calls, movement and return. A reference-free
alternative has a known empty set; it must not borrow an unrelated argument
just because another alternative needs one. This also applies after generic
reference substitution and when multiple other alternatives contain borrows:
the selected typed alternative, not the type-wide number of reference paths,
determines whether initialization needs external owners. Source-bodied empty
factories establish that complete fact through their return expressions; a bare
declaration or missing dependency metadata does not establish an empty proof.
A shared-reference enum may be copied when its other fields permit copying. An exclusive-reference field makes
the complete enum move-only, including when nested in another enum.

A source class can retain that conditional dependency set in a field. The
current structural slice requires every retained dependency and alternative
guard to share one exact field prefix; selecting the field removes that prefix
from the whole set, never just the convenient subset. That field holds the
payload enum itself: a field whose type is another conditionally borrowed
class, such as a `Holder` storing an `expected<int&, E>`, is not yet supported
(`GTI-S2045`), and neither is an enum payload whose type is such a class
(`GTI-S2067`); classes with unconditional reference fields remain valid
payloads. A reference selected from a conditional carrier is
relayed only from a named local, parameter or temporary carrier, not through a
field, element or dereference of another value (`GTI-S2045`). A free function
whose carrier result could borrow from more than one eligible parameter is
ambiguous unless `[[borrowed_from(name)]]` names the owner, as for a plain
reference return. Empty-body constructors
establish the result from their exact field initializers, including a proved
reference-free alternative. The initializer may construct the enum in place or
move or copy a by-value enum parameter into the field, as in
`Shelf(Slot slot) : slot(std::move(slot))`; the object then keeps that
argument's external owners. A missing proof is not an empty alternative.
Implicit copying remains conditional on all fields being copyable; copying a
shared-reference wrapper retains the same external owners, while moving a
wrapper does not move those owners. A consuming method may return a selected
external reference after retiring the wrapper, but cannot return a reference
to the wrapper's own storage.

An implicit constructor uses the same complete dependency proof in variable
initialization and return conversion. The original expression remains the
constructor input; the constructed result carries a distinct checked set.
A fresh wrapper can also be consumed immediately, and an ordinary reference
return can relay the extracted external reference. The temporary's mutable
shell loan retains its producer's complete stored group through invocation;
retiring that shell does not retire the caller's referent. Consuming a fresh or
explicitly moved wrapper does not upgrade its dependencies either: a read-only
dependency stays read-only, so another wrapper may keep reading the same owner
while one is consumed. A relay must still
obey its own parameter-origin/access contract and cannot escape a local owner
or permit mutation while a returned shared reference remains live.

A reference accessor on a statically known reference-free alternative can
still be called when its checked body has no normal result for that
alternative. The compiler preserves the call's failure or nontermination;
it does not invent a referent or reject the call merely because no owner can
be supplied. This is an input-specific fact, not a `noreturn` property of the
accessor: calls on reference-bearing alternatives retain their ordinary
checked result. Unknown dependency information is insufficient to establish
this fact. Reference initialization, nested call arguments and reference
returns stop at the terminal call without publishing a reference.

An open generic receiver may not yet have a complete conditional inventory.
Only its dependent enum switch is deferred to concrete instance analysis;
every requested instance must establish the exact inventory and recheck the
selection, return and lifetime rules. Unrelated nondependent errors remain
diagnosed at the declaration. This is not permission to infer an external
owner from an incomplete type.

Selecting a direct reference field proves the active tag and retains all of
that field's possible external owners. Consuming the enum retires its old
dependency group before activating the selected references; moving the enum
does not move their referents. A returned selection publishes a separate return
capability and retires the local selection at the same boundary. MIR verifies
the executable selection, origin set, access and retirement, rather than
accepting matching return annotations as proof. Source-defined propagation can
therefore consume a borrowed-success/owned-error enum and forward the selected
success reference while preserving ordinary residual cleanup. A `has_value`
predicate alone grants no permission to project an unchecked alternative.

Construction and transport of nested reference-bearing enums do not yet permit
consuming an outer alternative into a new reference-bearing *value* pattern
binding. That nested value handoff remains a diagnosed structural goal, separate
from selecting a direct reference field; it needs its own transfer/retirement
proof. Neither this boundary nor the source-defined propagation protocol is a
special rule for a standard-library type.

The semantic `GTI-S2061` check implements the recursive active-cleanup
restriction after the more-specific unique-owner, private-storage, and
borrowed-state global diagnostics. Its related information identifies a
declared cleanup body, cleanup-owning base, or cleanup-owning field when one is
available. This rule prevents source global/static drop obligations; it does
not add process-shutdown cleanup.

The C++ backend emits generated operations explicitly as `= default`,
`= delete`, or an active-state move implementation. Its hidden active flag is a
representation of the GTI drop rule, not part of object layout or a stable ABI.

Field immutability is enforced by GTI semantic analysis rather than physical
C++ `const`. This permits assignment to replace a mutable whole-object binding
without making its immutable fields individually writable in GTI source.

The semantic model marks types that require lexical destruction. Typed HIR
assigns stable values, bindings, callable instances, and class instances. MIR
now makes moves, projected places, loans, borrow ends, lexical drop points, and
control-flow cleanup explicit. Object layout facts and ordinary source cleanup
bodies are implemented; arbitrary user-defined copy/move bodies remain
unsupported. Neither lifecycle eligibility nor cleanup follows from the C++
backend's type traits.

## Structural Relocation

### Restoring initialized places

`std::exchange(place, replacement)` replaces one initialized writable place
and returns its previous value, retaining that value's ownership. Both values
must have the exact same structurally relocatable type. Prepare conversions
explicitly; a named move-only replacement requires `std::move`.

`std::swap(left, right)` exchanges two disjoint initialized writable places
of the same structurally relocatable type. The ordinary exclusive-reference
contract applies: self-aliasing, overlapping places, and conflicting live loans
are rejected. Neither operation requires copying or move assignment.

Argument evaluation and replacement construction finish before transfer begins.
Failure there leaves the destination unchanged. The sealed transfer itself
uses generated, failure-free relocation: it neither calls user movement code
nor destroys the old resource. Every place is restored before any caller code
or cleanup can run. The returned old value follows ordinary exactly-once
cleanup, including the existing double-failure contract if its cleanup panics.
This does not permit arbitrary consumption through a reference or indexed place.

These operations do not introduce a mutable reference escape. Their exclusive
loans end at the ordinary call/full-expression boundary; an owning exchange
result carries the old cleanup obligation, not a reference to its former slot.
The current single-owner text-borrow model does not support exchanging dynamic
view dependency graphs through mutable references; these operations do not
erase that boundary. Such a call receives a semantic diagnostic, not a backend
failure. Create a new local view with an explicit owner instead.

Checked indexed swap takes one exclusive owner loan instead of two element
loans. It validates both initialized indices before transfer, accepts equal
valid indices as a no-op, and restores the complete initialized prefix before
returning. No user operation can observe an empty element. A live element
reference, iterator or other owner-dependent borrow prevents the mutation,
including the equal-index case. Array/vector `swap_elements`, array `swap`,
and vector sorting/reversal use this protocol; they do not permit arbitrary
movement out of an indexed element. A comparison that fails runs only between
complete transfers, preserving a valid permutation for deterministic cleanup.

### Relocation into empty storage

Relocation transfers one complete value into **empty, non-overlapping typed
storage**, ending the source value's lifetime. It neither copies the ownership
obligation nor replaces an already-live destination. Generated movement
transfers resource handles and active cleanup state; the source cleanup body
does not run, and the destination retains the one remaining cleanup obligation.
Ordinary assignments still clean up their old destination. Relocation does not
change `std::move`, implicit return movement, or move/use-after-move checking.

`std::relocatable<T>` describes structural eligibility without requiring move
assignment. The compiler derives it through generated movement, concrete
bases, fields, fixed arrays, payload alternatives, and owned captures. Deleted
movement and stored borrows deny it. Abstract classes and pointer-only native
pointee declarations cannot relocate as values. The bounded host-state policy
also excludes private thread, atomic, mutex, and lock-token state; having a
native move operation is not itself a relocation proof. Owning allocation
handles relocate independently of their pointee's movability. No class
annotation or unsafe opt-in is needed.

`std::bitwise_relocatable<T>` refines this to passive scalar state, fixed arrays
of eligible elements, and classes whose fields are all bitwise-eligible and
which have no bases, polymorphism, or declared cleanup. A closure is an
aggregate of its captures with no self-reference, base or source destructor:
it is bitwise relocatable exactly when every capture is, structural when some
capture is only structural (an owning string, say), and unavailable when a
capture is borrowed or cannot relocate. A class holding such a closure follows
the ordinary field rule. Deleted copy construction alone does not deny it:
relocation is not copying. Cleanup-owning
classes and tagged/optional carriers use generated structural transfer, not
bytewise copying. These concepts are constraints, not boolean expressions.

The bitwise concept is a language eligibility fact, not a promise about emitted
instructions or an unsafe `memcpy` API. The C++20/C++23 backend additionally
checks its representation's trivial copy, move-construction, and destruction
properties before using bulk byte transfer. Other representations retain
equivalent generated elementwise movement. In particular, a resource owner's
nontrivial C++ representation is never copied as bytes.

The first explicit public operation is
`std::storage<T>::relocate_to(mut storage<T>& destination, size_t count)` in
`<std/memory>`. It checks both capacities and every source/destination slot in
`[0, count)` before transferring anything. Source slots must be initialized;
destination slots must be empty. The source prefix becomes uninitialized and
can be constructed again; neither allocation is released. A zero count is a
no-op, including for zero-capacity storage. Existing storage element
requirements still apply. State/capacity violations remain defined
`invalid_storage_state` failures, not recoverable allocation errors.

Both owners are borrowed exclusively for this call. Self-aliasing and live
loans into either allocation are rejected by ordinary borrow checking, even
when `count` is zero. There is no address escape, overlap/memmove API,
replacement authorization, user relocation hook, or pinned/self-referential
object facility in this slice. Unsafe code retaining a raw address must still
obey its existing lifetime contract.

Vector/string growth uses source-defined initialized-prefix policy over the
same structural relocation eligibility. Sparse source loops invoke general `relocate_at`. Passive
eligible elements can move in bulk; resource-owning elements retain the
generated transfer and exactly-once cleanup path.

## Backend Boundary

The current backend represents the unique owner's private handle with a
native `std::unique_ptr`-shaped carrier. Allocation and selected GTI construction
are nevertheless separate verified-MIR stages; ordinary class allocation uses
`mir_object_allocation_v1` rather than delegating GTI constructor selection or
rollback to `std::make_unique`. Public wrappers are ordinary lowered GTI classes,
not a stable generated namespace or ABI.

The checked private upcast lowers to a consuming native owner conversion only
after frontend semantics has proved the public base relation and polymorphic
destruction; C++ conversion rules never select the GTI operation.

The implemented shared-owner carrier has explicit strong-count, allocation and
adjusted-view state. Promotion, cloning, upcast and last-release cleanup consume
verified GTI facts; it is not a public `std::shared_ptr` ABI or host-defined
ownership policy.

This is lowering, not the GTI ABI or language definition. Smart pointers do not
cross the C runtime boundary. A C ABI cannot portably represent C++ template
instances, destructors, deleters, or shared ownership control blocks.

A future LLVM backend will consume the same ownership and drop operations and
may implement them through LLVM IR and narrow runtime allocation helpers. A
low-level aligned allocator can live behind the runtime boundary, but it does
not define public raw-pointer ownership or manual lifetime semantics.

## Public Typed Allocation

`<std/memory>` provides a safe typed allocation boundary directly in `std`:

```gti
std::allocator allocator = std::allocator();
auto attempt = allocator.try_storage<Widget>(
    std::size_t(64), std::alignment(std::size_t(64)));
if (!attempt) {
  return 1;
}
mut std::storage<Widget> widgets = std::move(attempt).value();
widgets.construct(std::size_t(3), Widget(7));
Widget& value = widgets.get(std::size_t(3));
```

The public types are `std::allocator`, `std::storage<T>`, `std::alignment`, and
`std::alloc_errc`. They are ordinary source-defined standard-library policy;
the compiler does not recognize their spellings. There is no public `std::mem`
namespace, address accessor, manual deallocation, `new`, or `delete`.

`std::allocator` is initially stateless. `try_storage<T>(count)` requests
natural alignment, while the overload taking `std::alignment` requests a
nonzero power-of-two minimum. The effective alignment is at least
`alignof(T)`. A request returns
`expected<std::storage<T>, std::alloc_errc>` with these recoverable errors:

- `invalid_alignment` for an explicit zero or non-power-of-two alignment;
- `size_overflow` when the target cannot represent the element count, byte
  extent, or alignment padding; and
- `resource_exhausted` when the host cannot supply allocation or bookkeeping
  storage.

Zero elements is a successful allocation with capacity zero and no host
allocation. Failure is atomic: no storage owner or initialized slot is
published until both source-owned resource acquisitions have succeeded. A
nonempty request acquires object bytes first, then one metadata byte per slot
from the same process provider. Failure of metadata acquisition or validation
returns every acquired descriptor before reporting the error.

`std::storage<T>` is a move-only lexical owner of a typed object region and its
separately leased metadata region.
It retains capacity, alignment, slot-live state, and release authority as one
capability. It cannot be forged, copied or converted to an address. `release()`
performs early owning cleanup without detaching an unmanaged allocation.
Public `T` must not contain borrowed state. It need not be movable:
final-address `emplace` supports immovable objects, and moving the allocation
owner does not move its elements. `construct` requires a movable completed
value, while `relocate_to` requires both movability and structural relocatability.
Each in-bounds slot is either uninitialized or holds exactly one live
`T`. `construct` consumes one completed `T` and begins a slot lifetime,
`get`/`get_mut` return checked receiver-tied references, `destroy` ends one
slot lifetime, and `clear` ends all live slot lifetimes in reverse index order.
Owner destruction clears any remaining slots in the same order and then
releases metadata followed by object bytes.

A returned reference prevents storage mutation, movement, or destruction for
as long as the ordinary borrow analysis considers it live. Reconstructing an
index begins a new slot lifetime; a reference from the earlier lifetime cannot
be reused. The current implementation conservatively treats the whole owner as
borrowed rather than proving distinct dynamic slot indices.

Bounds, duplicate construction, uninitialized access, and uninitialized
destruction are defined runtime failures rather than allocation errors. The
public operation accepts an already constructed `T`; failure while constructing
that argument therefore occurs before the slot operation and follows ordinary
MIR cleanup without publishing a new slot lifetime. `emplace` forwards its
concrete argument pack into the final slot using the existing MIR constructor
preflight and partial-construction failure boundary. Failure preserves its
original identity and does not publish a live slot.
The complete rationale, provenance model, and
C++/Rust/Zig/Swift comparison are in
[ADR 022](../decisions/022-safe-typed-allocation.md).

## Bounded Arena And Pool Ownership

`<std/memory>` composes the typed storage owner into two ordinary GTI classes:

```gti
auto created = std::try_make_arena<Widget>(std::size_t(64));
if (!created) {
  return 1;
}
mut std::arena<Widget> widgets = std::move(created).value();
auto position = widgets.try_insert(Widget(7));
```

`std::arena<T>` and `std::pool<T>` require `std::movable T`, are move-only, and
carry the allocation owner directly. Their factories preserve the exact
`alloc_errc` result from typed allocation. A zero-capacity value succeeds and
is immediately full; insertion then returns the policy error `arena_errc::full`
or `pool_errc::full` rather than raising a runtime failure.

An arena keeps one monotonic `next` position. Successful insertion constructs
the completed value before incrementing `size`; `reset()` destroys every live
slot in reverse order and restarts at zero. A pool keeps a live count, chooses
the lowest uninitialized slot, and makes a checked erased slot reusable.
`clear()` destroys every live pool value in reverse slot order.

Positions are ordinary `size_t` values and carry no provenance or generation.
They identify the current slot only. Reset, erase, and destruction end the
corresponding slot lifetime, and later reuse may return the same number for a
new value. Checked access still rejects out-of-range and uninitialized slots,
but this first surface does not claim stale-handle detection. Retained element
references suspend all mutation and movement of the owning arena or pool under
the same conservative receiver-wide borrow rule as `std::storage<T>`.

The adopting constructors require a compiler-private, source-defined seal.
Only trusted standard-library factories can establish that the consumed
storage is freshly allocated and wholly uninitialized; application code cannot
forge the token or bypass the factories. The seal has no intrinsic behavior
and does not make either public class compiler-recognized.

## Programmable Resource Leases

`<std/resource>` separates four lifetimes that a C++ allocator usually blurs:

- **Keeping the resource alive.** `unsafe std::bind_resource(resource)` yields
  a `resource_ref<R>`, an ordinary checked shared borrow of the provider. Every
  owner built from it retains that borrow through its own cleanup, even when
  empty, so the provider cannot be moved, destroyed, or reached through a
  mutable method while any owner is live.
- **Mutating bookkeeping.** The protocol's `allocate` and `release` take a
  read-only receiver. A stateful provider mutates separately allocated
  bookkeeping through its own raw pointers inside `unsafe`. This is a provider
  obligation, not a new interior-mutability rule: ordinary fields of a shared
  resource stay read-only and no checked reference into the bookkeeping exists.
- **Owning regions.** A move-only lease owns exactly one region and its
  matching release authority. Sparse `resource_storage<T,R>` holds object and
  metadata leases; prefix owners need only their object region. Many leases
  from one resource coexist; none needs an exclusive borrow of the resource.
  Moving an owner transfers its region and release obligation without moving
  its elements.
  This includes taking an owner by value and moving that parameter through
  local bindings: the original provider dependency survives until the final
  owner's cleanup, not merely until the parameter's last source use.
- **Borrowing objects in a region.** Element references and iterators borrow
  the owning storage, not merely the resource. They block growth, release,
  clearing and movement of that owner until their last use. Keeping the
  resource alive alone never justifies resetting or reusing a live region.

Who guarantees what:

| Guarantee | Enforcement |
| --- | --- |
| Binding requires explicit `unsafe` and cannot outlive the resource | static |
| Provider reset, move or mutable access while an owner is live | rejected statically (exclusive access against a live shared borrow) |
| Element references and iterators versus growth, release, clear and owner movement | rejected statically |
| Element type capabilities: relocation and growth need movable, relocatable elements; immovable elements use fixed-address storage | static |
| Destruction before region release; exactly-once element cleanup; constructor rollback | verified MIR lifecycle and cleanup |
| Count overflow, invalid alignment, region shape, slot bounds and initialization | dynamic checks, reported as `alloc_errc` or a runtime failure |
| Provider returns writable, aligned, disjoint regions valid until matching release; truthful identity; release retires exactly that region; no independent raw reset, reentrancy or race invalidates a live region | unsafe obligation of the provider, accepted once at the binding |

The optional raw-pointer audit may report findings inside a provider's
implementation. Warn reports findings without changing acceptance; Strict
promotes reported findings to errors, but neither mode proves raw validity or
changes the runtime operation. The binding obligations are listed in full in
[raw pointers](raw-pointers.md#explicit-resource-binding), and the library API
in [the standard library](standard-library.md#custom-resource-protocol).

## Standard-Library Storage

Built-in fixed arrays provide initialized inline storage with compile-time
length:

```gti
int values[4] = {1, 2, 3, 4};
```

They are ordinary bounded values, not owning pointers or allocation handles.
They never decay to pointers, and their copy, move, and destruction traits
follow the element type. The same structural rule applies when the array is a
class field, including move-only and interface-implementing concrete element
types: the containing class inherits the array's copy, move, and cleanup
traits, and each constructed element owns one corresponding drop.
`std::array<T, N>` is already an ordinary source-defined wrapper over this
storage, with checked access and nominal cursors; it is not a future alias.

Partially initialized capacity requires a distinction between bytes and live
objects. Public `std::uninitialized<T>`, `construct_at`, `destroy_at` and
`relocate_at` provide that general boundary; their unsafe obligations are
specified in [raw pointers](raw-pointers.md). An application-defined owner may
use the same operations as the standard library, while checked references use
an explicit owner witness rather than acquiring permission from raw addresses.

`std::storage<T>` is now ordinary source policy over those facilities. Its two
leases own the object region and one metadata byte per slot from the same
provider. Source code validates descriptors, initializes metadata, checks
bounds and live state, performs final-address construction, and implements
relocation and shifts. It publishes a live slot only after successful
construction and retires the flag before destruction. A source cleanup guard
continues reverse traversal after a first destructor failure, preserving its
record; a second failure follows the existing terminal double-failure contract.
Metadata is released after all element cleanup, then object bytes are released.

The previous compiler-private `gti_internal::storage<T>` type and its
allocation/adoption/access/lifecycle intrinsics are retired. No native sparse
bitmap or specialized sparse drop loop remains. General typed operations still
carry exact semantic constructor selection, MIR cleanup recipes and unsafe
authority. Removing the sparse engine does not turn raw validity into a runtime
proof or remove checked owner loans.

Variadic construction retains GTI's existing bounds: final packs are immutable
by-value values, not C++ forwarding references. Copyable arguments may be copied
into a wrapper pack; a pack containing a move-only argument is one owned unit
consumed by its first whole-pack expansion. Sparse source policy checks the slot
before handing prepared arguments to the general final-address constructor.

Semantic visibility remains identity- and source-role-based. Applications
cannot name the private `gti_internal` namespace or recover private
capabilities through public signatures. This does not prevent ordinary
application owners from using the public lifetime and checked-reference APIs.

For containers whose live objects always form `[0, length)`, ordinary
`prefix_slots<T, Lease>` source policy adds one authoritative live count to a
raw buffer containing its resource lease, pointer and capacity. There is no
per-slot bitmap or metadata allocation. It publishes a count increment only after final-address
construction, retires an element before potentially failing destruction, and
uses source drain guards to preserve first/secondary failure cleanup. A process
or resource lease releases bytes after the live prefix has been drained. Sparse
storage remains a distinct policy for arbitrary initialized-slot patterns.
The private prefix-storage type, operations, construction-preflight instruction
and optimizer model are retired. S-ALLOC-09's admitted container migration and
recorded acceptance are delivered; later optimizer precision and new container
profiles have separate obligations, not an unfinished private-storage cutover.

## Source-Defined Vector

`std::vector<T>`, imported with `#include <std/vector>`, is now an ordinary GTI
class over the source-defined process-backed prefix owner. Its element type
satisfies `std::storage_element` and failure-free `std::relocatable`, not
assignment-bearing `std::movable`: growth, insertion and erasure transfer
objects into vacant slots rather than assigning over live elements.
Construction-only owners such as an owning
record containing an optional string may therefore be vector elements and may
be cloned when their explicit clone contract permits it. Borrowed-state and
deleted-move elements remain rejected. The storage field makes the vector
itself move-only, so transfer uses `std::move` and an ordinary copy is rejected.
No frontend or backend phase
recognizes the public `std::vector` name to supply container behavior. The
separate hosted-entry rule validates the canonical
`std::vector<std::string>` nominal identity only as an accepted `main`
parameter and retains the exact source-defined startup append callable.

The initial surface provides default and size construction, `size`, `capacity`,
`empty`, `reserve`, `clear`, `push_back`, `emplace_back`, `pop_back`, indexed
`insert` and `erase`, and checked `at` and `operator[]` access. The size
constructor value-initializes each
element and therefore succeeds only when the concrete element type is default
initializable. Unlike C++, both `at` and `operator[]` retain GTI's checked
storage failure. The separate `unsafe get_unchecked(index)` and
`unsafe get_unchecked_mut(index)` APIs omit the index check; their caller must
prove `index < size()`. Their references retain the same checked owner loans,
so unsafe indexing does not permit mutation, growth or movement through a live
element borrow. `string`, `array`, `span` and `mutable_span` provide the
corresponding read-only accessor, and mutable owners/views provide the
exclusive accessor. A string's hidden NUL is outside the allowed index range;
an empty sequence or zero-length fixed array has no valid unchecked index.

`emplace_back(arguments...)` forwards one final by-value pack through source
prefix policy to `std::construct_at`, which executes the selected constructor
in its final slot. It returns a writable receiver-tied reference to
that element. The reference prevents reserve, push, clear, movement, or another
mutable vector operation until its loan ends. This API avoids an intermediate
element value but deliberately does not promise C++ forwarding-reference
behavior; copyable arguments may be copied at the method boundary, while a
move-only pack is consumed once.

The read-only iterator retains one checked read-only reference to the backing
storage. `begin_mut()` returns a distinct move-only mutable iterator that
transfers the range's exclusive storage capability and uses checked mutable
dereference. The range statement adds its dedicated parent range loan and
per-iteration element child; an owned vector temporary may be transferred into
its hidden loop owner. Reserve, push, clear, movement, and every other
overlapping owner operation are rejected while the mutable range is active.
This is conservative whole-owner invalidation exclusion, not a
container-specific invalidation trait. Read cursors already provide backward
and signed random-access movement plus exact sentinel distance; iterator-position
insertion/erasure and additional category relationships remain separate work. Read-only and mutable single-owner spans
share the same prefix owner and retain checked owner dependencies.
`std::vector` itself always uses the process allocator; programmable placement
is provided by the separate `resource_vector` described under
[programmable resource leases](#programmable-resource-leases). The bounded read-only single-owner span is described below.

## Owning Text

`std::string_view` is a counted read-only view. A literal-backed view is an
independent trivial value over static storage. `std::string::view()` produces
a dynamic view carrying one compiler-tracked read-only dependency on that
string's character storage. Both forms support `size()`, `empty()`, and checked
read-only indexing. These apply equally to a named view and to a view produced
by a call in the same expression, as in `text.view().size()`,
`record.name().empty()` or `entry.first.view().size()` over a range element;
the call's owner dependency stays live for the whole expression, including a
temporary owner such as `std::string("tmp").view().size()`.

`std::string`, imported with `#include <std/string>`, is an ordinary nominal GTI
class over the source-defined terminated-prefix owner. Its shared character
prefix includes one initialized trailing NUL, excluded from logical size and
capacity. Zero-capacity empty strings use an immutable static terminator without
allocation; clearing a reserved string retains its region and terminator.
It supports construction and
append from a string view, capacity management, checked read-only and mutable
indexing, clear, comparison, explicit `clone()`, and read-only structural
iteration. Its source-defined iterator retains a checked read-only storage
borrow, so a live iterator prevents mutation, replacement, or movement of the
string. Because its source-defined storage owns its region and live prefix,
the compiler derives a move-only lifecycle for the class. This avoids a hidden allocation on
assignment; duplication is visible at the call site as `value.clone()`.

Dynamic views may be copied among local bindings and relayed from one exact
read-only parameter or receiver. Concrete generic instantiation derives this
same return dependency after substituting the exact parameter and return
types. A literal-backed argument remains an ownerless static view and needs no
runtime loan; a dynamic argument transfers its existing owner dependency to
the result. That retained loan prevents mutation, movement, or destruction of
the owner until the last local carrier ends. A function whose returns reach a
call to itself, directly or through other functions, does not yet relay a
parameter's view: returning one there reports `GTI-S2045`. Its text result is
ownerless when its other returns are, as for
`std::string_view loop(std::string_view s, int n)` returning `"done"` or
`loop(s, n - 1)`. A dynamic view cannot escape a
local owner, originate from temporary storage, be assigned into existing
storage, or be nested in a global, static, array, or dynamic container. It
reaches a field only through the owner-tracked view fields described below.
Mutable, multiple-origin, nested, and reassignable view graphs remain
deliberately unsupported. These restrictions are semantic and survive through
HIR and verified MIR; emitted C++ does not decide the lifetime.

Constructor field initializers obey the same storage rule. A constructor that
directly stores a by-value text parameter in a text field requires an ownerless
argument at each selected call. This includes generic fields after substitution,
parenthesized inputs, and explicit movement: moving a view does not move its
owner. Literal-backed inputs and ownerless defaults remain valid. A computed
dynamic initializer such as `text(owner.view())` is rejected at its initializer;
an ordinary view parameter forwarded to a storing constructor is potentially
dynamic, even if some callers happen to pass literals. Conditional expressions
cannot erase a dynamic operand's dependency.

### Owner-tracked view fields

A value may hold a view of a named local owner in a direct text field when
the value stays in the body that creates it. A constructor whose initializer
stores a text parameter, or a designated list, then publishes the argument's
owner dependency at that field instead of rejecting the argument:

```gti
std::string input{"token text"};
Token token = Token(input.view(), 3);       // token.text views input
Diag diag{.text = input.view(), .line = 2}; // so does diag.text
std::string_view head = token.text;         // a view of input as well
```

The dependency belongs to the value, not to its type. A `Token` over a literal
is ownerless and keeps every capability: it may be stored, passed, returned or
used as a method receiver. A value with a dependency:

- may initialize a local binding, be copied into another local (both carry the
  one dependency) or be moved;
- yields, when its published field is read, a dynamic view of the same owner;
  its other text fields stay ownerless;
- keeps its owner borrowed read-only until its last carrier ends, so the owner
  cannot be mutated, moved or destroyed while the value is live.

It may also be passed to a function, by value or by reference, or call a
method, when that function keeps the field's text within the call:

```gti
uint64_t width(Token token) { return token.text.size(); }
void keep(Token token) { kept = token.text; }   // kept is a global view

width(token);       // valid: the text stays within the call
token.text.size();  // valid
keep(token);        // GTI-S2045: keep lets the text outlive the call
```

Each function's field contract is inferred from its body. A text field of a
formal or of the receiver requires ownerless text at every call when the
function:
- stores it;
- passes it to a function that requires it;
- lets the whole formal escape: copies, stores or returns it, captures it, or
  passes it where no such contract applies.

A function that returns one text field instead relays it: the caller's
result holds whatever the argument or receiver holds in that field.

```gti
std::string_view first(Token token) { return token.text; }
std::string_view name() { return this.text; }    // a Token method

std::string_view view = first(token);  // valid: views input, as token does
token.name().size();                   // valid
kept = first(token);                   // GTI-S2045: the result views input
kept = first(Token("literal", 1));     // valid: the result is ownerless
```

The result depends on the owner exactly when the argument's dependency is at
that field; another field of the same value stays ownerless. The relay applies
only when every return of the function that may hold dynamic text reads the
same field of the same formal or receiver, possibly through such a function.
A function that returns that field beside another source, or two different
fields, keeps the requirement above instead. So does a function whose returns
reach a call to itself, directly or through other functions. Some depth of the
recursion may return any formal that it passes along, so a recursion that
permutes its formals requires ownerless text in each of them:

```gti
std::string_view pick(Token a, Token b, int n) {
  if (n == 0) {
    return a.text;
  }
  return pick(b, a, n - 1);  // a deeper call returns b.text
}

pick(Token("ab", 1), Token("xyz", 2), 1);  // valid: returns "xyz"
pick(Token("ab", 1), token, 1);            // GTI-S2045: token views input
```

Literal-backed values satisfy every contract, so existing calls are
unaffected. Inside such a function, reading the field gives a dependent view
of whatever the caller passed.

A payload enum's text payload holds a local owner's text the same way. The
construction publishes the dependency at that payload, and a `switch` that
observes the value binds a dependent view of it:

```gti
enum Lexeme { word(std::string_view text), count(int value) };

Lexeme lexeme = Lexeme::word(input.view());   // the payload views input
switch (lexeme) {
case Lexeme::word(text): std::println(text); break;   // a view of input
case Lexeme::count(value): break;
}
```

Such a value follows the class rules above: it may initialize, copy or move a
local, and it keeps its owner borrowed while it is live. Only the published
payload depends on the owner; another text payload of the same construction
stays ownerless. In this release a dependent payload value is only observed
by a `switch`: it is not yet passed to functions, consumed by `switch
(std::move(value))`, or bound by an `auto&` pattern at the dependent payload,
and the enum must not be generic.

A temporary holds the dependency within its expression. Reading its published
field, passing it to a function that relays the field, or calling a method
that returns the field gives a view of the owner rather than of the
temporary. The view may therefore outlive the temporary, and the owner stays
borrowed while the view is live:

```gti
std::string_view head = Token(input.view(), 1).text;      // views input
std::string_view relayed = first(Token(input.view(), 2)); // views input
Token(input.view(), 3).name().size();                     // valid
kept = Token(input.view(), 4).text;                       // GTI-S2045
kept = Token("literal", 5).text;                          // valid: ownerless
```

A temporary over literal text has ownerless fields, which may be stored or
returned.

A dependent value still cannot be passed to a constructor, a lambda, a
virtual method or a generic function, nor returned itself, captured, bound by
a reference, assigned into existing storage, or stored in a global, a field,
an array or a container. Each of these is `GTI-S2045`. For the same reason a
class with a destructor, a base or virtual methods keeps the ownerless field
rule. So does a constructor with a non-empty body, which could read the
published field back through `this` as ownerless text. A value holds dynamic
text from at most one owner, and that owner must be a named place: a view of a
temporary owner is rejected as before.

An adapter that needs to retain dynamic input can instead declare a checked
`std::string_view&` or `c_string&` field and accept the same reference type in its
constructor. The adapter borrows the view object and retains its underlying
text dependency separately. Both must outlive its uses: neither invalidating the
text owner nor replacing the borrowed view is permitted while the adapter is
live. This does not enable by-value dynamic views in arbitrary aggregates, and
does not copy or allocate the input text. JSON's private scanning adapters use
this checked-reference form.

A view read through such a reference, as in `this.text`, is dynamic: it keeps
the external text owner. It may be inspected, indexed, copied into a local,
passed to a parameter, or returned from a method under that method's receiver
dependency. A fresh adapter built over a local view borrows that exact view
object as a bound adapter does. The view cannot be passed to a constructor that
stores it by value, stored in a field, or escape the owner, including through a
relaying call.

An owning wrapper may relay a field's text accessor, for example
`return text.view();` or `return this.text.view();`. Both spellings retain
the same enclosing receiver dependency when `text` names an instance field.
A same-named parameter takes precedence; a shadowing local owner still cannot
escape, and static methods do not acquire an implicit receiver.
The summary names the enclosing receiver (or one
read-only reference parameter for a free relay); body analysis must validate
that the selected call actually returns a borrow from that source. Merely
calling a method on an owner does not prove this. Local owners, by-value owning
parameters, and fresh temporary owners cannot supply an escaping view.

A relay may also return a string literal on another path, for example
`if (empty) { return ""; }` before returning an owner's view. A `c_string`
relay may similarly return `nullptr`. These lifetime-independent alternatives
need no escaping loan inside the function; the dynamic alternatives still
require the declared owner. Callers conservatively retain the summarized owner
dependency for the whole result, even when a literal/null branch executes.

A `std::string_view` or `c_string` field, or a fixed-array element of either
type, holds only an ownerless view wherever a method, parameter or return can
reach it: a dynamic view is stored there, including through a constructor
initializer, only by an owner-tracked value that stays in its creating body.
Returning such storage,
as in `return this.name;`, `return name;`, `return this.label.text;`,
`return this.aliases[index];` or
`return label.text;` through a parameter, copies that independent value. It
does not borrow the receiver or parameter, so a method returning only stored
views has no owner dependency and its caller may mutate, move or destroy the
receiver while the result remains live. Concrete generic instances apply the
same rule after substitution. A stored view may also be a lifetime-independent
alternative beside a dependent relay, like a literal. Only a direct field or
element read qualifies: a checked `std::string_view&` or `c_string&` reference
field, a local copy or a call result keeps its ordinary origin rules. A
shadowing local remains a local, and a dependent value it holds still needs an
exact owner dependency.

The receiver path may include indexing, including an overloaded subscript:
`return records[index].text();` can relay the enclosing receiver's dependency
when resolved body analysis proves that the selected element and text accessor
retain that owner. A free relay through a read-only reference parameter follows
the same rule. Indexing alone does not establish a lifetime contract, and a
retained view still prevents conflicting mutation of the enclosing collection.

`std::string::c_string()` and `std::string::try_c_string()` use the same
single-owner dependency model for the compiler-owned `c_string` boundary type.
The unchecked spelling always returns the string's non-null terminated storage
and permits embedded NUL bytes; the checked spelling returns `nullptr` if an
embedded NUL is present. A retained result prevents mutation, movement, or
destruction of the source string and cannot escape a local owner. A `c_string`
received from native code has no inferred GTI owner, and hidden retention by a
native callee remains part of that API's external contract.

Both accessors use the general unsafe owner-witness bridge from
`<std/memory>` inside their safe, source-defined policy. The raw address carries
no checked lifetime by itself: the nominated string owner supplies the mandatory
read-only dependency. The compiler verifies that dependency and the exact staged
address inputs, but neither stores the prefix length nor scans for embedded NUL
bytes. These rules apply equally to unrelated user-defined character owners.
