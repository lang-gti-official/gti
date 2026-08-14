# GTI Compiled Compiler Library Migration

> **Plan status:** Implemented. Current compiled/header boundaries are
> documented in
> [`docs/architecture/overview.md`](../architecture/overview.md).

Status: implemented; phases 1 through 7 are complete

Prompt-sized migration ordering is maintained in
[`implementation-sequence.md`](implementation-sequence.md). The phases below
own the mechanical subsystem design; they must not be combined with semantic
behavior changes in one task.

This proposal migrates GTI from a fully header-only compiler implementation to
compiled internal libraries without changing the GTI language, direct compiler
workflow, frontend phase order, diagnostics, generated C++, or installed command
behavior. The migration is incremental: each phase moves one coherent subsystem
behind an existing declaration boundary and leaves the complete toolchain usable.

Phase 1 is implemented by the change that introduces this document. `Lexer`
keeps its declaration in `include/gti/lexer.h`, its implementation now lives in
`src/compiler/lexer.cpp`, and `gti_compiler` is a static CMake library linked by
the CLI, LSP, and compiler tests.

Phase 2 now has its first complete subsystem slice. `SourceLoader` keeps its
declaration and request-owned state in `include/gti/source_loader.h`, while its
filesystem, include-resolution, cycle, visibility-edge, provenance, and
source-manager algorithms live in `src/compiler/source_loader.cpp`. Parser
algorithms and request-local state now live behind the `Parser` facade in
`src/compiler/parser.cpp`.

Phase 3 is implemented through the same behavior-preserving boundary.
`SemanticModel` and semantic records remain visible snapshot contracts, while
the mutable visitor, declaration registration, resolution, flow/lifecycle
analysis, and concrete-instance reanalysis compile in
`src/compiler/semantic_analyzer.cpp` behind the `SemanticVisitor` facade.

Later optimizer groundwork has also moved coherent non-template utilities into
the library without completing the intervening migration phases. MIR
reachability/use repair and verification live in `src/compiler/mir.cpp`,
deterministic MIR printing lives in `src/compiler/mir_printer.cpp`, and effect
classification plus the identity optimization facade live under
`src/compiler/optimization/` and `src/compiler/optimizer.cpp`. Checked integer
arithmetic is compiled in `src/compiler/checked_integer.cpp` using the sole
`llvm::APInt` implementation selected under ADR 006; its former portable
implementation is non-built reference material under `archive/compiler/`.
Target-triple parsing lives in `src/compiler/target.cpp`, and tool-process
support in `src/compiler/support.cpp`. Phase 4 is complete: concrete instance
de-duplication is compiled in `src/compiler/hir.cpp`, HIR lowering lives in
`src/compiler/hir_lowering.cpp`, and MIR body/CFG lowering lives in
`src/compiler/mir_lowering.cpp`. Optimization and C++ emission algorithms are
also compiled. Frontend orchestration, semantic snapshot queries, constant
evaluation, formatting, and language-query algorithms compile as well. Headers
retain records, templates, short `constexpr` operations, trivial accessors, and
abstract contracts rather than traversal or policy implementations.

## Decision Summary

GTI will no longer treat header-only implementation as a permanent compiler
constraint.

- `include/gti/` remains the declaration and reusable data-model surface.
- Non-template compiler algorithms move to `src/compiler/` one subsystem at a
  time.
- `gti_compiler` becomes the compiled static frontend and middle-end library.
- The CLI, LSP, tests, and future driver reuse the same compiled implementation.
- The C++ backend and native driver gain separate compiled targets only when
  their current contracts are ready to enforce that dependency direction.
- Release binaries continue to link the compiler statically. GTI does not
  introduce a shared-library runtime dependency or stable compiler ABI.
- Installed headers and `libgti_compiler.a` are an exact-version pair. Their ABI
  may change whenever `VERSION` changes; no cross-version binary compatibility
  is promised.

This changes how GTI itself is built, not how GTI source programs are compiled.
One `.gti` entry and its complete `SourceGraph` still produce one whole-program
backend artifact and one native compiler invocation.

## Why Migrate

The original header-only layout was effective while the compiler was small. It
kept the tutorial-stage build simple and allowed the CLI, tests, and later LSP
to reuse implementation immediately. The current compiler has grown into
distinct source-loading, parsing, semantic, HIR, MIR, optimization, backend,
formatting, and tooling-query subsystems. Keeping every algorithm in a header
now has costs that scale with each new feature:

