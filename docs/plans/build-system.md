# GTI Build And Package System Proposal

> **Plan status:** Non-canonical staged work. Implemented direct/project
> behavior is summarized in
> [`docs/architecture/build-and-driver.md`](../architecture/build-and-driver.md).

Status: implementation in progress; Milestones 0 through 6 are complete for
local packages, including native dependency composition

Operational ordering is maintained in
[`implementation-sequence.md`](implementation-sequence.md). At the current
checkpoint the completed project test targets, whole-program cache, and
workspace/path dependencies are followed by locked Git dependencies; the
milestone numbers below
remain the detailed domain decomposition rather than a competing live queue.

This document proposes a staged project build system for GTI. It deliberately
preserves GTI's existing compiler-driver workflow while adding a modern,
declarative project workflow. The first implementation is an orchestration
layer over the existing whole-program compiler; it is not a package registry,
a binary module system, or a second language for writing build scripts.

## Current Implementation

The direct compiler contract is frozen and covered by CLI tests. The compiled
`gti_driver` library now owns immutable whole-program compilation requests,
resolved toolchain resources, structured C and C++ native compile requests,
native command construction and process execution, and temporary
generated-artifact lifetime. Declared native source objects and linker output
are staged beside their destinations and published only after successful
invocations, so a failed native tool cannot truncate a previous object or
executable. Direct outputs are also checked against every loaded source
identity before writing or publication, including symbolic-link and hard-link
aliases. `src/cli/main.cpp` retains argument routing, diagnostic rendering,
user-facing output, and exit-status policy.

One resolved `TargetInfo` is selected before compilation and passed unchanged
to frontend semantics, optimization, and backend generation. Native inputs are
represented as ordered argument, include, library, and framework collections;
processes are invoked directly from an argument vector without a shell.
`gti_compiler` has no dependency on the driver.

Direct mode and project builds now resolve the same explicit execution-profile
fact. The direct option is
`--execution-profile single-threaded|concurrent`; project
`[profiles.<name>]` tables use the optional `execution-profile` string and the
CLI may override it. The default is single-threaded. This is C-GLOBAL-01
language-policy selection, not a native thread flag or a public runtime
capability.

Manifest schema version 1 also accepts `[workspace].members`, optional
`[package].source-root`, and source-only `[dependencies]` path tables.
Workspace members share `build/gti`, with package artifacts under
`build/gti/packages/<package>/...`; `--package` selects a root/member package.
`#include <alias/unit>` resolves only through the including package's direct
alias map. Cycles, duplicate canonical roots/names, nested workspaces, and
missing source roots fail before compilation. No command in this phase performs
network access.

`[targets]` is optional for a source-only dependency package. Selecting that
package as a command target is diagnosed; depending on and including its source
does not require a dummy executable/test declaration.

Project mode supports `gti build`, `gti check`, `gti run`, `gti test`,
`gti clean`, and `gti metadata`. It discovers `gti.toml` upward from the
working directory,
parses TOML 1.0 with vendored toml++ v3.4.0, validates manifest schema version
1 with exact source spans, resolves executable/test targets and named profiles,
and writes artifacts beneath
`build/gti/<profile>/<arch>-<vendor>-<os>/` for standalone packages (and the
package-qualified workspace layout above), with verified-payload whole-program
cache entries beneath `build/gti/cache/v2/`. Declared native C/C++ sources,
declared include directories, and content-complete exact link files now join
cache identity through native dependency discovery; opaque native argument
vectors, name-resolved libraries/frameworks, native library search
directories, non-content-complete link inputs, and dependency-injecting
native environment search paths still bypass the cache until their exact
inputs can be identified. `gti build --all [--jobs <count>]` builds every
declared target through bounded concurrent child builds with deterministic
name-ordered output and byte-identical serial/parallel artifacts. Plain project commands select the
`dev` profile; `--release` is an exact alias for `--profile release`. Only the
selected profile directory is created, a symbolic-link package trust root is
rejected, and existing symbolic-link components or artifact leaves below the
package root are never traversed for managed output,
and build/check status
identifies the effective target, profile, target triple, and artifact or source
path.

Package, target, and profile names use one portable artifact-name rule. The
ASCII manifest spelling remains case-sensitive for selection, but names that
case-fold to the same path identity and Windows reserved device names are
rejected before output planning or scaffolding mutates the filesystem.

`check` stops after shared frontend analysis without emitting C++ or invoking a
native compiler. `run` builds through the normal atomic publication path and
then invokes the executable with inherited standard streams and the exact
arguments after `--`. `test` selects every test target in deterministic name
order, or one named test, and builds each root as an independent whole program.
Runtime failures do not prevent later tests from running; the command reports a
summary and propagates the first failing status. `clean` resolves a valid
workspace and removes its shared validated `build/gti` subtree; if project
resolution fails, it falls back to the nearest literal package subtree so a
broken manifest can still be cleaned. Both paths refuse symbolic-link or
filesystem-root escapes. `metadata` enumerates all manifest targets,
profiles, and planned output paths as deterministic schema-versioned JSON
without compiling or creating build directories.

Project scaffolding supports `gti new <path>` and `gti init [path]`. `new`
requires a destination that does not exist. `init` requires an existing
directory, preserves an existing regular `src/main.gti`, and refuses to replace
an existing `gti.toml`. Both commands derive the package name from the
destination unless `--name` supplies one, validate the manifest's portable-name
rule, and generate one schema-version-1 executable target. A generated entry
uses the owned-argument `int main(int, std::vector<std::string>)` form and
includes the standard string and vector modules. They do not build the project,
initialize version control, or consult a parent manifest.

Direct `gti source.gti` compilation remains manifest-independent, including
when an invalid manifest is present beside the source. Project native inputs
are accepted at package, profile, and target scope, selected from the resolved
target, and passed through the shared native request. Declared `.c` inputs are
compiled with a separately resolved C compiler; declared `.cpp`, `.cc`, and
`.cxx` inputs use the existing resolved C++ compiler and profile policy. Both
become managed link objects. Structured paths are validated within the package;
exact argument arrays use the documented trusted escape-hatch policy. Eligible
project build/run/test requests use the whole-program cache unless `--no-cache`
is selected; declared native sources, native search directories, opaque native
argument vectors, native link operands, and name-resolved
libraries/frameworks bypass it.

