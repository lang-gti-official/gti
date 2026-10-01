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
allocation call. `std::make_shared` remains planned.

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
   `std::unique_ptr<T>`, `std::storage<T>`, `std::allocator`, and the first
   source-defined `std::vector<T>` slice.
2. Those classes are implemented, as the class and generic systems mature, in
   GTI over compiler-defined capabilities in `gti_internal`, including
   ownership and partially initialized storage operations.
3. Lexical `unsafe {}` permits the implemented one-level raw-pointer operations
   needed by native wrappers and low-level code without making private
   `gti_internal` capabilities public.

The implemented public allocation surface is typed and owner-returning. It
deliberately stops before source-level raw allocation/deallocation functions or
C++-style `new` and `delete`. Any future untyped or manual-lifetime surface must
define its own provenance, aliasing, initialization, failure, and diagnostic
contracts before it becomes public.

Intrinsic capabilities are ordinary bodyless functions declared either in the
implicit prelude when shared across modules or in the one canonical
standard-library unit that owns them. Unique ownership and sparse storage live
in `<std/memory>`; prefix storage and the public logical-bounds check remain
shared prelude facilities. Semantic registration marks only those exact
trusted declarations, and a call receives intrinsic behavior from its selected
`FunctionId`. Reusing the same spelling in application source creates an
ordinary function and grants no compiler behavior. No source attribute,
keyword, or public wrapper name selects an intrinsic.

The private owner, storage, prefix-storage, and text-view types have ordinary
nominal declarations in those same owning units. Their selected `ClassId`
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
private sparse-storage capability now has total allocation-extent and slot-live
observers because those facts are irreducible representation state; only the
ordinary `std::storage<T>` wrapper exposes them through checked public methods.
This is not a general `std::optional`-shaped place API and exposes no bitmap or
address.

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
generally bind to temporaries or outlive the storage they borrow. The bounded
call-only read borrow below is not a local or stored reference binding and does
not extend the temporary's lifetime.

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

A direct ordinary constructor or by-value GTI call may instead produce a
self-contained class or struct value that is borrowed transiently as one
read-only or mutable method receiver. The compiler materializes one mutable
temporary place, begins the selected receiver borrow after successful
production, ends that borrow after invocation, and retains ownership of the
value until the enclosing full-expression cleanup. This ephemeral capability
does not authorize a stored reference, address, field-assignment target, or
mutable-reference argument. It is unavailable to a temporary containing
tracked borrowed state, an explicit move from named storage, or a virtual
method. A read-only ordinary method may return a read-only reference or
`std::string_view` tied to that temporary receiver for immediate nonescaping
use, such as `read(make_owner().get())` or `parse(make_text().view())`.
The returned loan ends before the temporary is destroyed, including when a
later argument or the consuming call fails. Binding that result beyond the
full expression or returning it remains rejected; no temporary lifetime is
extended. Mutable borrowed results and stored borrowed-state carriers are
outside this temporary-receiver capability.

A direct ordinary constructor or by-value GTI call may also produce a fresh,
self-contained class or struct value for one exact read-only `T&` argument of
an ordinary GTI function or non-operator method. The compiler materializes the
owned value at its written argument position, begins one read loan, evaluates
later arguments, invokes the selected callable, ends the loan immediately
afterward, and retains the value's drop obligation until the enclosing
full-expression boundary. A by-value exact overload remains preferred because
this borrowed form is a fallback viability tier. The permission does not apply
to `mut T&`, local or stored bindings, explicit moves, compound or native
producers, borrowed-state values, or a callable result that can retain the
loan.

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

