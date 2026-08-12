# Language Restriction Ledger

> **Plan status:** D-LANG-01 complete. Maintained, non-canonical pre-1.0
> restriction ledger. Current language meaning remains under
> [`docs/language/`](../language/index.md).

Baseline: GTI 0.94.0.

This ledger classifies the restrictions called out by the third-party
[language audit](../third-party-audit/language-audit.md), the original language
alignment discussion, every explicit specification gap in `docs/language/`,
and the restrictions that currently protect GTI from its transitional C++
backend. It does not change syntax or semantics. A row describes the current
boundary, why that boundary exists, its 1.0 disposition, and the plan owner or
dependency chain authorized to change it.

GTI is not source-compatible C++. Its target remains a C++-familiar systems
language with explicit ownership, deterministic cleanup, predictable
performance, and native interoperation without inheriting C++ lookup,
conversion, lifetime, preprocessing, or undefined-behaviour defaults.

## Ledger Vocabulary

Every entry has one primary class:

| Class | Meaning |
| --- | --- |
| **safety/simplicity** | The restriction is an intentional rule that removes an unsafe, ambiguous, or disproportionately complex feature. |
| **proof** | The desired construct needs ownership, place, lifetime, initialization, or cleanup facts the compiler cannot yet prove. |
| **lowering** | The meaning can be stated, but HIR/MIR, the backend, runtime, or tooling does not yet represent it completely. |
| **library** | The core language can support the facility, but public source-defined policy or a narrow host capability is missing. |
| **choice** | GTI must first adopt a semantic, ABI, compatibility, or horizon decision. |

The 1.0 disposition is one of:

| Disposition | Requirement |
| --- | --- |
| **keep-v1** | The current rule is the GTI 1.0 rule. Relaxation after 1.0 requires an additive proposal or the compatibility mechanism. |
| **close-v1** | The named gap must be specified and implemented before the 1.0 release gate. |
| **bounded-v1** | The named minimum must ship for 1.0; explicitly broader forms remain rejected. |
| **adopt-v1** | A canonical decision is required before 1.0, while the executable feature may have a later horizon. |
| **post-v1** | The current rejection remains explicit in 1.0. No change is promised until the named row becomes eligible and demonstrates its client. |

An owner cell names the operational row or dependency chain from
[`implementation-sequence.md`](implementation-sequence.md). Prerequisites are
not permission to expand a row's scope. “New row required” means no
implementation is scheduled; the restriction cannot be relaxed by folding it
into an adjacent feature.

## 1.0 Scope Decisions

The ledger fixes these release-horizon questions:

1. Complete evaluation/full-expression order, temporary/drop authority,
   defined failure, source-text and target facts, compatibility, private
   capability enforcement, and the concurrency boundary are pre-1.0 work.
2. The transfer/share type facts and concurrent-global policy are adopted and
   represented before compatibility freezes. Public threads, atomics, mutexes,
   weaker memory orders, and native-thread entry are post-1.0 executable work.
3. A GTI-owned target data-layout contract plus bounded `sizeof`/`alignof`,
   defined wrapping/saturating integer operations, and IEEE-754 binary64 are
   required for 1.0.
4. The accepted range, mutable-iteration, single-owner view, owned-callable,
   shared/weak-owner, optional, text, container, and hosted-service minimums
   remain 1.0 library work.
5. Native records/callbacks, public allocator customization, freestanding
   execution, payload enums, propagation syntax, broader operator families,
   generalized stored borrows, and associative containers remain post-1.0.
6. Exact calls, read-only `T&`, writable `mut T&`, consuming `std::move`,
   mandatory result use, exact direct construction, non-textual direct
   includes, and the absence of source `new`/`delete` are intentional v1 rules.

## Foundational Semantics And Backend Independence

