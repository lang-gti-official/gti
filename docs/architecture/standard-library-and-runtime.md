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

## Module-only Phase 1 Status

This unreleased branch converts the standard library to explicit source module
interfaces. `import std.name;` resolves through `stdlib/module-map.json`;
`import std;` selects the map’s portable aggregate. The former `stdlib/modules`
wrappers and legacy republication are removed. Keeping canonical source paths
preserves trusted declaration identities rather than granting capabilities
through a public module name. The implicit prelude remains the language-wide
root and imports `std.meta` explicitly.

The public map excludes internal iterator and panic interfaces. Library modules
use same-owner quoted imports for those dependencies. Experimental optional has
the explicit public name `std.experimental.optional`. Internal helper tests use
disposable, explicitly registered test maps; production never exposes helpers
solely to make a test compile. Public/detail declaration exports and full
production acceptance remain tracked in the active phase 2 campaign.

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

The process resource now validates alignment and native-size limits, handles
zero-size requests, selects its target provider and maps failures in GTI source.
POSIX uses `posix_memalign`/`free`; Windows uses the matching CRT
`_aligned_malloc`/`_aligned_free` family. GTI can now express native `c_size_t`
and POSIX `void**`. The narrow, uniquely named v1 forwarding ABI remains useful
because C symbols may be declared only once per GTI program: private provider
declarations must coexist with applications' own `posix_memalign` and `free`
bindings. Windows CRT symbols also have reserved leading-underscore spellings.
Native facts (size limits, POSIX constants, pointer alignment) remain small
host observations. The adapters contain no GTI allocation policy. Sparse typed
owners now keep their slot state in ordinary GTI and lease its bytes from the same provider as
their object region. Public unsafe manual-lifetime operations supply typed
construction, destruction and relocation; raw validity remains the programmer's
obligation. The specialized sparse capability type, intrinsic family, adoption
marker, native bitmap and backend storage helpers are retired. This implementation
change is verified separately from the remaining allocation families below.

### Written expression metadata

The `source_expression_text` declaration in the trusted source-location unit
is an identity-bound compiler capability, unavailable to application source.
Its operand is checked through the same isolated request-session fork as typed
macro selection. The caller receives no speculative moves, copies, loans,
writes or execution effects. Semantics reads only the parser-retained complete
written origin against its owning source buffer, then records a string constant.
Missing or substituted compound origins fail closed with `GTI-S2019`; no AST
printer supplies a guessed replacement. Full argument forwarding retains the
original caller's origin, and defaults retain their declaration's source.
An explicit fallback overload binds a second exact private declaration to the
same capability. It validates fallback text with the existing constant service
in a separate isolated probe, requiring a literal-backed string-view constant
even when exact written text is available. A missing origin selects only that
explicit constant; the strict form still rejects missing provenance. Neither
operand publishes runtime ownership, loan or execution effects.

`std::expression_text` is an ordinary value-producing typed macro over that
capability. HIR normalizes its checked capability call to a literal before
lowering any operand; MIR and production emission see only the existing
literal-backed string-view representation. No runtime intrinsic, backend source
inspection, new constexpr execution rule or allocation is involved. The
successful result has static lifetime. Assertion macros use this public API in
their default failure messages; explicit messages retain their source policy.
Their explicit `assertion condition` fallback keeps synthesized conditions in
application-defined macros usable without inventing a source spelling.

### Allocation inventory and remaining boundaries

Inbound C-string import is source-defined scanning, byte conversion, terminated
prefix construction and failure policy in `string.gti`. `c_string_bytes` uses
one representation-only native bridge, `gti_rt_c_string_bytes_v1`; the opaque
boundary type otherwise exposes no raw byte address. That bridge performs no
read, allocation, lifetime extension or error mapping. Ordinary constructor
caller obligations are semantic facts retained through HIR, MIR and detached
lowering, independent of the public string spelling. The fallible factory
allocates through the same source process lease with an `expected` result.

The S-ALLOC-07 migration covers `std::allocator::try_storage`, sparse
`std::storage`, arenas/pools backed by that storage, and `std::process_resource`
used by resource-backed storage/containers. Both object and live-state regions
use the selected source provider. Zero capacity allocates neither; failure to
obtain metadata releases the object region before returning an error.

| Allocation family | Current owner and rationale |
| --- | --- |
| Sparse object bytes and per-slot metadata | Source `try_allocate_sparse`, leases and process-resource policy in the private `stdlib/std/detail/memory_storage.gti` interface; POSIX/CRT adapters only acquire or release bytes. The old `gti_rt_memory_allocate_v1`/`release_v1`, sparse C++ allocation/bitmap helpers and sparse capability operations have no remaining consumers and are removed. |
| Prefix storage used by strings/vectors and related clients | Source raw-buffer/prefix state and matching process/resource leases in `std.memory`. Terminated string policy adds one initialized NUL slot. S-ALLOC-09 and its performance/regression acceptance are integrated in 0.397.0; the native engine, prelude declarations, semantic type/intrinsic family, construction-preflight instruction, representation and private optimizer facts are retired. General object-lifetime and checked-loan mechanisms remain. |
| Unique ownership and generated allocation resources | `mir_object_allocation_v1` and scalar unique allocation in `cpp_failure_support_writer.cpp` still acquire native storage; verified MIR owns construction, publication, failure and cleanup. Legacy private shared ownership additionally uses its native control block; public shared ownership now allocates its source aggregate through process leases. These are remaining migration clients, not reasons to put sparse allocation policy back in the backend. |
| Native service implementation state | Runtime file/process/HTTP/thread/synchronization and conversion implementations use native containers or host handles internally. Their buffer/handle lifetimes belong to their service boundaries; they do not supply sparse-storage metadata. |
| Compiler and tool memory | C++ containers in compiler, driver, LSP and tooling are host implementation state, outside GTI-program allocation policy. |

No surviving adapter selects a GTI constructor, owns a slot bitmap, grows a
container or establishes a checked borrow. Retaining the fixed-width v1 ABI
does not imply `uint64_t` is C's `size_t`: source checks the native limit before
the adapter converts to the actual C type. The private forwarding symbols also
provide a deterministic test seam for exhaustion, rollback and matching release.

## Operation Ownership Ledger

Initial evidence snapshot: 2026-10-08, reviewed against `14caa023`; reconciled
against integrated 0.399.1 on 2026-10-11. Prefix and terminated-string rows
describe delivered S-ALLOC-09 source policy, not an unmerged branch. This ledger
classifies current ownership and remaining boundaries; it does not approve or
schedule migrations. The [implementation sequence](../plans/implementation-sequence.md)
remains the sole work queue. Source and owning tests take precedence when an
entry becomes stale.

### How To Read The Landscape

The useful question is **which operation owns which invariant**, not whether an
entire library is "compiler magic." One function can combine ordinary source
policy, compiler-checked lifecycle operations and a native service.

| Classification | Meaning |
| --- | --- |
| Source, implemented | Policy already executes in ordinary GTI, possibly using general language primitives or native calls. |
| Source candidate | The policy is expressible in principle. Current implementation still delegates it, or general prerequisites remain. This is not a claim that every existing supported type can migrate today. |
| Compiler fact/mechanism | GTI must define, check or preserve this fact. It can be exposed through a general source-facing operation; a particular private function or wrapper need not remain forever. |
| Native service | Interaction with the operating system, hardware or an external library. This is not inherently a compiler intrinsic. Source wrappers can choose policy around it. |
| Native algorithm by choice | An algorithm currently delegated to tested host code. It is not inherently impossible in GTI; replacing it requires a correctness/performance case. |

All GTI source is compiled, including "source-defined" operations. That term
means the compiler does not recognize the library's nominal name to invent its
policy. Also, an ordinary class inside `gti_internal` is not automatically a
compiler capability: exact trusted declaration identity determines actual
capabilities. `sparse_slots<T>` and the source process resource illustrate this
distinction.

### A. Policy Already Implemented In Ordinary GTI

These rows identify the source-owned part, not claim complete independence from
all compiler/runtime support.

