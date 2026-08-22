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
                         -> optimization
                         -> LoweredProgramBuilder
                         -> backend
                         -> native toolchain when required
```

## Target Boundaries

- `gti_compiler` contains reusable frontend, IR, optimizer, backend contracts,
  and compiler-owned language queries. It does not depend on manifests, TOML,
  native processes, or project output policy.
- `gti_cpp_backend` contains the compiled C++ emitter and consumes the immutable
  compiler-owned `LoweredProgram` contract plus explicit C++ standard policy.
  It depends on `gti_compiler`; the LSP never invokes it.
- `gti_driver` in `include/gti/driver/` and `src/driver/` owns immutable
  compilation/build requests, resources, manifests, project plans, artifacts,
  native command construction, and process execution.
- `src/cli/` owns argument routing, diagnostics/output presentation, and exit
  status. It constructs driver requests rather than reimplementing compilation.
- `gti_lsp` is built whenever `GTI_BUILD_LSP` is enabled. It links the driver
  for canonical read-only manifest discovery/parsing of `[build].defines`, but
  does not construct compilation, backend, or native-process requests. Its
  private JSON protocol machinery reuses the mandatory LLVM support dependency,
  so LSP availability no longer depends on discovering or bundling a separate
  json-c library.

`gti_driver` exposes `compileWithBackend` as the generic whole-program backend
seam. The driver runs the same frontend and optimization gates, constructs and
verifies one `LoweredProgram`, and then invokes a caller-provided `Backend`
with only that value. If construction or artifact generation fails, the driver
returns `CompilationStatus::BackendFailure` with a structured entry-anchored
diagnostic and the retained frontend source/diagnostic snapshot.
The CLI renders that diagnostic and returns its ordinary compilation-failure
status; it does not depend on an exception escaping to a CLI-only handler.

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

`gti_cpp_backend` depends on `gti_compiler`, and `gti_driver` depends privately
on the backend for the default native path; reverse dependencies are forbidden.
All are installed static exact-version libraries without a stable cross-version
compiler ABI promise. Installed consumers use `find_package(GTI CONFIG)` and
the `GTI::compiler`, `GTI::cpp_backend`, `GTI::driver`, and `GTI::runtime`
targets rather than linking archive paths by hand. Those targets carry the
required LLVM support link dependencies. Self-contained release packages
include the pinned LLVM archives; a package produced against system LLVM
requires that exact LLVM CMake package on the consumer machine. A CMake project
consuming a system-LLVM GTI package must enable both C and C++ before
`find_package(GTI CONFIG)`, because some supported LLVM distributions run C
feature probes while loading their package configuration. The bundled release
package does not load a downstream LLVM package.

## Direct Mode

Direct mode accepts one entry `.gti` source and remains manifest-independent.
Its source graph produces one whole-program C++ artifact and one native compiler
invocation. Repeatable `-D NAME` options seed valueless GTI configuration flags
for the invocation. Accepted native arguments after `--` remain exact argv
values; native `-DNAME[=VALUE]` operands there are compiler arguments and do
not define GTI flags.
The shared native-argument policy rejects response files and recognized option
families that replace driver-owned output, executable build mode,
language/optimization, target, sysroot, or data layout. Raw Clang cc1/driver
mode escapes and an unjoined `-Xlinker` are rejected because their following
payload cannot be validated independently against that policy. Native argument
arrays remain a trusted escape hatch: the driver does not claim to classify
every vendor-specific ABI flag, and admitted arguments must not contradict the
resolved `TargetInfo`. The driver appends `-fno-fast-math` and
`-ffp-contract=off` after every forwarded argument, followed by the generated
artifact's `__gti_strict_ieee754=1` policy marker. On Apple hosts the final
link also appends `-Wl,-reproducible`, because Apple's linker otherwise
randomizes `LC_UUID` in its fast mode; equal inputs must produce equal output
bytes for serial/parallel build equivalence and cache verification. For the
same reason the staged link output keeps the final executable's filename
inside a unique staging directory rather than using a uniquely named staged
file, since the linker derives output-identity metadata from the output
basename. `TargetInfo` is resolved
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

An optional top-level `[build]` table accepts
`defines = ["DEBUG", "FEATURE"]`. Names must be ordinary GTI identifiers;
they are sorted and deduplicated. Repeatable project-command `-D NAME` values
are added to the manifest set. The effective set is immutable in every
`ProjectBuildPlan`, participates in its model identity, and is passed through
the shared frontend request. Metadata schema 9 reports that effective `defines`
array beside each target/profile output plan.

`gti build --all` builds every declared executable and test target of the
selected package for one profile. Each target is delegated to a child
`gti build <target>` invocation carrying the parent's resolved options, so
every target remains an independent whole-program build; `--jobs <count>`
bounds how many children run concurrently (defaulting to the host's hardware
concurrency, clamped to the target count). Children start in deterministic
target-name order and the parent replays each child's captured output in that
same order, so output and diagnostics are byte-stable regardless of
scheduling. A failing target does not stop the remaining targets; the command
reports each failure and returns the first failing target's status in name
order. Serial (`--jobs 1`) and parallel builds produce byte-identical
artifacts. `--all` rejects an explicit target selection and `--emit-mir`, and
`--jobs` requires `--all`.

Package, target, and profile names have portable artifact identity: reserved
device names and case-fold collisions are rejected before filesystem mutation.
Generated C++ and declared native objects live beneath the driver-owned hidden
`.gti-intermediate` directory, so a target named `intermediate` remains valid.
Managed builds require the declared project trust root itself to be a real
directory and reject symbolic links in every output-directory component and at
an output leaf; generated C++, native objects, executables, and cache payloads
publish from unique sibling staging paths rather than following a leaf link.

`new` and a source-creating `init` scaffold the implemented owned-argument
entry form, `int main(int argc, std::vector<std::string> argv)`, with the
required standard string and vector includes. `init` continues to preserve an
existing regular entry source rather than modernizing or replacing it.

`gti format init [directory]` is the bounded formatter-configuration scaffold.
It defaults to the current directory, requires an existing real directory,
refuses filesystem roots, symbolic links, and an existing `.gti-format`, and
writes the compiler-owned default configuration. It does not discover or
modify a manifest and does not format source files.

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
`ExecutableBuildRequest`. `CompilationRequest` carries the normalized initial
configuration flag set alongside target and source-graph inputs. A manifest
describes package/target policy; it does not replace `SourceGraph` or flatten
GTI visibility.

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

A workspace root is itself a package and lists canonical member roots. Loading
each member or path dependency must produce the same canonical package root as
the directory declared by the owning manifest; a `gti.toml` symbolic link cannot
redirect package ownership elsewhere. Commands run inside a member select that
member; commands at the root select
the root package; `--package <name>` selects any root/member package
explicitly for build/check/run/test/metadata. Package names are unique across
the graph under portable case-fold identity. The resolver loads
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

Dependency packages compose their package-scope native inputs into every
dependent build as isolated per-package groups. Structured contained inputs
compose — include directories, C and C++ sources, the C standard, library
directories, link files, libraries, frameworks, and platform fragments
selected against the resolved target. Argument vectors do not compose except
for validated `-D<name>[=<value>]` and `-U<name>` macro definitions from the
compile-argument fields; any other compiler argument and any linker or raw
argument on a dependency is rejected with `GTI-B1606` anchored at that
dependency's declaration, because an opaque argv from a transitive package is
an untrusted injection surface. Each group's include directories and macros
scope to that group's own declared sources only: they are never applied to the
root package's native sources, another package's sources, or the generated
whole-program translation unit. Group library directories and link operands
append after the root package's own, in deterministic
dependents-before-dependencies order (reverse postorder over the dependency
graph). Target-scope native tables on a dependency do not compose; they apply
only when that package is built directly. Plans that carry dependency native
groups bypass the whole-program cache with an explicit reason until dependency
discovery covers group sources.

### Locked Git Dependencies

`alias = { git = "<url>", rev = "<full 40-hex commit>" }` declares a pinned
git dependency; branch and tag selectors are rejected so a build plan never
depends on mutable remote state. `gti fetch` is the sole writer of the
workspace `gti.lock` (lock-version 1): it resolves the manifest closure,
acquires every pinned source, and records one sorted entry per git package
with its `name`, `version`, `source = "git+<url>"`, `rev`, a deterministic
`sha256:` checksum of the extracted tree, and its direct dependency names.
Regenerating an unchanged closure is byte-identical, and fetch re-derives
every checksum from the immutable object database, so a locally modified
checkout can never launder content into a fresh lock. `gti fetch --offline`
verifies and extracts from the local store only.

Acquisition never executes repository code. History is fetched into a bare
object database under `build/gti/deps/git/db/` (keyed by URL identity), and
the pinned tree is extracted blob-by-blob with `git ls-tree`/`git cat-file`
into `build/gti/deps/git/checkouts/<url-hash>/<rev>/` — no working-tree
checkout, hook, filter, or submodule machinery runs. Trees containing
symbolic links, gitlinks/submodules, unsafe or `.git` paths, or case-folded
path collisions are rejected. Git discovery follows the toolchain
convention: `GTI_GIT`, then `git`.

`gti build`, `check`, `run`, and `test` consume the lock: every declared git
dependency must be recorded at its exact URL and revision (a missing or
stale lock directs to `gti fetch`), the stored tree's checksum and the
locked package identity are verified before source loading, and mismatches
refuse the build with `GTI-B17xx` diagnostics. A plain build may materialize
a lock-covered missing checkout; `--offline` and `--locked` never run git,
so `gti fetch && gti build --locked` is the reproducible-pipeline shape.
`gti metadata` and `gti clean` never acquire dependencies, and metadata
schema 9 publishes each package's source as `path` or
`git+<url>#<rev>#<checksum>`. A fetched package's own path dependencies must
stay inside its checkout; its git dependencies join the same locked closure
transitively. The workspace model identity carries the git source identity,
so two revisions of one `name@version` never share cache state. Because the
store lives under `build/gti`, `gti clean` removes it; rebuilding then
requires either re-acquisition or a fresh `gti fetch`.

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

