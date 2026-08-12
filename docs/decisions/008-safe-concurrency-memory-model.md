# 008: Safe Concurrency Is Capability-Gated And Data-Race-Free

Status: Accepted

## Context

GTI already defines ownership, local loans, exclusive reborrows, movement,
deterministic cleanup, raw-pointer obligations, and conservative synchronizing
effects. It does not yet expose threads, atomics, mutexes, or entry from a
foreign-created thread. The current executable language is therefore
single-threaded even though generated C++ or a native dependency may internally
use host threads.

That implementation accident cannot become the language memory model. Before
the ownership and compatibility contracts freeze, GTI needs one answer for:

- when a value may move to or be observed from another thread;
- whether ordinary safe code can contain a data race;
- how `mut`, globals, cleanup, raw pointers, and native resources interact with
  concurrency;
- which operations create ordering edges that optimizers must preserve; and
- how task lifetime and the defined-failure contract compose.

The design analysis is recorded in the
[D-MEM-01 proposal](../plans/concurrency-memory-model.md). This record adopts
its bounded direction. It does not add concurrency syntax or claim that the
current compiler implements the future profile.

## Decision

### Execution Profiles

GTI defines two execution profiles:

1. The **single-threaded profile** has one GTI thread of execution. It remains
   the current and default executable profile. Existing mutable globals and
   current native calls retain their documented single-threaded meaning.
2. The **concurrent profile** permits managed threads, recognized atomic and
   synchronization operations, and attached foreign-thread entry. It is a
   future opt-in target/runtime capability. The selected profile must be known
   before semantic analysis and preserved in program facts through HIR and MIR.

An implementation shall not infer the concurrent profile from backend flags,
native link arguments, host-library behavior, or incidental use of a host
thread. The eventual source/manifest spelling and target capability are owned
by the concurrency runtime and global-policy implementation rows.

The semantic boundary, transfer/share type facts, and concurrent-global policy
are pre-1.0 commitments. Public atomics, threads, mutexes, weaker memory orders,
and native-thread entry remain post-1.0 systems-completeness work. Adopting the
model now is not authorization to expose an incomplete API.

### Safe Data-Race Freedom

A **memory location** is one live scalar object or one non-overlapping scalar
subobject. Two accesses conflict when they touch the same location or
overlapping lifetime and at least one is a write or lifetime operation.

A **data race** occurs when different threads perform conflicting accesses, at
least one access is non-atomic, and neither access happens-before the other.
Safe GTI makes such a race unrepresentable: safe boundary operations,
capability checking, ownership transfer, borrowing, global policy, and
synchronization wrappers must reject a program before backend entry when they
cannot establish the required isolation or ordering.

A race manufactured through an unsafe raw-pointer operation, an unsound unsafe
capability assertion, retained native state, or foreign code violates the
specific unsafe boundary obligation and has undefined behavior. `unsafe` does
not turn a race into a defined nondeterministic result. Atomic accesses are not
data races with one another, but mixing atomic and ordinary access to the same
live location does not make the ordinary access safe.

The model does not promise fairness. Deadlock, livelock, starvation, priority
inversion, and blocking during automatic join are permitted unless a more
specific operation contract says otherwise.

### Transfer And Sharing

Every concrete type has two independent semantic facts:

- **transfer-capable** means exclusive ownership or access, including eventual
  cleanup responsibility, may move to another thread without violating memory
  safety or thread affinity; and
- **share-capable** means multiple threads may concurrently hold read-only
  shared access to one live value while its lifetime is protected.

These are GTI semantic facts, not aliases for copyability, movability,
ownership kind, triviality, native layout, or a C++ type trait. Semantic
analysis computes them for every concrete generic instance. HIR preserves the
resolved results. MIR and boundary operations verify and consume them; neither
the backend nor the standard-library wrapper name may rediscover them.

Ordinary values derive both facts structurally through state-bearing bases,
fields, active alternatives, captures, and lifecycle policy. Recursive
nominal types use a cycle-aware structural fixed point. Interfaces state a
capability requirement explicitly and every implementation proves it.
Declaring a cleanup body conservatively denies automatic transfer and sharing;
compiler-owned owner/storage types use explicit generic rules instead.

Thread affinity is not derivable from representation. A type may safely opt
out of either fact. A positive assertion that overrides a structural denial,
raw-pointer field, declared cleanup body, or native-resource restriction is an
explicit unsafe nominal promise. Its author proves movement, access, and
cleanup for every safe operation. C-TYPE-01 owns the final source spelling;
this decision requires the distinction but introduces no keyword or attribute.

The initial structural boundary is:

- primitive scalar values, enums, and `nullptr_t` are transfer- and
  share-capable;
- arrays and ordinary aggregates derive both facts from their contents and
  lifecycle;
- `expected<T, E>` derives them from both alternatives;
- a unique owner transfers only when its pointee and stored cleanup state can
  transfer; sharing the handle never grants handle mutation;
- callable values derive the facts from every owned capture, lifecycle, and
  invocation access contract;
