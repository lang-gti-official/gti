# Ownership, References, And Allocation

Status: Current implemented ownership and lifetime contract. Remaining work is
tracked in [`docs/plans/roadmap-to-1.0.md`](../plans/roadmap-to-1.0.md).

GTI exposes explicit ownership separately from its bounded raw-pointer escape
hatch. Raw pointers exist for native interoperation and audited low-level
wrappers, but they own nothing and never replace the language's move, borrow,
or deterministic cleanup rules. Source-level `new` and `delete` remain absent.

## Public Ownership Surface

The implemented unique ownership surface is:

```gti
std::unique_ptr<Entity> entity = std::make_unique<Entity>(arguments);
```

These names are standard-library API, not keywords. The current implementation
defines `std::unique_ptr<T>` as a nominal GTI class in `stdlib/prelude.gti`.
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
allocation call. `std::make_shared` remains planned.

The safe ownership surface does not provide:

- ownership inferred from a raw `T*` value;
- integer-to-pointer or pointer-to-integer conversions;
- construction of an owner from an address;
- unchecked ownership casts;
- `unique_ptr::release()`;
- source-level `new` or `delete`.

One-level `T*` and `const T*` values and their lexically gated operations are
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
   `std::unique_ptr<T>` and the first source-defined `std::vector<T>` slice.
2. Those classes are implemented, as the class and generic systems mature, in
   GTI over compiler-defined capabilities in `gti_internal`, including
   ownership and partially initialized storage operations.
3. Lexical `unsafe {}` permits the implemented one-level raw-pointer operations
   needed by native wrappers and low-level code without making private
   `gti_internal` capabilities public.

This implemented low-level surface deliberately stops before source-level
allocation and deallocation functions or C++-style `new` and `delete`. Any
future manual-lifetime surface must define its own provenance, aliasing,
initialization, failure, and diagnostic contracts before it becomes public.

Intrinsic capabilities are declared as ordinary bodyless functions in the
implicit prelude. Semantic registration marks only the trusted prelude
declarations, and a call receives intrinsic behavior from its selected
`FunctionId`, including through a namespace alias. Reusing the same spelling in
application source creates an ordinary function and grants no compiler
behavior. No source attribute, keyword, or public wrapper name selects an
intrinsic.

The private owner, storage, and text-view types also have ordinary nominal
declarations in the implicit prelude. Their selected `ClassId` receives a
compiler-capability kind only for the exact trusted declaration. Application
source cannot enter or alias `gti_internal`; doing so is `GTI-S2058`, and a
same-spelled application declaration never acquires the private semantic type.

The C++ backend may implement one through C++ RAII or aligned allocation while
a future LLVM backend lowers the same operation differently. HIR retains the
operation without treating its bodyless declaration as a source function to
instantiate. Neither backend representation is part of the source language.

An intrinsic must represent an operation or invariant that ordinary GTI cannot
yet express. It may retain private bookkeeping needed to validate that operation,
but it must not expose library policy or answer wrapper-level questions. Logical
size, capacity, engagement, and similar state belong to the nominal stdlib type.
In particular, partially initialized storage exposes no slot-engagement query,
and the compiler has no `std::optional`-shaped storage API.

The unique-owner capabilities used by the wrapper are:

```gti
gti_internal::unique_owner<T>
gti_internal::allocate_unique_owner<T>(arguments...)
gti_internal::unique_owner_borrow(owner)
gti_internal::unique_owner_borrow_mut(owner)
gti_internal::unique_owner_is_null(owner)
```

They provide allocation, checked receiver-tied borrows, and observation of the
handle's irreducible null representation. The source-defined `std::unique_ptr`
operators decide what that state means for boolean conversion and comparison.
They do not expose an address, manual deallocation, release, or unchecked
dereference operation.

## Concurrency Transfer And Sharing

The default executable profile is single-threaded. GTI also implements the
ownership facts and process-wide storage policy of its opt-in concurrent
profile before the 1.0 ownership contract freezes; public concurrent
operations remain unavailable.

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
synchronization contract, such as a future atomic or mutex guard.

The first executable thread model consumes one owned task plus owned
transfer-capable arguments. References, borrowed-state iterators/views, raw
pointers, and borrowed captures cannot cross even when a native lifetime looks
long enough. A later scoped-borrow extension requires an explicit structured
join proof, a child loan and suspended parent, cleanup on every normal and
failure exit, and verified reactivation only after child completion.

