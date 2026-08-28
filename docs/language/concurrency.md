# Concurrency And Atomic Values

Status: Current language contract for the bounded production concurrent
profile: owned managed threads, ordered scalar atomics, and scoped mutex
guards.

Rationale is recorded in
[ADR 008](../decisions/008-safe-concurrency-memory-model.md). Remaining
condition-variable, scoped-thread, and foreign-entry work is tracked in the
[concurrency plan](../plans/concurrency-memory-model.md).

## Profiles And Availability

GTI has `single-threaded` and `concurrent` execution profiles. The former is
the default. Direct compilation selects the latter with:

```sh
gti --execution-profile concurrent program.gti -o program
```

A project profile uses `execution-profile = "concurrent"`. Selection occurs
before semantic analysis and is preserved through HIR and MIR. Source imports,
native flags, incidental host threads, and emitted C++ never infer it.

`std::thread`, `std::thread_result<Result>`, `std::atomic<T>`, and
`std::mutex<T>` require the concurrent profile and a target advertising
thread-capable concurrency support. Merely importing `<std/thread>`,
`<std/atomic>`, or `<std/mutex>` without constructing or calling their public
values remains valid in the default profile and does not select native
synchronization support. An executable use fails in semantics with
`GTI-S2084` when either requirement is absent.

## Data-Race Freedom

One memory location is a live scalar object or non-overlapping scalar
subobject. Two accesses conflict when they touch the same location or
overlapping lifetime and at least one is a write or lifetime operation. A data
race occurs when different threads perform conflicting accesses, at least one
is non-atomic, and neither happens-before the other.

Safe GTI makes such a race unrepresentable through owned task transfer,
transfer/share capabilities, the concurrent global policy, loans, and exact
synchronization operations. A race manufactured through an unsafe raw pointer,
an unsound unsafe capability assertion, retained native state, or foreign code
violates that boundary's obligation and is undefined behavior. `unsafe` does
not turn a race into a defined nondeterministic result.

`mut` is local mutation permission. It is not atomic, volatile, synchronized,
transfer-capable, or share-capable. Safe shared mutation requires an approved
interior-synchronization type.

## Managed Threads

`<std/thread>` provides the move-only `std::thread` and
`std::thread_result<Result>` owners. A task and every capture cross by owned
transfer exactly once. References, stored borrowed state, raw pointers, and
non-transfer-capable values do not cross. An outstanding handle automatically
joins during deterministic cleanup; detach is absent.

