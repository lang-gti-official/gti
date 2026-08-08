---
name: gti-build-architecture
description: Design, review, or implement GTI's project build and package architecture. Use for gti.toml, gti_driver, direct compiler compatibility, gti build/run/check/test/clean/fetch/metadata commands, manifests, targets, profiles, native linking, build DAGs, caching, workspaces, dependency source roots, lockfiles, package resolution, build diagnostics, or LSP project configuration.
---

# GTI Build Architecture

Add a modern project workflow without weakening GTI's permanent,
C++-familiar direct compiler driver.

## Start Every Task

1. Run `git status --short` and preserve unrelated changes.
2. Read `docs/build-system-proposal.md` completely. Treat it as the planned
   architecture, not evidence that a milestone is already implemented.
3. Inspect the live implementation before choosing a seam, especially
   `src/cli/main.cpp`, `include/gti/frontend.h`, `source_loader.h`,
   `source_graph.h`, `target.h`, CMake installation rules, README CLI
   documentation, and `tests/cli_smoke_test.py`.
4. Identify the proposal milestone and acceptance criteria the task advances.
   Avoid pulling later package, ABI, registry, or build-hook decisions into an
   earlier milestone.
5. Establish a direct-mode baseline before changing driver behavior.
6. Use `$gti-language` as well when the change affects source syntax,
   visibility, frontend semantics, target conditionals, runtime bindings, HIR,
   MIR, or backend contracts.

## Permanent User Contract

GTI has two complementary modes:

```text
Direct mode:   gti main.gti -O2 --std c++20 -o main -- <native flags>
Project mode:  gti build|check|run|test|clean|fetch|metadata ...
```

Keep these invariants:

- Direct mode remains manifest-independent and accepts one `.gti` entry source.
- Preserve `-o`, `-O0` through `-O3`, `--cxx`, `--std`, `--emit-cpp`,
  `--keep-cpp`, `--verbose`, and native arguments after `--`.
- A project command may discover `gti.toml`; compiling a `.gti` path may not.
- C++ backend standard selection is not a GTI language edition.
- Familiar spelling does not justify fake support for `-c`, multiple C++-style
  translation units, `-E`, or `-D` macros.
- Project mode reuses the same compilation request and language pipeline as
  direct mode; it does not fork compiler behavior.

Add compatibility aliases only when their meaning is exact and tested. Never
repurpose an existing option silently.

## Current Compiler Facts

- The current CLI is a direct whole-program driver in `src/cli/main.cpp`.
- One entry source and its `SourceGraph` produce one backend artifact and one
  native compiler invocation.
- `SourceLoader` owns canonical source identity, includes, load-once behavior,
  cycles, standard-library roots, and direct visibility edges.
- `Frontend` owns the shared compiler phase ordering used by CLI and LSP.
- `gti_compiler` is a compiled static library; the lexer is the first subsystem
  migrated from header implementation into `src/compiler/lexer.cpp`.
- The CLI currently selects `TargetInfo::host()` inside compilation and owns
  toolchain discovery, temporary C++, process execution, and native output.
- GTI has no stable binary module boundary or cross-version language ABI.

Confirm each fact in current code because the proposal is staged and the
repository evolves quickly.

## Ownership Boundaries

| Component | Owns | Must not own |
| --- | --- | --- |
| CLI router | mode selection, argument syntax, help, presentation, exit status | manifest semantics, source semantics, cache algorithms |
| `gti_driver` | compilation requests, manifests, target/profile resolution, native toolchain, build planning, artifacts | GTI parsing, type checking, LSP protocol state |
| `Frontend` | source loading through typed HIR/MIR and diagnostics | TOML, project discovery, native processes, output directories |
| `SourceLoader` | source-unit and declared package-root resolution | dependency downloads, profile selection, link flags |
| Backend | checked program to backend artifact | manifest lookup, dependency resolution, process execution |
| LSP | document overlays, immutable snapshots, protocol conversion | builds, fetching, cleaning, lockfile mutation |

Implement `gti_driver` as a separately compiled library rather than making the
reusable compiler frontend depend on TOML, process execution, or mutable
caches.
Keep `src/cli/main.cpp` thin enough that direct and project modes construct
shared driver requests instead of duplicating orchestration.

## Compilation Requests And Build Steps

Resolve policy before compiler execution. A request should carry the entry,
standard-library layout, target, optimization, C++ standard, backend, and
declared source roots as immutable values. Do not select the host target again
inside a lower phase.

Build project targets at the granularity GTI actually implements:

```text
ValidateManifest
  -> ResolveTarget
  -> AnalyzeSourceGraph
  -> GenerateBackendArtifact
  -> InvokeNativeCompiler
  -> PublishArtifact
```

`check` stops after analysis, emitted-C++ workflows stop after backend
generation, and `run` adds execution after publication. Do not invent one
native object node per `.gti` file while GTI emits one whole-program artifact.

