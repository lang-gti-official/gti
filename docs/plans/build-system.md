# GTI Build And Package System Proposal

> **Plan status:** Non-canonical staged work. Implemented direct/project
> behavior is summarized in
> [`docs/architecture/build-and-driver.md`](../architecture/build-and-driver.md).

Status (reconciled 2026-10-28): implementation in progress; Milestones 0
through 7 are complete for their bounded local/path/native and pinned-Git dependency scopes. The source
registry bootstrap is also implemented; Stage 4 below records its accepted
scope and remaining service/governance work.

Operational ordering is maintained in
[`implementation-sequence.md`](implementation-sequence.md). Project test targets, whole-program caching, workspace/path and pinned Git
dependencies, registry acquisition/authentication, approved build scripts and
local program installation are delivered within their documented scopes.
The milestone numbers below preserve that domain decomposition and its
historical evidence; they are not a competing live queue.

This document proposes a staged project build system for GTI. It deliberately
preserves GTI's existing compiler-driver workflow while adding a modern,
declarative project workflow. The initial slice established orchestration over the existing whole-program
compiler. Registry packages and explicitly approved GTI-authored build scripts
have since extended that foundation. A stable binary GTI module/ABI system is
still not implemented. Public `kind = "library"` is implemented for infallible
LLVM closures on macOS arm64/Linux x86_64, including generated headers,
immutable generations and cache/report/clean integration. Fallible exports and
complete E-EMBED/B-LIB/T-EMBED/Q-EMBED acceptance remain; this is not the
backend's ordinary object ABI. Aggregate `import std` and read-only LSP
`ProjectFacts` consumption are delivered, not remaining project foundations.

## Current Implementation

Integrated test targets now support name filtering/exclusion, discovery listing,
explicit empty-selection policy, bounded parallel isolated children, configurable
per-test deadlines, and versioned JSON Lines lifecycle reporting. Runner-owned
completion accounting is separate from test-language assertions. Integrated
coverage, benchmarking, fuzzing, named parameterized cases and source-level
ignored-test attributes remain follow-on goals, not implemented by these controls.

Configuration defines are implemented in `[build]`, `[profiles.<name>]` and
`[targets.<name>]`. One driver-owned normalized union combines the selected
package/dependency build flags, consumer profile, consumer target and CLI
flags. Metadata, cache identity, build/check and read-only editor facts share
that selection; dependency target/profile flags remain local to their own
consumer builds. Inactive-name diagnostic facts preserve typo checking without
warning on deliberately disabled consumer switches. The runnable
`examples/scoped-configuration` project and `scoped_configuration_defines` gate
cover dev/release, distinct executable/test
targets, CLI additions, cached rebuilds and source-order undefinition.

Raw-pointer audit policy is now resolved through the existing immutable
direct/project request path: `off` by default, profile
`raw-pointer-audit = "off"|"warn"|"strict"`, and explicit command override.
The policy is retained in read-only editor project facts, child invocations,
cache identities and build-report verification. It does not add another
language-analysis pipeline or change native arguments.

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