Direct mode also exposes `--emit-mir`. It runs the same complete frontend,
optimization compatibility check, and MIR verification, then selects
`MirBackend`, which serializes the verified optimized MIR snapshot that
executable backends consume in the deterministic versioned `mir-v*` text
format owned by `MirPrinter`. The mode writes one `.mir` artifact, defaults to
`<entry>.mir`, applies the loaded source overwrite guard, and performs no
native compilation or linking. It is mutually exclusive with `--emit-cpp`,
`--emit-native-header`, `--keep-cpp`, and trailing compiler arguments. The
artifact is an inspection surface for the backend-authority migration; the
serialization contract remains
[`docs/architecture/mir.md`](mir.md).

Project builds expose the same mode: `gti build --emit-mir` resolves the
manifest target, profile, and overrides exactly as an executable build, then
runs the identical `compileToMir` path over the resolved plan and writes
`<target>.mir` beside the profile's executable output under
`build/gti/<profile>/<triple>/`. It performs no native compilation, ignores
the cache, applies the same loaded-source overwrite guard, and is rejected
for `gti check`, `gti run`, and `gti test` and alongside `--keep-cpp`. The
direct and project modes therefore emit byte-identical serializations for
the same resolved configuration.

## Whole-Program Project Cache

`gti build`, `gti run`, and `gti test` use a workspace-local,
content-addressed whole-program cache by default. Declared native C and C++
sources, declared native include directories, and content-complete exact link
files (regular non-thin archives and relocatable objects) participate in cache
identity exactly and no longer force a bypass. The remaining conservative
bypasses are opaque native argument vectors, name-resolved libraries and
frameworks, native library search directories, link inputs that are not
content-complete (thin archives, linker scripts, shared libraries, and
unrecognized formats), dependency-injecting native environment search paths,
and any input whose exact identity cannot be established. Direct
`gti source.gti` mode and `gti check` remain uncached. `--no-cache` disables
both lookup and publication for one project build/run/test command; under
`--verbose`, the CLI reports the cache identity and whether the request hit,
missed, recovered a corrupt entry, or was conservatively bypassed with its
reason.