In the concurrent profile, namespace globals and static fields must be
immutable and share-capable. Ordinary mutable globals/static fields are
ill-formed; synchronized mutation uses an immutable global atomic or future
mutex value. Borrowed-state and raw-pointer globals do not qualify merely by
being immutable, and cleanup-owning process-wide values wait for a complete
shutdown/foreign-thread contract. These restrictions do not change current
single-threaded mutable globals. The reference compiler reports `GTI-S2060`
at the process-wide binding when concurrent selection rejects mutability or a
non-share-capable concrete type. Structural aliases and concrete generic
instances use the same resolved type fact; explicit nominal opt-outs are
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

Borrowed references and unique owners are source-reachable. Shared ownership
remains reserved in semantic metadata until its syntax, weak observation, and
lowering rules are implemented.

## References

Reference syntax is:

```gti
void render(Entity& entity);
void update(mut Entity& entity);
```

`T&` is a non-null, read-only borrow. `mut T&` is a non-null, writable borrow.
A reference does not own its referent and cannot be reseated. References cannot
bind to temporaries or outlive the storage they borrow.

References may be parameters and non-escaping local bindings. They require an
addressable initializer, and mutable references require a mutable place.
Method returns may use `T&` when the returned place is derived from `this`.
`mut T&` is also available from a method with a trailing `mut` receiver when
the returned place is writable. The call result is a borrow tied to its
receiver, so it cannot be retained from a temporary receiver:

```gti
T& at(std::size_t index) {
  return gti_internal::storage_read(this.data, index);
}
```

A free function or static method may return a read-only borrowed result when
every reachable return derives from one eligible read-only parameter. A `T&`
parameter can introduce that dependency. A by-value parameter whose concrete
type is a direct one-owner borrowed-state carrier can only transfer its
existing dependency; it does not create a new runtime borrow. Concrete generic
instances are checked by the same rule, but the declaration must name the
carrier shape: an unconstrained `T` parameter does not gain this capability
from an eventual view substitution. The selected parameter index is part
of resolved call metadata, so callers attach the result to the argument's owner
instead of treating the returned value as independent:

```gti
T& identity<T>(T& value) {
  return value;
}
```

This is implicit single-origin lifetime inference, not a general lifetime
parameter system. Other parameters may contribute ordinary values, but a
returned borrow may not choose between owners or combine dependencies.

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
parent remains live but suspended: reading, writing, moving, or borrowing
through an overlapping part of the parent or owner place is rejected. Known-
disjoint named-field projections remain usable through that parent, and
multiple children may coexist only when their stable places are known to be
disjoint. Ending a child reactivates its parent only when no other active child
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
Raw pointer provenance, opaque storage sources, globals, and captured storage
do not receive this precise treatment.

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
is invalid. A mutation that completes in an earlier argument may be followed
by a later borrow when no other active loan conflicts. Known-disjoint origin
places remain valid in either order.

The current semantic implementation still rejects the overlapping mutation
and transient-borrow pair in both written orders because the compatibility
emitter has not migrated that call family to ordered MIR. M-EXEC-01 and the
matching M-BACK slice may remove the conservative rejection only for a family
whose emitted order and cleanup are authoritative.

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

These are local loan transitions, not stored or escaping lifetime graphs.
Mutable reference fields, returning any local child reborrow (mutable or
read-only) directly or through a stored carrier, and mutable owner-tied carrier
values remain rejected. This does not remove the existing receiver-tied
mutable-reference return rule described above. The slice also does not make
mutable owner-tied range iteration available or define range-level and
per-element loan scopes. Existing structural range syntax and writable yields
keep their narrower contract.

A local borrowed-state carrier may still retain the existing ordinary
read-only owner dependency. It cannot retain a mutable parent loan or any child
reborrow; that shape may be consumed only as a non-retained full-expression
temporary in this checkpoint.

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

A receiver- or argument-tied call result that is consumed without being stored
ends its MIR loan at the enclosing full-expression boundary. This includes a
borrow used to compute an `if`, loop, or switch condition. Retaining the result
in a reference or borrowed-state value still uses the conservative lexical
rule above.

Nested owner projections remain outside this checkpoint. In particular, a
borrow reached through `expected<owner, E>.value()` may be consumed immediately
within the same full expression, but it cannot be retained or returned through
another helper yet.

