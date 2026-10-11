# GTI Systems-Readiness Roadmap And Soft 1.0 Goal

> **Role:** durable capability and readiness goals, not a second work queue or
> specification. The [implementation sequence](implementation-sequence.md)
> owns prompt-sized work. Source and tests are authoritative when a document
> disagrees with them; current semantics belong in `docs/language/` and
> implemented architecture in `docs/architecture/`.

## Direction And Reconciliation

GTI is an independent systems language with strong C++ syntax familiarity,
explicit ownership, deterministic cleanup and excellent safety as a goal. Its
current C++ backend implements GTI semantics; neither generated C++ nor an LLVM
representation defines the language.

Public policy belongs in ordinary GTI wherever its invariants can be expressed.
The compiler and runtime still own irreducible facts such as layout, object
lifetime, checked borrowing, target operations and verified failure control
flow. Each private/source boundary needs its own rationale. Removing a private
handle is not success if safety, supported programs or execution cost regress.

This roadmap was reconciled against the 0.399.1 source checkpoint on
2026-10-11. The earlier chronological milestones are preserved in the
[historical roadmap](archive/systems-readiness-roadmap-2026-10-11.md), not
silently discarded or presented as unfinished foundations. Their restrictions
and version horizons must not override newer implemented contracts.

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

Self-hosting, an LLVM code-generation backend, separate compilation and a stable
ABI for ordinary GTI objects are not automatic prerequisites of that claim.
They can become scheduled goals when a supported outcome needs them. A source
package registry already exists; its historical non-requirement is not evidence
that it remains unimplemented. General allocation, casts and manual lifetime
facilities likewise already exist within their current safe/unsafe contracts.

## Implemented Baseline

The table names delivered families and their owning contracts, not exhaustive
API or performance parity. Remaining breadth is described in the next section.

