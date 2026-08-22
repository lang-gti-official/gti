# 007: Defined Failure Is Contained, Cleanup-Preserving Termination

Status: Accepted

## Context

GTI already checks integer arithmetic, numeric conversions, indexing, owner
access, compiler-private storage, and infallible allocation. The transitional
C++ emitter currently implements those checks with several local helpers that
write an English message and call `std::abort()`. That implementation loses the
failure category and GTI source site, skips deterministic cleanup, exposes a
signal-derived status, and terminates an embedding process. Invalid
`expected` observers additionally inherit native C++ exception or assertion
behavior.

Those outcomes are incompatible with GTI's backend-independent checked
execution, explicit error values, deterministic cleanup, and future hosted
embedding and task boundaries. A valid program needs one answer that does not
depend on C++ exceptions or on which helper happened to detect the condition.

## Decision

A **defined runtime failure** is a non-resumable GTI control effect. It carries
one immutable failure record, propagates through compiler-generated failure
edges, performs deterministic cleanup, and stops at the nearest explicit GTI
containment boundary. It is not a source exception, is not catchable in GTI,
and shall not use native exception or ABI unwinding as its language model.
Only a local checked detector selects the record; calls, joins, and forwarding
edges preserve it byte-for-byte rather than adding caller sites or transitive
category sets.

The record is a fixed-size, versioned, copyable, allocation-free value. It
consists of:

- one stable failure code and category;
- one bounded category-specific detail chosen by the compiler or trusted
  library operation; and
- one GTI source-site token containing a deterministic artifact identity and
  artifact-local site index.

The ordinary record always describes one primary failure. A separate
fixed-size emergency envelope contains exactly two ordinary records—primary
and first secondary—when failure occurs during cleanup. `GTI-R0014` is the
envelope's effective report category, not a third nested record. The envelope
is process-terminal and is never returned through an embedding boundary.

An immutable artifact descriptor resolves the token to a logical source unit,
zero-based half-open UTF-8 byte coordinates, and one-based source line.
Artifact identity must be content/logical-input-derived rather than a load
address or global counter, so unequal failure descriptors from multiple loaded
GTI artifacts cannot be confused. Equal descriptors may share an identity
because their site lookup is interchangeable. The descriptor is pinned by the
loaded hosted artifact or embedding context, not by each freely copied record.
A host that unloads the context must first copy any resolved information it
wishes to retain; a future stable embedding ABI must make that
descriptor-lifetime rule explicit.
The all-zero identity is reserved for the synthetic runtime site.
The byte span is the stable machine coordinate. A display path and richer
line/column presentation may be derived from the table, but an absolute build
path, generated C++ location, native address, snapshot-local source-unit ID,
HIR/MIR ID, or process-global counter is never part of the record. A check
inside a generic body identifies the generic definition site. A trusted
standard-library check identifies the lexical GTI operation in that library;
call-stack or caller attribution is a later diagnostic extension. Generated
program-entry checks use a synthetic operation anchored to the selected source
`main` declaration.

Hosted artifact construction assigns logical display names as follows:

- the implicit prelude is `<prelude>` and a standard unit is its canonical
  include spelling such as `<std/vector>`;
- project sources use `/`-separated paths relative to the manifest package
  root, while direct compilation uses paths relative to the entry file's
  parent; and
- a source outside that logical root uses a deterministic
  `<external>/<unit-id>/<basename>` name rather than an absolute path. The
  artifact-local unit identity includes content and include-graph position, so
  two same-named identical files remain distinguishable.

The artifact identity is the SHA-256 digest of the canonical pre-optimization
failure-site descriptor serialization specified by execution semantics.
Reports render it as 64 lowercase hexadecimal digits. Target
selection affects the digest only when it changes that descriptor; two builds
with byte-identical failure descriptors may share an identity. Unequal
descriptors with the same identity are rejected rather than silently merged.

