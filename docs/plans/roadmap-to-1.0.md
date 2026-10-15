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
2026-10-11 and incorporates merged 0.400.0 reflection phase 3 and 0.401.0
ordered-query composition, plus 0.402.0 owned environment observation. These
updates record implementation status, not a
newly executed whole-toolchain acceptance run. The earlier chronological milestones are preserved in the
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
| C interoperability | Target-correct C scalars, imported enum aliases/constants, passive C ABI records, opaque handles, nested pointers, native function-pointer invocation, exact GTI callback adapters, generated C/C++ headers and libclang-based binding import. Explicit `[[c_abi]]` passive unions support C ABI passage, nested layouts and import; ordinary unannotated unions remain non-ABI types. See [native interop](../language/native-c-interop.md). |
| Compile-time facilities | Staged verified-MIR evaluation of the supported scalar/text/passive-record/fixed-array profile, checked read-only reference observation and selected operators. Reflection phases 1–3 supply `constexpr for`, declaration/type/field/enumerator/base/generic/callable metaobjects, attributes and structural construction through the designated-construction model. Metadata folds to constants; structural construction uses ordinary initialization/ownership and does not enable metaobject invocation or runtime reflection. See [static semantics](../language/static-semantics.md) and [the reflection plan](reflection.md). |
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
| Compile-time execution and reflection | Extend the staged MIR profile rather than permanently duplicate runtime rules in an AST interpreter. Reflection's structural construction and callable metadata are delivered within initialization/ownership/budget contracts. Follow-on codec clients, including the scheduled experimental JSON verification gate, and broader constant-execution profiles remain distinct work. See `L-CONST-01` and [reflection](reflection.md). |
| Generic contracts and text | Conditional member operators, required literal-format validation, useful range/callable/hash relationships, named/dynamic fields and explicit decoding. Current exact formatter contracts and ordinary method requirements are delivered; broader constraints need their own source rule. `L-UP-14/15` and the [restriction ledger](language-alignment.md) own these boundaries. |
| Native boundary breadth | C-08B foreign variables, explicit ownership transfer, retained/captured callbacks and justified foreign-thread entry; later C++ import policy/reader work. Reuse implemented nested pointer/native signature identities. See [binding import](native-bindings-import.md). |
| Hosted and concurrent services | Read-only environment lookup is delivered; environment mutation/enumeration and child-local overrides, process capture/redirection/asynchronous ownership, traversal/watch and further network/clock clients remain; safe condition variables and scoped borrowed threading where required. Implemented unsafe C-style services are not missing safe wrappers by default. See [library gaps](../language/standard-library.md#77-library-specification-gaps) and [concurrency omissions](../language/concurrency.md#current-omissions). |
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

## Historical Milestone Text Retained During Reconciliation

The following milestone and track sections preserve the complete incoming
upstream planning text at `fe2d8751`. They are historical context, not a second
current queue or specification: early private-storage, callable, allocation,
MIR, constraint and version-horizon restrictions below have later delivered
contracts. The reconciled capability/remaining-goal tables above and the
implementation sequence own current status. The same excerpt is preserved in
the historical roadmap so no upstream planning content is lost.

## Milestone 5: Standard-Library Foundation

The systems-ready library should be layered. Portable policy belongs in GTI
source; narrow native C entries exist only for host services that cannot be
implemented portably and remain behind source-defined GTI wrappers.

### Required foundation modules

| Module area | Minimum systems-ready surface |
| --- | --- |
| utility | move, swap, exchange, pair, comparison helpers, limits |
| ownership | unique ownership, typed recoverable storage allocation, shared ownership, weak observation, and recoverable owner factories |
| optional values | `std::optional<T>` over checked single-slot storage; `expected<T, E>` observers and combinators |
| fixed and dynamic storage | complete array, vector, span, string view, and owning string APIs |
| traversal | structural iterators, sentinels, sized ranges, range-first foundational algorithms |
| text | byte/UTF-8 policy, parsing, formatting, and explicit owning/view conversion rules |
| math | portable primitive math functions, constants, integer utilities, and checked edge behavior |
| diagnostics | implement Execution §4.10's stable runtime failure records and source-facing messages |

`std::optional<T>` is now an ordinary move-only class over private checked
inline storage, not a nullable reference rule. Its first production slice
supports empty/engaged construction, checked access, reset/emplace, move-only
payload transfer, and exactly-once cleanup without allocation. Conditional
copy and assignment remain later `L-VALUE-01` breadth. `std::variant` and
arbitrary tuple packs can wait until independent pack element access and
visitation are justified by real uses.

### Host-service modules

Add narrow, versioned runtime operations and source-defined wrappers for:

- counted stdout/stderr and bounded stdin byte input are implemented;
- unbuffered file open/read/write/seek/tell/close and
  process-working-directory path resolution are implemented in `<std/cstdio>`;
  richer path handling, buffering, and explicit text decoding remain;
- monotonic and wall-clock time;
- deterministic pseudo-random engines plus an explicitly nondeterministic seed
  source are implemented in `<std/random>`; distribution APIs remain;
- process arguments are implemented through the owned
  `int main(int, std::vector<std::string>)` entry form; read-only environment
  lookup returns independent optional strings through `<std/environment>`;
  environment mutation/enumeration, child-local overrides and any owner-tied
  process views remain;

Recoverable host failures return `expected`; programmer contract failures such
as out-of-bounds access retain stable terminating diagnostics. The public
library must not expose C pointers or backend handles merely because the C ABI
uses them internally.

### Second container and algorithm wave

- Add a hash table and `std::unordered_map` only after exact hasher/equality
  capabilities and iterator invalidation are proven.
- Add a tree map only when ordering capabilities and ownership behavior are
  stable enough to justify it.
- Add sorting, binary search, partitioning, accumulation, copy/move algorithms,
  and range predicates with explicit value/borrow behavior.
- Prefer complete-range overloads. Add iterator/sentinel pairs only where a
  real subrange use cannot be expressed cleanly.

Linked lists, deques, regex, broad filesystem traversal/watch, locale-heavy
text, Unicode grapheme algorithms, parallel algorithms, weak memory-order
breadth, and detached threads are not required for the first systems-ready
library. Ordered scalar atomics including integer fetch operations, joined
tasks/threads, and mutex-guard access are required by the concurrency workload.
Omitted breadth should be documented
rather than covered by thin unsafe wrappers.

### Exit gate

At least one nontrivial application can use only documented public GTI APIs for
dynamic collections, text processing, ownership, error handling, files, time,
randomness, formatting, and testing. No example imports `gti_internal`.

## Compiler Track A: MIR, Optimization, And Backend Authority

This was originally organized as a parallel track and was serialized while the
runtime-performance gate was active. The exact compiler sequence in
[`implementation-sequence.md`](implementation-sequence.md) closed
`O-GAME-01` on 2026-08-27, so unrelated language, MIR, backend,
runtime-representation, and standard-library semantics work is no longer
parked by that campaign.

Implemented foundation:

1. Deterministic MIR printing, stable typed editing, independent proof replay,
   pass management, body-revisioned analysis invalidation, and verification.
2. MIR scalar block parameters/edge arguments, acyclic scalar promotion,
   literal/compute/branch folding, unreachable cleanup, and dead non-trapping
   value elimination.
3. Complete conservative per-instance call effects with exact acyclic static
   refinement and conservative recursion/virtual/native boundaries.
4. One optimized `LoweredProgram` consumed by every executable backend, with
   no source-expression execution authority.
5. Opt-in pre/post MIR, human remarks, versioned JSON reports, and exact current
   safety-operation counts.
6. O2/O3 proof-carrying discharge of exact fixed-array read bounds,
   sparse-storage read bounds/initialization, and checked integer
   add/subtract/multiply/negate overflow outcomes.
7. Deterministic monotone local dataflow with executable-edge tracking and
   proof-carrying scalar SCCP.
8. Dominance-derived natural-loop forests with bounded invariant/affine range
   facts, plus body-local place alias/escape facts used by safety invalidation.

Operational sequence: the exclusive row order, prerequisites, stop conditions,
and implementation exit gates live only in
[`implementation-sequence.md`](implementation-sequence.md). This roadmap keeps
the durable requirements: use verified MIR proofs, preserve exact failure and
partial effects, measure end-to-end runtime, and add general loop transforms,
MemorySSA, devirtualization, or a new backend only when a selected workload
proves the bounded architecture insufficient.

Systems readiness requires one checked executable representation to control behavior. It does
not require an LLVM backend or aggressive `-O3`. Correct `-O0`, deterministic
output, measurable local optimizations, and no semantic dependence on the
native optimizer are more important.

## Parallel Track B: Project Builds And Packages

Keep direct mode permanently available:

```sh
gti main.gti -O2 -o main
```

Add the staged project workflow from the build-system proposal:

1. extract immutable compiler and native-toolchain requests into `gti_driver`;
2. add `gti.toml`, one executable target, and dev/release profiles;
3. add project workflow commands: `gti build`, `check`, `run`, `test`, `clean`,
   and `metadata` are complete; structured native manifest inputs and test
   targets are complete;
4. add deterministic whole-program caching — complete;
5. add workspaces and source-only path dependencies — complete;
6. add exact Git dependency resolution, `gti.lock`, `fetch`, `--locked`, and
   `--offline` — complete for pinned full revisions;
7. make LSP project discovery read-only and use the same target/source-root
   facts as the CLI.

Steps 1 and 2 are complete: direct mode and project builds route immutable
compilation and native requests through the separately compiled `gti_driver`
library. Project mode discovers and validates schema-versioned `gti.toml`
manifests, resolves executable/test targets and profiles, and publishes
artifacts under `build/gti/`. Step 3 includes frontend-only
checking, build-and-run with exact arguments, safe cleanup, read-only
deterministic metadata, and package/profile/target native inputs with explicit
target selection, containment, ordering, and automatic compilation of declared
C and C++ sources. Project test targets build and execute independently in
deterministic order. Step 4 now adds a verified SHA-256 whole-program cache for
project build/run/test requests, with exact source-graph, effective policy,
runtime/toolchain, and admitted-environment identity. Requests with declared
native sources, native search directories (including dependency-injecting
environment paths), opaque native argument vectors, native link operands, or
unresolved library/framework inputs bypass the cache until their transitive
inputs are modeled. Cache hits skip parsing through
native linking; corruption rebuilds before replacement; `--no-cache` bypasses
lookup/publication; and direct mode remains uncached. Step 5 now adds canonical workspace membership, recursive
source-only path dependency graphs, direct package include aliases,
deterministic package selection/shared outputs, schema-9 metadata, and package
cache provenance without network access. Step 6 adds explicit, content-verified
acquisition of pinned full Git revisions and reproducible locked/offline builds.
The read-only driver project-facts bridge is the next independent milestone.

The systems-ready build workflow does not need a registry, package build
scripts, binary GTI libraries, source globbing, or CMake replacement for
building the GTI compiler.

## Parallel Track C: Specification, Tooling, And Quality

### Language and library specification

- Keep a normative grammar and a semantic contract for every shipped feature.
- Document evaluation order, ownership state transitions, runtime failures,
  and target behavior independently of C++.
- Generate public standard-library API documentation from retained source
  comments.
- Implement the bounded source-owned deprecation diagnostic and tooling
  metadata under the already published Edition 1 retention/removal policy.

### Diagnostics and editor tooling

- Finish compiler-owned semantic tokens and remove confident identifier
  guessing from the LSP fallback.
- Add documentation hover, signature help, document symbols, references, and
  safe rename using compiler symbol identity.
- Make completion, definition, formatting, and diagnostics project-aware
  without letting the LSP fetch or build dependencies.
- Keep Tree-sitter structural highlighting usable without the LSP and keep GTI
  visuals aligned with equivalent C++ roles.
- Add a warning/lint policy that does not turn backend C++ warnings into GTI
  language rules.

### Verification and robustness

- Add parser, semantic, HIR, MIR, backend, CLI, LSP, formatter, Tree-sitter,
  installed-toolchain, and standard-library tests for each feature.
- Parse and compile every valid example in CI; keep negative examples tied to
  stable diagnostic codes and precise spans.
- Add fuzzing for lexing, parsing, formatting, source loading, and protocol
  framing; add differential and property tests for numeric and container edge
  behavior.
- Run sanitizer builds and platform release smoke tests.
- Implement the benchmark harness, compiler phase timing, optimization remarks,
  safety-check reports, and deterministic HIR/MIR dumps before making broad
  performance claims.
- Test both supported C++ backend modes where their representations differ.

## C++ Features: Adopt, Adapt, Or Defer

### Adopt or adapt for systems readiness

- bounded `constexpr` and `if constexpr`;
- range-for, iterators, spans, and algorithms with tracked owner lifetimes;
- RAII, smart ownership, and deterministic destruction without the rule of
  five;
- confined callable parameters and explicit move captures;
- deprecation attributes and documentation comments;
- an explicit, audited native service boundary for standard-library host
  operations.

### Preserve the spelling but improve the rule

- one-level `T*`/`const T*` retain familiar declarators, while dangerous
  operations require lexical `unsafe` and raw pointers never imply ownership.
- `T&` is non-null and read-only; `mut T&` is a writable borrow.
- `std::move(place)` consumes a tracked place instead of producing an
  unspecified moved-from value.
- range-for owns temporaries and tracks invalidation.
- `switch` has no implicit fallthrough.
- numeric conversions and dangerous arithmetic edges are checked.
- constructors remain explicit and overloads remain exact.
- classes use generated lifecycle facts instead of C++ special-member
  suppression rules.
- constraints describe exact capabilities without SFINAE or overload ranking.

Revision, 2026-10-14: the event-handler port demonstrated a current need for
protected implementation state. Protected **member access** is implemented with
receiver restrictions and ordinary ownership checks, demonstrated by example
150; it is no longer deferred breadth. Protected inheritance remains excluded.

### Later breadth unless a readiness client proves necessity

- unrestricted pointer-to-pointer and first-class function-pointer types beyond
  the bounded C out-parameter/callback clients, unchecked casts, source-level
  `new`/`delete`, placement construction, and manual lifetime;
- textual macros and general-purpose preprocessing;
- exceptions and native unwinding;
- ADL, free operator lookup, rewritten equality, and customization-point
  objects beyond exact domain-operator capabilities;
- virtual or borrow-carrying conversion operators (explicit-by-default
  conversion operators exist; see ADR 027);
- implicit user conversions outside declaration initialization, and
  conversion-ranked overload resolution;
- unrestricted multiple state-bearing inheritance, diamonds, protected inheritance, and
  covariant virtual returns;
- initializer-list preference, aggregate list conversion, CTAD, SFINAE,
  partial or constraint-selected specialization, and unrestricted compile-time
  metaprogramming;
- value packs, non-`uint64_t` value domains, and unrestricted dependent
  expressions beyond the bounded `L-GEN-01` default/argument model;
- stored reference captures, general escaping lambdas, and type erasure beyond
  the bounded owned-callable minimum;
- mutable, reference, nested, inherited, or partial-move structured bindings
  whose copy/borrow/move behavior is not represented by the current
  hidden-owner and projected-place model;
- coroutines, generators, runtime reflection and source generation
  (compile-time reflection over declarations is delivered; see the
  bounded compile-time programming section);
- unrestricted manual allocation/lifetime policy, varargs, packing/bit-fields,
  and freestanding execution beyond the bounded systems-readiness clients;
- binary modules, separate compilation, and a stable native GTI ABI.

“Later breadth” is not “never” and is not tied to a version number. It means no
current readiness workload requires the broader form. A focused client can
move it earlier after showing its safety model, IR ownership,
standard-library need, and tooling impact.

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