| ID | Operation | Source owns | Lower boundary retained |
| --- | --- | --- | --- |
| S01 | Validate an allocation request | Power-of-two alignment, overflow/native-size checks, zero-size handling and error translation. | Target size/alignment facts; actual byte acquisition. |
| S02 | Select and pair process providers | POSIX versus CRT policy; matching acquisition/release family and descriptor publication. | Small native forwarding adapters and target configuration. |
| S03 | Lease and release a resource region | Original descriptor/resource retention, move-only release authority and cleanup order. | Checked provider borrow and ordinary generated movement/drop. |
| S04 | Acquire sparse objects plus metadata | Two same-provider regions, one live byte per slot, failure rollback and zero-capacity policy. | Native byte provider; concrete object layout. |
| S05 | Track live sparse slots | Capacity, live flags, bounds/vacancy/liveness predicates and publication only after construction. | General typed construction and checked reference formation. |
| S06 | Construct at a selected slot | Select address, validate vacancy, pass arguments and mark live after success. | Compiler-selected constructor, exact argument staging and partial-object cleanup. |
| S07 | Drain sparse objects | Reverse traversal, retire each live flag before destruction, resume remaining cleanup after a first failure. | Execute the selected destructor/field cleanup; secondary-failure machinery. |
| S08 | Relocate and shift sparse elements | Validate source/destination ranges, loop order and update live state. | General typed relocation eligibility and transfer execution. |
| S09 | Arena/pool allocation policy | Cursor/free-slot choice, reuse, capacity and public access rules. | Source sparse storage and owner-anchored checked references. |
| S10 | Resource-vector growth | Capacity choice, reserve, recoverable allocation errors, append/pop/clear and iteration. | Source resource storage, relocation and checked borrows. |
| S11 | Map/hash-map algorithms | Tree/bucket organization, hash/comparison protocol, lookup and insertion policy. | Source vector storage and the still compiler-backed unique-owner foundation. |
| S12 | List/deque/forward-list algorithms | Links/indices, ordering, insertion/removal and traversal. | Currently use unique owners and, for some containers, vectors. |
| S13 | Callable-wrapper policy | Source model objects, public invocation/clone/reset policy, inline-versus-boxed choice and type-erased interface composition. | General private polymorphic storage, unique allocation, ordinary dispatch and compiler callable/ownership facts. |
| S14 | Format grammar and customization | Replacement parsing, argument counting, formatter protocol, formatting errors and integral-enum default policy. | Primitive enum classification/extraction; owning string; some native float conversion. |
| S15 | Integer text conversion | Digits, radix, sign, consumed offset, overflow policy and output construction. | Exact-width arithmetic operations and string storage. |
| S16 | Base64/hex and JSON grammar | Encoding alphabets/validation and JSON parsing, traversal and serialization policy. | Scalar/text operations and the containers used to hold output/data. |
| S17 | Buffered I/O policy | Transfer loops, line reading, buffering and reader/writer composition. | Native partial I/O; checked storage/adapter loans. |
| S18 | Panic adoption policy | Message formatting and deciding when a recoverable library error becomes a panic. | Terminal failure, caller provenance and verified cleanup. |
| S19 | Resource unsafe-binding wrapper | Signature contract, one-time binding API and retained provider reference. | Compiler enforces declared borrow lifetime; caller proves actual raw-region validity. |
| S20 | Raw-to-checked reference wrapper | `assume_ref`/`assume_mut` body and owner-witness API. | General `borrowed_from` checking and unsafe dereference; caller proves initialization/alignment/aliasing. |
| S21 | Synchronization policy above primitives | Public order tags and source algorithms such as the once-initialization CAS state machine. | Atomic synchronization semantics and native thread services. |
| S22 (was M01) | Prefix allocation, length and live-prefix bookkeeping | Source `raw_buffer<T, Lease>` owns a lease, element pointer and capacity; `prefix_slots<T, Lease>` adds the sole live count, with no per-slot metadata. | General typed construction, destruction and relocation; process and resource byte providers. |
| S23 (was M02) | Prefix append, pop, insertion, erasure and reserve | Source prefix-owner policy with one check per safe operation and bulk byte relocation of bitwise-relocatable elements. | Final-address emplacement, construction rollback and exactly-once cleanup through general object operations. |
| S24 (was M04) | Views and iterators over prefix storage | Span, vector and string wrappers borrow source prefix owners; `unsafe` unchecked accessors on vector, string, span and fixed array. | General carrier and checked-loan contracts; raw text conversion through an exact checked owner witness. |
| S25 (was M03) | Character trailing-NUL policy | Source `terminated_prefix_owner`, allocation-free empty sentinel, hidden initialized terminator and embedded-NUL scan. | General text/C-string carrier conversion through a checked owner witness; this is not a private string-storage engine. |
| S26 (was M09) | Expected active-alternative storage and ordinary helpers |
Source `expected<T, E>`, `expected<void, E>` and `expected<T&, E>` prelude
classes keep their alternatives in a payload enum; observers, combinators and
`unexpected` are ordinary members and functions. The compiler's built-in
expected type and operations are removed. | `?` through `std::tryable`,
payload-enum lifecycle, reference payload loans and the declaration-bound
inactive-access failure intrinsics. |

