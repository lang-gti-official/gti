# GTI Systems-Readiness Roadmap And Soft 1.0 Goal

> **Role:** durable capability and readiness goals, not a second work queue or
> specification. The [implementation sequence](implementation-sequence.md)
> owns prompt-sized work. Source and tests are authoritative when a document
> disagrees with them; current semantics belong in `docs/language/` and
> implemented architecture in `docs/architecture/`.

## Direction And Reconciliation

GTI is an independent systems language with strong C++ syntax familiarity,
explicit ownership, deterministic cleanup and excellent safety as a goal. Its
backends implement GTI semantics; neither generated C++ nor an LLVM
representation defines the language.

Public policy belongs in ordinary GTI wherever its invariants can be expressed.
The compiler and runtime still own irreducible facts such as layout, object
lifetime, checked borrowing, target operations and verified failure control
flow. Each private/source boundary needs its own rationale. Removing a private
handle is not success if safety, supported programs or execution cost regress.

Implementation-status audit: **2026-10-28, canonical main `d4acdb004`,
GTI 0.486.1**. This supersedes the 0.399.1/2026-10-11 status checkpoint.
The audit compares current source, registered positive/negative fixtures and
canonical contracts across every active plan listed in `docs/index.md`.
It does not rerun or certify the release-wide execution/performance matrix.
The [sequence reconciliation](implementation-sequence.md#implementation-status-audit-2026-10-28)
records evidence and the disposition of each domain plan.

**Implemented** means executable support in the stated profile, rather than a
declaration or proposal. **Partial** means a delivered foundation with named
missing shapes. **Planned** means the requested capability is still absent.
**Acceptance pending** means implementation exists but a required validation or
promotion gate remains; do not schedule its foundation again or call it released.
These labels do not turn every later-breadth idea into a mandatory 1.0 feature.
The earlier chronological milestones remain in the
[historical roadmap](archive/systems-readiness-roadmap-2026-10-11.md).

Current evidence is capability-specific. A source declaration, emission-only
test, focused runtime probe and complete installed-toolchain matrix establish
different things. Nothing in this roadmap is a blanket certification of every
composition, target or release configuration. An unmerged PR is not part of
the implemented baseline.

## What 1.0.0 Means

Under [ADR 012](../decisions/012-outcome-first-systems-readiness.md), 1.0 is a
soft, revisable goal: a full-featured language usable for serious systems
programming on its declared platforms. It is not a deadline, frozen checklist,
C++ feature-parity target or permission to stabilize an intentionally
incomplete subset. Publishing it activates the compatibility boundary in
[ADR 011](../decisions/011-language-compatibility-and-editions.md).

The readiness claim requires:

1. representative systems programs through documented public GTI facilities;
2. backend-independent evaluation, ownership, numeric, unsafe and failure rules;
3. a useful standard library whose public policy is ordinary source;
4. deterministic direct and manifest-based compilation, tests and dependencies;
5. reliable diagnostics, formatting, highlighting and package-aware editing;
6. matching installed compiler, runtime, libraries and tools on shipped platforms;
7. reproducible execution-cost evidence, not merely successful compilation; and
8. an explicit compatibility, omission and support contract.

LLVM is the primary development and intended public production backend under
[ADR 031](../decisions/031-native-llvm-backend-foundation.md#backend-development-policy-2026-10-25).
Completing that transition is an explicit delivery goal, not a requirement to
prove a C++ backend regression first. C++ is deprecated as the primary backend:
only bug fixes preserve its supported comparison-oracle behavior through v1.0;
new features need not be implemented twice. Catch-up after v1.0 is a possible
future owner decision, not automatic or part of 1.0 acceptance. LLVM is the
default on macOS arm64 since the owner's 2026-10-27 promotion. C++ remains the
default on other targets until
the [LLVM campaign](implementation-sequence.md#llvm-production-backend-campaign)
passes full current-language conformance there, including Linux. Self-hosting,
separate compilation and a stable ABI for ordinary GTI objects remain separate
goals rather than automatic prerequisites of systems readiness. A source
package registry already exists; its historical non-requirement is not evidence
that it remains unimplemented. General allocation, casts and manual lifetime
facilities likewise already exist within their current safe/unsafe contracts.

## Implemented Baseline

The table names delivered families and their owning contracts, not exhaustive
API or performance parity. Remaining breadth is described in the next section.

| Area | Implemented capability and authority |
| --- | --- |
| Numeric and target facts | Exact binary32/binary64, checked fixed-width arithmetic and conversions, explicit arithmetic modes, target-owned data-layout facts, bounded `sizeof`/`alignof` and public layout queries. See [static semantics](../language/static-semantics.md) and [target rules](../language/programs-and-targets.md). |
| Values and control flow | Classes, interfaces, protected member access, generated lifecycle, fixed arrays, generics/defaults/type packs, payload enums with owned/reference alternatives and exhaustive/consuming matching, conditional values, loops and explicit movement. See [static semantics](../language/static-semantics.md). |
| Ownership and borrowing | Checked shared/exclusive references, stable-place reborrows, indexed places, range-element loans, multiple stored dependencies and conflict-checked mixed reference aggregates. Inference remains the default; explicit origin contracts cover supported otherwise-ambiguous bridges, concept operations and callable results. Owner-dependent text fields and nested text values have delivered profiles; arbitrary borrowed containers remain incomplete. See [ownership](../language/ownership-and-lifetimes.md). |
| Low-level memory | General nested raw pointers, safe inert address formation, explicit unsafe access/conversion obligations, manual construction/destruction/relocation, typed leases, source sparse storage, process/resource providers and arena/pool clients. See [raw pointers](../language/raw-pointers.md) and [allocation rationale](../decisions/022-safe-typed-allocation.md). |
| Source-owned sequences | Vector, string and resource-vector initialized-prefix policy is source-defined over raw buffers and general lifetime operations; the former private prefix-storage engine is retired. See [the operation ledger](../architecture/standard-library-and-runtime.md#operation-ownership-ledger). |
| Library composition | Mutable spans and buffers, read/mut iteration, AVL maps, bucketed unordered maps, other executable containers, explicit cloning, shared ownership, source-defined Expected propagation/extraction/combinators through `std::tryable`, optional values, container category/reverse traversal, reference proxies and bounded value-yield ranges. These do not imply that every declared operation is executable; consult the [library contract](../language/standard-library.md). |
| Callables | Exact function items, local move captures, checked shared/exclusive reference captures, borrowed callable views and result-origin contracts, typed invocation pointers, owned read/mut/once wrappers, and bounded inline/boxed polymorphic storage. General callable and lifecycle facts remain distinct from source-selected storage policy. See [static semantics](../language/static-semantics.md) and [library architecture](../architecture/standard-library-and-runtime.md). |
| Text and services | Source formatter customization including integral-enum defaults, numeric parsing, encoding, buffered/file I/O, clocks, synchronous processes, filesystem operations, HTTP/HTTPS, URLs, owned read-only environment lookup and reflection-driven JSON. See [the library specification](../language/standard-library.md). |
| Concurrency | Transfer/share facts, concurrent-global policy, joined owned tasks/results, scoped parallel borrowing, ordered scalar atomics and integer fetch operations, safe atomic wait/notify, scoped mutex guards, per-thread storage, and bounded unsafe C-style thread/mutex/condition-variable services. See [concurrency](../language/concurrency.md). |
| C interoperability | Target-correct C scalars, imported enum aliases/constants, passive C ABI records, opaque handles, nested pointers, native function-pointer invocation, ordinary external C variables with unsafe access, exact GTI callback adapters, generated C/C++ headers and libclang-based binding import. Explicit `[[c_abi]]` passive unions support C ABI passage, nested layouts and import; ordinary unannotated unions remain non-ABI types. See [native interop](../language/native-c-interop.md). |
| Compile-time facilities | Staged verified-MIR evaluation of the supported scalar/text/passive-record/fixed-array profile, checked read-only/mutable references, mutable builders, ordinary enums, passive Expected composition, selected operators, transient exclusive allocation and destruction. Reflection phases 1–3 supply `constexpr for`, declaration/type/field/enumerator/base/generic/callable metaobjects, attributes and structural construction through the designated-construction model. Metadata folds to constants; structural construction uses ordinary initialization/ownership and does not enable metaobject invocation or runtime reflection. See [static semantics](../language/static-semantics.md) and [the reflection plan](reflection.md). |
| Modules and packages | Explicit source imports/exports/re-exports, named standard-library modules and aggregate `import std`, workspaces/path/Git dependencies, integrity locks, source registry acquisition/publication/authentication and build programs. See [programs](../language/programs-and-targets.md) and [driver architecture](../architecture/build-and-driver.md). |
| Generic contracts and typed macros | Boolean and multi-parameter protocol concepts with associated types/operations, conformance blocks, operation borrow-origin contracts, source-defined requirement policies/static queries, and confined typed macros with statement/value results. See [concepts](../language/concepts.md) and [static semantics](../language/static-semantics.md). |
| Compiler authority | Every executable body follows verified MIR into a self-contained `LoweredProgram`; exact generated-item, layout, ABI and representation tables keep source meaning out of backend execution. See [backend architecture](../architecture/backend.md). |
| Optimization | Stable editing/proof replay, per-instance effects, deterministic local dataflow/SCCP, loop/range and alias/capture facts, bounded closed-call/exact-receiver facts and proof-carrying check elimination. Analysis availability does not imply every loop/memory/interprocedural transform exists. See [optimization architecture](../architecture/optimization.md). |
| Tooling and quality | Compiler-owned hover/completion/signatures/navigation, safe actions, documentation projection, project configuration, source-level debugging foundations, colour/progress, timing/reports and typed interaction/differential/metamorphic/reduction tools. See [LSP](../architecture/lsp.md), [driver](../architecture/build-and-driver.md) and [verification](../architecture/verification.md). |

## Systems-Readiness Workloads

These remain program/API outcomes, not public-name compiler exceptions. Existing
acceptance is a baseline to preserve, not proof that later extensions are free.

| Outcome | Current foundation | Remaining readiness work |
| --- | --- | --- |
| Real C-library wrapper | Layout-stable records, opaque handles, callbacks, native pointer/scalar families and generated bindings. | Broader ownership/retention contracts and additional foreign-storage profiles where a real binding needs them; keep exact ABI and native fixture evidence. |
| Application-owned arena or pool | Typed providers, leases, source allocator policy, manual lifetime and allocator clients exist. | Extend allocator-aware clients without private policy shortcuts; verify layout, rollback, exhaustion, release identity and execution cost. |
| Multithreaded work queue | Owned and scoped borrowed tasks, ordered atomics/fetch/wait/notify, scoped mutex guards, join, worker failure and per-thread storage are executable. | Safe guard-aware condition variables, timed/cancellable waits, persistent pools and foreign-thread entry remain distinct goals. Preserve exact ownership, publication and worker cleanup evidence. |
| Renderer/game update | Mutable containers/views, source prefix storage, time/files/allocation, payloads and exact domain operators exist. | Remaining node/inline/immovable construction profiles, useful range/operator breadth and optimized representative frame workloads. |
| Compiler-style AST/protocol | Generic payload enums, matching and owned transport have delivered profiles. | Record any newly needed recursive/indirect/callable composition as a concrete lifecycle prerequisite, rather than restarting the enum feature. |
| Fallible application pipeline | Owned and checked borrowed Expected extraction, `?`, mapping and source-defined host wrappers exist. | Broaden applicable construction/borrowed-combinator profiles, recoverable owner factories and service composition with exact cleanup. |
| Host-embedded gameplay module | Infallible LLVM library targets, generated versioned wrappers/header, context lifecycle and immutable shared-artifact/cache/report/clean publication are implemented for macOS arm64/Linux x86_64. | Fallible export containment, editor integration and full installed load/call/fail/unload/reload acceptance. See [embedding delivery](implementation-sequence.md#engine-embedding-delivery-campaign). |

Each extension must state the general rule without its motivating library's
name, preserve semantic -> HIR -> MIR authority and prove it with an unrelated
user type where appropriate. Existing safety guarantees are not relaxed merely
because a native backend could execute a representation.

## Remaining Goal Families

The operational sequence determines readiness and ordering for individual
slices. This table is the durable scope map; it is not another numbered queue.

| Goal family | Remaining scope and prerequisites |
| --- | --- |
| Final-address and inline construction | Close the remaining borrowed-pack composition acceptance, adopt direct node emplacement, extend optional-like inline publication/rollback and prove immovable wrapper eligibility. Sparse allocated final-address emplacement already exists. `L-UP-10/11/12/18` distinguish these outcomes. |
| Borrow precision and traversal | Independently shorten selected owner dependencies, support justified reseatable and further proxy/output-range forms and refine invalidation. Preserve whole-carrier dependencies while that carrier remains live. `L-UP-16` and the [range plan](iterators-and-ranges.md) own the next clients. |
| Ownership-library completion | Recoverable unique/shared factories, executable weak ownership and selected aliasing/co-allocation. Source-policy candidates M05–M10 require per-operation proofs; they do not mandate erasing irreducible lifetime, borrowing or propagation support. See [the operation ledger](../architecture/standard-library-and-runtime.md#operation-ownership-ledger). |
| Compile-time execution and reflection | Extend the staged MIR profile rather than permanently duplicate runtime rules in an AST interpreter. Reflection's structural construction and callable metadata are delivered within initialization/ownership/budget contracts. The reflection-driven JSON client and its promotion to `<std/json>` are delivered (0.409.0). Metaobject invocation/runtime reflection, remaining metadata kinds and broader constant-execution profiles remain distinct work. See `L-CONST-01` and [reflection](reflection.md). |
| Generic contracts and text | Conditional member operators, required literal-format validation, further range/callable/hash relationships, named/dynamic fields and explicit text decoding. Associated types, protocol operation contracts, static queries, exact formatter contracts and ordinary method requirements are delivered; broader constraints need their own source rule. `L-UP-14/15` and the [restriction ledger](language-alignment.md) own these boundaries. |
| Native boundary breadth | TLS/volatile/top-level-array foreign objects, explicit ownership transfer, retained/captured callbacks and justified foreign-thread entry; ordinary C-08B foreign variables are implemented; later C++ import policy/reader work. Reuse implemented nested pointer/native signature identities. See [binding import](native-bindings-import.md). |
| Hosted and concurrent services | Read-only environment lookup is delivered; environment mutation/enumeration and child-local overrides, process capture/redirection/asynchronous ownership, traversal/watch and further network/clock clients remain; safe guard-aware condition variables and further thread lifecycle/scheduling profiles where required. Scoped borrowed invocation and scalar atomic waiting are implemented. Implemented unsafe C-style services are not missing safe wrappers by default. See [library gaps](../language/standard-library.md#77-library-specification-gaps) and [concurrency omissions](../language/concurrency.md#current-omissions). |
| Efficient successful execution | Context refinement, conversion/division/shift and relational proofs, optimized failure-effect refresh, bounded inlining, loop/memory clients and source-library representations. Foundations and earlier performance campaigns are not completion of this ongoing requirement. See [optimization goals](optimization.md). |
| Daily development latency | On-demand trusted-body facts, scoped completion, appropriate reuse, whole-program reachability and cleanup/emission scaling, with bounded diagnostics and identity/invalidation contracts. See [LSP evolution](lsp-evolution.md) and [performance tooling](performance-tooling.md). |
| Packaging/module/platform breadth | Further platform selection and installed target resources, migration of remaining library compatibility facades to real source exports, registry governance/backend extensions, freestanding or further target profiles, and separate compilation when a named outcome requires it. Existing manifests/cache/Git/registry/project-aware LSP are foundations, not tasks to repeat. |

General requires-expressions beyond the implemented validity-only/static-query
profiles, generic associated-type families beyond current protocol projections,
partial specialization, broader inheritance, coroutines, unrestricted reflection and ordinary GTI binary ABI
remain separate design goals when justified. Intentional exact overloads,
explicit movement, compiler-controlled lifecycle and absence of textual macros
are not automatically missing features. Do not reintroduce withdrawn custom
copy/move bodies as an incidental prerequisite of a library optimization.

## Runtime Performance And Compiler Cost

[ADR 012's performance revision](../decisions/012-outcome-first-systems-readiness.md#runtime-performance-requirement-2026-10-09)
is current policy. Reasoned operation families and a cost model can justify
optimization architecture before a benchmark exposes a regression. Measurements
validate results; they are not permission to start. The old exclusive O-PERF
campaign is a historical completion, not a permanent freeze on language work.

Acceptance measures successful-path work, allocations, metadata, dispatch,
cleanup/failure isolation, code size and applicable compile/memory costs. Keep
semantic C++ and idiomatic C++ comparisons distinct. Preserve observable order,
first failure, partial writes, active lifetime and exact cleanup. A material
runtime regression requires a correction or explicit owner acceptance; a green
correctness suite or follow-up issue is not a waiver.

The existing reference-workload readiness objectives remain visible: fixed-array
overflow at most 1.2x idiomatic C++, core checked scalar/container kernels at
most 1.5x idiomatic, and game update at most 1.5x semantic/2x idiomatic C++, with
the recorded control-regression policy. These are workload-specific goals on a
published reference configuration, not a universal speedup threshold or shared
CI wall-time gate. Revalidate on the actual combined toolchain; historical
measurements do not certify every later source-storage or optimizer change.

Compiler/edit latency is measured separately from generated execution. Tests
must keep exact-head identities, bounded worker accounting, optimized compiler
builds and reproducible evidence. Rebuildable outputs are not recovery evidence;
retain the source/revision, settings, reports and logs needed to explain results.

## Reading What Remains Before 1.0

The remaining goal families above identify capability work, not a claim that
every item is an unconditional release blocker. The owner still chooses the
supported 1.0 profiles and records explicit omissions under ADR 012/011.
Within that scope, three kinds of unfinished work must remain visible:

| Kind | Current disposition | Completion evidence |
| --- | --- | --- |
| Missing or partial language/library capabilities | Weak ownership, recoverable unique/shared factories, direct inline/immovable emplacement, remaining borrow-storage/projection shapes, conditional operators and literal-format validation are open. See `L-OWN-01/02`, `L-UP-10`–`L-UP-18`, `L-CONST-01` and the restriction ledger for exact supported boundaries. | Implementation, owning positive/negative tests, public examples, documentation, cleanup and required platform/performance acceptance for each selected profile. |
| End-to-end workflows and tooling | Infallible LLVM shared-library builds and host loading are implemented; full context failure containment, editor and installed acceptance remain `E-EMBED-01B/C`, `B-LIB-02`, `T-EMBED-01`, `Q-EMBED-01`. Deprecation diagnostics (`Q-DEPRECATION-01`) and LSP shared-cache crash-state safety (`T-LSP-06`) remain planned. | Ordinary installed user workflows, not an internal foundation fixture or dormant configuration switch. |
| Backend and release acceptance | LLVM is the macOS arm64 default; production completion on Linux and the complete reconciled language/library/native/platform/performance matrix remain `O-LLVM-14/15/16` obligations. Existing native implementations and earlier corpus counts do not close that matrix. | Current source receipts for the [readiness baseline](../../tests/llvm_readiness_baseline.json), representative systems workloads, sanitizer/installed-toolchain checks, supported archives and explicit omissions/compatibility signoff. |

There is no evidence-backed percentage of “1.0 complete”: the roadmap does not
freeze the supported feature set, and historical passing suites are not a
current release-candidate run. The gates below define the release claim.

## Historical Milestone Text Retained During Reconciliation

The original foundation-module lists, “add maps/packages/tooling” instructions,
C++ adoption/defer lists and operational ordering are preserved in the
[historical roadmap](archive/systems-readiness-roadmap-2026-10-11.md).
They are historical evidence, not a second active backlog. In particular,
nested pointers, manual lifetimes, stored reference captures, associated types,
container iteration, project facts, source packages and reflection-driven JSON
must not be scheduled as absent foundations. The implementation sequence and
domain plans own the narrower unfinished shapes.

## Soft 1.0 Readiness Gates

### Language And Library

- Every admitted construct has backend-independent meaning and applicable
  semantic/HIR/MIR verification; invalid supported-source use diagnoses before
  native compilation rather than falling through to another language checker.
- Safe/unsafe obligations, exact arithmetic, evaluation order, movement,
  borrowing and cleanup remain explicit. Unsafe does not disable provable facts.
- Required library components are executable, documented and covered across
  applicable primitive, aggregate, move-only, polymorphic, empty, failure and
  boundary profiles. A bodyless declaration is not completed library support.
- Public examples use documented general facilities, not private names. Library
  policies and performance are validated independently of the backend choice.
- Compatibility, intentional restrictions and outstanding profiles are honest.

### Compiler, Runtime And Performance

- O0 is correct without optimizer rescue; higher optimization preserves exact
  observable behavior and independently verifies transformations.
- Every executable body and generated artifact uses the authoritative lowered
  contract. New backends start there rather than recreate source semantics.
- Defined failure, allocation abort, recovery through Expected, callback/task
  firewalls and future embedding containment retain distinct documented rules.
- Representative workload execution/cost meets its stated acceptance, with exact
  identities, raw observations, controls and explicit platform limits.
- Required sanitizers, malformed-IR, feature-interaction, differential and
  installed-toolchain gates have no unresolved release-blocking failure.

### Projects, Tooling And Release

- Direct and manifest build/check/run/test, dependencies, locks, offline use,
  publication/cache integrity and contained cleanup work from installed tools.
- CLI/LSP share compiler and driver facts; diagnostics, actions, completion,
  navigation, formatting and Tree-sitter agree during valid and incomplete edits.
- Supported archives contain matching compiler, LSP, parser, runtime, source
  library, metadata, checksums and licences; platform contracts are exercised.
- Release candidates state omissions, compatibility window and performance
  acceptance rather than delegate gaps to generated C++ or count syntax.

When these workloads and gates are satisfied, 1.0 is a meaningful readiness and
compatibility claim. The goals may be revised before that publication; the
revision must preserve truthful implementation status and the mandatory safety,
verification and performance contracts.
