# Standard Library, Compiler Capabilities, And Runtime

Status: Current architecture.

GTI separates public library policy from irreducible compiler capabilities and
host implementation:

```text
public std API in ordinary GTI
        -> private gti_internal capability or bounded extern "C" declaration
        -> runtime C ABI where needed
        -> host implementation / operating system
```

## Public Library

`stdlib/prelude.gti` is implicitly available. Optional source units live below
`stdlib/std/` and are loaded through `<std/name>`. Public classes and functions
under `std` own API policy, logical state, error handling, and RAII. The
compiler must not recognize public wrapper names such as `std::vector` or
`std::make_unique` as shortcuts for container or ownership behavior.

The implicit prelude is deliberately limited to language-wide vocabulary,
shared compiler-capability declarations, `std::move`, and the minimal hosted
output surface. Header-owned policy is not ambient: the `std::unique_ptr`
family and its private owner/storage capabilities are declared by
`<std/memory>`, checked one-slot optional storage is declared by
`<std/optional>`, the private file-descriptor bridge is declared by
`<std/cstdio>`, and defined integer arithmetic intrinsics are declared by
`<std/numeric>`. Atomic, iterator, and callable compiler constraints likewise
live with their public header. Optional units do not re-export their own
dependencies, so application source names the canonical unit for every API it
uses directly.

Integer `std::print`/`std::println` overloads, `<std/string>`'s integer
`std::to_string`, and `<std/format>`'s bounded sequential replacement are a
deliberate boundary example. Digit extraction, brace validation, exact
argument counting, owning-string construction, and format errors are ordinary
GTI source. The frontend's bounded comma-pack fold supplies only exact ordered
calls over a pack; it does not recognize any public formatting name or parse a
format string. Counted literal writes and scalar dynamic-code-unit writes are
the only host operations.

The hosted program-entry signature is one deliberately narrow exception to
name-independent library evolution: semantics validates the canonical
installed `std::vector<std::string>` declaration identities as the exact
language-defined argument type. It does not grant either class intrinsic
container behavior. Semantics also resolves and records the exact public
append operation used by startup conversion, so the backend does not infer a
method from the spelling `push_back`.

### Executable Surface Census

`tests/stdlib_surface_manifest.json` is the reviewed evidence ledger for the
shipped source surface. It fingerprints `stdlib/prelude.gti` and every
`stdlib/std/*.gti` unit, then classifies each inventoried public operation as
`executable`, an intentional declaration `scaffold`, or currently
`unsupported`. A changed or added source unit therefore requires an explicit
surface review; the manifest does not parse GTI or become API authority.

Every executable entry names at least one minimal concrete fixture and any
required generic instantiation. The `stdlib_surface` CTest compiles those
fixtures through the production compiler/native-driver path at O0 and O3 and
checks exact exit status, stdout, and stderr. Unsupported entries may name one
bounded expected-diagnostic case; scaffolds never masquerade as executable
coverage. `stdlib_surface_manifest` additionally injects missing source,
stale fingerprint, missing executable evidence, and wrong evidence-kind
mutations. The checked-in manifest exhaustively classifies every shipped
standard-library source unit and its executable, diagnostic, or scaffold
evidence, including managed-thread construction and join, concrete `std::pair`
construction/field access, and the
`std::forward_list` front operations and exclusive-node algorithms. Reverse,
resize, merge, remove, unique, and sort are ordinary GTI; they transfer node
owners instead of requiring a retained nullable traversal borrow. Clear and
destruction drain nodes iteratively. No public container name has compiler
recognition.

## Compiler-Private Capabilities

`gti_internal` declarations represent operations ordinary GTI cannot yet
express safely, including unique ownership and partially initialized storage.
The source graph assigns every unit one explicit role: application, implicit
prelude, or installed standard-library source. Only the prelude and physical
units below the configured standard-library root are compiler-trusted. A
quoted application file, an override-only path beneath that root, or a source
spelling cannot mint that role.

