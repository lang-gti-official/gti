# ADR 022: Safe Typed Allocation And Storage Provenance

Status: Accepted

Scope note (2026-10-07): the initial decision below describes the first checked
typed-storage slice. Its exclusions and private implementation choices are not
permanent restrictions on GTI's independent systems-language goal. The later
low-level, optional-audit and programmable-resource revisions supersede the
corresponding initial boundaries. The general object-lifetime and source-storage
revision below supersedes the private sparse capability and hidden bitmap.
Historical descriptions in the initial decision record their original scope;
they are not the current implementation contract or a claim of final campaign
validation.

## Context

Source-prefix revision (2026-10-08): the owner approved one region per
`resource_vector` capacity reservation. Its live objects are a prefix tracked
by one source length, so a separate live-slot bitmap and metadata allocation
are unnecessary. This changes observable vector provider request/release
counts, not raw provider obligations, checked loans, relocation, failure
atomicity or element-before-region cleanup. Public sparse `resource_storage`
and `storage` still require their two regions. Do not retain a useless metadata
allocation solely to imitate the former vector implementation.

GTI needs allocator-controlled storage for arenas, pools, containers, and
game-engine workloads. Fixed arrays cannot represent spare or partially
initialized capacity, while `std::unique_ptr<T>` represents one initialized
object rather than a set of independently live slots. Exposing C++-style
`new`/`delete`, an address/count/alignment deallocation tuple, or an untyped
byte allocator would make application code responsible for provenance,
initialization, aliasing, cleanup, and failure invariants that GTI otherwise
enforces.

The first public slice must therefore answer four questions together:

1. who owns the allocation and the authority to release it;
2. which type and alignment the allocation is valid for;
3. which slots currently contain live values; and
4. how allocation and checked storage failures interact with deterministic
   cleanup.

The closest precedents separate these concerns but leave different parts
unsafe or conventional:

- Rust's `Allocator` returns a fallible byte allocation and requires an unsafe
  matching deallocation; `MaybeUninit<T>` represents initialization state but
  safe code must not perform `assume_init` until it has separately proved the
  value valid. Its documentation explicitly makes element-by-element cleanup
  the programmer's responsibility.
- Zig conventionally passes an allocator into allocating functions, exposes
  allocation failure in the error set, and recommends arenas for request or
  frame lifetimes. Ownership and the point at which returned pointers become
  invalid remain API contracts rather than language-tracked provenance.
- Swift typed allocation binds memory to a pointee type, but direct allocation,
  initialization, deinitialization, pointer arithmetic, and deallocation are
  still unsafe manual operations.
- C++ allocators return pointer-like values to unconstructed elements,
  allocation may throw, a zero-count result is unspecified, and deallocation
  requires the original pointer plus a matching count and allocator
  relationship. Construction and destruction are separate lifetime operations
  whose correct pairing is not carried by the allocation result type.

GTI adopts the useful separation of allocation from construction and Zig's
explicit recoverable-failure bias, but makes the allocation result itself a
typed lexical owner with checked initialization state.