- the CLI, LSP, and tests repeatedly parse and compile the same implementation;
- an implementation-only edit invalidates every consumer that includes the
  changed header transitively;
- private implementation dependencies become transitive consumer dependencies;
- target boundaries cannot expose accidental reverse dependencies at link time;
- large headers encourage unrelated implementation growth in one file;
- build-time and binary-size effects are harder to attribute to one subsystem.

Header-only code does not inherently make the released compiler slow. The
current release build already optimizes the resulting native executables. This
proposal is primarily about development scalability, explicit ownership, and
keeping future compiler architecture changes reviewable.

## Goals

1. Compile substantial compiler algorithms once per build configuration rather
   than once per executable or test consumer.
2. Preserve `Frontend::analyze` as the shared CLI and LSP entry point and keep
   its source-loading, parsing, semantic, HIR, and MIR order unchanged.
3. Make include and link dependencies agree with documented phase ownership.
4. Keep data ownership, AST-address side tables, snapshot-scoped identities,
   diagnostics, and source provenance unchanged during mechanical migration.
5. Preserve release optimization and permit later IPO/LTO evaluation without
   coupling the migration to an unmeasured optimization policy.
6. Keep every intermediate commit buildable, testable, installable, and easy to
   revert at subsystem granularity.
7. Give agents a current map of which declarations are header-defined and which
   implementations require the compiled library.

## Non-Goals

- redesigning GTI language semantics;
- changing the frontend phase order;
- introducing a stable compiler-library ABI or shared library;
- publishing a general compiler SDK or CMake package in the first migration;
- splitting GTI source programs into C++-style translation units;
- rewriting a subsystem merely because its implementation moves to `.cpp`;
- introducing Pimpl wrappers for every compiler type;
- enabling LTO, PGO, `-march=native`, or new release optimization flags without
  measurements and a separate release-policy decision;
- combining the build-system proposal's future `gti_driver` extraction with
  frontend implementation movement.

## Target Architecture

The migration starts with one compiled core instead of one library per header:

```text
include/gti/                 declarations, value types, templates, facades
src/compiler/                frontend and middle-end implementations

                    +----------------------+
                    | gti_compiler STATIC  |
                    +----------+-----------+
                               |
              +----------------+----------------+
              |                |                |
            gti              gti_lsp          gti_tests
```

Later, after their contracts are independently usable, the target graph may
become:

```text
gti_compiler       source loading -> semantics -> HIR -> MIR -> optimization
       |
       +----------> gti_cpp_backend
       |                    |
       +--------------------+----------> gti_driver
                                             |
                                             v
                                            gti

gti_lsp ----------> gti_compiler + compiler-owned language queries
gti_tests ---------> the smallest target owning each behavior under test
```

`gti_cpp_backend` must consume frontend-owned facts through `BackendInput`.
`gti_driver` must own native processes and artifact policy without becoming a
dependency of `gti_compiler` or the LSP. The target graph is one-directional;
convenient linking is not permission to create cycles.

## Header And Source Rules

Keep these in headers:

- enums, IDs, immutable value records, AST/HIR/MIR data structures, and small
  result types needed by consumers;
- abstract interfaces and stable subsystem entry-point declarations;
- templates whose definitions must be visible at instantiation;
- short `constexpr`, trivial accessors, and mechanically obvious operations;
- ownership-relevant type definitions whose complete layout is required by a
  containing public value.

Move these to compiled sources:

- filesystem access, scanning loops, parser algorithms, semantic traversal,
  lowering worklists, CFG construction, optimization passes, and emission;
- large visitors and functions containing subsystem policy;
- non-template helpers used only by one implementation;
- standard-library dependencies that consumers need not parse;
- mutable implementation details that do not define a public value's layout.

Moving a definition is not permission to change behavior or rename the public
entry point. A phase's implementation source includes its own public header
first so missing declaration dependencies fail in that subsystem rather than
being supplied accidentally by a consumer.

Pimpl is not the default. GTI's executables and static libraries are released
together, so avoiding every internal rebuild is less valuable than keeping
ownership and data flow visible. Introduce an opaque implementation only when
measured rebuild cost or dependency isolation justifies its extra allocation,
lifetime, and debugging complexity.

## Installed Library Contract

Release archives historically install `include/gti/`. Once a declaration calls
compiled code, those headers must not be shipped without the matching archive.
Therefore the `gti_toolchain` component installs `libgti_compiler.a`,
`libgti_cpp_backend.a`, and `libgti_driver.a`, and release packaging requires
them.

