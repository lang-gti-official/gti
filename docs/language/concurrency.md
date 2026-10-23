# Concurrency And Atomic Values

Status: Current language contract for the bounded production safe concurrent
profile and the separate unsafe C-style threading surface.

Rationale is recorded in
[ADR 008](../decisions/008-safe-concurrency-memory-model.md). Remaining safe
condition-variable, worker-pool, and arbitrary foreign-entry work is tracked
in the [concurrency plan](../plans/concurrency-memory-model.md).

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
`std::mutex<T>`, and `std::parallel_invoke` require the concurrent profile and a target advertising
thread-capable concurrency support. Merely importing `<std/thread>`,
`<std/atomic>`, or `<std/mutex>` without constructing or calling their public
values remains valid in the default profile and does not select native
synchronization support. An executable use fails in semantics with
`GTI-S2084` when either requirement is absent.

The `<std/cthread>` create/join, once-initialization, manual-mutex, and
condition-variable operations have the same profile and target requirements
and additionally require lexical `unsafe`. Its scheduling hint, relative
sleep, and identity observation/comparison operations remain available in
either profile as specified below.

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

## Thread-Local Storage

`thread_local` gives a declaration one distinct storage instance per thread.
It is available in both execution profiles and does not require its type to be
share-capable. For example:

```gti
thread_local mut int visits = 0;
class Registry {
public:
  static thread_local mut Registry* current = nullptr;
};
int visit() {
  thread_local mut int calls = 0;
  calls += 1;
  return calls;
}
```

Namespace variables, non-generic static data members and ordinary function-local
variables are supported. `static thread_local` and `thread_local static` mean the
same storage duration; `static` retains its ordinary linkage/member meaning.
Block scope controls name visibility, not the lifetime of the per-thread object.
A block-local object can therefore be borrowed after its declaring function
returns, subject to the ordinary checked origin and access rules.

Initialization is lazy at the first evaluated use on each thread. A block
declaration also performs that first-use check when execution reaches it.
Initializers use ordinary GTI construction and rollback. Eligible primitive
literal/zero initialization has no user code, cleanup registration, added GTI
heap allocation or dynamic guard. Other initializers use an explicit per-thread
state transition; recursive
initialization is defined failure `GTI-R0013`, not an implicit C++ rule.
The passive fast path is currently literal-based; an integral-enum constant or
a computed/aggregate constant still uses the ordinary lazy initializer. This is
an optimization boundary, not a restriction on storing those owning values.

Successful cleanup-owning initializations register exactly once. Cleanup runs on
the same thread in reverse successful-initialization order, after automatic task
state is retired and before a worker outcome is published. The initial thread
drains at hosted exit; GTI-created manual C threads drain before leaving their
attached entry. Ordinary cleanup failure retains its record and remaining
cleanup; a secondary failure uses the existing bounded two-record emergency and
abandons remaining lifetimes. New initialization or resurrection during/after
thread-exit cleanup is `GTI-R0013`. Access to another initialized, not-yet-retired
slot remains permitted during cleanup.

A library callable may use dynamic TLS when invoked under an attached GTI
entry; it does not need to define its own hosted main. The runtime entry owns
the drain boundary, not an implicit native destructor. A public per-thread
enter/leave ABI for independent native embedding remains a separate goal;
passive literal/zero primitive TLS needs no managed drain.
Dynamic first-use on an unattached native thread is defined failure
`GTI-R0013` before construction or registration. Merely compiling a hosted main
does not attach an arbitrary foreign callback thread; GTI-owned entries establish
and close that runtime lifetime explicitly.

A TLS reference is not a process-lifetime promise or an alias exemption. Direct
accesses and hidden accesses through GTI helpers, constructors, operators,
initializers and destructors must respect live shared/exclusive loans. First-use
effects include the initializer; a live checked loan to the exact local slot
proves that slot already initialized, including through nested helpers. Open
GTI call targets require a structural ambient-effects proof when a persistent
borrow remains live. Native calls and raw pointers retain their existing unsafe
obligations; the compiler does not infer arbitrary native pointer retention.

