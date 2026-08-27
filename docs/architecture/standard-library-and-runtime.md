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
`std::forward_list` empty, front, push, pop, and clear operations.

## Compiler-Private Capabilities

`gti_internal` declarations represent operations ordinary GTI cannot yet
express safely, including unique ownership and partially initialized storage.
The source graph assigns every unit one explicit role: application, implicit
prelude, or installed standard-library source. Only the prelude and physical
units below the configured standard-library root are compiler-trusted. A
quoted application file, an override-only path beneath that root, or a source
spelling cannot mint that role.

Semantic analysis grants intrinsic behavior by exact trusted declaration
identity. The private `unique_owner<T>`, `storage<T>`, `prefix_storage<T>`, and
`text_view` type declarations receive compiler-capability identities only when
the exact trusted prelude declarations are selected. Module-specific
capabilities may instead be bound to one exact installed standard-library unit;
managed threads use this route. Aliases preserve the selected semantic type;
reusing a declaration or qualified spelling in application source creates no
capability. HIR/MIR retain selected intrinsic identity, and the backend queries
semantic capability facts rather than recognizing a source name before choosing
its representation.

Capabilities expose only irreducible representation invariants. Public logical
size, growth, allocation policy, and container behavior belong in ordinary GTI
wrappers. The sparse-storage allocation extent and initialized-slot bitmap are
the exception in kind, not in authority: narrow private observers let the
source-defined `std::storage<T>` wrapper expose checked `capacity()` and
`initialized(index)` without exposing an address, bitmap, raw mutation, or
deallocation operation.

The first public typed-allocation slice is the ordinary source-defined
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

The unique-owner upcast capability consumes one private owner after semantics
has proved an exact public derived-to-base relationship and polymorphic
destruction. The storage shift capabilities move initialized slots through a
verified empty slot. Public `std::upcast_unique`, `vector::insert`, and
`vector::erase` remain ordinary GTI policy; neither public name is recognized
by the compiler or backend.

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
compiler-trusted thread-wrapper body. That exception lets the complete unused
`<std/thread>` declaration inventory exist; it cannot create a task adapter and
does not bypass the public semantic profile check. Optimizer effects preserve
every record as a barrier. The concurrent profile plus target `threads`
capability admits the implemented thread operations. Spawn transfers one exact
owned task to the worker, successful explicit or automatic join establishes
completion-before-return, and no public detach operation exists.

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

Stdout has two deliberately small entries. `gti_rt_write_stdout` accepts one
non-retained counted literal/view, while
`gti_rt_write_stdout_byte(uint8_t)` writes one exact byte from dynamically
owned text. The source library reaches the latter through the explicit
lossless `uint8_t(char)` code-unit extraction. Neither entry knows about
`std::string`, a format grammar, a variadic argument, or a formatter object,
and the scalar entry preserves byte zero rather than relying on C-string
termination.

The hosted runtime serializes each stdout entry with one process-wide mutex.
The bytes of one call do not interleave with another call, but a source-level
operation implemented by several calls may interleave between those calls.
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
