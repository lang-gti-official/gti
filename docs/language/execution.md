# 4. Execution And Runtime Semantics

Status: Partial normative draft

## 4.1 Abstract Execution

A conforming implementation executes the selected program according to GTI
semantics. Translating operations to C++ does not import C++ evaluation order,
undefined behaviour, temporary lifetime, overload resolution, or exception
semantics.

### 4.1.1 Hosted Program Arguments

The owned program-argument entry form is initialized before user code runs.
The implementation preserves the host-provided argument order, each
argument's bytes excluding its native terminating NUL, and empty arguments. It
constructs independent owned `std::string` values and transfers their owning
`std::vector` into `main`; neither the vector nor its strings borrow native
argument storage. The GTI `int` count equals the vector size.

This startup conversion is an implementation operation with GTI-observable
allocation and numeric-failure policy. The reference C++ backend realizes it
in a compiler-generated native entry adapter, but native `argc`, `argv`, and
the adapter's representation are not source-language values or ABI promises.
The exact accepted source forms are specified in
[`programs-and-targets.md`](programs-and-targets.md).

## 4.2 Evaluation Order

Logical `and`/`&&` evaluates its right operand only when its left operand is
true. Logical `or`/`||` evaluates its right operand only when its left operand
is false. Target conditionals select one branch before semantic execution.

**Specification gap:** GTI has not yet assigned a complete order to operands,
call arguments, initialization subexpressions, temporaries, and cleanup at full
expression boundaries. Before 1.0, the language must choose an order or a
precisely bounded set of permitted orders and lower it consistently through
MIR. The selected C++ mode must not decide this rule accidentally.

## 4.3 Numeric Execution

Checked explicit conversions reject out-of-range constants and produce a
defined runtime failure when a dynamic value is outside the target range.
Float-to-integer conversion truncates toward zero after its validity check.

Built-in integer arithmetic first applies GTI's integer promotions and exact
common-type rules. The operation then executes in that fixed-width result
domain. Signed and unsigned `+`, `-`, and `*` produce a defined runtime failure
when the mathematical result is outside that domain; unsigned arithmetic does
not wrap implicitly. Integer `/` truncates toward zero and fails for a zero
divisor or a signed minimum divided by `-1`. Integer `%` fails for a zero
divisor, has the sign of its left operand, and defines signed minimum modulo
`-1` as zero.

Unary integer `-` fails when its result is outside the promoted signed domain.
Built-in increment and decrement use checked addition and subtraction.
Compound assignment evaluates its target place once, applies the corresponding
checked operation, and then performs a checked conversion back to the target
type.

Bitwise operations use the fixed-width two's-complement bit pattern of their
result type. Shift counts are nonnegative and less than the width of the
promoted left operand. Invalid counts produce a defined runtime failure. Left
shift discards bits outside that width, while signed right shift is arithmetic.

An optimizer may replace integer arithmetic only when a typed evaluator proves
the exact in-range result under these rules. If constant evaluation proves that
an ordinary expression would fail, the checked operation remains observable at
runtime; the expression is not made ill-formed solely because an optimization
can see the failure. Contexts that require a valid compile-time value, such as
a fixed-array extent, diagnose their own failed evaluation. This rule does not
relax the existing semantic rejection of a directly written zero divisor or
invalid literal shift count. Implementations must not use overflow behavior
from the compiler host or selected backend as the GTI constant-evaluation rule.

GTI `float` is the IEEE-754 binary32 format: one sign bit, an eight-bit biased
exponent, and 23 stored fraction bits with the usual implicit leading bit for
normal values. Every built-in `+`, `-`, `*`, `/`, and unary negation produces a
binary32 result. Integer operands in a mixed numeric operation are first
converted to binary32. Each conversion and arithmetic operation rounds once
using round-to-nearest, ties-to-even. Operations are not reassociated or
contracted into a fused operation; a future fused operation would require an
explicit language rule.

Floating exceptions do not trap. Division of a nonzero finite value by zero
produces the appropriately signed infinity; zero divided by zero and other
invalid operations produce a quiet NaN; overflow produces infinity; and
underflow is gradual through subnormal values to signed zero. Unary negation
flips the sign bit, including for zero. Positive and negative zero compare
equal, but their signs remain observable through operations such as division.