Pinned git dependencies are implemented:
`alias = { git = "<url>", rev = "<full 40-hex commit>" }` is the only accepted
git form, `gti fetch` is the sole writer of the workspace `gti.lock`
(lock-version 1, sorted entries with `sha256:` tree checksums and dependency
edges), and build/check/run/test verify lock coverage plus stored-tree
checksums and locked package identity before source loading. `--offline` and
`--locked` refuse acquisition; a plain build may materialize a lock-covered
missing checkout. Branch/tag selectors and registry ranges remain rejected.
Package-level native inputs on a dependency are rejected until native
dependency composition is defined.
is selected; declared native sources, include directories, and
content-complete exact link files join cache identity through native
dependency discovery, while opaque native argument vectors, native search
directories, name-resolved libraries/frameworks, and other link operands
still bypass it. Git/registry dependency declarations and fetch
options are not implemented and are rejected. A `gti.lock` file is not yet
consumed or authoritative; its presence has no effect. Package-level native
inputs on a dependency compose into every dependent build as isolated
per-package groups: structured contained inputs (include directories, C and C++
sources, the C standard, library directories, link files, libraries,
frameworks, and selected platform fragments) compose, and from the argument
vectors only validated `-D<name>[=<value>]` and `-U<name>` macro definitions
compose. Any other compiler argument and any linker or raw argument on a
dependency is rejected with `GTI-B1606` at that dependency's declaration. A
group's include directories and macros apply only to that package's own
declared native sources — never to the root package's sources, another
package's sources, or the generated whole-program translation unit — and link
operands append in deterministic dependents-before-dependencies order.
Dependency target-scope native tables do not compose; they apply only when
that package builds directly. Plans carrying dependency native groups bypass
the whole-program cache until dependency discovery covers group sources.

## Decision Summary

GTI should support two permanent entry modes through the same `gti` executable:

1. **Direct compiler mode** keeps the current C++-familiar command shape:

   ```sh
   gti main.gti -o main
   gti main.gti -O2 --std c++20 -o main
   gti main.gti --emit-cpp -o main.cpp
   gti binding.gti --emit-native-header -o binding.native.h
   gti main.gti -o main -- -Iengine/include -Lengine/lib -lengine
   ```

2. **Project mode** discovers a declarative `gti.toml` manifest:

   ```sh
   gti build
   gti build chip8 --release
   gti check
   gti run chip8 -- roms/pong.ch8
   gti new hello
   gti init
   gti test
   ```

Direct mode must remain useful for examples, small programs, compiler tests,
shell scripts, editor integrations, and C++-oriented users. Project mode should
own target discovery, profiles, dependency resolution, caching, repeatable
native linking, workspaces, and eventual package acquisition.

The project system should internally use a target DAG, but users should not
normally write that graph. GTI source dependencies already form a graph through
`include`; the manifest should describe package-level intent and native inputs,
not duplicate every source edge.

## Motivation

Before Milestone 1, the CLI implemented a complete and intentionally small
compiler driver directly. The same workflow now crosses the compiled driver
boundary:

```text
entry .gti file
  -> source loading and direct include graph
  -> frontend analysis
  -> HIR/MIR and optimization
  -> C++ backend artifact
  -> native C++ compiler invocation
  -> executable
```

That workflow should remain valid. It becomes insufficient when a project has:

- multiple executable or test targets;
- repeatable debug and release settings;
- native libraries or platform frameworks;
- local, Git, or registry dependencies;
- generated inputs;
- multiple packages developed together;
- cross-target builds;
- a need for incremental artifact reuse;
- reproducible dependency resolution.

Requiring every project to create a CMake program would solve those cases at
the cost of making a second build language part of ordinary GTI development.
Conversely, hiding the existing direct driver behind a mandatory manifest would
discard a familiar and useful property of compiled languages. The two-mode
model retains both strengths.

## Design Principles

### Preserve the direct driver

The following existing behavior is a compatibility surface:

- one `.gti` entry source identifies one whole program;
- `-o` and `--output` select the resulting path;
- `-O0` through `-O3` select GTI and matching native optimization;
- `--cxx` selects the native compiler;
- `--std c++20` and `--std c++23` select C++ backend compatibility;
- `--emit-cpp`, `--emit-native-header`, `--keep-cpp`, and `--verbose` retain
  their current meaning;
- accepted arguments after `--` are passed to the native C++ compiler, while
  driver-owned output, build mode, standard, optimization, target, sysroot,
  data-layout, and response-file overrides are rejected;
- the default output remains beside the entry source when `-o` is omitted.

Project support must be added without silently consulting a nearby manifest
when direct mode is selected. This keeps a command such as
`gti scratch.gti -o scratch` independent of the directory it happens to be in.

The direct driver may add compatibility spellings such as
`--std=c++23`, `-std=c++23`, and an unambiguous `--cpp-std c++23`, but the
current `--std c++23` form must remain accepted. Native compiler flags remain
explicitly separated after `--`; GTI should not accidentally interpret C++
preprocessor flags as GTI language features.

Milestone 0 keeps `--std <c++20|c++23>` as the sole standard-selection
spelling. Aliases are not part of the frozen compatibility surface and should
be added only as a separately tested CLI extension with exactly equivalent
meaning.

Familiarity does not require pretending unsupported compilation models exist.
GTI should not add `-c`, multiple independent input translation units, `-E`, or
`-D` language macros until GTI has deliberately designed separate compilation,
a stable link interface, preprocessing, or equivalent language-level concepts.

### Prefer a declarative manifest

An ordinary project should not execute arbitrary build code merely to state
its targets and dependencies. A declarative manifest is inspectable by the CLI,
LSP, release tooling, dependency auditing, and future package services without
running project-controlled code.

The initial format should be TOML in `gti.toml`. The implementation should use
a conforming, pinned parser rather than gradually inventing a TOML dialect.
The parser must be linked into the toolchain so installed GTI builds do not gain
a new runtime dependency.

### Infer source edges

The manifest identifies an entry source for each target. `SourceLoader` remains
the authority for the source-unit graph. Users do not list every `.gti` file in
the manifest, and project discovery must not introduce textual inclusion or
global declaration visibility.

### Keep build policy outside the language frontend

`gti.toml`, profiles, native libraries, cache paths, and dependency acquisition
are driver concerns. They must not become parser nodes or semantic declarations.
The frontend receives a fully resolved compilation request containing an entry
path, target information, source roots, standard-library layout, optimization
level, and overlays where applicable.

### Be deterministic by default

A build plan should depend on declared inputs. Dependency versions and Git
revisions are recorded in `gti.lock`. Dependency code must not execute build
scripts during the initial implementation. Network access should be separable
from compilation through `gti fetch`, `--locked`, and `--offline` behavior.

### Keep native interoperability explicit