The cache does not parse includes or manifests independently. The driver calls
`loadCompilationInputs`, which uses the compiler's existing `SourceLoader` to
produce the exact `SourceGraph`, source text, source diagnostics, and logical
dependency edges for the request. A miss moves that same loaded state into
`Frontend::analyzeLoaded`; a hit occurs before parsing, semantic analysis, HIR,
MIR, backend generation, or final native linking.

The current key includes:

- the GTI release and project-manifest model identities, normalized GTI
  configuration flags, backend C++ standard, optimization, execution profile,
  target triple, and complete GTI data layout;
- the ordered `SourceGraph::preludeRoots()` logical identities, followed by
  SHA-256 content identities for every loaded GTI unit, ordered logical source
  edges, and standard-library import names;
- the native C++ compiler command, resolved executable content, and `--version`
  output (plus the same identity for the C compiler when C sources are
  declared);
- runtime headers/archive and the C++20 compatibility headers when selected;
- selected native standards;
- declared native include directories as full sorted tree identities, so
  adding, removing, or editing any file below a declared include root changes
  the key and header shadowing cannot restore a stale executable;
- for every declared C or C++ source, a native dependency discovery probe: the
  driver runs the exact object-compile argument vector with `-E -MD -MF`,
  consumes the compiler's own make-style dependency report for the exact
  discovered input set, hashes every discovered file's content and path
  identity, and hashes the preprocessed translation unit itself so include
  resolution is captured as the compiler saw it at lookup time;