Evidence: [memory](../../stdlib/std/memory.gti),
[resource](../../stdlib/std/resource.gti),
[resource vector](../../stdlib/std/resource_vector.gti),
[map](../../stdlib/std/map.gti),
[unordered map](../../stdlib/std/unordered_map.gti),
[functional](../../stdlib/std/functional.gti),
[format](../../stdlib/std/format.gti),
[charconv](../../stdlib/std/charconv.gti),
[cthread](../../stdlib/std/cthread.gti).
Existing acceptance includes the
[source-allocation completion evidence](verification.md#completion-acceptance-2026-10-08)
and [object-lifetime tests](../../tests/object_lifetime_tests.cpp).
These links identify existing acceptance evidence; this documentation ledger
does not claim a new test run.

### B. Source-Policy Migrations And Remaining Prerequisites

These entries distinguish migration work from complete acceptance. S-ALLOC-09
completed the former M01–M04 rows, now S22–S25 above, and the source-defined
`expected` completed the former M09 row, now S26. The remaining
rows are candidates, not implicitly approved or completed migrations.
"Prerequisite" means a fact that must be established before retiring a path,
not permission to weaken existing safety or reject previously supported users.

| ID | Operation to move | Current owner | Prerequisite/acceptance boundary |
| --- | --- | --- | --- |
| M05 | Non-polymorphic unique allocation and release | `allocate_unique_owner`, private `unique_owner<T>` and generated allocation support. | Source lease/engagement state, constructor rollback, exactly-once final drop, safe borrow anchoring, movement and replacement. A sparse storage field is not automatically API/performance-equivalent. |
| M06 | Polymorphic unique-owner conversion and destruction | Private unique upcast and dynamic cleanup bindings. | Retain original allocation/release authority while changing the view; execute the actual dynamic object's cleanup. Ordinary base-pointer destruction is not a substitute. |
| M07 | Optional engagement, access and reset | Private `optional_storage<T>`. | Source inline storage plus engagement state, inactive-access failures and checked loans. Public `uninitialized<T>` is fixed-address for non-bitwise-relocatable payloads; preserving movable owning optional needs a general carrier/lifecycle solution. A flag alone is insufficient. |
| M08 | Shared control-block allocation, retain/release and observation | Source `shared_ptr<T>` and source atomic control block (LLVM). | General layout/lifetime, allocation, atomics, function pointers and capability predicates remain compiler-supported. Weak ownership is additional work, not unlocked automatically. |
| M10 | Container guard predicates and formatting of failures | Source computes many predicates but calls private `require_*`/bounds helpers. | Source can own predicates/message policy. Preserve stable failure categories, location, cleanup and optimizer proof identity through a general failure interface; replacing everything with generic panic is not equivalent. |

S22–S24 use the representation accepted in
[ADR 022](../decisions/022-safe-typed-allocation.md#revision-initialized-prefix-sequences-over-a-raw-buffer-2026-10-09)
and completed by
[`S-ALLOC-09`](../plans/implementation-sequence.md#s-alloc-09-raw-buffer-vector-and-string):
- a source-defined raw buffer;
- an initialized-prefix length with no per-slot metadata;
- one check per safe operation;
- `unsafe` unchecked accessors.

This follows Rust's `RawVec`/`Vec` layering. The private `prefix_storage`
native, prelude, IR, representation and optimizer family is removed. The
checked vector loop, game update and string workloads match or beat the
private path with smaller binaries; the measurements are in the
[verification record](verification.md).

Evidence: [vector](../../stdlib/std/vector.gti),
[string](../../stdlib/std/string.gti),
[span](../../stdlib/std/span.gti),
[optional](../../stdlib/std/optional.gti),
[owner declarations](../../stdlib/std/memory.gti),
[expected contract](../language/expected.md),
[private type/operation identities](../../include/gti/semantic_model.h),
[backend storage support](../../src/compiler/cpp_support_preamble.cpp)
and [allocation/failure support](../../src/compiler/cpp_failure_support_writer.cpp).

### C. Facts And Execution Mechanisms GTI Must Own

These responsibilities cannot be supplied solely by an ordinary library while
still being GTI language guarantees. They need not all remain private nominal
capability types: a general keyword, builtin, attribute or narrowly bound API
can expose the mechanism to any suitable user-defined library. Backend/runtime
execution implements the fact; it must not rediscover semantics from C++.

| ID | Compiler-owned operation/fact | Why source alone cannot establish it | Source policy allowed above it |
| --- | --- | --- | --- |
| C01 | Concrete layout, alignment and ABI classification | Depends on the selected GTI type/target representation, including hidden lifecycle state. | Request layout; choose extent/alignment and allocator strategy. |
| C02 | Uninitialized typed representation | Must provide size/alignment without constructing or reading a live payload. | Choose engagement/live-state bookkeeping; no mandatory hidden bitmap. |
| C03 | Constructor selection and final-address execution | Overload/type rules, exact argument order and completion determine which object becomes live. | Choose destination and preconditions, then call `construct_at`. |
| C04 | Destructor/field cleanup execution | Selected type, dynamic identity where applicable, reverse subobject order and failure obligations are language facts. | Choose when to retire a manual object and how to traverse a collection. |
| C05 | Generated move/relocation and structural eligibility | Must transfer owning representation without duplication or invalid reads, preserving all cleanup obligations. | Choose relocation order and new storage; use `relocate_at` only for admitted types. |
| C06 | Partial-construction and failure cleanup | The compiler knows which prepared arguments, fields and temporaries actually completed. | Allocation rollback and source cleanup guards compose with that schedule. |
| C07 | Borrow origin, permissions, escape and loan endpoints | A safe wrapper cannot override lifetime/alias rules or create proof by returning an address. | Declare `borrowed_from`, select owner witnesses, expose checked reference APIs. |
| C08 | Unsafe operation checking and known safety facts | Unsafe authorizes unproven obligations; it does not erase provable lifetime/mutability/type violations. | Provider authors prove raw validity, alignment, exclusivity and matching release. The compiler need not dynamically prove arbitrary raw memory valid. |
| C09 | Type/lifecycle classification | Exact type identity and structural copy/move/transfer/share/layout properties require semantic knowledge. | Compose source concepts and select algorithms using general traits. |
| C10 | `?` and return/control-flow transfer | Propagation changes enclosing control flow and must preserve cleanup and borrowed-result identity. | Error mapping and combinator policy where source callable support admits it. |
| C11 | Primitive integer/float semantics | Width, overflow, rounding and conversions are defined by GTI, not accidental host behavior. | Checked/saturating/wrapping algorithms may compose primitives; current helper identities are not necessarily permanent. |
| C12 | Atomicity and synchronization effects | Ordinary reads/writes cannot manufacture atomicity; optimizer/MIR must preserve memory-order and race facts even if execution uses a native ABI. | Order tags, CAS loops, reference-count algorithms and synchronization wrappers. |
| C13 | Polymorphic dispatch and dynamic cleanup identity | Changing a public view must not lose the actual concrete object's lifecycle/representation identity. | Interface models, owner conversions and public APIs. |
| C14 | Terminal defined-failure/panic integration and caller provenance | Cleanup, original/secondary failure state and nonreturning control flow must remain coherent across all bodies. | Choose panic versus recoverable error, format messages and render reports where interfaces permit. |
| C15 | Compile-time eligibility and execution staging | Required constants affect types/branches before complete program lowering; operations and budgets need compiler-owned evaluation rules. | Write eligible source algorithms; generic packs remain outside the current constexpr subset. |
| C16 | Native call/callback marshaling | Exact ABI, linkage, representations and ownership handoff must survive lowering. | Declare bindings and choose adapter/wrapper policy. |

Evidence: [raw lifetime operations](../language/raw-pointers.md),
[ownership/lifetimes](../language/ownership-and-lifetimes.md),
[execution](../language/execution.md),
[MIR contract](mir.md),
[native interoperation](../language/native-c-interop.md),
[atomic source boundary](../../stdlib/std/atomic.gti)
and [constant execution](../../src/compiler/mir_constant_evaluator.cpp).

### D. Native Services, Not Inherently Compiler Intrinsics

Some external boundary remains necessary for these effects. Its **present native
wrapper or runtime implementation is not necessarily permanent**: GTI could
implement more adapter logic through ordinary native bindings. That migration
must preserve platform and service contracts rather than remove them.

| ID | Operation | External boundary | Source-owned part / remaining concern |
| --- | --- | --- | --- |
| H01 | Acquire/release process memory | libc/CRT/OS byte provider. | Validation, failure mapping and matching leases are already source-defined. Provider cannot prove GTI object validity. |
| H02 | Files, terminals and filesystem effects | Host handles and I/O services. | Buffering, exact-transfer loops, path/API policy and handle ownership can be source; OS access cannot. |
| H03 | Spawn/wait child processes | OS process APIs. | Argument validation and public result/error policy can be source; platform launch/wait and pipe handling remain external. |
| H04 | Observe clocks and sleep | Host clock/scheduler. | Duration arithmetic, conversions and deadline policy can be source. |
| H05 | Network transport/TLS | OS sockets and, currently, external protocol/security implementation. | Request construction and parsing are algorithms; host transport remains external. TLS is not inherently a compiler operation or an impossible source algorithm. |
| H06 | Create/join/wake threads and block on synchronization | Host threading/wait services. | Source handle policy and state machines; managed task/result transfer still needs language lifecycle/thread-capability facts. |
| H07 | Native platform facts | Native size limits, pointer alignment and platform constants. | Observe through small adapters or general target facilities; do not hard-code host assumptions into source policy. |
| H08 | Observe process environment | Native lookup and an immutable, uniquely owned byte snapshot. | Source validates names, distinguishes absent/empty values, maps errors and copies into an independent GTI string; native mutation must exclude concurrent access. |

#### Algorithms Delegated By Choice, Not Necessity

| ID | Current native algorithm | Classification and adoption rule |
| --- | --- | --- |
| A01 | Binary float parsing/formatting | Native algorithm by choice in charconv/format. Source implementation is possible, but exact rounding, boundaries, locale independence and performance must justify replacing tested conversion code. |
| A02 | Transcendental/basic math library calls | Native algorithm by choice in cmath. Correct source algorithms are possible; allocation support itself adds no correctness/performance reason to replace them. |
| A03 | SIMD lane execution | Private accelerated path with a scalar fallback; wrappers/reductions are source. Scalar semantics can be expressed in source, while explicit target acceleration and effects need a general compiler/native boundary. No particular six-function private family is inherently permanent. |

Evidence: [runtime architecture](standard-library-and-runtime.md),
[process](../../stdlib/std/process.gti),
[chrono](../../stdlib/std/chrono.gti),
[cthread](../../stdlib/std/cthread.gti),
[charconv](../../stdlib/std/charconv.gti),
[cmath](../../stdlib/std/cmath.gti)
and [SIMD](../../stdlib/std/simd.gti).

### Reading A Migration Correctly

For example, migrating vector means source owns allocation, prefix state,
growth and traversal. It does **not** mean source recreates the compiler's type
system or writes its own constructor/destructor executor. The desired split is:

```text
source policy chooses when and where
    -> general GTI operations preserve type/lifetime/borrow facts
    -> native provider supplies/reclaims bytes where required
```

Before retiring any existing private path, establish:

1. **Semantic parity:** same accepted types, exact call ordering, borrow origins,
   movement/drop behavior, invalidation and defined failures.
2. **Failure parity:** failed allocation/construction/destruction leaves exactly
   the documented objects and release obligations; no duplicated cleanup.
3. **Representation parity:** immovable, owning, polymorphic and borrowed shapes
   are assessed explicitly. Record structural prerequisites rather than nominal
   exceptions or silently narrowing the API.
4. **General evidence:** an unrelated user-defined owner/container exercises the
   same primitives; compiler recognition of its name is unnecessary.
5. **Removal evidence:** eliminate subsumed operations and representations, but
   retain distinct language mechanisms and honest native boundaries.
6. **Performance evidence:** measure allocations, metadata, checks and throughput.
   Source-defined is an authority improvement, not an automatic speedup. The
   earlier S-ALLOC-07 acceptance records a material sparse-allocation regression;
   later S-ALLOC-09 prefix/string acceptance has its own paired measurements.
   Neither record establishes parity for every allocation family.

The main takeaway: **most library state and algorithms belong in source; layout,
typed lifecycle, ownership/borrow checking and primitive execution semantics
remain GTI's responsibility. OS access is a third category, not compiler magic.**

## Public Library

`stdlib/prelude.gti` is implicitly available. Optional source units live below
`stdlib/std/` and are loaded through `std.name`. Public classes and functions
under `std` own API policy, logical state, error handling, and RAII. The
compiler must not recognize public wrapper names such as `std::vector` or
`std::make_unique` as shortcuts for container or ownership behavior.

The implicit prelude is deliberately limited to language-wide vocabulary,
shared compiler-capability declarations, source concepts over general
`std::meta` type facts, `std::move`, and the minimal hosted output surface.
The reflection vocabulary bootstraps those concepts; higher-level module-owned
policy is not ambient: the `std::unique_ptr`
family and its private owner/storage capabilities are declared by
`std.memory`, checked one-slot optional storage is declared by
`std.optional`, the private file-descriptor bridge is declared by
`std.cstdio`, and defined integer arithmetic intrinsics are declared by
`std.numeric`. Atomic, iterator, and callable compiler constraints likewise
live with their public module. Optional units do not re-export their own
dependencies, so application source names the canonical unit for every API it
uses directly.

Optional payload constructors execute in ordinary GTI before the completed
value enters inline storage. The private publication capability takes exactly
one element, not arbitrary constructor arguments; semantic checking and MIR
verification reject a hidden source-constructor target. This preserves the
normal constructor failure record, argument ordering, and partial cleanup.

Missing-import fixes use compiler-owned declaration discovery, not a
hardcoded public-type-to-module catalog. Structured unresolved lookup requests
search the selected project, declared direct package roots and public standard
units. The canonical parser filters actual declarations, then speculative
frontend analysis must resolve the occurrence to that exact source declaration
without introducing an error. Dirty/new/deleted inputs invalidate discovery;
private declarations, non-interface sources and incompatible candidates fail
closed. Candidate edits use standard/package module names or same-package
quoted imports and must pass ordinary export visibility checks. This supplies
advice without granting semantic behavior or implementing a second declaration
parser; the edited file still uses ordinary source loading and exact identity.

Integer `std::print`/`std::println` overloads, `std.string` conversion, and
`std.format`'s bounded heterogeneous replacement are a deliberate
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
general reflection predicates and one private value-extraction intrinsic.
Semantics proves enum identity and exact underlying integer type;
HIR preserves the selected operation and MIR retains integral-enum declaration
metadata to verify extraction. The production backend emits that verified MIR
operation as a representation cast. No public formatter, concept, enum, or
format-pattern spelling grants compiler behavior. Explicit source
specializations retain their existing canonical selection and contract checks.

Formatting allocates once per result in the common case:
- `try_format` reserves the pattern length plus 16 bytes per argument.
- Every later append goes through `detail::reserve_additional`, which grows
  capacity at least geometrically. `string::append` itself reserves the exact
  new size.
- Integer formatting keeps its sign and base prefix in a three-character
  array, and its digits in the existing reversed digit buffer. It writes fill,
  prefix, zero padding and digits straight into the output.
- Text and `char` values are padded in place, with no temporary string.

Results, spec validation and errors are unchanged. `format_output_matrix_runtime`
compares a broad spec matrix with the output recorded before this layout, and
it passes at O0 and O3. A `format("{}:{}", i, j)` result previously cost about
five allocations: a digits string per integer, an exact-size output regrowth
per append, and a regrowth for the literal. That drove a 2-5x gap to C++ in
formatting-heavy code; see
[performance-tooling.md](../plans/performance-tooling.md#allocation-lean-formatting-2026-10-09).

`std.io` composes source-defined partial reader/writer contracts, memory
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
literal-output operations, independent of optional module visibility.

`std.panic` owns the public panic overloads. Its raw wrapper and the formatting
failure-policy helpers share `std/detail/panic.gti`, a non-formatting core over
the identity-bound panic capability; this avoids a format/panic import cycle.
The core is implementation detail, not a second failure protocol. Every wrapper
that owns panic policy is `[[track_caller]]`, so the existing semantic caller
token survives through HIR/MIR; neither backend nor runtime guesses a public
function name. Allocation-free capture does not imply allocation-free formatting,
raw owning-string conversion, or user cleanup. The surface census includes
nested implementation units as well as public units.

`std.encoding` is another entirely source-defined boundary. Base64 and
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

Ordered-query composition remains ordinary source policy in `std.algorithm`:
`partition_point` and comparator bounds/binary search compose the existing
forward-iterator, sentinel, exact-element and repeatable-callable constraints.
They carry the original first cursor's checked owner dependency and retain one
callback through the query, including failure cleanup. They introduce no
intrinsic, runtime service or public-name compiler recognition; application
records without native ordering and independent owner-tied cursors use the
same implementations.

## Compiler-Private Capabilities

The fixed-lane arithmetic capability is declared only by the physical trusted
`std.simd` unit. Its six selected identities operate on equal-length fixed
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

`gti_internal` capability declarations represent operations ordinary GTI cannot
establish alone, including selected unique/inline-owner representations, raw
uninitialized representation and general typed object-lifetime facts. Sparse
and prefix engagement bookkeeping are no longer private capabilities.
The source graph assigns every unit one explicit role: application, implicit
prelude, or installed standard-library source. Only the prelude and physical
units below the configured standard-library root are compiler-trusted. A
quoted application file, an override-only path beneath that root, or a source
spelling cannot mint that role.

Semantic analysis grants intrinsic behavior by exact trusted declaration
identity. The shared private `text_view` and `c_string`
type declarations receive compiler-capability identities only when the exact
trusted prelude declarations are selected. Module-specific capabilities bind
to one exact installed standard-library unit: `unique_owner<T>`, `shared_owner<T>`,
`uninitialized_storage<T>` and general object-lifetime operations to `std.memory`,
`optional_storage<T>` to `std.optional`,
managed threads to `std.thread`, atomics to `std.atomic`, mutexes to
`std.mutex`, and defined integer arithmetic to `std.numeric`. Iterator and
callable structural constraints are similarly owned by `std.iterator` and
`std.functional`; function-pointer primitives bind to the shared leaf
`<std/detail/function_pointer>`. Aliases preserve the selected semantic type; reusing a
declaration or qualified spelling in application source creates no capability.
HIR/MIR retain selected intrinsic identity, and the backend queries semantic
capability facts rather than recognizing a source name before choosing its
representation.

The retained legacy private `shared_owner<T>` is a nominal move-only strong token, not the legacy implicit
shared-pointer type. Its private operations create an empty token, consume an
existing unique owner, explicitly retain, borrow read-only, observe emptiness
or count, swap, and explicitly consume one token into a public polymorphic-base
view. Public `shared_ptr` no longer instantiates it: its source handle holds a
control pointer and read-only view pointer, and its source control holds an
atomic count, erased allocation address and exact-type destruction thunk. A
source aggregate co-allocates this control and its payload field; generated
field lifetime and a source process lease own rollback and final cleanup. The
new wrapping atomic decrement is LLVM-only, so C++ rejects executable shared
ownership with an explicit unsupported-operation diagnostic. The following
private-token details describe the retained legacy mechanism.
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

The source `raw_buffer<T, Lease>` owns one byte-region lease, element pointer
and capacity, with no knowledge of initialized objects. `prefix_slots<T, Lease>`
contains that buffer and one live count. Its destructor drains the count before
ordinary field cleanup releases the buffer's lease. Process and checked resource
owners share this policy; resource growth exchanges allocation descriptors only
between leases retaining the same provider binding. No bitmap or second region
is acquired. Allocation-result cleanup finishes before publishing pointer and
capacity. Successful initial prefix allocation then publishes zero live elements
after its own allocation-result temporary is retired; the one-argument entry
delegates to this same alignment-aware path. Rejected allocation on a nonempty
buffer and provider failure leave its prior count, capacity and elements intact.
An empty reservation makes no provider call. Default `std::vector<T>`
construction directly creates an empty prefix owner; it does not enter the
fallible count-based allocation path with a zero request. Pointer, capacity and
live count start empty, no `T` is constructed, and later reserve/append use the
same allocation and publication rules. This also applies to nested vectors and
vectors of non-default-constructible elements. Descriptor copies remain passive
and never gain release authority.

Capabilities expose only irreducible representation invariants. Public logical
size, growth, allocation policy, and container behavior belong in ordinary GTI
wrappers. Sparse-storage capacity, live-slot state, traversal and validation
are ordinary GTI fields and algorithms. The source owner obtains two regions
through matching leases, initializes one metadata byte per slot, and exposes
checked `capacity()` and `initialized(index)` without exposing its raw state.
Final-address construction, typed destruction and relocation use the general
manual-lifetime operations; checked access uses the existing owner-witness
bridge. Retiring a live byte precedes potentially failing element destruction,
and a source drain guard resumes reverse traversal after the first failure.

Source-defined `terminated_prefix_owner` reserves one additional
initialized slot and maintains NUL at the logical length. A
zero-capacity empty string uses an immutable process-lifetime NUL sentinel
instead. The hidden slot is not included in the public length or capacity and
does not change generic prefix policy for other element types. Source
`std::string` methods scan for embedded terminators where required. General
raw text/C-string bridges establish primitive carriers through verified MIR
and retain the exact supplied owner-witness loan, including static/null
addresses. Raw validity remains the unsafe caller's obligation; no string or
prefix type name grants compiler authority.

Numeric input in `std.charconv` keeps integer grammar, radix selection,
overflow checking, consumed-byte offsets, and output publication in ordinary
GTI. Integer accumulation uses exact-width overflow predicates and wrapping
arithmetic. The primitive returns only whether the mathematical operation exceeds
the operand domain; it does not construct a result carrier or choose an error.
`std.numeric` owns checked-result construction and `arithmetic_errc` in source.
The native predicate uses the same overflow builtin or portable mathematical
check as checked integer execution; source success paths use defined wrapping
arithmetic, whose value is exact when overflow was excluded. MIR constant
execution uses the shared exact-width arithmetic evaluator for the same fact.
Negative accumulation admits the signed minimum without overflowing its
positive magnitude. Binary32 and binary64 conversion use the bounded
`gti_rt_parse_f32_v1`/`gti_rt_parse_f64_v1` C ABI over a non-retained counted
input, offset, format selector, and scalar output slots. A configure-time
compile/link probe selects host `from_chars` when both floating overloads are
available. Older standard libraries use pinned Abseil 20250814.1 parsing code,
built as four private objects inside the runtime archive with a GTI-specific
inline namespace. Both paths provide allocation-free, locale-independent
rounding directly to the requested width in general, fixed, scientific, and
hexadecimal formats; the fallback adapter excludes Abseil's additional decimal
`0x` grammar. Errors preserve the caller's scalar value and report the consumed
span. The private dependency adds no installed headers or downstream link
requirements; its license accompanies affected packages. Configure with
`GTI_FORCE_PORTABLE_CHARCONV=ON` to exercise this path on a modern toolchain.
Parser regression checks cover exact boundaries, all rounding modes, consumed
spans, counted input, allocation, and (when available) a host-parser differential.
The `std.string` convenience functions own whitespace/sign/radix-prefix
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
surface in `std.memory`. The compiler recognizes none of those public names.
The public `unique_ptr::swap` delegates to one declaration-bound private
capability operation over two matching mutable owner tokens. MIR verifies both
places and emits native owner swap directly, so exchanging owners cannot copy a
pointee or trigger replacement cleanup.
The stateless allocator calls source-defined process-resource policy. A nonempty
request obtains object bytes, then one metadata byte per slot, through two
separate leases from the same provider; zero capacity acquires neither. Failure
of the second acquisition or validation returns acquired regions before an owner
is published. A successful storage owner carries extent, alignment, per-slot live
state, reverse cleanup, and release authority together. Allocation failure is
returned as `expected`, while bounds and invalid slot-state operations use the
normal defined-failure route. The public owner rejects borrowed element state
but does not require element movement. `construct(index, value)` requires a
movable element and consumes one completed `T`,
so failure while creating the argument remains under ordinary MIR cleanup.
`emplace<Args...>(index, args...)` also supports construction at the final slot
address: semantics selects the constructor, HIR preserves its concrete
parameters, and ordinary source checks establish bounds and vacancy before the
general `construct_at` handoff. The slot becomes live only after that constructor
succeeds; partial
members are rolled back with the original constructor failure record. The
backend implements this verified schedule rather than independently selecting
a native constructor or cleanup policy. Immovable ordinary objects can use this
path; storage handle movement does not relocate elements. Relocation separately
requires the existing element capabilities. ADR 022 owns the public provenance
and failure contract.

Sparse region validation, live-slot bytes and reverse traversal are ordinary GTI
policy. Numeric extent/alignment checks and the narrow native address-alignment
query reject malformed descriptors; they do not prove actual memory validity,
provenance or disjointness. The one-time unsafe resource binding establishes
those caller obligations. General typed lifetime operations carry explicit
`ObjectLifetime` authority and verified constructor/destructor recipes. Source
leases retain both regions through element cleanup, then release metadata before
object bytes. No compiler-private sparse adoption or hidden bitmap remains.
`process_resource` supplies nonthrowing aligned process allocation through
ordinary target-selected C declarations. Its GTI body checks nonzero
power-of-two alignment, native `size_t` conversion and zero size before byte
acquisition. POSIX alignment is raised to the actual native pointer size;
the published descriptor retains the requested alignment. Error constants
come from the target's headers, not hard-coded errno numbers. Windows validates
before calling the CRT, avoiding its invalid-parameter handler for rejected
requests. Empty release performs no native call. No GTI object is constructed
or destroyed by the byte provider.

Those runtime declarations carry a declared contract. The ABI in
`<gti/memory.h>` is GTI's own, so the compiler binds its meaning by trusted
declaration identity: semantics classifies a `ProcessMemoryRuntimeOperation`
only for an exact compiler-private C declaration in `std/detail/memory_capabilities`'s
`gti_internal::runtime` block, matching both symbol and signature, and MIR
verification re-checks that identity and the target-selected family. An
application declaration of the same symbol stays an ordinary native boundary.
The contract follows the execution rule that process allocations are not
observable effects. No operation synchronizes, runs GTI code, traps, reads
GTI-visible memory, or retains an argument. The size, pointer-size and errno
queries are pure target facts; the alignment query inspects only its address
value. Allocation returns fresh storage or null, and the POSIX form also writes
its status pointee. Release retires a block that, by the lease contract, holds
no live object. Effect summaries state both writes conservatively as unknown
memory writes, and the native allocator's internal free-to-allocation ordering
is not a GTI synchronization edge. The summaries let `process_resource` and
its clients keep precise effects above the runtime; they do not authorize
removing or reordering a call. [MIR](mir.md) records the representation.

The old `gti_rt_memory_allocate_v1`/`gti_rt_memory_release_v1` pair has no remaining
repository consumer and is retired rather than retaining a second validation
policy. The replacement native ABI in `<gti/memory.h>` exposes only target facts
and forwarding operations with documented caller preconditions. Native `size_t`
is not GTI's `std::size_t = uint64_t`; adapters perform that conversion only after
source validation. General `c_size_t` and nested-pointer support can now express
the POSIX signature. Its uniquely named allocation and release forwarders remain
because the current C ABI rejects even compatible duplicate symbol declarations;
private declarations of `posix_memalign` or `free` would otherwise prevent
applications from declaring their own libc bindings. CRT leading-underscore
symbols are also inaccessible through the current native-name rules. Reusable
native declarations and exact reserved-symbol binding are the respective removal
prerequisites, rather than missing scalar/output-pointer support. Native target
constants may still need a narrow host boundary. The independent libc-region
example uses ordinary `malloc(c_size_t)` and `free`.

Sparse object regions and live metadata now use this source-owned provider
policy, including default typed factories. Prefix owners now use the same source
provider. Other generated-program allocations remain in unique-owner
construction and shared-owner/control-block representations in the generated
support code. These retain their current allocation families until their own
source/lifecycle migration is validated. Native HTTP/filesystem/thread handles
also allocate host implementation objects behind their existing C ABI. Compiler
host allocations such as emission-plan `std::make_unique` are not GTI-program
allocations and are outside this migration.

`std::arena<T>`, `std::pool<T>`, `std::try_make_arena`, and
`std::try_make_pool` are the first bounded allocation-policy clients. They are
also ordinary GTI source and retain logical size, monotonic/reset policy,
lowest-empty-slot reuse, and public error mapping outside the compiler. Their
constructors consume newly allocated `std::storage<T>` behind an ordinary
source-defined `gti_internal::empty_storage_adoption` seal. The private
namespace makes the precondition unavailable to applications; the seal itself
has no intrinsic identity or backend behavior. HIR therefore selects ordinary
public factories and methods, then reaches the same general typed lifetime and
owner-witness operations available to unrelated source-defined owners.

The unique-owner upcast capability consumes one private owner after semantics
has proved an exact public derived-to-base relationship and polymorphic
destruction. Sparse and prefix shifts are source loops over their respective
live-state policies and the same general typed relocation operations.
Public `std::upcast_unique`, `vector::insert`, and
`vector::erase` remain ordinary GTI policy; neither public name is recognized
by the compiler or backend.

`std::optional<T>` is ordinary source-defined policy over the exact trusted
`gti_internal::optional_storage<T>` capability from `std.optional`. The
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
execute user code, allocate, or raise a contained defined failure. Such a
constructor failure leaves the slot empty after rollback. Infallible allocation
exhaustion instead aborts without cleanup under S-ALLOC-08; it is not an
optional-specific recoverable or cleanup-propagating failure.

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

`std::inline_polymorphic<Interface, Bytes, Align>` is ordinary source-defined
policy over the trusted `gti_internal::polymorphic_storage<Interface, Bytes,
Align>` capability from `std.polymorphic`, plus an ordinary
`std::unique_ptr<Interface>` for boxed payloads. Semantics admits a payload
model into the slot only when the compiler-bound structural constraint
`inline_polymorphic_payload` holds: a concrete class publicly derived from the
polymorphic interface, failure-free relocation, no active cleanup (no source
destructor and no owning field), no checked borrowed state, and no concurrency
capability beyond the interface's. Those facts make every slot operation
failure-free and cleanup-inert, so the slot needs no failure-aware cleanup
recipe; it is movable and move-assignable while optional's slot is not. The
library chooses inline placement with an `if constexpr` over that concept and
a runtime `std::layout_of` comparison, which the native compiler folds.

The source-defined consuming `invoke_once<R>` selects its inline or boxed
target. Semantics derives an owned-pointee projection from its transparent
mutable accessor; MIR independently checks the exact capability, field,
intrinsic, returned loan and single invocation use. The root may be consuming
`this` or an owned field path. Remaining cleanup stays with that owner. The
once-invoked function wrappers use this same owner and allocation policy, so
small eligible captures avoid a heap allocation without new callback-specific
compiler behavior. Cleanup-bearing captures remain boxed.

MIR verification re-proves each of the five operations: the exact capability
class and staged storage place, the interface result and owner-tied loan for
reads, no failure edge, and for construction one completed payload of a
concrete, cleanup-inert, relocatable class whose recorded base chain reaches
the interface. Violated private preconditions (an engaged construction or a
payload larger than the native slot) trap rather than produce defined failure,
because application code cannot name the capability.

The C++ representation stores the payload bytes, a pointer to one constant
per-model operations table, and a cached interface pointer. Moving or
move-assigning the slot calls the table's relocation, which move-constructs the
payload at the destination with its generated move and destroys the moved-from
source (the same failure-free relocation `relocate_at` uses), then recomputes
the interface pointer from the destination. A polymorphic payload is never
copied as bytes, which keeps signed vtable pointers valid. The cache is never
copied from the source. Closure-bearing instances of one generic function
share a C++ template body. When instances differ only through a closure
nested in a class argument, such as a model whose inline eligibility depends on
its captures, the emission planner adds a nested closure-identity selector so
each verified body stays distinct.

The same identity boundary applies to generic constraints. Classification,
lifecycle, relocation and transfer/share facts are general constexpr
`std::meta` queries over identity-bound compiler operations. The prelude defines
its public concepts with those queries and ordinary expression requirements;
it does not maintain duplicate private concept declarations for migrated facts.
The compiler retains structural truth, not library composition policy.
Comparison, callable, sentinel, checked exact-reference, inline-payload policy,
and complete range concepts now compose ordinary source reflection and
unevaluated requirements. Range queries reuse actual loop analysis, including
access, endpoint lifetime and cleanup checks; they are not a second range
protocol implementation. Input iteration, dereference-to and writable iteration
use inferred expression requirements plus exact declaration/reflection facts.
Size availability uses an ordinary exact-result expression query; fixed-array
size remains a checked compiler operation. There are no remaining private
concept bindings. Type categories, lifecycle/relocation, transfer/share,
layout, exact relations and checked borrow origins remain compiler facts;
source code decides how to combine them into public concepts.
Semantics recognizes only trusted declaration identities and exact facts,
never those public names or `std::accumulate`.

## Package Build-Program Boundary

`std.build` is an ordinary source-defined, host-only library surface for
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

`std.process` promotes exact synchronous argv execution to ordinary hosted
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

`std.environment` supplies the separate read-only observation family. Its
`get` function and error enum are ordinary GTI, with no public-name recognition.
The counted `gti_rt_environment_get_v1` ABI revalidates names, copies one current
native value, and distinguishes a missing variable (zero handle) from a present
empty value (live zero-length snapshot). Size/data/destroy operations access
only that immutable copy. Metadata and bytes share one native allocation; no
C++ exception crosses the boundary.

A source-defined move-only temporary owns the snapshot. It forms a checked
view using the general erased-address and owner-witness facilities, copies it
into an exactly reserved GTI string, then destroys the snapshot through normal
MIR cleanup. Only `optional<string>` crosses the public API. Concurrent
read-only queries work; foreign environment mutation retains its unsafe
process-wide synchronization obligations. Environment setters, enumeration,
and child-local overrides are not part of this slice. See
[the language contract](../language/process.md#environment-observation).

## Clock Boundary

`std.chrono` keeps duration and time-point policy in ordinary GTI. Its signed
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

Scalar atomic storage and operations are available in both execution profiles
without a host thread runtime. Explicit wrapping integer add/subtract use typed
RMW intrinsics; the shared owner proves a positive count before its total
wrapping decrement. Checked add/subtract retain their source compare/exchange
loops and defined overflow behavior. Blocking wait/notification retains the
concurrent-profile and host-thread-runtime requirements, carried
through the existing transitive thread-runtime call-effect analysis. This
separates a general atomic mechanism from permission to create or coordinate
workers; reference-count algorithms can remain ordinary source in either profile.

The public managed-thread type is the ordinary source-defined `std::thread`
class in `std.thread`. The compiler does not recognize that class, its
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

The ordinary `std.atomic` source unit defines move-only `std::atomic<T>` and
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

Safe scalar waiting uses the same private storage and ordinary public source
wrappers. Exact private wait declarations attach `AtomicWait` plus a load-legal
order; notification declarations attach `AtomicNotify` without an order and
retain distinct one/all intrinsic identities. MIR validates stable shared
storage, exact scalar operands, void results, absent ownership transfer and
failure edges, and the synchronization identity. Both operations are retained
as non-removable, non-speculatable, non-reorderable effects even for relaxed
waiting. Only verified MIR emits native wait/notify calls. The C++20/C++23
representation delegates parking to `std::atomic<T>` without extra GTI state,
polling policy, or per-call allocation. This is a target implementation of
GTI's documented wait contract, not semantic authority from the C++ spelling.
No mutex-loan suspension, public-name exception, new syntax or LSP inference
path is introduced. Ordinary application wrappers compose the same calls.

LLVM notification uses the versioned final-image runtime enrollment domain,
not a second parking table or a per-object extension. Its 256 buckets have
128-byte stride and 64-byte alignment; two fixed 32-bit metadata words precede
120 opaque native bytes. The native provider checks its actual pthread layout
fits, begins the native object lifetime once and never tears the domain down.
The domain occupies 32 KiB of static storage (previous private Linux buckets
occupied 24 KiB); this does not measure pthread/libc internal allocation.
The x86-64 path retains the acquire/release wake-bit RMW chain; arm64 retains
the sequentially-consistent fence and relaxed waiter-count test. Only a
registered notification calls the nonthrowing native broadcast/rearm service,
which does not consume a second wake bit. Direct C wait/notify services keep
their full input validation and share the identical domain. Typed code obtains
lane width/alignment from verified MIR and layout, without rediscovering source
ownership or weakening the synchronization contract. Independent unloadable
providers remain unsupported, as before. The identical-source wait cost oracle
compares optimized public LLVM and C++20/23 execution and retains their build,
memory and binary-size observations separately from raw C-service cost.

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
owned by the exact trusted `std.thread` unit rather
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
complete unused `std.thread`, `std.atomic`, and `std.mutex` declaration
inventories exist and build natively in either profile; it cannot create a
task adapter or executable synchronization use and does not bypass the public
semantic profile check. Their retained generic classes name the private
storage representations, so the backend forward-declares those templates for
each retained capability declaration without selecting native atomic or mutex
support.
Optimizer effects preserve every record as a barrier. The concurrent profile
plus target `threads` capability admits the implemented operations. Spawn
transfers one exact
owned task to the worker, successful explicit or automatic join establishes
completion-before-return, and no public detach operation exists.

The mutex surface follows the same policy split. Exact private storage and
lock-token declarations lower to the native mutex preamble only when a
concrete capability instance exists; without one they contribute only the
forward declarations of their representation templates. `std::mutex<T>` remains source policy;
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
ownership state. `gti_rt_thread_quarantine_v1` exposes that same terminal-only
ownership transfer to nonthrowing generated adapters. It permanently pins
native process storage: the worker may still use share-capable globals, and
the manual C-thread registry does not track managed workers. The nonthrowing
`gti_rt_thread_program_storage_retirement_ready_v1` query returns zero after
quarantine, even if the worker subsequently finishes. Hosted failure preserves
the original record while leaving native roots live until terminal process
exit. A native module-unload hook rejects live pinned roots rather than
destroying or unmapping them. An embedding context must consult the same query
and refuse shutdown/unload when pinned; normal foreign-caller quiescence remains
a separate host obligation. There is no asynchronous cleanup or retry policy.

The versioned nonthrowing worker seam is declared in `runtime_thread.h`.
`gti_thread_failure_view_v1` borrows one exact v2 ordinary or cleanup record
plus its pinned artifact descriptors for the duration of publication. A sink
must copy the record before returning. The runtime installs a default worker
sink around entry, task/result cleanup and TLS drain. Publication records an
outcome but performs no control transfer: generated adapters must catch their
selected internal GTI transport before returning through any C callback.
No C service or callback receives permission to unwind.

A typed completion callback consumes its task exactly once. Null is accepted
only after an outcome failure has been published and no live result remains;
a non-null result on failure is abandoned by the runtime. Successful null
completion remains an integrity termination. This lets a nonthrowing adapter
finish failed task/capture cleanup without manufacturing an owned result.

Synchronous launch rollback and post-join result take/abandon instead need a
caller-owned `gti_thread_failure_sink_v1`. Enter/leave nest address-stable,
thread-confined descriptors in strict LIFO order; an inner caller sink takes
precedence even when its caller is a managed worker. The descriptor allocates
no memory and owns no mandatory success-path failure buffer. Publication is
cold, rejects malformed records/reentry/descriptor mutation and terminates if
a sink throws. Worker task-cleanup queries expose the pending primary only
while that exact default sink is active. A second cleanup failure preserves
the primary in the emergency envelope; a third failure terminates.

`runtime_thread_outcome_boundary` tests the C17 layout/signatures, copied
records, nested caller scopes, launch rollback, worker and post-join result
cleanup, TLS-before-result cleanup and isolated integrity deaths. A real join
failure also checks sticky retirement pinning after worker cleanup. These are
isolated terminal-retention checks; only that subprocess disables TSan's
unjoined-thread exit report, while race detection and all ordinary shutdown
leak reports remain enabled. This is
runtime containment evidence, not generated LLVM worker admission or a C04
transport selection. The generated entry/cleanup/result adapters remain a
separate integration prerequisite.

The O13 mutex service oracle also lowers an unmodified sealed MIR program
through the production body lowerer before public admission is opened. It
executes source constructors, partial rollback, TLS and native process
retirement at O0-O3, with failures before initialization, in either constructor,
after publication and in the source entry. C observers count real native shell
initialization/retirement, verify TLS still sees both live roots and inspect the
original record at the real failure terminator. This is source-composition
evidence, not a public mutex/worker profile or a C04 transport decision. Separate
kernel cases check repeated teardown and unpublished-prefix handling. A real
quarantined worker keeps locking a published native root through explicit
hosted retirement; the original ordinary failure is preserved and both roots
remain live. The unload endpoint rejects those pinned roots without retiring
them. These are terminal, isolated processes, not leaked normal-shutdown tests.

An additional source-composition fixture uses the actual fallible GTI payload
destructor, rather than the kernel's explicit C destructor double. A source
record owns two mutexes; an unrelated owning sum carries another mutex inside
an outer mutex payload. Normal and first-failure traces require three exact
payload destructions in reverse order and one retirement per initialized native
identity, including identities created by payload transfer. A second destructor
failure requires the real emergency record and stops before retiring the final
native shell, matching terminal cleanup abandonment. Native O0-O3 and sanitizer
execution remain distinct from public admission.

The optional `llvm_mutex_service_test.py --gti` differential checks those actual
source destructor traces through C++20/O0 and C++23/O3. The current C++ backend
rejects the public wrapper constructor's fallible moved-parameter cleanup as
outside its complete text vocabulary. That failed comparator is a recorded
backend gap, not a pass or a reason to weaken the native oracle; existing C++
mutex regression families remain required. No C++ feature catch-up or failure
transport change is supplied to make this new composition comparable.

### Scoped-worker execution

`gti_rt_thread_scope_v1` adds a synchronous native group boundary over borrowed
opaque task contexts. The caller supplies task descriptors and scratch handle
slots; the service starts workers in descriptor order and joins every started
worker before returning, including after partial creation failure. It neither
destroys nor retains the contexts. This avoids a separate task-box or dynamic
handle-array allocation; each started worker still has the existing native
thread and runtime-state costs. It is not a worker pool or job scheduler.

Worker adapters use the same managed failure containment as owned tasks. The
group preserves the first failed worker in input order, not completion order,
with its exact failure record and artifact identities. Creation status is
reported separately so a generated caller can propagate a worker failure
before exposing a later creation error. All other started workers have already
joined at either outcome. A native join error is process-terminal without
unwinding the caller: borrowed contexts cannot enter the owned-thread terminal
quarantine, because the borrowing stack must not be destroyed beneath a live
worker.

The source-defined `std::parallel_invoke` wrapper maps recoverable status over
the trusted `ThreadScope` capability. Semantics selects concrete callable
entries and checks all borrowed origins. HIR preserves the group; MIR verifies
task consumption, exact dependency/access identities and loan endpoints on
normal and failure paths. Ordinary borrowed carriers and forwarded packs use
the same place/loan model as other calls. The raw ABI still requires its caller
to establish context lifetime, disjoint or synchronized access, and scratch
isolation; calling it does not manufacture a safe GTI proof.

The C++ backend roots every scoped adapter from its exact operand index and
emits a stack tuple of task lifetime slots plus fixed-size descriptors and
scratch slots. No per-task box allocation or native destructor supplies GTI
cleanup. Once the native group has joined, generated cleanup adapters run the
verified remaining drops in reverse argument order using the ordinary failure
cleanup context. Consuming-receiver cleanup has already discharged its target;
moved-from representation shells are retired separately. A worker failure is
re-raised only after that join-all boundary. Cleanup failure while another
failure is pending uses the existing emergency rule, not a second failure
interpreter.

## Native And Runtime Boundary

`runtime/include/gti/c_abi.h` defines the public C-compatible counted-text
record. `runtime/include/gti/runtime.h` defines runtime entry prototypes, and
`runtime/src/` implements host behavior. Bounded `extern "C"` GTI declarations
carry exact external symbols and an allowlisted ABI selected in semantics.

`runtime_execution.h` adds narrow C entry points for the already-existing
attached TLS lifetime and reverse cleanup drain. Native LLVM startup can call
them without naming C++ runtime helpers; typed native TLS registration remains
a later backend capability. They retain the existing runtime contract and
v2 failure transport, including containment rather than cross-C unwinding.
The [LLVM target/ABI boundary](backend.md#llvm-target-layout-and-execution-abi)
describes exact runtime-record layouts, native service declaration planning
and the Clang-generated complete C-service inventory. This is infrastructure,
not executable LLVM library support.

The original stdout and single-byte input/file entries remain compatibility
symbols. The version-one I/O family adds counted stdout/stderr writes, bounded
stdin/file reads, typed file open, complete file writes, file seek, and
standard-stream flush. It knows only counted bytes, descriptors, fixed-width
status values, and stable mode/origin ordinals. It does not know about
`std::FILE`, `std::string`, formatting, CSV records, buffering, or encoding.
Portable ownership, chunk allocation, mode spelling, EOF policy, and
`expected` translation remain ordinary `std.cstdio` source.
The `std::getline` overloads likewise implement delimiter recognition, final
unterminated-line handling, and owned-string assembly in GTI source over the
existing single-byte entries; the runtime has no line or terminal-prompt
policy.

Exact `char`/`uint8_t` conversion bridges the byte representation in trusted
source without making `char` numeric or adding an implicit conversion. Chunk
reads use one fixed internal array and return an owned string; application code
never receives the mutable runtime buffer or native descriptor.

`std.http` follows the same public-policy boundary for owned URL construction
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

`std.json` needs no runtime or compiler-private capability. Its strict
scanner, reflection-driven encoding and decoding, exact number handling,
deterministic serializer, limits, structured errors and explicit `codec<T>`
customization are all GTI source over `std.meta`. This is an intentional
proof that a format library can remain portable policy once compile-time
reflection, `std::meta::construct` and the ordinary string and vector
ownership primitives are sufficient. String-owning errors are ordinary source
`expected` values executed through verified MIR; the branches that add JSON
Pointer context remain explicit source code.
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

`std.random` keeps seeded pseudo-random state and sequence policy in ordinary
GTI source. Its `std::random_device` crosses the bounded runtime entry
`gti_rt_random_bytes` only when it refills an internal fixed array; the caller
supplies the checked buffer and count inside the lexical raw-pointer `unsafe`
boundary. The runtime contributes host entropy and a status code, not an
engine algorithm or public-library object representation.

`@runtime` remains a closed compiler-validated compatibility surface. New host
APIs should prefer ordinary GTI wrappers over the bounded native boundary when
the existing ABI can express them. Neither mechanism turns a public GTI class
layout into an ABI.

`std.cmath` supplies ordinary GTI wrappers for its complete declared
binary32 math family. They lower as ordinary verified-MIR native calls to
version-one `gti_rt_math_*_f32_v1` entries in `runtime/include/gti/math.h`.
`runtime/src/math.cpp` uses the host's float math implementations; the seam
passes and returns only scalar `float`, with no class layout, allocation,
ownership transfer, public-name intrinsic, or alternate backend body path.
Classification and absolute value remain GTI source. Domain, pole, overflow,
and underflow results follow the documented floating-point value contract,
not the defined-integer-failure contract. Host errno and floating exception
flags are not exposed to applications.

`std.cthread` begins with ordinary GTI `std::thrd_yield()` over the
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
Executable lowering follows reached roots rather than emitting every unused
ordinary wrapper, but native C declarations remain roots. `MirProgram` derives
the C-thread service requirement from retained native operation/adapter
records. Consequently a concurrent program importing `std.cthread` still
conservatively includes the empty begin/end lifecycle, even for yield-only
use; a default-profile yield/identity/sleep-only program does not start the
service. Demand-only native-service selection is not established by pruning
ordinary wrapper bodies alone. The C archive-member separation remains useful
to native consumers and must not be confused with that GTI startup policy.
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
handlers nor masks. Linux measures `nanosleep`'s remainder against the timer's
slack-extended expiry. An interruption within the slack can therefore report
more than was requested, so POSIX clamps the remainder to the request. Windows rounds up to the relative waitable timer's 100ns
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

Explicit thread-confined native adapters bind an atomic owner identity at their
verified MIR conversion. Repeated conversion first uses a relaxed owner load;
first registration uses a relaxed CAS. This publishes no user data. The owner
uses the existing never-reused C-thread identity space. Private
`gti_rt_confined_callback_*_v1` operations require an active initial/managed/
registered manual execution scope and check identity before target entry.
They perform no heap allocation or registry locking. Runtime phase is atomic
for this lock-free observation; existing service transitions remain mutex-owned.
Nested entries maintain a thread-local depth so all callback failures keep the
terminal firewall, including on managed workers. Leaving restores the outer
failure policy. No operation attaches a foreign thread or rebinds an adapter.
The per-adapter atomic and native signature are backend representation choices;
the entry contract and non-removable conversion effect are verified MIR facts.

## Defined-Failure Boundary

[Execution §4.10](../language/execution.md#410-defined-runtime-failure) assigns
checked-failure meaning to compiler-owned facts and leaves the runtime a narrow
environment role. Semantics/HIR select local origins and propagation, the
failure-metadata builder assigns artifact-local sites, and verified MIR retains
their exact detector mappings, propagation and cleanup control flow. Remaining
composition slices extend that authority rather than introducing a source-tree
emission fallback. A generated boundary/runtime pair may serialize,
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

`gti_rt_failure_abort_allocation_v2` is the runtime
primitive for infallible exhaustion. It reports a validated allocation record
without allocating, then calls native abort directly: no observer, managed-task
handoff, stream flush or terminal-arbitration wait. A malformed native record
falls back to a descriptor-free allocation report. This primitive must be
called at the detector, before cleanup. Verified MIR distinguishes allocation
abort from contained failures; scalar/class/shared allocation and source
infallible growth use that terminal path without an exhaustion cleanup edge.
Mixed outcomes retain their other cleanup edges. Combined source-family,
corpus and performance acceptance passed for S-ALLOC-08/09 in 0.397.0; see
[verification](verification.md#source-prefix-ownership-and-allocation-abort-acceptance-03970).

`gti_rt_contract_terminate_v1` ends a broken native or private runtime contract
that is not a GTI failure: a foreign exception reaching a GTI callback
boundary, a thread-confined callback bound on a second thread, or a malformed
private runtime handle. It calls `std::terminate`, so natively lowered code
reaches the host's C++ terminate handler exactly where generated C++ calls
`std::terminate`; without a handler the process aborts. It reports nothing,
runs no cleanup and never returns.

Only `panic` / `explicit_panic` (`GTI-R0017`, detail 37) admits a message.
Generated code prepares its exact record header, then calls
`gti_rt_failure_capture_panic_v2` before starting language cleanup. Capture
also has the scalar-ABI `gti_rt_failure_capture_panic_bytes_v2(record, data,
size)` form for native code generators; it forwards to the same implementation
without inventing an aggregate calling convention or changing the contract.
Both forms of capture
copy at most 512 bytes without allocation and do not split a valid UTF-8
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
stopped. Native variant, array, closure, and unique-owner shells may
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
identity-bound origin/check capability already supplied by M-FAIL-01. It uses
the established I-CAP-01 private identity and visibility boundary.
Ordinary GTI wrappers call it with logical size and a fixed public domain;
applications cannot forge its identity or choose an arbitrary category/detail.
This keeps public `vector`/`string` origins in source/library policy without
asking a backend to inspect names or call stacks.

`std::vector` derives live size and capacity directly from its source prefix
owner, without mirrored counts. Public accessors perform the identity-bound
logical bounds check and then use unsafe source access that adds no second
bounds guard. Capacity-established construction and mutation similarly use
unchecked internal operations. Independently callable checked detail-owner
methods retain their own boundary checks. Span constructors validate their
extent once; span element access checks its logical extent and uses the same
unchecked backing access. Unsafe `get_unchecked` and `get_unchecked_mut`
accessors preserve ordinary checked owner loans while requiring the caller to
prove the index. Fixed-array unchecked access forms an element pointer under
the concrete nonempty-extent branch; empty arrays have no valid unchecked index.
No former private prefix-length optimizer relation proves arbitrary source
calls. The integrated source cutover has passed its general source-container
proof/replay and optimized workload acceptance; that is not a claim that every
future accessor or workload is already optimized.

The source prefix owner also permits explicit unsafe publication of an
initialized length. This operation only changes the count: the caller must own
the exclusive lease, stay within capacity, construct every newly included
object, and destroy every removed object first. A fallible construction loop
must publish each completed element before another fallible operation, or keep
an independent rollback guard. The contract applies equally to unrelated
user-defined elements; it is not a character or container compiler exception.

The terminated character owner uses this mechanism for counted-view
construction, append, indexed-range copies, substring copies and clone. It
validates the source interval and reserves the complete destination before
mutation, retires the old terminator, constructs the character range, then
constructs one trailing NUL and publishes the complete physical prefix once.
The copy uses an absolute induction variable bounded by the checked source
endpoint, allowing ordinary bounds-check elimination and vectorization without
repeated prefix updates. The validated character-copy phase has no user callback
or fallible character constructor. Empty ranges do not touch storage. Embedded NULs remain counted
characters, and exclusive-receiver borrowing continues to exclude aliased
source views. Fixed-array and general input-iterator constructors retain their
existing per-element path.

Sparse and prefix relocation consume semantic `RelocationKind` facts verified
against concrete MIR type tables. Raw-buffer growth selects general
`relocate_range_at` for structurally bitwise-relocatable elements, transferring
the entire live prefix in one nonoverlapping byte copy. Other eligible elements
retain a source traversal over general `relocate_at`; insertion and erasure
also use exact per-element relocation. Native representation traits may restrict
lowering but never decide a GTI concept. Source lifetimes end, destination
lifetimes become live, and string relocation includes its hidden terminator.
Resource lease authority is transferred separately, never duplicated by the
element byte copy.

Utility `swap` and `exchange` are ordinary constrained GTI wrappers over
private restoring-transfer declarations in the prelude. Declaration identity,
not a public library spelling, selects the capability. The backend helper
performs only MIR-proven failure-free structural movement and ends inactive
native shells; it never calls replacement assignment or destroys a live GTI
resource. Existing MIR call-input staging and result slots own preparation,
publication, and cleanup on both ordinary and failure exits.

`array::swap` uses whole-array structural transfer. Array/vector
`swap_elements` perform public bounds checks. Arrays use the retained fixed-array
indexed transfer; vectors use three general relocations through source-selected
uninitialized storage. Reverse and introsort
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