GTI currently lowers to C++ and direct mode supports native compiler arguments
after `--`. This is the shipped link path for bounded `extern "C"`
declarations, for example:

```sh
gti main.gti -o main -- -Lvendor/lib -lfoo
```

The declaration fixes the native symbol and call ABI; it does not supply a
library. Project mode provides structured `native` tables at package, profile,
and target scope. Their target-selected include paths, library paths, exact link
files, library/framework operands, and compiler/linker arguments feed the same
native request as direct mode. `run --` remains reserved for the executed
program, and project `build` and `check` do not acquire an ambiguous trailing
argument channel. See
[`docs/language/native-c-interop.md`](../language/native-c-interop.md) for the
language-side boundary.

## User Model

### Package

A package is one versioned project described by one `gti.toml`. A package may
contain multiple named targets. Package identity is separate from source-unit
identity and from a future native ABI identity.

### Target

The supported target kinds are `executable` and `test`. Each has exactly one
entry `.gti` source and therefore one existing whole-program source graph. A
test target differs in project intent and command selection, not in language
semantics or native representation.

The manifest schema may reserve `library`, but implementation must not claim
native libraries until the compiler defines their exported surface, linkage,
lifecycle ABI, generic-instantiation ownership, and cross-package visibility.
Source dependencies can arrive before compiled-library targets.

### Profile

A profile is a named collection of build settings such as optimization,
generated C++ retention, C++ backend standard, and debug information. Built-in
profiles are `dev` and `release`; manifests may refine them without changing
their broad intent.

### Workspace

A workspace groups packages developed together and shares a build directory.
The root package declares canonical member paths; a shared lockfile follows in
the Git-dependency phase. Local members and path dependencies do not require
publishing or network access.

### Source dependency

A source dependency is still an edge in `SourceGraph`. Existing quoted includes
remain relative to their declaring unit, and `<std/name>` remains rooted in the
installed standard library. Project compilation generalizes angle includes to
manifest dependency aliases, for example:

```cpp
#include <graphics/window>
```

Here `graphics` is a direct dependency alias resolved to a declared package
source root. `std` remains reserved. Aliases are supplied to `SourceLoader` as
an immutable graph; a manifest does not put dependency declarations into global
scope, and aliases are not transitive.

## Manifest Schema Version 1

The first accepted manifest should be intentionally small:

```toml
manifest-version = 1

[package]
name = "chip8"
version = "0.1.0"

# Add this only when GTI implements language editions. A build tool must never
# silently accept and ignore an edition field.
# edition = "1"

[package.native]
include-dirs = ["native/include"]
c-sources = ["native/support.c"]
cpp-sources = ["native/support.cpp"]
c-standard = "c17"
c-compile-args = ["-DSUPPORT_API=1"]
link-files = ["native/lib/support.a"]
libraries = ["m"]

[[package.native.platforms]]
os = "macos"
frameworks = ["CoreFoundation"]

[targets.chip8]
kind = "executable"
root = "src/main.gti"

[targets.chip8-unit]
kind = "test"
root = "tests/unit.gti"

[profiles.dev]
optimization = 0
cpp-standard = "c++23"
keep-cpp = false

[profiles.release]
optimization = 3
cpp-standard = "c++23"
keep-cpp = false

```

Rules for version 1:

- `manifest-version` is mandatory and rejects unsupported newer schemas;
- package, target, and profile names use `[A-Za-z][A-Za-z0-9_-]*`;
- package versions use Semantic Versioning;
- paths are relative to the manifest directory unless explicitly documented;
- `[targets]` is optional for source-only packages; project build commands
  require the selected package to declare an applicable target;
- roots must exist, be regular files, use `.gti`, and remain beneath the
  package root unless a declared dependency grants another root;
- `build`, `check`, and `run` infer the sole target, or the sole executable when
  additional test targets exist; multiple executable targets require an
  explicit name;
- `build` and `check` may explicitly select either supported kind; `run`
  accepts only `executable` and directs a named test to `gti test`;
- `test` selects every test target when its target name is omitted and only the
  named test target when one is supplied;
- unknown fields are errors with source spans and a nearest-name suggestion;
- duplicate tables or keys are errors, even if a TOML parser could merge them;
- environment-variable interpolation is not performed in manifest strings;
- `dev` defaults to optimization 0 and `release` defaults to optimization 3;
- all profiles default to C++23 and do not retain generated C++;
- configuration never changes GTI language semantics implicitly.

Schema version 1 accepts `native` tables beneath `package`, an executable
target, or a profile. Each base table may contain `include-dirs`, `c-sources`,
`c-standard`, `c-compile-args`, `cpp-sources`, `library-dirs`, `link-files`,
`libraries`, `frameworks`, `compile-args`, `link-args`, and `raw-args`, plus
ordered `[[...native.platforms]]` fragments selected by one or more exact `os`,
`vendor`, and `arch` fields. Platform fragments accept the list fields but not
the scalar `c-standard`. Structured paths must exist with the declared kind,
remain within the package even through symbolic links, and are resolved only
for selected fragments. Frameworks require a macOS target.

C sources, C++ sources, search paths, and mixed link operands resolve target to
profile to package so specific providers appear first. Within a fragment the
fixed operand category order is link files, libraries, then frameworks. C, C++,
linker, and raw argument vectors resolve package to profile to target so
specific flags appear later. The C standard resolves target to profile to
package with `c17` as its default; C++ sources use the selected profile or CLI
C++ standard. All four argument fields are trusted exact argv escape hatches
for the package being built directly: GTI does not shell-split or
containment-check embedded paths. Dependency packages get no such trust — when
a package is consumed as a dependency, only validated `-D`/`-U` macro
definitions compose from its compile-argument vectors, and its linker and raw
argument vectors are rejected outright.
Response files and options that override output, phase, a language standard,
optimization, target, sysroot, or data layout remain rejected.

Source-only local dependencies are implemented in the schema-compatible form:

```toml
[dependencies]
graphics = { path = "../graphics" }
```

The dependency alias matches `[A-Za-z_][A-Za-z0-9_]*`, with `std` reserved.
Paths resolve canonically relative to the declaring manifest, must contain
`gti.toml`, and may be outside the package. A package may optionally set a
contained existing `[package].source-root`; omission means `src`. Workspace
roots use `[workspace].members = ["packages/a", "packages/b"]`; members are
canonical contained directories and cannot declare nested workspaces.

Pinned git dependencies are accepted with a full immutable revision; branch
and tag selectors are rejected so a build plan can never depend on mutable
remote state:

```toml
math = { git = "https://example.invalid/math.git", rev = "<full 40-hex commit>" }
```

