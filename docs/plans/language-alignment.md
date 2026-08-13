# Language Restriction Ledger

> **Plan status:** D-LANG-01 complete. Maintained, non-canonical restriction
> and readiness ledger. Current language meaning remains under
> [`docs/language/`](../language/index.md).

Baseline: GTI 0.105.0.

This ledger classifies the restrictions called out by the third-party
[language audit](../third-party-audit/language-audit.md), the original language
alignment discussion, every explicit specification gap in `docs/language/`,
and the restrictions that currently protect GTI from its transitional C++
backend. It does not change syntax or semantics. A row describes the current
boundary, why that boundary exists, its readiness role, and the plan owner or
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
| **choice** | GTI must first adopt a semantic, ABI, compatibility, or policy decision. |

The readiness role is one of:

| Role | Requirement |
| --- | --- |
| **durable-rule** | The restriction is an intentional long-term GTI safety or simplicity choice. |
| **systems-ready** | The gap must close before GTI can credibly declare full systems-language readiness. |
| **bounded-first** | Deliver the smallest coherent client-driven form; broader forms remain gated by evidence. |
| **design-first** | Adopt the cross-cutting contract before its executable client, then implement it with that client. |
| **later-breadth** | Useful breadth not required by the current readiness workloads; it may move earlier when a concrete client changes the evidence. |

An owner cell names the operational row or dependency chain from
[`implementation-sequence.md`](implementation-sequence.md). Prerequisites are
not permission to expand a row's scope. “New row required” means no
implementation is scheduled; the restriction cannot be relaxed by folding it
into an adjacent feature.

## Systems-Readiness Decisions

Under [ADR 012](../decisions/012-outcome-first-systems-readiness.md), 1.0 is a
soft, revisable readiness goal rather than a scheduling boundary. The ledger
therefore fixes these current priority and design questions:

1. The evaluation/full-expression, defined-failure, and Edition 1
   compatibility contracts are complete. Their temporary/drop/failure
   lowering plus source-text remain systems-readiness work. The target
   data-layout contract and private capability enforcement are complete, and
   the concurrency boundary is adopted in ADR 008.
2. The adopted transfer/share type facts and concurrent-global policy are
   represented. A bounded public profile with owned joined tasks/threads, SC
   atomics, mutex-guard access, and contained failure is systems-readiness work;
   detach, weak-order breadth, and advanced reclamation are later breadth.
3. The GTI-owned target data-layout contract, bounded `sizeof`/`alignof`, and
   explicit wrapping/saturating/checked-result fixed-width
   add/subtract/multiply are
   implemented. IEEE-754 binary64 is implemented with exact frontend and
   native parity. Result-returning division/remainder/shift families remain
   client-gated breadth.
4. The accepted range, mutable-iteration, single-owner view, owned-callable,
   shared/weak-owner, optional, text, container, and hosted-service minimums
   remain systems-readiness library work.
5. Bounded native records/callbacks, a public arena/pool allocation path,
   payload enums, cleanup-correct propagation, exact domain operator families,
   and one associative container are systems-readiness work. Freestanding
   execution and generalized borrow graphs remain later breadth unless a
   readiness client proves otherwise.
6. Exact calls, read-only `T&`, writable `mut T&`, consuming `std::move`,
   mandatory result use, exact direct construction, non-textual direct
   includes, and the absence of safe source `new`/`delete` are durable rules.

`I-CAP-01` completed the former `R-PRIVATE-CAPABILITY` restriction. Trusted
source roles, exact prelude declaration identity, `GTI-S2058`, private
signature publication, and shared LSP filtering now enforce the current
boundary, so that closed gap no longer remains a ledger row.

## Foundational Semantics And Backend Independence