- references, stored borrowed-state carriers, and raw pointers have neither
  fact in the first executable concurrent profile; and
- compiler-defined atomics are transfer- and share-capable by their own
  contract rather than by representation.

This capability system composes with the existing ownership/loan authority. It
does not create a second borrow checker, infer safe crossing from native
lifetimes, or convert a non-movable value into something movable.

### Borrow, Mutation, And Global Boundaries

The first executable thread model transfers only owned values. A task and its
arguments are consumed before publication and every transferred value must
also satisfy the ordinary move/construction rules. `T&`, `mut T&`, current
iterators/views and other borrowed-state carriers, raw pointers, and borrowed
captures cannot cross. A later scoped-borrow feature requires an explicit
structured join proof, child-loan representation, parent suspension, cleanup
on every exit, and verified reactivation after join.

`mut` remains permission to mutate through one local access path. It is not
atomic, volatile, synchronized, transfer-capable, or share-capable. Concurrent
mutation is possible only through an abstraction whose language contract
provides interior synchronization, such as a compiler-validated atomic or a
future mutex guard.

In the concurrent profile, process-wide globals and static fields must be
immutable and share-capable. Ordinary mutable namespace globals and mutable
static fields are ill-formed. Synchronized mutation uses an immutable global
whose value is an approved atomic or future mutex wrapper. Borrowed-state and
raw-pointer globals do not become eligible merely because their binding is
immutable. Cleanup-owning process-wide values remain unavailable until global
shutdown and foreign-thread participation have one represented contract.

These restrictions apply only when the concurrent profile is selected. The
single-threaded profile retains current mutable-global behavior.

Program-wide initialization completes before initial entry and every managed
spawn. With no detach in the first model, all managed tasks finish before
program-wide destruction begins.

Thread-local storage is a later surface with an adopted boundary: each
attached thread owns one instance; first-use initialization is sequenced before
that use; recursive initialization raises `GTI-R0013` while cleanup remains
safe; and initialized values are destroyed in reverse initialization order on
the same thread. No reference to thread-local storage crosses to another
thread. Worker failure cleans initialized thread-local values before storing
its record unless failure during cleanup takes the `GTI-R0014` emergency path.

### Ordering And Synchronization

**Sequenced-before** is the strict within-thread order selected by
[ADR 010](010-deterministic-evaluation-and-full-expressions.md) and the GTI
full-expression contract. **Synchronizes-with** is created only by a GTI
operation whose language contract says so. **Happens-before** is the transitive
closure of those relations.

The first executable concurrent profile will provide these edges:

- evaluations sequenced before successful spawn happen-before task entry;
- task completion happens-before a successful explicit or automatic join
  returns or continues cleanup;
- every sequentially consistent atomic operation participates in one
  program-wide sequentially consistent order; and
- future mutex unlock synchronizes-with the corresponding later successful
  acquisition.

The first atomic surface is limited to fixed-width integers and `bool`, uses
sequential consistency, and promises neither lock freedom nor C-compatible
layout. Later accepted orders are `relaxed`, `acquire`, `release`, `acq_rel`,
and `seq_cst`; there is no `consume`. Operation-specific legality must be
checked before lowering.

HIR and MIR must represent recognized synchronization operations and their
orders explicitly. An unknown call or `maySynchronize` effect is a conservative
optimization barrier; it is not proof of a language-level synchronization
edge. The backend may lower an accepted operation through host facilities only
after GTI has chosen its identity, order, failure, and lifetime semantics.

### Owned Threads, Automatic Join, And Failure

The first managed thread handle is owned, move-only, and join-structured. It
contains exactly one join obligation. Moving the handle transfers that
obligation; detach is absent. Destruction of an outstanding handle
**automatically joins**. This can block or participate in deadlock, but it
ensures no managed child outlives the cleanup of its owned task state. Later
result-bearing tasks must require explicit observation or an explicit discard
policy.

Creation and recoverable join errors use `expected` whenever a valid ownership
state can be returned. Task and argument values are moved into runtime-owned
storage before native creation publishes them. Creation failure destroys those
consumed values exactly once on the spawning thread.