Any comparison with a NaN operand is unordered: `==`, `<`, `<=`, `>`, and `>=`
are false, while `!=` is true. NaN payload and sign selection after arithmetic
are not language-defined and must not be used as portable information through
native interoperation. Infinity and NaN have no source-literal spellings in
the current grammar, but arithmetic and native calls may produce them.

Integer-to-float conversion uses round-to-nearest, ties-to-even.
Float-to-integer conversion first rejects NaN, infinity, and values whose
truncation is outside the destination domain, then truncates toward zero. The
compiler-owned constant evaluator applies these same rules and retains exact
binary32 bits; it does not calculate through host `float` or `double`.

GTI exposes no dynamic floating-rounding environment. Execution assumes
round-to-nearest, ties-to-even and gradual underflow at every GTI operation
boundary. Native code that changes the host rounding mode or flush-to-zero
state must restore the GTI environment before returning. Failing to restore it
violates that native function's interoperation contract.

## 4.4 Objects And Calls

Arguments and receivers are evaluated, the semantically selected target is
invoked, and its result has the recorded GTI type. Static and virtual dispatch
are decisions of the frontend and HIR/MIR.

A state-bearing base is constructed before fields. Fields are constructed in
declaration order. Cleanup for a live object executes before fields are
destroyed in reverse declaration order. Owned local bindings are destroyed in
reverse declaration order on every applicable scope exit.

A declared cleanup body is non-throwing and cannot be called directly. Its
generated active state ensures that cleanup obligations execute exactly once
across movement.

## 4.5 Control Flow

`if`, loops, `switch`, `break`, `continue`, and `return` follow their grammar
and static control-flow requirements. `continue` in a structural range loop
executes the selected iterator increment before the next condition test.

A `switch` selects the exact matching case value or `default`. Adjacent labels
share an arm. Arms do not fall through implicitly.

## 4.6 Arrays And Checked Access

A fixed array is inline contiguous storage containing exactly its compile-time
extent. It does not decay to a pointer. Constant out-of-range indexing is
ill-formed; dynamic out-of-range indexing produces a defined runtime failure.

The same distinction applies to checked standard-library owners and views:
compile-time proof may reject a program, while a dynamic failure follows the
specified GTI runtime contract rather than native undefined behaviour.

## 4.7 Raw Pointer Execution

An unsafe block is an ordinary lexical execution block. Entering or leaving it
does not alter object lifetime, add a runtime check, or create a cleanup
obligation. A raw-pointer value itself is trivial and non-owning.

Address formation produces the address of the selected live place. Raw
dereference, indexing, and arrow access read or write the selected pointee.
Adding or subtracting an integer offset produces the same pointer type;
subtracting two identical non-`void` pointer types produces `int64_t`. Pointer
arithmetic is valid only within one live object or array allocation. A one-past
value may be formed for arithmetic or equality but shall not be accessed.

Execution of a pointer-bearing C call follows the selected exact native symbol
and C ABI prototype. The implementation does not infer or enforce the native
function's bounds, retention, ownership, nullability, or aliasing policy.
Scalar-only and counted-text C calls do not acquire unsafe execution semantics
merely because their backend representation contains an address.

Each unsafe raw-memory operation requires the validity, lifetime, alignment,
declared type, initialization, bounds, provenance, writable-access, aliasing,
and native-call conditions stated in
[`raw-pointers.md`](raw-pointers.md). Violating an applicable
condition has undefined behaviour. A conforming implementation may diagnose or
instrument unsafe code, but it is not required to insert dynamic checks.

## 4.8 Recoverable Failure

Recoverable failure is represented with `expected<T, E>` and
`unexpected(error)`. Returning an ordinary value from an `expected`-returning
function creates success; returning `unexpected(error)` creates failure. A bare
return creates success only for `expected<void, E>`.

GTI does not currently provide language exceptions or implicit error
propagation.

## 4.9 Concurrency Boundary