Semantic analysis grants intrinsic behavior by exact trusted declaration
identity. The shared private `prefix_storage<T>`, `text_view`, and `c_string`
type declarations receive compiler-capability identities only when the exact
trusted prelude declarations are selected. Module-specific capabilities bind
to one exact installed standard-library unit: `unique_owner<T>` and
`storage<T>` to `<std/memory>`, `optional_storage<T>` to `<std/optional>`,
managed threads to `<std/thread>`, atomics to `<std/atomic>`, mutexes to
`<std/mutex>`, and defined integer arithmetic to `<std/numeric>`. Iterator and
callable structural constraints are similarly owned by `<std/iterator>` and
`<std/functional>`. Aliases preserve the selected semantic type; reusing a
declaration or qualified spelling in application source creates no capability.
HIR/MIR retain selected intrinsic identity, and the backend queries semantic
capability facts rather than recognizing a source name before choosing its
representation.

Capabilities expose only irreducible representation invariants. Public logical
size, growth, allocation policy, and container behavior belong in ordinary GTI
wrappers. The sparse-storage allocation extent and initialized-slot bitmap are
the exception in kind, not in authority: narrow private observers let the
source-defined `std::storage<T>` wrapper expose checked `capacity()` and
`initialized(index)` without exposing an address, bitmap, raw mutation, or
deallocation operation.

The first public typed-allocation slice is the ordinary source-defined
`std::unique_ptr<T>`, `std::make_unique`, `std::upcast_unique`,
`std::allocator`, `std::storage<T>`, `std::alignment`, and `std::alloc_errc`
surface in `<std/memory>`. The compiler recognizes none of those public names.
The stateless allocator calls one recoverable private allocation operation; a
successful storage owner carries allocation extent, alignment, per-slot live
state, reverse cleanup, and release authority together. Allocation failure is
returned as `expected`, while bounds and invalid slot-state operations use the
normal defined-failure route. The public owner requires `std::movable T`
without borrowed state. Construction consumes one completed `T`, so failure
while creating the argument remains under ordinary MIR cleanup;
arbitrary in-slot emplacement is intentionally not delegated to the private
C++ representation. ADR 022 owns the public provenance and failure contract.

`std::arena<T>`, `std::pool<T>`, `std::try_make_arena`, and
`std::try_make_pool` are the first bounded allocation-policy clients. They are
also ordinary GTI source and retain logical size, monotonic/reset policy,
lowest-empty-slot reuse, and public error mapping outside the compiler. Their
constructors consume newly allocated `std::storage<T>` behind an ordinary
source-defined `gti_internal::empty_storage_adoption` seal. The private
namespace makes the precondition unavailable to applications; the seal itself
has no intrinsic identity or backend behavior. HIR therefore selects ordinary
public factories and methods, then reaches exact private storage operations
only inside their bodies.

The unique-owner upcast capability consumes one private owner after semantics
has proved an exact public derived-to-base relationship and polymorphic
destruction. The storage shift capabilities move initialized slots through a
verified empty slot. Public `std::upcast_unique`, `vector::insert`, and
`vector::erase` remain ordinary GTI policy; neither public name is recognized
by the compiler or backend.

`std::optional<T>` is ordinary source-defined policy over the exact trusted
`gti_internal::optional_storage<T>` capability from `<std/optional>`. The
private representation contains one correctly aligned inline `T` slot and one
engagement bit; it performs no allocation merely to represent absence. Its
five operations observe engagement, construct into an empty slot, borrow a
live value read-only or exclusively, and reset a live slot. Semantics records
their selected identities, owner-tied loans, mutations, and defined-failure
outcomes. HIR and MIR retain those facts, whole-program verification checks the
exact class shape and each operation's result, loan, lifecycle, and failure
contract, and the C++ backend selects the inline representation only from that
verified capability row.

Allocation-free describes the optional slot itself. Constructing `T` may still
execute user code and report its ordinary `allocation_failure` /
`element_construction` outcome. A failed emplacement leaves the slot empty.

The public class owns empty/engaged policy, checked access, reset/emplacement,
and move-only ownership. Moving transfers the live payload and retires the
source ownership state exactly once. Copy construction and both assignment
forms fail closed in the current slice; GTI cannot yet provide a source-defined
special-member assignment body that conditionally destroys or constructs the
private slot. Applications replace a value through `reset()` or `emplace()`.
No public `std::optional` name is recognized by semantics or a backend.