The accepted vocabulary gives separate stable codes to integer-domain,
conversion, bounds, owner, expected-state, private-storage, allocation, host,
hosted-contract, and cleanup failures. Detail identifiers retain the concrete
operation without turning backend prose into compatibility identity. New
categories receive new codes; existing codes are never renumbered or reused,
and unassigned codes remain reserved. The normative code/detail table,
source-token rules, and exact report format are maintained only in
[execution semantics](../language/execution.md#410-defined-runtime-failure).
Public container operations must check their logical size and use `vector` or
`string`; reaching `invalid_storage_state` through a well-formed public
operation is an implementation/library invariant defect, not a public bounds
category. The current wrappers and private storage lowering do not yet enforce
that separation in every capacity-versus-size case.

### Cleanup And Propagation

After the failing operation has selected its record, ordinary GTI execution
does not resume. The implementation follows explicit failure successors and:

1. cleans every fully initialized live temporary, local, parameter-owned
   value, and initialized subobject between the failure site and the boundary;
2. cleans the fully initialized subobjects of a partially constructed
   aggregate in reverse successful-construction order, without invoking the
   enclosing cleanup body before the enclosing lifetime began;
3. preserves the same LIFO full-expression, child-before-parent, and reverse
   successful-initialization order used by an ordinary control-flow exit under
   [Execution Section 4.2](../language/execution.md#42-evaluation-order); and
4. transfers each active cleanup obligation exactly once.

The operation that detects failure must first restore or discard its own
unpublished partial state. Cleanup does not roll back completed assignment,
completed output operations, native calls, or other observable effects that
preceded the failure; a containment boundary is not a transaction. Output
completion means bytes were accepted by the runtime service. Immediate
termination does not implicitly flush host or C library buffers; a later
GTI-owned buffered abstraction must define any stronger failure-boundary flush.

If cleanup itself raises a second defined failure, further language cleanup is
not trustworthy. The runtime takes the emergency `GTI-R0014` path, retains
both primary and secondary sites in its bounded report data, skips the normal
observer, writes one best-effort emergency report, and terminates the process
with the defined failure status. No embedding boundary may recover from a
failure during failure cleanup.

This is compiler-managed, non-resumable language failure unwinding represented
as explicit control flow. It does not add `throw`, `try`, `catch`, a
source-visible handler/unwind facility, or permission for a native exception
to cross a GTI, `extern "C"`, callback, embedding, or thread-entry boundary.

### Containment Boundaries

The containment boundary, rather than the failure site, chooses the final
environment action:

- The hosted program boundary cleans the failing invocation, invokes the
  optional failure observer, writes the standard report to standard error, and
  terminates with status `70` using an
  immediate target-equivalent exit. Native `atexit`, host static destruction,
  or C++ unwinding is not an additional GTI cleanup mechanism.
- A future generated embedding boundary cleans only the invocation-owned GTI
  state, invokes its observer if configured, and returns the structured record
  to the host. It does not write the default report or terminate the host
  process. Returning a failure record does not make the failed GTI expression
  resumable and does not undo earlier effects. Re-entry is permitted only when
  retained context state remains structurally valid; otherwise the generated
  ABI must expose a poisoned context.
- A future managed-task entry boundary cleans task-owned and thread-owned GTI
  state and stores the original record without invoking the observer. Explicit
  join re-raises that record on the joining thread, distinct from returning a
  recoverable native join error, so the eventual hosted or embedding boundary
  observes it exactly once. ADR 008 adopts automatic join, and that path also
  re-raises the same record. A worker failure is never silently discarded or
  replaced with a generic thread category. Detach is absent from the first task
  model; a later detach design must choose an explicit observation/escalation
  rule before it becomes safe.

The current generated callback wrapper entered from C under the single-threaded
profile is a terminating containment boundary. It completes the selected GTI
target's verified cleanup, forwards an original failure record to the runtime
terminal primitive with no callback-local observer, and never returns failure
through C. Its `noexcept` adapter catches a native exception and terminates
rather than translating it. An ordinary outbound `extern "C"` call is not a
boundary. Foreign/native-thread callback entry and a host/task callback policy
remain future work. A native long-jump, abort, or contract violation does not
thereby create a GTI defined-failure record.

Direct calls to generated C++ symbols are not embedding boundaries. GTI does
not yet have a stable callable ABI or separate compilation, so this decision
defines the required behavior of a future generated wrapper rather than
claiming that current artifacts are embeddable. A wrapper may be offered only
when its owned state and failure channel are represented explicitly.

### Observer, Report, And Exit Status

An execution environment may select one failure observer plus host context
before an invocation begins. That pair is immutable for the invocation and the
host keeps it alive through every concurrent callback the invocation initiated.
The observer receives an immutable borrowed record after successful
cleanup and before a hosted or embedding boundary action; task capture and
callback forwarding do not invoke it independently. It is observational: it
cannot resume GTI, replace the record, suppress the hosted report, or change
status `70`.
It may run on any attached thread and concurrently for independent embedded
invocations. It shall return normally, shall not re-enter GTI, and shall not
allow a native exception to escape. It receives no runtime allocation service
and must tolerate an `allocation_failure` record without assuming allocation
will succeed. If the runtime detects return-state corruption, re-entry, or an
escaping native exception, it writes one best-effort standard report of the
original record and immediately terminates with status 70 without observer
re-entry; that host violation does not replace the original GTI record with
`GTI-R0014`. GTI cannot guarantee a report or status when observer code itself
hangs, aborts, long-jumps, or otherwise prevents control from returning.

The hosted runtime emits exactly one UTF-8 report line:

```text
GTI runtime failure [<code>] <category> in <artifact> at "<source>":<line>@<start>..<end>: <detail>\n
```

`<artifact>` is the record's 64-digit lowercase hexadecimal identity and
`<source>` is its logical display name. The normative Unicode allowlist,
byte-wise fallback escapes, half-open byte coordinates, runtime-site sentinel,
and single-LF terminator are defined in execution semantics. The runtime
formats without allocation.
A failed report write does not change the exit status or cause recursive
reporting. When concurrent terminal failures race,
one process-wide winner invokes the observer and emits the report before
immediate status-70 termination; other failures cannot interleave a second
report. Status 70 may also be an ordinary value returned by user `main`; the
report and structured record distinguish failure from a normal return.

The emergency `GTI-R0014` report uses the same escaping and one-line rule, with
the secondary record's artifact and site as its primary `in`/`at` fields and
this exact detail shape:

```text
failure during cleanup; primary [<primary-code>] in <primary-artifact> at "<primary-source>":<line>@<start>..<end>; secondary [<secondary-code>]
```

Later secondary failures are suppressed. Failure while formatting or writing
this emergency line terminates immediately with status 70.

### Recoverable And Terminating Operations

`expected<T, E>` remains the only ordinary recoverable failure path. An API
uses `expected` when a caller can continue safely and the condition is part of
normal environmental or resource handling: files, sockets, future thread
creation and recoverable join errors, and future `try_make_*` allocation
factories are examples.

A checked language operation or documented type-level-infallible API uses
defined failure when it cannot return an error value. A convenience host API
with an infallible signature must map its host error to `GTI-R0012`; it shall
not discard a native error. A corresponding recoverable API should be provided
when callers reasonably need to handle that condition.

In particular, `make_unique` and private storage remain infallible at the type
level and map allocation exhaustion to `GTI-R0011`; future `try_make_unique`
returns `expected`. Wrong-state `expected` observers map to `GTI-R0009` rather
than importing `bad_expected_access`, assertions, or undefined behavior.

Compile-time diagnostics, compiler/tool resource exhaustion, IEEE-754
nontrapping results, explicit `expected` errors, native sentinel/error values,
best-effort cleanup-service failures, and violated unsafe raw-pointer/native
obligations are not defined runtime failures. Unsafe obligation violations
remain undefined behavior unless a separately specified checked operation
detects one before the violation occurs.

A compiler/runtime integrity fault that cannot preserve ownership invariants,
such as genuinely indeterminate native join state, is also not a containable
record. The runtime terminates the process immediately because returning to an
embedder could leave live code accessing state that cleanup has destroyed.
`GTI-R0013` is only for detected hosted contract input/state that is still safe
to clean; it is not a catch-all internal-error category.

## Phase Ownership

- Semantics owns which resolved detector operations can originate which local
  categories/details and which call-like operations may propagate an existing
  record.
- HIR owns those concrete local origins and canonical frontend source anchors;
  calls do not acquire a transitive category set.
- A backend-independent metadata builder owns the canonical pre-optimization
  site table, artifact-local `FailureSiteId` assignment, descriptor digest, and
  logical-name/root plumbing. It consumes frontend sources plus HIR and supplies
  immutable metadata to MIR and backends.
- MIR owns origin-versus-propagation `Invoke` edges, cleanup, and containment
  control flow using artifact-local site IDs. A `mayTrap` boolean or retained
  HIR pointer is not sufficient authority for a MIR-only backend, and a
  propagating edge never re-sites or rewrites the original record.
- Optimizers may remove an outcome only with a GTI proof that it is impossible;
  they preserve the first observable category, site, cleanup, and prior effects.
- Backends lower the verified MIR contract. They do not select categories,
  derive locations from generated code, or substitute native exceptions.
- The runtime owns fixed record/descriptor primitives, allocation-free lookup
  and formatting, the observer-call primitive, report I/O, terminal
  arbitration, and terminal status. Generated hosted, task, callback, and
  embedding wrappers own containment policy, compiler cleanup completion,
  context guards, and record return/storage. The runtime does not decide source
  semantics, caller cleanup, or embedding handoff policy.

M-FAIL-01 and its co-delivered Q-FAIL-01 runtime/reporting slice implement the
current hosted-program portion of this decision over ordered MIR evaluation,
active-drop authority, failure-aware calls, rollback for supported partial
initialization, and reusable record/firewall machinery. The bounded
same-thread callback row integrates its verified failure sibling and
terminating firewall with that machinery. Future task, foreign-thread callback,
and E-EMBED-01 rows must test their own concrete boundaries against it; a
runtime-call replacement alone does not pass this decision.

## Alternatives

- **Immediate process abort with no cleanup.** This matches the current helper
  mechanism and is substantially smaller, but it makes checked arithmetic or
  indexing bypass GTI cleanup, prevents same-process containment, and turns an
  emitter limitation into the language's RAII boundary.
- **Source exceptions or native exception unwinding.** Rejected because GTI's
  recoverable model is explicit, exceptions do not compose with the current C
  boundary, and C++ exception ABI is not a language contract.
- **Make every checked operation return `expected`.** Rejected because it
  changes ordinary arithmetic and indexing types, obscures programming errors,
  and duplicates the explicit recoverable APIs that already exist.
- **Let an observer resume or suppress failure.** Rejected because code after
  the failed operation has no value/state with which to continue, and a
  process-wide recovery hook would bypass lexical ownership proofs.
- **Use absolute paths or native stack traces as source identity.** Rejected
  because they are build-dependent, may disclose host paths, and are neither
  deterministic nor backend-independent.

## Consequences

The implementation must eventually give potentially failing calls an explicit
failure channel and make cleanup complete before MIR-backed emission of those
bodies. That is broader than replacing `abort()` with one runtime call, but it
preserves GTI's existing ownership premise and provides a real seam for
embedding and tasks.

Failure propagation must be allocation-free after a record is selected.
Cleanup bodies should avoid failure because a second failure is necessarily
process-fatal. Public infallible wrappers need a recoverable sibling when host
failure is an expected operating condition. The current `std::print` status
discard and native `expected` observers are implementation gaps that their
owning implementation rows must close.