- exact link files and file link operands classified content-complete by
  magic (regular archives and relocatable objects), with content identity; and
- bounded scalar native-toolchain environment values that affect policy without
  injecting a mutable search root.

The deterministic preprocessing policy is part of discovery: a declared source
or discovered dependency spelling `__DATE__`, `__TIME__`, or `__TIMESTAMP__`
bypasses the cache, because object content would depend on ambient build time
that no input content identity can record. The scan is a conservative byte
match; a spelling inside a comment costs a rebuild, never a stale hit. A
failed probe, unreadable discovered input, or unparseable dependency report
likewise bypasses with an `identity unavailable` reason.

Application GTI paths in a resolved package graph use package
`name@version` plus the package-relative unit path; standalone application
paths remain relative to the selected source root. Standard-library units use
logical import names, and toolchain resources use content identity. A pure-GTI
workspace can therefore move together with its `build/gti` subtree without
invalidating an otherwise identical entry. Explicit external/native paths
retain canonical path identity because native `__FILE__`, search order, and
external ownership make those paths semantically observable.

Prelude-root order is execution-semantic input, so it is hashed before the
otherwise canonicalized unit/dependency facts; reordering roots changes the
cache key even when their contents and edge set are identical. Current direct
and project requests intentionally resolve one canonical standard-library
prelude root and expose no free-form override. `SourceGraph` already preserves
multiple roots for frontend callers. A later manifest/profile feature may
supply more than one only through an immutable resolved policy, with direct,
project, and CLI coverage for that ordering contract.

Schema-v2 entries live beneath `build/gti/cache/v2/<sha256>/` and begin with the
`gti-build-cache-v2` metadata marker. Older `v1` entries and metadata are not
interpreted under the new ordered-root identity. Each v2 entry contains
generated C++, the executable, and strict metadata recording the digest and
size of both.
Executable permissions are recorded as well. Metadata is published last and
acts as the commit marker. A hit verifies every digest and the executable mode
before atomically copying the executable to the requested output; it
also restores generated C++ when `keep-cpp` is active. Missing entries rebuild
normally. Incomplete or corrupt entries are diagnosed, never executed, and
are replaced only after a successful rebuild. Cache-publication failure does
not discard a successfully published program. Deleting only `build/gti/cache`
does not modify sources or published target artifacts; `gti clean` deliberately
removes the entire validated `build/gti` subtree, including both.

The remaining bypasses are exact-identity boundaries, not native-source
boundaries. Opaque `c-compile-args`, `compile-args`, `link-args`, and
`raw-args` bypass because options such as `-include` and linker scripts can
introduce undeclared inputs outside the probed translation units and the final
generated-C++ link. Named libraries/frameworks, non-file ordered link
operands, and native library search directories bypass because the driver
does not model the linker's search algorithm and must not guess which file a
name would resolve to. A link file that is not a content-complete regular
archive or relocatable object (thin archives, linker scripts, shared
libraries, unrecognized formats) bypasses because its bytes do not determine
its full contribution to the link. Non-empty compiler/SDK/include/library
search environment variables likewise bypass: hashing `CPATH=/path`, for
example, does not identify a transitive or subsequently mutated header below
that path. Narrowing any of these requires establishing exact identity first;
widening what the key trusts is never an acceptable way to obtain a hit.

Cacheable builds still consume implicit C++ standard-library and platform SDK
headers/libraries selected by the native compiler. The bounded toolchain
identity treats those implicit resources as stable for an unchanged compiler
executable, compiler version output, selected target, and admitted toolchain
environment. This is an explicit non-hermetic boundary, not proof of every
system-file dependency; use `--no-cache` after mutating an SDK/toolchain in
place without changing that identity. Compiler-generated depfiles must cover
the generated GTI translation unit before this assumption can be removed.
Content-modeled runtime and vendor resources are also expected to remain stable
for the duration of one build. GTI compiles the loaded GTI source snapshot, but
it does not yet snapshot those native resources or reverify their identity
after linking; concurrent mutation is outside the current cache contract.

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
Git dependencies support pinned full revisions only; registry ranges, branch
or tag selectors, replacement/override authority, native dependency
composition, arbitrary native build scripts, and a package registry are not
implemented.
Git/registry dependencies, lockfiles, `fetch`, arbitrary native build
scripts, and a package registry are not implemented. Dependency native groups
bypass the whole-program cache until dependency discovery covers group
sources.
Project-mode plans and milestone contracts live in
[`docs/plans/build-system.md`](../plans/build-system.md).
The LSP must consume reusable resolved project facts rather than parse manifest
semantics independently or mutate project state while opening a document.