| ID | Current restriction or gap | Class | 1.0 disposition | Owner and reconsideration evidence |
| --- | --- | --- | --- | --- |
| `R-EXEC-ORDER` | Only short-circuit and selected control-flow order is complete. Operand, argument, initialization, temporary, and cleanup order is not complete, so a transient borrow and overlapping mutation are conservatively rejected in one call regardless of written order. | lowering | **close-v1** | `D-EXEC-01` chooses the rule; `M-LIFE-01` and `M-EXEC-01` provide the facts, while `M-BACK-01/02` migrate affected families before each conservative restriction is narrowed. |
| `R-TEMP-DROP` | Every temporary lifetime, partial-construction cleanup, compound-expression cleanup order, and cleanup at a checked failure is not yet authoritative in executable MIR. | lowering | **close-v1** | Execution §4.10 fixes failure cleanup semantics; `M-OWN-02`, `D-EXEC-01`, `M-LIFE-01`, `M-EXEC-01`, and `M-FAIL-01` must provide the drop/rollback/order facts, verifier mutations, and O0/O3 exactly-once traces. |
| `R-FAIL` | Execution §4.10 now defines categories, artifact-qualified sites, status/reporting, cleanup, observation, embedding, allocation, and worker containment, but the emitter still aborts without those semantics and native expected observers still escape them. | lowering | **close-v1** | `D-FAIL-01` is done; `I-CAP-01`, `M-LIFE-01`, the relevant `M-EXEC-01` slices, and co-delivered `M-FAIL-01`/`Q-FAIL-01` implement the IR/runtime substrate; `M-BACK-02` then migrates every closed call-graph family and removes native helper behavior. |
| `R-MEMORY-MODEL` | Current execution is single-threaded and has no canonical data-race, transfer/share, atomic, thread, or foreign-thread rule. | choice | **adopt-v1** | `D-MEM-02` adopts the completed proposal. `I-CAP-01`, `C-TYPE-01`, and `C-GLOBAL-01` are pre-1.0 representation/policy work; canonical language docs and the restriction ledger must agree. |
| `R-CONCURRENCY-API` | No public atomic, thread, mutex, detach, scoped-thread borrow, or native-thread callback API exists. | library | **post-v1** | `C-MIR-01` through `C-CONFORM-01` in their dependency order. D-MEM-02 may refine the accepted first profile but does not promote executable concurrency into the v1 gate. |
| `R-SOURCE-TEXT` | Source encoding, BOM handling, newline normalization, Unicode identifiers, and normalization are not normative. | choice | **close-v1** | The independent source-text sub-slice of `L-TEXT-01`; lexer, source offsets, formatter, Tree-sitter, LSP, invalid-byte cases, and installed-toolchain tests must share one byte/Unicode contract. |
| `R-DOC-COMMENTS` | Documentation comments have no declaration attachment; the lexer, formatter, Tree-sitter, and LSP currently recover comment information separately. | lowering | **close-v1** | `T-LSP-01`; retain declaration-owned Markdown and extents once, then test hover, completion, formatting, parsing, and generated docs. |
| `R-DEPRECATION` | Declarations cannot carry a source-owned deprecation message that produces a use-site diagnostic and tooling metadata. | lowering | **close-v1** | `D-COMPAT-01` defines policy and `Q-DEPRECATION-01` implements the bounded attribute, declaration metadata, diagnostics, hover, completion, formatter, and Tree-sitter coverage. |
| `R-TARGET` | The complete target-property vocabulary, triple interpretation, unknown-property behavior, supported cross-target contract, and data-layout facts are incomplete. | choice | **close-v1** | `S-LAYOUT-01`; selected target facts must be immutable, backend-neutral, deterministic, and checked against installed native probes before source queries use them. |
| `R-PRIVATE-CAPABILITY` | Trusted intrinsic declarations bind by identity, but application source can still name ordinary `gti_internal` declarations and some wrappers expose forgeable implementation types. | lowering | **close-v1** | `I-CAP-01`; forged declarations, aliases, direct references, public-signature leakage, LSP exposure, and the current `cstdio` constructor case must fail while ordinary stdlib wrappers continue to work. |
| `R-COMPATIBILITY` | Pre-1.0 releases may change meaning; no 1.x compatibility/edition policy exists. | choice | **close-v1** | `D-COMPAT-01`; unknown selectors must fail and old source meaning must not change silently. |
| `R-BACKEND-AUTHORITY` | The C++ backend still emits complete bodies from AST/semantic/HIR data, and some accepted evaluation/lifetime behavior is not yet executable MIR authority. | lowering | **close-v1** | `M-EXEC-01` and `M-BACK-01` start the migration; `M-BACK-02` completes it by closed call-graph family. The 1.0 release gate requires every accepted observable order, failure, lifetime, and cleanup rule to be represented independently of C++. |

## Stable V1 Surface Choices