Primary references used for this comparison are the
[Rust `Allocator`](https://doc.rust-lang.org/std/alloc/trait.Allocator.html) and
[`MaybeUninit`](https://doc.rust-lang.org/std/mem/union.MaybeUninit.html)
documentation, the
[Zig allocator guidance](https://ziglang.org/documentation/master/#Choosing-an-Allocator),
Apple's
[typed-pointer initialization guidance](https://developer.apple.com/documentation/swift/unsafemutablepointer/initialize%28to%3A%29),
and the C++ draft
[`Allocator` requirements](https://eel.is/c++draft/allocator.requirements).

## Decision

### 1. The public surface is typed and lives directly in `std`

The first public API is:

```gti
std::alloc_errc
std::alignment
std::allocator
std::storage<T>
```

There is no public `std::mem` namespace, raw allocator handle, allocation
address, byte slice, `new`, `delete`, `deallocate`, or unchecked
slot operation.
The general-memory extension adds `storage.release()` as explicit owning
cleanup, not as detachment of a raw allocation from its release authority.

`std::allocator` is initially a stateless process allocator. It creates typed
storage through two generic methods:

```gti
expected<std::storage<T>, std::alloc_errc>
allocator.try_storage<T>(size_t count)

expected<std::storage<T>, std::alloc_errc>
allocator.try_storage<T>(size_t count, std::alignment minimum)
```

The returned owner does not borrow the allocator. All release authority moves
into `std::storage<T>`, so the allocator value may end immediately after a
successful call.

### 2. `std::storage<T>` is the allocation and provenance token

A `std::storage<T>` value uniquely owns one allocation origin and its metadata:
the typed capacity, effective alignment, initialized-slot map, and authority to
destroy remaining values and release the allocation. It is move-only. Moving
it transfers the same allocation origin and invalidates use of the moved-from
owner; copying or constructing one without a successful allocator result is
rejected.

This is capability-based ownership rather than pointer ownership. Application
code cannot separate the address from the release authority, manufacture an
owner from an integer or pointer, or accidentally deallocate with a mismatched
count, alignment, or allocator.

The first public slice requires `T` to satisfy `std::movable` and rejects
element types containing borrowed state. This is the exact boundary needed to
move a completed value into a slot without exposing constructor argument packs
or an untracked placement-construction failure edge.

The public operations are:

```gti
size_t storage.capacity()
bool storage.initialized(size_t index)
void storage.construct(size_t index, T value) mut
void storage.destroy(size_t index) mut
T& storage.get(size_t index)
mut T& storage.get_mut(size_t index) mut
void storage.clear() mut
void storage.release() mut
```

Capacity is allocation extent, not logical container size. A container or
arena built over storage retains its own public policy such as length, growth,
free-list order, and invalidation rules.

### 3. Slots have checked initialization state and lifetime epochs

Each index in `[0, capacity)` is either uninitialized or contains exactly one
live `T`:

- `construct` requires an uninitialized in-bounds slot, consumes one completed
  `T`, and begins a new slot lifetime only after its move succeeds;
- `get` and `get_mut` require a live in-bounds slot and return references tied
  to the storage receiver;
- `destroy` requires a live in-bounds slot, ends that value's lifetime exactly
  once, and makes the slot available for a later, distinct lifetime; and
- `clear` destroys every live value once in descending index order; and
- `release` performs owning cleanup and also releases the allocation, leaving
  capacity zero. It installs an empty receiver before cleanup of the detached
  owner, preserving exactly-once cleanup even when an element destructor fails.

Destroying the owner performs the same reverse-slot cleanup for every value
still live and then releases the allocation. A slot lifetime is not revived by
reusing its index: references from an earlier slot epoch cannot be used after
destruction and reconstruction. The first implementation enforces a
conservative owner-wide borrow, so any live returned reference blocks storage
mutation or movement even when another index would be disjoint. Later indexed
place precision may relax that conservatism without changing this contract.

Types containing borrowed state are not valid storage elements in this slice.
The owner cannot prove that independently stored references remain valid for
the allocation's whole lifetime.

### 4. Allocation is failure-atomic and recoverable

`try_storage` returns one of:

```gti
enum class std::alloc_errc {
  invalid_alignment,
  size_overflow,
  resource_exhausted,
};
```

No owner is published until allocation storage and all private initialization
metadata have succeeded. On failure, no slot is live, no application-visible
state changes, and temporary backend resources are released.

`size_overflow` covers a count that cannot be represented by the target or for
which `count * sizeof(T)` plus alignment padding overflows. It is distinct from
host resource exhaustion. Explicit alignment must be a nonzero power of two;
natural alignment is requested by omitting the alignment argument. The
effective alignment is at least both `alignof(T)` and the requested minimum.

A zero-count request succeeds, owns no host allocation, and returns a value
whose capacity is zero. The `expected` value state distinguishes that success
from failure; no special pointer sentinel or unspecified zero-allocation result
is exposed.

Checked bounds, duplicate construction, uninitialized access, and
uninitialized destruction follow GTI's defined runtime-failure and cleanup
contract. They are programming-state violations, not allocation errors.

The first public operation consumes an already constructed `T` instead of
forwarding an arbitrary constructor argument pack. Construction of that value
therefore completes before the storage operation begins. If it raises defined
failure, no new slot lifetime is published and ordinary MIR cleanup destroys
every previously live owner, including the storage and its initialized slots.
The final-address extension adds `emplace<Args...>(index, args...)`. It reuses
the MIR constructor-failure protocol: a non-consuming slot preflight precedes
parameter handoff, and the exact selected constructor runs at the final slot
address. Partial members are rolled back on failure without publishing a live
slot. The original constructor failure is propagated, not translated to an
allocation error. A successful call returns a receiver-tied exclusive borrow.

### 5. Public policy remains ordinary GTI source

The structural-relocation extension exposes `storage<T>::relocate_to` over
the existing declaration-bound storage transfer. It moves a checked initialized
prefix into empty slots in a distinct destination owner, retaining both
allocations. This enables ordinary GTI growable typed-storage policy without
raw addresses or manual lifetime reconstruction. It deliberately does not add
replacement, overlap, or allocation failure to relocation itself. All state
validation precedes transfer, and both owners remain exclusively borrowed.

The public classes and methods in `<std/memory>` are ordinary source-defined
GTI. No compiler or backend phase recognizes `std::allocator`,
`std::storage<T>`, `std::alignment`, or `std::alloc_errc` by spelling.

Their implementation uses exact declaration-bound `gti_internal` operations
for the current allocation, alignment, bitmap, construction, destruction,
and checked-reference mechanics. These responsibilities are not all inherently
irreducible: raw byte acquisition and bookkeeping can be source-defined policy,
whereas typed lifecycle and checked-reference facts need language/compiler
authority. Reassess private placement per operation as public facilities grow.
The current declarations are available only to the trusted prelude and installed
standard-library sources. HIR and MIR retain
their selected intrinsic identities and effects, and the backend receives
only verified typed operations.

### 6. Arenas and pools compose over storage; they do not weaken it

A homogeneous arena can own `std::storage<T>`, monotonically choose an empty
slot, construct a value, and clear all remaining values at arena teardown. A
pool can additionally keep an ordinary GTI free list and use `initialized` to
check its internal invariant. Neither needs an exposed allocation address or
manual deallocation.

The implemented first policy clients are fixed-capacity `std::arena<T>` and
`std::pool<T>`. Their `try_make_*` factories retain allocation failure as
`alloc_errc`; insertion has the separate recoverable `arena_errc::full` or
`pool_errc::full` outcome. The arena chooses positions monotonically and can
destroy all values before reset. The pool chooses the lowest empty position
and supports checked erase and reuse. Both are move-only and use receiver-tied
borrows and reverse-slot cleanup inherited from storage.

Their returned `size_t` positions deliberately are not generation-bearing
handles. Reset or erase ends the current slot lifetime, and reuse may assign
the same numeric position to another value. Stateful allocator ownership,
generation-safe handles, allocator-aware growth/propagation, and heterogeneous
arenas were follow-up goals for this initial slice. They should use ordinary GTI
policy over coherent general facilities, including explicitly unsafe raw
operations where needed. The later low-level and programmable-resource revisions
extend the original typed-owner-only boundary.

## Why This Is Safer Than The C++ Model

The claim is narrower than “GTI allocation is more flexible than C++.” It is
not. The first GTI surface deliberately trades low-level generality for
enforceable invariants:

- the owner carries pointer, count, alignment, initialization map, and release
  authority together instead of asking callers to reproduce a matching
  deallocation tuple;
- typed provenance prevents cross-type access and forged owner creation;
- checked initialization state prevents safe reads of unconstructed objects,
  duplicate construction, and duplicate destruction;
- receiver-tied borrows prevent move, destruction, or mutation while a
  returned reference may remain live;
- recoverable allocation uses `expected` rather than a hidden exception or
  process termination; and
- lexical reverse cleanup covers partially initialized allocations on every
  supported normal and failure exit, including failure while constructing the
  value passed to `construct`.

The reference backend currently implements these operations using C++. Its
pointer and object-lifetime rules are representation mechanics, not GTI source
semantics. GTI unsafe code and general public low-level facilities are the
source-language extension boundary; missing source capabilities are goals to
implement, not reasons to make host C++ the permanent user escape mechanism.

## Alternatives

- **Expose `allocate`/`deallocate` and typed pointers as the initial safe API.**
  Rejected for that slice because the caller could lose or forge the
  provenance/count/alignment relationship and
  must manually pair every live object with destruction.
- **Expose a public `MaybeUninit<T>`-style value.** Rejected for this slice
  because an unchecked `assume_init` would move the crucial proof into
  `unsafe`; storage can track the finite slot state directly. This does not
  reject a general unsafe uninitialized-storage facility as a future goal.
- **Use `std::allocator<T>` and C++ allocator traits.** Rejected because rebind,
  equality, propagation traits, fancy pointers, and container-specific copy
  rules are not required to create the first safe arena/pool substrate.
- **Put the surface under `std::mem`.** Rejected because it adds syntax without
  introducing a separate policy or safety boundary; the public names are
  already unambiguous in `std`.
- **Make arenas part of S-ALLOC-02.** Rejected because allocation/provenance is
  a reusable primitive while reset generations, free lists, handle validity,
  and container policy require their own acceptance client.

## Consequences

- GTI can allocate and partially initialize typed capacity from safe
  application code without exposing raw memory management.
- The public surface now includes homogeneous fixed-capacity arenas and pools
  and remains suitable for future allocator-aware containers, but not
  heterogeneous byte arenas, custom
  placement layouts, memory mapping, DMA, or native buffer adoption.
- Owner-wide borrowing is intentionally conservative. Per-slot disjoint
  mutable borrows require indexed place/epoch precision and must preserve this
  API's stale-reference guarantees.
- Stateful allocators, allocator propagation,
  bulk initialization, growth, shrinking, and generation-bearing pool handles
  are later coherent slices rather than optional parameters on this one.

## General low-level extension

The accepted Phase 1 campaign (`S-ALLOC-04`) revises the original exclusion of
public raw allocation and manual object lifetime. Checked storage remains the
safe default; general facilities must additionally permit application-defined
owners and raw-backed views. Raw addresses alone grant neither ownership nor
checked lifetime. An explicit unsafe owner-witness bridge, retained known
provenance, typed layout/allocation authority and verified lifecycle operations
are required together. The final-address constructor protocol above is a
prerequisite, not completion of that campaign. `std::assume_ref` and
`std::assume_mut` now expose the unsafe owner-witness assertion through checked
MIR loans as ordinary GTI definitions using public `borrowed_from` contracts,
with source-wrapper, receiver-field and loop requirements. Concrete ordinary
source-result queries retain callee-produced raw origins; unsupported query
shapes fail closed at memory access rather than losing known facts. Built-in address
formation itself is safe when its operand is safe to evaluate. Explicit
allocation release now uses ordinary GTI movement and MIR cleanup. The
existing typed allocation request supplies ordinary-object layout authority,
including owning class elements; this does not expand constexpr layout queries
or promise a stable foreign ABI. Public erased-address conversions preserve
constness, known pointee types and origins without creating a checked loan.
Conservative source-access/write summaries and byte-preserving native-record
transport are implemented: a safe inert copy does not evaluate an expired
native pointer subobject. Ordinary source calls, callbacks, virtual families,
Expected/enum alternatives and storage transport have focused native evidence.
The full completion suite and installed/tooling checks complement those
focused checks; the local evidence is recorded in
[verification](../architecture/verification.md#low-level-memory-phase-1).
Stateful allocator policies and resource leases were deferred to Phase 2; see
[the programmable resource revision](#revision-programmable-resources-phase-2).

The Phase 2 storage extension removes the public movable-element restriction
from allocation and final-address emplacement. Handle movement transfers the
same slots without moving their objects. Completed-value construction and
element relocation keep their distinct capability requirements. The existing
MIR constructor/preflight and cleanup engines already represent those facts;
there is no second immovable-object lifecycle. Example 120 demonstrates the
distinction with an ordinary owning class whose copy and move are deleted.

## Revision: optional unsafe auditing

The Phase 1 provenance analysis exposed an inappropriate acceptance boundary:
a valid raw-pointer relay could fail solely because a bounded query was
incomplete, while reordering definitions warmed its cache and accepted the
same program. An ordinary aggregate mutator was likewise treated as definitely
ending an object's lifetime despite only conservative effect knowledge.

The revised boundary keeps typing, constness, unsafe permission, checked
ownership/loan/escape contracts and all HIR/MIR verification mandatory. The
programmer explicitly assumes raw validity, alignment, initialization, aliasing
and owner association at unsafe access or a raw-to-checked bridge. Safe return
annotations alone cannot relabel unrelated lifetimes. The resulting checked
reference remains compiler-tracked, including retained owner dependencies.

Raw provenance/access/effect checking is now optional (`off` by default,
`warn`, or CI-oriented `strict`). Disabled analysis performs no source-summary
queries. Enabled analysis distinguishes observed contradictions, potential
violations and incomplete inference. Strict promotes findings without changing
inference or runtime behavior. This is neither an unchecked release mode nor
permission for undefined behavior; it makes the guarantee/obligation boundary
explicit and avoids treating a bounded audit as a complete proof system.

Independent per-demand query budgets and root-only reuse also remove the
declaration-order bug within the optional analyzer itself. Disabling the audit
is not the fix for that bug. Existing useful Phase 1 storage, placement,
release, transport and owner-witness facilities are retained.

## Revision: programmable resources (Phase 2)

Phase 2 (`S-ALLOC-05`) lets applications implement allocation policy in
ordinary GTI and supply it to typed owners and a growable container. The
accepted design, chosen with the owner on 2026-10-06:

- **Small structural protocol.** `memory_resource<R>` requires read-only
  `unsafe allocate(layout) -> expected<raw_allocation, alloc_errc>` and
  `unsafe release(raw_allocation)`. Resources are selected by generic type
  argument. `raw_allocation` is passive address/extent/alignment/identity
  metadata. There is no traits, propagation, fancy-pointer or runtime-erasure
  framework, and nothing in the protocol assumes a process heap.
- **One explicit trust step.** `unsafe bind_resource(resource)` is the caller's
  single acknowledgement that the provider honours its documented raw-memory
  contract. The sealed `resource_ref<R>` it returns is an ordinary checked
  shared borrow; later storage and container use is safe. Concept satisfaction
  and the optional raw audit check signatures and effects, never allocator
  correctness.
- **Statically borrowed, move-only leases.** Each typed owner holds a lease
  that retains its resource borrow through final cleanup and owns exactly one
  region. Several leases coexist without an exclusive borrow of the resource.
  A safe reset or move of the resource needs exclusive access and is rejected
  while any lease is live. Element references borrow the owner as well, so
  growth, release and movement are rejected until their last use. No reference
  counting, shared ownership or broad interior mutability is introduced:
  bookkeeping mutation goes through a provider's own raw pointers under its
  unsafe contract.
- **Existing lifecycle engine.** Typed owners adopt a provider-selected region
  through a compiler-private, declaration-bound capability, then reuse the
  established final-address construction, partial-construction rollback,
  destruction and failure machinery. Elements are destroyed before the lease
  returns the original descriptor to the original resource.
- **General language support, not client exceptions.** The prerequisites are
  general rules proven with unrelated types: unsafe member functions and unsafe
  structural requirements, runtime object-layout queries for ordinary owning and
  immovable types, and destructor-bearing borrowed classes whose dependencies
  persist through cleanup, movement and constructor rollback. The compiler
  recognises no public resource or container name.
- **Small differing-resource rule.** Owners never rebind: moving an owner moves
  its binding with it, assignment between owners is unavailable, and explicit
  relocation between distinct resource instances leaves each region released
  through its own binding.

Rejected alternatives: per-allocation `unsafe` (it repeats the same
obligation at every safe call site); runtime lease counting or shared ownership
of the resource (hidden dynamic cost and a second ownership model); an opaque
request that always delegates to the process heap (no placement control); and
a C++-style allocator-traits/propagation framework (large surface with no
current client). The implemented contract, its remaining unsafe obligations and
its structural limits are specified in
[raw pointers](../language/raw-pointers.md#explicit-resource-binding) and the
[standard library](../language/standard-library.md#custom-resource-protocol).

## Revision: source-owned process byte-provider policy

The independent first `S-ALLOC-07` slice revises the process resource's aligned
C++ allocation implementation. Request validation, native-size limits, empty
requests, descriptor publication and error mapping now live in GTI. Hosted
POSIX targets acquire bytes with `posix_memalign` and release with `free`;
Windows uses `_aligned_malloc` and `_aligned_free`. Their pairing is immutable
target policy, not an inferred property of the address. No object lifetime is
started by acquiring bytes, and release requires that all live objects have
already ended.

The original adapter rationale included missing native `size_t` and `void**`
support. That rationale is superseded: GTI now expresses both through general
C interop. The uniquely named fixed-width v1 ABI is retained because GTI still
permits only one declaration of each C symbol across a program. It lets an
application bind `posix_memalign` and `free` independently of the private
provider. Native errno/size/alignment facts and reserved Windows CRT symbol
spellings still justify small adapters. They neither validate GTI requests nor
own storage/bitmap policy; checked source limits precede native conversions.
The subsequent general-lifetime revision below migrates sparse owners, their
metadata and default typed factories onto that source-owned provider policy.

## Revision: general object lifetimes and source-owned sparse storage

The original sparse capability combined irreducible object-lifetime facts with
expressible allocation and slot policy. General public `std::uninitialized<T>`,
`construct_at`, `destroy_at` and `relocate_at`, plus the existing checked owner
witness, now separate these responsibilities. An unrelated application-defined
inline owner or pool can use the same facilities; no public container spelling
grants compiler authority.

The typed inline carrier has no engagement flag and performs no automatic
payload destruction. Ordinary movement is limited to structurally bitwise-
relocatable payloads; copying additionally requires copyability. Other carriers
remain fixed-address, including when empty. Explicit typed relocation transfers
eligible live payloads between distinct vacant regions with verified generated
lifecycle semantics, rather than treating owning representation as bytes.

`std::storage<T>` and `resource_storage<T,R>` now implement validation,
initialization state, construction publication, reverse traversal and relocation
loops in GTI. Nonempty storage requests object bytes and then a separate region
of one metadata byte per slot from the same bound resource. Zero capacity
requests neither. Failed metadata acquisition or descriptor validation releases
every acquired region before reporting failure. A slot becomes live only after
construction succeeds; destruction retires its flag before cleanup. A source
guard resumes reverse traversal after a first failure, preserving its record.
Elements are destroyed before metadata and object leases release their original
descriptors, in that order.

This revises the earlier single-region/hidden-process-bitmap implementation and
makes the two resource requests observable library policy. Region shape checks
do not establish provenance, real extent, non-overlap, writability or race
freedom. The one-time unsafe resource binding retains those explicit caller
obligations. Checked provider/element loans remain mandatory.

The private sparse type, allocation/adoption/access/lifecycle intrinsic family,
adoption marker and native helper engine are retired. Prefix/optional storage
remain distinct capabilities with their existing contracts. MIR still owns
typed lifecycle verification and failure cleanup; the backend implements those
verified operations. Sparse initialization-set optimization is retired with its
old intrinsic clients rather than being inferred from new source wrappers.
This revision records implementation authority, not completion of production,
cross-platform, installed-consumer or publication gates.