Registry ranges should wait until package identity, lockfile behavior,
publishing policy, yanking, checksums, and trust boundaries are specified.

## Command-Line Contract

The first non-option argument chooses the mode:

```text
gti <path-ending-in-.gti> ...   direct compiler mode
gti build ...                   project build mode
gti check ...                   project analysis mode
gti run ...                     project build-and-run mode
gti new <path> ...              new executable package scaffolding
gti init [path] ...             existing-directory package scaffolding
gti test ...                    project test mode
gti clean ...                   project cleanup mode
gti fetch ...                   dependency acquisition mode
gti metadata ...                machine-readable project description
```

There is no conflict with the current driver because it already rejects an
input that does not end in `.gti`. Unknown subcommands must produce one focused
error rather than falling through to an input-file diagnostic.

Direct-mode exit status ownership is frozen as follows:

| Status | Meaning |
| --- | --- |
| `0` | Successful compile, emit, help, or version request |
| `64` | Invalid command-line usage |
| `65` | GTI source, frontend, or internal MIR compilation failure |
| `74` | Generated-artifact or native-output-capture I/O failure |
| `78` | Required installed toolchain resource is unavailable |
| native status | The native compiler started and returned a nonzero status |

### Direct mode examples

```sh
gti src/main.gti
gti src/main.gti -o chip8
gti src/main.gti -O3 --std c++23 -o chip8
gti src/main.gti --emit-cpp -o build/chip8.cpp
gti src/main.gti -o chip8 -- -Ivendor/include -Lvendor/lib -lSDL2
```

These commands must remain independent of `gti.toml` discovery.

### Project mode examples

```sh
gti build
gti build chip8
gti build chip8 --profile release
gti build chip8 --release
gti build chip8 --verbose
gti build chip8 --no-cache
gti build --all --jobs 4
gti fetch
gti build --locked
gti build --offline
gti check chip8
gti check chip8 --release
gti run chip8
gti run chip8 --release -- roms/pong.ch8
gti new hello
gti new tools/hello --name hello
gti init
gti init existing/project --name project
gti test
gti clean
gti metadata --format json
```

For `run`, arguments after `--` belong to the built program and are passed as
an argument vector without shell splitting. `build`, `check`, and `test`
reject `--`.
Native compiler escape hatches in project mode require explicit repeatable
options or manifest fields, avoiding an ambiguous second separator; their
surface remains part of the structured-native-input design.

Project build/run/test commands consult and update the whole-program cache by
default. `--verbose` reports the identity and disposition; `--no-cache`
bypasses both lookup and publication. The option is intentionally not accepted
by direct mode or frontend-only `check`, because those paths do not use the
project cache.

Plain `build`, `check`, `run`, and `test` select `dev`. A profile declaration
refines a named profile but does not select it. `--release` and
`--profile release` are
exactly equivalent, and combining either spelling with another profile
selection is a usage error.

### Configuration precedence

The effective configuration is resolved in this order, highest priority first:

1. explicit command-line option;
2. selected target setting;
3. selected profile setting;
4. package-wide manifest setting;
5. toolchain default.

Environment variables remain restricted to documented toolchain discovery
such as `GTI_CXX` and installed-resource overrides. Environment variables do
not silently override package semantics or dependency versions.

## Proposed Architecture

### Keep `gti_compiler` reusable

The compiled static `gti_compiler` target remains focused on language
compilation. Its declarations and reusable data models live under
`include/gti/`, while non-template implementation migrates incrementally to
`src/compiler/` according to
[the compiler library migration plan](compiler-library-migration.md).
The LSP continues to use `Frontend` directly and must not depend on project
execution, native linking, or cache mutation merely to analyze a document.

### Driver library

Compiler-driver policy lives in the concrete `gti_driver` library. The CLI is
routing, presentation, and exit-status policy. Compilation, native toolchain,
and artifact APIs are implemented; the later project responsibilities remain
staged:

```text
gti_driver
├── DriverInvocation       resolved direct or project invocation
├── CompilationRequest     one whole-program frontend/backend request
├── NativeToolchain        C and C++ compiler discovery and invocation
├── ProjectManifest        parsed and validated gti.toml
├── Workspace              discovered package and selected targets
├── BuildPlanner           immutable DAG construction
├── BuildExecutor          dependency-aware execution
├── ArtifactStore          output layout and cache lookup
└── DependencyResolver     path first; Git and registry later
```

Current and planned source layout:

```text
include/gti/driver/
  artifact.h             # implemented
  build.h                # implemented
  compilation.h          # implemented
  manifest.h             # implemented
  native_toolchain.h     # implemented
  project.h              # implemented
  invocation.h
  workspace.h           # implemented
  build_plan.h
  artifact_store.h

src/driver/
  artifact.cpp           # implemented
  build.cpp              # implemented
  compilation.cpp        # implemented
  manifest.cpp           # implemented
  native_toolchain.cpp   # implemented
  project.cpp            # implemented
  workspace.cpp         # implemented
  build_plan.cpp
  artifact_store.cpp
```

Keeping the driver separately compiled prevents the reusable language frontend
from acquiring process, filesystem-cache, TOML, or package-manager
dependencies.

### Make compilation a value request

`lang::driver::compileToCpp` consumes the implemented immutable request below.
Direct mode selects the host before constructing it; project mode resolves
its target and profile into the same value:

```cpp
CompilationRequest(
    std::filesystem::path entry,
    lang::StandardLibraryLayout standardLibrary,
    lang::TargetInfo target,
    lang::OptimizationLevel optimization,
    lang::CppStandard cppStandard,
    std::vector<lang::PackageSourceRoot> packageSourceRoots = {});
```

The fields are private and exposed through const accessors. Project mode now
supplies the resolved package roots and direct alias graph; direct mode leaves
that value empty and does not discover manifests. Cache identity is serialized
field-by-field from the effective request and loaded source graph; it never
hashes C++ object layout.

### Build graph

Version 1 needs a small graph:

```text
ValidateManifest
      |
ResolveTarget
      |
LoadSourceGraph
      |
ComputeAndLookupWholeProgramIdentity
      | miss
AnalyzeFrontend
      |
GenerateBackendArtifact
      |
CompileDeclaredNativeSources
      |
InvokeNativeCompiler
      |
PublishArtifact
```

A verified cache hit branches from `ComputeAndLookupWholeProgramIdentity`
directly to atomic publication. The loaded graph is produced by `SourceLoader`
in both cases; a miss moves it into the frontend rather than loading it again.

