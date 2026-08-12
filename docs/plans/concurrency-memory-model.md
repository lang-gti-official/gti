# GTI Concurrency And Memory-Model Proposal

> **Plan status:** D-MEM-01 proposal complete and superseded for normative
> decisions by [ADR 008](../decisions/008-safe-concurrency-memory-model.md),
> [execution semantics](../language/execution.md#49-concurrency-boundary), and
> [ownership semantics](../language/ownership-and-lifetimes.md#concurrency-transfer-and-sharing).
> D-MEM-02 adopted the boundary; implementation remains staged below.
> ADR 012 supersedes this proposal's former pre/post-1.0 scheduling split. Its
> technical prerequisites remain evidence, while the bounded public
> concurrency profile is now systems-readiness work.

Baseline: GTI 0.93.0.

This document proposes the language boundary needed before GTI can add
threads, atomics, mutexes, or callbacks entered from native threads. It does
not change current single-threaded GTI behavior, choose final source spelling,
or authorize concurrency implementation. Current behavior remains defined by
[`docs/language/`](../language/), and implementation order remains owned by
[`implementation-sequence.md`](implementation-sequence.md).

The proposal deliberately uses the neutral semantic terms
**transfer-capable** and **share-capable**. ADR 008 adopted those terms without
requiring them to become source keywords or public concept names.

## Recommended Decision Summary

1. A well-formed program using only sound safe GTI operations cannot have a
   data race. Thread-boundary capability failures are diagnosed before backend
   entry. A data race manufactured through `unsafe`, an unsound safe wrapper,
   or native code is undefined behavior under a specific unsafe obligation.
2. Add two independent semantic type facts: transfer capability permits a
   value's ownership, use, and eventual cleanup to move to another thread;
   share capability permits concurrent shared read-only access to one live
   value. They are not aliases for copyability, movability, ownership kind, or
   a C++ backend trait.
3. Derive both facts structurally for ordinary safe values. Allow a safe
   negative declaration. Permit a positive assertion that overrides a
   structural or cleanup/native-resource denial only as an explicit unsafe
   nominal promise.
4. The first executable thread model transfers only owned values. References,
   borrowed-state carriers, raw pointers, and borrowed captures do not cross a
   thread boundary. Scoped borrows require a later structured-join proof.
5. `mut` remains a local binding/place permission and grants no synchronization
   or cross-thread access. Concurrent mutation is exposed only through
   compiler-validated interior-mutable abstractions such as atomics and
   mutex guards.
6. The first atomic surface accepts fixed-width integers and `bool`, is
   sequentially consistent, and promises neither lock freedom nor C-compatible
   layout. Later orderings are `relaxed`, `acquire`, `release`, `acq_rel`, and
   `seq_cst`; there is no `consume` ordering.
7. The first public thread is an owned, move-only, join-structured handle. It
   accepts one consumed transfer-capable task and owned transfer-capable
   arguments. Destruction joins an outstanding thread. Detach and cross-thread
   borrows are later features.
8. GTI defines sequenced-before, synchronizes-with, and happens-before itself.
   Spawn, successful join, atomic operations, and future lock operations carry
   explicit GTI semantics through HIR and MIR; native C++ ordering is only a
   lowering mechanism.
9. A native thread enters GTI only through a validated runtime attachment and
   generated callback boundary. Raw pointers, retention, synchronization, and
   callback lifetime remain explicit unsafe proof obligations.
10. Adopt the semantic boundary before public execution. Implement
    transfer/share facts and concurrent-global policy first, then deliver the
    bounded public SC atomic/thread/mutex profile for the work-queue readiness
    outcome.

## Current Facts This Proposal Preserves

GTI already has a substantial single-threaded memory-safety foundation:

- semantic values distinguish places from values, read-only from mutable
  access, value/borrow/unique/shared ownership, copy/move availability, and
  lexical destruction;
- `T&` is a non-null read-only loan and `mut T&` is an exclusive writable loan;
  retained loans, reborrows, parent suspension, and proven endpoints are
  semantic facts preserved through HIR and MIR;
- raw pointers are nullable non-owning values, create no semantic loan, and
  require lexical `unsafe` for address formation, access, arithmetic, and
  pointer-bearing C calls;
- violating an unsafe raw-pointer validity, lifetime, alignment,
  initialization, bounds, provenance, aliasing, native-retention, or
  concurrent-access obligation is already undefined behavior;
- class lifecycle is structural, declared cleanup suppresses copying, and
  active cleanup transfers exactly once during movement;
- globals and static fields exist, including mutable scalar storage, while
  unique owners and borrowed-state values are already rejected in global or
  static storage;
- lambdas currently take immutable copy snapshots of explicitly named local
  values, reject reference and move-only captures, and are non-escaping; and
- MIR has conservative reads/writes, calls, moves, loans, drops, raw-memory
  effects, and `maySynchronize`, but no atomic order, thread operation, or
  cross-thread edge. The C++ backend still emits bodies from AST/HIR facts.

The proposal composes with those facts. It does not replace the borrow checker
with a separate concurrency checker or treat C++ `std::thread` and
`std::atomic` behavior as language authority.

## Abstract Concurrent Execution

### Threads And Sequencing

A **thread of execution** is one ordered execution agent with its own call
stack, automatic storage, and thread-local state. The initial thread begins
after program-wide initialization. A managed thread begins only through a GTI
spawn operation. A native-created thread becomes a GTI execution agent only
after the runtime successfully attaches it at an approved entry boundary.

**Sequenced-before** orders evaluations within one thread according to the
strict left-to-right evaluation and full-expression contract adopted by
D-EXEC-01. Compiler or backend reordering may not change observable behavior
or any happens-before relation.

A **memory location** is one live scalar object or one non-overlapping scalar
subobject. Fixed-array elements and distinct non-overlapping fields are
different locations. Object lifetime transitions conflict with access to the
same object or affected subobject.

An access is:

- an ordinary read or write;
- an atomic read, write, or read-modify-write; or
- a lifetime operation that begins, moves, replaces, or ends a live object.

Two accesses **conflict** when they touch the same memory location or
overlapping lifetime and at least one is a write or lifetime operation.

### Synchronizes-With And Happens-Before

**Synchronizes-with** is created only by a GTI operation whose contract says
so. Conservative optimizer barriers and unknown calls do not themselves
create a language-level synchronization edge.

**Happens-before** is the transitive closure of sequenced-before and
synchronizes-with. The first executable profile provides these edges:

- every evaluation sequenced before a successful spawn happens-before the
  first task evaluation;
- completion of a task happens-before a successful join returns;
- every sequentially consistent atomic operation participates in the one
  program-wide sequentially consistent order; and
- a future mutex unlock synchronizes-with a later successful lock acquisition
  that observes that release of the same mutex.

Later acquire/release atomics add the edges specified in
[Future Atomic Orders](#future-atomic-orders). Native code creates a GTI edge
only through an ABI/runtime contract recognized by the compiler and runtime or
through an unsafe wrapper whose correctness proof establishes the same edge.

### Data Races And Undefined Behavior

A **data race** occurs when two different threads perform conflicting accesses,
at least one access is non-atomic, and neither happens-before the other. Mixing
an atomic access with an ordinary access to the same live location is therefore
not a synchronization shortcut; it is still a data race without ordering.

The recommended rule has two layers:

1. Safe GTI APIs, type capabilities, ownership transfer, borrow checking,
   globals policy, and synchronized wrappers make a data race unrepresentable.
   A source-visible violation such as moving a non-transfer-capable value into
   a task is ill-formed.
2. If unsafe raw-pointer code, an unsafe capability assertion, retained native
   state, or foreign code violates its synchronization proof and causes a data
   race, behavior is undefined. The obligation is attached to that unsafe
   boundary, not to ordinary safe code.

This is consistent with the existing raw-pointer contract. Defining arbitrary
races as nondeterministic values or dynamically trapping would require global
instrumentation, would not compose with native hardware, and would prevent
normal optimization. A conforming safe library wrapper remains responsible for
meeting the unsafe proof for every input its safe API accepts.

Conflicting accesses that are all atomic are not data races. Incorrect use of
relaxed atomics can still be a logic error, and it can expose a separate unsafe
lifetime violation, but atomic operations themselves do not become undefined
merely because their interleaving is surprising.

The model guarantees no fairness. Deadlock, livelock, starvation, and priority
inversion are permitted outcomes unless a particular library operation states
a stronger progress guarantee. They are not memory undefined behavior.

## Transfer And Share Capabilities

### Meanings

For a concrete type `T`:

- **transfer-capable** means that exclusive ownership or access to a `T`,
  including responsibility for its eventual cleanup, may move to another
  thread without violating memory safety or a thread-affinity contract;
- **share-capable** means that multiple threads may concurrently hold and use
  read-only shared access to the same live `T` while its lifetime is protected.

These properties are independent. A type may be transfer-capable but not
share-capable. A non-movable type may still be transfer-capable in the abstract
when it remains at a stable address behind a transferable owner. A copyable
type is not automatically transfer- or share-capable.

Capability computation belongs to semantic analysis and concrete generic
reanalysis. HIR preserves the resolved facts. MIR and the backend consume them
at boundary operations; they do not infer them from emitted C++ traits or
names.

### Structural Rules

The recommended structural baseline is:

| Type category | Transfer-capable | Share-capable | Notes |
| --- | --- | --- | --- |
| fixed-width integers, `bool`, `char`, `float`, `double`, enums, `nullptr_t` | yes | yes | Subject to their ordinary value semantics. |
| fixed array `T[N]` | when `T` is | when `T` is | Extent does not change the rule. |
| `expected<T, E>` and other value aggregates | when every contained value is | when every contained value is | Active alternative/lifetime must already be valid. |
| ordinary class or struct | when every state-bearing base and instance field is, and lifecycle policy permits | when every state-bearing base and instance field is, and lifecycle policy permits | Static fields are validated as globals, not instance state. |
| interface-erased value or owner | only when the interface contract requires transfer and every implementation proves it | only when the interface contract requires sharing and every implementation proves it | Method signatures alone do not imply either fact. |
| `std::unique_ptr<T>` | when `T` and any stored allocator/deleter state are transfer-capable | when `T` and shared observer state are share-capable | Moving the handle transfers one owner; sharing never permits handle mutation. |
| future shared owner of `T` | when `T` and control/deleter state are both transfer- and share-capable | under the same condition | The last owner and `T` cleanup may occur on any participating thread. |
| `T&` or `mut T&` | no in the first model | no in the first model | Later scoped eligibility is defined separately below. |
| value containing stored borrowed state | no in the first model | no in the first model | Includes current iterator/view carriers. |
| `T*`, `const T*`, and `void*` | no | no | A nominal unsafe wrapper may assert a stronger contract. |
| lambda or callable object | when every owned capture and its callable lifecycle are | when every capture is share-capable and concurrent invocation uses only a read-only callable contract | A mutable/exclusive callable may still be transferred for one invocation. |
| atomic scalar wrapper | yes | yes | It is not necessarily copyable, movable, or lock-free. |
| future `mutex<T>` | when `T` is transfer-capable | when `T` is transfer-capable | Shared mutation is mediated exclusively by its guard. |
| future single-thread cell of `T` | when `T` is transfer-capable | no | Interior mutation without synchronization prevents sharing. |

Recursive nominal types use a cycle-aware structural fixed point. A node that
owns another node through `unique_ptr<Node>` does not fail merely because its
capability query is recursive. Concrete generic instances are independently
checked after substitution, consistent with existing lifecycle traits.

A share-capable type may contain fields declared `mut`: shared access still
cannot mutate those fields through an ordinary `T&`. If a field enables
interior mutation, that field's own share capability decides the aggregate
result.

### Cleanup, Native Resources, And Explicit Policy

Thread affinity is not derivable from representation. A window, graphics
context, event-loop token, or native descriptor may be represented by an
integer or opaque pointer while requiring use and cleanup on one thread.

The recommended policy is:

- ordinary classes without a declared cleanup body derive capabilities from
  bases and fields;
- declaring a cleanup body conservatively suppresses automatic transfer and
  sharing, even when every field would otherwise qualify;
- compiler-owned owners and storage capabilities have explicit generic rules
  rather than being treated as arbitrary cleanup bodies;
- any nominal type may safely opt out of transfer, sharing, or both;
- there is no ordinary safe positive override, because a fact already proven
  structurally needs no assertion; and
- a nominal type may make an explicit unsafe positive assertion. That promise
  may override a raw-pointer field, a cleanup suppression, or another
  non-derivable native-resource fact. The author then proves all use, movement,
  concurrent access, and cleanup obligations for every safe public operation.

The implemented C-TYPE-01 spelling uses safe `[[no_transfer]]`/
`[[no_share]]` opt-outs, unsafe positive assertions, and explicit interface
requirements. The semantic distinction remains required:
a negative declaration is safe policy; a positive override is an unsafe proof
and must be visible in semantic metadata, diagnostics, documentation, and
tooling. Changing fields or lifecycle later must re-evaluate structural facts
and retain the explicit unsafe assertion as reviewable source, never as a
backend-only trait specialization.

A wrapper containing unsafe code is unsound if it permits automatic structural
capabilities that its native contract cannot uphold. Its author must use the
safe opt-out even when the representation is only an integer and would
otherwise derive both capabilities.

## Thread Boundaries, Borrows, And Captures

### First Executable Model: Owned Transfer Only

The first public spawn operation accepts one consumed callable and consumed
arguments. Every transferred value must be transfer-capable. Transfer happens
before task entry, invalidates the source bindings under ordinary move rules,
and makes the task the sole owner of those values.

Transfer capability is an additional boundary condition, not a synthesized
move operation. A value transferred directly into task storage must also meet
the ordinary move and construction rules for that value; a stable-address
non-movable pointee can instead travel behind an eligible owner.

The following are rejected at the boundary regardless of apparent native
lifetime:

- `T&` and `mut T&`;
- any current stored-reference iterator, view, or other borrowed-state carrier;
- a lambda capture containing either of those;
- every raw pointer type; and
- a capture or wrapper lacking the required transfer capability.

An owned capture follows the same rule as an explicit argument. Current
copy-snapshot lambdas remain non-escaping and cannot serve as thread tasks
until C-CALL-01 implements the consumed-task adapter over D-CALL-01's accepted
[callable contract](callable-ownership-and-escape.md). Future explicit owned
move capture consumes the source and derives the closure's lifecycle and
transfer facts from the captured value. No implicit reference capture is
introduced.

An exclusively owned callable may mutate its own captured state during its one
task invocation without being share-capable. Sharing one callable for
concurrent invocation requires the callable and every capture to be
share-capable and its invocation contract to use read-only access.

### Later Scoped Borrowing

No borrow crosses in the first executable model. A later scoped-thread feature
may permit it only when all of these facts are represented and verified:

1. one lexical concurrency scope owns every child handle and joins every child
   on every normal and failure exit;
2. no scoped child detaches, escapes the scope, or stores the borrow in a value
   that can outlive the scope;
3. a shared `T&` crosses only when `T` is share-capable;
4. an exclusive `mut T&` crosses only when `T` is transfer-capable, and the
   parent place/loan is suspended in the spawning thread until join;
5. aliases and disjoint projections use the same semantic place/loan authority
   as ordinary GTI rather than a thread-library side table;
6. child completion happens-before loan reactivation and every later parent
   access; and
7. MIR represents the scope, spawn, join, child loan, parent suspension, and
   all cleanup edges explicitly.

This proof depends on stored/escaping mutable dependencies and complete
temporary/drop authority. A native thread API called inside `unsafe` does not
gain this scoped-borrow permission automatically.

## `mut`, Globals, And Interior Mutability

### Meaning Of `mut`

`mut` continues to mean that one binding, field, receiver, or borrowed place
may be changed through the current access path. It does not mean volatile,
atomic, synchronized, share-capable, or safe for access from another thread.

Moving a `mut` or immutable owned binding into a task uses the same transfer
rule. Once moved, the original thread cannot access it. A shared `T&` cannot
call a mutable receiver or write a `mut` field merely because another thread
exists.

Interior mutation is available only through a type whose semantics establish
it. An atomic exposes value-returning atomic operations rather than `mut T&`.
A mutex is held through an immutable shared binding; successful lock creates a
guard-tied exclusive access capability. A single-thread cell remains not
share-capable.

### Process-Wide Static Storage

Current mutable globals remain valid in the single-threaded language. In a
future thread-capable execution profile, the conservative first policy is:

- every process-wide global or static binding is immutable and its value is
  share-capable;
- ordinary `mut` namespace globals and `static mut` class fields are
  ill-formed for that profile;
- synchronized mutation uses an immutable binding of an approved atomic or
  mutex wrapper;
- borrowed-state and raw-pointer globals do not become valid merely because
  their binding is immutable; and
- cleanup-owning process-wide values remain unavailable in the first
  concurrent profile until global shutdown and foreign-thread registration are
  represented completely.

This declaration-wide rule is intentionally conservative. It avoids requiring
whole-program proof that a particular mutable global is accessed only before
spawn. C-GLOBAL-01 now provides `GTI-S2060` as the migration diagnostic for
programs that remain valid in the default single-threaded profile.

Program-wide initialization completes before the initial entry function and
before any managed spawn. Therefore successful spawn publishes initialized
global and transferred state to the child. With no detach in the first model,
all managed threads finish before program-wide destruction begins.

### Thread-Local State

Thread-local storage is a later library/language surface, but its contract is
proposed now:

- each attached thread owns a distinct instance;
- initialization occurs on first use in that thread, is sequenced before the
  access, and recursive initialization raises `GTI-R0013` with detail
  `recursive_thread_local_initialization` while the detected state is still
  safe to clean;
- initialized thread-local values are destroyed in reverse initialization
  order on that same thread at normal detach/exit;
- a thread-local value cannot contain borrowed state escaping its initializing
  call and no reference to it crosses to another thread; and
- a primary worker failure cleans initialized thread-local values before the
  task boundary stores its record, while Execution §4.10's failure-during-cleanup
  emergency path may terminate without completing remaining cleanup.

Thread-local values may be mutable without synchronization because they are
not shared. Their final source spelling is outside D-MEM-01.

## Atomics And Synchronization

### Initial Atomic Surface

The first standard atomic wrapper accepts only `bool` and fixed-width signed
and unsigned integer types. It provides construction, load, store, exchange,
and one strong compare-exchange operation. It does not initially provide:

- floating, enum, pointer, reference, owner, aggregate, or interface atomics;
- weak compare-exchange or spurious failure;
- fetch arithmetic, wait/notify, or atomic references;
- a lock-free guarantee; or
- stable object layout or C/C++ ABI compatibility.

All first-surface operations are sequentially consistent. Construction occurs
before publication under ordinary ownership rules. Atomic destruction is not
an access synchronization operation and must occur only after all users have
ended.

The strong compare-exchange succeeds only when the observed value equals the
expected value and never fails spuriously. Success is one atomic
read-modify-write; failure is one atomic load. The operation reports success
and the value actually observed without exposing the atomic's storage through
a reference. Every atomic read-modify-write observes the immediately preceding
value in that object's modification order and contributes its replacement as
the next modification.

Sequential consistency means that all such atomic operations participate in
one total order consistent with each thread's sequenced-before order. A load
observes the most recent preceding store or read-modify-write to that atomic in
the total order. Every atomic object also has one modification order for its
writes and read-modify-writes. A sequentially consistent store has release
semantics, a sequentially consistent load has acquire semantics, and a
sequentially consistent read-modify-write has both. An acquiring operation
that observes a releasing operation therefore creates a synchronizes-with
edge in addition to participating in the total order.

The wrapper is an ordinary public GTI type over a trusted capability. Compiler
semantics bind the capability by declaration identity and record the operation
and order. The compiler does not recognize the public wrapper name.

### Future Atomic Orders

After the sequentially consistent implementation is validated, C-ORDER-01 may
add this complete vocabulary:

| Operation | Permitted orderings |
| --- | --- |
| load | relaxed, acquire, seq_cst |
| store | relaxed, release, seq_cst |
| exchange/read-modify-write | relaxed, acquire, release, acq_rel, seq_cst |
| compare-exchange success | relaxed, acquire, release, acq_rel, seq_cst |
| compare-exchange failure | relaxed, acquire, seq_cst; never stronger than success |
| fence | acquire, release, acq_rel, seq_cst |

There is no consume ordering. The exact source enum and default-argument
spelling remain library decisions.

For compare-exchange, the legal failure orders by success order are exact:
relaxed permits relaxed; acquire permits relaxed or acquire; release permits
only relaxed; acq_rel permits relaxed or acquire; and seq_cst permits relaxed,
acquire, or seq_cst. This makes “not stronger” a semantic table rather than a
backend-dependent ranking between incomparable acquire and release effects.

A **release sequence** is a release operation followed contiguously in that
atomic object's modification order by zero or more atomic read-modify-write
operations. A release operation synchronizes-with an acquire operation that
reads the value written by the release or by a member of its release sequence.
That edge publishes all evaluations sequenced before release to evaluations
after the acquire. Relaxed operations are atomic and participate in
modification order but create no inter-thread happens-before edge by
themselves. A sequentially consistent operation additionally participates in
the one global sequentially consistent order.

Operation/order mismatches are semantic errors. They are never delegated to a
native template overload or runtime branch.

### Mutexes And Guards

A future mutex owns `T` and exposes no direct `T&` or `mut T&`. A successful
lock returns one noncopyable, non-transferable owner-tied guard. The guard
provides exclusive access, unlocks exactly once on every exit, and prevents the
protected access from escaping. Unlock synchronizes-with a later successful
lock of the same mutex.

This requires M-OWN-03 stored/escaping mutable dependency support. The current
bounded reborrow and read-only iterator carrier do not satisfy the guard
contract. Execution §4.10 now requires unlock/guard cleanup on a primary failure;
poisoning, recoverable lock failure, and the exact guard behavior remain owned
by C-SYNC-01.

Condition variables require an atomic guard release/wait/reacquire operation
and are deferred beyond that first mutex slice. Volatile/MMIO operations and
signals are separate language problems: volatile is not synchronization, and
neither is added by this proposal.

## Thread Lifecycle, Join, Detach, And Failure

### Creation And Task Entry

Thread creation is a recoverable host operation and returns an `expected`
result rather than terminating for resource exhaustion. The task and arguments
are fully moved into runtime-owned storage before native creation can publish
them. The spawn call consumes those values regardless of its result. If native
creation fails before publication, runtime-owned task storage and all consumed
values are destroyed exactly once on the spawning thread; no partially created
source value is exposed.

The task entry thunk is compiler/runtime-owned. It reconstructs no GTI types
from untyped native bytes, invokes exactly one resolved consumed callable, and
destroys task-owned values on the worker thread after invocation. Spawn
synchronizes the parent evaluations before publication with task entry.

### Join-Structured Handle

The first handle is move-only and owns one join obligation. Moving the handle
transfers that obligation. A successful explicit join may occur once and makes
task completion happen-before every evaluation after join returns.

Explicit join consumes the handle into the operation. A join error is
recoverable only when the runtime can prove that the worker remains joinable;
the error result must then return ownership of the same join obligation. If
the worker has completed and the obligation is safely discharged but a
destructor/automatic-join API has no result channel, it may raise Execution §4.10's
`infallible_host_operation_failed`. Truly indeterminate native state cannot be
returned or contained as an ordinary GTI failure because invocation cleanup
cannot prove that no worker still accesses owned state; it is a runtime
integrity fault that terminates the process. No safe result or embedding record
silently abandons a live obligation.

ADR 008 selects automatic join when an outstanding handle is destroyed. This
preserves structured lifetime and ensures no managed child survives program or
scope cleanup. It can block and can participate in deadlock; that cost is part
of the contract. The rejected alternative made dropping an unjoined handle a
defined runtime failure, avoiding hidden blocking at the cost of weaker RAII
and path-sensitive must-join diagnostics. An environmental failure
during an automatic join cannot be returned and raises
`GTI-R0012` on the joining thread. If that occurs while another failure is
already cleaning up, it takes Execution §4.10's `GTI-R0014` emergency path.

The initial task shape is `void()` after owned argument binding, so automatic
join does not silently discard a normal value result. Later result-bearing
tasks must require explicit result observation or an explicit discard policy.

### Detach

Detach is absent from the first model. A later explicit detach operation may
consume an owned handle only when the task contains exclusively owned
transfer-capable state and no scoped borrow, guard, thread-local reference, or
program-shutdown dependency. A detached task performs its own cleanup on its
thread. Process termination is not required to wait for or clean up detached
tasks.

Detach is not inherently an unsafe raw-memory operation once those ownership
conditions are proven, but its process-lifetime and failure-reporting contract
must be accepted before it becomes safe public API.

### Failure And Termination

Ordinary recoverable task errors are returned explicitly as values. Native
thread creation and join errors also use `expected` whenever the runtime can
return a valid join obligation. A task must not silently lose a checked runtime
failure.

The execution specification and ADR 008 select contained worker failure.
Compiler-managed failure edges clean task-owned and initialized thread-owned
GTI state before the task-entry boundary stores the original fixed-size record.
The worker then completes in a failed state; it does not continue user code and
the failure is not reported as normal task completion. Explicit join re-raises
the captured GTI record on the joining thread, distinct from returning a
recoverable native join error. Task capture does not invoke the observer; the
eventual hosted or embedding boundary observes the record once. Automatic join
re-raises the original record in the same way. Detach remains absent from the
first model; a later detach proposal must define an unobserved-failure policy.

The explicit GTI failure channel and any native exception stop at
`extern "C"`, generated callback, embedding, and native task-entry firewalls;
neither may use native ABI unwinding across them. Callback policy, exit status,
diagnostic serialization, and observers follow Execution §4.10 rather than host C++.

## Native Threads, Callbacks, And FFI

The current C ABI remains call-only. A foreign declaration is not presumed
thread-safe merely because its parameters contain no pointers. In a concurrent
profile, safe invocation requires an adopted declaration contract saying that
the operation is thread-safe and either nonsynchronizing or has one recognized
synchronization effect. A thread-confined operation is callable only on its
owning thread. An unclassified foreign call requires an unsafe proof, including
serialization of hidden foreign state; final declaration spelling belongs to
the native-callable and FFI rows. Existing single-threaded calls are unchanged.

Future native callbacks and foreign-thread entry must satisfy all of these
rules:

1. a native thread enters through a generated ABI thunk tied to one exact
   semantically resolved GTI callable;
2. the thunk attaches runtime thread state before GTI execution and detaches it
   after all GTI values and thread-local values for that entry are cleaned up;
3. entry is rejected before user code if the runtime is uninitialized,
   shutting down, or lacks the target `threads` capability;
4. callback arguments use an adopted C ABI representation; no GTI class,
   owner, reference, atomic, or mutex layout becomes C-compatible implicitly;
5. native retention of a callback, context address, or GTI-derived raw pointer
   is an unsafe lifetime and synchronization obligation;
6. a safe registration wrapper owns the registration, prevents callback use
   after unregistration, and establishes completion-before-destruction for
   every in-flight callback; and
7. a native callback cannot create a shared or transferable GTI reference by
   merely receiving an address.

Calling a native thread API with raw pointers remains unsafe. The wrapper must
prove pointee lifetime, transfer/share capability, aliasing, publication,
completion, and exactly-once cleanup. A raw pointer remains neither
transfer-capable nor share-capable; an audited nominal wrapper uses the unsafe
positive capability assertion when the foreign contract genuinely supports
cross-thread use.

Unknown native and runtime calls remain conservative optimization barriers.
That conservatism does not assert a synchronizes-with edge. A future native
declaration contract may state a recognized synchronization effect, but final
spelling and ABI expansion belong to the native-callable and FFI rows.

The runtime must classify every host service callable from a worker: allocator,
failure reporting, stdout/stderr, files, sockets, and future environment/time
services. Thread safety, output interleaving, handle affinity, and blocking
behavior are library/runtime contracts, not accidental properties of libc.

C and C++ atomic object layouts are not GTI ABI. Sharing one atomic object with
native code requires a separately adopted ABI capability or an opaque runtime
operation. Signals, asynchronous signal handlers, and callback entry without
runtime attachment remain outside the concurrency model.

## Compiler And Runtime Representation Plan

### Semantic Analysis

Semantic analysis remains the authority for:

- transfer/share facts on every resolved type and concrete generic instance;
- structural derivation, recursive types, safe opt-outs, and unsafe positive
  assertions;
- interface capability requirements and implementation validation;
- thread-boundary validation of tasks, arguments, captures, owners, borrows,
  globals, and raw pointers;
- atomic domain and operation/order legality;
- process-wide versus thread-local storage policy; and
- exact task, atomic, synchronization, native-entry, and runtime capability
  identities.

Diagnostics should put the primary span on the boundary operation and provide
related information for the first field, base, capture, stored borrow, raw
pointer, cleanup declaration, global, or unsafe assertion that prevents the
capability. They must distinguish transfer failure from sharing failure and
must never cite a generated C++ type as the reason.

### HIR

HIR preserves concrete capability results, nominal policy declarations,
task/callback identities, captured and transferred values, atomic operation
and order, storage class, and runtime target capability. It discovers concrete
task and wrapper instances through the existing instance worklists. It does
not recompute traits or infer thread safety from a public standard-library
name.

### MIR And Verification

MIR needs explicit operation categories for:

- atomic load, store, exchange/read-modify-write, compare-exchange, and future
  fence, each with an order;
- task storage construction, spawn, task entry, completion, and join;
- future lock, unlock, wait, and reacquire; and
- foreign attach, callback entry/exit, and runtime detach where those become
  executable.

Memory effects distinguish ordinary unknown memory from atomic memory and
record whether an operation may synchronize, establishes a known
synchronizes-with category, blocks, invokes user code, may fail, or owns
cleanup. `maySynchronize` remains an optimizer barrier, not proof of a
happens-before edge.

The verifier checks identity, capability evidence, operation/order legality,
task ownership and exactly-one entry, join state, cleanup edges, and the
structural pairing required by known synchronization operations. It does not
try to discover source-level transfer/share validity after semantics or prove
arbitrary native synchronization.

### Optimizer And Backend

Atomic, spawn/join, lock/unlock, attach/detach, unknown native, runtime, and
callback operations are non-speculatable, non-removable, and non-reorderable
until an operation-specific proof says otherwise. Ordinary accesses may be
optimized only while preserving GTI sequenced-before observables and all
happens-before edges. A pass cannot invent atomicity, merge distinct atomic
operations, remove a join, or move lifetime operations across publication.

The C++ backend must lower validated MIR operations with at least the GTI
strength. It may use C++ atomics and threads as representation, but it may not
select memory orders, task identities, lifetime, or failure behavior. Public
concurrency should not ship while complete affected bodies still depend on
host expression ordering rather than MIR-backed emission.

### Runtime And Standard Library

The runtime owns platform thread creation/join, attachment, task-entry thunks,
thread-local bookkeeping, thread-safe failure reporting, and target capability
validation behind a narrow C ABI. It does not expose pthread, Windows, C++
thread, or native atomic types to GTI source.

Public policy remains ordinary GTI source: atomic, thread, join result, and
future mutex/guard classes constrain compiler-private capabilities through
trusted declaration identity. The compiler recognizes irreducible operations,
not public wrapper names. I-CAP-01 has closed the `gti_internal` visibility
gap: only trusted prelude and physical standard-library units can access the
namespace, while application declarations, aliases, references, and tooling
presentation are rejected or filtered.

## Staged Delivery And Verification Matrix

The stages below assign semantic and diagnostic authority first, preserve the
facts in HIR, represent and verify effects in MIR, constrain optimization and
backend lowering, then expose runtime and standard-library policy. The evidence
column is part of every stage rather than a final test-only pass.

| Stage / owner | Required implementation | Focused evidence |
| --- | --- | --- |
| D-MEM-02 | **Done:** ADR 008 adopts this boundary, Execution §4.10's contained worker failure, automatic join, neutral semantic capability terms, and the ledger-selected horizon | ADR, canonical execution/ownership docs, restriction ledger, and roadmap agree. |
| I-CAP-01 | **Done:** compiler-private declarations and types bind by trusted source/declaration identity, with `GTI-S2058` and shared tooling filtering | Forged aliases/declarations and direct application access fail; std wrappers still work. |
| C-TYPE-01 | **Done:** structural transfer/share facts, public concepts, and nominal negative/interface/unsafe-positive policy are semantic and HIR facts | Primitive, aggregate, recursive, generic, interface, owner, raw-pointer, cleanup, native-handle, and capture positive/negative semantic tests; deterministic `GTI-S2059` related spans and tooling coverage. |
| C-GLOBAL-01 | **Done:** explicit pre-semantics profile selection plus concurrent-profile global/static enforcement | `GTI-S2060` rejects mutable and non-share-capable storage; immutable share-capable aliases/generics/statics/internal linkage pass; semantic/HIR/MIR and direct/project CLI evidence agree. |
| C-MIR-01 | Add synchronization operation metadata and exhaustive conservative effects | Deterministic HIR/MIR snapshots, verifier mutation tests, exhaustive effect-table assertions, and no speculation/removal/reordering. |
| C-RUNTIME-01 | Add target `threads` capability, private handle/task storage, attach state, and thread-safe host services | Unsupported targets fail before backend; installed runtime smoke and platform linkage pass; allocator/failure/I/O service classifications are exercised. |
| C-ATOM-01 | Add SC scalar atomic wrapper and exact lowering | Domain and order diagnostics; strong CAS semantics; message-passing and modification-order tests at O0/O3; controlled native-thread harness and TSAN where available. |
| D-CALL-01 done / C-CALL-01 | Implement one owned consumed `void()` task over the accepted callable identity/capability model | Exactly-one invocation/drop, move-only owned captures, and rejection of borrow/raw/non-transfer captures through semantics, HIR, and MIR. |
| C-THREAD-01 | Add move-only automatic-join thread and recoverable creation/join | Spawn/join happens-before tests, move-only argument transfer, join on every cleanup edge, creation failure injection, stress, O0/O3, and sanitizers. |
| M-OWN-03 / C-SYNC-01 | Add guard-tied mutex access after stored mutable dependencies exist | Exactly-once unlock on every edge, no protected-reference escape, contention stress, poisoning/failure behavior, and TSAN. |
| C-ORDER-01 | Add only the accepted weaker orders | Operation/order compile-fail cases plus deterministic litmus tests for release/acquire and relaxed non-synchronization. |
| later scoped threads | Extend the existing loan graph through structured spawn/join | Shared and exclusive scoped borrow positives; escape/detach/early-drop negatives; parent suspension and reactivation MIR verification. |
| later native callbacks | Add ABI thunk, registration owner, attach/detach, and in-flight completion | Foreign-thread entry stress, post-unregister rejection, TLS cleanup, failure-boundary tests, and no unwind across C. |
| diagnostics and LSP across public stages | Publish shared frontend capability, boundary, order, and target diagnostics without spelling inference | Deterministic primary/related spans, generic-instance cases, incomplete-source recovery, and protocol snapshots. |
| C-CONFORM-01 | Make the selected public profile a release gate | Deterministic semantic/IR tests, O0/O3 runtime equivalence, installed-toolchain coverage, TSAN where supported, and no timing-only oracle. |

LSP publication uses the shared frontend diagnostics and capability facts. It
must not infer transfer/share from spelling. Hover displays the adopted public
concept results and unsafe assertions;
completion must not offer unsupported atomics or threads for a target lacking
the runtime capability.

## Deliberate Omissions

This proposal does not add or promise:

- source syntax for thread creation, atomics, thread-local storage, memory
  orders, or native callbacks beyond the implemented capability attributes;
- public threads, atomics, mutexes, condition variables, or detach in D-MEM-01;
- cross-thread references in the first executable profile;
- pointer atomics before a public provenance/cast contract;
- volatile/MMIO, signals, coroutines, async tasks, work stealing, parallel
  algorithms, lock-free guarantees, atomic aggregates, hazard pointers, epoch
  reclamation, or a C-compatible atomic layout;
- general unwind, exceptions, or recovery from unsafe undefined behavior; or
- a stable GTI ABI or reliance on a particular C++ concurrency implementation.

## D-MEM-02 Adoption Resolution

[ADR 008](../decisions/008-safe-concurrency-memory-model.md) resolves every
choice this proposal reserved:

1. transfer/share facts and concurrent-global policy precede public execution;
   public SC atomics, joined threads/tasks, and mutex-guard access are
   systems-readiness executable work;
2. concurrent policy selection is explicit before semantics and the default
   executable profile remains single-threaded; public concurrent runtime
   operations retain their later gates;
3. destruction of an outstanding first-model handle automatically joins;
4. worker failure is contained, cleaned, preserved byte-for-byte, and re-raised
   by explicit or automatic join;
5. transfer-capable/share-capable are canonical semantic terms, while
   C-TYPE-01 owns final declaration spelling and need not add keywords; and
6. detach, scoped cross-thread borrows, pointer atomics, mutex guards, and
   foreign-thread callbacks retain their explicit later prerequisites.

This plan remains design evidence and an implementation matrix. The ADR and
canonical language documents own the accepted rule.