GTI has an adopted concurrency and memory-model boundary, with rationale in
[ADR 008](../decisions/008-safe-concurrency-memory-model.md). The compiler does
not yet expose public threads, atomics, mutexes, or foreign-thread entry. The
single-threaded profile is the current and default executable profile.

The future concurrent profile is an explicit target/runtime capability known
before semantic analysis and retained in program, HIR, and MIR facts. It is
never inferred from native link flags, host-library behavior, backend code, or
incidental use of a host thread. Selecting that profile applies the
transfer/share and global rules below; it does not make an unimplemented
operation available.

One **memory location** is a live scalar object or non-overlapping scalar
subobject. Two accesses conflict when they touch the same location or
overlapping lifetime and at least one is a write or lifetime operation. A
**data race** occurs when different threads perform conflicting accesses, at
least one is non-atomic, and neither happens-before the other. Safe GTI shall
make a data race unrepresentable. A race caused through an unsafe raw-pointer
operation, unsafe nominal capability assertion, retained native state, or
foreign code violates that boundary's stated obligation and has undefined
behavior.

Within one thread, **sequenced-before** follows GTI's evaluation and
full-expression rules. **Synchronizes-with** is created only by a GTI operation
whose contract says so. **Happens-before** is the transitive closure of those
relations. A compiler effect such as `maySynchronize` is a conservative
optimization barrier, not proof of a synchronizes-with edge.

The first executable concurrent profile shall provide these edges:

- every evaluation sequenced before successful spawn happens-before task
  entry;
- task completion happens-before successful explicit or automatic join
  returns or continues cleanup;
- every sequentially consistent atomic operation participates in one
  program-wide sequentially consistent order; and
- a future mutex unlock synchronizes-with the corresponding later successful
  acquisition.

The first atomic value domains are fixed-width integers and `bool`. Their
initial operations are sequentially consistent and promise neither lock
freedom nor C-compatible layout. Later accepted order names are `relaxed`,
`acquire`, `release`, `acq_rel`, and `seq_cst`; `consume` is absent. Operation-
specific legality must be checked before lowering.

The first managed thread model transfers only owned values into one consumed
task. References, borrowed-state carriers, raw pointers, and borrowed captures
cannot cross. The move-only handle owns one join obligation, detach is absent,
and destruction of an outstanding handle automatically joins. Automatic join
may block or participate in deadlock. A later scoped-borrow model requires a
represented structured join, child loan, parent suspension, cleanup on every
exit, and verified reactivation after join.

Program-wide initialization completes before initial entry and every managed
spawn. With no detach in the first model, all managed tasks finish before
program-wide destruction begins. Future thread-local values are initialized on
first use in their owning attached thread and destroyed there in reverse
initialization order. Recursive thread-local initialization raises `GTI-R0013`
while the detected state is still safe to clean; references to thread-local
storage cannot cross to another thread.

A future worker checked failure cleans task-owned and initialized thread-owned
state to the task-entry boundary and stores the original section 4.10 record.
Explicit or automatic join re-raises that record on the joining thread; task
capture does not independently invoke the observer. An environmental automatic-
join failure with no result channel raises `GTI-R0012`, and such a failure
during other failure cleanup takes the `GTI-R0014` emergency path.

The model guarantees no fairness. Deadlock, livelock, starvation, priority
inversion, and permitted blocking are not memory undefined behavior. Native
threads may enter GTI only through a generated callback boundary after runtime
attachment, and native operations are not presumed thread-safe from their
signature alone.

## 4.10 Defined Runtime Failure

A defined runtime failure is a non-resumable GTI control effect raised during
a well-formed GTI invocation when a dynamic checked condition fails. The
invocation begins before GTI module/static initialization and includes checked
hosted setup before source `main`, so an initializer or malformed but safely
detected entry state is covered even when the entry body has not run.
A defined failure is neither an `expected` error nor a source or native
exception. GTI source cannot catch it, and an implementation shall not use
native exception or ABI unwinding as the language contract. Cleanup propagation
is compiler-managed, non-resumable language failure unwinding represented by
explicit control-flow edges; GTI has no source handler or resume mechanism.

An ordinary failure carries one immutable fixed-size, versioned, copyable,
allocation-free record containing:

- one stable `GTI-Rnnnn` code and category;
- one bounded category-specific detail; and
- a source-site token consisting of a deterministic artifact identity and an
  artifact-local site index.

The exceptional `GTI-R0014` case uses a separate fixed-size emergency envelope
containing exactly the original ordinary record and the first ordinary record
raised during its cleanup. Later cleanup failures are suppressed. The envelope
is process-terminal and is never returned from an embedding boundary.

An immutable artifact descriptor resolves the token to a logical source unit,
zero-based half-open UTF-8 byte coordinates `[start, end)`, and a one-based
source line. Artifact identity is the 256-bit SHA-256 digest of the canonical
descriptor serialization below and is rendered in reports as 64 lowercase
hexadecimal digits. An implementation must reject, rather than silently merge,
unequal loaded descriptors with the same identity. Equal descriptors may share
an identity because their site lookup is interchangeable; unequal descriptors
remain distinguishable. The byte span is the authoritative machine location.
Logical source names must be deterministic within the artifact and must not
expose a compiler temporary or incidental absolute build path. Generated C++
positions, native addresses, snapshot-local source-unit IDs, and
compiler-internal IDs are not GTI source tokens.

The descriptor is pinned for the lifetime of a loaded hosted artifact or
embedding context, not reference-counted per record copy. Records may be copied
freely while that descriptor is loaded. A host that needs resolved source text
after unloading the artifact/context must copy that resolved information first;
the future embedding ABI must expose this descriptor-lifetime rule explicitly.
The all-zero artifact identity is reserved for the synthetic runtime site and
is never assigned to a loaded artifact.

Logical names are `<prelude>` for the implicit prelude, canonical include
spellings such as `<std/vector>` for standard units, and `/`-separated paths
relative to the manifest package root or, for direct compilation, the entry
file's parent. A source outside that root uses
`<external>/<unit-id>/<basename>`, never an absolute path; `<unit-id>` is the
64-lowercase-hexadecimal rendering of SHA-256 over the source's exact contents
plus its selected route from the entry unit. Select routes by shortest edge
count and then by the lexicographically least complete encoded-route byte
string under unsigned-byte order. A route serializes each include spelling and
that directive's zero-based lexical occurrence in its including unit, so it
exposes no resolved host path and distinguishes equal basenames at different
graph positions; the hash input uses ASCII
`GTI-EXTERNAL-SOURCE-V1`, one zero byte, the source byte length and contents,
the route edge count, and for each edge its include-spelling length/bytes and
occurrence integer, using the encoding below.
Artifact identity is the identity of the immutable
failure descriptor, not of unrelated code bytes, and is derived as follows:

1. Byte strings are length-prefixed with an unsigned 64-bit little-endian byte
   count. Integers are otherwise unsigned 64-bit little-endian values. No host
   structure padding or text encoding is serialized.
2. The byte stream starts with ASCII `GTI-FAILURE-ARTIFACT-V1` followed by one
   zero byte.
3. After concrete semantic/HIR discovery but before optimization, one
   descriptor site is interned for each local checked-failure origin. Origins
   at the same generic definition anchor coalesce and union their outcome pairs
   rather than creating an instance-specific site. Sites are sorted by unsigned
   UTF-8 bytes of the logical source name, then start, end, then their sorted
   `(code, detail)` outcome pairs; byte-identical site records coalesce. Each
   record serializes logical name, line, start, end, outcome count, and every
   code/detail pair in that order.
   Its one-based position is the artifact-local site index; zero is reserved
   for no source site. The stored line is one plus the number of LF bytes
   (`0x0A`) before `start`. Calls and joins that merely propagate a record add
   no site.
   A code is serialized as its unsigned numeric `R` suffix and a detail as its
   length-prefixed ASCII identifier; outcome pairs sort by that number and then
   unsigned detail bytes.
4. SHA-256 over that complete byte stream is the artifact identity. An all-zero
   result is reserved and causes artifact construction to fail. Optimizing,
   changing native symbol names, relocating the artifact, or changing load
   order cannot alter the table or identity. Two code artifacts with identical
   failure descriptors intentionally share an identity; the runtime can compare
   the complete static descriptor bytes to detect a hash collision.