The same identity boundary applies to generic constraints. The prelude binds
private unary and structural concept declarations to compiler facts, and
compiler-trusted standard-library units compose them into public source
concepts such as `std::input_iterator`, `std::sentinel_for`, and
`std::accumulates_into`, plus the unary `std::transferable` and
`std::shareable` type facts. Semantics recognizes only the selected private
declaration and its exact type relationship. It does not recognize those public
names or `std::accumulate`, and application source cannot name the underlying
`gti_internal` capability.

## Managed Concurrency Boundary

The public managed-thread type is the ordinary source-defined `std::thread`
class in `<std/thread>`. The compiler does not recognize that class, its
constructors, `join`, or its destructor by qualified spelling. Its move-only
handle state and automatic-join policy belong in GTI source and use the
ordinary class lifecycle rules. Stop tokens are not part of this first slice.

`std::thread` is GTI's normal managed-thread model. Unlike C++'s class with the
same spelling, it always owns a join obligation and has no detach operation.
Any later unstructured or detached task facility requires its own explicit
lifetime, shutdown, and unobserved-failure contract rather than weakening
`std::thread`.

The same source unit defines `std::thread_result<Result>` for a worker that
returns one owned value. Its `start` consumes an exact `Result()` task, its
explicit `join` returns `expected<Result, thread_errc>`, and destruction joins
then abandons an unobserved value exactly once. GTI does not infer either
public class by qualified spelling.

The ordinary `<std/atomic>` source unit defines move-only `std::atomic<T>` and
its compare/exchange result policy. Semantics does not recognize either public
name. The exact trusted unit alone owns the compiler-private
`gti_internal::atomic_storage<T>` capability and load, store, exchange, and
strong compare/exchange declarations. The capability admits only `bool` and
fixed-width integers. Distinct public static order-tag overloads route to exact
private declarations, whose selected identity records the operation and order
pair in semantics. HIR preserves that record, MIR verifies the complete legal
matrix, and the sealed C++ plan maps private storage and the verified order to
their native representations. The backend never infers an atomic from a class
name or reconstructs an order from source syntax.

Fixed-width integer `fetch_add`, `fetch_sub`, `fetch_and`, `fetch_or`, and
`fetch_xor` remain source-library policy: private generic update helpers form
strong compare/exchange loops from those same exact declarations. Consequently
MIR sees and verifies every initial load, retry, and order pair, while the
backend gains no fetch-specific public-name rule. Checked add/subtract failure
occurs before the attempted compare/exchange publishes a value; native C++
wrapping is not semantic authority.

Only the irreducible host boundary is compiler-private. Its capability surface
is the identity-bound `gti_internal::thread_handle` and
`gti_internal::thread_result_handle<Result>` plus their empty, spawn,
joinable, explicit-join, and automatic-join operations. These declarations are
owned by the exact trusted `<std/thread>` unit rather
than the implicit prelude, so programs that do not import the module retain no
thread declaration bodies or runtime bridge dependency. A compiler-generated,
type-specific task-entry adapter erases the exact closure only after semantic
analysis proves one consumed exact `void()` or `Result()` callable and
transfer-capable captures and result. HIR, MIR, `LoweredProgram`, and the C++
backend retain and verify the adapter identity and exact result type. The
runtime owns only the native thread, opaque state, task destruction, typed
result take-or-abandon callbacks, and fixed worker-failure handoff; it does not
own public class policy or know a GTI standard-library type.

The synchronization contract gives a resolved private capability call
an exact backend-independent synchronization record. HIR and MIR distinguish
thread spawn/join, atomic operation families, and mutex lock/unlock; atomic
records retain legal order dimensions. MIR rejects these records in the
single-threaded execution profile except in an exact, lowering-marked dormant
compiler-trusted thread-, atomic-, or mutex-wrapper body. That exception lets
complete unused `<std/thread>`, `<std/atomic>`, and `<std/mutex>` declaration
inventories exist; it cannot create a task adapter or executable
synchronization use and does not bypass the public semantic profile check.
Optimizer effects preserve every record as a barrier. The concurrent profile
plus target `threads` capability admits the implemented operations. Spawn
transfers one exact
owned task to the worker, successful explicit or automatic join establishes
completion-before-return, and no public detach operation exists.

