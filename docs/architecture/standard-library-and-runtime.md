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

## Source Policy And Compiler Mechanisms

GTI is an independent language. The use of C++ in the reference backend or
runtime is an implementation fact, not a requirement on source-level library
design. Evaluate each facility's invariant before deciding where it belongs:
ordinary GTI owns expressible policy, compiler support owns language facts, and
native calls provide host services. A previous private implementation is not
evidence that the entire facility must remain private as the language grows.

For memory, distinguish acquiring bytes from starting and ending GTI object
lifetimes. A provider can obtain regions through libc, operating-system APIs or
another allocator. Layout queries and construction/drop operations describe the
selected GTI representation and follow verified lifecycle schedules. Safe owners
retain their resource dependencies regardless of how bytes were acquired.
`malloc`/`free` and aligned `new`/`delete` are distinct allocation families;
replacing a provider must preserve matching release, extent, alignment and
failure contracts. An erased address alone establishes neither initialization
nor checked ownership.

The current process resource calls a C ABI implemented with aligned C++
allocation. That host implementation is replaceable without changing GTI
semantics. Typed region adoption remains private, and its slot bitmap currently
uses the process heap. General public lifecycle facilities and provider-controlled
bookkeeping are explicit extension goals, not claims of current support and not
operations made inherently private by C++ generation.

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

Optional payload constructors execute in ordinary GTI before the completed
value enters inline storage. The private publication capability takes exactly
one element, not arbitrary constructor arguments; semantic checking and MIR
verification reject a hidden source-constructor target. This preserves the
normal constructor failure record, argument ordering, and partial cleanup.

The compiler retains one declarative mapping from public nominal type names to
their canonical optional unit solely for unresolved-type diagnostics and
missing-include fix-its. That catalog grants no semantic behavior and does not
make a declaration visible: applying the edit causes the normal source loader
and semantic declaration identity to resolve the type on the next snapshot.
Unknown application/package names never receive a guessed standard-library
include.

Integer `std::print`/`std::println` overloads, `<std/string>` conversion, and
`<std/format>`'s bounded heterogeneous replacement are a deliberate
boundary example. Digit extraction, built-in argument adapters, brace
validation, exact argument counting, owning-string construction, and format
errors are ordinary GTI source. The frontend's bounded comma-pack fold supplies
only exact ordered calls over a pack; it does not recognize any public
formatting or string name or parse a format string. Counted literal/formatted
writes and scalar dynamic-code-unit writes cross the host boundary. Public
`std::formatter<T>` specializations use the same source-defined formatting
grammar and checked member-signature protocol; compile-time unsupported-type
diagnostics arise from ordinary constraints, not recognition of public names.
The primary formatter's integral-enum default is ordinary source policy over
two trusted classification constraints and one private value-extraction
intrinsic. Semantics proves enum identity and exact underlying integer type;
HIR preserves the selected operation and MIR retains integral-enum declaration
metadata to verify extraction. The production backend emits that verified MIR
operation as a representation cast. No public formatter, concept, enum, or
format-pattern spelling grants compiler behavior. Explicit source
specializations retain their existing canonical selection and contract checks.

`<std/io>` composes source-defined partial reader/writer contracts, memory
transports, exact transfer/line/copy helpers and owned buffering. Mutable spans
borrow initialized prefix storage exclusively; byte buffers are ordinary
vectors. Checked carrier loans permit a buffered adapter to borrow another
carrier while owning a disjoint buffer field. Compiler facts describe places,
permissions and lifetime dependencies, not readers or writers. The only new
host operations are versioned non-retaining partial binary writes for files
and stdout/stderr, reporting the committed prefix. Existing read and flush
entries are reused. SIGPIPE containment checks for an actually pending signal
before consuming it, including hosts where SIGPIPE is ignored.

The panic-adoption layer remains source-defined: `try_format` and
`try_format_to` own recoverable formatting, plain formatting translates their
errors to panic, and `try_print`/`try_println` add stdout submission status in
`print_errc`. Formatted `println` submits the text and newline together. Existing
raw output helpers check their status instead of discarding it. They remain
literal-output operations, independent of optional header visibility.