| ID | Current restriction or gap | Class | Readiness role | Owner and reconsideration evidence |
| --- | --- | --- | --- | --- |
| `R-EXEC-ORDER` | ADR 010 and Execution Section 4.2 define strict left-to-right operands/arguments, target-first assignment, initialization, full-expression cleanup, and program-wide initialization. The executable schedule is not yet authoritative, so a transient borrow and overlapping mutation remain conservatively rejected in one call regardless of written order. | lowering | **systems-ready** | `D-EXEC-01` is done. `M-LIFE-01` and `M-EXEC-01` provide obligations and ordered MIR; `M-BACK-01/02` migrate affected production families before each conservative restriction is narrowed. |
| `R-TEMP-DROP` | The obligation and reverse-successful-cleanup order is specified, but every temporary lifetime, partial-construction state, compound-expression cleanup, and checked-failure cleanup is not yet authoritative in executable MIR. | lowering | **systems-ready** | Execution Sections 4.2 and 4.10 fix normal/failure cleanup semantics and `M-OWN-02` now preserves bounded partial-array state; `M-LIFE-01`, `M-EXEC-01`, and `M-FAIL-01` provide the remaining drop/rollback/order facts, verifier mutations, and O0/O3 exactly-once traces. |
| `R-FAIL` | Execution §4.10 now defines categories, artifact-qualified sites, status/reporting, cleanup, observation, embedding, allocation, and worker containment, but the emitter still aborts without those semantics and native expected observers still escape them. | lowering | **systems-ready** | `D-FAIL-01` and `I-CAP-01` are done; `M-LIFE-01`, the relevant `M-EXEC-01` slices, and co-delivered `M-FAIL-01`/`Q-FAIL-01` implement the remaining IR/runtime substrate; `M-BACK-02` then migrates every closed call-graph family and removes native helper behavior. |
| `R-MEMORY-MODEL` | ADR 008 and Execution §4.9 define safe data-race freedom, transfer/share facts, an explicit concurrent profile, SC atomic and owned automatic-join first boundaries, happens-before, globals, worker failure, and native entry. C-TYPE-01 and C-GLOBAL-01 implement structural facts, public concepts, nominal policy, explicit profile selection, and concurrent global/static enforcement while public concurrency remains unavailable. | choice | **durable-rule** | `D-MEM-02`, `I-CAP-01`, `C-TYPE-01`, and `C-GLOBAL-01` are complete. Any incompatible memory-model change requires the compatibility mechanism; executable concurrency retains its correctness prerequisites. |
| `R-CONCURRENCY-API` | No public atomic, thread, mutex, detach, scoped-thread borrow, or native-thread callback API exists. | library | **systems-ready** | `C-MIR-01` through `C-CONFORM-01` implement ADR 008 in dependency order. The first public profile is owned-only, SC, automatic-join, and detach-free; broader forms retain their named prerequisites. |
| `R-SOURCE-TEXT` | Source encoding, BOM handling, newline normalization, Unicode identifiers, and normalization are not normative. | choice | **systems-ready** | The independent source-text sub-slice of `L-TEXT-01`; lexer, source offsets, formatter, Tree-sitter, LSP, invalid-byte cases, and installed-toolchain tests must share one byte/Unicode contract. |
| `R-DOC-COMMENTS` | Documentation comments have no declaration attachment; the lexer, formatter, Tree-sitter, and LSP currently recover comment information separately. | lowering | **systems-ready** | `T-LSP-01`; retain declaration-owned Markdown and extents once, then test hover, completion, formatting, parsing, and generated docs. |
| `R-DEPRECATION` | Declarations cannot carry a source-owned deprecation message that produces a use-site diagnostic and tooling metadata. | lowering | **systems-ready** | ADR 011 fixes the use-site-only migration and Edition 1 retention policy. After `T-LSP-01`, `Q-DEPRECATION-01` implements the bounded attribute, declaration metadata, diagnostics, hover, completion, formatter, and Tree-sitter coverage. |
| `R-TARGET` | `os`/`vendor`/`arch` have one exact vocabulary; malformed and unsupported triple dimensions are distinguished; every accepted arm64/x86_64 macOS/Linux/Windows triple selects one GTI-owned 64-bit little-endian scalar layout checked by installed native probes. The CLI still selects the host and does not promise a cross toolchain. | choice | **durable-rule** | `S-LAYOUT-01` is complete. `TargetDataLayout` remains backend-neutral and LLVM-free; `GTI-S2062` rejects unsupported selected layouts before source target selection or lowering. New architectures, operating systems, endianness, or pointer widths require their own normative layout and installed-toolchain evidence. |
| `R-COMPATIBILITY` | Documented 0.x minor releases may change draft meaning, while patches do not intentionally break source. GTI 1.0 freezes Edition 1; omission permanently selects Edition 1 once selectors exist, incompatible successful meaning requires opt-in, and unknown selectors fail before analysis. No selector is accepted while only the pre-1.0 draft exists. | choice | **durable-rule** | `D-COMPAT-01` is complete in ADR 011 and Scope Section 1.6. A second edition requires a new bounded selector row covering direct/project/LSP/metadata paths and cross-version fixtures; it cannot be inferred from compiler, target, backend, or C++ versions. |
| `R-BACKEND-AUTHORITY` | The C++ backend still emits complete bodies from AST/semantic/HIR data, and some accepted evaluation/lifetime behavior is not yet executable MIR authority. | lowering | **systems-ready** | `M-EXEC-01` and `M-BACK-01` start the migration; `M-BACK-02` completes it by closed call-graph family. Systems readiness requires every accepted observable order, failure, lifetime, and cleanup rule to be represented independently of C++. |