| ID | Current restriction or gap | Class | 1.0 disposition | Owner and reconsideration evidence |
| --- | --- | --- | --- | --- |
| `R-EXACT-CALLS` | Calls use one exact parameter match after substitution, apart from bounded null/pointee qualification. Safe integer widening remains available only in documented value-assignment contexts such as initialization and return; calls have no conversion ranking, ADL, return-context inference, or concrete-over-generic preference. | safety/simplicity | **keep-v1** | Current static semantics. Any later additive conversion set needs a new row proving unique selection without ranking; conversion-ranked overloads remain outside v1. |
| `R-REFERENCE-SPELLING` | `T&` is a non-null read-only loan and `mut T&` is writable; GTI does not import C++ reference or cv meanings. | safety/simplicity | **keep-v1** | Current ownership contract and `D-COMPAT-01`; diagnostics/documentation remain the migration mechanism. |
| `R-MOVE-SPELLING` | `std::move(place)` consumes the tracked source even when it is copyable; later use is invalid until permitted reinitialization. | safety/simplicity | **keep-v1** | Current ownership contract. A spelling change would be compatibility work and is not scheduled. |
| `R-RESULT-USE` | Every non-void call result must be consumed or explicitly discarded at the call site. There is no declaration-side implicit-discard policy. | safety/simplicity | **keep-v1** | Current static semantics. A future declaration-side policy needs a new row proving it cannot hide `expected` or ownership failures. |
| `R-DIRECT-CONSTRUCTION` | `Type value{args}` is exact direct construction, not aggregate/list conversion, initializer-list preference, narrowing policy, or CTAD. | safety/simplicity | **keep-v1** | Current static semantics. Initializer-list and CTAD semantics require a separate post-1.0 proposal. |
| `R-INHERITANCE` | One state-bearing public base plus stateless interfaces is permitted; diamonds, private/protected inheritance, multiple state-bearing bases, slicing, covariant returns, and generic virtual methods are rejected. | safety/simplicity | **keep-v1** | Current class/interface contract. A new row is required to demonstrate an ownership, layout, dispatch, and cleanup need before any relaxation. |
| `R-INCLUDES` | `include` is load-once, direct-visibility, non-textual dependency loading. It does not re-export, preprocess text, define macros, or name binary modules. | safety/simplicity | **keep-v1** | `D-COMPAT-01` freezes the v1 spelling. Module vocabulary and binary distribution remain post-1.0 under `E-ABI-01`; textual macros are not planned. |
| `R-INFERENCE` | `auto` is confined to initialized locals and range elements; it does not infer API signatures, globals, fields, arrays, references from plain `auto`, or untyped braced values. | safety/simplicity | **keep-v1** | Current grammar/static semantics. A new row is required for any API-visible inference proposal. |
| `R-STRUCTURED-BINDINGS` | Structured bindings are immutable flat projections of one hidden owned array/aggregate. Mutable/reference/nested/inherited patterns, loop declarations, and partial movement are rejected. | proof | **post-v1** | No change row is scheduled. Reconsideration requires `M-OWN-02` and `M-LIFE-01` evidence plus a dedicated structured-binding row covering projection identity and partial drop. |
| `R-BLOCK-STATIC` | Namespace internal-linkage and class static storage exist; block-scope static and static members of generic classes are rejected. | choice | **post-v1** | `C-GLOBAL-01` must first settle initialization/concurrency policy. A new row must then define once initialization, failure, destruction, generic identity, and target behavior. |
| `R-EXCEPTIONS` | GTI has no source exceptions, catch/resume, or native exception-ABI unwinding; recoverable APIs use `expected`. Defined failure uses compiler-managed non-resumable control-flow propagation solely to perform Execution §4.10 cleanup and reach a containment boundary. | safety/simplicity | **keep-v1** | `D-FAIL-01` is complete. Source handlers, resumption, or cross-ABI native unwinding need a new post-1.0 design row. |

## Ownership, Places, Borrows, And Allocation