Mutable free/static reference returns, reference globals, nested references,
and references over fixed arrays or compiler-private owner handles remain
unavailable to ordinary source. A read-only free/static reference return is
accepted only under the single-parameter rule above. A non-escaping local
reference may borrow the public `std::unique_ptr<T>` class itself, but
conservatively prevents transfer or mutation of that owner for the rest of the
function.

One deliberately confined stored-reference form is available for owner-tied
library values such as iterators. A class or struct may contain one direct
read-only `T&` field. Every constructor must bind that field directly from one
exact read-only reference parameter. The resulting class is move-constructible
but noncopyable and nonassignable. An instance method may return it when the
dependency is derived from `this`; a free function or static method may return
it when the dependency is derived from one eligible read-only parameter:

```gti
class Iterator<T> {
  T& current;

public:
  Iterator(T& value) : current(value) {}
  T& operator*() { return this.current; }
};
```

That one dependency survives ordinary calls, concrete generic substitution,
explicit moves, checked returns, and lexical destruction. A helper may
therefore construct a read-only cursor or view from one owner, pass the carrier
through a concrete generic move relay, and return it to the caller without
losing the owner loan. Dependency propagation follows the selected argument
and is verified before backend entry; it is not inferred from an emitted C++
reference field. See
[`41-owner-dependencies.gti`](../../examples/41-owner-dependencies.gti) for a
runnable free/static factory and concrete generic relay.

Mutable stored references, multiple reference fields, inherited or nested
borrowed state, user-defined destructors, and global/static or captured
borrowed storage remain rejected. The stored owner-dependency model also
rejects mutable stored dependencies, multi-origin and dependency-changing
returns, and assignment that would replace a carrier's dependency. The local
exclusive-reborrow rules above do not widen that stored-value contract.
Retaining one of these values creates a semantic owner loan, so the owner
cannot be moved, replaced, or used through a mutable method while that loan
remains live. Moving the carrier transfers the same loan identity; creating a
read-only alias adds a carrier to that identity. HIR carries proven
straight-line, nested-merge, and conditional branch-entry endpoints, along
with bounded switch-exit and same-path immediate-break endpoints. MIR records
every carrier on one loan and emits an explicit borrow ending only after the
frontend-selected aggregate endpoint. These are GTI lifetime rules; the
emitted C++ reference field is only a backend representation.

A trusted prelude or imported standard-library unit may use the same contract
to retain one read-only `gti_internal::storage<T>&`. The exception applies only
to the carrier field and its exact binding constructor parameter. It allows an
ordinary source-defined iterator to traverse checked storage without exposing
an address or making the compiler recognize a container name. Application
fields, locals, parameters, returns, writable stored borrows, and references to
the private unique-owner capability remain rejected.

Restricted member `operator*`, `operator->`, and `operator[]` declarations may
return these receiver-tied references. A wrapper can provide paired read-only
and mutable receiver overloads, while semantic analysis records the selected
method and returned access mode. `operator->` performs exactly one checked
reference step; recursive C++ proxy behavior is not exposed. These checked
nominal operators remain distinct from built-in raw-pointer `*`, `->`, and
`[]`, which require an unsafe block and create no borrow or loan.

Range-based `for` uses `operator*` through the same receiver-tied rules and
holds a stable borrow of the range for the loop. Both self-contained iterators
and the confined owner-tied iterator form above are supported. The compiler
does not invent a raw pointer or public intrinsic for iteration; see
[`ranges.md`](ranges.md).

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

Safe code may carry, compare, pass, return, and copy compatible raw pointers.
Address formation, dereference, indexing, member access, arithmetic, and calls
through pointer-bearing C declarations require `unsafe {}`. This gate transfers
the validity, lifetime, alignment, initialization, bounds, provenance, aliasing,
and native-call obligations listed in
[`raw-pointers.md`](raw-pointers.md) to the programmer.

The intended ownership pattern is to keep a raw handle in a nominal class,
perform native operations inside small reviewed unsafe blocks, and use the
ordinary generated move and cleanup machinery to guarantee exactly-once
release. The class owns the resource; the raw pointer field still does not.

## Ownership Transfer

`std::move(value)` is a compiler-defined explicit move operation with familiar
C++ spelling. It accepts a named movable local value, by-value parameter, or
writable field place rooted in one of those values or in a mutable `this`.
Checked field access through `operator->` is also supported. A directly owned
fixed-array element is movable when its index is an in-range compile-time
integer value and its containing local, parameter, or writable field has an
exact ownership place. Move-only values require `std::move` at consuming calls,
returns, initializers, and assignments; copyable values may also be moved
explicitly so generic code does not need an ownership-specific spelling.

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