## Durable Surface Choices And Later Breadth

| ID | Current restriction or gap | Class | Readiness role | Owner and reconsideration evidence |
| --- | --- | --- | --- | --- |
| `R-EXACT-CALLS` | Calls use one exact parameter match after substitution, apart from bounded null/pointee qualification. Safe integer widening remains available only in documented value-assignment contexts such as initialization and return; calls have no conversion ranking, ADL, return-context inference, or concrete-over-generic preference. | safety/simplicity | **durable-rule** | Current static semantics. Any additive conversion set needs a new row proving unique selection without ranking; conversion-ranked overloads remain later breadth. |
| `R-REFERENCE-SPELLING` | `T&` is a non-null read-only loan and `mut T&` is writable; GTI does not import C++ reference or cv meanings. | safety/simplicity | **durable-rule** | Current ownership contract and ADR 011; diagnostics/documentation remain the migration mechanism. |
| `R-MOVE-SPELLING` | `std::move(place)` consumes the tracked source even when it is copyable; later use is invalid until permitted reinitialization. | safety/simplicity | **durable-rule** | Current ownership contract. A spelling change would be compatibility work and is not scheduled. |
| `R-RESULT-USE` | Every non-void call result must be consumed or explicitly discarded at the call site. There is no declaration-side implicit-discard policy. | safety/simplicity | **durable-rule** | Current static semantics. A future declaration-side policy needs a new row proving it cannot hide `expected` or ownership failures. |
| `R-DIRECT-CONSTRUCTION` | `Type value{args}` is exact direct construction, not aggregate/list conversion, initializer-list preference, narrowing policy, or CTAD. | safety/simplicity | **durable-rule** | Current static semantics. Initializer-list and CTAD semantics are later breadth requiring a separate proposal. |
| `R-INHERITANCE` | One state-bearing public base plus stateless interfaces is permitted; diamonds, private/protected inheritance, multiple state-bearing bases, slicing, covariant returns, and generic virtual methods are rejected. | safety/simplicity | **durable-rule** | Current class/interface contract. A new row is required to demonstrate an ownership, layout, dispatch, and cleanup need before any relaxation. |
| `R-INCLUDES` | `include` is load-once, direct-visibility, non-textual dependency loading. It does not re-export, preprocess text, define macros, or name binary modules. | safety/simplicity | **durable-rule** | ADR 011 freezes the Edition 1 spelling and forbids later reinterpretation. Module vocabulary and binary distribution are later breadth under `E-ABI-01`; textual macros are not planned. |
| `R-INFERENCE` | `auto` is confined to initialized locals and range elements; it does not infer API signatures, globals, fields, arrays, references from plain `auto`, or untyped braced values. | safety/simplicity | **durable-rule** | Current grammar/static semantics. A new row is required for any API-visible inference proposal. |
| `R-STRUCTURED-BINDINGS` | Structured bindings are immutable flat projections of one hidden owned array/aggregate. Mutable/reference/nested/inherited patterns, loop declarations, and partial movement are rejected. | proof | **later-breadth** | No change row is scheduled. Reconsideration requires `M-OWN-02` and `M-LIFE-01` evidence plus a dedicated structured-binding row covering projection identity and partial drop. |
| `R-BLOCK-STATIC` | Namespace internal-linkage and class static storage exist; block-scope static and static members of generic classes are rejected. | choice | **later-breadth** | `C-GLOBAL-01` has settled profile-specific concurrency policy. A new row must still define once initialization, failure, destruction, generic identity, and target behavior. |
| `R-EXCEPTIONS` | GTI has no source exceptions, catch/resume, or native exception-ABI unwinding; recoverable APIs use `expected`. Defined failure uses compiler-managed non-resumable control-flow propagation solely to perform Execution §4.10 cleanup and reach a containment boundary. | safety/simplicity | **durable-rule** | `D-FAIL-01` is complete. Source handlers, resumption, or cross-ABI native unwinding require a new later-breadth design row. |