| Area | Implemented capability and authority |
| --- | --- |
| Numeric and target facts | Exact binary32/binary64, checked fixed-width arithmetic and conversions, explicit arithmetic modes, target-owned data-layout facts, bounded `sizeof`/`alignof` and public layout queries. See [static semantics](../language/static-semantics.md) and [target rules](../language/programs-and-targets.md). |
| Values and control flow | Classes, interfaces, generated lifecycle, fixed arrays, generics/defaults/type packs, payload enums and exhaustive matching, conditional values, loops and explicit movement. See [static semantics](../language/static-semantics.md). |
| Ownership and borrowing | Checked shared/exclusive references, stable-place reborrows, indexed places, range-element loans, multiple stored dependencies and conflict-checked mixed reference aggregates. Inference remains the default; explicit origin contracts cover supported otherwise-ambiguous bridges. See [ownership](../language/ownership-and-lifetimes.md). |
| Low-level memory | General nested raw pointers, safe inert address formation, explicit unsafe access/conversion obligations, manual construction/destruction/relocation, typed leases, source sparse storage, process/resource providers and arena/pool clients. See [raw pointers](../language/raw-pointers.md) and [allocation rationale](../decisions/022-safe-typed-allocation.md). |
| Source-owned sequences | Vector, string and resource-vector initialized-prefix policy is source-defined over raw buffers and general lifetime operations; the former private prefix-storage engine is retired. See [the operation ledger](../architecture/standard-library-and-runtime.md#operation-ownership-ledger). |
| Library composition | Mutable spans and buffers, read/mut iteration, AVL maps, bucketed unordered maps, other executable containers, explicit cloning, shared ownership, Expected propagation/extraction/combinators and optional values. These do not imply that every declared operation is executable; consult the [library contract](../language/standard-library.md). |
| Callables | Exact function items, local move captures, owned read/mut/once wrappers, and bounded inline/boxed polymorphic storage. General callable and lifecycle facts remain distinct from source-selected storage policy. See [static semantics](../language/static-semantics.md) and [library architecture](../architecture/standard-library-and-runtime.md). |
| Text and services | Source formatter customization including integral-enum defaults, numeric parsing, encoding, buffered/file I/O, clocks, synchronous processes, filesystem operations, HTTP/HTTPS, URLs and JSON. See [the library specification](../language/standard-library.md). |
| Concurrency | Transfer/share facts, concurrent-global policy, joined owned tasks/results, ordered scalar atomics and integer fetch operations, scoped mutex guards, and bounded unsafe C-style thread/mutex/condition-variable services. See [concurrency](../language/concurrency.md). |
| C interoperability | Target-correct C scalars, native enums, passive records/unions, opaque handles, nested pointers, native function-pointer invocation, exact GTI callback adapters, generated C/C++ headers and libclang-based binding import. See [native interop](../language/native-c-interop.md). |
| Compile-time facilities | Staged verified-MIR evaluation of the supported scalar/text/passive-record/fixed-array profile, checked read-only reference observation and selected operators. Reflection phases 1/2 supply `constexpr for`, metaobjects, attributes, fields, enumerators, bases and generic arguments. See [static semantics](../language/static-semantics.md) and [the reflection plan](reflection.md). |
| Modules and packages | Explicit source imports/exports/re-exports, named standard-library modules, workspaces/path/Git dependencies, integrity locks, source registry acquisition/publication/authentication and build programs. See [programs](../language/programs-and-targets.md) and [driver architecture](../architecture/build-and-driver.md). |
| Compiler authority | Every executable body follows verified MIR into a self-contained `LoweredProgram`; exact generated-item, layout, ABI and representation tables keep source meaning out of backend execution. See [backend architecture](../architecture/backend.md). |
| Optimization | Stable editing/proof replay, per-instance effects, deterministic local dataflow/SCCP, loop/range and alias/capture facts, bounded closed-call/exact-receiver facts and proof-carrying check elimination. Analysis availability does not imply every loop/memory/interprocedural transform exists. See [optimization architecture](../architecture/optimization.md). |
| Tooling and quality | Compiler-owned hover/completion/signatures/navigation, safe actions, documentation projection, project configuration, source-level debugging foundations, colour/progress, timing/reports and typed interaction/differential/metamorphic/reduction tools. See [LSP](../architecture/lsp.md), [driver](../architecture/build-and-driver.md) and [verification](../architecture/verification.md). |

## Systems-Readiness Workloads

These remain program/API outcomes, not public-name compiler exceptions. Existing
acceptance is a baseline to preserve, not proof that later extensions are free.

| Outcome | Current foundation | Remaining readiness work |
| --- | --- | --- |
| Real C-library wrapper | Layout-stable records, opaque handles, callbacks, native pointer/scalar families and generated bindings. | Broader ownership/retention contracts and foreign storage where a real binding needs them; keep exact ABI and native fixture evidence. |
| Application-owned arena or pool | Typed providers, leases, source allocator policy, manual lifetime and allocator clients exist. | Extend allocator-aware clients without private policy shortcuts; verify layout, rollback, exhaustion, release identity and execution cost. |
| Multithreaded work queue | Owned tasks, ordered atomics/fetch, scoped mutex guards, join and worker failure are executable. | Scoped borrowed tasks or richer synchronization require additional explicit lifetime/entry contracts; the current owned queue does not wait for them. |
| Renderer/game update | Mutable containers/views, source prefix storage, time/files/allocation, payloads and exact domain operators exist. | Direct final-address/inline construction, useful range/operator breadth and optimized representative frame workloads. |
| Compiler-style AST/protocol | Generic payload enums, matching and owned transport have delivered profiles. | Record any newly needed recursive/indirect/callable composition as a concrete lifecycle prerequisite, rather than restarting the enum feature. |
| Fallible application pipeline | Owned and checked borrowed Expected extraction, `?`, mapping and source-defined host wrappers exist. | Broaden applicable construction/borrowed-combinator profiles, recoverable owner factories and service composition with exact cleanup. |
| Host-embedded gameplay module | Versioned ABI decision, dormant library/export configuration and semantic export roots are delivered. | Context containment, generated wrappers/header, shared-library publication, editor support and installed load/call/fail/unload/reload acceptance. See [embedding delivery](implementation-sequence.md#engine-embedding-delivery-campaign). |

Each extension must state the general rule without its motivating library's
name, preserve semantic -> HIR -> MIR authority and prove it with an unrelated
user type where appropriate. Existing safety guarantees are not relaxed merely
because a native backend could execute a representation.

## Remaining Goal Families

The operational sequence determines readiness and ordering for individual
slices. This table is the durable scope map; it is not another numbered queue.

| Goal family | Remaining scope and prerequisites |
| --- | --- |
| Final-address and inline construction | Close borrowed-pack acceptance, adopt node emplacement, extend optional-like inline publication/rollback and prove immovable wrapper eligibility. Sparse allocated final-address emplacement already exists. `L-UP-10/11/12/18` distinguish these outcomes. |
| Borrow precision and traversal | Independently shorten selected owner dependencies, support justified reseatable/proxy/output-range forms and refine invalidation. Preserve whole-carrier dependencies while that carrier remains live. `L-UP-16` and the [range plan](iterators-and-ranges.md) own the next clients. |
| Ownership-library completion | Recoverable unique/shared factories, executable weak ownership and selected aliasing/co-allocation. Source-policy candidates M05–M10 require per-operation proofs; they do not mandate erasing irreducible lifetime, borrowing or propagation support. See [the operation ledger](../architecture/standard-library-and-runtime.md#operation-ownership-ledger). |
| Compile-time execution and reflection | Extend the staged MIR profile rather than permanently duplicate runtime rules in an AST interpreter. Reflection phase 3 adds structural construction, decoding and callable metadata within initialization/ownership/budget contracts. See `L-CONST-01` and [reflection](reflection.md). |
| Generic contracts and text | Conditional member operators, required literal-format validation, useful range/callable/hash relationships, named/dynamic fields and explicit decoding. Current exact formatter contracts and ordinary method requirements are delivered; broader constraints need their own source rule. `L-UP-14/15` and the [restriction ledger](language-alignment.md) own these boundaries. |
| Native boundary breadth | C-08B foreign variables, explicit ownership transfer, retained/captured callbacks and justified foreign-thread entry; later C++ import policy/reader work. Reuse implemented nested pointer/native signature identities. See [binding import](native-bindings-import.md). |
| Hosted and concurrent services | Environment access, process capture/redirection/asynchronous ownership, traversal/watch and further network/clock clients; safe condition variables and scoped borrowed threading where required. Implemented unsafe C-style services are not missing safe wrappers by default. See [library gaps](../language/standard-library.md#77-library-specification-gaps) and [concurrency omissions](../language/concurrency.md#current-omissions). |
| Efficient successful execution | Context refinement, conversion/division/shift and relational proofs, optimized failure-effect refresh, bounded inlining, loop/memory clients and source-library representations. Foundations and earlier performance campaigns are not completion of this ongoing requirement. See [optimization goals](optimization.md). |
| Daily development latency | On-demand trusted-body facts, scoped completion, appropriate reuse, whole-program reachability and cleanup/emission scaling, with bounded diagnostics and identity/invalidation contracts. See [LSP evolution](lsp-evolution.md) and [performance tooling](performance-tooling.md). |
| Packaging/module/platform breadth | Aggregate standard-library import and platform selection, real source exports, registry governance/backend extensions, freestanding or further target profiles, and separate compilation when a named outcome requires it. Existing manifests/cache/Git/registry/project-aware LSP are foundations, not tasks to repeat. |

General requires-expressions, associated types, partial specialization, broader
inheritance, coroutines, unrestricted reflection and ordinary GTI binary ABI
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

## Operational Sequence

Use the [implementation sequence](implementation-sequence.md) for the next
prompt. Select an unfinished outcome with met external prerequisites, or
co-deliver its explicitly scoped prerequisite. Diagnose actual failures instead
of relying on a dated absence list. Do not mark a whole family complete from one
fixture or require a monolithic rewrite before a useful vertical slice.

The grammar specifies source structure, not all implemented semantics. Canonical
language documents, source and tests establish actual support; detailed plans
describe proposed/incomplete work. Update those roles as behavior lands and
retain dated evidence separately from the active queue.

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
