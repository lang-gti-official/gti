# Build And Driver Architecture

Status: Current implementation. Future caching, dependencies, workspaces, and
package acquisition remain plans.

GTI has one language compilation pipeline and two user entry modes:

```text
gti source.gti ...                 direct mode
gti build|check|run|clean|metadata project mode
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
invocation. Native arguments after `--` are exact argv values. `TargetInfo` is
resolved before frontend entry and passed unchanged through semantics,
optimization, and backend generation.

## Project Mode

The implemented manifest path discovers `gti.toml`, parses schema version 1,
resolves executable targets/profiles and structured native inputs, and produces
an immutable `ProjectBuildPlan`. `build`, `check`, `run`, `clean`, `metadata`,
`new`, and `init` are implemented. `check` stops after the frontend; `run`
executes through exact arguments and inherited streams; `clean` removes only a
validated tool-owned subtree; `metadata` is read-only.

Project and direct modes construct the same `CompilationRequest` and
`ExecutableBuildRequest`. A manifest describes package/target policy; it does
not replace `SourceGraph` or flatten GTI visibility.

## Current Limits

Project tests, caching, external dependencies, lockfiles, workspaces, `fetch`,
and a package registry are not implemented. Project-mode plans and milestone
contracts live in [`docs/plans/build-system.md`](../plans/build-system.md).
The LSP must consume reusable resolved project facts rather than parse manifest
semantics independently or mutate project state while opening a document.