`check` stops after frontend analysis. `--emit-cpp` stops after backend
generation, and project `gti build --emit-mir` (implemented) stops after MIR
verification and serializes the verified snapshot beside the profile output.
`run` adds a process step after publishing. Independent targets
may execute concurrently, but operations for one whole-program target remain
ordered.

Do not initially create one native compilation node per `.gti` source unit.
The current compiler intentionally performs whole-program analysis and emits
one C++ artifact. Pretending it has C++-style translation units would create a
false incremental model and complicate generic ownership before GTI has a
stable module boundary.

Each graph step has:

- a stable internal kind;
- declared input artifacts;
- declared output artifacts;
- an immutable options value;
- a display label;
- dependencies;
- a cache policy;
- structured failure information.

The executor must never infer hidden edges from execution order.

### Output layout

Project artifacts should not appear beside source files. A predictable initial
layout is:

```text
build/
  gti/
    dev/
      <target-triple>/
        chip8
        .gti-intermediate/
    release/
      <target-triple>/
        chip8
        .gti-intermediate/
```

The manifest directory anchors the default `build/` path. A CLI option may
override it. `gti clean` removes only the validated GTI-owned subtree and must
refuse broad, unresolved, or filesystem-root targets.

Profile directories are created lazily. A plain `gti build` therefore creates
only `build/gti/dev/<target-triple>/`; declaring `[profiles.release]` does not
create a release directory until a command selects it with `--release` or
`--profile release`.

Direct mode keeps its existing output behavior and temporary C++ handling.

The driver implements the `PublishArtifact` boundary by directing the native
compiler to a unique hidden sibling of the requested executable. A successful
native invocation publishes that staged file with a same-filesystem rename on
POSIX, `ReplaceFileW` for an existing Windows destination, or `MoveFileExW`
for a new Windows destination. Failed native invocations and publication
errors remove the staged output while preserving the previous executable;
publication errors also retain generated C++ for diagnosis. This is atomic
with respect to readers under the host filesystem's replacement semantics,
but it is not a power-loss durability guarantee because files and parent
directories are not explicitly synchronized to stable storage.

### Cache model

**Implementation status: complete for the bounded whole-program unit described
below.** The current cache stores generated C++ plus the final executable and
is keyed by:

- GTI compiler and runtime version;
- manifest schema and effective target/profile configuration;
- backend identity and C++ standard;
- target OS, vendor, and architecture;
- optimization level;
- content hashes for every loaded source unit in `SourceGraph`;
- ordered direct dependency edges and logical standard-library imports;
- standard-library/runtime identity;
- native C++ compiler identity and relevant version output;
- selected native standards;
- resolved dependency identities from `gti.lock` once dependencies exist.

Canonical filesystem paths may be recorded for diagnostics but should not be
the sole content identity, otherwise moving a checkout invalidates every
shareable cache entry. The build must not hash arbitrary ambient environment
state. Every environment value that affects output must be explicitly admitted
to the request and key.

The implementation admits a bounded list of native-toolchain environment
variables that affect executable/search-path selection and hashes their exact
presence/value. Content-modeled toolchain resources contribute their contents;
paths are also retained when their native spelling is observable. Manifest
native search paths and opaque trusted argument vectors are not
dependency-complete and therefore bypass lookup and publication rather than
merely contributing their declared tree or strings.

Non-empty compiler/SDK/include/library environment search paths also bypass.
Their string values do not identify mutable or transitive resources selected
below those roots. Scalar policy environment values that do not inject a search
root remain part of eligible cache identity.

Declared C and C++ sources now participate in the whole-program cache through
native dependency discovery (implemented): at lookup time each declared source
is preprocessed with its exact object-compile argument vector plus
`-E -MD -MF`, the compiler's own dependency report supplies the exact
discovered input set with content identity, and the preprocessed translation
unit itself is hashed so include resolution — including shadowing — is
captured as the compiler would see it. The deterministic preprocessing policy
is defined as: any declared source or discovered dependency spelling
`__DATE__`, `__TIME__`, or `__TIMESTAMP__` bypasses the cache. Declared
include directories join the key as full sorted content trees, and exact link
files/file operands join with content identity when their magic classifies
them as content-complete regular archives or relocatable objects. Thin
archives, linker scripts, shared libraries, unrecognized link-input formats,
name-resolved libraries/frameworks, native library search directories, and
opaque argument vectors still bypass, because a name or script resolves to
files the driver has not identified exactly. Narrowing a bypass must always
come from establishing exact identity, never from widening what the key
trusts.

The remaining cacheable generated C++ build can still consume implicit system
headers and libraries. The current bounded contract assumes those resources are
stable while compiler executable content, version output, target, and admitted
toolchain environment remain unchanged. This is deliberately documented as a
non-hermetic toolchain boundary; generated-translation-unit depfiles are future
work.

Content-modeled runtime and vendor resources are currently assumed not to
change between key construction and native linking. A future hardening step
should snapshot them or recompute the admitted external-input identity before
cache publication so a concurrent mutation cannot publish under the earlier
key.

Cache metadata is an atomic commit marker and records the SHA-256 and size of
both payloads plus executable permissions. Cache corruption is a build
diagnostic, never permission to use
an artifact whose inputs cannot be verified. A clean build always remains
possible, and cache-publication failure does not invalidate a successfully
published target.

### Native toolchain boundary

`NativeToolchain` now owns native compiler selection, command construction,
process execution, captured output, and runtime discovery; the driver artifact
API owns temporary-file policy. This first implementation milestone preserves
the direct CLI byte-for-byte where tests assert behavior.

Native configuration should have structured representations for:

- compiler executable;
- C++ standard;
- compile arguments;
- linker arguments;
- include directories;
- library directories;
- library names;
- platform frameworks;
- runtime and compatibility include paths;
- runtime libraries.

The implemented `NativeInputs` also retains an ordered tagged link-operand
sequence so an exact file, `-l` library, and macOS framework do not lose their
manifest ordering when converted into toolchain arguments. Category vectors
remain available for diagnostics and metadata.

The native command remains available under `--verbose`. Its leading `+ ` is a
display marker, and the remainder uses POSIX-shell quoting so users on that
shell family can reproduce the exact argument vector. This is not a Windows
`cmd.exe` or PowerShell contract. Dependency manifests must not inject
undeclared shell commands. Process execution uses argument vectors and never a
shell-concatenated command string.

### Diagnostics

Manifest, package, planning, cache, and native-toolchain diagnostics should use
the shared diagnostic model where a source span exists. Introduce a build-phase
code family, for example `GTI-Bxxxx`, rather than emitting unrelated ad-hoc
strings.