Checked scoped/joined borrowing can lend a TLS referent while its owning thread
and loan remain alive and the referent permits the selected access. This is not
a blanket ban on cross-thread references. An independently created worker's
direct lookup names its own slot, not the captured parent-thread object. Owned
tasks and results still cannot silently retain borrowed state past its owner.

Current structural boundaries: no non-static TLS fields, TLS parameters or
functions, `constexpr` TLS, native TLS declarations, generic-instance TLS, or
lambda-local TLS declarations. A local initializer cannot depend on an automatic
binding, parameter or instance receiver; guarded lexical/per-instance
initialization is a separate goal. A source diagnostic identifies these shapes.
Generic function-local and generic class-static storage require a descriptor key
that includes the exact concrete function/class instance, rather than the
unspecialized source `SymbolId`; until that inventory is represented and
verified, both shapes are rejected instead of sharing a slot across instances.
`thread_local` does not add thread-specific-storage keys, detach, arbitrary
foreign-thread entry or async suspension. Runnable example:
[`173-thread-local-storage.gti`](../../examples/173-thread-local-storage.gti).

## C-Style Scheduling Hint

`<std/cthread>` currently implements `void std::thrd_yield()`. It offers the
host scheduler an opportunity to run another thread and may have no effect.
The call returns no value, owns no resource, and raises no defined failure.
It is valid in both execution profiles, including on the initial thread:
yielding does not create a second GTI execution agent.

Yield does not establish a synchronizes-with edge, publish memory, release a
lock, guarantee progress or fairness, or impose a minimum delay. A polling
algorithm still needs atomics or another supported synchronization contract.
The ordinary native-call effects prevent GTI optimizations from deleting or
reordering the host operation; they do not constitute synchronization proof.
Its scheduling-hint meaning follows
[C11 §7.26.5.8](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf),
not C handle ABI parity.

## C-Style Relative Sleep

`<std/cthread>` provides `std::timespec`, an ordinary copyable GTI struct with
mutable public `int64_t tv_sec` and `int64_t tv_nsec` fields. Its default
constructor sets both to zero; its two-argument constructor stores seconds and
nanoseconds. For sleep it represents a relative duration; timed mutex locking
instead uses an absolute timestamp. Neither use promises native C `timespec`
layout or ABI.

```gti
int32_t thrd_sleep(timespec duration);
int32_t thrd_sleep(timespec duration, mut timespec& remaining);
unsafe int32_t thrd_sleep(const timespec* duration, timespec* remaining);
```

Valid durations have nonnegative seconds and nanoseconds in `[0, 1000000000)`.
Sleep returns 0 when elapsed, -1 when interrupted, and -2 for invalid input,
unrepresentable host duration, or host failure. These results are not the
create/join status constants. A zero duration succeeds without a host wait.
Only interruption writes `remaining`; success and error preserve its previous
value. The reported remainder is normalized, nonnegative, and no greater than
the request. Host clock resolution and scheduling apply; no exact wakeup time
or nanosecond precision is promised, and sleep may overshoot.

The value input is copied before the mutable output borrow, so
`thrd_sleep(duration, duration)` is valid for a mutable binding. The pointer
overload also snapshots its input before writing output, allowing aliasing.
A null input returns -2; a null output discards the remainder. Non-null raw
pointers must address live, aligned, correctly synchronized values, with
writable storage for the output. These are unsafe caller obligations.

Sleep is valid in either execution profile and does not require target
thread-creation support. It creates no thread, ownership, callback permission,
synchronizes-with edge, or GTI defined failure. It does not retry interruptions
automatically. This relative-duration and interruption model follows
[C11 §7.26.5.7](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf),
without promising native C object layout or platform-independent interruption
delivery.