`<std/panic>` owns the public panic overloads. Its raw wrapper and the formatting
failure-policy helpers share `std/detail/panic.gti`, a non-formatting core over
the identity-bound panic capability; this avoids a format/panic include cycle.
The core is implementation detail, not a second failure protocol. Every wrapper
that owns panic policy is `[[track_caller]]`, so the existing semantic caller
token survives through HIR/MIR; neither backend nor runtime guesses a public
function name. Allocation-free capture does not imply allocation-free formatting,
raw owning-string conversion, or user cleanup. The surface census includes
nested implementation units as well as public units.

`<std/encoding>` is another entirely source-defined boundary. Base64 and
hexadecimal operate over counted string views and construct owning strings;
their alphabets, padding rules, canonical-bit validation, and recoverable
`decode_errc` results are ordinary GTI. No codec name, lookup table, or native
encoding operation is recognized by the compiler or runtime.

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
`stdlib/std/**/*.gti` unit, then classifies each inventoried operation as
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

The fixed-lane arithmetic capability is declared only by the physical trusted
`<std/simd>` unit. Its six selected identities operate on equal-length fixed
arrays with float/double lanes and an equally sized mutable output (bool for
comparison). Semantics validates lane type, extent 1..64, and output mutation,
and records immediate read/read/write borrows. They retain no borrow and return
no pointer or value. These private immediate array borrows do not relax the
public restriction on reference-to-array declarations.

HIR retains the selected intrinsic and parameter types. MIR verifies the exact
three places, matching concrete extents, input/output lane types and access,
void result, absence of failure/escape/lifecycle metadata, and mutation effects.
The C++ backend emits only that verified operation. Public vector/mask classes,
bounds checks, copying, selection, and ordered reduction remain ordinary GTI.
The private native helper uses copied 16-byte chunks, so it requires neither
aligned input nor type-punning; no vector object escapes into the source ABI.
Clang/GCC vector extensions implement the chunk operations, with scalar tails
and a portable scalar fallback for other C++20 compilers. The test-only
`GTI_SIMD_SCALAR_REFERENCE` native macro selects that same fallback for parity.
The preamble planner emits this support only when verified MIR contains one of
the six operations; a dormant library import does not add vector support.

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
to one exact installed standard-library unit: `unique_owner<T>`, `shared_owner<T>`, and
`storage<T>` to `<std/memory>`, `optional_storage<T>` to `<std/optional>`,
managed threads to `<std/thread>`, atomics to `<std/atomic>`, mutexes to
`<std/mutex>`, and defined integer arithmetic to `<std/numeric>`. Iterator and
callable structural constraints are similarly owned by `<std/iterator>` and
`<std/functional>`. Aliases preserve the selected semantic type; reusing a
declaration or qualified spelling in application source creates no capability.
HIR/MIR retain selected intrinsic identity, and the backend queries semantic
capability facts rather than recognizing a source name before choosing its
representation.

`shared_owner<T>` is a nominal move-only strong token, not the legacy implicit
shared-pointer type. Its private operations create an empty token, consume an
existing unique owner, explicitly retain, borrow read-only, observe emptiness
or count, swap, and explicitly consume one token into a public polymorphic-base
view. The public `shared_ptr` policy remains ordinary GTI.
MIR verifies each exact capability/operand/result, its loan anchor, failure
outcome, and invocation handoff. A shared drop recipe contains one unprojected
unique-owner recipe, executed only on final release; passive pointees still
have allocation cleanup. Release is a synchronization and unknown-memory
barrier, including through aggregate and callable effect summaries.

