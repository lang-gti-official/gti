# Build And Driver Architecture

Status: Current implementation. Future caching, dependencies, workspaces, and
package acquisition remain plans.

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

Verbose native command lines are presentation text. They retain the leading
`+ ` trace marker, and the remainder is encoded as a reproducible POSIX-shell
command; native execution itself always passes exact argument vectors without
a shell. The display contract does not claim compatibility with Windows
`cmd.exe` or PowerShell syntax.

Each manifest `[profiles.<name>]` table may set
`execution-profile = "single-threaded"|"concurrent"`; the selected value is
resolved into the plan's `TargetInfo`, and the command-line option is an
explicit project override. Metadata schema 6 publishes the declared value for
every profile. This build profile field selects static language policy only;
the later target/runtime `threads` capability remains independent.

Arguments after `gti run --` are passed as exact program arguments and become
owned values when the target uses
`main(int, std::vector<std::string>)`. In contrast, arguments after `--` in
direct compilation mode still belong to the native C++ compiler; run the
produced executable separately to supply its program arguments. This routing
rule is driver policy, while the typed `main` contract is compiler semantics.

## Current Limits

Caching, external dependencies, lockfiles, workspaces, `fetch`, arbitrary
native build scripts, and a package registry are not implemented.
Project-mode plans and milestone contracts live in
[`docs/plans/build-system.md`](../plans/build-system.md).
The LSP must consume reusable resolved project facts rather than parse manifest
semantics independently or mutate project state while opening a document.