Every step needs declared inputs, outputs, options, dependencies, cache policy,
and structured failure data. Never rely on incidental execution order to imply
an edge.

## Manifest Rules

Keep `gti.toml` declarative, versioned, and small:

- Parse it with a conforming pinned TOML implementation linked into the
  installed toolchain.
- Require `manifest-version` and reject unsupported versions.
- Resolve paths relative to the manifest and validate package containment.
- Reject unknown fields with an exact span and nearest-name suggestion.
- Do not interpolate arbitrary environment variables.
- Apply precedence as CLI, target, profile, package, then tool default.
- Do not accept and ignore future fields such as `edition` or `library`.
- Let target roots define source graphs; do not require source-file lists or
  globs for ordinary builds.

Route manifest and planning failures through `Diagnostic` with a dedicated
`GTI-Bxxxx` family when source spans exist. Keep native compiler failures
clearly labeled as generated-backend failures.

## Native Toolchain

Extract the existing compiler discovery, runtime discovery, command assembly,
process execution, captured output, and temporary-file policy without changing
direct-mode behavior first.

Represent common native inputs structurally: compiler, C++ standard, compile
arguments, linker arguments, include directories, library directories,
libraries, frameworks, runtime files, and compatibility includes. Pass process
arguments as a vector and never through a shell-concatenated command.

Keep raw native arguments available as an explicit escape hatch. Hash their
ordered values into cache identity and show the exact native command under
`--verbose`.

## Caching And Outputs

Add caching only after uncached project builds are correct. Start with one
whole-program backend/native artifact key. Include compiler/runtime identity,
effective configuration, target, optimization, backend, C++ standard, every
loaded source content hash, logical dependency edges, standard-library inputs,
native compiler identity, ordered native flags, and locked dependencies.

Do not use canonical path alone as content identity or hash undeclared ambient
environment state. Cache corruption must fail safely, and deleting the cache
must never damage sources or published artifacts.

Keep project outputs under a validated tool-owned subtree such as
`build/gti/<profile>/<target-triple>/`. `gti clean` must resolve and validate
that boundary before deletion and refuse roots, broad parents, and unresolved
paths. Direct mode retains its current output behavior.

## Dependencies And Lockfiles

Implement dependency capability in this order:

1. no external dependencies;
2. declared path dependencies;
3. package-root include resolution with direct visibility;
4. pinned Git dependencies and `gti.lock`;
5. registry evaluation only after governance and trust are designed.

The main package controls replacements. Preserve direct dependency visibility;
do not leak transitive or sibling declarations. `--locked` rejects manifest
drift, `--offline` rejects required network access, and `gti fetch` acquires
source without compiling or executing package code.

Arbitrary dependency build scripts are out of scope. A future build hook must
declare inputs, outputs, host/target execution, permissions, cache identity,
and network policy before it can be considered deterministic.

## LSP Project Configuration

Expose a reusable resolved project model or stable metadata query. The LSP may
consume target, source-root, and configuration facts, but it must not reparse
manifest semantics independently or invoke a mutating CLI subprocess merely to
open a document.

Manifest or lock changes advance a configuration generation and invalidate
affected snapshots. Preserve support for standalone `.gti` files with no
manifest. Never fetch dependencies, run hooks, build, clean, or rewrite a
lockfile as an editor-open side effect.

## Change Workflow

1. State the current direct behavior and the project behavior being added.
2. Select the earliest proposal milestone that can express the change.
3. Refactor a shared request or driver seam before adding a second code path.
4. Keep source semantics in the frontend and project policy in the driver.
5. Add focused invalid manifest, path-safety, and native-failure diagnostics.
6. Test direct compatibility and project behavior together.
7. Update `docs/build-system-proposal.md`, README commands, and installed
   behavior when a decision becomes implemented rather than merely proposed.
8. Advance `VERSION` for shipped CLI, driver, runtime, or package behavior in
   accordance with the repository release policy.

## Verification

For direct-driver or extraction changes, cover:

```sh
cmake --build build -j4
ctest --test-dir build --output-on-failure -R 'compiler_pipeline|cli_workflow'
```

Add CLI workflow cases for default outputs, `-o`, every optimization level,
C++20/C++23, emitted and retained C++, `--cxx`, `--verbose`, native arguments,
installed resources, and invocation outside a manifest.

For project work, add unit coverage for manifest spans, schema validation,
precedence, target selection, containment, planning, cache keys, and lockfiles;
then add end-to-end CLI coverage from the project root and nested directories.
Test macOS, Linux, and Windows path differences in CI where applicable.

Run `git diff --check` and confirm unrelated worktree changes are not staged.

## Non-Goals Until Explicitly Designed

- replacing CMake for building the GTI compiler;
- mandatory manifests for direct compilation;
- arbitrary `build.gti` programs or shell hooks;
- a central registry;
- binary GTI modules or stable ABI promises;
- C++-style separate translation units;
- source globs and manual compilation order;
- implicit environment-driven semantics;
- treating backend C++ versions as GTI editions.
