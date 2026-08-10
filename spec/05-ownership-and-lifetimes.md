# 5. Ownership And Lifetimes

Status: Ownership contract incorporated and summarized

The detailed implemented contract is
[`docs/ownership.md`](../docs/ownership.md). This chapter provides the normative
shape into which that contract will be migrated.

## 5.1 Ownership Categories

A GTI expression and binding carries semantic traits describing whether it is
a value, borrow, unique owner, or reserved shared owner; whether it is copyable
or movable; and whether it requires lexical destruction.

Class, struct, and fixed-array traits are derived structurally from their
elements after generic substitution. A value containing a move-only owner is
itself move-only unless a future rule states otherwise.

## 5.2 References

`T&` is a non-null read-only borrow. `mut T&` is a non-null writable borrow. A
reference does not own its referent, cannot be reseated, and cannot bind to a
temporary.

Reference parameters and local bindings require addressable places. Mutable
references additionally require writable places and exclusive access for the
duration of the loan.

A method reference return is valid only when its origin is derived from the
receiver. A mutable reference return requires a mutable receiver and a writable
returned place. A borrow from a temporary receiver may not escape that
temporary.

## 5.3 Stored Borrowing Values

The current confined stored-borrow carrier may contain one direct read-only
reference field initialized directly from one exact reference constructor
parameter. It is move-constructible, noncopyable, and nonassignable. Mutable or
multiple reference fields, nested or inherited borrowed state, user cleanup,
global or static storage, and free-function escape are outside this subset.

Retaining such a value retains a loan of its owner. Until the loan ends, the
owner cannot be moved, replaced, destroyed, or used through an invalidating
mutable operation.

This subset exists to support owner-tied library values such as iterators. It
does not define general lifetime parameters or arbitrary reference fields.

## 5.4 Raw Pointers

A raw pointer is not an owner or borrow. Copying, moving, assigning, passing,
or returning one copies an address without changing the pointed-to object's
lifetime or creating a loan. No destruction, deallocation, or retention follows
from the pointer type.

Consequently, the compiler does not prevent a referent from being moved,
destroyed, or invalidated merely because a raw pointer still contains its old
address. Any later unsafe access is valid only when the programmer has proved
that the applicable lifetime and memory conditions still hold.

A safe abstraction may store a raw native handle and perform pointer-bearing C
calls inside reviewed unsafe blocks. The abstraction must preserve its
invariants for every safe input, keep ownership separate from the pointer
value, and use ordinary GTI lifecycle rules to ensure exactly-once cleanup.
Exposing a safe wrapper does not transfer undocumented proof obligations to its
caller.

The detailed ownership separation and RAII pattern are incorporated from
[`docs/raw-pointers.md`](../docs/raw-pointers.md) and
[`docs/ownership.md`](../docs/ownership.md).

## 5.5 Explicit Movement

`std::move(value)` is a compiler-defined move operation with a function-like
public spelling. It accepts an available movable named local or by-value
parameter. It produces a value of the same type and changes the source binding
to the consumed state.

The source-state transition is:

| Initial state | Operation | Resulting source state |
| --- | --- | --- |
| Available | `std::move(source)` | Consumed |
| Consumed mutable binding | valid plain assignment | Available |
| Consumed immutable binding | any read or reinitialization | Ill-formed |

Reading a consumed binding is ill-formed. Moving a copyable value is permitted
and still consumes its source. References, globals, fields, captures,
temporaries, and partial places are not valid sources in the current subset.
Direct self-move assignment is ill-formed.

Movement is represented as a language operation in HIR/MIR. Emitting C++
`std::move` is a backend choice.

For same-type initialization and by-value argument passing, an available place
requires copy construction and an explicit moved value requires move
construction. GTI does not fall back from an unavailable move to a copy. A
class or struct may independently default or delete either construction
operation with the public policy forms `Type(Type&) = default|delete;` and
`Type(Type&&) = default|delete;`. The first parameter is a read-only copy source;
the second spelling is confined to this policy and does not create a storable
rvalue reference.

Defaulting a policy cannot override a noncopyable or nonmovable base, field,
stored borrow, or cleanup obligation. Deleting a policy updates the containing
type's structural traits, including after generic substitution and when nested
in another aggregate. Custom lifecycle bodies are deferred until the ownership
model can prove field-place moves and cleanup after partial initialization.

## 5.6 Unique Ownership

`std::unique_ptr<T>` is a nominal source-defined standard-library owner over a
compiler-private unique-owner capability. It may be empty, is noncopyable, and
is explicitly movable. Dereference and member access borrow the owned value and
produce a defined runtime failure when the owner is empty.

`std::make_unique<T>(arguments...)` is an ordinary generic standard-library
factory. Its public name is not an allocation intrinsic. Allocation failure in
the current infallible API produces a defined runtime failure; a recoverable
factory may return `expected` in a later library layer.

## 5.7 Partially Initialized Storage

Compiler-private `gti_internal::storage<T>` owns aligned capacity whose slots
may be initialized independently. Its checked capabilities construct, borrow,
destroy, and relocate elements while retaining slot-state and ownership
invariants. Logical container size and capacity are public-wrapper policy and
are not intrinsic state.

Construction accepts zero or more exact constructor arguments and creates the
element directly in its destination slot. Class elements select one accessible
constructor during GTI semantic analysis. Primitive elements accept either
zero arguments for value initialization or one value of the exact element
type. No implicit conversion or backend constructor selection is permitted.
Relocation requires a movable element type.

Storage elements may not retain borrowed state until general owner
dependencies can be represented. This restriction applies equally when a
concrete storage type appears directly and when it is reached through a
source-defined container.

Private storage operations are not a public raw-memory or deallocation API.
Their C++ RAII representation is non-normative.

## 5.8 Destruction

Every live owner is destroyed automatically at its specified scope or field
boundary. Movement transfers the applicable cleanup obligation. A moved-from
value does not execute transferred user cleanup, but its remaining active
fields are still destroyed according to their state.

A source cleanup body makes its type noncopyable. Generated move construction
and assignment preserve exactly-once cleanup and destroy or clean an active
assignment target before replacement.

## 5.9 Lifetime Gaps

Before general views, iterators, callbacks, and broader unsafe memory can be
stable,
the specification must define:

- precise last-use loan endings and reborrowing;
- owner dependencies carried through calls and returns;
- multiple and mutable stored borrows;
- field and index moves with partial initialization;
- temporary ownership and drop boundaries;
- escaping and reference-capturing callables;
- allocator provenance and manual object lifetime; and
- proof obligations for any future casts, pointer-to-pointer APIs, native
  layouts, callbacks, allocation, or manual-lifetime surface beyond the
  implemented one-level pointer contract.