The bounded consuming-call spelling is `std::move(value)(arguments)` when the
selected nominal target is an exact `operator()(parameters) &&`. Trailing
`&&` is a receiver capability only for
`operator()`; it does not introduce a general `T&&` type. The receiver may
mutate or move from its fields because the whole exact receiver place is being
consumed, and it may finish with moved fields rather than restoring a value
that is no longer available. It cannot return a reference or tracked
borrowed-state carrier. Mutually exclusive control-flow paths may each consume
the callable, but any path permitting a second invocation or later use is
rejected by the same availability analysis as an ordinary explicit move.
The receiver must currently be free of structural active-cleanup state. A
native rvalue-qualified call does not transfer that state into a distinct
full-expression-owned value, and materializing an ordinary helper parameter
would clean up at the wrong boundary relative to sibling operands. GTI
therefore rejects direct and concretely selected generic consuming calls for
cleanup-owning receivers until that representation exists.

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

The semantic model records which immutable bindings are explicitly moved. The
C++ backend must not equate those bindings with physical C++ `const`, while HIR
represents the operation directly as `Move` rather than an ordinary function
call. These are backend contracts for a source-level rule, not C++ semantics.

The nominal wrapper may be used as a local binding, parameter, return value,
class field, fixed-array element, or non-escaping local reference. A class or
array containing it becomes move-only through structural field traits. Unique
owners remain unavailable as globals. Fixed generic functions, methods,
classes, and constructors may use move-only type arguments because typed HIR
rechecks each concrete body after substitution. Copies remain errors and
transfers still require `std::move`. Typed HIR also retains concrete variadic
pack elements. A pack containing any move-only element is consumed as one unit
by its first `values...` expansion, so forwarding it again is a use-after-move
error. Copyable packs may still be forwarded repeatedly; individual pack
elements cannot yet be named or moved separately.

The planned `std::shared_ptr<T>` surface will be copyable and movable. Copying
will add an owner; moving will transfer one handle. Shared ownership does not
solve cycles, so GTI also needs a non-owning weak observation type before shared
cyclic object graphs can be supported responsibly.

## Nullability And Allocation Failure

Owning pointers may be empty, including after movement. Boolean and `nullptr`
comparisons inspect that state. Dereference and member access on an empty owner
produce a defined GTI runtime failure rather than undefined behavior.

Empty construction is explicit, consistent with all GTI class construction:

```gti
std::unique_ptr<Entity> entity = std::unique_ptr<Entity>();
```

GTI does not implicitly convert `nullptr` into a class value.

The default `make_unique` operation is infallible at the type level: allocation
exhaustion raises `GTI-R0011` with detail `unique_owner`. Future
`try_make_unique` and `try_make_shared` APIs may return `expected` for programs
that need recoverable allocation failure. The transitional C++ emitter catches
native allocation failure but still uses its old generic abort helper; that is
an implementation gap, not the language contract.

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

A class or struct may declare one public `~Type()` cleanup body. The body has an
implicitly mutable receiver, is non-throwing, cannot return, and is invoked
automatically before fields are destroyed in reverse declaration order. It is
not a callable method. Declaring cleanup makes the type noncopyable because the
compiler cannot prove that duplicating copyable field representations also
duplicates the external cleanup obligation safely.

Cleanup-owning types remain movable when their fields are movable. Movement
transfers an active-drop state: moved-from values still destroy their fields but
skip the source cleanup body. Move assignment first runs cleanup for the active
target, then replaces its fields and transfers active state. This avoids C++'s
rule-of-five and moved-from destructor traps while preserving explicit GTI
transfer semantics. Custom copy and move lifecycle bodies remain unavailable
because the initial field-place slice does not yet model arbitrary
constructor-time partial state, indexed places, or failure-aware active-drop
rollback through a user-defined lifecycle body.

A type requires active cleanup when its own declared cleanup runs or a base,
field, or fixed-array element owns a resource/active cleanup obligation. Such a
type cannot currently have namespace-global or static-field storage. Global/static
shutdown remains absent, while program-wide initialization and initializer-
temporary cleanup use the deterministic cross-unit order in Execution Section
4.2.4. This restriction is structural and applies through aliases, enclosing
aggregates, and concrete generic fields; ordinary cleanup-free value globals
remain governed by the separate global-storage and concurrency rules.

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
control-flow cleanup explicit. Concrete object layout and custom lifecycle
bodies remain deferred rather than being inherited from the C++ backend.