The source anchor is the complete token or token sequence that denotes the
check: a binary, unary, compound-assignment, increment,
decrement, or shift operator; the destination type name of an explicit numeric
conversion; the opening `[` of checked indexing; `*` or `->` for owner access;
the observer/callee name for expected, allocation, private-storage, and trusted
host operations; and the source `main` name for a synthetic hosted-entry check.
The token's exact half-open byte span is recorded. A checked operation in a
generic body identifies its definition site, and a trusted standard-library
check identifies its lexical library operation rather than its caller.

The initial category vocabulary is:

| Code | Stable category | Meaning |
| --- | --- | --- |
| `GTI-R0001` | `integer_overflow` | integer result outside its fixed domain |
| `GTI-R0002` | `division_by_zero` | dynamic integer division by zero |
| `GTI-R0003` | `modulo_by_zero` | dynamic integer remainder by zero |
| `GTI-R0004` | `negative_shift_count` | dynamic negative shift count |
| `GTI-R0005` | `shift_count_out_of_range` | count outside the promoted operand width |
| `GTI-R0006` | `numeric_conversion_out_of_range` | value outside the destination domain |
| `GTI-R0007` | `index_out_of_bounds` | checked index outside its domain |
| `GTI-R0008` | `empty_owner_access` | access through an empty owner |
| `GTI-R0009` | `invalid_expected_access` | access to an inactive `expected` state |
| `GTI-R0010` | `invalid_storage_state` | invalid compiler-private slot/capacity state |
| `GTI-R0011` | `allocation_failure` | infallible allocation exhaustion |
| `GTI-R0012` | `infallible_host_operation_failed` | failure of an explicitly infallible host operation |
| `GTI-R0013` | `hosted_runtime_contract_failure` | malformed or impossible hosted/runtime state detected before unsafe state is exposed |
| `GTI-R0014` | `failure_during_cleanup` | second failure during failure cleanup |

Codes and category meanings are stable identities. Existing codes are never
renumbered or reused; unassigned codes are reserved. Ordinary records use
`GTI-R0001` through `GTI-R0013`; their details are stable snake-case
identifiers from this bounded vocabulary and are printed exactly, rather than
replaced by backend prose:

| Category | Stable detail identifiers |
| --- | --- |
| `integer_overflow` | `addition`, `subtraction`, `multiplication`, `division`, `negation` |
| `division_by_zero` | `integer_division` |
| `modulo_by_zero` | `integer_modulo` |
| `negative_shift_count`, `shift_count_out_of_range` | `left_shift`, `right_shift` |
| `numeric_conversion_out_of_range` | `numeric_cast`, `hosted_argument_count` |
| `index_out_of_bounds` | `fixed_array`, `string_view`, `vector`, `string`, `private_storage` |
| `empty_owner_access` | `dereference`, `member_access` |
| `invalid_expected_access` | `value_on_error`, `error_on_value` |
| `invalid_storage_state` | `duplicate_construction`, `uninitialized_access`, `relocation_capacity`, `invalid_relocation_source`, `occupied_relocation_destination` |
| `allocation_failure` | `unique_owner`, `private_storage`, `element_construction`, `hosted_arguments` |
| `infallible_host_operation_failed` | `stdout_write`, `automatic_join` |
| `hosted_runtime_contract_failure` | `negative_argument_count`, `recursive_thread_local_initialization` |

`GTI-R0014` is not stored in an ordinary record and therefore has no ordinary
detail identifier. It is the effective category of the emergency envelope;
its report uses the exact bounded primary/secondary detail shape specified
below.

A public vector or string checks logical size and uses its public detail;
`invalid_storage_state` denotes a trusted internal invariant failure and must
not replace public bounds checking.

Only a local checked-failure origin selects a code, detail, and site. A direct
or virtual GTI call, constructor call, future task join, or callback-forwarding
edge may propagate an already formed ordinary record, but it preserves every
field byte-for-byte and does not add a caller site or transitive category set.