The native control block uses an atomic count shared by typed move-only inert
shells. A verified upcast changes only the consuming shell's view pointer; it
retains the same block, does not change the count, and relies on the existing
dynamic GTI cleanup binding when a base-typed shell performs final release.
Native destruction of an unreleased strong token fails rather than silently
substituting C++ cleanup for MIR. Promotion allocates before detaching the
consumed unique input and owns its rollback on allocation failure. Managed
thread adapters likewise execute the exact verified task/result cleanup
recipe before destroying the native box; active task/result types without
such a recipe are rejected by MIR verification. Native exception unwinding
does not choose GTI cleanup or reference-count ownership policy.
The runtime exposes the pending failure kind only during its task-cleanup
phase. Adapters pass an existing ordinary failure into the recipe as unwinding;
a secondary failure records the emergency and stops further capture/result
cleanup without destroying still-active native shells. This also applies when
cleanup itself has already produced an emergency before returning to the adapter.

Capabilities expose only irreducible representation invariants. Public logical
size, growth, allocation policy, and container behavior belong in ordinary GTI
wrappers. The sparse-storage allocation extent and initialized-slot bitmap are
the exception in kind, not in authority: narrow private observers let the
source-defined `std::storage<T>` wrapper expose checked `capacity()` and
`initialized(index)` without exposing an address, bitmap, raw mutation, or
deallocation operation.

Allocated character `prefix_storage<char>` reserves one additional
representation-private slot and maintains NUL at the logical length. A
zero-capacity empty string uses an immutable process-lifetime NUL sentinel
instead. The hidden slot is not included in the public length or capacity and
does not change generic prefix-storage semantics for other element types. The
trusted
`prefix_storage_c_string` and `prefix_storage_try_c_string` intrinsics reach the
backend only through verified MIR: both attach the storage dependency, while
the checked helper scans the logical prefix and returns null on an embedded
terminator. Public `std::string` methods remain ordinary source wrappers over
those narrow capabilities.

Numeric input in `<std/charconv>` keeps integer grammar, radix selection,
overflow checking, consumed-byte offsets, and output publication in ordinary
GTI. Integer accumulation uses the exact-width checked arithmetic capabilities;
negative accumulation admits the signed minimum without overflowing its
positive magnitude. Binary32 and binary64 conversion use the bounded
`gti_rt_parse_f32_v1`/`gti_rt_parse_f64_v1` C ABI over a non-retained counted
input, offset, format selector, and scalar output slots. Host `from_chars`
provides locale-independent rounding without an intermediate floating width.
The `<std/string>` convenience functions own whitespace/sign/radix-prefix
policy in GTI and return recoverable errors instead of exposing native
exceptions or errno. No public parsing function receives compiler recognition.

Floating output uses the sibling bounded
`gti_rt_format_f32_v1`/`gti_rt_format_f64_v1` ABI. GTI parses replacement
fields, selects argument adapters, enforces the precision bound, and owns the
transactional `try_format`/`try_format_to` error policy. The host operation performs
only locale-independent `to_chars` conversion plus field-local sign,
alternate, width, fill, and alignment composition into caller-owned storage.
It reports invalid requests and insufficient capacity without publishing a
partial size. This boundary avoids native stream state, allocation ownership,
or C++ exceptions becoming part of the public library contract.

The first public typed-allocation slice is the ordinary source-defined
`std::unique_ptr<T>`, `std::make_unique`, `std::upcast_unique`,
`std::allocator`, `std::storage<T>`, `std::alignment`, and `std::alloc_errc`
surface in `<std/memory>`. The compiler recognizes none of those public names.
The public `unique_ptr::swap` delegates to one declaration-bound private
capability operation over two matching mutable owner tokens. MIR verifies both
places and emits native owner swap directly, so exchanging owners cannot copy a
pointee or trigger replacement cleanup.
The stateless allocator calls one recoverable private allocation operation; a
successful storage owner carries allocation extent, alignment, per-slot live
state, reverse cleanup, and release authority together. Allocation failure is
returned as `expected`, while bounds and invalid slot-state operations use the
normal defined-failure route. The public owner rejects borrowed element state
but does not require element movement. `construct(index, value)` requires a
movable element and consumes one completed `T`,
so failure while creating the argument remains under ordinary MIR cleanup.
`emplace<Args...>(index, args...)` also supports construction at the final slot
address: semantics selects the constructor, HIR preserves its concrete
parameters, and MIR schedules non-consuming slot preflight before parameter
handoff. The slot becomes live only after that constructor succeeds; partial
members are rolled back with the original constructor failure record. The
backend implements this verified schedule rather than independently selecting
a native constructor or cleanup policy. Immovable ordinary objects can use this
path; storage handle movement does not relocate elements. Relocation separately
requires the existing element capabilities. ADR 022 owns the public provenance
and failure contract.

