# Ownership, References, And Allocation

GTI exposes explicit ownership without exposing raw pointer types, `new`, or
`delete`. The source surface stays familiar to C++ users, but ownership,
transfer, lifetime, and failure behavior are GTI language rules rather than
properties inherited from the active backend.

## Public Ownership Surface

The planned public owning types are:

```gti
std::unique_ptr<Entity> entity = std::make_unique<Entity>(arguments);
std::shared_ptr<Texture> texture = std::make_shared<Texture>(arguments);
```

These names are standard-library API, not keywords. They are compiler-known
types because copyability, movement, destruction, nullability, and dereference
cannot be implemented safely as unconstrained ordinary generic classes.

`std::make_unique<T>(arguments)` and `std::make_shared<T>(arguments)` are
compiler-recognized construction intrinsics. They validate `arguments` against
`T`'s constructor without introducing variadic generic functions into GTI.

Public GTI does not provide:

- raw `T*` types;
- pointer arithmetic or integer-to-pointer conversions;
- construction of an owner from an address;
- unchecked ownership casts;
- `unique_ptr::release()`;
- source-level `new` or `delete`.

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

The semantic type system reserves representations for borrowed references,
unique owners, and shared owners. They are not source-reachable until the
corresponding syntax, diagnostics, and lowering phases are implemented.

## References

The planned reference syntax is:

```gti
void render(Entity& entity);
void update(mut Entity& entity);
```

`T&` is a non-null, read-only borrow. `mut T&` is a non-null, writable borrow.
A reference does not own its referent and cannot be reseated. References cannot
bind to temporaries or outlive the storage they borrow.

Initial reference support will be restricted to parameters and non-escaping
local borrows. Returning and storing references requires lifetime relationships
to be represented explicitly. A method returning a reference will eventually
be able to tie that lifetime to `self`.

## Ownership Transfer

`std::unique_ptr<T>` is move-only. Ownership-consuming calls and assignments
must make transfer explicit. The source spelling for that operation will be
finalized with the first move-checking implementation; a C++-familiar
`std::move(owner)` intrinsic is the current direction.

An immutable GTI binding may be consumed even though it cannot be reassigned or
mutated. The C++ backend must therefore not equate semantic immutability with
physical C++ `const` for move-only storage. GTI diagnostics enforce the source
rule, and the backend chooses a representation that permits the validated
transfer.

Use after transfer is a semantic error. Flow-sensitive ownership state will be
tracked before unique owners become source-reachable.

`std::shared_ptr<T>` is copyable and movable. Copying adds an owner; moving
transfers one handle. Shared ownership does not solve cycles. GTI will need a
non-owning weak observation type before shared cyclic object graphs are
recommended, even though unique and shared pointers remain the only public
owning pointer categories.

## Nullability And Allocation Failure

Owning pointers may be empty, including after movement. Boolean and `nullptr`
comparisons inspect that state. Dereference and member access on an empty owner
produce a defined GTI runtime failure rather than undefined behavior.

The default `make_unique` and `make_shared` operations are infallible at the
type level: allocation failure terminates with a stable GTI runtime diagnostic.
Future `try_make_unique` and `try_make_shared` APIs may return `expected` for
programs that need recoverable allocation failure.

## Destruction

Owned bindings are destroyed at the end of their lexical scope in reverse
declaration order. Class fields are destroyed in reverse field declaration
order after the owning object finishes its destruction work. Temporaries are
dropped at a defined expression boundary that will become explicit in MIR.

The current semantic model marks types that require lexical destruction. Typed
HIR will assign stable values and symbols; MIR will make drop points, ownership
transfers, and control-flow cleanup explicit for every backend.

## Backend Boundary

The C++ backend initially represents GTI ownership with C++ RAII:

```cpp
std::unique_ptr<T>
std::shared_ptr<T>
std::make_unique<T>(arguments...)
std::make_shared<T>(arguments...)
```

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
always initialized. The standard library will therefore need a compiler-private
storage facility with allocation, alignment, construction, destruction, and
deallocation operations.

That facility belongs under `gti_internal` and is not available to ordinary
programs. Public code continues to use values, references, and the standard
owning pointer types without gaining raw memory access.

## Delivery Order

1. Ownership-aware expression and binding metadata.
2. Non-null references with conservative non-escaping lifetime checks.
3. Unique ownership, heap construction, dereference, and explicit movement.
4. Flow-sensitive use-after-move diagnostics.
5. Shared ownership and weak observation.
6. Compiler-private uninitialized storage for containers.
7. Explicit HIR and MIR ownership/drop operations shared by all backends.
