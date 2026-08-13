# Build And Driver Architecture

Status: Current implementation. Local path dependencies and deterministic
workspaces are implemented; remote acquisition, lockfiles, and finer-grained
incremental compilation remain plans.

GTI has one language compilation pipeline and two user entry modes:

```text
gti source.gti ...                 direct mode
gti build|check|run|test|clean|metadata project mode
                 \                /
                  gti_driver requests
                         -> Frontend
                         -> optimization/backend
                         -> native toolchain when required
```

## Target Boundaries

- `gti_compiler` contains reusable frontend, IR, optimizer, backend contracts,
  and compiler-owned language queries. It does not depend on manifests, TOML,
  native processes, or project output policy.
- `gti_driver` in `include/gti/driver/` and `src/driver/` owns immutable
  compilation/build requests, resources, manifests, project plans, artifacts,
  native command construction, and process execution.
- `src/cli/` owns argument routing, diagnostics/output presentation, and exit
  status. It constructs driver requests rather than reimplementing compilation.

`include/gti/target.h` owns the selected target vocabulary and the immutable
`TargetDataLayout` value carried through these layers. The public value contains
only GTI domains, byte sizes, ABI/preferred alignments, pointer width, and
endianness; no LLVM or native compiler type crosses the interface. Private
`src/compiler/target.cpp` code uses `llvm::Triple` only to normalize and
classify a spelling, then maps it into the GTI representation. The parser uses
the same `os`/`vendor`/`arch` property vocabulary rather than maintaining a
second spelling table.

The current supported set is arm64 or x86_64, little-endian, 64-bit, on macOS,
Linux, or Windows. `parseTargetTripleResult` distinguishes malformed triples
from unsupported architecture, endianness, and operating-system cases, while
the compatibility `parseTargetTriple` wrapper returns only an optional target.
`Frontend` rejects a selected target whose data-layout value is unsupported
with `GTI-S2062` after source loading and before parsing, semantic selection,
HIR, MIR, optimization, or backend invocation. A compiler-library caller that
assembles `TargetInfo` directly is checked against the same OS, vendor,
architecture, and layout vocabulary; assigning a canonical layout to an
unknown target name does not bypass the boundary.

`gti_driver` depends on `gti_compiler`; the reverse dependency is forbidden.
Both are installed static exact-version libraries without a stable cross-version
compiler ABI promise. Installed consumers use `find_package(GTI CONFIG)` and
the `GTI::compiler`, `GTI::driver`, and `GTI::runtime` targets rather than
linking archive paths by hand. Those targets carry the required LLVM support
link dependencies. Self-contained release packages include the pinned LLVM
archives; a package produced against system LLVM requires that exact LLVM CMake
package on the consumer machine. A CMake project consuming a system-LLVM GTI
package must enable both C and C++ before `find_package(GTI CONFIG)`, because
some supported LLVM distributions run C feature probes while loading their
package configuration. The bundled release package does not load a downstream
LLVM package.

## Direct Mode

Direct mode accepts one entry `.gti` source and remains manifest-independent.
Its source graph produces one whole-program C++ artifact and one native compiler
invocation. Native arguments after `--` remain exact argv values, but cannot
override language invariants: the driver appends `-fno-fast-math` and
`-ffp-contract=off` after every forwarded argument, followed by the generated
artifact's `__gti_strict_ieee754=1` policy marker. `TargetInfo` is resolved
before frontend entry and passed unchanged through semantics, optimization,
and backend generation. `--execution-profile single-threaded|concurrent`
selects its execution-profile fact; omission remains single-threaded. The
option changes frontend global/static policy and never infers runtime support
from native arguments.

The direct CLI currently selects `TargetInfo::host()`; it does not expose a
general cross-compilation option. Compiler-library clients can request another
supported normalized target for analysis, but the data-layout contract does
not configure a native compiler, sysroot, runtime, or linker for that target.

## Project Mode

The implemented manifest path discovers `gti.toml`, parses schema version 1,
resolves executable and test targets, profiles, and structured native inputs
(including declared C and C++ sources), and produces immutable
`ProjectBuildPlan` values. `build`, `check`, `run`, `test`, `clean`, `metadata`,
`new`, and `init` are implemented. `check` stops after the frontend; `run`
executes one executable target through exact arguments and inherited streams;
`test` deterministically plans every test target or one named test and
builds/runs each as an independent whole program in target-name order. A build
failure stops the command; a runtime failure is recorded while later tests
continue, and the command returns the first failing process status after the
summary. `build` and `check` can explicitly select either kind, while `run`
rejects a test target and points to `gti test`. `clean` removes only a validated
tool-owned subtree. When a package has one executable plus test targets, that
executable remains the default for `build`, `check`, and `run`. `metadata` is
read-only.