`expected<T&, E>` applies this same inference to a checked read-only borrow.
The success path publishes the referent together with the receiver, argument,
or global owner dependency; an `unexpected(E)` path publishes no loan. A local
complete result is itself a carrier of that dependency, and `value()` creates
a child read loan rather than copying `T`. Copying, moving, or forwarding the
complete carrier preserves the one dependency. Fields, globals, parameters,
lambda returns, mutable reference payloads, and multiple-owner results remain
outside this bounded form. See [Expected values](expected.md#fallible-borrows).

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
values remain rejected outside the compiler-generated range plan. This does
not remove the existing receiver-tied mutable-reference return rule described
above. L-RANGE-02 defines the narrow range-level and per-element exception: an
exclusive parent covers the whole traversal, its one mutable element child is
confined to the current iteration, and the parent fully reactivates only after
that child ends.

A local borrowed-state carrier may still retain the existing ordinary
read-only owner dependency. It cannot retain a mutable parent loan or any child
reborrow as an ordinary value; that shape may be consumed only as a
non-retained full-expression temporary or as the compiler-generated mutable
range iterator.

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

General retained nested owner projections remain outside this checkpoint. In
particular, a borrow reached through `expected<owner, E>.value()` may be
consumed immediately within the same full expression, but it cannot be stored
or returned through another helper. The one bounded exception is a read-only
range sourced by exact `value()` on a named local or by-value parameter. That
loop retains a shared loan rooted in the complete `expected`, allows nested
read-only traversal, and blocks owner movement, replacement, mutation, and
drop until every loop exit ends the range loan. It does not authorize a
standalone payload reference, field/global receiver, mutable payload range, or
escaping element.

Reference globals, nested references, and references over fixed arrays or
compiler-private owner handles remain unavailable to ordinary source. A
free/static reference return is accepted only under the exact global/static
origin or one compatible reference-parameter rule: a read-only return selects
one read-only origin and a mutable return selects one mutable origin. A
non-escaping local reference may borrow the public `std::unique_ptr<T>` class
itself, but conservatively prevents transfer or mutation of that owner for the
rest of the function.

One deliberately confined stored-reference form is available for owner-tied
values such as iterators, views, and guard-shaped capabilities. A class or
struct may contain direct `T&` fields or nominal fields whose types recursively
contain shared references. Each independent owner remains part of the carrier's
checked dependency set; two fields may also refer to the same owner. Exclusive
carriers remain limited to exactly one `mut T&` reference-bearing field path;
mixing shared and exclusive paths is not supported. A direct reference must
bind from one exact reference parameter with the same access mode. A nested
carrier field must initialize directly from one exact carrier parameter,
using an explicit move for an exclusive dependency. Neither form supports
copy or move assignment because assignment would reseat an established
dependency.

A read-only carrier permits copy construction when its ordinary fields are
copy-constructible, and move construction when they are movable. A copy
constructs a new carrier shell, copies its ordinary fields independently, and
adds an alias to the same shared owner dependency. A mutable carrier is
move-constructible but not copy-constructible: its one exclusive dependency is
transferred, never duplicated. An instance method may return a carrier whose
dependency is derived from `this`; a free function or static method may return
one when compatible reference or carrier parameters supply its checked origin
set. Multi-origin returns must preserve all summarized parameter dependencies;
they cannot substitute local storage or select a different dependency set on
another return path:

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

That one dependency survives ordinary calls, concrete generic substitution,
explicit moves, checked returns, and lexical destruction. A helper may
therefore construct a cursor or view from one owner, pass the carrier through
a concrete move relay, and return it to the caller without losing the owner
loan. For a mutable carrier, construction suspends direct access to the exact
owner, a move transfers that suspension, and the final carrier end reactivates
the owner. A mutable method may create a call-local or retained child reborrow;
that child must end before the carrier dependency can end. Dependency
propagation follows the selected argument and is verified before backend
entry; it is not inferred from an emitted C++ reference field. See
[`41-owner-dependencies.gti`](../../examples/41-owner-dependencies.gti) for a
runnable shared factory/generic relay and an exclusive write view.

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

Composed shared carriers conservatively retain their complete dependency set
when a method exposes a reference or a subview. Such a projection does not yet
release an unrelated owner independently. `std::paired_span<L, R>` is an ordinary
source-defined client: it holds two spans, uses the shorter input length, and
offers indexed `first`, `second`, and `subspan` operations. It has no special
compiler recognition or independent iterator/proxy protocol.
Whole-carrier by-value forwarding is supported. The separately escaping
reference-return rule for a by-value carrier parameter still requires its one
exact reference path; selecting an individual path from a multi-path by-value
parameter is not part of this slice. Receiver-based accessors remain available
and conservatively retain the receiver's complete owner set.

Multiple exclusive paths, mixed shared/exclusive paths, inherited borrowed
state, arrays of borrowed carriers, user-defined destructors, global/static or
captured borrowed storage, dependency-changing returns, and assignment that would replace a carrier's
dependency remain rejected. Mutable dependencies are limited to an exact
stable local, parameter, receiver, or existing-loan origin. They do not admit
raw or opaque origins, dynamic-index identity, arbitrary lifetime parameters,
or an independently escaping child reference.

The compiler-trusted mutex guard retains its existing direct, destructor-bound
dependency rule. Wrapping it in another carrier is not included: its lock
obligation cannot be shortened to an ordinary carrier's last-use endpoint.

Retaining one of these values creates a semantic owner loan, so the owner
cannot be moved, replaced, or used through a mutable method while that loan
remains live. Moving the carrier transfers the same loan identity; creating a
read-only alias, including copy construction of a shared carrier, adds a
carrier to that identity. Dropping one shared copy ends only that shell's
lifetime; owner protection remains until the final reachable use of every
alias. An exclusive carrier has only one transferable shell. HIR carries
proven straight-line, nested-merge, and conditional branch-entry endpoints,
along with bounded switch-exit and same-path immediate-break endpoints. MIR
records every carrier on one loan and emits an explicit borrow ending only
after the frontend-selected aggregate endpoint. These are GTI lifetime rules;
the emitted C++ reference field is only a backend representation.

A trusted prelude or imported standard-library unit may use the same contract
to retain one read-only checked-storage reference, currently
`gti_internal::storage<T>&` or `gti_internal::prefix_storage<T>&`. A
compiler-generated mutable range iterator may instead carry one exact writable
checked-storage dependency from `begin_mut()` until range cleanup. These
exceptions apply only to the carrier field and its exact binding constructor
parameter. They allow an ordinary source-defined iterator to traverse checked
storage without exposing an address or making the compiler recognize a
container name. The writable carrier is noncopyable: duplicating its shell
would duplicate an exclusive capability. Ordinary source may now define the
same one-origin mutable carrier over public stable storage, but cannot name the
private checked-storage or unique-owner capabilities. Multiple origins remain
rejected in both source roles.

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
may declare a consuming receiver with trailing `&&`, as may `operator()`.
This is a receiver capability, not a general `T&&` type. Free and static
functions have no receiver and cannot use this qualifier. Assignment,
compound assignment, and increment/decrement retain their mutable contracts;
comparisons and `operator bool` retain their read-only contracts. Dereference,
arrow, and subscript do not gain consuming forms.

A named value never selects a consuming member implicitly. An owned movable
temporary, including a returned intermediate or conditional result, or an
explicitly moved receiver prefers an otherwise-exact consuming overload.
Examples are `make_builder().append(part)`, `base / first / second`, and
`std::move(base).append(part)`. The first `/` on named `base` selects its
read-only overload; later owned intermediates may select consuming `/`.
If no consuming overload matches, ordinary receiver-qualified fallback remains
available. Other arguments keep their existing copy/explicit-move rules.
Receiver selection adds no implicit conversions or argument ranking.
The [consuming builder example](../../examples/88-consuming-builders.gti)
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
an arbitrary borrow. Final combined validation and release remain tracked by
L-CALL-02.

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
be movable and its capture state cannot contain a reference, tracked borrow, or
raw pointer. The examples above describe the original exact-transport minimum,
not a restriction to one-field owners: ordinary direct by-value generic
parameters, concrete field storage, final value packs, and explicit interface
erasure also support source-defined owning callback wrappers. Each concrete
body must retain its exact callable identity, ownership, and cleanup schedule.
Fresh-lambda inferred returns, implicit ownership copies, namespace/static
closure storage, borrowed captures, and callable-reference erasure remain
outside this contract.

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
Mutable-reference elements likewise permit repeated exact forwarding: each
element retains an exclusive entry capability over its caller-owned referent,
not ownership of that referent. Ordinary call-site exclusivity rules reject
overlapping mutable/mutable and mutable/read-only inputs before the pack is
formed. Forwarding does not make the immutable grouped binding reseatable.

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
block destroys the completed object before reporting `allocation_failure` in
`shared_owner`; it does not leak or invoke the destructor twice. Last-owner
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
The current non-transferable mutex storage does not gain cross-thread transfer
permission merely by being allocated behind a shared handle.

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

This is not a general implicit class conversion. In particular, an ordinary
call such as `consume(nullptr)` does not construct a `unique_ptr` argument;
call arguments still require an exact parameter type.

The default `make_unique` operation is infallible at the type level: allocation
exhaustion raises `GTI-R0011` with detail `unique_owner`. Future
`try_make_unique` and `try_make_shared` APIs may return `expected` for programs
that need recoverable allocation failure. Scalar allocation returns a checked
status to the verified MIR `Invoke`: the owner becomes live only on success,
and exhaustion runs caller cleanup before the original allocation record is
reported. Allocation failure during failure cleanup preserves the primary
record and reports `GTI-R0014`. Native allocator exceptions are not exposed to
source. Class/struct allocation with a selected ordinary constructor now
separates allocation from construction. Prepared arguments are cleaned on
exhaustion; once construction begins its parameters belong to the constructor.
Failure frees unconstructed storage, never publishes an owner, and preserves
the constructor's original failure record. This includes the currently proved
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

The base-construction implementation requires parameterless derived
construction, cleanup-free derived fields, and generated base/derived
destruction; combinations outside that existing constructor proof still fail
closed. Targetless
generated default/copy/move allocation retains the terminal compatibility
boundary and remains an implementation gap.

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
not create empty source bodies: declaration initializers, state-bearing base
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
receiver-derived return retains its original owner dependency.
Concrete generic callable instances and owning wrappers retain the same
obligations, including enum elements in storage-backed collections and inline
optional storage. No ownership rule follows merely from a generic argument's
spelling. Polymorphic enum callable transport, direct enum lambda capture, direct array
payloads, stored-reference owners, compiler-private or native
layout records, and polymorphic bases remain diagnosed rather than inventing
indirect cleanup state.

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

The checked private upcast lowers to a consuming native owner conversion only
after frontend semantics has proved the public base relation and polymorphic
destruction; C++ conversion rules never select the GTI operation.

The planned shared-owner implementation may use the corresponding C++ RAII
types in this backend, but that representation is not source-reachable yet.

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
published until every private resource has succeeded.

`std::storage<T>` is a move-only lexical owner of one typed allocation origin.
It retains capacity, alignment, slot-live state, and release authority as one
capability. It cannot be forged, copied, converted to an address, or manually
released. Public `T` must satisfy `std::movable` and must not contain borrowed
state. Each in-bounds slot is either uninitialized or holds exactly one live
`T`. `construct` consumes one completed `T` and begins a slot lifetime,
`get`/`get_mut` return checked receiver-tied references, `destroy` ends one
slot lifetime, and `clear` ends all live slot lifetimes in reverse index order.
Owner destruction clears any remaining slots in the same order and then
releases the allocation.

A returned reference prevents storage mutation, movement, or destruction for
as long as the ordinary borrow analysis considers it live. Reconstructing an
index begins a new slot lifetime; a reference from the earlier lifetime cannot
be reused. The current implementation conservatively treats the whole owner as
borrowed rather than proving distinct dynamic slot indices.

Bounds, duplicate construction, uninitialized access, and uninitialized
destruction are defined runtime failures rather than allocation errors. The
public operation accepts an already constructed `T`; failure while constructing
that argument therefore occurs before the slot operation and follows ordinary
MIR cleanup without publishing a new slot lifetime. Arbitrary in-slot
constructor argument forwarding remains deferred until it has an explicit MIR
failure boundary. The complete rationale, provenance model, and
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
traits, and each constructed element owns one corresponding drop. This is
sufficient underlying storage for fixed-size aggregates and a future
`std::array` alias.

A GTI implementation of `std::vector<T>` needs capacity containing partially
constructed elements. Smart pointers alone cannot safely represent that
storage, and a fixed array cannot represent it because all of its elements are
always initialized. `<std/memory>` therefore declares this compiler-private
storage surface:

```gti
gti_internal::storage<T>
gti_internal::allocate_storage<T>(uint64_t capacity)
gti_internal::storage_try_allocate(storage, uint64_t capacity,
                                   uint64_t minimum_alignment)
gti_internal::storage_capacity(storage)
gti_internal::storage_initialized(storage, uint64_t index)
gti_internal::storage_construct(storage, uint64_t index, arguments...)
gti_internal::storage_read(storage, uint64_t index)
gti_internal::storage_read_mut(storage, uint64_t index)
gti_internal::storage_destroy(storage, uint64_t index)
gti_internal::storage_relocate(source, destination, uint64_t count)
gti_internal::storage_shift_right(storage, uint64_t first, uint64_t last)
gti_internal::storage_shift_left(storage, uint64_t first, uint64_t last)
```

`storage<T>` is an aligned, move-only lexical owner. It tracks which slots
contain live values, destroys those values in reverse slot order at scope exit,
and then releases the allocation. Construction, destruction, and relocation
require mutable storage. Index failures use `GTI-R0007`; duplicate,
uninitialized, relocation-capacity, and relocation-state failures use `GTI-R0010`.
Infallible allocation failure uses `GTI-R0011`. Each follows the common
cleanup, containment, and reporting contract rather than exposing a
compiler-private C++ helper message. `storage_try_allocate` is the narrow
exception: it publishes into a fresh empty private owner only after complete
success and returns a private integer status so the ordinary public wrapper can
map invalid alignment, arithmetic overflow, and resource exhaustion into
`std::alloc_errc`.

The allocation extent and initialized-slot map remain private safety
bookkeeping. The total `storage_capacity` and `storage_initialized` observers
exist only so `std::storage<T>` can provide checked public queries; they expose
neither an address nor mutation of the bookkeeping. A container still records
its own logical size and growth policy rather than treating allocation extent
as element count.
The shift operations move an initialized half-open slot range through one
verified empty slot. They expose no container size or insertion policy;
source-defined containers use them to preserve partial-initialization state
while implementing indexed insertion and erasure.

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

This facility belongs under the reserved `gti_internal` namespace and is
available only to trusted compiler and standard-library code. Applications use
the public `std::storage<T>` owner instead. Neither layer exposes an address,
raw-data escape, or independent deallocation operation.
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

The prelude also declares `gti_internal::prefix_storage<T>` for containers whose
live elements must always occupy the half-open prefix `[0, length)`. Its narrow
operations allocate capacity, append or pop the final live element, read within
the prefix, relocate the complete prefix, and insert or erase while preserving
that invariant atomically. Unlike sparse `storage<T>`, it cannot expose or
create an arbitrary initialized-slot pattern. Both capabilities are move-only,
reject borrowed-state elements, expose no raw address or manual deallocation,
and keep container policy such as public size, growth, and invalidation in
ordinary GTI source.

## Source-Defined Vector

`std::vector<T>`, imported with `#include <std/vector>`, is now an ordinary GTI
class over `gti_internal::prefix_storage<T>`. Its element type satisfies
`std::movable`; the storage field makes the vector itself move-only, so transfer
uses `std::move` and an ordinary copy is rejected. No frontend or backend phase
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
storage failure rather than exposing an unchecked indexing path.

`emplace_back(arguments...)` forwards one final by-value pack to
`storage_construct`, which selects the element's exact constructor and builds
the element in its final slot. It returns a writable receiver-tied reference to
that element. The reference prevents reserve, push, clear, movement, or another
mutable vector operation until its loan ends. This API avoids an intermediate
element value but deliberately does not promise C++ forwarding-reference
behavior; copyable arguments may be copied at the method boundary, while a
move-only pack is consumed once.

The read-only iterator retains one checked read-only reference to the backing
storage. `begin_mut()` returns a distinct ephemeral mutable iterator that
transfers the range's exclusive storage capability and uses checked mutable
dereference. The range statement adds its dedicated parent range loan and
per-iteration element child; an owned vector temporary may be transferred into
its hidden loop owner. Reserve, push, clear, movement, and every other
overlapping owner operation are rejected while the mutable range is active.
This is conservative whole-owner invalidation exclusion, not a
container-specific invalidation trait. Iterator categories, iterator-position
insertion/erasure, allocator policy, and mutable or multi-owner spans remain
future work. The bounded read-only single-owner span is described below.

## Owning Text

`std::string_view` is a counted read-only view. A literal-backed view is an
independent trivial value over static storage. `std::string::view()` produces
a dynamic view carrying one compiler-tracked read-only dependency on that
string's character storage. Both forms support `size()`, `empty()`, and checked
read-only indexing.

`std::string`, imported with `#include <std/string>`, is an ordinary nominal GTI
class over `gti_internal::prefix_storage<char>`. It supports construction and
append from a string view, capacity management, checked read-only and mutable
indexing, clear, comparison, explicit `clone()`, and read-only structural
iteration. Its source-defined iterator retains a checked read-only storage
borrow, so a live iterator prevents mutation, replacement, or movement of the
string. Because its storage field is a unique owner, the compiler derives a
move-only lifecycle for the class. This avoids a hidden allocation on
assignment; duplication is visible at the call site as `value.clone()`.

Dynamic views may be copied among local bindings and relayed from one exact
read-only parameter or receiver. Concrete generic instantiation derives this
same return dependency after substituting the exact parameter and return
types. A literal-backed argument remains an ownerless static view and needs no
runtime loan; a dynamic argument transfers its existing owner dependency to
the result. That retained loan prevents mutation, movement, or destruction of
the owner until the last local carrier ends. A dynamic view cannot escape a
local owner, originate from temporary storage, be assigned into existing
storage, or be nested in a field, global, static, array, or dynamic container.
Mutable, multiple-origin, nested, and reassignable view graphs remain
deliberately unsupported. These restrictions are semantic and survive through
HIR and verified MIR; emitted C++ does not decide the lifetime.

An owning wrapper may relay a field's text accessor, for example
`return this.text.view();`. The summary names the enclosing receiver (or one
read-only reference parameter for a free relay); body analysis must validate
that the selected call actually returns a borrow from that source. Merely
calling a method on an owner does not prove this. Local owners, by-value owning
parameters, and fresh temporary owners cannot supply an escaping view.

`std::string::c_string()` and `std::string::try_c_string()` use the same
single-owner dependency model for the compiler-owned `c_string` boundary type.
The unchecked spelling always returns the string's non-null terminated storage
and permits embedded NUL bytes; the checked spelling returns `nullptr` if an
embedded NUL is present. A retained result prevents mutation, movement, or
destruction of the source string and cannot escape a local owner. A `c_string`
received from native code has no inferred GTI owner, and hidden retention by a
native callee remains part of that API's external contract.