## C-Style Thread Identity

`<std/cthread>` provides safe `thrd_t thrd_current()` and
`int32_t thrd_equal(thrd_t left, thrd_t right)`. Both work in either execution
profile; neither creates a thread or requires target thread-creation support.
The initial thread and managed workers have identities too. `thrd_current()`
returns a stable, nonzero, process-local identity that is never reused, even
after thread exit. A manually created worker observes the exact handle returned
by `thrd_create`. Native host thread IDs are not exposed. Exhaustion of the
identity space terminates instead of returning zero or reusing an identity;
manual creation can instead return `thrd_error` without publishing a handle.

`thrd_equal` returns exactly 1 for equal values and 0 otherwise. It is total
value comparison, including zero, unknown values, and saved identities after
exit; it is not validation or a liveness query. Observing, copying, or comparing
an identity grants no join obligation, callback-entry permission, or
synchronizes-with edge. Only successful manual creation supplies its matching
join obligation. Initial and managed identities cannot be joined through
`thrd_join`. The runtime identity observer can be called by foreign native code
without registering that thread for GTI callback entry.

## C-Style Manual Create And Join

`<std/cthread>` also provides `thrd_t` (an opaque-purpose `uint64_t` identity),
`thrd_start_t` (`(void*) -> int32_t`), and these ordinary GTI wrappers:

```gti
unsafe int32_t thrd_create(thrd_t* output, thrd_start_t entry, void* context);
unsafe int32_t thrd_create(mut thrd_t& output, thrd_start_t entry, void* context);
unsafe int32_t thrd_join(thrd_t handle, int32_t* result);
unsafe int32_t thrd_join(thrd_t handle, mut int32_t& result);
```

Calls require lexical `unsafe`, the concurrent execution profile, and target
thread support. Importing the module, describing its aliases, yielding,
sleeping, and observing/comparing identities do not require the concurrent
profile.
Status constants are `thrd_success` (0),
`thrd_error` (1), `thrd_nomem` (2), and `thrd_busy` (3). These are GTI's C-style
operations, not a promise of native `<threads.h>` type, layout, or status ABI
parity.

Creation success publishes one nonzero, never-reused handle. Failure preserves
the output and context; a null output or callback is an error. Null context is
valid. The unsafe caller keeps the context and everything reachable through
it alive and appropriately synchronized until successful join. Creation
orders prior writes before worker entry; successful join orders worker
completion and writes before continuation and optionally publishes its signed
32-bit result. A null result pointer discards the result. Join consumes the
handle once; stale, unknown, self, and competing joins cannot consume it again.
A recoverable native join error preserves a live handle for retry. Copying the
integer handle does not duplicate its obligation.

Every successful creation must be matched by successful join before hosted
entry returns. There is no automatic join or detach on this manual surface.
Returning with an outstanding handle terminates as an unsafe lifecycle contract
violation. Use `std::thread` for owned, automatically joined tasks instead.

Concurrent GTI callback conversion accepts the exact `thrd_start_t` worker
signature or the once-initializer signature described below and requires the
trusted `<std/cthread>` runtime declarations. The worker's
generated adapter enters only on the worker registered for that exact adapter,
once. Initial-thread invocation, unregistered foreign threads, another adapter,
recursive entry, and replay are stopped before entering the GTI target.
Transporting an adapter through C does not grant general foreign-thread entry.
The target may also be called as an ordinary GTI function; that is not adapter
entry.

Hosted MIR initializes the runtime after successful program initialization and
before user entry (before argument-vector construction for owned-argument
entry). Normal entry completion and its cleanup precede runtime shutdown.
Currently a concurrent program importing this module conservatively includes
that empty-service lifecycle even if it only yields. A default-profile
yield-only program does not initialize the service.