| ID | Current restriction or gap | Class | 1.0 disposition | Owner and reconsideration evidence |
| --- | --- | --- | --- | --- |
| `R-PLACE-STATE` | Precise places cover roots, fields, and bounded checked dereferences, but constant/dynamic indexed partial movement, complete definite initialization, and general reinitialization are incomplete. | proof | **close-v1** | `M-OWN-01` and `M-OWN-02`; semantic and MIR state must agree at branches, loop backedges, return, and drop without backend repair. |
| `R-READ-BORROW-CARRIERS` | One direct read-only stored reference and one exact owner origin are supported. Multiple/nested/inherited origins, dependency-changing assignment, captured/global storage, and arbitrary borrowed aggregates are rejected. | proof | **bounded-v1** | `L-RANGE-03` ships the focused single-owner view required by v1. Broader graphs require a new row after the stable place/drop model; they are not implied by that view. |
| `R-BORROWED-MERGES` | A conditional expression can merge exact owned values but cannot select a borrowed result whose origin and endpoint differ by branch. | proof | **post-v1** | No change row is scheduled. Reconsideration requires `M-OWN-01`, `M-OWN-02`, and `M-LIFE-01` evidence plus a dedicated row preserving the selected origin, loan state, and endpoint through semantics, HIR, and MIR. |
| `R-MUTABLE-STORED-BORROWS` | Mutable child loans are local and non-escaping. A field, guard, return, iterator, or other stored carrier cannot retain a mutable parent/child dependency. | proof | **post-v1** | `M-OWN-03`; exactly one stable origin, parent suspension, movement, assignment, child-first cleanup, and path-sensitive reactivation must verify before a second shape is considered. |
| `R-NESTED-OWNER-BORROWS` | Borrows through shapes such as `expected<owner, E>.value()` may be consumed in one full expression but cannot be retained or returned; callee-internal return-field transforms are not inferred. | proof | **post-v1** | No implementation row is scheduled. A new owner-dependency row must follow `M-OWN-02`/`M-LIFE-01` and preserve the complete source-place transform through semantics, HIR, and MIR. |
| `R-CUSTOM-LIFECYCLE` | Copy/move construction may be structurally generated, defaulted, or deleted. Custom copy/move bodies and manual destructor calls are rejected. | proof | **post-v1** | No custom-body row is scheduled. A new row requires `M-OWN-02` and `M-LIFE-01` and must prove constructor-wide partial initialization, active-drop transfer, assignment, failure, and exactly-once cleanup. |
| `R-MOVE-PLACES` | Explicit movement supports locals, by-value parameters, writable named fields, and checked owner projections. Globals, captures, borrowed fields, temporaries, and indexed places are rejected. | proof | **bounded-v1** | `M-OWN-02` closes constant-index movement and `L-CALL-01` owns explicit move capture. Globals and borrowed fields remain rejected for v1; any later form needs its own state owner. |
| `R-GLOBAL-OWNERSHIP` | Reference, borrowed-state, and unique-owner globals are rejected. Cleanup-owning globals are normatively unavailable but declared-cleanup value types currently expose a semantic trait hole; mutable ordinary globals exist only in the current single-thread profile. | proof/choice | **bounded-v1** | `M-LIFE-01` closes the recursive cleanup-owning global/static hole. `C-GLOBAL-01` then defines the adopted concurrent-profile rule. Broader process-lifetime ownership requires a new row after global initialization, shutdown, failure, and foreign-thread participation are explicit. |
| `R-SHARED-OWNERSHIP` | Shared and weak owners are absent; unique ownership is the only public smart-owner model. | library | **close-v1** | `L-OWN-01`; ship shared and weak observation together with exact copy/move/drop, cycle limitations, allocation failure, and single-threaded baseline tests. |
| `R-RAW-POINTERS` | Raw pointers are one-level, nullable, non-owning values. No array decay, pointer/reference nesting, casts, typed `void*` conversion, ordering, owner inference, `release`, or address-to-owner construction exists. | safety/simplicity | **keep-v1** | Current raw-pointer contract. Selected C ABI extensions are post-1.0 under `S-FFI-02`; every additional operation needs explicit provenance, aliasing, lifetime, and unsafe obligations. |
| `R-ALLOCATOR-MODEL` | Compiler-private checked storage exists, but public allocation has no allocator, provenance, zero-size, alignment, initialization, placement, destruction, or recoverable-failure contract. | choice | **adopt-v1** | `S-ALLOC-01` produces the pre-1.0 design after layout/failure/place/drop prerequisites. It starts from safe typed storage and arenas, not source `new`/`delete`. |
| `R-PUBLIC-ALLOCATORS` | Applications cannot implement allocator objects, arenas, pools, raw deallocation, or allocator-aware containers through a stable public capability. | library | **post-v1** | `S-ALLOC-02` then `S-ALLOC-03`. A real arena/pool must prove provenance, initialization, failure, cleanup, and container propagation before the API generalizes. Source-level `new`/`delete` remain outside v1. |

## Generics, Callables, Values, And Operators