The mutex surface follows the same policy split. Exact private storage and
lock-token declarations lower to the native mutex preamble only when a
concrete capability instance exists. `std::mutex<T>` remains source policy;
its guard carries a verified owner dependency and lock token, guard reads
publish only owner-tied MIR loans, and reverse deterministic cleanup emits the
one unlock before normal return or failure propagation. The runtime has no
separate public mutex ABI.

Worker checked failure completes the worker's verified MIR cleanup before the
generated adapter copies the exact ordinary record or `GTI-R0014` envelope and
its immutable descriptor pointers into runtime-owned state. The worker does
not invoke the observer or report. The runtime then destroys the transferred
callable and captures as the final task-entry cleanup phase. A failure there is
stored as the worker outcome; if the body already failed it completes the
exact `GTI-R0014` envelope, while an unrepresentable third failure terminates
for integrity. Explicit or automatic join consumes that state and re-raises it
on the joining thread. Recoverable native creation and explicit-join errors
remain `expected` results; an incoherent native join state is an integrity
termination rather than a forgeable recoverable handle. An automatic-join
host failure transfers its still-coherent state into a private terminal
quarantine and raises `GTI-R0012`; this is neither detach nor a resumable
ownership state.

## Native And Runtime Boundary

`runtime/include/gti/c_abi.h` defines the public C-compatible counted-text
record. `runtime/include/gti/runtime.h` defines runtime entry prototypes, and
`runtime/src/` implements host behavior. Bounded `extern "C"` GTI declarations
carry exact external symbols and an allowlisted ABI selected in semantics.

The original stdout and single-byte input/file entries remain compatibility
symbols. The version-one I/O family adds counted stdout/stderr writes, bounded
stdin/file reads, typed file open, complete file writes, file seek, and
standard-stream flush. It knows only counted bytes, descriptors, fixed-width
status values, and stable mode/origin ordinals. It does not know about
`std::FILE`, `std::string`, formatting, CSV records, buffering, or encoding.
Portable ownership, chunk allocation, mode spelling, EOF policy, and
`expected` translation remain ordinary `<std/cstdio>` source.

Exact `char`/`uint8_t` conversion bridges the byte representation in trusted
source without making `char` numeric or adding an implicit conversion. Chunk
reads use one fixed internal array and return an owned string; application code
never receives the mutable runtime buffer or native descriptor.

The hosted runtime independently serializes each stdout and stderr entry with
one process-wide mutex per stream. The bytes of one call do not interleave with
another call, but a source-level operation implemented by several calls may
interleave between those calls. POSIX writes temporarily block `SIGPIPE` only
on the calling thread, consume a signal generated by the current failed write,
and restore the caller's prior mask. A pre-existing pending signal is never
consumed.
This serialization is a host-service integrity property, not a language
`synchronizes-with` edge. Failure-state publication is protected inside each
managed worker and consumed after native join. File, socket, and opaque native
owners remain governed by their transfer/share facts; the runtime does not
make a thread-affine or unsynchronized handle safe merely because it can be
moved as native storage. Unknown native calls remain conservative effect
barriers and retain their documented unsafe and thread-safety obligations.

`<std/random>` keeps seeded pseudo-random state and sequence policy in ordinary
GTI source. Its `std::random_device` crosses the bounded runtime entry
`gti_rt_random_bytes` only when it refills an internal fixed array; the caller
supplies the checked buffer and count inside the lexical raw-pointer `unsafe`
boundary. The runtime contributes host entropy and a status code, not an
engine algorithm or public-library object representation.

`@runtime` remains a closed compiler-validated compatibility surface. New host
APIs should prefer ordinary GTI wrappers over the bounded native boundary when
the existing ABI can express them. Neither mechanism turns a public GTI class
layout into an ABI.

`<std/cmath>` supplies ordinary GTI wrappers for its complete declared
binary32 math family. They lower as ordinary verified-MIR native calls to
version-one `gti_rt_math_*_f32_v1` entries in `runtime/include/gti/math.h`.
`runtime/src/math.cpp` uses the host's float math implementations; the seam
passes and returns only scalar `float`, with no class layout, allocation,
ownership transfer, public-name intrinsic, or alternate backend body path.
Classification and absolute value remain GTI source. Domain, pole, overflow,
and underflow results follow the documented floating-point value contract,
not the defined-integer-failure contract. Host errno and floating exception
flags are not exposed to applications.