The private `storage_try_adopt` capability accepts a writable external region,
count, extent and alignment only inside an explicit unsafe block. Semantics
records `StorageAdoption`; HIR/MIR preserve it, and verification rejects a
missing marker or a forged destination/address/extent contract. The backend
checks numeric extent and actual address alignment, allocates the existing slot
bitmap, and publishes only on success. It never frees the external region.
Source-defined resource leases must retain region release authority until the
ordinary MIR element cleanup and bitmap destruction finish. This is not a
second construction/destruction engine, nor proof of an allocator algorithm.
The narrow `gti_rt_memory_allocate_v1`/`gti_rt_memory_release_v1` C ABI supplies
nonthrowing aligned process allocation for those source-defined policies;
zero bytes allocate nothing and invalid alignment is a recoverable status.

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
source ownership state exactly once. Its payload requires move construction,
not assignment, so construction-only owners can nest. This uses the ordinary
`std::move_constructible` concept over the existing semantic movement fact;
`std::movable` retains its stronger assignment requirement. Copy construction
and both assignment
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

## Package Build-Program Boundary

`<std/build>` is an ordinary source-defined, host-only library surface for
approved package `build.gti` programs. `std::build::context` owns the exact
driver-supplied host/target/toolchain facts, `std::build::command` constructs an
exact argv without shell expansion, and `std::build::outputs` serializes the
versioned native-output description. The compiler recognizes none of these
public names.

The only irreducible host operation is the narrow counted runtime entry
`gti_rt_run_process_v1`. It accepts a NUL-separated argument blob, rejects a
missing/empty executable or malformed encoding, and uses `fork`/`execvp` plus
`waitpid` on supported POSIX hosts. It returns the ordinary child exit status;
Windows fails closed with status 127. Environment selection, working directory,
consumer approval, caching, and output containment remain driver policy rather
than runtime policy. This boundary provides exact execution, not sandboxing.

## Public Child-Process Boundary

`<std/process>` promotes exact synchronous argv execution to ordinary hosted
applications without exposing the build-program policy type. Public
`std::process::command`, `exit_status`, and `errc` are source-defined GTI
values. They own validation, stable error categories, and the distinction
between ordinary exit and signal termination; neither compiler nor backend
recognizes their names.

The versioned `gti_rt_process_run_v1` host entry receives the same counted
NUL-delimited encoding and writes only scalar termination and native-error
facts. On POSIX, a close-on-exec error pipe distinguishes a failure of
`execvp` from a successfully started child that exits with code 127. The
runtime retains no input bytes and owns no public command, string, or status
representation. The older `gti_rt_run_process_v1` entry delegates to this
transport and keeps the build-program normalized-integer behavior.

The bounded public slice waits synchronously and inherits environment, working
directory, and standard streams. Capture, redirection, environment and
directory overrides, asynchronous process ownership, cancellation, timeouts,
and pipelines remain later process-policy work rather than implicit shell
behavior. Windows is rejected by the source unit until its UTF-16 command-line
and handle-inheritance boundary is implemented.

## Clock Boundary

`<std/chrono>` keeps duration and time-point policy in ordinary GTI. Its signed
`nanoseconds`, nominally distinct `steady_time_point` and `system_time_point`,
checked elapsed subtraction, comparison, and Unix decomposition contain no
compiler or backend name recognition. Exact `std::checked_sub` supplies the
recoverable duration-overflow decision.