| ID | Current restriction or gap | Class | 1.0 disposition | Owner and reconsideration evidence |
| --- | --- | --- | --- | --- |
| `R-CONCEPTS` | User-defined unary conjunction concepts are implemented. Disjunction, negation, expression requirements, multiple parameters, `requires`, specialization, and constraint-based overload ranking are absent; callable/range/hash capabilities are incomplete. | safety/simplicity | **bounded-v1** | D-CALL-01's accepted [callable contract](callable-ownership-and-escape.md) defines exact signatures plus read/mut/once capability without adding ranking. `L-CALL-01` and `L-RANGE-04` implement the v1 callable/range minimum. Hash capability waits for a demonstrated `L-CONT-02` client. `requires`, specialization, and ranking remain post-1.0 and need a new proposal. |
| `R-VALUE-GENERICS` | Class/struct value generics are immutable `uint64_t` values with literal/parameter arguments. Functions, other value types, expressions, defaults, and packs are rejected. | proof | **post-v1** | `L-CONST-01` may add one demonstrated bounded family after stable identity; unrestricted metaprogramming and specialization are not reconsideration evidence. |
| `R-CONSTEXPR` | Constant evaluation is scalar and bounded. Generic/instance execution, aggregate values, references, arrays, allocation, runtime/native calls, and `static_assert` are rejected. | proof | **post-v1** | `L-CONST-01` requires a concrete library client and constexpr/runtime parity. `static_assert` must use the same evaluator and source diagnostics, never native C++ evaluation. |
| `R-CALLABLES` | Lambdas use explicit typed parameters/results and immutable copy snapshots. Capture defaults, reference/init/move capture, mutable closures, arbitrary results, callable references, field/global storage, and general escape are rejected. | proof | **bounded-v1** | D-CALL-01 now defines one exact concrete identity, signature, read/mut/once capability, lifecycle, and confined/owned escape model in the accepted [callable contract](callable-ownership-and-escape.md). `L-CALL-01` implements only the owned callable/move-capture minimum needed by v1 algorithms. General type erasure, reference capture, recursive closures, global storage, and unconstrained escape require a new post-1.0 row. |
| `R-FUNCTION-VALUES` | Function names must normally be called; ordinary function items and exact function-pointer values are not first-class. | lowering | **post-v1** | The accepted [callable contract](callable-ownership-and-escape.md) reserves one GTI-owned exact function-item form. `S-CALL-01` implements it only for a demonstrated non-capturing C callback boundary. General function values need a separate callable client and row. |
| `R-SUM-TYPES` | Scoped enums have no payload and GTI has no general closed sum/pattern-match construct; `expected` is a dedicated result form. | choice | **post-v1** | `L-SUM-01`; payload construction, exhaustive move/borrow matching, partial initialization, drop, generic payloads, and deterministic layout must land together in bounded families. C unions are not a substitute. |
| `R-INTEGER-MODES` | Built-in integer arithmetic remains checked by default and there is no explicit wrapping, saturating, or checked-result operation family. | library | **close-v1** | `L-NUM-01`; exhaustive domains, constexpr/runtime/O0/O3 parity, and optimizer evidence must ship without making `unsafe` disable checks. |
| `R-BINARY64` | `float` is exact IEEE-754 binary32; no binary64 type or width-selecting literal exists. Infinity and NaN have no literal spelling. | lowering | **close-v1** | `L-FLOAT-01` applies the binary32 contract to binary64 with deliberate spelling and exact APFloat-backed evaluation. `L-VALUE-01`/`L-CONST-01` may expose special values as library constants; special literals are not required. |
| `R-WIDE-INTEGERS` | Integer domains stop at 64 bits and literal magnitude is bounded by `uint64_t`. | choice | **post-v1** | No change row is scheduled. A new numeric row must demonstrate a systems/library client and specify literals, conversions, ABI status, constexpr, optimizer, and backend representation. |
| `R-OPERATORS` | User operators are exact member-only forms from a small allowlist; arithmetic operators, free lookup, ADL, rewritten candidates, postfix customization, and conversion operators are absent. | safety/simplicity | **post-v1** | `L-OP-01` may add one exact member/capability family after a demonstrated domain-type client. ADL, conversion ranking, and C++ rewrite rules are not accepted evidence. |
| `R-ERROR-PROPAGATION` | `expected` propagation is explicit; there is no `?`, `try`, implicit conversion, or exception-based propagation. | choice | **post-v1** | `L-ERR-01` after ordered full expressions and drop authority. It must preserve exact success/error types and all early-return cleanup. Exceptions remain excluded by `R-EXCEPTIONS`. |