`<std/cthread>` begins with ordinary GTI `std::thrd_yield()` over the
version-one `gti_rt_cthread_yield_v1(void)` C entry in `gti/cthread.h`.
`runtime/src/cthread.cpp` implements the host scheduling hint. The operation
is thread-safe, non-owning, allocation-free, and has no failure result or
GTI synchronization edge. It is legal in either execution profile because it
creates no execution agent and accesses no application memory. The existing
conservative native-call effects and verified-MIR emission own execution;
no public-name recognition, concurrency intrinsic, or AST/HIR fallback is
introduced. The module's remaining C-style families require their own
attachment, ownership, synchronization, and failure contracts.

The source-private `runtime/src/cthread_state.cpp` now supplies the native
create/join and registered-entry prerequisite. It is a separate archive member,
so yield-only clients do not acquire the registry. `cthread_internal.h` is not
installed and no GTI declaration or generated backend path calls these helpers
yet. This is runtime implementation evidence, not a public C-thread API or
permission for concurrent GTI callback conversion.

One initial thread starts the service once and alone owns shutdown. Nonzero
64-bit handle identities are never reused; the registry retains each worker
until exactly one successful join publishes its signed 32-bit result and
ordinary writes. Creation failure leaves the output and caller-owned context
untouched. Recoverable join failure leaves the handle live; invalid, consumed,
self, and competing joins cannot consume another obligation. The caller still
owns raw context lifetime and synchronization—this is not managed task transfer.

The worker installs a thread-local registration for the exact entry function.
An adapter can claim that registration once, then leave once after its own
cleanup. Initial/unregistered threads, mismatched callbacks, recursion, and
replay fail the check. Native exceptions and an entered-but-unfinished adapter
terminate at the host firewall; they do not manufacture a GTI failure record.
Shutdown closes creation and returns busy while any registered worker or join
obligation remains, allowing existing workers to finish and owners to join.
Only an empty registry permits successful shutdown, and shutdown cannot reopen
the service. This bounded service intentionally adds neither detach nor general
foreign-thread registration. The compiler integration must still represent
hosted startup/shutdown, exact entry and synchronization contracts, and defined
failure cleanup/containment in verified MIR before exposing create/join.

## Defined-Failure Boundary