## Backend Boundary

The C++ backend emits the public wrapper as `__gti_std::unique_ptr<T>`. Its
compiler-private field and allocation operation currently use C++ RAII:

```cpp
std::unique_ptr<T>
std::make_unique<T>(arguments...)
```

The planned shared-owner implementation may use the corresponding C++ RAII
types in this backend, but that representation is not source-reachable yet.

This is lowering, not the GTI ABI or language definition. Smart pointers do not
cross the C runtime boundary. A C ABI cannot portably represent C++ template
instances, destructors, deleters, or shared ownership control blocks.

A future LLVM backend will consume the same ownership and drop operations and
may implement them through LLVM IR and narrow runtime allocation helpers. A
low-level aligned allocator can eventually live behind the runtime boundary,
but it does not define public raw-pointer ownership or manual lifetime
semantics.

## Standard-Library Storage

Built-in fixed arrays provide initialized inline storage with compile-time
length:

```gti
int values[4] = {1, 2, 3, 4};
```

They are ordinary bounded values, not owning pointers or allocation handles.
They never decay to pointers, and their copy, move, and destruction traits
follow the element type. This is sufficient underlying storage for fixed-size
aggregates and a future `std::array` alias.

A GTI implementation of `std::vector<T>` needs capacity containing partially
constructed elements. Smart pointers alone cannot safely represent that
storage, and a fixed array cannot represent it because all of its elements are
always initialized. GTI therefore provides this compiler-private storage
surface:

```gti
gti_internal::storage<T>
gti_internal::allocate_storage<T>(uint64_t capacity)
gti_internal::storage_construct(storage, uint64_t index, arguments...)
gti_internal::storage_read(storage, uint64_t index)
gti_internal::storage_read_mut(storage, uint64_t index)
gti_internal::storage_destroy(storage, uint64_t index)
gti_internal::storage_relocate(source, destination, uint64_t count)
```

`storage<T>` is an aligned, move-only lexical owner. It tracks which slots
contain live values, destroys those values in reverse slot order at scope exit,
and then releases the allocation. Construction, destruction, and relocation
require mutable storage. Index failures use `GTI-R0007`; duplicate,
uninitialized, relocation-capacity, and relocation-state failures use `GTI-R0010`.
Infallible allocation failure uses `GTI-R0011`. Each follows the common
cleanup, containment, and reporting contract rather than exposing a
compiler-private C++ helper message.
The allocation extent and initialized-slot map are private safety bookkeeping,
not queryable source state. A container records its own logical size and
capacity and updates those fields when it allocates or relocates storage.

The element type must not contain borrowed state. An owner cannot safely keep a
partially initialized element whose stored reference lifetime is independent of
the storage owner, so semantics rejects that storage type before lowering.

`storage_construct(storage, index, arguments...)` constructs the element
directly in the selected empty slot. For a class or struct, semantics expands a
concrete final pack and selects one exact accessible constructor, including an
empty pack for default construction. Primitive construction accepts either no
argument for value initialization or one exact value; it does not add an
implicit conversion path. HIR and MIR retain both the storage-call operands and
the selected nested constructor identity, so the backend does not choose the
constructor and GTI does not first materialize a temporary `T` merely to move it
into storage.

This is bounded in-place construction, not C++ perfect forwarding. GTI's final
parameter packs are immutable by-value values and there are no forwarding
references or reference collapsing. A copyable constructor argument may be
copied as it enters a wrapper's pack. A pack containing a move-only argument is
one owned unit, is consumed by its first whole-pack expansion, and cannot be
expanded again.

`storage_read` returns a checked read-only borrow tied to its storage argument;
it does not copy the element. This lets a nominal container expose `T&` access
even when `T` is move-only while preventing the borrow from outliving the
container.

`storage_read_mut` requires mutable storage and returns a checked writable
borrow tied to that storage. The same conservative loan tracking prevents
relocation, destruction, or ownership transfer while the borrow may remain
live. Public containers expose this capability only through mutable receiver
operations such as `operator[]`.