The contract is deliberately narrow:

- installed `gti`, `gti_lsp`, headers, and static libraries come from one
  `VERSION` and build configuration;
- installed consumers use the exact-version `GTIConfig.cmake` package and its
  `GTI::compiler`, `GTI::cpp_backend`, `GTI::driver`, and `GTI::runtime`
  targets;
- release smoke coverage configures, compiles, and runs external compiler and
  driver clients against those staged imported targets;
- bundled releases install the pinned LLVM support archives required by the
  static compiler library; system-LLVM packages rediscover the exact LLVM CMake
  package used to build them;
- the archive is statically linked and adds no runtime loader dependency;
- symbol or data-layout compatibility across GTI versions is not guaranteed;
- a semantic-versioned SDK surface and stable ABI policy still require a later
  explicit proposal.

## Migration Principles

### Preserve authority

File movement must not create a second source of compiler truth. `SourceLoader`
still owns source identity and include edges, `Parser` owns grammar, semantics
owns resolved meaning, HIR owns concrete instances, MIR owns body-local effects,
optimization owns proven transformations, and the backend owns representation.

### Preserve snapshot lifetimes

`FrontendResult` owns `Program` before semantic, HIR, and MIR records whose side
tables refer to AST nodes. Migration may not retain raw AST pointers, symbol
IDs, or body-local IR IDs in a process-global cache or library singleton.

### Prefer subsystem seams over file-size splitting

Do not split a large header into arbitrary source fragments merely to reduce its
line count. A source file should represent a real responsibility such as
semantic registration, callable resolution, lifecycle analysis, or MIR body
lowering. If a subsystem needs multiple source files, keep one public facade and
name private files after those responsibilities.

### Measure both clean and incremental builds

A compiled source may slightly reduce cross-translation-unit inlining while
substantially improving developer rebuilds. Measure:

- clean Debug and Release builds;
- no-op builds;
- implementation-only rebuild of the migrated subsystem;
- rebuild after changing a public data structure;
- compiler executable size;
- representative GTI compilation latency.

Keep small hot operations inline when profiles justify it. Evaluate release IPO
or LTO separately; do not preserve a large inline implementation solely from an
assumption that it is faster.

#### Recorded baseline

