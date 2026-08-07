# Ownership, References, And Allocation

GTI exposes explicit ownership without exposing raw pointer types, `new`, or
`delete`. The source surface stays familiar to C++ users, but ownership,
transfer, lifetime, and failure behavior are GTI language rules rather than
properties inherited from the active backend.

## Public Ownership Surface

The implemented unique ownership surface is:

```gti
std::unique_ptr<Entity> entity = std::make_unique<Entity>(arguments);
```

These names are standard-library API, not keywords. The current implementation
recognizes them directly because copyability, movement, destruction,
nullability, and dereference cannot yet be implemented safely as ordinary GTI
classes. That is a staging constraint: the intended public form is a nominal
standard-library class over narrower compiler-defined capabilities.

`std::make_unique<T>(arguments)` is a compiler-recognized construction
intrinsic. It validates `arguments` against a class or struct constructor
without introducing variadic generic functions into GTI. `std::make_shared`
remains planned.

Public GTI does not provide:

- raw `T*` types;
- pointer arithmetic or integer-to-pointer conversions;
- construction of an owner from an address;
- unchecked ownership casts;
- `unique_ptr::release()`;
- source-level `new` or `delete`.

## Capability Layers

GTI's memory model is designed in three layers:

1. Ordinary application code uses safe standard-library classes such as
   `std::unique_ptr<T>` and, later, `std::vector<T>`.
2. Those classes are implemented, as the class and generic systems mature, in
   GTI over compiler-defined capabilities in `gti_internal`, including
   ownership and partially initialized storage operations.
3. A future explicitly opt-in low-level API may expose a selected subset of
   those capabilities for systems and engine code that needs direct control.

The possible `dangerous` namespace is a policy direction, not settled public
spelling. This design does not yet commit GTI to raw pointer syntax,
source-level allocation and deallocation functions, or C++-style `new` and
`delete`. Any low-level surface must define its own lifetime, aliasing,
initialization, failure, and diagnostic contracts before it becomes public.

Intrinsic capabilities are identified semantically rather than by the names
of public wrappers. The C++ backend may implement one through C++ RAII or
aligned allocation while a future LLVM backend lowers the same operation
differently. Neither representation is part of the source language.

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
tables preserve source AST identities and will feed typed HIR without encoding
C++ representation choices in the frontend.

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
Read-only method returns may use `T&` when the returned place is derived from
`self`. The call result is a borrow tied to its receiver, so it cannot be
retained from a temporary receiver:

```gti
T& at(uint64 index) {
  return gti_internal::storage_read(self.data, index);
}
```

Retaining a borrow from a move-only receiver conservatively prevents moving or
replacing that receiver and calling its mutable methods for the remainder of
the function. This protects storage-backed references from reallocation. A
future lexical loan analysis can end that restriction at the borrow's last use
instead of the function boundary.

Free-function reference returns, mutable reference returns, stored references,
globals, nested references, and references over fixed arrays or owner handles
remain unavailable. Those forms require more general lifetime relationships
than the current receiver-tied rule.

## Ownership Transfer

`std::unique_ptr<T>` is move-only. Ownership-consuming calls, returns, and
assignments require the C++-familiar `std::move(owner)` intrinsic.

An immutable GTI binding may be consumed even though it cannot be reassigned or
mutated. The C++ backend must therefore not equate semantic immutability with
physical C++ `const` for move-only storage. GTI diagnostics enforce the source
rule, and the backend chooses a representation that permits the validated
transfer.

Use after transfer is a semantic error. Straight-line flow records moved owners;
branches merge owner state and report a later use when any reachable path moved
the owner. Loops conservatively account for zero or more iterations.

The first allocation layer permits unique owners as local bindings, parameters,
and return values. Direct unique-owner fields, globals, fixed arrays, references
to owner handles, and ordinary generic instantiations with owner arguments
remain unavailable. Classes containing compiler-private storage are supported:
their ownership traits propagate recursively through nested aggregates. Fully
general owner-containing generic instantiations still require ownership-aware
monomorphization.

The planned `std::shared_ptr<T>` surface will be copyable and movable. Copying
will add an owner; moving will transfer one handle. Shared ownership does not
solve cycles, so GTI also needs a non-owning weak observation type before shared
cyclic object graphs can be supported responsibly.