Passive duration/time-point constructors and pure observers are source-defined
`constexpr` clients of staged canonical MIR. Clock reads and Expected-returning
elapsed subtraction are not const-eligible. The adoption adds no evaluator
opcode, native constexpr authority, or public-name recognition.

The two `gti_rt_chrono_*_now_v1` entries expose only a stable status and one
`int64_t` output. The runtime converts host clock readings into a floor-based
seconds plus normalized nanosecond remainder, proves the joined value fits,
and leaves the output unchanged on failure. The steady epoch remains opaque;
the system result is signed Unix time excluding leap seconds. No native
`std::chrono` type crosses the ABI.

Generic ratio durations, sleep, calendars, formatting, parsing, time zones,
and additional clocks remain outside this first observation slice.

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
The `std::getline` overloads likewise implement delimiter recognition, final
unterminated-line handling, and owned-string assembly in GTI source over the
existing single-byte entries; the runtime has no line or terminal-prompt
policy.

Exact `char`/`uint8_t` conversion bridges the byte representation in trusted
source without making `char` numeric or adding an implicit conversion. Chunk
reads use one fixed internal array and return an owned string; application code
never receives the mutable runtime buffer or native descriptor.

`<std/http>` follows the same public-policy boundary for owned URL construction
and a synchronous, buffered HTTP/HTTPS client. Absolute parsing, component
percent-encoding, URL cloning and rendering, request construction, header
validation, stable error categories, owned response copying, and RAII live in
ordinary GTI. A versioned `gti_rt_http_*_v1` C ABI accepts only counted views,
scalar limits,
stable method ordinals, and opaque integer handles. Its host implementation
dynamically loads libcurl, enforces certificate and hostname verification,
disables redirects, bounds buffered response data, and retains no caller
views. No libcurl type, option, callback, or pointer crosses into public GTI.
The response handle owns bytes that have already been received; it is not a
public streaming or socket abstraction.

`<std/json>` needs no runtime or compiler-private capability. Its strict parser,
owned value tree, exact number lexemes, deterministic serializer, limits,
structured errors, and explicit `codec<T>` customization are all GTI source.
This is an intentional proof that a format library can remain portable policy
once the ordinary string and vector ownership primitives are sufficient.
String-owning errors execute through verified MIR in both native Expected
representations. The parser and builders use ordinary exact-error propagation;
conversion branches that add JSON Pointer context remain explicit source code.
Arena/pool factories use the same propagation rule, while elapsed clock
subtraction deliberately maps arithmetic errors through a typed
`transform_error` function. None changes recoverable errors into panic.

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
create/join, identity, and registered-entry implementation. It is a separate archive member,
so C consumers referencing only the yield ABI do not acquire the registry.
GTI currently emits unused ordinary wrappers conservatively, so importing the
expanded module can link that archive member without starting the service.
`cthread_internal.h` is not
installed. The stable C boundary now exposes create/join plus compiler-generated
begin/end/enter/leave entries. Ordinary `unsafe` GTI wrappers expose
`std::thrd_create` and `std::thrd_join`, with raw-pointer and mutable-reference
output overloads. The frontend recognizes only exact private native
declarations from the trusted `std/cthread` unit. Their concrete call graph
derives wrapper profile requirements without recognizing public names.

`std::thrd_current` is an ordinary GTI wrapper over the scalar native
`gti_rt_cthread_current_v1` entry; `std::thrd_equal` is ordinary GTI integer
comparison. Neither gains a compiler operation or synchronization record.
One atomic, non-wrapping identity allocator serves initial, managed, foreign,
and manual native threads. Per-thread storage keeps an observed identity stable;
manual creation reserves its worker's identity before launch. Relaxed atomic
allocation guarantees uniqueness but publishes no application data. Identity
observation is independent of service begin/end, callback registration, and
the join registry. Exhaustion permanently closes allocation: creation returns
error and observation of a not-yet-identified thread terminates. The runtime
never recycles host thread IDs into public handles.