## Ownership, Places, Borrows, And Allocation

| ID | Current restriction or gap | Class | Readiness role | Owner and reconsideration evidence |
| --- | --- | --- | --- | --- |
| `R-PLACE-STATE` | Precise places cover roots, fields, bounded checked dereferences, and directly owned fixed-array elements selected by an in-range constant. Semantic state and MIR verification agree across moves, restoration, branches, and loop backedges. Dynamic-index movement, arbitrary provenance, lifetime epochs, and complete active-drop obligations remain unavailable. | proof | **bounded-first** | `M-OWN-01` and `M-OWN-02` are done. `M-LIFE-01` consumes the carried partial state for complete temporary/drop obligations; any dynamic-index or broader provenance precision needs a separate proof row. |
| `R-READ-BORROW-CARRIERS` | One direct read-only stored reference and one exact owner origin are supported. Multiple/nested/inherited origins, dependency-changing assignment, captured/global storage, and arbitrary borrowed aggregates are rejected. | proof | **bounded-first** | `L-RANGE-03` ships the focused single-owner view needed by the container workload. Broader graphs require a new row after the stable place/drop model; they are not implied by that view. |
| `R-BORROWED-MERGES` | A conditional expression can merge exact owned values but cannot select a borrowed result whose origin and endpoint differ by branch. | proof | **later-breadth** | No change row is scheduled. Reconsideration requires `M-OWN-01`, `M-OWN-02`, and `M-LIFE-01` evidence plus a dedicated row preserving the selected origin, loan state, and endpoint through semantics, HIR, and MIR. |
| `R-MUTABLE-STORED-BORROWS` | Mutable child loans are local and non-escaping. A field, guard, return, iterator, or other stored carrier cannot retain a mutable parent/child dependency. | proof | **bounded-first** | `M-OWN-03`; the first client is mutex-guard or mutable-iterator access with exactly one stable origin, parent suspension, movement, assignment, child-first cleanup, and path-sensitive reactivation. Broader graphs remain later breadth. |
| `R-NESTED-OWNER-BORROWS` | Borrows through shapes such as `expected<owner, E>.value()` may be consumed in one full expression but cannot be retained or returned; callee-internal return-field transforms are not inferred. | proof | **later-breadth** | No implementation row is scheduled. A new owner-dependency row must follow `M-OWN-02`/`M-LIFE-01` and preserve the complete source-place transform through semantics, HIR, and MIR. |
| `R-CUSTOM-LIFECYCLE` | Copy/move construction may be structurally generated, defaulted, or deleted. Custom copy/move bodies and manual destructor calls are rejected. | proof | **later-breadth** | No custom-body row is scheduled. A new row requires `M-OWN-02` and `M-LIFE-01` and must prove constructor-wide partial initialization, active-drop transfer, assignment, failure, and exactly-once cleanup. |
| `R-MOVE-PLACES` | Explicit movement supports locals, by-value parameters, writable named fields, checked owner projections, and in-range constant elements of directly owned fixed arrays. Globals, captures, borrowed fields, temporaries, and dynamic indexed places are rejected. | proof | **bounded-first** | `M-OWN-02` is complete and `L-CALL-01` owns explicit move capture. Any additional form needs a named client and state owner. |
| `R-GLOBAL-OWNERSHIP` | Reference, borrowed-state, and unique-owner globals are rejected. Cleanup-owning globals are normatively unavailable but declared-cleanup value types still expose a single-threaded semantic trait hole. Mutable ordinary globals remain available only under default single-threaded selection; the implemented concurrent profile requires immutable share-capable globals/statics, with synchronized mutation behind a future approved wrapper. | proof/choice | **bounded-first** | `C-GLOBAL-01` is complete and preserves existing single-threaded source while enforcing the selected concurrent policy. `M-LIFE-01` still closes the general recursive cleanup-owning global/static hole. Broader process-lifetime ownership requires a new row after global initialization, shutdown, failure, and foreign-thread participation are explicit. |
| `R-SHARED-OWNERSHIP` | Shared and weak owners are absent; unique ownership is the only public smart-owner model. | library | **systems-ready** | `L-OWN-01`; ship shared and weak observation together with exact copy/move/drop, cycle limitations, allocation failure, and single-threaded baseline tests. |
| `R-RAW-POINTERS` | Raw pointers are one-level, nullable, non-owning values. No array decay, pointer/reference nesting, casts, typed `void*` conversion, ordering, owner inference, `release`, or address-to-owner construction exists. | safety/simplicity | **durable-rule** | Current raw-pointer contract. Selected C ABI extensions are bounded-first under `S-FFI-02`; every additional operation needs explicit provenance, aliasing, lifetime, and unsafe obligations. |
| `R-ALLOCATOR-MODEL` | Compiler-private checked storage exists, but public allocation has no allocator, provenance, zero-size, alignment, initialization, placement, destruction, or recoverable-failure contract. | choice | **design-first** | `S-ALLOC-01` produces the design after layout/failure/place/drop prerequisites. It starts from safe typed storage and arenas, not source `new`/`delete`. |
| `R-PUBLIC-ALLOCATORS` | Applications cannot implement allocator objects, arenas, pools, raw deallocation, or allocator-aware containers through a stable public capability. | library | **systems-ready** | `S-ALLOC-02` then one demonstrated `S-ALLOC-03` integration. A real arena/pool must prove provenance, initialization, failure, cleanup, and container propagation before the API generalizes. Source-level `new`/`delete` are not required. |