Live build progress is implemented as optional observational compiler/driver
events with CLI-owned Cargo-style rendering. Default terminal output refreshes
the active phase and elapsed time; redirected output retains stage transitions.
`--progress=plain|never` controls presentation without changing request,
artifact, cache or program-output policy. Parallel target children remain quiet
and the parent reports target waits. The current contract and deliberately
whole-program boundaries are documented in
[Live Build Progress](../architecture/build-and-driver.md#live-build-progress).
Human-readable backend progress, retention and failure summaries are
representation-neutral; explicit legacy input spellings and machine-readable
schemas remain compatible. Raw backend evidence is retained in private logs,
with the actual representation format named only when reporting a saved
artifact. This is implemented presentation policy, not a backend migration.

Direct mode and project builds now resolve the same explicit execution-profile
fact. The direct option is
`--execution-profile single-threaded|concurrent`; project
`[profiles.<name>]` tables use the optional `execution-profile` string and the
CLI may override it. The default is single-threaded. This is C-GLOBAL-01
language-policy selection, not a native thread flag or a public runtime
capability. The driver-owned immutable analysis configuration separately
selects the safe `None` or installed `Hosted` runtime profile; the latter
derives thread support only for a supported target and is shared by CLI and
LSP adapters.

Successful direct executable builds may also request a versioned structured
provenance sidecar with `--build-report <path>`. The driver captures its exact
loaded source graph before moving frontend inputs and its actual runtime/header
closure from a depfile emitted by the real generated-C++ native invocation;
the CLI atomically serializes the typed result after collision checks. The
record includes exact native argv, runtime archive, producer/native compiler
hashes, the produced artifact's content identity, and the compiler's captured
source commit plus clean/dirty/unavailable state. Compiler builds refresh that
source identity; it is not merely the configure-time checkout revision. `gti build --build-report` now
records the same sidecar with the resolved package, target, profile, selected
target triple, and project model identity, and `gti verify --build-report
<path>` rebuilds a recorded project build for the target the record names and
reports every recorded identity the tree no longer reproduces. See
[`docs/architecture/build-and-driver.md`](../architecture/build-and-driver.md)
for exactly what that verification proves and does not prove.

Manifest schema version 1 also accepts `[workspace].members`, optional
`[package].source-root`, and source-only `[dependencies]` path tables.
Workspace members share `build/gti`, with package artifacts under
`build/gti/packages/<package>/...`; `--package` selects a root/member package.
`#include <alias/unit>` resolves only through the including package's direct
alias map. Cycles, duplicate canonical roots/names, nested workspaces, and
missing source roots fail before compilation. Read-only workspace/path
resolution performs no network acquisition; explicit Git/registry commands
follow the separate dependency policy below.

`[targets]` is optional for a source-only dependency package. Selecting that
package as a command target is diagnosed; depending on and including its source
does not require a dummy executable/test declaration.

Project mode supports `gti build`, `gti check`, `gti run`, `gti test`,
`gti install`, `gti uninstall`, `gti clean`, and `gti metadata`. It discovers `gti.toml` upward from the
working directory,
parses TOML 1.0 with vendored toml++ v3.4.0, validates manifest schema version
1 with exact source spans, resolves executable/test targets and named profiles,
and writes artifacts beneath
`build/gti/<profile>/<arch>-<vendor>-<os>/` for standalone packages (and the
package-qualified workspace layout above), with verified-payload whole-program
cache entries beneath `build/gti/cache/v4/`. Declared native C/C++ sources
(including a dependency group's), declared include and library search
directories, content-complete exact link files, and `-l` names resolving to
exactly one content-complete file inside a declared search directory now join
cache identity through native dependency discovery and full-tree identity;
Apple frameworks join only when the selected-linker probe identifies one
exact stub or binary. Opaque native argument vectors, untraceable frameworks,
unresolved or ambiguous `-l` names, non-content-complete link inputs, and
dependency-injecting native environment search paths still bypass the cache
until their exact inputs can be identified. `gti build --all [--jobs <count>]` builds every
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

Profiles also accept Boolean `dependency-warnings`, defaulting to true. A
consumer may disable compiler warnings from actual non-selected packages while
retaining selected-package warnings, every error and native-tool diagnostics.
The immutable package/source graph owns classification, not a dependency-cache
path or a dependency's own preferences. Resolved build and read-only editor
facts share the same policy; metadata schema 14 reports it in profiles and
output plans. Changes to warning policy invalidate artifact-cache reuse for
fresh analysis; ordinary hits still do not replay semantic warnings. This
diagnostic-presentation addition changes no source-language safety rule.

The driver also publishes the resolved project as read-only values through
`gti/driver/project_facts.h`, so a consumer that must agree with `gti build`
and `gti check` — an editor, a documentation generator, an external analysis
tool — reads the driver's resolution instead of reinterpreting `gti.toml`.
The facts include the package source-root graph, which cannot be
reconstructed from one manifest. Resolution runs no native tool, git process,
or build, creates no directories, and uses `metadata`'s dependency policy.

Project scaffolding supports `gti new <path>` and `gti init [path]`. `new`
requires a destination that does not exist. `init` requires an existing
directory, preserves an existing regular `src/main.gti`, and refuses to replace
an existing `gti.toml`. Both commands derive the package name from the
destination unless `--name` supplies one, validate the manifest's portable-name
rule, and generate one schema-version-1 executable target. A generated entry
uses the owned-argument `int main(int, std::vector<std::string>)` form and
includes the standard string and vector modules. `gti init --git` opts into a
standalone Git repository and the canonical GTI `.gitignore` containing the
root `/build/` rule. It refuses existing `.git` and `.gitignore` entries and
rolls scaffold-owned output back if Git cannot initialize the repository.
Without that option scaffolding has no version-control side effects. Neither
command builds the project or consults a parent manifest.

Direct `gti source.gti` compilation remains manifest-independent, including
when an invalid manifest is present beside the source. Project native inputs
are accepted at package, profile, and target scope, selected from the resolved
target, and passed through the shared native request. Declared `.c` inputs are
compiled with a separately resolved C compiler; declared `.cpp`, `.cc`, and
`.cxx` inputs use the existing resolved C++ compiler and profile policy. Both
become managed link objects. Structured paths are validated within the package;
exact argument arrays use the documented trusted escape-hatch policy. Eligible
project build/run/test requests use the whole-program cache unless `--no-cache`
is selected; opaque native argument vectors, frameworks without an exact
linker trace, and `-l` names that do not resolve to exactly one
content-complete file inside a declared search directory bypass it.

Pinned git dependencies are implemented:
`alias = { git = "<url>", rev = "<full 40-hex commit>" }` is the only accepted
git form. Explicit `gti fetch` writes the workspace `gti.lock`; a pure Git
closure uses lock-version 1, while registry-containing closures use version 2.
Entries record checksums and dependency edges; build/check/run/test verify lock
coverage, stored-tree checksums and locked identity before source loading.
Git builds with `--offline` or `--locked` refuse missing-checkout acquisition;
a plain build may materialize a lock-covered missing checkout. Registry
`fetch --locked` may populate exact verified locked sources, but does not
re-resolve versions; offline mode acquires nothing. Registry `gti add` is also
an explicit manifest/lock writer. Branch/tag Git selectors remain rejected. Registry requirements
now resolve through a backend-independent source-package protocol, with
`gti add`, locked/offline fetch, immutable checksummed archives, and Gitea Generic
Packages as the configured official backend; see the current architecture
document for its explicit bounded format and security policy.

Declared native sources, include and library search
directories, content-complete exact link files, and resolvable `-l` names
join cache identity through native dependency discovery and full-tree
identity, with Apple frameworks admitted through the exact linker trace above.
Opaque native argument vectors, untraceable frameworks, unresolved or ambiguous
`-l` names, and non-content-complete link inputs still bypass it. Package-level native
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
that package builds directly. A composed group joins whole-program cache
identity on the same terms as the selected package's own native inputs: its
sources reach the same preprocessor discovery probe under the group's own
include scope and macros, and its search directories and link operands reach
the same tree identity and link-input classification.

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
`#include`; the manifest should describe package-level intent and native inputs,
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
GTI does not currently support `-c`, multiple independent input translation
units or `-E` as a source compilation model. Valueless `-DNAME` configuration
flags are implemented and participate in GTI conditional compilation; they are
not textual or value-bearing C-preprocessor macros. Separate compilation and
any new preprocessing model need their own semantics and link contract.

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

The released target kinds are `executable`, `test` and bounded LLVM
`library`. Each has one entry `.gti` source and one whole-program source graph.
A test target changes command selection, not source semantics. A library target
requires `linkage = "shared"` and explicit manifest-selected C exports; it
builds an infallible module on macOS arm64/Linux x86_64 with an adjacent generated
header in a new immutable generation directory. Ordinary `run`/`test` commands
and the C++ backend reject library targets. The default manifest policy enables
library requests; the older disabled `GTI-B2000` checkpoint is historical.

The project workflow reuses the existing compiler/native pipeline, caches the
object/header pair, reports both published artifacts and cleans managed
generations. It does not add separately compiled GTI dependencies or a stable
ABI for ordinary GTI values. Full fallible containment, editor and installed
acceptance remain in the
[embedding campaign](implementation-sequence.md#engine-embedding-delivery-campaign). Source dependencies can
arrive before compiled-library targets.

### Profile

A profile is a named collection of build settings such as optimization,
generated C++ retention, C++ backend standard, and debug information. Built-in
profiles are `dev` and `release`; manifests may refine them without changing
their broad intent.

The Boolean `debug-info` setting is implemented, defaulting to false, with
`-g`/`--debug-info` and `--no-debug-info` executable-build overrides. Its first
slice enables native file/line breakpoints through the existing C++ backend;
see [source-level debug builds](../architecture/build-and-driver.md#source-level-debug-builds).
Rich debugger variables/types and symbol-aware caching are not implied.

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
disabled-warnings = []
dependency-warnings = true
keep-cpp = false

[profiles.release]
optimization = 3
cpp-standard = "c++23"
disabled-warnings = ["unused-mut"]
dependency-warnings = true
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

Bounded registry requirements, logical identity, checksummed immutable source
archives and version-2 lock behavior are implemented in Stage 4. Broader
publishing governance, yanking and registry service policy remain explicit
follow-on work, not prerequisites to the delivered bootstrap.

## Command-Line Contract

The first non-option argument chooses the mode:

```text
gti <path-ending-in-.gti> ...   direct compiler mode
gti build ...                   project build mode
gti check ...                   project analysis mode
gti run ...                     project build-and-run mode
gti new <path> ...              new executable package scaffolding
gti init [path] [--git] ...     existing-directory package scaffolding
gti test ...                    project test mode
gti verify ...                  recorded-provenance verification mode
gti install ...                 build and install an executable target
gti uninstall <name>...         remove an installed program
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
gti init existing/project --git
gti test
gti install
gti install chip8 --path ../chip8 --root ~/tools
gti install --list
gti uninstall chip8
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
[the compiler library migration record](archive/compiler-library-migration.md).
The LSP continues to use `Frontend` directly and must not depend on project
execution, native linking, or cache mutation merely to analyze a document.

### Driver library

Compiler-driver policy lives in the concrete `gti_driver` library. The CLI is
routing, presentation, and exit-status policy. Compilation, native toolchain,
and artifact APIs are implemented alongside project/workspace resolution,
dependency acquisition, registry credentials, build scripts and installation.
The current seams are:

```text
gti_driver
├── CompilationRequest     immutable whole-program compiler request
├── NativeToolchain        structured native tools, inputs and invocation
├── ProjectManifest        parsed and validated gti.toml
├── ProjectWorkspace       resolved acyclic package/source graph
├── ProjectBuildPlan       immutable target/profile/toolchain selection
├── executable build       cache, native objects and atomic publication
├── registry/dependencies  explicit acquisition and verified source identity
└── project facts          read-only compiler/editor configuration
```

The implemented public seams live under `include/gti/driver/`, with compiled
implementations under `src/driver/`: `compilation`, `native_toolchain`,
`manifest`, `workspace`, `project`, `build`, `artifact`, `build_report`,
`dependencies`, `registry`, `registry_credentials`, `build_script`,
`bindgen_import`, `install` and `project_facts`. Private cache/provider files
remain implementation details. The original proposed `BuildPlanner`,
`ArtifactStore` and `DriverInvocation` class/file split is not an outstanding
requirement; an additional split must solve a current authority or dependency
problem. See the canonical [build architecture](../architecture/build-and-driver.md)
for the current interfaces.

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
- resolved dependency identities and verified checksums from `gti.lock`.

Canonical filesystem paths may be recorded for diagnostics but should not be
the sole content identity, otherwise moving a checkout invalidates every
shareable cache entry. The build must not hash arbitrary ambient environment
state. Every environment value that affects output must be explicitly admitted
to the request and key.

The implementation admits a bounded list of native-toolchain environment
variables that affect executable/search-path selection and hashes their exact
presence/value. Content-modeled toolchain resources contribute their contents;
paths are also retained when their native spelling is observable. Declared
include/library directories join through full-tree identity, discovered native
headers and exactly classified link inputs. Opaque trusted argument vectors
still bypass lookup and publication: hashing their spelling does not establish
the resources they select.

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
them as content-complete regular archives or relocatable objects. Declared
library search directories join as the same full sorted content trees, and a
`-l` name joins when searching those directories in link order finds exactly
one modeled candidate whose classification is content-complete. Thin archives,
linker scripts, shared libraries, unrecognized link-input formats, `-l` names
resolving outside the declared directories or ambiguously within one, and
opaque argument vectors still bypass, because a name or script resolves to
files the driver has not identified exactly. Apple framework names now join
only after a bounded link probe using the selected compiler/target/SDK policy
produces a linker map with one exact text stub or binary; that path and content
join the key, while an unavailable or ambiguous trace still bypasses. Narrowing
a bypass must always come from establishing exact identity, never from widening
what the key trusts.

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
- runtime include path;
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

Native compiler/link failures remain labeled as backend build failures and
retain available representation and raw diagnostic evidence for investigation.
Build-system diagnostics must not disguise backend failures as GTI source
errors or label every external-toolchain failure as a compiler bug. Default
human-readable output is backend-neutral; saved artifacts identify their actual
format, and explicit input spellings and metadata schemas remain compatible.

`gti metadata --format json` exposes the manifest schema version, canonical
manifest and package paths, package identity, host target fields, sorted
profiles, sorted executable/test targets with their kinds, and each
target/profile output and generated-C++ path. Current metadata schema version 14 reports
every declared execution profile and effective GTI configuration flag set plus
every effective native category, C source, C++ source, C standard, C argument,
and ordered link operand, the
composed per-dependency native groups (`dependencyNative`), and the
resolved workspace/package/dependency graph, declared build-script policy, and
approved per-plan build-script commands, inputs, and output directories. It is
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

Status: complete for the source-package bootstrap accepted by ADR 025.

The official backend is Gitea Generic Packages under configurable `gti-lang`
ownership, behind logical registry identity and an independent GTI metadata/
archive protocol. General bounded SemVer solving, exact lock closures,
deterministic source packaging, completeness-last publication, explicit
acquisition, user caching and integrity checks are implemented. Native build
script permission remains consumer-owned. Tests use in-memory backends and a
local fake Gitea service, not a live registry dependency.

One-time scoped registry login is implemented with protected macOS Keychain
storage and automatic acquisition/publication lookup. Normal Gitea login uses
username/password/optional TOTP to issue a package-only token; existing-token
login remains available. Failed issuance/save has attempt-scoped rollback with
visible partial-outcome diagnostics. The environment override
remains available on all hosts. Credential-store and optional authentication
interfaces are independent of transport and archive format; other OS stores,
browser/device login and token refresh remain future work, with no plaintext
fallback. The current build/driver document owns the exact workflow.

Package signing, yanking, mirrors, search, richer feature/target solving and
multiple versions of one canonical workspace name remain deferred. Initial
package publication and publishing permissions are service operations, not
automatic compiler release side effects. A registry remains a governance/trust
commitment rather than a guarantee that arbitrary package code is safe to run.

### Build hooks

Status: consumer-approved native package build programs are complete, including
GTI-authored `build.gti` programs and the original external-command form.

The implemented hook accepts exactly one GTI source file or exact argv command,
declared contained inputs, content-based cache identity, a managed output
directory, and a versioned TOML output contract. GTI programs use the ordinary
compiler and the source-defined `<std/build>` context, exact-command, and
structured-output API; external commands remain compatible. Dependency scripts
require an exact `name@version` entry in
the selected package's `[build].allow-build-scripts`; resolution, metadata,
fetch, and check do not execute code. The implementation currently provides:

- declared input paths and validated contained output paths;
- deterministic cache identity over inputs, command, toolchains, and target;
- a clear host-command versus target-output environment contract;
- consumer-owned dependency permission and lockfile-backed source identity;
- dependency-first ordering inherited from the acyclic package graph;
- structured driver diagnostics and versioned machine-readable outputs.

Sandboxing and enforced network denial remain future hardening. Until then,
approval explicitly authorizes arbitrary host code from the reviewed package
revision. Windows execution support is also deferred.

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

GTI now supports explicit LLVM backend selection without changing package
identity. Backend-specific policy remains separate from language/package
identity; compiled GTI library reuse is still the staged goal below.

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
- Accept `--name <name>` without adding editions, library targets, or
  dependency fields. Scaffolding has no version-control side effects unless
  `--git` is explicitly selected.
- Accept `--git` on `gti init` as an explicit version-control side effect. It
  creates a standalone repository and canonical `/build/` ignore rule, refuses
  user-owned `.git` or `.gitignore` entries, and rolls back scaffold-owned
  output when Git fails.

Acceptance criteria:

- generated manifests load through the ordinary schema-version-1 parser;
- no existing manifest, source, symlink, or destination is overwritten;
- invalid package names fail before filesystem mutation;
- Git conflicts fail before filesystem mutation and Git invocation failure
  leaves no scaffold-owned output behind;
- inherited Git repository-location overrides fail before mutation, and a
  metadata reservation collision never authorizes deleting the existing entry;
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
- Report the resolved C and C++ inputs through metadata (currently schema
  version 14), while keeping `check` native-tool-free and output-free.

Acceptance criteria:

- a project containing only GTI declarations and declared C/C++ definitions
  builds and runs without separately precompiled objects;
- selected paths, extensions, standards, arguments, and platform fragments have
  focused manifest diagnostics;
- a failed C or C++ source compilation preserves the prior object and
  executable and prevents final linking;
- direct compilation and prebuilt native link inputs retain their existing
  behavior.

### Post-Milestone 3 addition: Objective-C++ native bridges

Status: complete

`cpp-sources` accepts `.mm` for selected macOS targets, including composed
dependency inputs and approved build-script outputs. Object compilation and
cache dependency discovery share explicit Objective-C++ language selection;
framework linking and ordinary C ABI declarations remain the existing paths.
`objective_cpp_bridge` executes the Foundation bridge with default ownership
and explicit ARC, C++20/C++23, header invalidation, cache reuse and native
failure preservation. `project_model` and `driver_pipeline` cover target
rejection, exact declaration spans and command construction. General Objective-C
type import, `.m`, non-Apple runtimes and a dedicated ARC policy are not part
of this slice.

### Milestone 4: whole-program incremental cache

**Complete:**

- Expose loaded source paths and logical dependency data needed for hashing.
- Implement content-addressed whole-program backend/native artifact keys.
- Add verified cache hit/miss/corruption explanations under `--verbose`.
- Add `--no-cache` for verification and debugging.
- Add `gti run --no-build` as an explicit stale-artifact escape hatch with
  managed-path and executable validation and no acquisition or build side
  effects.

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
- Resolve Apple framework names with an actual selected-linker map and hash the
  exact selected text stub or binary, retaining a stated bypass whenever that
  trace cannot establish one identity.

Acceptance criteria:

- **Passed:** a project with a declared C source reports a verified cache hit
  on an unchanged rebuild and misses when any discovered header, shadowing
  header, declared include tree entry, or link input changes;
- **Passed:** every remaining bypass reason is reported explicitly and covered
  by CLI tests, including opaque argument vectors, name resolution, library
  search directories, non-content-complete link inputs, injected environment
  search paths, and time-sensitive macros;
- **Passed:** an unchanged Apple framework build hits the cache, changing the
  exact linker-selected framework stub misses, and untraceable framework
  selection bypasses;
- **Passed:** serial and parallel `--all` builds produce byte-identical
  artifacts and byte-identical ordered output, including under a failing
  target;
- **Passed:** malformed or partial cache entries are diagnosed, never
  executed, and replaced only after a successful rebuild.

### Milestone 5: tests and workspaces

- **Complete:** accept manifest `test` targets, build each selected root as an
  independent whole program, execute them in deterministic target-name order,
  continue after runtime failures, and propagate the first failing status.
- **Complete:** test progress and pass/fail summary counts use the shared CLI
  terminal-colour policy. Captured/default or explicitly uncoloured output keeps
  the existing text; test-program streams and first-failure status are unchanged.
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
  units. The compiler query API accepts the same graph, and the driver now
  publishes that graph as read-only `ProjectFacts`;
- **Complete (T-LSP-04):** editor analysis and completion consume those facts
  without fetching/building, preserve standalone analysis, and apply dirty
  source overlays over the resolved graph.

Acceptance criteria:

- **Passed:** undeclared filesystem access is rejected;
- **Passed:** transitive and sibling package declarations do not leak;
- **Passed:** CLI and LSP resolve the same package source graph through
  driver-owned `ProjectFacts`, including structured locked-Git failures;
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
- **Passed:** plans with dependency native groups join the whole-program
  cache through the group's own preprocessor discovery, and a change to a
  group-discovered header changes the key rather than restoring a stale
  executable.

### Milestone 7: Git resolution and lockfile

Status: complete for pinned full revisions. This milestone's Git contract does
not admit mutable branch/tag selectors; registry requirements are delivered by
the distinct Stage 4 source-package protocol.

- **Complete:** fetch/cache storage under `build/gti/deps/git` (bare object
  databases keyed by URL identity, blob-extracted checkouts keyed by URL and
  revision), exact revision resolution, and `gti.lock`.
- **Complete:** `gti fetch [--offline]`, and `--locked`/`--offline` on
  build, check, run, and test.
- **Complete:** Git lock updates are explicit. `gti fetch` re-derives every Git
  checksum from the immutable object database, so a locally modified checkout
  cannot launder content into a fresh lock. Builds only consume the lock.
  The later registry `gti add` command is another explicit lock writer, not an
  automatic build-time update.
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

### Post-Milestone 7 addition: declared cross-compilation toolchains

Status: complete for the declared-toolchain scope; a per-target GTI runtime
archive and a `gti run`/`gti test` runner concept remain out of scope.

Cross-target builds were listed above as a case the direct driver cannot
express. Project mode now expresses it.

- **Complete:** `--target <triple>` on `build`, `check`, `run`, and `test`,
  parsed with the compiler's own `parseTargetTripleResult` so project mode
  and `gti bindgen` share one spelling vocabulary. Omitting it selects the
  host, and `gti build --all` forwards the selection to each child build.
- **Complete:** a `[toolchains.<triple>]` manifest table declaring `cxx`,
  optional `cc`, optional `sysroot`, and an optional `native-target` native
  triple spelling. Keys normalize through the target vocabulary, duplicates
  that select one target are rejected, and entries sort by declared spelling
  for deterministic identity.
- **Complete:** the resolved sysroot and native triple become one
  driver-owned target policy emitted as `--sysroot=`/`--target=` on the
  whole-program link, on every declared C and C++ object compile, and on the
  cache's dependency-discovery probes. These are exactly the options every
  user-supplied native argument vector rejects, so the driver is their only
  source.
- **Complete:** the selected triple and resolved toolchain join the project
  model identity, and the target policy joins the whole-program cache key.
- The GTI runtime archive and runtime headers are still discovered once for
  the host toolchain. Supplying a runtime built for the selected target uses
  the existing `GTI_RUNTIME_LIBRARY`/`GTI_RUNTIME_INCLUDE` overrides; a
  target-built runtime/resource selection is remaining work. Its prerequisite
  is a reproducible target runtime and provenance contract; it does not need
  another user-reported failure before implementation can begin.

Acceptance criteria:

- **Passed:** an unchanged manifest still builds for the host, and a host
  build carries no cross policy;
- **Passed:** a `build`, `run`, or `test` for a target that is not the build
  host, with no matching `[toolchains.<triple>]`, fails with `GTI-B1904`
  naming the missing table instead of reusing host tools;
- **Passed:** `gti check` analyzes any supported target without a declared
  toolchain, because it compiles nothing native;
- **Passed:** `gti run` and `gti test` refuse a non-host `--target` rather
  than building a program this machine cannot execute;
- **Passed:** a declared toolchain without `cc` rejects a build that compiles
  native C sources rather than reaching for host `cc`;
- **Passed:** a host plan and a cross plan of one package never share a
  project model identity.

### Post-Milestone 7 addition: program installation

Status: phase 1 implemented for macOS and Linux. The current contract is in
[Installing Programs](../architecture/build-and-driver.md#installing-programs).

`gti install` builds one executable target of a local project with the
`release` profile and copies it into `<root>/bin` (default `~/.local`).
`gti uninstall` removes it. An ownership record under
`<root>/share/gti/installs.toml` limits replacement and removal to programs
install placed. Phase 1 acceptance:

- **Passed:** default root, `GTI_INSTALL_ROOT`, and `--root` precedence.
  Explicit relative `--root` is resolved against the working directory;
  environment roots must be absolute. The filesystem root is refused, while
  valid missing installation directories are created as needed;
- **Passed:** the installed program is a copy that survives `gti clean`, with
  `0755` permissions, published by rename;
- **Passed:** same-package reinstall replaces without `--force`; an unrecorded
  file or another package's program is refused before building and left
  untouched unless `--force` is given;
- **Passed:** test targets and non-host `--target` triples are refused;
- **Passed:** uninstall removes only recorded names, tolerates an already
  deleted file, leaves a replaced file alone, and continues past failures;
- **Passed:** an unreadable record or one naming a path outside `<root>/bin`
  fails closed.

Later phases:

- Windows: `.exe` names, `%LOCALAPPDATA%`-style default root, `ReplaceFileW`
  publication of a possibly running program, and `LockFileEx` for the record;
- registry and pinned Git sources (`gti install <package>[@requirement]`)
  through the existing resolver and package cache, without a consumer
  workspace;
- installing every executable target of a package, and skipping an up-to-date
  reinstall.

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

GitHub Actions publishes Linux x64 and Apple Silicon toolchains to GitHub
Releases (nightly, then the beta/stable train). Linux arm64 awaits a reviewed
runtime compiler profile; Windows publication is not implemented.
Applicable host/path behavior needs focused evidence on those supported release
platforms; Windows publication/execution needs its own implementation and
validation before claiming support. Filesystem tests must avoid assuming case
sensitivity, POSIX separators, executable suffixes or symlink behavior without
an explicit platform branch.

The LSP shares driver-owned project facts, named profile resolution, target
inference and execution/runtime analysis configuration. Explicit package/target
selection, cross-target selection and future library facts are remaining
editor-policy work, not a second manifest resolver. It must retain immutable
document overlays and must not fetch dependencies, invoke build hooks, clean
outputs or mutate a lockfile as a side effect of opening an editor.

## Explicit Non-Goals For Version 1

- replacing CMake for building the GTI compiler itself;
- implicit or unapproved execution of package build programs/hooks (explicit,
  trust-approved `build.gti` and external commands are implemented);
- a central package registry (superseded by the implemented Stage 4 bootstrap);
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
6. Current metadata JSON schema version 14 is a read-only enumeration of every
   target/profile plan, including target kinds, declared execution profiles,
   effective GTI configuration flags, resolved native C and C++ sources, C
   policy, other native inputs, ordered link operands, and composed
   per-dependency native groups. It also publishes the selected workspace and
   sorted source-package graph. Platform selection and precedence use the
   resolved target.

## Historical First Pull Requests

These seams were delivered; this list is not the current prompt queue. The
initial implementation deliberately used small changes:

1. **Direct CLI contract tests** with no production behavior change.
2. **Native toolchain extraction** with no production behavior change.
3. **Compilation request extraction** and explicit host `TargetInfo` plumbing.
4. **Manifest model and parser tests** without building a project.
5. **Single-target `gti build`** reusing the direct compilation request.

That sequence established reviewable seams before dependency management,
caching and workspaces added state. Current work should use those interfaces
and the reconciled implementation queue, not repeat their extraction.

## Remaining Build Goals (2026-10-11)

These goals feed the existing [implementation sequence](implementation-sequence.md),
not a second milestone queue. Earlier benchmark-first or demonstrated-need
language is superseded by ADR 012; prerequisites and acceptance still apply.

| User-facing outcome | Prerequisite / next coherent slice | Acceptance |
| --- | --- | --- |
| Complete the engine-host module workflow | Infallible LLVM project libraries, context lifecycle, generated wrappers/header and cache/report/clean publication are implemented. Complete E-EMBED-01B/C fallible containment, T-EMBED editor integration and Q-EMBED installed/platform acceptance. | Atomic shared artifact/header publication, exact exported ABI and runtime resources, cache/report/clean support, and installed load/call/fail/unload/reload evidence through `Q-EMBED-01`. |
| Cross-build without accidental host resources | Reproducible target-built runtime/header closure, then immutable driver resource selection and target provenance. | Native objects, runtime, layout and reports agree on the target; missing target resources fail closed; host builds remain unchanged. |
| Reuse builds without stale native inputs | Extend the existing exact native discovery with generated-unit SDK/header identity and concurrent-input publication checks. | Changing a discovered input invalidates reuse; unsupported opaque inputs still bypass; races cannot publish under an earlier identity. |
| Install and authenticate across more package/host workflows | Reuse delivered registry/provider/credential seams and local install receipts; add one source-install or host-support profile at a time. | Explicit trust/acquisition, immutable identity, credential non-disclosure, receipt-safe replacement/removal and appropriate platform evidence. Registry governance remains distinct from compiler releases. |
| Shorter build/edit cycles | Use the incremental/query and native-object goals in [performance tooling](performance-tooling.md) and [LSP roadmap](lsp-evolution.md); identify reused facts and invalidation boundaries first. | Current-head compile/edit latency and memory evidence with unchanged diagnostics, ownership and source-overlay publication. No pretend per-file GTI translation-unit model. |


## Compiled library and separate-compilation stages

The accepted direction is [ADR 032](../decisions/032-compiled-library-bundles.md).
A library target, not each source module, is the reusable compilation boundary.
These stages refine the existing shorter-build-cycle goal. Normal project
commands consume the bounded typed `.gtilib` profile described below; the full
semantic and production-promotion goals remain open.

| Stage / owner | Prerequisite and deliverable | Required acceptance evidence |
| --- | --- | --- |
| F0 container / driver — implemented | Versioned `.gtilib` codec, compatibility envelope, deterministic object/dependency inventory; no CLI or semantic importer | Round trips, order independence, corruption/truncation/version/limit/path rejection and exact identity/dependency mismatch tests; affected driver regressions. |
| F1 identity and interface / frontend + semantics — identity foundation in progress | Finalize declaration-key grammar and metadata schema; export/private closure, module ownership, complete type/lifetime/borrow/callable contracts, generic declaration recipes and diagnostic provenance; import into existing semantic domains | Export/re-export visibility equals source mode; unrelated user-defined borrowed type and private layout dependencies; ID remapping across separate processes and reordered inputs; malformed references, incomplete summaries and untrusted private-capability declarations rejected. |
| F2 unit lowering/linkage / HIR + MIR + LLVM — typed vertical slice implemented | F1; imported declaration/body distinction, stable symbol plan, selected runtime closure, per-library initialization/retirement, object verification; immutable driver target request distinct from C embedding | Multiple modules in one library, independent consumer links/runs with no library implementation source; correct cleanup/failure and exactly-once globals/TLS; no duplicate main/symbols; retained exports; platform object/ABI and runtime mismatch gates. C++ receives no new feature work. |
| F3 generics and early constants / semantics + staged HIR/MIR | F1/F2; generic recipe importer and instance inventory, shared consumer instantiation owner, query-driven completed constexpr closures | New consumer substitution, constraints/defaults/specializations, early array extent/value argument/static assertion/branch shaping, resource/cycle errors and compile-time/runtime parity; no requirement to seal whole-program MIR before these queries. Private generic dependencies and unrelated user types included. |
| F4 reusable target graph / driver — same-manifest DAG implemented | F1–F3; explicit module-to-library-target mapping, DAG/diamond validation, complete configuration/toolchain/runtime/native fingerprints, bounded file adapters and atomic publication | Consumer-only edits reuse a library without reopening its implementation source; source/body/config/target/runtime/compiler/dependency changes invalidate it; conflicting diamonds and cycles reject; input-race and corrupt-cache recovery; direct/source project behavior preserved. |
| F5 promotion / verification + tooling | All admitted semantic families, example project, build reports, LSP/import diagnostics, native platform coverage and documented unsupported families | End-to-end fixtures below, installed consumer checks, optimized runtime/allocation/code-size comparison and cold/warm build/memory/bundle-size evidence. Optional LTO/body indexing follows measured needs and retains semantic verification. |

The end-to-end fixture must build a library target containing an exported API,
a second module and private implementation files; a separate executable package
then imports only its compiled artifact. Remove access to producer implementation
sources to prove reuse. Exercise an ordinary function, user-defined nominal type,
borrowed return with negative escape/mutation cases, a new generic substitution,
and an early constexpr query. Edit only the consumer and verify the library is
unchanged and not reanalyzed. Change the library body, public contract, dependency,
configuration and compiler/runtime identity in separate cases and prove correct
rebuild or explicit incompatibility. Include truncation, payload corruption,
unknown versions, forged but well-checksummed invalid metadata, unsupported target,
and duplicate/conflicting dependency cases. Check native execution and cleanup,
not only successful linking. C embedding keeps its own existing ABI tests.

F0 intentionally has no runnable GTI example: it changes no language/build surface.
F2/F4 must ship a runnable multi-module library/consumer example and inventory
updates with the first user-visible compiled-library mode. `.gtimeta` remains
reserved until an analysis-only consumer justifies independent metadata use.


F1 progress (persistent declaration identity): the compiler now projects and
reads a bounded, canonical key for nongeneric scalar-parameter namespace functions.
Tests use real semantic records, reordered/relocated source and independent
processes. This completes only the first structural identity profile, not F1.
Nongeneric nominal declaration keys and an explicit per-snapshot type registry
now remap class/enum references to fresh local IDs. Scalar/nominal, reference,
raw-pointer and fixed-array recipes retain their structural facts; tests include
real source classes/enums, independent processes, conflicting bindings and malformed
input. Primary generic class/enum owners now record ordered type/value/final-pack shapes,
and concrete recipes preserve fully resolved type/uint64 substitutions with exact
arity checks and fresh-ID remapping. Real generic source fixtures and cross-process
checks cover this profile. Transformed families, exact specializations, nested
class owners and dependent body recipes remain unsupported. Next extend those
owner identities, then serialize export/private closure and complete callable
contracts. Imported declarations must enter existing semantic lookup and staged
query catalogs before F1 can pass its acceptance gates. No function key authorizes
a call, substitutes for a borrow summary, or promises a native symbol ABI.


F1/F2 executable progress (scalar vertical slice): an LLVM development API now
builds a multi-module `.gtilib` and compiles/links independent consumers after
all producer sources are deleted. Primitive/void interface metadata, overload-family selection, explicit
semantic import admission, compiler-verified stable linkage, native-object
checks and exact toolchain/runtime/configuration checks are implemented.
The registered `compiled_scalar_library` example exercises separate processes,
consumer-only edits, visibility and rejection at O0/O1/O3. This advances the
user-visible workflow beyond codecs without marking general F1 or F2 complete.

Project workflow progress: `gti-library` targets now select root-module exports
through the compiler and publish `.gtilib`; executable targets resolve one
`compiled-library` or several independent `compiled-libraries` artifacts and
virtual interfaces through immutable project/build requests. The combined
profile caps imports at 64 libraries and 256 declarations; typed result-aware
symbols and source-scoped bindings preserve each artifact's callable contract. Logical identities come from the manifest and root module. Bounded
reads, atomic publication and native staging preserve source-absent reuse.
Same-manifest dependency graphs and conservative freshness now build on this path.
`compiled_library_project` exercises the normal CLI and failure preservation.

Read-only query progress: `gti check` now validates scalar producer exports and
source-absent consumer imports through shared artifact preparation and the normal
frontend/lowered binding pipeline, without native emission or output publication.
Structured source diagnostics remain intact. This does not admit artifact-aware
LSP snapshots or certify producer native emission/consumer linking.

Primitive capability progress: bool/char/fixed integer/float/double arguments
and results, void results, overloaded exports and checked scalar failure
propagation are implemented. The primitive example covers O0/O1/O3 source-absent
execution and consumer cleanup; the multi-library example covers independent
imports, unused selections and overlapping-name rejection. These remain bounded
F1/F2/F4 slices, not completion of the general stages.

Typed-interface progress: concrete generated value records, nested record fields,
private field layout, scalar defaults and integral enums cross the native boundary.
Fresh-session type registries remap their IDs and validate canonical declaration
closure against lowered facts. Checked read/mutable reference parameters and
whole-parameter borrowed returns preserve semantic/MIR origin and access rules;
consumer escape and overlapping-loan rejection use ordinary analysis. The
`compiled_value_types` and `compiled_borrow_library` examples remove producer
sources before consumer analysis and execution. Custom ownership/drop, borrowed
record fields, projected/conditional returns, generic definitions and constexpr
queries still require richer contracts. These bounded additions do not complete F1/F2.

Namespace progress: qualified root-module functions and nominal declarations retain
namespace identity, inherited namespace exports, overload visibility and private
layout dependencies. Virtual interfaces use private root aliases to preserve
resolved types through namespace shadowing. `compiled_namespaces` distributes two
libraries with same-leaf types/functions and checks source-free execution and
borrowing at O0/O1/O3. Nested class-owned declarations and nominal
types from ordinary non-root source modules still need richer ownership contracts.

Cross-library nominal progress: the interface records each type's defining
package/target/module and imports that dependency's virtual declarations. Records,
private layout dependencies, enums, scalar defaults and whole-reference contracts
compose across chains and diamonds without copying nominal identities. Logical
owner bindings survive unused-callable pruning, and stale or missing defining
artifact receipts reject. `compiled_type_forwarding` covers O0/O1/O3, consumer-only
reuse, private-layout invalidation, relocated paths and source-free execution.
This does not complete whole-module re-export semantics, ordinary multi-module
source ownership, colliding qualified names or the remaining lifecycle/generic
contracts.

Dependency progress: same-manifest `compiled-dependencies` edges build targets in
topological order, deduplicate diamonds and reject cycles. Bundles retain exact
direct dependency receipts; consumers validate the complete transitive closure.
A conservative source/configuration/compiler/runtime/dependency fingerprint reuses
unchanged library outputs and rebuilds stale or corrupted ones. This still reads
producer source inputs to establish freshness. `check` remains read-only and
requires existing dependency artifacts. Explicit distributed artifact closures
remain usable without sources. The `compiled_dependency_project` example covers
these paths. Package-qualified source edges, zero-source-read freshness, input-race
publication proofs and general cache/report integration remain F4 prerequisites.

Next coherent outcomes: general ownership and lifecycle metadata, generic/staged
constexpr recipes, complete multi-module ownership/re-export contracts, composable exactly-once
globals/TLS, and artifact-aware diagnostic provenance/LSP snapshots. Production
acceptance still requires both native platforms, installed-toolchain evidence,
representative performance/build-cost measurements and the full F1–F5 fixture.


F5 evidence prerequisite exposed by the scalar slice: the existing LLVM
promotion provider probes ordinary source and C-embedding requests. It cannot
yet bind an artifact-input profile to a verified import-plan receipt. The
`CompiledScalarImport` ledger row therefore records that explicit promotion
evidence gap; it does not claim that a source-only probe exercised the new path.
The mandatory `compiled_scalar_library` CTest already executes the real producer,
source deletion, import and native link on macOS arm64 and Linux x86-64. Promotion
must add exact bundle/runtime/compiler identities and import-plan receipts to
the evidence provider, retaining those execution tests and malformed-input
controls. This is not a waiver of the general F5 readiness gates.