`new` and a source-creating `init` scaffold the implemented owned-argument
entry form, `int main(int argc, std::vector<std::string> argv)`, with the
required standard string and vector includes. `init` continues to preserve an
existing regular entry source rather than modernizing or replacing it.

Selected `.c` inputs are compiled by a separately resolved C compiler into
staged objects beside the generated C++ intermediate. Selected `.cpp`, `.cc`,
and `.cxx` inputs follow the same managed-object path using the resolved C++
compiler, profile/CLI C++ standard, optimization, include paths, and native
`compile-args`. C objects precede C++ objects, and all declared-source objects
precede the runtime and manifest libraries in the existing final C++ link
invocation. Each successful object atomically replaces its prior intermediate;
a failed source compile preserves any prior object and executable and retains
the generated C++ for diagnosis. C compiler selection is `--cc`, then `GTI_CC`,
then `CC`, then `cc`; C++ compilation and final linking use `--cxx`, then
`GTI_CXX`, then `CXX`, then `c++`.

Project and direct modes construct the same `CompilationRequest` and
`ExecutableBuildRequest`. A manifest describes package/target policy; it does
not replace `SourceGraph` or flatten GTI visibility.

### Workspaces And Local Source Dependencies

Manifest schema version 1 accepts a bounded local package graph:

```toml
[package]
name = "game"
version = "0.1.0"
# source-root = "src" # optional; this is the default

[dependencies]
math = { path = "../math" }

[workspace]
members = ["packages/game", "packages/math"]
```

`[targets]` is optional so a source-only dependency package does not need a
dummy executable or test. Selecting such a package for `build`, `check`,
`run`, or `test` produces a focused no-target diagnostic; it remains valid as
a dependency.

A workspace root is itself a package and lists canonical member roots.
Commands run inside a member select that member; commands at the root select
the root package; `--package <name>` selects any root/member package
explicitly for build/check/run/test/metadata. Package names are unique across
the graph. The resolver loads
members and recursively declared local dependencies, rejects nested workspace
declarations, duplicate canonical dependency roots, duplicate names, missing
source roots, and package cycles before target selection or compilation. It
never performs network access.

Workspace builds share `<workspace>/build/gti`; member artifacts live below
`packages/<package>/...` so equal target names cannot collide. The cache is
shared but its model and source identities include the complete sorted package
graph, dependency aliases, package versions, and package-relative unit paths.
Standalone package layout remains unchanged. `clean` resolves the workspace
when possible and removes that shared managed subtree; if a manifest is broken,
it preserves the older recovery behavior and cleans only the nearest literal
package subtree.

`CompilationRequest` carries immutable `PackageSourceRoot` values produced by
`gti_driver`. `SourceLoader` remains the only include resolver. In a project
compilation, `#include <math/add>` resolves `math` only through the including
package's direct dependency aliases and loads `<math-source-root>/add.gti` as
ordinary untrusted GTI source. Transitive aliases do not leak, and quoted
includes cannot cross an owning package boundary to bypass the manifest edge.
Direct mode receives no package graph and therefore rejects package angle
includes without consulting nearby manifests.

This slice composes GTI source only. A dependency package with package-level
native inputs is rejected because silently dropping or reordering its native
contract would be unsound. Native dependency composition remains future work.

Direct mode also exposes `--emit-native-header`. It runs the same complete
frontend, optimization compatibility check, and MIR verification as C++
emission, then selects `NativeHeaderBackend` instead of `CppBackend`. The mode
writes one `.h` artifact, defaults to `<entry>.native.h`, applies the loaded
source overwrite guard, and performs no native compilation or linking. It is
mutually exclusive with `--emit-cpp`, `--keep-cpp`, and trailing compiler
arguments. Automatic placement of this generated artifact into project-native
include paths is a later build-system convenience; current projects may check
in or explicitly regenerate the header and list its directory under the
existing native include settings.

## Whole-Program Project Cache