Examples include:

```text
error[GTI-B1002]: Unknown target 'chpi8'.
help: did you mean 'chip8'?

error[GTI-B1104]: Target root escapes the package directory.
note: dependency source roots must be declared in [dependencies].
```

Native compiler failures remain explicitly labeled as generated-backend
failures and retain the generated C++ artifact for investigation. Build-system
diagnostics must not disguise C++ backend failures as GTI source errors.

`gti metadata --format json` exposes the manifest schema version, canonical
manifest and package paths, package identity, host target fields, sorted
profiles, sorted executable/test targets with their kinds, and each
target/profile output and generated-C++ path. Metadata schema version 9 reports
every declared execution profile and effective GTI configuration flag set plus
every effective native category, C source, C++ source, C standard, C argument,
and ordered link operand, the
composed per-dependency native groups (`dependencyNative`), and the
resolved workspace/package/dependency graph. It is
deterministic, works for multi-target manifests without selecting one target,
performs no compilation, and creates no output directories.

## Dependency And Package Stages

### Stage 1: no external dependencies

Ship manifest targets and profiles while all source includes remain relative or
standard-library imports. This proves the project model without coupling it to
package identity.

### Stage 2: path dependencies

Status: complete for local packages, including package-scope native input
composition under the documented trust policy.

Add declared local dependencies and package-root resolution. Define package
include spelling, direct visibility, duplicate package names, cycles, and
whether one dependency can expose another. Preserve the existing rule that
transitive source declarations do not leak.

### Stage 3: Git dependencies and lockfile

Status: complete for pinned full revisions (see Milestone 7).

Resolve only pinned revisions into an immutable lock entry containing:

- source URL;
- requested selector;
- exact commit;
- manifest/package identity;
- content checksum where practical;
- dependency graph edges.

`--locked` rejects a manifest/lock mismatch. `--offline` rejects required
network access. `gti fetch` populates the source cache without compiling.

### Stage 4: registry evaluation

Do not create a registry merely because a manifest exists. First specify name
ownership, semantic-version policy, immutable releases, yanking, checksums,
signing/trust, malicious-package handling, source availability, and native
dependency policy. A registry is a service and governance commitment, not a CLI
subcommand.

### Build hooks

Arbitrary package build scripts are a non-goal for the initial system. When a
real need appears, prefer declared generator inputs and outputs executed through
an explicit tool dependency. Any future build hook needs:

- declared input and output paths;
- deterministic cache identity;
- no implicit network access;
- a clear host-versus-target execution model;
- dependency permission and trust policy;
- cycle detection;
- machine-readable diagnostics.

## Language And ABI Boundaries

The project system must not force premature language commitments:

- `edition` is not accepted until the frontend implements edition semantics;
- when it lands, edition selection is package-wide, omission resolves
  permanently to Edition 1, and an unknown or unsupported value is an error;
- library targets do not imply a stable GTI ABI;
- dependencies are initially compiled from source as part of a target;
- generic instantiations remain whole-program compiler responsibilities;
- project targets do not alter direct include visibility;
- target selection must feed the same `TargetInfo` to semantics, optimization,
  and backend emission;
- native C++ output remains an implementation artifact;
- C++ backend standard selection is not a GTI language edition.

If GTI later gains a native or LLVM backend, the manifest should select a
backend without changing package identity. Backend-specific options live in a
namespaced table rather than becoming universal package fields.

## Implementation Plan

Each milestone must leave direct mode passing before project behavior advances.

### Milestone 0: freeze the compatibility contract

Status: complete

- Add focused CLI tests for every current direct-mode option and default.
- Test `--` native argument ordering and output-path defaults.
- Record exit status categories and diagnostic ownership.
- Decide whether `--std=c++23`, `-std=c++23`, and `--cpp-std` aliases ship.

Acceptance criteria:

- all documented current commands behave exactly as before;
- no manifest lookup occurs in direct mode;
- tests can detect accidental command-line regressions.

### Milestone 1: extract `gti_driver`

Status: complete

- Move native compiler discovery and invocation out of `src/cli/main.cpp`.
- Introduce immutable direct `CompilationRequest` and native-link request types.
- Pass `TargetInfo` into compilation instead of creating host target inside
  `compileToCpp`.
- Keep the frontend and LSP dependency boundaries unchanged.

Acceptance criteria:

- no user-visible CLI changes;
- build-tree and installed-resource smoke tests pass;
- C++20, C++23, optimization, emitted C++, retained C++, and forwarded native
  argument paths remain covered.

### Milestone 2: manifest and single executable target

Status: complete

- Vendor or link a pinned TOML parser into `gti_driver`.
- Implement upward `gti.toml` discovery for project subcommands only.
- Parse and validate schema version 1 with exact source spans.
- Implement one executable target and `dev`/`release` profiles.
- Add `gti build`, target selection, and project output layout.

Acceptance criteria:

- `gti build` and the equivalent direct command compile equivalent programs;
- unknown keys, bad roots, unsupported schemas, and ambiguous targets produce
  focused `GTI-B` diagnostics;
- direct mode remains manifest-independent;
- project builds work from the package root and nested directories.

### Milestone 3: project commands and native declarations

Status: complete

- Add `gti check`, `gti run`, `gti clean`, and `gti metadata`. Complete.
- Add structured native include/library/framework settings. Complete.
- Define program arguments versus native compiler arguments unambiguously.
  Complete for the current command surface: only `run -- args` accepts the
  separator as program input, and the owned hosted `main` form exposes those
  values to GTI source. Accepted direct-mode arguments after `--` remain native
  compiler inputs; driver-owned build invariants remain reserved.
- Add safe output cleanup and machine-readable metadata tests. Complete.

Acceptance criteria:

- `check` never invokes the native compiler;
- `run -- args` preserves program arguments exactly;
- `clean` cannot remove paths outside its validated GTI build subtree;
- platform-specific native settings select from explicit target information.

### Post-Milestone 3 addition: project scaffolding

Status: complete

- Add `gti new <path>` for a destination that does not yet exist.
- Add `gti init [path]` for an existing directory, defaulting to the current
  directory.
- Generate `gti.toml` and `src/main.gti`, while preserving an existing regular
  entry source during `init`. The generated source uses the implemented owned
  argument-vector entry form and its required standard-library includes.
- Accept `--name <name>` without adding editions, library targets, dependency
  fields, or version-control side effects.

Acceptance criteria:

- generated manifests load through the ordinary schema-version-1 parser;
- no existing manifest, source, symlink, or destination is overwritten;
- invalid package names fail before filesystem mutation;
- direct compilation remains manifest-independent.