`std::thrd_sleep` uses the existing ordinary MIR native-call route over
`gti_rt_cthread_sleep_v1`. Public `std::timespec` remains a GTI struct; the C
boundary receives two signed 64-bit scalars and two optional scalar output
pointers, never the struct's layout. Source wrappers validate duration and
snapshot aliased input; both source and runtime publish remaining time only
on interruption. The runtime validates normalized input and host results,
rejects incomplete or identical output pointers, and uses private output slots
until the result is accepted. No new compiler intrinsic, synchronization
record, ownership operation, or AST/HIR emission path is added.

`runtime/src/cthread_sleep.cpp` is a separate archive member, preserving the
yield-only C link boundary and native-symbol test overrides. POSIX calls
`nanosleep` once, maps `EINTR` to interruption, and changes neither signal
handlers nor masks. Windows rounds up to the relative waitable timer's 100ns
units with checked range conversion and uses an alertable wait; an APC/I/O
completion interruption reports a clamped remainder derived from
`GetTickCount64`, at host tick resolution. Neither path retries interruption.
The source-private `cthread_sleep_internal.h` seam accepts a host function per
call for deterministic validation/publication tests, with no global injection
state or installed testing API. Native tests also exercise real host
interruption separately from the deterministic wrapper oracle.

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
foreign-thread registration. Semantic conversion permits the exact concurrent
`int32_t(void*)` worker entry and `void()` once initializer; HIR/MIR retain their distinct registered-entry
policy. Create/join are native calls with exact spawn/join synchronization
records. Hosted MIR begins the service after program initialization and ends
it after normal user-entry cleanup; outstanding manual handles at that terminal
boundary terminate. Existing failure exits keep the original GTI record and
do not attempt ordinary shutdown. Generated callback adapters enter once,
invoke the MIR target, and leave after normal cleanup. Defined worker failures
clean the target and terminate at the original failure firewall rather than
being re-raised at join, unlike managed threads.

The manual C-style mutex surface uses exact private `gti_rt_mtx_*_v1`
declarations from `std/cthread`. Semantics selects their operation and propagates
concurrent-profile requirements through ordinary wrappers; HIR/MIR and the
detached declaration table preserve that selection. Native lock/try/timed calls
carry `MutexLock` and unlock carries `MutexUnlock`, independently verified
against exact native signatures. The existing native-call emitter and optimizer
barriers consume them; no public wrapper names or new C++ body path are used.

`runtime/src/cthread_mutex.cpp` owns a never-reused mutex-handle registry.
Each mutex has a short-held state gate, condition variable, owner identity, and
recursive depth. No state gate is held throughout user critical sections.
Final release wakes a waiter; an acquisition loop tolerates spurious wakeups.
If a woken wait fails before taking a free mutex, it notifies another waiter
before returning error; it cannot consume the only release notification.
Timed acquisition validates and range-checks scalar UTC deadlines before any
ownership change. In-flight calls retain the native object independently of
registry lookup, so retirement cannot destroy a waiter or active call. The
void C destruction boundary terminates on invalid or busy retirement. This
surface has explicit unsafe lifetime obligations, not managed-owner cleanup.
The source-private header exposes allocation/wait-failure and waiter-observation
test seams; neither is installed. The public ABI remains scalar-only.

The same translation unit implements manual condition variables so atomic
enrollment/release can share the private mutex state gate without exposing a
native lock or callback API. A separate never-reused condition registry owns
condition gates and intrusive lists of stack-owned waiters. Enrollment holds
the condition gate while checking and releasing exactly one mutex acquisition.
Signal selects one unnotified waiter; broadcast selects all. No notification
credit survives for future waiters. Host waits atomically release the condition
gate, tolerate spurious wakes, and remove their list entry before reacquiring
the mutex. Both registry references survive through reacquisition, preventing
retirement of a woken but still blocked waiter. Invalid input preserves the
original ownership; recoverable wait errors reacquire, and an unrecoverable
reacquisition error terminates. Private allocation/wait-failure and enrollment
observation seams support deterministic tests and are not installed.