Worker checked failure follows
[Execution section 4.10](../language/execution.md#410-defined-runtime-failure):
task-owned and initialized thread-owned state is cleaned to the task-entry
boundary, the original fixed-size record is stored unchanged, and user code
does not resume. Explicit join re-raises that record on the joining thread.
Automatic join does the same. Capture does not independently invoke the
failure observer, so the eventual hosted or embedding boundary observes the
record once. An environmental automatic-join failure without a result channel
raises `GTI-R0012`; if it occurs while another failure is cleaning up, it takes
the `GTI-R0014` emergency path. Genuinely indeterminate native join state is a
process-terminal integrity fault, not a recoverable or containable GTI record.

### Native Threads And Runtime Services

A foreign-created thread may enter GTI only through an exact generated
callback boundary after successful runtime attachment. Attachment establishes
thread-local runtime state; exit cleans GTI thread-owned values before detach.
The registration owner must remain live until unregistration and all in-flight
callbacks complete. Raw-pointer validity, callback retention, synchronization,
and native lifetime remain explicit unsafe obligations.

Before public tasks ship, every hosted runtime service callable from them must
be classified for thread safety, output interleaving, handle transfer, and
synchronization effects. An unclassified foreign operation is not presumed
thread-safe merely because its signature contains no pointers.

## Required Implementation Order

This decision unlocks representation and policy work, not public concurrency.
The implementation sequence remains:

1. secure compiler-private capability identity;
2. compute and preserve structural transfer/share facts;
3. enforce the concurrent-profile global/static policy;
4. establish temporary/drop, failure, ordered-execution, callable, target, and
   runtime prerequisites;
5. add explicit synchronization operations/effects;
6. expose sequentially consistent scalar atomics and the owned automatic-join
   task/thread model; and
7. add conformance, stress, optimizer-ordering, installed-toolchain, and
   sanitizer gates before advertising the profile.

No implementation row may use public wrapper spelling, emitted C++ traits, or
a parallel runtime-only table as semantic authority.

## Required Conformance Cases

Final source spelling may differ, but the implementation gates must express
and test these semantic cases:

| Shape | Required result in the concurrent profile |
| --- | --- |
| consumed primitive or structurally eligible aggregate enters a task | accepted; source is moved and child owns cleanup |
| eligible `unique_ptr<T>` enters a task | accepted only when pointee and stored cleanup state transfer |
| `T&`, `mut T&`, iterator/view carrier, raw pointer, or borrowed capture enters a task | rejected at the boundary with the preventing origin identified |
| class with a declared cleanup body but no unsafe positive assertion enters or is shared | rejected even when its fields are primitive |
| explicit safe negative capability declaration | rejected at every matching boundary |
| explicit unsafe positive assertion over reviewed native state | accepted as an unsafe proof and visible in semantic/HIR/tooling facts |
| immutable share-capable global/static | accepted after initialization-before-entry validation |
| ordinary mutable, borrowed-state, raw-pointer, or cleanup-owning global/static | rejected for the concurrent profile; existing single-threaded source remains valid |
| immutable atomic/mutex global performing contracted mutation | accepted only after its compiler-private operation and runtime support exist |
| two overlapping ordinary accesses that could race | rejected before backend entry |
| recognized SC atomic accesses | accepted and retained as ordered synchronization operations |
| unknown/native call claimed as synchronization without a contract | retained as an optimizer barrier but rejected as proof of happens-before |
| task checked failure followed by explicit or automatic join | task state cleans; the exact original record is re-raised on the joining thread |
| outstanding handle reaches ordinary destruction | automatic join; completion precedes continuation of cleanup |
| environmental automatic-join operation fails during ordinary execution or failure cleanup | `GTI-R0012` or the `GTI-R0014` emergency path, respectively |
| foreign-created thread calls GTI without attachment/exact callback registration | rejected or stopped at the runtime boundary before user GTI code |

Every public slice additionally needs deterministic semantic/HIR/MIR tests,
O0/O3 runtime equivalence, installed-toolchain coverage on each advertised
target, optimizer-ordering assertions, and sanitizer/stress coverage where
supported. No timing-only test is a correctness oracle.

## Consequences

- GTI 1.0 can freeze ownership and optimization assumptions without pretending
  public concurrency already exists.
- Existing programs remain in the current single-threaded profile and do not
  acquire new global diagnostics merely because the backend uses host threads.
- Safe concurrent GTI will reject races before backend entry; unsafe/native
  boundaries retain precise, reviewable proof obligations.
- Transfer and sharing become reusable semantic facts for owners, callables,
  interfaces, globals, and later library constraints.
- Automatic join preserves structured lifetime at the cost of potentially
  blocking cleanup and participating in deadlock.
- Stored and scoped cross-thread borrows, detach, mutex guards, pointer
  atomics, weaker orders, shared ownership across threads, and native callbacks
  retain their explicit prerequisite rows.

## Rejected Alternatives

- **Inherit the C++ memory model.** Rejected because backend expression order,
  traits, thread objects, and atomics are lowering choices rather than GTI
  semantic authority.
- **Allow safe data races as nondeterministic behavior.** Rejected because it
  does not compose with ownership, native hardware, or ordinary optimization.
- **Treat copy/move traits as transfer/share facts.** Rejected because thread
  affinity and concurrent read safety are independent of those operations.
- **Allow references or raw pointers in the first task model.** Rejected until
  structured joins, parent suspension, provenance, and cleanup are represented
  across every exit.
- **Fail when an unjoined handle is dropped.** Rejected in favor of automatic
  join and ordinary structured cleanup; the accepted behavior and its blocking
  cost are explicit.
- **Make atomics and threads a pre-1.0 executable commitment.** Rejected by the
  maintained restriction ledger. The semantic facts that constrain v1 remain
  pre-1.0; the public execution surface stays post-1.0.