A worker defined failure performs its verified GTI cleanup and terminates the
invocation with the original failure record. Unlike managed threads, this
C-style callback boundary does not save the failure for re-raising at join.
Native exceptions terminate at the callback firewall rather than escaping C.
Failure termination does not run normal runtime shutdown or replace the
original failure with an outstanding-handle error.

This is not yet a completed `<threads.h>` equivalent. Detach, exit,
and thread-specific storage
remain implementation work. The safe managed owners below retain their
separate lifetime and failure contract.

## C-Style Once Initialization

`<std/cthread>` supplies noncopyable, nonmovable `once_flag`, the constructor
tag `ONCE_FLAG_INIT`, `once_initializer_t` (`() -> void`), and unsafe
`call_once(once_flag&, once_initializer_t)` plus a `const once_flag*` overload.
Default construction and construction from the tag each create independent,
uninitialized state. The tag is a value, not a shared flag. Flags may be
immutable globals: their private atomic state supplies synchronization without
granting arbitrary mutable-global access.

Calls require the concurrent profile, target threads, and lexical `unsafe`.
Exactly one caller claims a fresh flag and invokes its supplied initializer.
Only normal completion publishes the completed state; every normally returning
caller observes the initializer's writes through release/acquire ordering.
If different callers supply different initializers, the winning claim selects
which one runs. Nested calls with different flags work. A recursive call on
the same active flag cannot complete; it is a caller contract violation.
Contenders may yield, with no fairness or bounded-wait guarantee. There is no
reset, relocation, or native C `once_flag` layout promise.

The caller keeps the flag alive through all uses and supplies a non-null
initializer. A null flag or an invoked null initializer terminates. A GTI
initializer adapter enters only within its exact synchronous runtime
invocation on the initial thread, an entered manual worker, or a managed GTI
worker. Passing the adapter directly to arbitrary native code does not grant
callback permission, nor does the invocation helper attach a foreign thread.

A defined failure runs the initializer's verified cleanup and terminates with
the original record, even inside a managed worker; `call_once` does not turn
that failure into a join result or retry initialization. Native exceptions
terminate at the callback boundary. Completion is never published on either
failure path.

## C-Style Manual Mutexes

`<std/cthread>` provides `mtx_t` (a non-owning `uint64_t` handle) and unsafe
`mtx_init`, `mtx_destroy`, `mtx_lock`, `mtx_trylock`, `mtx_timedlock`, and
`mtx_unlock`. Every operation requires the concurrent profile and target thread
support. Initialization accepts a mutable handle reference or output pointer;
destruction takes a mutable reference or pointer and clears that handle on
success. Lock/try/unlock accept a handle value or pointer. Timed locking accepts
a handle value plus `timespec` value, or pointers to both.

Flags are `mtx_plain` (0), `mtx_recursive` (1), and `mtx_timed` (2); the allowed
combinations are 0, 1, 2, and 3. Initialization publishes a nonzero, never-reused
identity only on success. Invalid flags/null output return `thrd_error` (1);
allocation failure returns `thrd_nomem` (2), preserving output. A successful
initialization creates one explicit destruction obligation. Handle copies do
not duplicate it. The caller keeps the mutex live through all uses and excludes
concurrent destruction. These handles are not native C `mtx_t` layout.

Lock returns `thrd_success` (0) after acquisition, or `thrd_error` on failure.
Try-lock returns `thrd_busy` (3) when another thread holds the mutex, without
waiting for that ownership to end. A plain mutex cannot be recursively locked:
self lock is an error and self try-lock returns busy. Recursive acquisition
increments a checked lock count; each successful acquisition requires one
unlock by the same thread. Wrong-thread/unlocked unlock returns error without
changing ownership. Only the final recursive release makes the mutex available.