Semantics binds six exact private `gti_rt_cnd_*_v1` declarations. HIR/MIR retain
`ConditionWait` and `ConditionNotify`, with release/reacquisition rather than
notification supplying shared-data ordering. The ordinary native-call backend
and existing effect propagation consume these records; no public wrapper
spelling or backend-inferred synchronization is involved.

`std::once_flag` is ordinary source-defined policy over `std::atomic<int32_t>`:
0 is unclaimed, 1 is initializing, and 2 is complete. Acquire loads and a strong
acquire CAS select the initializer; its normal return precedes a release store
of 2. Other callers yield and retry. These are existing verified MIR atomic
operations, not a backend-recognized public once operation.

The only new host capability is exact synchronous `gti_rt_once_invoke_v1`.
`cthread_state.cpp` keeps a stack of thread-local invocation registrations;
generated thunks enter/leave the matching registration. Initial-thread
ownership, manual-worker attachment, or the managed runtime's private active
scope establishes the allowed execution domain. Nested distinct initializers
restore the outer registration; arbitrary foreign threads cannot claim one.
The installed C ABI contains no once-flag representation or reset operation.
`CThreadCallbackKind` selects `ThreadEntry` versus `OnceInitializer` from
semantic conversion through HIR, MIR, detached rows, and the emitted thunk.
Failure uses the existing terminating firewall and never publishes completion.
While a native callback registration is entered, defined failure bypasses the
managed-worker handoff even when the callback runs on a managed worker. Its
verified local cleanup precedes the original diagnostic and termination rather
than throwing a handoff across the native callback. Leaving a successful
initializer restores ordinary managed-worker failure transport.

## Defined-Failure Boundary