After a failure record is selected, ordinary execution does not resume. The
implementation follows compiler-generated failure edges and destroys every
fully initialized live temporary, local, owned parameter, and initialized
subobject between the operation and the nearest containment boundary. Cleanup
uses the same child-before-parent and reverse-declaration order as an ordinary
exit and transfers every active cleanup obligation exactly once. A partially
constructed aggregate cleans fully initialized subobjects in reverse
successful-construction order and never invokes the enclosing cleanup body
before the enclosing lifetime begins. The failing operation first releases or
cleans any unpublished partial state it owns.

Failure cleanup is not transactional. Assignment, completed output operations,
native calls, and other effects completed before the check remain observable.
Completion of a GTI output operation means its bytes were accepted by the
runtime service; the terminal failure path does not implicitly flush host or C
library buffers. A GTI-owned buffered output abstraction must define and run
its own failure-boundary flush before it can promise stronger persistence. A
second defined failure during cleanup takes the emergency `GTI-R0014` path:
remaining cleanup and the ordinary observer are skipped, the primary and
secondary sites are reported best-effort, and the process terminates with the
standard failure status. An embedding boundary cannot recover from this
double-failure case.

There are three containment policies:

- The hosted program boundary completes invocation-owned cleanup, invokes the
  optional observer, writes the standard report to standard error, and
  terminates with status `70` through an
  immediate target-equivalent exit. GTI v1 does not admit cleanup-owning global
  state, and native `atexit`, host static destruction, and C++ unwinding are not
  additional GTI cleanup mechanisms. GTI module/static initializers and their
  temporaries execute inside this boundary; their cross-source order remains
  owned by the pending D-EXEC-01 evaluation contract. Native pre-`main`
  initialization is not a containment boundary, and an implementation cannot
  claim conforming initializer failure behavior until that ordering contract
  and its lowering are complete.
- A future generated embedding boundary completes invocation-owned cleanup,
  invokes its observer, and returns the structured record to the host without
  the default report or host-process termination. This does not make the
  failed GTI operation resumable and does not imply a stable GTI ABI today. The
  same context may be invoked again only when all retained program/context
  state remains structurally valid; the generated ABI must expose a poisoned
  state instead when its chosen persistent state cannot meet that invariant.
- A future managed-task entry boundary completes task/thread-owned cleanup and
  stores the original record without invoking the observer. Explicit join
  re-raises that record on the joining thread, distinct from returning a
  recoverable native join error; observation occurs once at the eventual hosted
  or embedding containment boundary. If the adopted task model provides
  automatic join, it likewise re-raises the same record. Worker failure never
  becomes silent or a category-erasing generic thread error. Detach is absent
  from the first task model and requires its own later observation/escalation
  rule.

A future generated callback entered from C or a native thread is a containment
boundary with an explicitly selected host/task policy. An ordinary outbound
`extern "C"` call is not. Native throw, long-jump, abort, or contract violation
does not become a GTI defined-failure record.

An execution environment may select one observer plus host context before an
invocation. That pair is captured immutably for the invocation and the host
keeps it alive until the invocation and every concurrent callback it initiated
complete. The observer receives an immutable record after cleanup and before a
hosted or embedding boundary action. Task capture and callback forwarding do
not independently invoke it. It cannot resume GTI, replace the record,
suppress the hosted report, or change
the exit status. It may be called on any attached thread and concurrently for
independent embedded invocations; it shall return normally, shall not re-enter
GTI, and shall not unwind through the boundary. It receives no runtime
allocation service and must tolerate `GTI-R0011` without assuming allocation
will succeed. If the runtime detects observer return-state corruption, re-entry,
or an escaping native exception, it writes one best-effort standard report of
the original record and immediately terminates with status 70 without observer
re-entry; it does not replace the original record with `GTI-R0014`. An observer
that itself hangs, aborts, long-jumps, or otherwise prevents control from
returning is a host contract violation for which GTI cannot guarantee a report
or status.

The hosted runtime emits exactly one UTF-8 report line:

```text
GTI runtime failure [<code>] <category> in <artifact> at "<source>":<line>@<start>..<end>: <detail>\n
```