## Layout, Native Interoperation, And Execution Environments

| ID | Current restriction or gap | Class | 1.0 disposition | Owner and reconsideration evidence |
| --- | --- | --- | --- | --- |
| `R-LAYOUT-QUERIES` | Source has no `sizeof`/`alignof`, and ordinary class/virtual/native layout is not a GTI fact. | choice | **close-v1** | `S-LAYOUT-01` and `S-LAYOUT-02`; the v1 slice covers primitives, raw pointers, fixed arrays, and explicitly layout-stable records only. Values are frontend constants checked against target probes. |
| `R-LAYOUT-CONTROL` | `alignas`, packed records, bit-fields, unions, and stable layout for ordinary classes are absent. | safety/simplicity | **post-v1** | No general layout-control row is scheduled. `S-ABI-01` may justify only the bounded native-record subset; MMIO/packing need separate alignment and access semantics. |
| `R-NATIVE-RECORDS` | C linkage cannot pass user records, arrays, enums, bool/char, owners, references, or aggregate results; only the dedicated non-retained string-view record exists. | choice | **post-v1** | `S-ABI-01` then `S-ABI-02`; a C oracle must agree on source opt-in, fields, padding, alignment, target dependence, and ownership exclusions. |
| `R-C-CALLBACKS` | GTI cannot expose function pointers/callback thunks, retained userdata, or native-thread entry. | lowering | **post-v1** | D-CALL-01's accepted [callable contract](callable-ownership-and-escape.md) supplies exact function-item/callable identity without defining an ABI. `S-CALL-01` owns the same-thread adapter and lifetime proof. Foreign-thread entry additionally requires the adopted concurrency capability/runtime rows. |
| `R-C-ABI-FAMILIES` | Pointer-to-pointer out parameters, opaque ownership transfer, arrays, casts, varargs, native variables, alternate calling conventions, and C++ linkage are absent. | choice | **post-v1** | `S-FFI-02` adds one demonstrated family at a time. Varargs, unions, bit-fields, and packing need separate proposals; `printf` alone is insufficient evidence. |
| `R-FREESTANDING` | The language/runtime assumes hosted allocation and output; no freestanding prelude or required-service profile exists. | choice | **post-v1** | `S-FREE-01`; enumerate every required service and prove one installed smoke or an early target-capability diagnostic before claiming a profile. |
| `R-STABLE-ABI` | Whole-program compilation, counter-shaped generated names, no stable GTI ABI, and no binary modules prevent separately distributed GTI objects. | choice | **post-v1** | `E-SYM-01`, `E-INST-01`, then `E-ABI-01` after layout, package identity, concrete emission, and compatibility. C++ ABI artifacts never become the GTI ABI. |

## Ranges And Standard-Library Completion