Timed locking requires `mtx_timed`. Unlike sleep, its `timespec` is an absolute
UTC/Unix timestamp (seconds since 1970-01-01, normalized nanoseconds). Negative
seconds are allowed. The timestamp must fit a signed 64-bit nanosecond count;
otherwise it returns error without acquiring. An available mutex may be
acquired even if the deadline has passed. If a held mutex remains unavailable
at the deadline, the result is `thrd_timedout` (4), with no acquired lock. Host
clock changes/resolution and scheduling apply; no precise wakeup or fairness
guarantee is made.

A final successful unlock synchronizes with a later successful acquisition of
the same mutex. Busy, timeout, errors, and intermediate recursive operations
do not grant a new inter-thread synchronization edge. HIR/MIR retain exact
private native acquire/release identities, including try/timed calls whose edge
depends on success; unknown native calls remain only optimization barriers.

Destruction returns no status and requires an unlocked mutex with no operations
in flight. Invalid, stale, locked, or in-use destruction terminates as an unsafe
lifecycle violation. There is no automatic unlocking or destruction on scope
exit or GTI defined failure. Ordinary return/error paths must balance their
locks and destroy the mutex; prefer `std::mutex<T>` below when an owned scoped
guard is appropriate. Pointer arguments retain the ordinary raw-pointer
lifetime, alignment, writable-output, and synchronization obligations. Null
lock/try/timed/unlock pointers return error; null destruction terminates.

## C-Style Manual Condition Variables

`<std/cthread>` supplies unsafe `cnd_init`, `cnd_destroy`, `cnd_signal`,
`cnd_broadcast`, `cnd_wait`, and `cnd_timedwait` over non-owning `cnd_t`
(`uint64_t`) handles. All require the concurrent profile and target threads.
Initialization takes an output pointer or mutable reference; failure preserves
it, with `thrd_error` for invalid input and `thrd_nomem` for allocation failure.
Success creates one explicit destruction obligation with a nonzero, never-reused
condition identity. Copies do not duplicate the obligation or native C layout.

Wait takes condition/mutex values or pointers to both. The calling thread must
hold exactly one acquisition of the mutex, including for a recursive mutex.
It atomically registers its wait and releases that acquisition relative to
condition notifications. Before returning it reacquires the same mutex once.
Success returns `thrd_success`; a recoverable host wait error returns
`thrd_error` with the mutex reacquired. An unrecoverable reacquisition failure
terminates rather than returning without ownership. Invalid handles or lock
ownership/count are errors before release and leave ownership unchanged.

Timed wait additionally takes a `timespec` value (or pointer for the pointer
overload). It uses the same normalized absolute UTC/Unix deadline range as
`mtx_timedlock`, but does not require the mutex's `mtx_timed` flag. An expired
deadline returns `thrd_timedout` **after reacquiring**, unlike timed mutex
acquisition. Invalid timestamps are errors before release. Timeout bounds the
condition wait, not subsequent mutex reacquisition or total call duration.

Signal wakes one current waiter, if any; broadcast wakes all current waiters.
Both accept a handle value or pointer and return success even with no waiters.
Notifications are not retained for future waits and do not transfer mutex
ownership. There is no fairness guarantee or independent shared-data publication
edge from notification: publication occurs through mutex release/reacquisition.
Callers must check a shared predicate under its mutex in a loop, including
after notification or timeout; notification alone does not prove the predicate.
MIR explicitly records `ConditionWait` and `ConditionNotify` native operations;
neither is interchangeable with a simple mutex acquisition or an unknown call.

Destruction accepts a mutable reference or pointer, clears that handle on
success, and requires every call to have finished, including waiters currently
reacquiring their mutex. Invalid, stale, or in-use destruction terminates.
Neither condition nor mutex is automatically destroyed/unlocked by GTI cleanup.
Null operation pointers return error; null destruction terminates. Other raw
pointer validity, alignment, lifetime, and synchronization obligations remain
with the unsafe caller. This is not a safe scoped condition-variable wrapper.

## Managed Threads