[Execution §4.10](../language/execution.md#410-defined-runtime-failure) assigns
checked-failure meaning to compiler-owned facts and leaves the runtime a narrow
environment role. Semantics/HIR select local origins and propagation, the
failure-metadata builder assigns artifact-local sites, MIR retains their exact
detector mappings, and later MIR slices own propagation and cleanup control
flow. A generated boundary/runtime pair may serialize,
observe, return, or terminate with the completed record, but it may not infer a
category, choose caller cleanup, or use a generated/native source location.

`runtime/include/gti/runtime_failure.h` now provides the version-one C
boundary. Its
stable code and detail ordinals exactly match the compiler vocabulary. On the
supported 64-bit runtime ABI, the ordinary record is 48 bytes, an allowed
outcome is 8 bytes, a site descriptor is 56 bytes, an artifact descriptor is
72 bytes, and the two-record emergency envelope is 104 bytes. Every reserved
field must be zero. A nonzero record identity and one-based site must match the
artifact descriptor and the site's exact allowed `(code, detail)` set. The
all-zero identity with site zero is the sole descriptor-free runtime sentinel
and resolves to `"<runtime>":0@0..0`.

The production C++ backend now emits one immutable ABI-v1 artifact descriptor
from the exact verified MIR metadata snapshot. Internal constant tables retain
the artifact identity, canonical descriptor bytes, one-based sites, counted
logical-source bytes, unsigned-64 line and byte spans, and the canonical
allowed outcomes for every site. Empty-site artifacts retain their canonical
bytes and identity with a null site pointer. A failure-capable MIR body creates
an exact record on its local failure edge, forwards it through the transformed
native ABI, and reaches the hosted terminal primitive only after its verified
cleanup. Descriptor emission alone never grants failure or containment
authority. There is no public no-MIR `CppEmitter` route.

The runtime formats and writes ordinary and cleanup-failure reports without
allocation, invokes the optional hosted observer over a protected record copy,
and arbitrates one process-wide terminal winner. Observer re-entry, mutation of
the presented copy, or an escaping C++ exception preserves and reports the
original record before immediate status-70 termination. POSIX report writes
retry `EINTR`, complete partial writes, and contain a broken-pipe `SIGPIPE` in
the reporting thread; Windows uses binary `WriteFile` so the final LF is not
translated. Report-write failure is nonrecursive and does not change terminal
status. The generated hosted adapter owns process containment, the same-thread
native callback adapter owns its terminating firewall, and generated managed-
task adapters own worker handoff after verified cleanup. Future embedding and
foreign-thread callback wrappers still own their containment policy, cleanup
completion, context guards, and record return/storage. This boundary does not make the
failure record the ABI of a public GTI class and does not establish a general
callable ABI.

Quoted source names use a strict UTF-8 decoder and a checked-in interval table
for Unicode 15.1 `L*`, `M*`, `N*`, `P*`, `S*`, and `Zs`. The table is generated
from the official `DerivedGeneralCategory.txt` whose pinned SHA-256 is
`760720ac034f96b630a3055879a744e0907184e8aa811e89ba34583a7a487e85`:

```sh
python3 scripts/generate_failure_unicode_table.py \
  /path/to/15.1.0/DerivedGeneralCategory.txt \
  runtime/src/failure_unicode_15_1.inc --check
```

Runtime formatting never consults a host locale, host Unicode library, LLVM,
or a newer Unicode release.

Public source-defined vector/string bounds require a narrow trusted,
identity-bound origin/check capability supplied by M-FAIL-01. I-CAP-01 has
already established the private identity and visibility boundary that this new
capability must use.
Ordinary GTI wrappers call it with logical size and a fixed public domain;
applications cannot forge its identity or choose an arbitrary category/detail.
This keeps public `vector`/`string` origins in source/library policy without
asking a backend to inspect names or call stacks.

`std::vector` derives its live size directly from its private
`prefix_storage<T>` capability. It has no mirrored element count: construction,
append, insertion, erasure, pop, clear, resize, relocation, movement, iteration,
and indexed access therefore observe one length authority. Capacity remains a
separate wrapper policy value. Public accessors perform the identity-bound
logical vector check; optimized MIR may discharge only the immediately related
private prefix recheck under the exact proof described in
[`optimization.md`](optimization.md).

**Current gap:** The generated C++ support prelude still contains terminal
integrity and low-level capability helpers that call `std::abort()`. These are
representation guards and legacy terminal operations, not an alternate source-
body emitter, and they must not be used to bypass a MIR-defined failure edge or
cleanup schedule. Embedding and foreign-thread callback containment remain
open. Managed-task ordinary and cleanup-double-failure containment are
implemented for exact owned `void()` and owned-result task boundaries.

## Source And Tooling Privacy

`gti_internal` is reserved at the root namespace. Application declarations,
direct references, and namespace-alias targets that enter it are rejected by
semantic diagnostic `GTI-S2058`; analysis recovers without granting the
declaration private authority. Private declarations and aliases are published
only to compiler-trusted source consumers. A nominal declaration, function,
constructor, field, or alias whose exposed type contains a private capability
is also treated as compiler-private, so a trusted library declaration cannot
leak that capability through an otherwise public spelling.

The shared compiler query layer applies the same source-role and semantic-type
checks to completion, hover, definition, and resolved semantic-token
classification. The LSP protocol adapter does not maintain a spelling
blacklist. Trusted standard-library documents may inspect their implementation
surface; application documents see only public source-defined wrappers.

Language-facing ownership and native rules are documented in
[`docs/language/ownership-and-lifetimes.md`](../language/ownership-and-lifetimes.md)
and [`docs/language/native-c-interop.md`](../language/native-c-interop.md).
[ADR 004](../decisions/004-standard-library-runtime-boundary.md) records the
rationale.