| ID | Current restriction or gap | Class | 1.0 disposition | Owner and reconsideration evidence |
| --- | --- | --- | --- | --- |
| `R-RANGE-LOANS` | Fixed arrays and owned temporaries do not yet have complete range lowering; mutable owner-tied iteration and per-element invalidation loans are unavailable. | proof | **close-v1** | `L-RANGE-01` and `L-RANGE-02`; fixed-array/value/read-only/mutable cases must prove increment, `continue`, `break`, nesting, invalidation, owner move/drop, and MIR cleanup. |
| `R-DYNAMIC-VIEWS` | Dynamic `string_view`, `span`, mutable views, and general owner-dependent views are absent; current string views reference static literal storage. | proof | **close-v1** | `L-RANGE-03` ships a focused single-owner span/view and dynamic string view. Multi-owner, nested, and general mutable stored graphs remain under the post-v1 borrow rows. |
| `R-CONTAINER-SURFACE` | Vector/string lack the accepted insertion/erasure and complete invalidation surface; vector is move-only, has no allocator policy, and does not promise C++ API parity. | library | **close-v1** | `L-CONT-01` completes the reviewed v1 surface. Implicit allocating vector copies and allocator customization are not v1 requirements; duplication may remain explicit. |
| `R-OPTIONAL-UTILITIES` | Optional, pair, foundational comparison/swap/limits utilities, and complete expected observers are incomplete. | library | **close-v1** | `L-VALUE-01`; public names remain ordinary GTI over checked storage with move-only, empty/engaged, assignment, destruction, and failure evidence. General payload sums remain `R-SUM-TYPES`. |
| `R-RECOVERABLE-ALLOCATION` | `make_unique` is type-level infallible and maps exhaustion to Execution §4.10's `GTI-R0011`, but there is no complete `try_make_unique`/shared allocation family. | library | **close-v1** | `D-FAIL-01` is done; `L-OWN-01` must expose the selected `expected` allocation path without raw ownership construction. General allocator customization remains post-v1. |
| `R-TEXT-FORMATTING` | Source/text encoding policy, dynamic owning/view conversion, numeric parsing/formatting, buffered/structured I/O, and stderr are incomplete. | library | **close-v1** | `L-TEXT-01` owns source/text policy and formatting/parsing; `L-HOST-01` owns stderr and host streams. No public native buffer lifetime may leak. |
| `R-HOST-SERVICES` | File write/seek, filesystem, time, randomness, environment, stderr, connected networking, traffic buffers, and portable target capability diagnostics are incomplete. | library | **bounded-v1** | `L-HOST-01` ships the v1 file/time/random/environment/stderr minimum one family at a time. Broader filesystem traversal/watch, connected networking, and traffic buffers remain post-1.0 and additionally require demonstrated clients or accepted native-record/buffer contracts. |
| `R-ASSOCIATIVE-CONTAINERS` | No hash/tree associative container or exact hasher/range/allocation policy exists. | library | **post-v1** | `L-CONT-02` after the v1 container minimum and accepted allocator design. A client and benchmark must justify the first container and its deterministic-iteration contract. |

## Audit And Specification Traceability

The third-party audit predates several implemented improvements. In
particular, unary source concepts now exist, so audit §4.12 maps to the
remaining bounded concept row rather than to “no user concepts.” Binary32
semantics were already closed by the audited baseline; the remaining float gap
is binary64.

| External language-audit finding | Ledger coverage |
| --- | --- |
| §4.1 concurrency and memory model | `R-MEMORY-MODEL`, `R-CONCURRENCY-API` |
| §4.2 layout control and queries | `R-LAYOUT-QUERIES`, `R-LAYOUT-CONTROL`, `R-NATIVE-RECORDS` |
| §4.3 public allocator model | `R-ALLOCATOR-MODEL`, `R-PUBLIC-ALLOCATORS`, `R-RECOVERABLE-ALLOCATION` |
| §4.4 broad C interoperation | `R-NATIVE-RECORDS`, `R-C-CALLBACKS`, `R-C-ABI-FAMILIES` |
| §4.5 escaping callables/function pointers | `R-CALLABLES`, `R-FUNCTION-VALUES`, `R-C-CALLBACKS` |
| §4.6 sum types/pattern matching | `R-SUM-TYPES` |
| §4.7 arithmetic escape hatch | `R-INTEGER-MODES` |
| §4.8 broader operators | `R-OPERATORS` |
| §4.9 second float width and special values | `R-BINARY64` |
| §4.10 mutable iteration | `R-RANGE-LOANS`, `R-MUTABLE-STORED-BORROWS` |
| §4.11 error propagation | `R-ERROR-PROPAGATION`, `R-EXCEPTIONS` |
| §4.12 concepts, value generics, custom lifecycle, statics, references, compile-time assertions, and wider integers | `R-CONCEPTS`, `R-VALUE-GENERICS`, `R-CUSTOM-LIFECYCLE`, `R-BLOCK-STATIC`, `R-READ-BORROW-CARRIERS`, `R-MUTABLE-STORED-BORROWS`, `R-GLOBAL-OWNERSHIP`, `R-CONSTEXPR`, `R-WIDE-INTEGERS` |
| §5.2 backend-visible restrictions | `R-EXEC-ORDER`, `R-TEMP-DROP`, `R-BACKEND-AUTHORITY` |
| §5.3 process failure contract | `R-FAIL` |
| §5.4 modules, separate compilation, freestanding | `R-INCLUDES`, `R-STABLE-ABI`, `R-FREESTANDING` |