### Post-Milestone 3 addition: formatter configuration scaffolding

Status: complete

- Add `gti format init [directory]`, defaulting to the current directory.
- Emit the compiler-owned GTI default `.gti-format` document and verify that it
  parses back to the same style.
- Refuse missing, non-directory, symbolic-link, filesystem-root, and existing
  configuration destinations without replacing user data.
- Keep source formatting and manifest discovery outside this initialization
  command.

### Post-Milestone 3 addition: declared native C and C++ sources

Status: complete

- Accept package-contained `.c` files, one resolved `c11`/`c17`/`c23`
  standard, and C-only exact compiler arguments in native tables.
- Accept package-contained `.cpp`, `.cc`, and `.cxx` files using the resolved
  project C++ compiler, standard, optimization, include paths, and existing
  `compile-args`.
- Resolve C sources target to profile to package, arguments package to profile
  to target, and the scalar standard by most-specific base table. Resolve C++
  sources through the same target-to-package source ordering.
- Discover the C compiler through `--cc`, `GTI_CC`, `CC`, then `cc`; direct mode
  remains unchanged.
- Compile each selected source to an atomically published managed intermediate
  object, then place C objects followed by C++ objects before runtime and
  manifest libraries in the existing final C++ link.
- Report the resolved C and C++ inputs through metadata schema version 9 while
  keeping `check` compiler-free and output-free.

Acceptance criteria:

- a project containing only GTI declarations and declared C/C++ definitions
  builds and runs without separately precompiled objects;
- selected paths, extensions, standards, arguments, and platform fragments have
  focused manifest diagnostics;
- a failed C or C++ source compilation preserves the prior object and
  executable and prevents final linking;
- direct compilation and prebuilt native link inputs retain their existing
  behavior.

### Milestone 4: whole-program incremental cache

**Complete:**

- Expose loaded source paths and logical dependency data needed for hashing.
- Implement content-addressed whole-program backend/native artifact keys.
- Add verified cache hit/miss/corruption explanations under `--verbose`.
- Add `--no-cache` for verification and debugging.

Acceptance criteria:

- **Passed:** an unchanged cache-eligible rebuild performs no native
  compilation;
- **Passed:** changing a directly or transitively included unit invalidates the
  target;
- **Passed:** effective profile/target/runtime/C++ standard/compiler/native
  identity participates in the key;
- **Passed:** moving a pure-GTI checkout together with its cache does not
  invalidate content-only entries; external/native paths retain identity where
  path spelling is semantically observable;
- **Passed:** deleting a cache entry never damages source or the published
  target and degrades to a clean rebuild.

### Post-Milestone 4 addition: native-source cache identity and parallel target builds

Status: complete

- Consume native compiler depfiles (`-E -MD -MF` discovery probes with the
  exact object-compile argument vector) so declared C/C++ sources join the
  whole-program cache identity with exact header content, and hash the
  preprocessed translation unit so include resolution and shadowing are part
  of the key.
- Define the deterministic preprocessing policy: `__DATE__`, `__TIME__`, and
  `__TIMESTAMP__` in any declared source or discovered dependency bypass.
- Resolve exact link files to content identities and classify them by magic;
  only regular non-thin archives and relocatable objects are trusted, while
  thin archives, linker scripts, shared libraries, and unknown formats keep
  bypassing with a stated reason.
- Add `gti build --all [--jobs <count>]`: every declared target builds as an
  independent child whole-program build with a bounded concurrent job count,
  deterministic name-ordered output/diagnostics, name-ordered first-failure
  status, and byte-identical serial/parallel artifacts.
- Make link output deterministic: the staged link output keeps its final
  basename inside a unique staging directory, and Apple links append
  `-Wl,-reproducible` so identical inputs produce identical bytes.

Acceptance criteria:

- **Passed:** a project with a declared C source reports a verified cache hit
  on an unchanged rebuild and misses when any discovered header, shadowing
  header, declared include tree entry, or link input changes;
- **Passed:** every remaining bypass reason is reported explicitly and covered
  by CLI tests, including opaque argument vectors, name resolution, library
  search directories, non-content-complete link inputs, injected environment
  search paths, and time-sensitive macros;
- **Passed:** serial and parallel `--all` builds produce byte-identical
  artifacts and byte-identical ordered output, including under a failing
  target;
- **Passed:** malformed or partial cache entries are diagnosed, never
  executed, and replaced only after a successful rebuild.

### Milestone 5: tests and workspaces

- **Complete:** accept manifest `test` targets, build each selected root as an
  independent whole program, execute them in deterministic target-name order,
  continue after runtime failures, and propagate the first failing status.
- **Complete:** add workspace membership, shared collision-free output, and
  explicit package selection. Lockfile state begins with Milestone 7.
- **Complete:** treat canonical workspace members as local packages without
  publishing or network access.

Acceptance criteria:

- **Passed:** tests execute independently and report their target names;
- **Passed:** workspace commands have deterministic package selection;
- **Passed:** package cycles and duplicate identities are rejected before
  compilation.

### Milestone 6: path package dependencies

Status: complete for source-only local packages; the native dependency
composition addition below extends this to package-scope native inputs.

- **Complete:** define dependency package identity and package include syntax.
- **Complete:** extend `SourceLoader` with declared package roots without weakening direct
  include visibility.
- **Complete:** test missing direct dependencies, dependency cycles, duplicate
  roots/loads, cache identity, CLI selection, and diagnostics in dependency
  units. The compiler query API accepts the same graph; wiring driver-owned
  project facts into LSP snapshots remains B-PROJECT-05/T-LSP-04.

Acceptance criteria:

- **Passed:** undeclared filesystem access is rejected;
- **Passed:** transitive and sibling package declarations do not leak;
- **Pending B-PROJECT-05/T-LSP-04:** CLI and LSP resolve the same package
  source graph;
- **Passed at the compiler-library boundary:** direct compilation requests can
  receive explicit package-root mappings when needed without a
  manifest.

### Post-Milestone 6 addition: native dependency composition

Status: complete for declared package-scope inputs.

- **Complete:** define the dependency native trust policy: structured
  contained inputs compose; only validated `-D<name>[=<value>]`/`-U<name>`
  macro definitions compose from compile-argument vectors; linker and raw
  argument vectors on a dependency are rejected with `GTI-B1606`.
- **Complete:** compose per-package `NativeDependencyGroup` values over the
  dependency closure in deterministic dependents-before-dependencies order
  (reverse postorder), with platform fragments selected against the resolved
  target before composition.