Issue [#50](https://github.com/LukeHHA/gti/issues/50) records the first
reproducible baseline at `977879d` on Apple clang/arm64 with eight-way
parallelism:

- touching `semantic_analyzer.h` rebuilt 11 translation units in a Debug build,
  taking 31.4 seconds wall and 208 seconds CPU;
- compiling the 141-line `src/driver/compilation.cpp` at `-O2` took 20.6
  seconds after preprocessing to 131,098 lines;
- compiling the 22,702-line `tests/compiler_tests.cpp` at `-O1` took 27.0
  seconds;
- `semantic_analyzer.h` was 28,032 lines and 1.14 MB, which identified phase 3
  as the highest-leverage implementation migration.

GitHub release run
[31710836803](https://github.com/LukeHHA/gti/actions/runs/31710836803) supplies
the corresponding workflow baseline for `977879d`. The `darwin-x64` package
job took 30 minutes 17 seconds: configuration took 2 minutes 16 seconds, the
cold bundled-LLVM and GTI build took 17 minutes 37 seconds, serial build-tree
tests took 2 minutes 44 seconds, and installed-toolchain verification took 7
minutes 26 seconds. The release workflow now uses an exact-input compiler cache
and four-way CTest scheduling. Warm-cache improvement must be read from later
workflow evidence; it is not treated as completion of phase 3.

After the initial phase 3 extraction, the public semantic header was 3,479
lines. An implementation-only edit compiled `semantic_analyzer.cpp` once, then
relinked consumers. On the same local arm64 development machine used for
migration that path took 5.89 seconds wall, with no CLI, LSP, driver, or test
source recompilation. These numbers are directional rather than a cross-machine
comparison with issue #50; clean Release workflow measurements remain the
release evidence.

The completion pass moved `SemanticDatabase`, `SemanticModel`, place/trait and
array-extent policy, and semantic type presentation into compiled sources. The
header is now 1,856 lines of records, storage, declarations, and small value
operations. Touching `semantic_model.cpp` rebuilt one compiler object and
relinked consumers in 2.89 seconds without recompiling CLI, LSP, driver, or test
sources. Formatter-only changes likewise rebuild one object; the recorded local
path took 1.15 seconds before relinking.

### Completion validation

The final local validation on Apple clang 21/arm64 used four build workers and
the exact pre-migration `d4c80cb` checkpoint as its controlled comparison. Both
comparison builds used Release mode, system LLVM 22.1.8, disabled tests and LSP,
and built the `gti` target from an empty object tree:

- the clean `gti` build fell from 33.34 seconds to 20.54 seconds, a 38 percent
  wall-time reduction;
- touching the public `semantic_analyzer.h` boundary and rebuilding fell from
  30.28 seconds to 18.36 seconds, a 39 percent wall-time reduction;
- the Release executable changed from 6,430,528 bytes to 6,429,520 bytes, so
  the migration introduced no binary-size growth;
- ten alternating `examples/56-sum-types.gti` frontend traces measured 2.155 ms
  before and 2.084 ms after at the median across source loading, parsing,
  semantics, HIR lowering, and MIR lowering. Emitted C++ was byte-identical.

A separate empty Debug tree configured in 1.41 seconds, built all 86 compiler,
tooling, and test steps in 20.29 seconds, and completed a no-op build in 0.01
seconds. An empty self-contained Release tree configured against the already
downloaded pinned LLVM 20.1.0 source in 8.44 seconds, built LLVM's required
support subset plus all 294 GTI/tooling/test steps in 57.88 seconds, and
completed a no-op build in 0.04 seconds. Source download time is deliberately
not included in those build measurements.

Both clean trees passed all 29 CTest targets. The staged Release toolchain then
passed the direct CLI, project CLI, and LSP smoke suites; an external
`find_package(GTI CONFIG)` consumer passed all six installed-library tests; and
`PackageRelease.cmake` produced the expected `v0.130.0` darwin-arm64 archive.
The full optional language audit, Tree-sitter corpus, capture tests, shipped
source parse, format check, and generated-parser cleanliness checks also passed.

## Phased Implementation

### Phase 0: freeze build and behavior contracts

Status: implemented by existing coverage and reinforced by phase 1

- Retain the `compiler_pipeline`, `cli_workflow`, and `lsp_protocol` tests.
- Keep direct CLI options and installed resource discovery unchanged.
- Record the compiled-library direction in compiler and agent architecture
  documentation.
- Require each migrated subsystem to have focused behavior coverage.

Acceptance criteria:

- a baseline complete build and focused tests are recorded before movement;
- the worktree is clean apart from task-owned migration changes;
- no user-visible language or CLI change is included in a migration phase.

### Phase 1: compile the lexer subsystem

Status: implemented

- Change `gti_compiler` from an interface target to a static library.
- Keep `Lexer` and `LexDiagnostic` declarations in `include/gti/lexer.h`.
- Move file reading, token scanning, completion-token insertion, literal
  decoding, keyword selection, and lexical diagnostic construction to
  `src/compiler/lexer.cpp`.
- Keep token kinds, token values, keyword identity, and token formatting in
  `include/gti/token.h`.
- Link the CLI, LSP, and compiler tests to the same static library.
- Install the exact-version compiler archive and verify a staged release can
  compile and run a small lexer client.

Why lexing first:

- it is a complete frontend phase with an already narrow API;
- its state is instance-owned and has no AST-address lifetime coupling;
- it is used by direct compiler, source loading, completion, formatting tests,
  LSP fallback paths, and diagnostics;
- existing tests cover valid tokens, aliases, numeric and character literals,
  completion, source names, error spans, and stable `GTI-L` codes;
- it proves consumers can link compiled compiler behavior before larger phases
  move.

Acceptance criteria:

- `lexer.h` contains declarations and state, not scanning algorithms;
- `gti_compiler` produces a static archive in Debug and Release builds;
- existing lexer, frontend, CLI, and LSP behavior remains unchanged;
- the installed headers and archive pass the compiler-library smoke test;
- release archives fail packaging if the compiler archive is missing.

### Phase 2: compile source loading and parsing

Status: implemented

- `SourceLoader` filesystem, include-resolution, cycle, visibility-edge,
  provenance, and source-manager algorithms now live behind its existing API.
- Parser algorithms now compile in `src/compiler/parser.cpp`; `parser.h` keeps
  only the facade, AST result types, diagnostics, and ownership contract.
- Preserve per-source-unit parsing and dependency-ordered program assembly.
- Keep completion recovery and exact diagnostic spans unchanged.

Acceptance criteria:

- quoted and standard-library include rules retain focused coverage;
- load-once, cycles, direct visibility, prelude, overrides, and completion work;
- parser recovery and all grammar tests remain byte-for-byte compatible where
  diagnostics are asserted;
- implementation-only edits rebuild the compiler library without recompiling
  CLI or LSP sources.

### Phase 3: separate semantic data from semantic algorithms

Status: implemented

- Semantic record types, IDs, query facades, and snapshot ownership remain in
  declarations visible to HIR, backend, and language queries.
- Registration, inheritance, lookup, overload selection, flow analysis,
  lifecycle calculation, occurrence finalization, and generic reanalysis now
  compile behind `SemanticVisitor` in `semantic_analyzer.cpp`.
- Preserve the documented semantic prepass order exactly.
- Add private implementation headers only beneath `src/compiler/`; do not make
  downstream phases depend on semantic visitor internals.

Acceptance criteria:

- every `SemanticModel` consumer builds without access to private analysis
  helpers;
- symbolic and concrete generic analysis still use one semantic rule system;
- diagnostic codes, related locations, and fix-its remain stable;
- a semantic implementation edit no longer recompiles CLI or LSP sources.

### Phase 4: compile HIR and MIR lowering

Status: implemented

- Keep IR value types, IDs, validation results, and read-only consumer APIs in
  headers.
- Move instance worklists, concrete generic lowering, expression dispatch, MIR
  body construction, CFG cleanup, use indexing, and validation algorithms into
  compiled sources.
- Preserve ID scope, source provenance, dispatch identity, move/loan/drop
  effects, and constructor ordering.

`HirLowerer` now exposes a request-owned facade over its compiled instance
worklist and expression lowering. Stateless `MirLowerer` exposes an out-of-line
lowering API while its body builder remains private to the implementation.
Touching `mir_lowering.cpp` on the migration machine rebuilt one compiler
object and relinked consumers in 2.97 seconds without recompiling consumer
sources.

Acceptance criteria:

- HIR dynamically discovered instances are not replaced by a fixed pass;
- MIR validation covers the same structures before and after movement;
- optimizer and backend consume public IR contracts only;
- focused HIR/MIR tests and the full compiler pipeline pass.

### Phase 5: compile optimization infrastructure

Status: implemented

- Keep the optimization-level and pipeline facade small.
- Put pass implementations, analysis caches, invalidation, effect
  classification, and verification in compiled sources.
- Follow the MIR ownership and capability milestones rather than freezing the
  current HIR replacement mechanism into a library boundary.

MIR editing, dominance, effects, verification, deterministic printing, the
MIR identity-folding pass, and the compatibility HIR constant-folding pass now
compile in `src/compiler/`. Touching `optimizer.cpp` on the migration machine
rebuilt one compiler object and relinked consumers in 2.21 seconds without
recompiling consumer sources.

Acceptance criteria:

- `-O0` remains behaviorally unchanged;
- each enabled pass is deterministic and verifier-checked;
- backends consume one authoritative optimization result;
- optimization implementation changes have a bounded rebuild surface.

### Phase 6: compile and isolate the C++ backend

Status: implemented

- Introduce `gti_cpp_backend` when `BackendInput` is sufficient to prevent
  reverse dependency on CLI policy.
- Move C++ emission algorithms and representation-only helpers out of public
  headers.
- Continue the planned migration from AST traversal toward optimized MIR
  without making the library split itself a backend rewrite.

`CppEmitter` now exposes a narrow compiled facade, `CppBackend` generates only
from `BackendInput`, and representation algorithms live in the separately
installed `gti_cpp_backend` archive. `gti_driver` privately links that target;
`gti_lsp` links only `gti_compiler`. Touching `cpp_emitter.cpp` on the migration
machine rebuilt one backend object and relinked backend consumers in 2.52
seconds without rebuilding the compiler archive or LSP.

Acceptance criteria:

- frontend and LSP targets do not compile C++ emission implementation;
- the backend does not infer language meaning from source spelling;
- C++20 and C++23 output and native execution tests pass;
- generated artifacts change only when an intentional backend change is
  separately reviewed.

### Phase 7: extract the native driver

Status: extraction complete; initial project orchestration implemented

- Implement `gti_driver` as a compiled library over immutable compilation and
  native-toolchain requests.
- Keep manifest, cache, dependency, process, and artifact policy outside
  `gti_compiler`.
- Make the CLI a router and presentation boundary.

The compiled and installed `gti_driver` now owns `CompilationRequest`,
`NativeCompileRequest`, toolchain resource discovery and validation, native
process execution, temporary artifact lifetime, schema-versioned manifest
parsing, project discovery, profile and target resolution, and the initial
executable build plan. It also owns the workspace-local verified whole-program
cache and compiler/toolchain/input identity; the CLI constructs requests and
presents those results. The driver now also resolves canonical workspaces and
source-only path dependency graphs. `gti_compiler` consumes only the resulting
immutable `PackageSourceRoot` values for include loading; TOML, workspace
selection, acquisition, cache, and artifact policy remain outside it.

### Completion pass: shared compiler and tooling algorithms

Status: implemented

The final cross-cutting pass compiled the remaining substantial non-template
algorithms without changing their public contracts:

- frontend orchestration, source-graph operations, source locations,
  executable/resource discovery, and the token keyword table;
- semantic database/model operations, place and trait policy, array-extent
  evaluation, semantic/signature printing, and shared constant evaluation;
- HIR/MIR aggregate queries and dominance traversal;
- AST printing, formatting, and hover/completion/definition queries.

`AstPrinter` remains an `ExprVisitor`, and `SignaturePrinter` retains its public
methods and value layout; their implementations are simply out of line. The
installed compiler smoke compiles and executes those contracts against
`GTI::compiler`. Headers intentionally continue to define AST/HIR/MIR records,
templates, `constexpr` tables and mappings, and trivial ownership/accessor
operations. Moving those would either remove compile-time API behavior or turn
plain data models into opaque allocation boundaries, neither of which is a
migration goal.

## Testing And Verification

For every migration phase:

```sh
cmake -S . -B build
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
python3 scripts/local_language_audit.py --full
git diff --check
```

Also configure a clean Release build with `GTI_RELEASE_BUILD=ON`, build and test
it, install the `gti_toolchain` component into a temporary prefix, run CLI and
LSP installed-toolchain smoke tests, configure the external installed-library
smoke project through `find_package(GTI CONFIG)`, and package the staged tree.

Use compiler-specific warnings or sanitizers as optional validation, not as a
new language rule. When comparing generated C++, use identical GTI inputs and
optimization levels; a pure implementation migration should produce identical
artifacts.

## Release And Version Policy

A mechanical internal migration with no shipped artifact or behavior change
does not by itself require a `VERSION` increase. Phase 1 does advance `VERSION`
because it changes the installed archive contract: release packages now ship
the static compiler library required by installed compiler headers.

Advance `VERSION` when a migration also changes compiler, CLI, LSP, standard
library, runtime, package, or diagnostic behavior. Never hide a user-visible
change inside a build-only migration commit.

The release workflow must use the same Release configuration for
`gti_compiler`, `gti`, `gti_lsp`, and tests. Future IPO/LTO work must enable and
verify the property on the relevant compiled targets instead of relying on a
header-only single translation unit.

## Risks And Controls

### Accidental behavior changes

Control: move declarations and definitions mechanically, retain method
signatures, and run focused plus full tests before cleanup or refactoring.

### Lost inlining or runtime regression

Control: benchmark representative compilation, keep proven small hot paths
inline, and evaluate IPO/LTO. Do not respond by returning entire algorithms to
headers without measurements.

### Static initialization and global state

Control: keep compiler state owned by request, frontend result, or subsystem
instances. Do not introduce mutable library globals while moving code.

### ABI misunderstanding

Control: ship headers and archive from one version, document the exact-version
contract, use static linking, and avoid promising an exported SDK until it has
an explicit compatibility policy.

### Target cycles

Control: keep dependencies aligned with compiler phase direction and reject a
backend, driver, or LSP dependency from the frontend core.

### Migration becoming a rewrite

Control: each phase moves one subsystem, preserves behavior, and leaves design
changes for a separate review. A subsystem may be internally split only when
the split reflects already-documented ownership.

## Completion Criteria

The migration is complete when:

- substantial non-template compiler algorithms live in compiled sources;
- CLI, LSP, tests, and future project driver share those implementations;
- implementation-only edits have bounded rebuild surfaces;
- frontend, optimizer, backend, and driver targets follow one-directional
  dependencies;
- installed headers always have the exact-version static libraries they need;
- Debug and Release builds, installed smoke tests, and packaging pass on every
  supported release platform;
- compiler latency and binary size have no unexplained regression;
- documentation and agent navigation identify declarations and implementations
  without requiring source-tree inference.