`<artifact>` is the record's 64-digit lowercase hexadecimal artifact identity.
`<line>`, `<start>`, and `<end>` are unsigned base-10 ASCII integers with no
leading zeroes except the single digit `0`.
The quoted logical source name preserves only printable Unicode scalar values,
using the Unicode 15.1 `General_Category` values `L*`, `M*`, `N*`, `P*`, `S*`,
and `Zs`; ASCII space is included. Backslash, quote, CR, LF, and tab use the
exact escapes `\\`, `\"`, `\r`, `\n`, and `\t`. Every other UTF-8 source byte,
including a byte from invalid UTF-8 or a scalar outside the allowlist, is
escaped separately as uppercase `\xHH`. This byte-wise fallback prevents NEL,
line/paragraph separators, bidi controls, and future Unicode reclassification
from creating or disguising another physical line. Category/detail identifiers
come from bounded ASCII runtime tables. An unavailable site uses an all-zero
artifact identity and `"<runtime>":0@0..0`. The runtime formats without
allocation. The final `\n` is exactly one LF byte (`0x0A`), independent of host
text-mode newline translation.
Failure to write the report does not change status `70` or recursively report.
Concurrent terminal failures select one process-wide report/observer winner
before immediate termination. A normal user `main` may also return 70; the
report and structured record distinguish that return from failure.

`GTI-R0014` uses the secondary record's artifact and site in the standard
`in`/`at` fields and the detail `failure during cleanup; primary
[<primary-code>] in <primary-artifact> at
"<primary-source>":<line>@<start>..<end>; secondary [<secondary-code>]`.
Later secondary failures are suppressed; formatting or write failure causes
immediate status-70 termination.

`expected<T, E>` is the ordinary recoverable path. Environmental or resource
conditions that callers can reasonably handle use an API returning
`expected`: file/socket operations, future thread creation, recoverable join,
and future `try_make_*` allocation are examples. A checked language operation
or documented type-level-infallible API instead raises defined failure. An
infallible host convenience API shall map its native error to `GTI-R0012`, not
discard it, and should have a recoverable sibling when callers need control.

Consequently wrong-state `expected.value()` and `expected.error()` access uses
`GTI-R0009`; it never inherits native `bad_expected_access`, assertion, or
undefined behavior. `make_unique` and private storage map exhaustion to
`GTI-R0011`, while future recoverable factories return `expected`.

Compile-time diagnostics, compiler resource exhaustion, IEEE-754 nontrapping
results, explicit `expected` errors, native error/sentinel values,
best-effort cleanup-service errors, and violated unsafe raw-pointer or native
obligations are not defined runtime failures. Unsafe obligation violations
remain undefined behavior unless a separately specified checked operation
detects the condition first.

A compiler/runtime integrity fault that cannot preserve ownership invariants,
such as genuinely indeterminate native join state, terminates the process and
is not returned as a containable record. `GTI-R0013` covers only detected
hosted input/state that remains safe to clean; it is not an internal-error
catch-all.

[ADR 007](../decisions/007-defined-runtime-failure.md) records the rationale
for this normative contract.

**Implementation gap:** the transitional C++ emitter still uses duplicated
message-plus-`abort()` helpers, performs no failure cleanup, exposes a
signal-derived status, and lets native expected observers escape this
contract. HIR/MIR also lack explicit failure records and propagation edges.
The frontend also still accepts some declared-cleanup value globals, and the
C++ backend may execute GTI static initialization before its native `main`.
M-FAIL-01 and its co-delivered Q-FAIL-01 runtime/reporting slice own that
failure substrate after ordered MIR evaluation and active-drop authority exist;
M-LIFE-01 and M-BACK-02 own the matching global restriction and executable
closed-body migration.

## 4.11 Temporary And Cleanup Gaps

MIR represents an increasing portion of loans, moves, drops, and control-flow
cleanup. The following lifetime specification and executable-authority gaps
remain:

- the lifetime of every temporary;
- cleanup after partial construction;
- cleanup ordering within all compound expressions;
- complete executable representation of the failure cleanup required by
  Section 4.10; and
- cleanup interaction with any future manual object-lifetime operations.

These gaps are release blockers for a backend-independent 1.0 definition.