`gti build`, `gti run`, and `gti test` use a workspace-local,
content-addressed
whole-program cache by default. Direct `gti source.gti` mode and `gti check`
remain uncached. `--no-cache` disables both lookup and publication for one
project build/run/test command; under `--verbose`, the CLI reports the cache
identity and whether the request hit, missed, recovered a corrupt entry, or
was conservatively bypassed.

The cache does not parse includes or manifests independently. The driver calls
`loadCompilationInputs`, which uses the compiler's existing `SourceLoader` to
produce the exact `SourceGraph`, source text, source diagnostics, and logical
dependency edges for the request. A miss moves that same loaded state into
`Frontend::analyzeLoaded`; a hit occurs before parsing, semantic analysis, HIR,
MIR, backend generation, declared-native-source compilation, or final native
linking.

The current key includes:

- the GTI release and project-manifest model identities, backend C++ standard,
  optimization, execution profile, target triple, and complete GTI data
  layout;
- SHA-256 content identities for every loaded GTI unit, ordered logical source
  edges, and standard-library import names;
- the native C/C++ compiler command, resolved executable content, and
  `--version` output;
- runtime headers/archive and the C++20 compatibility headers when selected;
- structured native C/C++ sources, include/library directory contents, link
  files, standards, ordered operands, and exact argument vectors; and
- the bounded native-toolchain environment variables that can change compiler
  or linker selection/search behavior.

Application GTI paths in a resolved package graph use package
`name@version` plus the package-relative unit path; standalone application
paths remain relative to the selected source root. Standard-library units use
logical import names, and toolchain resources use content identity. A pure-GTI
workspace can therefore move together with its `build/gti` subtree without
invalidating an otherwise identical entry. Explicit external/native paths
retain canonical path identity because native `__FILE__`, search order, and
external ownership make those paths semantically observable.

Entries live beneath `build/gti/cache/v1/<sha256>/` and contain generated C++,
the executable, and strict metadata recording the digest and size of both.
Executable permissions are recorded as well. Metadata is published last and
acts as the commit marker. A hit verifies every digest and the executable mode
before atomically copying the executable to the requested output; it
also restores generated C++ when `keep-cpp` is active. Missing entries rebuild
normally. Incomplete or corrupt entries are diagnosed, never executed, and
are replaced only after a successful rebuild. Cache-publication failure does
not discard a successfully published program. Deleting only `build/gti/cache`
does not modify sources or published target artifacts; `gti clean` deliberately
removes the entire validated `build/gti` subtree, including both.

Exact native argument strings are part of the key, but the driver does not
interpret embedded paths inside trusted `c-compile-args`, `compile-args`,
`link-args`, or `raw-args`. A package whose trusted argument refers to an
undeclared external file should use the structured path fields or
`--no-cache`; changing an undeclared file cannot be discovered safely from an
opaque argv element.

Verbose native command lines are presentation text. They retain the leading
`+ ` trace marker, and the remainder is encoded as a reproducible POSIX-shell
command; native execution itself always passes exact argument vectors without
a shell. The display contract does not claim compatibility with Windows
`cmd.exe` or PowerShell syntax.

Each manifest `[profiles.<name>]` table may set
`execution-profile = "single-threaded"|"concurrent"`; the selected value is
resolved into the plan's `TargetInfo`, and the command-line option is an
explicit project override. Metadata schema 7 publishes the declared value for
every profile plus the selected workspace and sorted package/dependency graph.
This build profile field selects static language policy only;
the later target/runtime `threads` capability remains independent.

Arguments after `gti run --` are passed as exact program arguments and become
owned values when the target uses
`main(int, std::vector<std::string>)`. In contrast, arguments after `--` in
direct compilation mode still belong to the native C++ compiler; run the
produced executable separately to supply its program arguments. This routing
rule is driver policy, while the typed `main` contract is compiler semantics.

## Current Limits

The cache is whole-program and workspace-local. It does not yet cache individual
parsed units, HIR/MIR bodies, native objects, or remote/shared artifacts.
Git/registry dependencies, lockfiles, `fetch`, native dependency composition,
arbitrary native build scripts, and a package registry are not implemented.
Project-mode plans and milestone contracts live in
[`docs/plans/build-system.md`](../plans/build-system.md).
The LSP must consume reusable resolved project facts rather than parse manifest
semantics independently or mutate project state while opening a document.