## Generics, Callables, Values, And Operators

| ID | Current restriction or gap | Class | Readiness role | Owner and reconsideration evidence |
| --- | --- | --- | --- | --- |
| `R-CONCEPTS` | User-defined multi-parameter conjunction concepts and bounded trailing `requires` conjunctions are implemented. The current structural facts cover input-iterator dereference/increment, exact iterator/sentinel inequality, and exact accumulation referents. Disjunction, negation, general requires-expressions, arbitrary expression requirements, associated types, specialization, subsumption, and constraint-based overload ranking remain absent; callable/complete-range/hash capabilities are incomplete. | safety/simplicity | **bounded-first** | [ADR 009](../decisions/009-bounded-requires-contracts.md) keeps requirements validity-only and preserves exact overloads. D-CALL-01's accepted [callable contract](callable-ownership-and-escape.md) defines exact signatures plus read/mut/once capability without adding ranking. `L-CALL-01`, `L-RANGE-04`, and `L-CONT-02` supply the next client-driven capabilities. General expression requirements, specialization, subsumption, and ranking remain later breadth unless a client cannot be expressed by bounded exact relations. |
| `R-VALUE-GENERICS` | Class/struct value generics are immutable `uint64_t` values with literal/parameter arguments. Functions, other value types, expressions, defaults, and packs are rejected. | proof | **later-breadth** | `L-CONST-01` may add one demonstrated bounded family after stable identity; unrestricted metaprogramming and specialization are not reconsideration evidence. |
| `R-CONSTEXPR` | Constant evaluation is scalar and bounded. Generic/instance execution, aggregate values, references, arrays, allocation, runtime/native calls, and `static_assert` are rejected. | proof | **bounded-first** | `L-CONST-01` requires a concrete library or domain client and constexpr/runtime parity. `static_assert` must use the same evaluator and source diagnostics, never native C++ evaluation. |
| `R-CALLABLES` | Lambdas use explicit typed parameters/results and immutable copy snapshots. Capture defaults, reference/init/move capture, mutable closures, arbitrary results, callable references, field/global storage, and general escape are rejected. | proof | **systems-ready** | D-CALL-01 defines exact concrete identity, signature, read/mut/once capability, lifecycle, and confined/owned escape in the accepted [callable contract](callable-ownership-and-escape.md). `L-CALL-01` implements the owned callable/move-capture slice needed by algorithms, tasks, and callbacks. General type erasure, reference capture, recursive closures, and unconstrained escape remain later breadth. |
| `R-FUNCTION-VALUES` | Function names must normally be called; ordinary function items and exact function-pointer values are not first-class. | lowering | **systems-ready** | The accepted [callable contract](callable-ownership-and-escape.md) reserves one GTI-owned exact function-item form. `S-CALL-01` first implements it for the demonstrated non-capturing C callback boundary; other callable clients can extend it without introducing C++ function-pointer inference. |
| `R-SUM-TYPES` | Scoped enums have no payload and GTI has no general closed sum/pattern-match construct; `expected` is a dedicated result form. | choice | **systems-ready** | `L-SUM-01`; payload construction, exhaustive move/borrow matching, partial initialization, drop, generic payloads, and deterministic layout land in bounded families for the compiler-AST/protocol workload. C unions are not a substitute. |
| `R-INTEGER-MODES` | **Closed in 0.111.0:** built-in integer arithmetic remains checked by default. `<std/numeric>` provides explicit fixed-width wrapping, saturating, and `expected`-returning checked-result add/subtract/multiply with constexpr/runtime and optimizer evidence. | library | **closed** | `L-NUM-01` reuses ordinary `expected<T, std::arithmetic_errc>`, preserves existing operator failures, and never lets `unsafe` disable checks. Division/remainder/shift result families are later client-gated breadth, not a gap in the selected minimum. |
| `R-BINARY64` | **Closed in 0.110.0:** `double` is exact IEEE-754 binary64 and `d`/`D` selects it; unsuffixed decimals remain binary32. Mixed arithmetic promotes to binary64, widening is implicit, and narrowing is explicit. Infinity and NaN still have no literal spelling. | lowering | **closed** | `L-FLOAT-01` completed the exact APFloat-backed frontend, IR, optimizer, layout, tooling, and native matrix. `L-VALUE-01`/`L-CONST-01` may expose special values as library constants; special literals are not required. |
| `R-WIDE-INTEGERS` | Integer domains stop at 64 bits and literal magnitude is bounded by `uint64_t`. | choice | **later-breadth** | No change row is scheduled. A new numeric row must demonstrate a systems/library client and specify literals, conversions, ABI status, constexpr, optimizer, and backend representation. |
| `R-OPERATORS` | User operators are exact member-only forms from a small allowlist; arithmetic operators, free lookup, ADL, rewritten candidates, postfix customization, and conversion operators are absent. | safety/simplicity | **systems-ready** | `L-OP-01` adds exact member/capability families for demonstrated vector/matrix or domain-value clients. ADL, conversion ranking, and C++ rewrite rules remain excluded. |
| `R-ERROR-PROPAGATION` | `expected` propagation is explicit; there is no `?`, `try`, implicit conversion, or exception-based propagation. | choice | **systems-ready** | `L-ERR-01` follows ordered full expressions and drop authority for the fallible-pipeline workload. It must preserve exact success/error types and all early-return cleanup. Exceptions remain excluded by `R-EXCEPTIONS`. |