| Original alignment area | Ledger coverage |
| --- | --- |
| Exact calls | `R-EXACT-CALLS` |
| Reference and move spelling | `R-REFERENCE-SPELLING`, `R-MOVE-SPELLING` |
| Result use | `R-RESULT-USE` |
| Operators | `R-OPERATORS` |
| Interfaces | `R-INHERITANCE` |
| Construction braces | `R-DIRECT-CONSTRUCTION` |
| Includes | `R-INCLUDES`, `R-STABLE-ABI` |
| Generic capabilities | `R-CONCEPTS`, `R-VALUE-GENERICS`, `R-CONSTEXPR` |
| Borrowed values | `R-PLACE-STATE`, `R-READ-BORROW-CARRIERS`, `R-BORROWED-MERGES`, `R-MUTABLE-STORED-BORROWS`, `R-NESTED-OWNER-BORROWS`, `R-RANGE-LOANS`, `R-DYNAMIC-VIEWS` |
| Low-level control | `R-RAW-POINTERS`, `R-ALLOCATOR-MODEL`, `R-PUBLIC-ALLOCATORS` |
| Internal capabilities | `R-PRIVATE-CAPABILITY` |
| Backend completeness | `R-EXEC-ORDER`, `R-TEMP-DROP`, `R-FAIL`, `R-BACKEND-AUTHORITY` |

| Explicit language-specification gap | Ledger coverage |
| --- | --- |
| Scope §1.1 freestanding, stable ABI, binary modules, and separate compilation; §1.6 compatibility; §1.7 backend independence | `R-FREESTANDING`, `R-STABLE-ABI`, `R-INCLUDES`, `R-COMPATIBILITY`, `R-BACKEND-AUTHORITY` |
| Lexical §2.2 source text and §2.5 documentation comments | `R-SOURCE-TEXT`, `R-DOC-COMMENTS` |
| Static semantics §3.12 capabilities, borrowed aggregates, places, callables, constexpr, FFI, and layout | `R-CONCEPTS`, `R-READ-BORROW-CARRIERS`, `R-BORROWED-MERGES`, `R-MUTABLE-STORED-BORROWS`, `R-NESTED-OWNER-BORROWS`, `R-PLACE-STATE`, `R-MOVE-PLACES`, `R-CALLABLES`, `R-FUNCTION-VALUES`, `R-CONSTEXPR`, `R-VALUE-GENERICS`, `R-NATIVE-RECORDS`, `R-C-CALLBACKS`, `R-C-ABI-FAMILIES`, `R-LAYOUT-QUERIES` |
| Execution §4.2 order, §4.9 concurrency, §4.10 failure, and §4.11 temporary/cleanup | `R-EXEC-ORDER`, `R-MEMORY-MODEL`, `R-FAIL`, `R-TEMP-DROP` |
| Programs §6.2 target model and §6.5 native ABI | `R-TARGET`, `R-NATIVE-RECORDS`, `R-C-CALLBACKS`, `R-C-ABI-FAMILIES` |
| Focused ownership, range, raw-pointer, native-interop, I/O, and TCP limitation statements | `R-PLACE-STATE` through `R-PUBLIC-ALLOCATORS`, `R-RANGE-LOANS`, `R-DYNAMIC-VIEWS`, `R-RAW-POINTERS`, `R-NATIVE-RECORDS`, `R-C-CALLBACKS`, `R-C-ABI-FAMILIES`, `R-TEXT-FORMATTING`, `R-HOST-SERVICES` |
| Standard library §7.1 private visibility and §7.7 component omissions | `R-PRIVATE-CAPABILITY`, `R-SHARED-OWNERSHIP`, `R-SUM-TYPES`, `R-DYNAMIC-VIEWS`, `R-CONTAINER-SURFACE`, `R-CONCEPTS`, `R-TEXT-FORMATTING`, `R-HOST-SERVICES`, `R-RECOVERABLE-ALLOCATION`, `R-ALLOCATOR-MODEL`, `R-PUBLIC-ALLOCATORS`, `R-NATIVE-RECORDS`, `R-C-CALLBACKS`, `R-C-ABI-FAMILIES` |
| Architecture audit §7.1 order, §7.2 failure identity, and §7.3 documentation retention | `R-EXEC-ORDER`, `R-FAIL`, `R-DOC-COMMENTS` |

## Maintenance Rule

When an owner row completes, update this ledger in the same change as the
canonical language/architecture documents and tests. A restriction leaves the
ledger only when its replacement is current, backend-independent behavior. A
proposal or parser production alone is not completion.

Every new restriction must state its class, horizon, owner, and evidence gate.
Do not write only “deferred,” “future work,” or “not yet supported.” If no row
owns a proposed relaxation, the current rule remains and a new prompt-sized row
is required before implementation.