`storage_relocate` requires a movable element type, move-constructs the leading
live elements into empty destination slots, and destroys the source elements.
This gives a container a single operation whose semantics can later lower to
explicit MIR move and drop instructions. It remains intrinsic only because GTI
cannot yet move a value out of a partially initialized place. Once
partial-place movement and precise loans can express that loop safely,
relocation can move into ordinary library code.

This facility currently belongs under the reserved `gti_internal` namespace
and is available only to trusted compiler and standard-library code. It
exposes no address, raw-data escape, or independent deallocation operation.
The existence of general raw-pointer syntax does not grant access to private
storage representation or initialized-slot bookkeeping. Merely adding an
intrinsic does not make it public or stable.

Semantic visibility is identity- and source-role-based. Application
declarations, references, or namespace aliases involving root
`gti_internal` are rejected with `GTI-S2058`, and declarations whose exposed
type contains a private capability are withheld from application lookup and
tooling. Public library signatures therefore cannot be used to recover these
types indirectly.

Within a trusted standard-library source unit, a validated stored-reference
class may retain one read-only `storage<T>&`. Its constructor and lifetime are
checked by the ordinary stored-borrow rules, and returning it from a container
method ties the result to that container. This is a borrow of the owner
capability, not a pointer or a query into storage bookkeeping.

The C++ backend represents storage with a private RAII helper built from aligned
allocation and explicit construction/destruction. This remains a backend
choice. A future LLVM backend can lower the same checked operations to MIR plus
narrow aligned allocation/deallocation runtime calls.

## Source-Defined Vector

`std::vector<T>`, imported with `#include <std/vector>`, is now an ordinary GTI
class over `gti_internal::storage<T>`. Its element type satisfies
`std::movable`; the storage field makes the vector itself move-only, so transfer
uses `std::move` and an ordinary copy is rejected. No frontend or backend phase
recognizes the public `std::vector` name to supply container behavior. The
separate hosted-entry rule validates the canonical
`std::vector<std::string>` nominal identity only as an accepted `main`
parameter and retains the exact source-defined startup append callable.

The initial surface provides default and size construction, `size`, `capacity`,
`empty`, `reserve`, `clear`, `push_back`, `emplace_back`, `pop_back`, and checked
`at` and `operator[]` access. The size constructor value-initializes each
element and therefore succeeds only when the concrete element type is default
initializable. Unlike C++, both `at` and `operator[]` retain GTI's checked
storage failure rather than exposing an unchecked indexing path.

`emplace_back(arguments...)` forwards one final by-value pack to
`storage_construct`, which selects the element's exact constructor and builds
the element in its final slot. It returns a writable receiver-tied reference to
that element. The reference prevents reserve, push, clear, movement, or another
mutable vector operation until its loan ends. This API avoids an intermediate
element value but deliberately does not promise C++ forwarding-reference
behavior; copyable arguments may be copied at the method boundary, while a
move-only pack is consumed once.

The first iterator is read-only and retains one checked read-only reference to
the backing storage. It supports the existing structural range protocol for a
stable vector lvalue, and an active iterator prevents mutation, replacement, or
movement of the vector. This is the same conservative one-owner carrier used by
`std::string`, not complete iterator invalidation semantics. Mutable iteration,
owned temporary ranges, nested/shared readers, precise per-element loans,
iterator categories, insert/erase, allocator policy, and owner-tied `span`
remain future work.

## Owning Text

`std::string_view` is a trivial counted view over static literal storage in the
current lifetime model. It supports `size()`, `empty()`, and checked read-only
indexing, but cannot be formed from dynamically owned storage yet.

`std::string`, imported with `#include <std/string>`, is an ordinary nominal GTI
class over `gti_internal::storage<char>`. It supports construction and append
from a string view, capacity management, checked read-only and mutable indexing,
clear, comparison, explicit `clone()`, and read-only structural iteration.
Its source-defined iterator retains a checked read-only storage borrow, so a
live iterator prevents mutation, replacement, or movement of the string.
Because its storage field is a unique owner, the compiler derives a move-only
lifecycle for the class. This avoids a hidden allocation on assignment;
duplication is visible at the call site as `value.clone()`.

Returning a dynamic `std::string_view` from `std::string` is intentionally
deferred. The single-origin read-only dependency now provides the necessary
compiler foundation, but the public view representation and invalidation tests
have not yet adopted it. Treating the current trivial view as an independent
value would still permit it to outlive the owner.