## Layout, Native Interoperation, And Execution Environments

| ID | Current restriction or gap | Class | Readiness role | Owner and reconsideration evidence |
| --- | --- | --- | --- | --- |
| `R-LAYOUT-QUERIES` | Reserved, type-only `sizeof(type)`/`alignof(type)` produce exact `uint64_t` frontend constants for primitives, one-level raw pointers, transparent aliases, recursive positive concrete fixed arrays, and valid passive `[[c_abi]]` records. Pointer layout is independent of pointee layout. ABI alignment is queryable; preferred alignment is not. Zero/symbolic extents, overflow, direct symbolic types, references, ordinary nominal aggregates, enums, and every other backend-dependent category are rejected before lowering. | choice | **closed** | `S-LAYOUT-01/02` and `S-ABI-01/02` are complete. Synthetic supported-target checks plus native and installed-library probes cover the GTI-owned facts. Expression operands, direct query expressions in array-extent grammar, and ordinary class layout remain outside the bounded contract. |
| `R-LAYOUT-CONTROL` | `alignas`, packed records, bit-fields, unions, and stable layout for ordinary classes are absent. | safety/simplicity | **bounded-first** | `S-ABI-01/02` completed the layout-stable passive native-record subset needed by real C bindings. General `alignas`, MMIO, packing, bit-fields, and unions remain later breadth with separate access semantics. |
| `R-NATIVE-RECORDS` | **Closed in the bounded native-record release:** `[[c_abi]] struct` opts a passive, non-owning fixed-width/nested-record/one-level-pointer field family into compiler-owned source-order layout and by-value or one-level-pointer `extern "C"` passage. Ordinary records, arrays, enums, bool/char, owners, references, and cleanup-owning values remain excluded. | choice | **closed** | `S-ABI-01/02` and ADR 013 are complete. A native C oracle proves size, alignment, offsets, nested records, by-value return/passage, and pointer mutation at O0/O3 and C++20/C++23. |
| `R-C-CALLBACKS` | GTI cannot expose function pointers/callback thunks, retained userdata, or native-thread entry. | lowering | **systems-ready** | D-CALL-01's accepted [callable contract](callable-ownership-and-escape.md) supplies exact function-item/callable identity without defining an ABI. `S-CALL-01` owns the same-thread adapter and lifetime proof. Foreign-thread entry additionally requires the adopted concurrency capability/runtime rows. |
| `R-C-ABI-FAMILIES` | Pointer-to-pointer out parameters, opaque ownership transfer, arrays, casts, varargs, native variables, alternate calling conventions, and C++ linkage are absent. | choice | **bounded-first** | `S-FFI-02` adds the opaque-handle and selected out-parameter families required by a real binding. Varargs, unions, bit-fields, packing, and general C++ linkage remain later breadth. |
| `R-FREESTANDING` | The language/runtime assumes hosted allocation and output; no freestanding prelude or required-service profile exists. | choice | **later-breadth** | `S-FREE-01`; enumerate every required service and prove one installed smoke or an early target-capability diagnostic before claiming a profile. |
| `R-STABLE-ABI` | Whole-program compilation, counter-shaped generated names, no stable GTI ABI, and no binary modules prevent separately distributed GTI objects. | choice | **later-breadth** | `E-SYM-01`, `E-INST-01`, then `E-ABI-01` after layout, package identity, concrete emission, and compatibility. C++ ABI artifacts never become the GTI ABI. |