`<std/thread>` provides the move-only `std::thread` and
`std::thread_result<Result>` owners. A task and every capture cross by owned
transfer exactly once. References, stored borrowed state, raw pointers, and
non-transfer-capable values do not cross. An outstanding handle automatically
joins during deterministic cleanup; detach is absent.

In addition to an exact lambda or nominal callable object, `start` accepts an
eligible namespace free-function name whose exact `void()` or `Result()`
signature is supplied by the selected thread contract. Semantics selects one
non-generic body-bearing GTI function and transfers a nominal zero-state
function item. Overloads are not ranked, and no C/C++ function pointer or ABI
address is formed. Eligible unambiguous function items can also be stored and
captured in ordinary GTI; this is not permission to supply extra task
arguments or erase unsafe/native/generic declaration contracts. See
[function-item ownership](ownership-and-lifetimes.md).

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

## Scoped Parallel Invocation

`<std/thread>` provides the synchronous operation
`expected<void, std::thread_errc> std::parallel_invoke<Tasks...>(Tasks... tasks)`.
It consumes an ordered group of exact `void()` callable values and completes
every started worker before returning or propagating a defined failure. The
empty group succeeds. Read, mutable and consuming call operators are supported;
the existing exact callable selection rules choose each entry. Lambdas with
owned captures, function items and ordinary user-defined callable classes use
the same operation.

Unlike the unscoped `std::thread::start` ownership boundary, a scoped task can
retain checked reference fields, including nested carriers and multiple
dependencies. Shared references require share-capable referents. Exclusive
references require transfer-capable referents and exclude overlapping parent
or sibling access until completion. Two shared readers can overlap; two writes
or a read and write cannot. Existing place disjointness admits independent
fields without a special rule for any library container. Generic by-value and
parameter-pack forwarding preserve each task's exact dependency identity.
Moving a carrier into a scope does not make it eligible for an unscoped thread
or permit its dependencies to disappear through erasure. Raw pointers and
thread-affine state retain their existing unsafe and capability boundaries.

```gti
class Advance {
  mut int& position;
public:
  Advance(mut int& position) : position(position) {}
  void operator()() mut { this.position += 1; }
};

// Within a concurrent-profile function:
mut int left = 0;
mut int right = 0;
auto finished = std::parallel_invoke(Advance(left), Advance(right));
// Both workers and task cleanup have finished before parent access resumes.
```

The scope owns its task values, not their external referents. All started
workers join before remaining task values are cleaned in reverse argument
order, including tasks whose native creation failed or was never attempted.
A consuming callable follows ordinary consuming-receiver cleanup during its
invocation; the scope does not run that cleanup twice. Borrow dependencies stay
live through whichever cleanup route applies. A user destructor retains the
ordinary requirement for an audited transfer assertion; a scope is not an
exception to native-resource or cleanup-thread affinity.

Creation failure returns `resource_unavailable` or `operation_not_permitted`
only after earlier workers have joined and all task values have been cleaned.
A worker defined failure takes precedence over a later creation error. If
several workers fail, the first in argument order supplies the propagated
record, independent of completion order. Task-cleanup failure follows the
ordinary defined-failure rule, including `GTI-R0014` when another failure is
already being cleaned. An indeterminate native join terminates without
unwinding storage beneath a live child.

This first implementation creates one native worker per task. Task contexts
and group descriptors are stack-resident, but native creation and runtime
worker state still have costs. This is not a persistent pool, scheduler,
asynchronous task API or a promise of cheap fine-grained dispatch. The runnable
example is [066-managed-threads.gti](../../examples/066-managed-threads.gti).

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

## Safe Atomic Waiting

`<std/atomic>` also supplies `wait(old)`, `notify_one()`, and `notify_all()` on
every supported atomic scalar. These are safe shared-receiver operations in
the concurrent profile, not async suspension or a worker pool:

