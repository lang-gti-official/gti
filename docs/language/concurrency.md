# Concurrency And Atomic Values

Status: Current language contract for the implemented concurrent profile,
managed threads, and sequentially consistent scalar atomics.

Rationale is recorded in
[ADR 008](../decisions/008-safe-concurrency-memory-model.md). Remaining mutex,
weaker-order, scoped-thread, and foreign-entry work is tracked in the
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

`std::thread`, `std::thread_result<Result>`, and `std::atomic<T>` require the
concurrent profile and a target advertising thread-capable concurrency
support. Merely importing `<std/thread>` or `<std/atomic>` without constructing
or calling their public values remains valid in the default profile. An
executable use fails in semantics with `GTI-S2084` when either requirement is
absent.

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

## Sequentially Consistent Operations

The current API is:

```gti
std::atomic<int32_t> value{1};

int32_t current = value.load();
value.store(2);
int32_t prior = value.exchange(3);
auto changed = value.compare_exchange_strong(3, 4);
bool success = changed.exchanged();
int32_t observed = changed.observed();
```

Construction initializes the atomic before ordinary ownership publication.
`load`, `store`, `exchange`, and `compare_exchange_strong` are all
sequentially consistent. There is no source order argument in this first
surface.

Strong compare/exchange performs one atomic read-modify-write on success and
one atomic load on failure. It succeeds exactly when the value observed at the
decision equals the expected value and never fails spuriously. Its result
object reports both success and the value actually observed; it exposes no
reference into atomic storage. On success `observed()` is the replaced value.
On failure it is the value that prevented the exchange.

Every atomic object has one modification order over its stores and
read-modify-writes. All current atomic operations additionally participate in
one program-wide total sequentially consistent order consistent with each
thread's sequenced-before order. A load observes the most recent preceding
store or read-modify-write to that object in this total order. Sequentially
consistent stores release, loads acquire, and read-modify-writes do both.

The compiler records operation identity and order in HIR and verified MIR.
Optimization treats those records as synchronization barriers. The public
wrapper is ordinary GTI source over exact compiler-private declarations from
the trusted `std/atomic` source unit; neither its public name nor the generated
C++ representation defines semantics.

## Current Omissions

The current surface deliberately excludes weak compare/exchange, spurious
failure, fetch arithmetic, wait/notify, atomic references, and fences. Explicit
`relaxed`, `acquire`, `release`, `acq_rel`, and `seq_cst` selection is not yet a
source API. Mutex guards, condition variables, scoped borrows, detach, thread
locals, and foreign-created-thread entry are also not implemented.

The absence of these APIs is not permission to call similarly named native
facilities and treat their behavior as GTI synchronization.