[Execution §4.10](../language/execution.md#410-defined-runtime-failure) assigns
checked-failure meaning to compiler-owned facts and leaves the runtime a narrow
environment role. Semantics/HIR select local origins and propagation, the
failure-metadata builder assigns artifact-local sites, MIR retains their exact
detector mappings, and later MIR slices own propagation and cleanup control
flow. A generated boundary/runtime pair may serialize,
observe, return, or terminate with the completed record, but it may not infer a
category, choose caller cleanup, or use a generated/native source location.

`runtime/include/gti/runtime_failure.h` provides versioned C boundaries. Its
stable code and detail ordinals exactly match the compiler vocabulary. The
frozen version-one record remains 48 bytes and its emergency envelope remains
104 bytes. The version-two record is 568 bytes: the same header fields followed
by a 32-bit message length, 32-bit flags, and 512 owned message bytes. Its
two-record emergency envelope is 1144 bytes. Both are trivially copyable;
neither retains a message pointer. The descriptor protocol is still version
one: an allowed outcome is 8 bytes, a site descriptor is 56 bytes, and an
artifact descriptor is 72 bytes on supported 64-bit runtime targets. Every reserved
field must be zero. A nonzero record identity and one-based site must match the
artifact descriptor and the site's exact allowed `(code, detail)` set. The
all-zero identity with site zero is the sole descriptor-free runtime sentinel
and resolves to `"<runtime>":0@0..0`.

Only `panic` / `explicit_panic` (`GTI-R0017`, detail 37) admits a message.
Generated code prepares its exact record header, then calls
`gti_rt_failure_capture_panic_v2` before starting language cleanup. Capture
copies at most 512 bytes without allocation and does not split a valid UTF-8
scalar at the truncation boundary. Invalid UTF-8 remains byte-exact for later
escaping. The only message flag means truncation; uncaptured bytes are zero.
The report renders a quoted message and an explicit `[truncated]` marker when
needed. Ordinary failure records have zero message length, flags, and bytes.
`gti_rt_failure_validate_v2` checks this complete invariant together with the
existing descriptor/outcome contract, without producing output. The generated
managed-thread join validator reuses it instead of reimplementing the failure
vocabulary or permitting unchecked message lengths.

The runtime stores version-two records in its opaque managed-worker state.
Version-two join and value-join results transport them byte-for-byte, including
both messages in a cleanup emergency. Spawn, task callbacks, and the opaque
state handle retain their version-one signatures. Legacy failures promote to
the new record without changing category or source. Legacy join entry points
still return representable old failures; if a new panic crosses such a native
compatibility boundary, they terminate with its full version-two report rather
than discard the message or return a false success. New generated programs use
the version-two failure and join channels throughout. This compatibility rule
does not establish a mixed-version GTI callable ABI or add source recovery.

A worker cleanup emergency retains the existing join-time propagation policy.
After storing both records, its private native handoff marks an exact
worker-local representation-abandonment window. Only the explicit runtime
emergency-handoff entry point enables that window; generated calls originate
at verified MIR emergency terminal edges. An ordinary pending failure or a
foreign native exception does not enable it. The handoff copies the envelope;
the complete record/descriptor validation still occurs at final reporting,
not before that private transfer. During that transfer, lifetime slots (including
representation-only slots), generated lifecycle wrappers, and private
storage/owner/lock carriers do not invent the GTI cleanup that MIR deliberately
stopped. Native expected, variant, array, closure, and unique-owner shells may
retire representation storage, but their payloads recursively reach those
same guarded lifecycle boundaries. A locked native mutex is carried in raw
lifetime storage so its destructor cannot run on an abandoned lock. Any live
native thread handle reached on that path is quarantined, never detached or
joined as an extra cleanup action. Unpublished allocation resources likewise
remain abandoned until process termination.

The generated adapter catch and defensive runtime entry catch clear that
window before task-object handling. Generated adapter invocation closures are
non-owning references; their catch has no trailing GTI-owned capture teardown.
The task box remains governed by the separate exact cleanup-emergency state,
which prohibits remaining task/capture cleanup. Ordinary failure propagation
still requires every scheduled Drop and retains all lifetime integrity
assertions. The private native transfer is therefore not an alternative
language unwinder or a recoverable panic boundary.

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

Runtime report headings use red ANSI styling when stderr is a supported
terminal, unless `NO_COLOR` is nonempty or `TERM=dumb`. Runtime-only
`GTI_COLOR=auto|always|never` overrides that policy; explicit CLI `run/test
--color` choices reach it through the child environment. Formatting remains
allocation-free, and both ordinary/panic and emergency reports use the same
heading helper. A partially written styled heading makes one best-effort reset
attempt before returning an I/O error. Failure records, source/message
escaping, observer arbitration, language cleanup and terminal status are
unchanged. The CLI never filters executed-program streams.

Source names and quoted panic messages use a strict UTF-8 decoder and a checked-in interval table
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

Sparse and prefix relocation consume semantic `RelocationKind` facts verified
against concrete MIR type tables. Bitwise-eligible passive elements select a
bulk candidate after the same complete capacity/liveness preflight. The C++
support helper additionally requires a trivially copyable, trivially
move-constructible and trivially destructible representation, permitting
C++20 implicit lifetime creation by `memcpy`. All other representations retain
generated elementwise transfer. These native traits only restrict lowering;
they never decide a GTI concept. Source lifetimes end, destination lifetimes
become live, and string terminators are repaired using the existing prefix
policy. Resource owners are never byte-copied. Public `storage.relocate_to`
is an ordinary GTI wrapper over the same verified private operation.

Utility `swap` and `exchange` are ordinary constrained GTI wrappers over
private restoring-transfer declarations in the prelude. Declaration identity,
not a public library spelling, selects the capability. The backend helper
performs only MIR-proven failure-free structural movement and ends inactive
native shells; it never calls replacement assignment or destroys a live GTI
resource. Existing MIR call-input staging and result slots own preparation,
publication, and cleanup on both ordinary and failure exits.

`array::swap` uses whole-array structural transfer. Array/vector
`swap_elements` perform public bounds checks then invoke the sealed checked
indexed transfer; its own preflight remains mandatory. Reverse and heapsort
are ordinary GTI over these APIs, with read-only comparison scopes ending
before each mutation. No public container or algorithm name grants compiler
authority, and neither the library nor backend adds a copy/clone pivot.

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