- **Complete:** compile each group's C and C++ sources with only that group's
  include directories and macros plus the runtime include; nothing leaks into
  the root package's native sources, other groups, or the generated
  whole-program translation unit.
- **Complete:** metadata reports the composed groups per plan as the
  schema-version-8 `dependencyNative` array.

Acceptance criteria:

- **Passed:** a dependency's include directories are invisible to root-package
  native sources (observed as a real native compile failure);
- **Passed:** composed macro definitions and link operands change dependency
  object code and final links end-to-end, including transitive chains;
- **Passed:** unselected platform fragments compose nothing and reject
  nothing;
- **Passed:** plans with dependency native groups bypass the whole-program
  cache with an explicit reason rather than risking a stale hit.

### Milestone 7: Git resolution and lockfile

Status: complete for pinned full revisions; registry ranges and any mutable
selector (branch/tag) remain out of scope by design.

- **Complete:** fetch/cache storage under `build/gti/deps/git` (bare object
  databases keyed by URL identity, blob-extracted checkouts keyed by URL and
  revision), exact revision resolution, and `gti.lock`.
- **Complete:** `gti fetch [--offline]`, and `--locked`/`--offline` on
  build, check, run, and test.
- **Complete:** lock updates are never automatic — `gti fetch` is the lock's
  only writer, and it re-derives every checksum from the immutable object
  database so a locally modified checkout cannot launder content into a
  fresh lock. Builds only consume the lock.
- Replacement/override authority beyond the manifest's own pins waits for a
  demonstrated need.

Acceptance criteria:

- **Passed:** locked offline builds are repeatable from a populated source
  cache (`gti fetch` then `gti build --offline`);
- **Passed:** a changed upstream branch cannot alter a locked build — only a
  manifest re-pin plus an explicit `gti fetch` moves the closure;
- **Passed:** checksums and locked package identities are verified before
  source loading, and mismatches refuse the build with focused `GTI-B17xx`
  diagnostics;
- **Passed:** dependency acquisition never executes package code — trees are
  extracted blob-by-blob from a bare database (no checkout, hooks, filters,
  or submodules), and symbolic links, gitlinks, unsafe paths, and
  case-folded collisions reject the tree.

## Testing Strategy

The build system needs tests at four boundaries:

1. **Unit tests** for manifest validation, precedence, target selection, path
   containment, graph construction, cache keys, and lockfile resolution.
2. **CLI workflow tests** for direct compatibility, project discovery, output
   layout, native arguments, exit status, cleanup safety, and offline behavior.
3. **Compiler integration tests** proving a project request supplies identical
   target and source-root facts to the frontend, optimizer, and backend.
4. **Installed-toolchain tests** proving manifests work when `gti` is launched
   by basename outside the repository and discovers runtime/stdlib resources
   beside the installed executable.

Every milestone should test macOS, Linux, and Windows path behavior in CI where
the release workflow already produces artifacts. Filesystem tests must avoid
assuming case sensitivity, POSIX separators, executable suffixes, or symlink
behavior without an explicit platform branch.

The LSP should initially discover a project only to obtain source roots, target
information, and configuration. It must retain immutable document overlays and
must not fetch dependencies, invoke build hooks, clean outputs, or mutate a
lockfile as a side effect of opening an editor.

## Explicit Non-Goals For Version 1

- replacing CMake for building the GTI compiler itself;
- arbitrary `build.gti` programs or dependency-supplied shell hooks;
- a central package registry;
- binary GTI modules or a stable cross-version GTI ABI;
- separate compilation that imitates C++ translation units;
- automatic C/C++ header discovery;
- silently scanning and compiling every `.gti` file in a directory;
- source glob lists in the ordinary manifest;
- environment interpolation throughout configuration;
- making `gti.toml` mandatory for direct compilation;
- treating C++ backend versions as GTI language editions;
- accepting manifest fields whose semantics are not implemented.

## Milestone 2 Decisions

The first project implementation fixes the following contracts:

1. TOML is parsed by the vendored amalgamated header from toml++ v3.4.0. The
   upstream archive SHA-256 is recorded beside the header, TOML types remain
   private to `gti_driver`, and release archives carry its MIT license.
2. Project `build`, `check`, and `test` reject arguments after `--`; `run`
   reserves them exclusively for the executed program. An explicit
   project-native argument
   contract must be designed before native arguments are accepted elsewhere.
3. Package, target, and profile names match
   `[A-Za-z][A-Za-z0-9_-]*`; package versions use Semantic Versioning.
4. Target output directories use `<arch>-<vendor>-<os>` with unsupported path
   characters replaced by `_`.
5. The default project output root is `<package>/build/gti/`.
6. Native tables were reserved until their values could feed native requests
   in Milestone 3; package/profile/target native tables are now accepted under
   that contract.

Language editions, metadata stability, direct-mode standard aliases, and the
long-term native argument spelling remain open because Milestone 2 does not
need them.

## Milestone 3 Command Decisions

1. `dev` remains the default profile. Declaring or refining a profile never
   selects it or creates its output directory.
2. `--release` is an exact, tested alias for `--profile release` on `build`,
   `check`, `run`, and `test`.
3. `check` uses the shared frontend through typed HIR/MIR and stops before
   optimization, C++ emission, and native compilation.
4. `run` inherits standard input, output, and error and returns the program's
   exit status. Its arguments are passed directly without a shell.
5. `clean` resolves a valid workspace so a member cleans the shared managed
   root. If resolution fails, it retains the recovery path for a broken
   manifest and validates/removes only the nearest literal
   `<package>/build/gti`. Both paths refuse symbolic-link boundaries.
6. Metadata JSON schema version 9 is a read-only enumeration of every current
   target/profile plan, including target kinds, declared execution profiles,
   effective GTI configuration flags, resolved native C and C++ sources, C
   policy, other native inputs, ordered link operands, and composed
   per-dependency native groups. It also publishes the selected workspace and
   sorted source-package graph. Platform selection and precedence use the
   resolved target.

## Recommended First Pull Requests

Keep the first implementation changes deliberately small:

1. **Direct CLI contract tests** with no production behavior change.
2. **Native toolchain extraction** with no production behavior change.
3. **Compilation request extraction** and explicit host `TargetInfo` plumbing.
4. **Manifest model and parser tests** without building a project.
5. **Single-target `gti build`** reusing the direct compilation request.

This sequence creates reviewable seams before dependency management, caching,
and workspaces add state. It also prevents the project model from becoming a
collection of special cases inside `src/cli/main.cpp` while the language and
backend are still evolving.