## Nullability And Allocation Failure

Owning pointers may be empty, including after movement. Boolean and `nullptr`
comparisons inspect that state. Dereference and member access on an empty owner
produce a defined GTI runtime failure rather than undefined behavior.

The default `make_unique` operation is infallible at the type level: the C++
backend catches native allocation failure and terminates with a stable GTI
runtime diagnostic.
Future `try_make_unique` and `try_make_shared` APIs may return `expected` for
programs that need recoverable allocation failure.

## Destruction

Owned bindings are destroyed at the end of their lexical scope in reverse
declaration order. Class fields are destroyed in reverse field declaration
order after the owning object finishes its destruction work. Temporaries are
dropped at a defined expression boundary that will become explicit in MIR.

Every class and struct now has explicit frontend lifecycle metadata. Declared
constructors form exact-match overload sets. The compiler independently derives
default construction, copy/move construction, copy/move assignment, and
destruction from field traits; adding an ordinary constructor does not suppress
movement as it can in C++.

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
transfer semantics. Custom copy and move lifecycle bodies remain unavailable.

The C++ backend emits generated operations explicitly as `= default`,
`= delete`, or an active-state move implementation. Its hidden active flag is a
representation of the GTI drop rule, not part of object layout or a stable ABI.

Field immutability is enforced by GTI semantic analysis rather than physical
C++ `const`. This permits assignment to replace a mutable whole-object binding
without making its immutable fields individually writable in GTI source.

The current semantic model marks types that require lexical destruction. Typed
HIR will assign stable values and symbols; MIR will make drop points, ownership
transfers, and control-flow cleanup explicit for every backend.

## Backend Boundary

The C++ backend currently represents unique ownership with C++ RAII:

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
but it does not define public pointer semantics.

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
gti_internal::allocate_storage<T>(uint64 capacity)
gti_internal::storage_capacity(storage)
gti_internal::storage_construct(storage, uint64 index, T value)
gti_internal::storage_read(storage, uint64 index)
gti_internal::storage_destroy(storage, uint64 index)
gti_internal::storage_relocate(source, destination, uint64 count)
```

`storage<T>` is an aligned, move-only lexical owner. It tracks which slots
contain live values, destroys those values in reverse slot order at scope exit,
and then releases the allocation. Construction, destruction, and relocation
require mutable storage. All index and slot-state failures terminate with a
stable GTI runtime diagnostic. Allocation failure follows the existing
infallible allocation policy and terminates with `memory allocation failed`.

`storage_read` returns a checked read-only borrow tied to its storage argument;
it does not copy the element. This lets a nominal container expose `T&` access
even when `T` is move-only while preventing the borrow from outliving the
container.

`storage_relocate` move-constructs the leading live elements into empty
destination slots and destroys the source elements. This gives a container a
single operation whose semantics can later lower to explicit MIR move and drop
instructions.

This facility currently belongs under the reserved `gti_internal` namespace
and is available only to trusted compiler and standard-library code. It
exposes no address, pointer arithmetic, raw-data escape, or independent
deallocation operation. Public code continues to use values, references, and
the standard owning pointer types without gaining raw memory access. A future
low-level API may deliberately re-export audited capabilities, but merely
adding an intrinsic does not make it public or stable.

The C++ backend represents storage with a private RAII helper built from aligned
allocation and explicit construction/destruction. This remains a backend
choice. A future LLVM backend can lower the same checked operations to MIR plus
narrow aligned allocation/deallocation runtime calls.

## Delivery Order

1. Ownership-aware expression and binding metadata. Implemented.
2. Non-null references with conservative non-escaping lifetime checks.
   Implemented.
3. Unique ownership, heap construction, checked dereference, and explicit
   movement. Implemented for local and function values.
4. Conservative flow-sensitive use-after-move diagnostics. Implemented.
5. Compiler-private uninitialized storage for containers. Implemented.
6. Aggregate ownership traits. Implemented.
7. Self-tied read-only method reference returns with conservative move-only
   receiver invalidation checks. Implemented.
8. Exact constructor overloads and explicit compiler-generated class lifecycle
   metadata. Implemented.
9. Source cleanup with noncopyable ownership and active-drop generated moves.
   Implemented.
10. Shared ownership and weak observation.
11. Explicit HIR and MIR ownership/drop operations shared by all backends.