## Ranges And Standard-Library Completion

| ID | Current restriction or gap | Class | Readiness role | Owner and reconsideration evidence |
| --- | --- | --- | --- | --- |
| `R-RANGE-LOANS` | Fixed arrays and owned temporaries do not yet have complete range lowering; mutable owner-tied iteration and per-element invalidation loans are unavailable. | proof | **systems-ready** | `L-RANGE-01` and `L-RANGE-02`; fixed-array/value/read-only/mutable cases must prove increment, `continue`, `break`, nesting, invalidation, owner move/drop, and MIR cleanup. |
| `R-DYNAMIC-VIEWS` | Dynamic `string_view`, `span`, mutable views, and general owner-dependent views are absent; current string views reference static literal storage. | proof | **systems-ready** | `L-RANGE-03` ships a focused single-owner span/view and dynamic string view. Multi-owner, nested, and general mutable stored graphs remain later breadth. |
| `R-CONTAINER-SURFACE` | Vector/string lack the accepted insertion/erasure and complete invalidation surface; vector is move-only, has no allocator policy, and does not promise C++ API parity. | library | **systems-ready** | `L-CONT-01` completes the reviewed surface needed by the renderer/game-loop workload. Implicit allocating vector copies are not required; duplication may remain explicit. |
| `R-OPTIONAL-UTILITIES` | Optional, pair, foundational comparison/swap/limits utilities, and complete expected observers are incomplete. | library | **systems-ready** | `L-VALUE-01`; public names remain ordinary GTI over checked storage with move-only, empty/engaged, assignment, destruction, and failure evidence. General payload sums remain `R-SUM-TYPES`. |
| `R-RECOVERABLE-ALLOCATION` | `make_unique` is type-level infallible and maps exhaustion to Execution §4.10's `GTI-R0011`, but there is no complete `try_make_unique`/shared allocation family. | library | **systems-ready** | `D-FAIL-01` is done; `L-OWN-01` must expose the selected `expected` allocation path without raw ownership construction. The bounded public allocator path is owned by `S-ALLOC-02`. |
| `R-TEXT-FORMATTING` | Source/text encoding policy, dynamic owning/view conversion, numeric parsing/formatting, buffered/structured I/O, and stderr are incomplete. | library | **systems-ready** | `L-TEXT-01` owns source/text policy and formatting/parsing; `L-HOST-01` owns stderr and host streams. No public native buffer lifetime may leak. |
| `R-HOST-SERVICES` | File write/seek, filesystem, time, randomness, environment, stderr, connected networking, traffic buffers, and portable target capability diagnostics are incomplete. | library | **bounded-first** | `L-HOST-01` ships the file/time/random/environment/stderr minimum one family at a time. Broader traversal/watch, networking, and traffic buffers require demonstrated clients or accepted native-record/buffer contracts. |
| `R-ASSOCIATIVE-CONTAINERS` | No hash/tree associative container or exact hasher/range/allocation policy exists. | library | **systems-ready** | `L-CONT-02` follows the foundational container surface and allocator contract. One real compiler/game workload and benchmark justify the first container and its deterministic-iteration contract; additional container families remain client-driven. |