Successful spawn orders prior evaluations before task entry. Task completion
orders before successful explicit or automatic join continuation. Worker
defined failures are contained, task-owned state is cleaned, and the exact
failure record is re-raised by join. See
[Execution section 4.9](execution.md#49-concurrency-boundary) for the complete
failure and cleanup boundary.

`Result` may be a transfer-safe owned class. A successful explicit `join()`
returns `expected<Result, thread_errc>` and owns the completed result until it
is extracted, for example with `std::move(joined).value()`. Cleanup-owning
results are armed exactly once when join succeeds; extraction transfers that
obligation to the destination, while an unobserved result is destroyed during
automatic handle cleanup.

## Atomic Scalar Domain

`<std/atomic>` provides `std::atomic<T>` when `T` satisfies
`std::atomic_scalar<T>`. The closed domain is:

- `bool`;
- `int8_t`, `int16_t`, `int32_t`, and `int64_t`; and
- `uint8_t`, `uint16_t`, `uint32_t`, and `uint64_t`.

Floating types, enums, pointers, references, owners, aggregates, interfaces,
and arbitrary native records are rejected before lowering. The wrapper is
move-only, transfer-capable, and share-capable. It promises neither lock
freedom, stable object layout, nor C/C++ ABI compatibility.

An immutable binding may call atomic operations because the wrapper provides
compiler-validated synchronized interior mutation. In the concurrent profile,
this permits an immutable, share-capable namespace global such as:

```gti
#include <std/atomic>

std::atomic<int32_t> requests{0};
```

Global and static bindings cannot be consumed with `std::move`, and local
movement remains subject to ordinary loan and move-state rules. Destruction is
not a synchronization operation and is valid only after every possible user
has ended.

## Default Sequentially Consistent Operations

The current API is:

```gti
std::atomic<int32_t> value{1};

int32_t current = value.load();
value.store(2);
int32_t prior = value.exchange(3);
auto changed = value.compare_exchange_strong(3, 4);
bool success = changed.exchanged();
int32_t observed = changed.observed();
int32_t fetched = value.fetch_add(2);
```

Construction initializes the atomic before ordinary ownership publication.
`load`, `store`, `exchange`, `compare_exchange_strong`, and the integer
`fetch_*` operations are sequentially consistent when no order argument is
supplied.

Strong compare/exchange performs one atomic read-modify-write on success and
one atomic load on failure. It succeeds exactly when the value observed at the
decision equals the expected value and never fails spuriously. Its result
object reports both success and the value actually observed; it exposes no
reference into atomic storage. On success `observed()` is the replaced value.
On failure it is the value that prevented the exchange.

Every atomic object has one modification order over its stores and
read-modify-writes. Every sequentially consistent atomic operation additionally
participates in one program-wide total order consistent with each thread's
sequenced-before order. A sequentially consistent load observes the most recent
preceding store or read-modify-write to that object in this total order.
Sequentially consistent stores release, loads acquire, and read-modify-writes
do both.

The compiler records operation identity and order in HIR and verified MIR.
Optimization treats those records as synchronization barriers. The public
wrapper is ordinary GTI source over exact compiler-private declarations from
the trusted `std/atomic` source unit; neither its public name nor the generated
C++ representation defines semantics.

## Explicit Memory Orders

The public order vocabulary consists of five statically typed tags:

```gti
std::memory_order_relaxed()
std::memory_order_acquire()
std::memory_order_release()
std::memory_order_acq_rel()
std::memory_order_seq_cst()
```

The constructor spelling makes each order an ordinary `std` type rather than
a runtime enum. An atomic method overload accepts only an order type legal for
that operation. Illegal combinations therefore fail in source overload
resolution before HIR or backend invocation; generated code never branches on
an order value.

```gti
value.store(7, std::memory_order_release());
int32_t observed = value.load(std::memory_order_acquire());
int32_t prior = value.exchange(8, std::memory_order_acq_rel());
auto changed = value.compare_exchange_strong(
    8, 9, std::memory_order_acq_rel(), std::memory_order_acquire());
```

The exact legality matrix is:

| Operation | Accepted order types |
| --- | --- |
| `load` | relaxed, acquire, seq_cst |
| `store` | relaxed, release, seq_cst |
| `exchange` | relaxed, acquire, release, acq_rel, seq_cst |
| compare/exchange success | relaxed, acquire, release, acq_rel, seq_cst |
| compare/exchange failure | relaxed for relaxed/release success; relaxed or acquire for acquire/acq_rel success; relaxed, acquire, or seq_cst for seq_cst success |

The three-argument compare/exchange form accepts one success-order tag and
derives the failure order as follows: relaxed -> relaxed, acquire -> acquire,
release -> relaxed, acq_rel -> acquire, and seq_cst -> seq_cst. The four-
argument form exposes all nine valid success/failure pairs. Failure can never
be release or acq_rel and can never be stronger than success. There is no
`consume` order.

Relaxed operations remain atomic and participate in their object's
modification order, but create no synchronizes-with edge themselves. A release
operation synchronizes with an acquire operation that reads the released value
or a value in its release sequence. A release sequence is that release followed
contiguously in modification order by atomic read-modify-write operations.
Everything sequenced before the release then happens-before everything
sequenced after the acquiring observation. Acquire/release operations do not
join the global sequentially consistent order unless their selected order is
seq_cst.

Semantic resolution records the exact operation and order pair. HIR preserves
it, MIR applies the operation-specific legality table and optimizer barrier,
and the sealed backend maps that verified record to the corresponding native
order spelling. Native overload resolution never supplies or strengthens GTI
semantics.

## Integer Fetch Operations

Every fixed-width signed or unsigned integer atomic exposes these
read-modify-write operations:

```gti
int32_t before_add = value.fetch_add(2);
int32_t before_sub = value.fetch_sub(1, std::memory_order_relaxed());
int32_t before_and = value.fetch_and(0xff, std::memory_order_acquire());
int32_t before_or = value.fetch_or(0x10, std::memory_order_release());
int32_t before_xor = value.fetch_xor(0x03, std::memory_order_acq_rel());
```

Each returns the value immediately before its successful update. All five
order tags are accepted because every operation is a read-modify-write. `bool`
remains available for load, store, exchange, and strong compare/exchange, but
does not satisfy the integer constraint for these fetch operations.

`fetch_add` and `fetch_sub` use GTI's checked integer arithmetic. Overflow is a
defined failure raised before that attempted update is published; these
operations deliberately do not inherit C++'s wrapping integral `fetch_add` or
`fetch_sub` behavior. Bitwise fetch operations are total after the normal
checked conversion back to the atomic's fixed-width type.

The standard library defines the fetch families as strong compare/exchange
loops over the sealed atomic operations rather than as new backend
instructions. The initial observation is relaxed. A successful attempt uses
these success/failure pairs: relaxed/relaxed, acquire/acquire,
release/relaxed, acq_rel/acquire, or seq_cst/seq_cst. Contention may cause the
pure built-in update expression to be recomputed; the successful
compare/exchange is the operation's linearization point. HIR/MIR verification
therefore sees the same exact atomic identities and barriers as an explicitly
written strong compare/exchange loop.

## Mutexes And Scoped Guards

`<std/mutex>` provides `std::mutex<T>` for an owned movable `T`. The mutex is
noncopyable and may be moved locally only while no guard derived from it is
live. It exposes no direct reference to its payload. Shared mutation is
available only through `lock()`:

```gti
std::mutex<int32_t> count{0};
{
  mut std::mutex_guard<int32_t> guard = count.lock();
  mut int32_t& value = guard.get();
  value = value + 1;
}
```

`lock()` accepts a shared mutex receiver and returns one move-only,
non-transferable, non-shareable `std::mutex_guard<T>`. A guard represents both
the native lock obligation and an exclusive owner dependency on that mutex.
While it is live, safe source cannot lock or move the owner again. Moving the
guard transfers the obligation and leaves the source guard inert.

`get()` returns `T&` from a read-only guard or `mut T&` from a mutable guard.
The returned loan is bounded by both the guard and its mutex owner: it cannot
outlive a temporary/local guard, be returned from that scope, or cross a
thread-transfer boundary. Guard destruction ends any child loan and unlocks
exactly once. This same reverse cleanup occurs before a defined failure is
propagated, so a waiter may acquire the mutex after the failing guarded scope
has been cleaned.

Unlock synchronizes-with a later successful lock of the same mutex. Everything
sequenced before unlock therefore happens-before everything sequenced after
that acquisition. The compiler records lock/unlock as non-removable,
non-reorderable synchronization operations in HIR and verified MIR; guard
access retains a separate owner-tied loan identity rather than treating a
native reference as lifetime proof.

GTI mutexes do not use poisoning. A defined failure inside a guarded region
performs deterministic cleanup and then preserves the original failure record;
the next lock is permitted. Applications that need transactional invariants
must encode rollback or validity state in the protected value. A native
lock-acquisition exception is contained and reported as the defined
`infallible_host_operation_failed`/`mutex_lock` outcome; no partially active
guard is published.

The representation promises neither recursive locking, fairness, stable
layout, C/C++ ABI compatibility, nor that moving a mutex preserves waiter
identity. A move constructs a fresh unlocked native mutex around the moved
payload and is legal only when ordinary ownership proves no guard is live.

## Bounded Production Guarantee

The combination of owned automatic-join tasks (including typed results), the
closed scalar atomic domain with all five accepted orders and integer fetch
operations, and owner-tied mutex guards is GTI's bounded production concurrent
surface. “Production” here means that each admitted operation has a complete
source contract, semantic and verified-HIR/MIR authority, deterministic
failure/cleanup behavior, native C++20/C++23 execution coverage, optimizer
barriers, installed-toolchain coverage, and sanitizer coverage where the host
supports it. It does not mean C++ API parity or a lock-free guarantee.

This boundary supports owned worker tasks, result handoff, counters/flags, and
lock-protected shared values without exposing native handles or raw pointers.
Programs needing an omitted family must wait for that family's own lifetime,
ordering, and failure contract instead of treating a similarly named native
facility as part of safe GTI.

## Current Omissions

The current surface deliberately excludes weak compare/exchange, spurious
failure, wait/notify, atomic references, pointer atomics, and fences.
Condition variables, public manual unlock, recursive/read-write mutexes,
scoped cross-thread borrows, detach, thread locals, and foreign-created-thread
entry are also not implemented.

The absence of these APIs is not permission to call similarly named native
facilities and treat their behavior as GTI synchronization.
