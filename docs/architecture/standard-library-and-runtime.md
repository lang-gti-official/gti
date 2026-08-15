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

## Compiler-Private Capabilities

`gti_internal` declarations represent operations ordinary GTI cannot yet
express safely, including unique ownership and partially initialized storage.
The source graph assigns every unit one explicit role: application, implicit
prelude, or installed standard-library source. Only the prelude and physical
units below the configured standard-library root are compiler-trusted. A
quoted application file, an override-only path beneath that root, or a source
spelling cannot mint that role.

Semantic analysis grants intrinsic behavior by trusted declaration identity in
the implicit prelude. The private `unique_owner<T>`, `storage<T>`, and
`text_view` type declarations likewise receive compiler-capability identities
only when the exact trusted prelude declarations are selected. Aliases preserve
the selected semantic type; reusing a declaration or qualified spelling in
application source creates no capability. HIR/MIR retain selected intrinsic
identity, and the backend queries semantic capability facts rather than
recognizing a source name before choosing its representation.

Capabilities should expose only an irreducible invariant. Public engagement,
size, capacity, allocation policy, or container behavior belongs in ordinary
GTI wrappers.

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

The public managed-thread type will be an ordinary source-defined
`std::jthread` class. The compiler must not recognize that class, its
constructors, `join`, or its destructor by qualified spelling. Its move-only
handle state, automatic-join policy, and eventual stop-token policy belong in
GTI source and use the ordinary class lifecycle rules.

Only the irreducible host boundary is compiler-private. The planned capability
surface consists of an opaque owning native-thread state plus identity-bound
spawn and join operations. A compiler-generated, type-specific task-entry
adapter may erase the exact closure only after semantic analysis has proved a
consumed `void()` callable and transfer-capable captures. The runtime owns the
native thread and fixed worker-failure handoff; it does not own public class
policy or know a GTI standard-library type.

The implemented C-MIR-01 groundwork gives a resolved private capability call
an exact backend-independent synchronization record. HIR and MIR distinguish
thread spawn/join, atomic operation families, and mutex lock/unlock; atomic
records retain legal order dimensions. MIR rejects these records in the
single-threaded execution profile, and optimizer effects preserve every record
as a barrier. No public thread declaration, native task adapter, runtime thread
handle, or backend lowering is implemented yet. Those remain gated on the
owned-task, defined-failure, ordered-execution, runtime, and backend rows rather
than being approximated with C++ `std::thread` behavior.

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
bytes and identity with a null site pointer. The selected
`scalar-failure-callgraph-v1` component creates an exact record on a local MIR
failure edge, forwards it through its hidden ABI, and calls the hosted terminal
primitive after verified cleanup. Other generated compatibility bodies do not
gain record or containment authority from descriptor emission alone. The
public compatibility-only `CppEmitter` API has no MIR snapshot and does not
emit these tables.

The runtime formats and writes ordinary and cleanup-failure reports without
allocation, invokes the optional hosted observer over a protected record copy,
and arbitrates one process-wide terminal winner. Observer re-entry, mutation of
the presented copy, or an escaping C++ exception preserves and reports the
original record before immediate status-70 termination. POSIX report writes
retry `EINTR`, complete partial writes, and contain a broken-pipe `SIGPIPE` in
the reporting thread; Windows uses binary `WriteFile` so the final LF is not
translated. Report-write failure is nonrecursive and does not change terminal
status. The bounded generated hosted adapter now owns containment for the
selected scalar failure component. Future general hosted, task, callback, and
E-EMBED-01 wrappers still own their containment policy, cleanup completion,
context guards, and record return/storage. This boundary does not make the
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

**Current gap:** `scalar-failure-callgraph-v1` is the first generated hosted
consumer, not a general failure cutover. The C++ compatibility emitter still
generates several English-message helpers that call `std::abort()`, while
native expected observers can throw or assert. Those unselected paths do not
complete compiler-managed failure cleanup, pass the fixed record to the
runtime, or produce the specified status/report. They are removed only by
their matching closed M-BACK-02 migrations; they are not alternate runtime
policy. Double-failure cleanup and task/callback/embedding containment remain
open.

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