```gti
// Consumer: avoid a load/notification race by waiting on the observed value.
mut int32_t observed = completed.load(std::memory_order_acquire());
while (observed != expected_count) {
  completed.wait(observed, std::memory_order_acquire());
  observed = completed.load(std::memory_order_acquire());
}
// Producer, after publishing one completion:
// completed.fetch_add(1, std::memory_order_release());
// completed.notify_all();
```

Wait repeatedly loads and compares against `old`. It returns only after
observing a different value; internal spurious wakes do not become successful
returns. A value already different before the call returns without requiring a
notification. A blocked waiter needs a subsequent notification to ensure it
rechecks a change. Store/exchange/fetch operations do **not** implicitly notify.
`notify_one` unblocks at least one eligible blocked waiter, if any;
`notify_all` unblocks all eligible blocked waiters. No notification credit is
saved for future waits, and notification alone does not change the value.

Wait's default loads are sequentially consistent. Explicit overloads accept
`memory_order_relaxed`, `memory_order_acquire`, and `memory_order_seq_cst` only.
An acquire observation of a release-published value has the ordinary atomic
happens-before contract. Notification has no order argument and establishes
no independent publication edge. Use the atomic value as state, not the
notification as a retained event. A transient `old -> other -> old` change can
be missed (ABA); a generation counter can represent transitions when needed,
with wraparound handled by the application's protocol.

The shared receiver loan keeps the atomic storage alive and unmoved during
wait/notify, including across scoped borrowed tasks. No payload reference is
exposed and no mutex guard is released. Deadlock freedom, fairness, deadlines,
cancellation and lock freedom are not promised. Unrecoverable native waiting
failures terminate rather than returning with a false observation or escaping
as a native exception. The operations add no GTI per-wait allocation or extra
per-object state; host parking implementation costs are platform-dependent.

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

The synchronization owner is transferable and shareable exactly when `T` is
transferable. Acquiring the lock grants exclusive access on the acquiring
thread, so `T` need not itself allow concurrent shared access. A thread-affine
payload remains usable in a local mutex, but prevents either cross-thread
capability. Guards remain non-transferable and non-shareable regardless of
the payload. `std::shared_ptr<std::mutex<T>>` therefore supports dynamically
owned synchronized state when `T` is transferable, without allowing movement
of a live lock or bypassing protected-value loans.

## Transfer-Qualified Callable Storage

`<std/functional>` supplies move-only `std::transfer_function<R(Args...)>`,
`std::transfer_function<R(Args...) mut>` and
`std::transfer_function<R(Args...) &&>`. Construction and replacement require
a transferable target; interface erasure retains that requirement. As with
ordinary owning function wrappers, the signature selects read-only,
exclusive-repeatable or consuming invocation. Transfer does not grant
concurrent invocation, sharing, or borrowed captures. Ordinary `function`
and `move_only_function` keep accepting local-only targets and do not acquire
a transfer guarantee from their signature alone.

The implementation uses ordinary `[[transfer]]` interfaces and owning models,
not a compiler rule for a standard-library name. Unrelated interfaces use the
same capability contract. Repeatable wrappers use the existing inline/boxed
polymorphic owner; consuming wrappers retain the existing boxed unique-owner
projection. Empty invocation follows the ordinary checked empty-owner failure.

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

The safe surface deliberately excludes weak compare/exchange, spurious
failure, timed/cancellable atomic waiting, atomic references, pointer atomics,
and fences. It also
has no safe scoped condition-variable wrapper, safe public manual-unlock API,
or safe recursive/read-write mutex family. The unsafe `<std/cthread>` surface
does provide manual unlock, recursive mutexes, and manual condition variables.
Across both surfaces, detach, C-style thread exit,
C-style thread-specific-storage keys, read/write mutexes, and arbitrary
foreign-created-thread entry are not implemented.
Persistent worker pools, dependency scheduling and asynchronous suspension are
also separate facilities; synchronous scoped invocation does not provide them.

The absence of these APIs is not permission to call similarly named native
facilities and treat their behavior as GTI synchronization.