## Audit And Specification Traceability

The third-party audit predates several implemented improvements. In
particular, multi-parameter source concepts and bounded trailing requirements
now exist, so audit §4.12 maps to the remaining capability and general
expression-requirement gaps rather than to “no user concepts.” Binary32
semantics were already closed by the audited baseline; the remaining float gap
is binary64. Audit §4.2's missing-query observation is likewise historical:
the bounded `R-LAYOUT-QUERIES` surface and passive native records are
implemented, while general layout control and broader C ABI families remain
open under their separate rows.

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
| Backend completeness | `R-EXEC-ORDER`, `R-TEMP-DROP`, `R-FAIL`, `R-BACKEND-AUTHORITY` |

| Explicit language-specification gap | Ledger coverage |
| --- | --- |
| Scope §1.1 freestanding, stable ABI, binary modules, and separate compilation; §1.6 compatibility; §1.7 backend independence | `R-FREESTANDING`, `R-STABLE-ABI`, `R-INCLUDES`, `R-COMPATIBILITY`, `R-BACKEND-AUTHORITY` |
| Lexical §2.2 source text and §2.5 documentation comments | `R-SOURCE-TEXT`, `R-DOC-COMMENTS` |
| Static semantics §3.12 capabilities, borrowed aggregates, places, callables, constexpr, FFI, and layout | `R-CONCEPTS`, `R-READ-BORROW-CARRIERS`, `R-BORROWED-MERGES`, `R-MUTABLE-STORED-BORROWS`, `R-NESTED-OWNER-BORROWS`, `R-PLACE-STATE`, `R-MOVE-PLACES`, `R-CALLABLES`, `R-FUNCTION-VALUES`, `R-CONSTEXPR`, `R-VALUE-GENERICS`, `R-NATIVE-RECORDS`, `R-C-CALLBACKS`, `R-C-ABI-FAMILIES`, `R-LAYOUT-QUERIES` |
| Execution §4.2 order, §4.9 concurrency, §4.10 failure, and §4.11 temporary/cleanup | `R-EXEC-ORDER`, `R-MEMORY-MODEL`, `R-FAIL`, `R-TEMP-DROP` |
| Programs §6.2 target model and §6.5 native ABI | `R-TARGET`, `R-NATIVE-RECORDS`, `R-C-CALLBACKS`, `R-C-ABI-FAMILIES` |
| Focused ownership, range, raw-pointer, native-interop, I/O, and TCP limitation statements | `R-PLACE-STATE` through `R-PUBLIC-ALLOCATORS`, `R-RANGE-LOANS`, `R-DYNAMIC-VIEWS`, `R-RAW-POINTERS`, `R-NATIVE-RECORDS`, `R-C-CALLBACKS`, `R-C-ABI-FAMILIES`, `R-TEXT-FORMATTING`, `R-HOST-SERVICES` |
| Standard library §7.7 component omissions | `R-SHARED-OWNERSHIP`, `R-SUM-TYPES`, `R-DYNAMIC-VIEWS`, `R-CONTAINER-SURFACE`, `R-CONCEPTS`, `R-TEXT-FORMATTING`, `R-HOST-SERVICES`, `R-RECOVERABLE-ALLOCATION`, `R-ALLOCATOR-MODEL`, `R-PUBLIC-ALLOCATORS`, `R-NATIVE-RECORDS`, `R-C-CALLBACKS`, `R-C-ABI-FAMILIES` |
| Architecture audit §7.1 order, §7.2 failure identity, and §7.3 documentation retention | `R-EXEC-ORDER`, `R-FAIL`, `R-DOC-COMMENTS` |

## Maintenance Rule

When an owner row completes, update this ledger in the same change as the
canonical language/architecture documents and tests. A restriction leaves the
ledger only when its replacement is current, backend-independent behavior. A
proposal or parser production alone is not completion.

Every new restriction must state its class, readiness role, user-facing client,
owner, and evidence gate.
Do not write only “deferred,” “future work,” or “not yet supported.” If no row
owns a proposed relaxation, the current rule remains and a new prompt-sized row
is required before implementation.
