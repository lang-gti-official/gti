# Build And Driver Architecture

Status: Current implementation. Local and pinned-Git dependencies,
deterministic workspaces, lockfiles, explicit dependency acquisition, and
consumer-approved native package build scripts are implemented. Finer-grained
GTI compilation remains a plan. Source-package registries, SemVer resolution,
explicit add/fetch and deterministic publication are implemented behind a
backend-independent protocol; Gitea Generic Packages is the official bootstrap
backend.

GTI has one language compilation pipeline and two user entry modes:

```text
gti source.gti ...                 direct mode
gti build|check|run|test|install|clean|metadata project mode
                 \                /
                  gti_driver requests
                         -> Frontend
                         -> optimization
                         -> LoweredProgramBuilder
                         -> backend
                         -> native toolchain when required
```

## Target Boundaries

Terminal status and diagnostic colour is CLI-only, not a manifest or build-cache
input. Use `gti build --color=always` to retain ANSI styling in captured output,
or `gti build --color=never` to disable it. Automatic terminal detection and
`NO_COLOR` handling are described in [Diagnostics](diagnostics.md#terminal-colour).
For `gti run`, build status remains on stderr and the program's stdout is left
unchanged. Parallel `build --all` preserves both coloured child diagnostics and
their aggregated counts without adding presentation policy to driver requests.
For `gti test`, build/run progress is cyan, passed labels/counts are green, and
failed labels/counts are red. Test-runner status stays on stderr; test-program
stdout and stderr remain inherited and are never restyled or filtered. The
same `auto`, `always`, `never`, and `NO_COLOR` policy applies to test output.
Parallel build target-failure labels are red under that policy as well.

An explicit `run/test --color` choice is passed only to the executed program as
a `GTI_COLOR` environment override, using the existing process invocation API.
The runtime applies it to panic/checked-failure reports, not arbitrary program
text. With no explicit choice, the child's environment and automatic stderr
detection remain authoritative. This includes `run --no-build`; no colour
choice enters a compilation request, manifest, executable or cache key.

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
  for canonical read-only manifest discovery and selected configuration, but
  does not construct compilation, backend, or native-process requests. Its
  private JSON protocol machinery reuses the mandatory LLVM support dependency,
  so LSP availability no longer depends on discovering or bundling a separate
  json-c library.

The root `CMakeLists.txt` owns product targets, build options, the installed
target graph, and package/install rules. When `BUILD_TESTING` is enabled it
delegates once to `tests/CMakeLists.txt`; that file owns test executables,
corpus tools, and every CTest registration. Test executables retain their
project-build-root locations so repository scripts and structural evidence do
not acquire a path-dependent compatibility branch. A static CMake ownership
gate rejects test registrations or test-source paths returning to the root.

The installed `gti_compiler` and `gti_cpp_backend` archives remain the public
compiled phase boundary. One private `gti_semantic_analysis` object target has
exactly one client: its objects enter `gti_compiler`, it exports no target, and
it cannot link back to the compiler archive it helps construct. This makes the
already-separated semantic implementation units one real build-DAG node. The
node precompiles only its private declaration/state contract; that optimization
was retained after measuring reduced aggregate semantic compile work, not as a
substitute for the source ownership split.

Other semantic, MIR, lowered-program, and C++ backend APIs enforce direction
through self-contained headers and repository boundary checks rather than
additional object targets without independent clients. Formatting-only C++
writers are additionally forbidden from including MIR or `LoweredProgram`
authority. The partitioned compiler tests similarly reuse one test-only
precompiled fixture after a measured reduction; the neutral emitter-fixture
trial is deliberately absent.

`gti_driver` exposes `compileWithBackend` as the generic whole-program backend
seam. The driver runs the same frontend and optimization gates, constructs and
verifies one `LoweredProgram`, and then invokes a caller-provided `Backend`
with only that value. If construction or artifact generation fails, the driver
returns `CompilationStatus::BackendFailure` with a structured entry-anchored
diagnostic and the retained frontend source/diagnostic snapshot.
The CLI renders that diagnostic and returns its ordinary compilation-failure
status; it does not depend on an exception escaping to a CLI-only handler.

Definition closure is an explicit immutable `CompilationRequest` policy,
independent of requiring `main`. Both `compileToCpp` entry paths derive a
`withDefinitionClosure()` request before frontend analysis, so a selected
ordinary GTI call without a definition receives `GTI-S2106` rather than reaching
backend emission. C++ fragments still need no entry point. Compiler-library
clients may select this policy for checks or custom backends; open fragment
analysis and MIR-only inspection remain available. Native declarations,
intrinsics and runtime bindings retain their own implementation contracts.

`include/gti/target.h` owns the selected target vocabulary and the immutable
`TargetDataLayout` value carried through these layers. The public value contains
only GTI domains, byte sizes, ABI/preferred alignments, pointer width, and
endianness; no LLVM or native compiler type crosses the interface. Private
`src/compiler/target.cpp` code uses `llvm::Triple` only to normalize and
classify a spelling, then maps it into the GTI representation. The parser uses
the same `os`/`vendor`/`arch` property vocabulary rather than maintaining a
second spelling table.

`gti_driver` owns the immutable `AnalysisConfiguration` that combines one
selected target, execution profile, runtime profile, and normalized
configuration flags before source loading. A raw `TargetInfo` advertises no
runtime capabilities. `RuntimeProfile::Hosted` derives `threads` only for a
supported target; the safe `None` profile leaves it false. Direct/project CLI
adapters and the LSP select the hosted profile because they describe the
installed hosted toolchain. Frontend, HIR, MIR, optimizer, and backend phases
only consume the resulting `TargetInfo`; none infers support from the target
OS, execution profile, public library spelling, or native arguments.

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

## Published Toolchain Channels

Gitea is the Git, tag, workflow, and release authority. Release archives
are downloaded by the plugin from `https://git.internal.howetotech.dev`.
The plugin uses a full HTTPS Git URL, not GitHub-style owner/repository shorthand.
Archive shapes
exist for Linux x64, Linux arm64, and macOS arm64, but an archive is published
only by a configured native runner job. The current workflow packages the
native platform of the runner claiming `ubuntu-latest`; adding another
advertised binary platform requires a statically labelled native package job.
Version `v0.299.0` is the final historical macOS x64 toolchain; source
compilation can still target or run on Intel macOS when its prerequisites are
supplied locally.

`scripts/install_neovim_local.py` is a macOS-only local installation workflow:
it builds with system LLVM, stages `gti_toolchain`, smoke-tests a checksummed
archive, and activates it through the existing Lua release installer without
network downloads. It writes a local-checkout Lazy spec with explicit tool
paths and downloads disabled, backing up an existing different spec only after
`--replace-config`. It also publishes `gti`/`gti_lsp` symlinks in `~/.local/bin`
(or `--bin-dir`) to the complete installed toolchain. Existing different
commands require `--replace-bin` and receive preserved backups; command-pair
publication rolls back on failure. `--no-terminal` retains editor-only setup.
Shell configuration is not modified; a PATH shadow is reported explicitly.
`--activate-only` validates the installed resources and both executable
versions against the installed VERSION, then activates commands/configuration
without consulting LLVM or rebuilding. It reports the installed version even
when the checkout is newer. This permits recovery after Homebrew removes build
prerequisites while the bundled runtime remains usable. It preserves a supplied
build directory's generator and
does not change source compilation, project manifests, LSP semantics, or release
publication authority. Discovery accepts LLVM 18–22, skips unsupported newer
Homebrew formulas, and considers retained supported versions without assuming
an Intel or Apple Silicon installation prefix. Existing build generators are
preserved while configuration/install rules are refreshed.

On macOS, the `gti_toolchain` CMake install step runs
`scripts/bundle_macos_runtime.py` for both system and bundled LLVM builds. It
resolves the non-Apple runtime dependency closure before modifying staged
copies, gives the configured libclang identity precedence over volatile
Homebrew `opt` aliases, includes dependency licenses, rewrites references
relative to each loader, and ad-hoc signs and verifies the resulting tools and
dylibs. An unresolved/ambiguous dependency or missing license fails installation
before activation; a different dylib ABI is never substituted for a missing
one. `share/gti/host-runtime.json` records the copied runtime paths. The installed
CLI/LSP no longer depend on Homebrew directories. This runtime portability does
not change the binary's architecture or minimum macOS version, bundle build
prerequisites, or make a system-LLVM CMake developer SDK self-contained.
Builds default to all detected logical CPUs (one if detection is unavailable),
at normal process priority. `--jobs N` overrides the job count; the dry-run and
normal output both report the selected parallelism.

A single toolchain-release workflow builds, tests, stages, and smoke-tests each
Gitea `main` commit once with bundled LLVM/Clang. That verified stage supplies
nightly and, when `VERSION` changes, stable archives. Stable is archived before
the nightly `BUILD_ID` stamp; the two channels retain separate artifact names.
The stable publication job owns the matching immutable tag and versioned
release. A manual dispatch may rebuild that exact tagged commit, but cannot
move a stable tag to another commit. New main commits do not cancel stable
publication.

The `nightly` release is one mutable channel whose tag and `BUILD_ID` advance
to each admitted Gitea `main` commit. Nightly publication is serialized and
checks current remote main before changing the tag; a superseded build skips
publication. Its publication step uploads the complete current asset set before a
Gitea API pass removes any asset not produced by that build. This exact
synchronization keeps retired platform archives from surviving a successful
rolling publication while preserving current archives if upload is
interrupted.

Stable versions describe user-visible toolchain behavior. Internal compiler or
runtime work, tests, and documentation alone retain `VERSION` and reach users
through nightly. `scripts/check_release_version.py` conservatively identifies
potentially shipped files, including deletions and moves. An unchanged version
requires each affected first-parent commit (or a merge's complete destination-
parent delta) to carry these Git trailers:

```text
Release-Impact: internal
Release-Reason: Explain why no user-visible behavior or public API changes.
```

This is an explicit author/reviewer impact assertion, not automated proof that
a change is invisible. User-visible fixes still require a patch increment;
new or changed surface requires a minor increment. Missing, duplicate,
malformed, or reason-free declarations fail the check, and a later internal
commit cannot excuse earlier unclassified changes. Versions may not decrease;
an internal declaration cannot authorize version churn. Documentation-only
changes need no trailer. A stable-version advance retains the ordinary release
workflow, with no competing manual tag.

## Live Build Progress

Executable direct compilation and project `build`, `run`, `test`, and `check`
report live, Cargo-style aligned phase verbs on stderr. A successful build can
look like this (counts and timings are illustrative):

```text
  Building  tea (dev, arm64-apple-macos)
   Loading  source graph
   Loading  24 source units
     Cache  checking build cache
   Parsing  24 source units
  Checking  tea
Optimizing  program (-O2)
Publishing  executable and build cache
  Finished  tea (2.81s)
```

HIR/MIR lowering, backend emission, native compilation and linking, and native
dependency preparation are backend detail. They are still observed, but they
get rows only under `--verbose`, which restores `Lowering`, `Emitting` and
`Native` between `Checking` and `Publishing`. While one of these hidden stages
runs, the live display shows `Building  <name>` with the build's total elapsed
time instead, so a long native link does not look like a hang.

`--progress=auto` is the default: a capable stderr terminal updates one active
row in place, refreshing elapsed time every 200 ms. Redirected streams and
`TERM=dumb` retain durable stage transitions without cursor controls. A silent
stage emits an elapsed-time heartbeat every ten seconds in that log mode.
`--progress=plain` forces durable output even on a terminal;
`--progress=never` disables this telemetry without suppressing diagnostics or
the existing artifact/test summaries. `--color` and `NO_COLOR` govern colours
independently; forcing colour on a redirected stream never enables cursor
rewriting. Live rows are bounded to the terminal width and source/path control
bytes are escaped.

Timings use a monotonic clock. Completed phase rows show a duration for slower
work (at least 100 ms in terminals, one second in logs); `Finished`/`Failed`
always includes total elapsed time. A heartbeat means the operation remains
pending, not that the compiler or native subprocess is proven to be advancing.
There are no estimated overall percentages or completion times.

Source-unit totals come from the canonical loaded graph, including the prelude
and dependencies. Parsing publishes actual per-unit events; `--verbose` exposes
those paths and a slow interactive parse shows the current path. Semantic
analysis, HIR/MIR lowering, optimization, and backend emission remain
whole-program stages: the CLI does not invent a per-file semantic or object
compilation counter. Under `--verbose`, declared native sources report their
exact source paths; the final representation compilation and link are honestly
one `Native` stage, and native dependency build scripts have a separate row.
Darwin symbol collection always has its own row. Neither the emission row nor
the native row names the selected backend or its compiler executable. Exact
commands remain available under `--verbose`.

Human-readable build output describes GTI work rather than exposing the current
representation language. Retention summaries say `Kept backend representation`.
The legacy `--emit-cpp`, `--keep-cpp`, `--cxx`, standard-selection flags, manifest
keys, artifact extensions and machine-readable metadata/provenance fields stay
unchanged for compatibility; this presentation policy does not promise a new
backend or change the representation itself.

Backend generation exceptions produce an entry-anchored `GTI-B0001` diagnostic
without the backend name or implementation exception text. `CompilationResult`
retains the complete developer details separately. Executable builds preserve
those details, or a failed native invocation's exact command, exit status,
driver diagnostic and captured tool output, in a fresh private temporary
`backend.log`. Direct emit-only CLI failures use the same log writer. The CLI
reports the saved log path; `--verbose` additionally exposes the raw evidence.
Logs are retained outside outputs/caches and POSIX directories/files are
owner-only. If saving fails, the original evidence is displayed instead of
being silently lost or claiming a saved file. They may contain sensitive native
arguments, so review them before including them in a bug report.

A retained artifact is identified explicitly as `backend 'c++' representation
saved at <path>`: its format is relevant when inspecting evidence, not as the
language's public compilation model. That line is emitted only when retention
actually occurred. An emitter failure before artifact construction reports a
log, not a nonexistent representation. A native compilation/link failure is
reported as a backend build failure, not automatically an internal compiler
bug: missing tools, native libraries and link inputs can also cause that stage
to fail. Handwritten native source failures keep actionable tool diagnostics
under language-neutral `native source` headings. GTI build-script compilation
uses the same preservation/reporting contract. Failure exit codes, previous
artifact preservation and successful output bytes remain unchanged.

An optional caller-owned `ProgressObserver` observes synchronous begin/update/end
events through the compiler frontend and reusable driver entry points.
`ProgressScope` times only observed work; ending a phase does not assert success.
Events borrow detail strings only during their callback, and neither caches,
results, artifacts, nor compiler workers retain an observer. The compiler
reports its own work; the driver reports cache/native/publication work; only
the CLI owns colours, terminal state, output, and the heartbeat thread. A null
observer leaves library and LSP clients silent. Progress policy never enters an
immutable semantic/build request, provenance sidecar, or cache key.

A cache hit reports `hit; restoring executable` and skips parsing, semantics,
lowering, emission and native compilation. `run --no-build` does not create a
build display. Direct emit-only modes remain silent by default for tooling;
explicit `--progress` enables observation for them. Project emit-MIR supports
the same display. stdout—including generated/metadata data and executed
program output—is unchanged. The heartbeat is joined and transient rows are
cleared before diagnostics or program execution.

Parallel `build --all` disables child progress so captured diagnostics and
artifact summaries still replay in deterministic target-name order. The parent
reports its actual target waits, with target ordinal/count, while other children
may also be running. `--progress=never` preserves byte-identical serial/parallel
presentation; elapsed progress is intentionally not byte-stable. The display
does not imply that waiting for one target means the other targets are idle.

## Direct Mode

Executable requests explicitly require a compiler-selected global `main`
definition in the entry source unit. `ExecutableBuildRequest` enables this
policy, as does project `check`; it is diagnosed by the shared frontend as
`GTI-S2094`, not delegated to the native linker. Default library compilation
requests and direct/project emit-only modes remain valid without an entry.
Externally supplied native entry points are not a GTI executable contract.

Direct mode accepts one entry `.gti` source and remains manifest-independent.
Its source graph produces one whole-program C++ artifact and one native compiler
invocation. Repeatable `-D NAME` options seed valueless GTI configuration flags
for the invocation. Repeatable `-Wno-<group>` options (or the equivalent
`--disable-warning <group>`) add validated warning groups to the immutable
`CompilationRequest`; for example, `-Wno-unused-mut` suppresses `GTI-S2096`.
Accepted native arguments after `--` remain exact argv values; native
`-DNAME[=VALUE]` operands there are compiler arguments and do not define GTI
flags.
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
from native arguments. The direct CLI resolves that selection together with
the installed hosted runtime profile, so a concurrent standalone compilation
receives the same supported-target `threads` capability as project mode and
editor analysis.

The direct CLI currently selects `TargetInfo::host()`; it does not expose a
general cross-compilation option. Compiler-library clients can request another
supported normalized target for analysis, but the data-layout contract does
not configure a native compiler, sysroot, runtime, or linker for that target.
Project mode does select a target: see
[Cross Compilation](#cross-compilation) below.

### Structured build provenance

Successful direct executable builds may opt into a versioned JSON sidecar with
`--build-report <path>`. The output path participates in the same collision
checks as executable and generated-source outputs, must not alias a loaded
source or the producer executable (including through a hard link), and is
published atomically only after the driver has completed the native build and
provenance capture. Emit-only builds reject the option. The public driver
request/result types expose the structured record, and the driver owns the
sidecar format in both directions: verification reads a recorded report back
through the same schema, so the writer and the reader cannot drift apart. The
CLI owns path validation, atomic publication, and human presentation.

Source provenance is captured from the exact `SourceGraph` and
`SourceManager` snapshot returned by `SourceLoader`, before compilation inputs
move into the frontend. It records every consumed unit's role, canonical and
available logical/package identities, byte count and SHA-256 over consumed
text, entry and prelude roots, dependency edges, and dependency-first
compilation order. The driver reuses its source-identity projection; no report
consumer resolves includes again. Its aggregate digest uses stable logical
unit order, role and entry/prelude flags, consumed content size/hash, ordered
prelude roots, and sorted logical dependency edges. Canonical paths and
ephemeral source-unit identifiers remain explicit sidecar facts but do not
enter that relocation-stable aggregate; an external-path logical identity
remains path-sensitive by design.

When capture is requested, the real generated-C++ compile/link invocation adds
a driver-owned depfile and bypasses whole-program cache reuse so the report
always describes a performed invocation. The existing native depfile parser
selects only dependencies under the resolved runtime and compatibility include
roots. Only the `gti/...` runtime and `nonstd/...` compatibility subtrees are
classified, even when an installed toolchain resolves both include roots to
the same directory. The sidecar records those used headers, the exact runtime
archive input, their content hashes, the native compiler executable identity,
and exact native argv. Its runtime aggregate hashes header kind/logical name
and content facts plus archive content facts, while retaining canonical paths
only in the individual records. It does not claim which archive members the
linker selected.

Every build refreshes the compiler source identity from the source repository,
rather than freezing it at configure time: a 40-hex commit is paired with
`clean` or `dirty`, while unavailable Git state produces no commit claim. The
sidecar also hashes and identifies the producer executable supplied by the CLI.
This executable binding is independent of packaged `BUILD_ID` metadata and is
the fact benchmark tooling uses to verify that a sidecar came from the selected
`--gti` binary.

The current sidecar schema is 2. Beyond the facts above it records the
published executable's own content identity, captured from the final output
path after publication, and a `project` section that is `null` for a direct
build. Schema 1 records carry neither and are refused rather than partially
interpreted.

## Source-level Debug Builds

Direct compilation and executable project commands (`build`, `run`, `test`)
accept `-g` / `--debug-info`. `--no-debug-info` disables it; the last explicit
selection wins. Profiles accept the Boolean `debug-info` field, defaulting to
false in both built-in profiles. A CLI override wins over the profile. Debug
info is independent of optimization: use `-O0` for predictable breakpoints.
Metadata schema 14 reports `debugInfo` in each profile and resolved output plan.
Analysis-only `check` and MIR/native-header emission do not enable debug
output. Direct `--emit-cpp -g` emits mappings for a separately invoked native
compiler, which must also receive `-g`.

```toml
[profiles.dev]
optimization = 0
debug-info = true
```

```sh
gti build -g -O0
lldb build/gti/dev/<host-triple>/<target-name>
```

At the LLDB prompt, use the real source file and an executable statement:

```text
breakpoint set --file main.gti --line 12
run
frame info
next
continue
```

For a standalone program, `gti main.gti -g -O0 -o app` followed by `lldb ./app`
uses the same path. No generated C++ file needs to remain beside the binary.
The C++ backend's [source debug contract](backend.md#opt-in-source-debug-locations)
describes the first-phase limits: statement lines work, but GTI variable names,
rich types, and expression evaluation are not yet provided.

The immutable compilation and native requests carry the debug policy; the
native command adds `-g` after forwarded arguments. On ELF toolchains symbols
remain in the executable. On Darwin, Clang's `.dSYM` bundle (or a `dsymutil`
fallback) is published beside the executable before link staging is cleaned
up. Keep the executable and matching bundle together. Rebuilds replace only a
GTI-marked bundle, refusing symlinks, unowned destinations, and loaded-source
collisions. Bundle publication is recoverable but not a transaction with the
separately atomic executable replacement. A non-debug build does not delete
an existing bundle; the debugger checks native UUIDs before using it.

Debug builds bypass the executable cache in this first slice: native metadata
retains absolute source/checkout paths, and cached symbol sidecars are not yet
modeled. Debug builds are not promised byte-reproducible native artifacts.
Schema-2 build reports add `debug_info` (absent in old reports means false);
the artifact fingerprint still covers the executable, not the `.dSYM` contents.
The benchmark harness accepts old and new non-debug reports, validates the
optional Boolean field, and rejects a source-debug policy it did not request.
`verify` reuses that recorded policy and compares it, rather than silently
rebuilding without source mappings. Debug artifacts can still report native
byte differences. Raw native `-- -g` alone is not the GTI source-debug option.
An explicit debug-info override on `verify` is also compared, not ignored.
Syntax, ownership, LSP analysis, and runtime behavior are unchanged.

## Project Mode

### Raw pointer audit policy

Direct compilation and project `build`, `check`, `run`, `test`, and `verify`
accept `--raw-pointer-audit=off|warn|strict` (also a separate value operand).
The default is `off`. A project profile may select the same policy:

```toml
[profiles.dev]
raw-pointer-audit = "warn"
```

An explicit command-line choice overrides the selected profile, including an
explicit `off`; duplicate or unknown choices are rejected. `build --all`
forwards the override to every child. `run --no-build` rejects it because no
analysis takes place. Direct mode remains manifest-independent.
The selection governs the requested GTI target. Approved `build.gti` host
tools retain their independent fixed compilation policy, including audit
`off`; changing the target's audit mode does not reinterpret a build script.

This policy selects compiler-owned optional raw-pointer auditing, not the
mandatory type, constness, unsafe-context, checked-borrow, or ownership rules.
`warn` publishes demonstrated violations, potential violations, and incomplete
analysis as distinct warning groups. `strict` promotes those findings to
errors at the frontend boundary, after the same analysis; warning suppression
cannot hide a strict error. Neither mode supplies optimization authority or
proves that foreign pointers are valid.

The immutable `CompilationRequest`, resolved profile/plan and read-only
`ProjectFacts` carry the same enum into the frontend. The mode joins project
model and executable cache identity; an `off` artifact cannot satisfy a
`strict` request. Unchanged-policy cache hits keep the existing no-warning-
replay contract; use `check` or `--no-cache` for fresh diagnostics. Metadata
schema 14 includes the additive `rawPointerAudit` field on profiles and plans.
Build-report schema 2 includes `raw_pointer_audit` (older reports imply `off`);
`verify` replays that policy unless explicitly overridden and compares it as a
separate provenance fact.

The implemented manifest path discovers `gti.toml`, parses schema version 1,
resolves executable and test targets, profiles, declared cross-compilation
toolchains, and structured native inputs (including declared C and C++
sources), and produces immutable `ProjectBuildPlan` values. `build`, `check`,
`run`, `test`, `clean`, `metadata`, `new`, and `init` are implemented. `check`
stops after the frontend; `run` executes one executable target through exact
arguments and inherited streams;
`test` deterministically plans every test target or one named test and
builds/runs each as an independent whole program in target-name order. A build
failure stops the command; a runtime failure is recorded while later tests
continue, and the command returns the first failing process status after the
summary. `build` and `check` can explicitly select either kind, while `run`
rejects a test target and points to `gti test`. `clean` removes only a validated
tool-owned subtree. When a package has one executable plus test targets, that
executable remains the default for `build`, `check`, and `run`. `metadata` is
read-only.

The verified hosted `ReturnEntry` adapter preserves returned integers in
0..255 and maps every other integer to status 1, after invocation cleanup.
Consequently, a test returning a nonzero failure count such as 256 cannot be
reported as passed because the host truncates its process status. The defined
runtime-failure status remains 70.

The driver also owns an internal, dormant `library` target model for the
engine-embedding campaign. Under the non-default `ManifestFeaturePolicy`, a
target must declare `linkage = "shared"` and at least one
`[[targets.<name>.exports]]` table mapping an exact qualified GTI function to
an explicit C symbol. `ProjectTarget`, `ProjectBuildPlan`, `ProjectFacts`, and
`CompilationRequest` retain that policy and all declaration spans; the build
plan's model identity canonicalizes export rows by public symbol. Its planned
output uses the target platform's `.dylib`, `.so`, or `.dll` spelling. The
ordinary CLI never enables this policy, so released manifests diagnose
`kind = "library"` as unavailable and no incomplete shared artifact can be
published. Internal tests may resolve the plan and run the ordinary frontend
`check` path. Activation and native publication remain owned by `B-LIB-02`.

The top-level `[build]`, selected `[profiles.<name>]` and selected
`[targets.<name>]` tables accept `defines = ["DEBUG", "FEATURE"]`. Names must
be ordinary GTI identifiers; all scopes use one parser that sorts and
deduplicates flags while retaining each selected declaration span. The
effective set is the union of selected-package and transitive-dependency
`[build]` flags, the consumer's selected profile and target flags, and repeatable
project-command `-D NAME` values. Unselected targets/profiles, dependency
target/profile flags and unrelated workspace members do not contribute.
`ProjectWorkspace` collects package flags, and `composeProjectConfigurationFlags`
combines scoped inputs for both immutable build plans and read-only
`ProjectFacts`, so check, build, metadata and editor analysis share that policy.
Flags remain program-wide, not private to their declaring package; source
`#undef` still applies in load order. These are additive valueless switches;
there is no manifest undefinition or automatic `DEBUG` flag for the dev profile.
The driver also publishes declared flag names from all consumer profiles and
targets. Source loading uses this separate set only for `GTI-S2075` typo
diagnostics: a dev-only switch remains known but false in a release build.
Dependency target/profile names do not enter this declaration set. Both active
and declared sets participate in frontend/build cache identities, and immutable
compilation requests preserve them through build, check and editor completion.
The effective set is immutable in every
`ProjectBuildPlan`, participates in its model identity, and is passed through
the shared frontend request. Metadata schema 14 reports that effective `defines`
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
existing regular entry source rather than modernizing or replacing it. An
explicit `gti init --git` additionally initializes a standalone Git repository
at the package root and writes the canonical GTI `.gitignore`, which ignores
the root `/build/` subtree. The option refuses existing `.git` or `.gitignore`
entries rather than modifying user-owned version-control state. Git follows
the dependency tool convention (`GTI_GIT`, then `git`); a failed invocation
rolls back the manifest, entry source, ignore file, source directory, and Git
metadata created by that scaffold operation while preserving pre-existing
files and directories. Repository-location Git environment overrides are
rejected before any mutation, including empty overrides; `-C` alone does not
isolate Git from inherited repository settings. The driver reserves the exact
missing `.git` directory before invocation and rolls it back only when that
reservation succeeds, never treating a preflight absence as deletion authority.

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

### Objective-C++ bridges

`cpp-sources` also accepts exact `.mm` paths for a selected macOS target.
These are foreign Objective-C++ translation units: the same immutable
`NativeCppCompileRequest` supplies the C++ standard, optimization, include
scope, target/SDK policy and arguments, while both object compilation and
dependency preprocessing explicitly select `-x objective-c++`. The generated
GTI translation unit and final link stay ordinary C++. Manifest resolution
rejects a selected `.mm` on another OS with `GTI-B1401` at its source path;
unmatched platform fragments remain inactive. Approved build-script outputs
accept the same extension and reject an unsupported target with `GTI-B2005`.
The low-level executable build API checks root and dependency-group sources
too, before frontend or filesystem work. The selected C++ driver must support
Objective-C++ and the requested Apple SDK; GTI does not substitute another
compiler or download SDKs.

No separate Objective-C runtime/ownership model exists in GTI. The bridge
exposes existing C-compatible `extern "C"` symbols and owns all Objective-C
objects, autorelease pools, exceptions and retained-resource cleanup itself.
Frameworks use ordinary structured `frameworks` link operands. ARC is not
enabled implicitly: an application may explicitly supply `-fobjc-arc` through
`compile-args`, which also reaches its ordinary C++/generated translation
units (Clang accepts it without changing their language). Such opaque flags
retain the existing conservative cache bypass and dependency-package argument
restrictions. This first slice does not add `.m`, Objective-C type import,
message syntax, ARC manifest policy, or non-Apple Objective-C runtimes.

Without opaque flags, `.mm` participates in the existing exact source,
preprocessed translation-unit, depfile/header and framework-link identity.
Language selection is deterministic from the declared source extension and
identical for discovery and compilation; a `.mm` cannot reuse a `.cpp` cache
identity. Native compile failures retain the same prior-object/executable
preservation guarantees. `objective_cpp_bridge`, `driver_pipeline`, and
`project_model` cover the runtime, argument, cache and target boundaries.

### Cross Compilation

`gti build`, `check`, `run`, and `test` accept `--target <triple>`. Omitting
it selects the build host, so an existing manifest and an existing command
keep working unchanged. An explicit triple is parsed by the compiler's own
`parseTargetTripleResult`, so project mode and `gti bindgen` share one
spelling vocabulary; a malformed or unsupported triple is a usage error
(`64`), and a triple GTI can name but cannot lay out is a
toolchain-configuration error (`78`). The resolved `TargetInfo` reaches
`ProjectBuildRequest` in place of `TargetInfo::host()` and flows unchanged
through the shared analysis configuration into semantics, optimization, and
backend generation. `gti build --all` forwards the selection to each child
build. `gti metadata` still reports the host target.

A target that is not the build host must be declared:

```toml
[toolchains.aarch64-unknown-linux-gnu]
cxx = "aarch64-linux-gnu-g++"          # required
cc = "aarch64-linux-gnu-gcc"           # needed for C unless --cc supplies it
sysroot = "/opt/sysroots/aarch64"      # optional
native-target = "aarch64-linux-gnu"    # optional; emitted as --target=
```

The table key is normalized through the same target vocabulary, so
`[toolchains.aarch64-unknown-linux-gnu]` and `[toolchains.arm64-unknown-linux]`
name one entry; declaring both is `GTI-B1901` rather than a silently ordered
choice. A key that is not a supported triple is `GTI-B1900`. Selection
compares only the OS, vendor, and architecture; the data layout always comes
from the resolved build target. Entries are sorted by declared spelling for
deterministic identity. The selected package owns the declaration, and a
workspace member falls back to the workspace root package.

`cxx` is required, because a declared toolchain that named no compiler would
fall back to host discovery and link host objects into a cross build.
`cxx`, `cc`, and `native-target` must be an executable or triple spelling
rather than an option (`GTI-B1902`). `sysroot` is either an absolute path,
taken as an explicit machine-specific declaration, or a package-contained
relative path resolved against the package root; either way it must name an
existing directory (`GTI-B1903`). `cc` is optional, and a build that actually
compiles declared or composed native C sources without either a declared `cc`
or an explicit `--cc` override is rejected with `GTI-B1905` rather than
reaching for host `cc`. Unknown fields are rejected by the shared manifest
field validator (`GTI-B1001`).

The rule is fail-closed: `gti build`, `run`, and `test` for a target that is
not the build host, with no matching `[toolchains.<triple>]` entry, fail with
`GTI-B1904` naming the missing table. Host tools are never substituted,
because the published executable would be built for the wrong target and no
later stage can detect that. `gti check` is deliberately exempt — it stops
after the frontend and compiles nothing native — so any supported target can
be analyzed without a declared toolchain. `gti run` and `gti test` execute
the produced program on this machine, so they reject a non-host `--target`
as a usage error and point at `gti build --target` instead; GTI models no
runner or emulator.

Configuration precedence for the native compilers is the manifest's general
rule: an explicit `--cxx`/`--cc` first, then the declared toolchain for the
selected target, then the existing `GTI_CXX`/`CXX` and `GTI_CC`/`CC`
discovery order. A declared toolchain outranks those environment variables
because they describe the host.

The resolved sysroot and `native-target` become one driver-owned
`NativeTargetPolicy` carried by the plan's native inputs. It is emitted as
`--target=<native-target>` and `--sysroot=<path>` ahead of every forwarded
native argument and every input, on the final whole-program link and on each
declared C and C++ object compile, including the cache's dependency-discovery
probes. `native-target` is separate from GTI's normalized triple on purpose:
a target-prefixed cross compiler already knows its target and must not be
told one, while a multi-target Clang must be. GTI cannot verify that the
spelling agrees with the selected target, so it is an explicit declaration in
the same sense as a native argument vector. These two options are exactly the
ones every user-supplied native argument vector rejects, so the driver is
their only source.

The selected triple and the resolved toolchain join the project model
identity, and the target policy joins the whole-program cache key alongside
the target OS, vendor, architecture, and complete data layout that the key
already carried. A host build and a cross build therefore cannot share a
cache entry, and changing the declared compiler, sysroot, or native triple
invalidates one. The sysroot contributes path identity rather than content:
it is a platform SDK tree, covered by the same explicitly non-hermetic
toolchain boundary as implicit system headers. Output already lives under
`build/gti/<profile>/<triple>/`, so artifacts for different targets sit
beside each other.

The installed GTI runtime archive and runtime headers are still discovered
once for the host toolchain. A cross link therefore needs a runtime built for
the selected target, supplied through the existing `GTI_RUNTIME_LIBRARY` and
`GTI_RUNTIME_INCLUDE` overrides; the manifest does not yet select them per
target.

### Recorded Provenance And Verification

`gti build [target] --build-report <path>` records the same structured sidecar
in project mode, through the same driver capture path as direct mode, and adds
the project facts a direct compilation has no concept of: the resolved package
name and version, target name and kind, profile name, selected target triple,
and the project model identity that produced the effective request. Capture
bypasses whole-program cache reuse exactly as it does in direct mode, so the
record always describes a native invocation that was actually performed. The
option builds one executable: `--emit-mir` and `--all` reject it, and `check`,
`run`, and `test` do not accept it.

`gti verify --build-report <path>` reproduces a recorded project build and
reports what no longer matches. It reads the record, re-resolves the package,
target, profile, and target triple the record names from the current
directory, rebuilds with capture, and compares the fresh record against the
recorded one. Selecting the recorded triple rather than the build host is what
makes a cross build verifiable: a record naming a non-host target resolves that
target's declared `[toolchains.<triple>]` table and fails closed when the table
is absent, instead of silently reproducing the build for the host and reporting
the mismatch as a divergence. The recorded report is only ever read;
verification cannot repair its own evidence.
Because the record names the selection, `verify` refuses a contradicting
target, `--package`, `--profile`, `--release`, or `--target`; host toolchain
and resolution
options (`--cxx`, `--cc`, `--offline`, `--locked`, `--no-cache`, `-v`) and the
build overrides that a recorded invocation may have used (`-O`, `--std`,
`--execution-profile`, `-D`) remain available. A direct-mode record is refused:
its sidecar does not describe the option surface needed to reissue that
command.

Comparison is by content identity, not by path. Source units and runtime
headers are matched on their logical identities, so a workspace that moved
compares equal when its content did not change, while an application include
outside the package keeps its path-sensitive external identity. Every recorded
identity is compared: the producer executable, the compiler source identity,
each source unit and the relocation-stable source aggregate, the project
selection and model identity, the native compiler executable, the native argv,
each runtime and compatibility header, the runtime archive, the runtime
aggregate, and the produced executable's bytes. Divergences are reported one
per fact, naming the subject, the compared field, and the recorded and rebuilt
values, and the command exits 70. Two native argv operands are normalized
before comparison because the driver generates them fresh for every
invocation: the staged link output after `-o` and the build-report dependency
file after `-MF`. Every other operand, absolute paths included, compares
exactly, so verifying a relocated checkout reports the paths that moved.

**What a successful verification proves.** The current tree, built by this
compiler on this host, consumed exactly the recorded GTI sources and toolchain
materials, issued the recorded native command, and produced a byte-identical
executable. Artifact bytes are compared deliberately: comparing inputs alone
would accept a rebuild that silently produced different output, and the
driver's determinism work (including `-Wl,-reproducible` on Apple hosts) is
what makes byte equality a testable claim rather than an aspiration.

**What it does not prove.** It is not a hermeticity proof. The
non-hermetic boundary named for the cache applies unchanged: implicit C++
standard-library and platform SDK headers are treated as stable for an
unchanged native compiler identity, and they are not hashed. It does not
attest that the recorded build ever happened on another machine, that the
recorded compiler binary is trustworthy, or that a byte-identical executable
behaves identically under a different kernel or loader. Declared native C/C++
sources, their headers, and their per-object compile commands are not yet
enumerated as separate report facts; their effects are checked through the
project model and the final executable bytes, so verification does not prove
identical native-input traversal when different native inputs happen to
produce the same artifact. A divergence in
artifact bytes with no divergence in any recorded input is exactly the signal
that something outside the recorded boundary moved. Verification also proves
nothing about a build it cannot reproduce: an unresolvable package, target, or
profile, or a build that no longer completes, is reported as a failure rather
than as a match.

### Bindgen Imports

A package may declare `[bindgen.<alias>]` tables:

```toml
[bindgen.counter]
header = "native/counter.h"       # required, package-contained
namespace = "counter_ffi"         # optional GTI namespace
include-dirs = ["native/include"] # optional, package-contained
defines = ["COUNTER_STEP=2"]      # -D for the import's preprocessing only
undefines = ["COUNTER_TRACE"]     # -U likewise

# Opt-in import policy (C-07); every entry joins the model identity.
allow = ["counter_*"]                 # import only matching declarations
deny = ["counter_internal_*"]         # never import matching declarations
rename = { counter_open = "open", counter__impl = "CounterImpl" }
array-returns = [{ function = "counter_all", count = "count" }]
```

`allow`/`deny` patterns are an exact name or one trailing `*`. An excluded
declaration still resolves uses exactly like a system-provenance one.
`rename` gives a declaration a usable GTI source name. A renamed function
retains its original C symbol through `[[link_name("...")]]`; types and
constants continue to replace their source spelling directly. Portable
assembler labels use the same function metadata. `array-returns` declares the
pointer-plus-count contract a C header cannot state, emitting
`[[c_array(count)]]` after the planner validates the language's rule. A
configured entry that matches nothing is reported.

During `gti build`, `check`, `run`, and `test`, the driver generates GTI
declarations from the header with the same pipeline as `gti bindgen`
(one Clang preprocessing pass resolving the full include graph with line
markers preserved, then the in-process importer applying provenance-based
import policy) into
`build/gti/bindgen/<package>/<alias>/bindings.gti`, published atomically
through a staged sibling. The declaring package's sources reach them as
`#include <alias/bindings>`: the generated directory is a synthetic package
source root joined to the declaring package by an ordinary alias edge, so
resolution, visibility, and cache identity follow the existing package
mechanism and the alias shares the dependency-alias namespace (a collision
is a manifest diagnostic). Every package in the resolved graph generates its
own imports; aliases do not leak to dependents.

Generation is deterministic and keyed: a `bindings.key` sidecar hashes the
preprocessed translation unit (which captures every transitive header
fact), the ordered options, the resolved target triple, and the GTI release
identity, so an unchanged import is reused without re-running the importer
while any header, option, target, or toolchain change regenerates. The
generated unit then participates in the whole-program cache as ordinary
loaded source, and the declared import joins the project model identity.
Import `defines` scope to the import's preprocessing only; native
compilation of declared C/C++ sources keeps its own `c-compile-args`
authority. Clang selection follows `GTI_CLANG`, then `clang` — bindgen
requires Clang specifically. Editor analysis of a package include whose
bindings have never been generated reports an unresolved include until the
first project build or check.

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

### Source Module Registration

Source packages can declare a public module map independently of file layout:

```toml
[modules]
main = "src/api.gti"
geometry = "src/geometry_api.gti"
```

Each key is one non-reserved GTI identifier other than `std`, which names the
standard library's modules (`import std.vector;`). Each value is a package-relative
path to an existing regular `.gti` interface. Canonical paths must remain inside
the package, including through symlinks. Duplicate physical files, portable
case-fold name collisions, invalid names/types and missing paths are rejected
by manifest loading. The frontend additionally requires the file to begin with
`export module` using the registered name. Paths need not be under `source-root`;
that field continues to control legacy angle includes, not public module names.

The owning package writes `import geometry;`. Consumers write
`import dependency.geometry;`, using their declared dependency alias. The
shorter `import dependency;` selects the dependency's `main` module. An
unqualified local-name/dependency-alias collision is an error. Only direct
dependency aliases resolve; re-exporting declarations never leaks aliases.
Direct compilation remains manifest-independent and can write a relative
quoted import instead.

`ProjectPackage` and immutable `PackageSourceRoot` values carry these maps;
`SourceLoader` owns language resolution. Registrations are part of project
model identity, while interface bytes and import/re-export edges participate in
source cache keys and build-report provenance. The LSP receives the same
read-only facts and does not fetch or build module artifacts. This adds no
binary modules, BMI files, stable ABI, or separate-compilation promise.

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

### Approved Package Build Scripts

A package that must configure or compile an upstream native library may declare
either a GTI build program or an exact external host command, plus its complete
contained input set. The GTI-authored form is the preferred package surface:

```toml
[package.build-script]
source = "build.gti"
inputs = ["vendor/upstream"]
```

The external-command form remains supported for existing packages and for
tools that are more naturally maintained in another language:

```toml
[package.build-script]
command = ["python3", "build.py"]
inputs = ["build.py", "vendor/upstream"]
```

The selected application owns the trust decision for dependency scripts. A
dependency cannot approve itself:

```toml
[build]
allow-build-scripts = ["gti-glfw@0.1.0"]
```

The identity is the dependency's manifest `name@version`; a pinned Git
dependency's exact source revision and checksum remain enforced independently
by `gti.lock`. Missing approval is `GTI-B2002` during project resolution, before
any process starts. A selected root package may run its own script without a
self-entry because selecting that package is already an explicit trust action.

Exactly one of `source` and `command` is required. A source path must name a
contained `.gti` file and is automatically part of the content cache inputs.
The driver compiles it as an ordinary hosted GTI executable for the build host,
using the installed standard library/runtime and host C++ toolchain; the
package's requested target still governs the native artifacts it publishes.
Compilation diagnostics retain `build.gti` source locations.

Build programs run dependency-first, with the package root as their working
directory, through exact argv execution rather than a shell. The driver exports
`GTI_BUILD_PACKAGE_DIR`, `GTI_BUILD_OUT_DIR`, `GTI_BUILD_OUTPUT`,
`GTI_BUILD_HOST`, `GTI_BUILD_TARGET`, `GTI_BUILD_PROFILE`, `GTI_BUILD_JOBS`,
`CC`, and `CXX`. `GTI_BUILD_OUTPUT` must be written as TOML with `version = 1`
and may declare `include-dirs`, `c-sources`, `cpp-sources`, `library-dirs`,
`link-files`, `libraries`, and `frameworks`. Paths are relative to
`GTI_BUILD_OUT_DIR`, must remain inside it, and must name existing files or
directories. C and C++ source extensions and native names are validated before
the outputs enter the resolved package-native group.

`<std/build>` is the source-defined API for GTI build programs. Their entry
point uses the normal owned argument form, checks `argc == 11`, and creates a
`std::build::context` with `std::build::context_from_args(argv)`. The context
exposes the output manifest, package/output directories, host/target facts,
profile, parallelism budget, and selected target C/C++ compilers.
`std::build::command` constructs and executes one exact argument vector without
shell parsing. `std::build::outputs` records include directories, C and C++
sources, library directories/files/names, and frameworks, then `publish`
writes the versioned output manifest consumed by the same validation path as
an external command.

```gti
#include <std/build>
#include <std/string>
#include <std/vector>

int main(int argc, std::vector<std::string> argv) {
  if (argc != 11) { return 64; }
  mut std::build::context context = std::build::context_from_args(argv);
  mut std::build::command configure = std::build::command("cmake");
  configure.arg("-S");
  configure.arg("vendor/upstream");
  configure.arg("-B");
  configure.arg(context.output_dir().view());
  int32_t status = configure.run();
  if (status != 0) { return status; }

  mut std::build::outputs outputs = std::build::outputs();
  outputs.link_file("lib/libupstream.a");
  return outputs.publish(context);
}
```

The output tree is managed below
`build/gti/build-scripts/<package>/<version>/<target>/<profile>/<project-target>`.
The cache identity includes the command or GTI source, host/target/profile,
selected C/C++ compilers, and content of every declared input. GTI-authored
programs additionally include the GTI compiler identity and host C++ compiler.
A matching state is reused only when its structured output still validates;
changed or missing inputs rerun the program. `--jobs N` is forwarded as the
program's parallelism budget and defaults to at most four jobs. `gti check`,
`fetch`, `metadata`, and project resolution never compile or execute build
programs or create their output directories; executable-producing `build`,
`run`, `test`, and `verify` do.

This implementation deliberately does not claim process sandboxing or network
isolation: both GTI-authored and external programs inherit the host environment
except for the documented overrides. Approval therefore means permission to
run arbitrary code from that reviewed, locked package revision. GTI syntax and
ownership checks make the program safer to author; they do not make executing a
third-party build program a sandbox. For correct cache reuse, a program must
derive its outputs only from its declared inputs and the documented context;
ambient environment values and undeclared host files are not part of the cache
identity. Declared input trees must contain only ordinary files and
directories; symbolic links and special files are rejected. Execution is
supported on macOS and Linux; Windows process overrides and package build
programs remain unsupported.

### Vendored Native Dependencies

The repository's optional `packages/` collection exercises this contract with
GLFW, GLAD, SQLite3, cimgui, zlib, libpng, Assimp, libgit2, and CLI11. Its explicit
repository-maintainer preparation command verifies pinned upstream archives and
generates reviewed public GTI units with `gti bindgen`. GLFW, Assimp, and
libgit2 then compile their vendored upstream sources through approved package
build scripts during an ordinary consumer build; no platform archive is checked
into the package. Other native C/C++ inputs compile through isolated package
groups. Libpng, Assimp, and libgit2 compose their zlib dependency through the
normal package graph. Published standalone Git packages carry their vendored
inputs, so their consumers do not run the repository preparation command.
Libgit2 disables HTTPS and SSH transports in its pinned build.
CLI11 remains header-only behind one exception-containing C++ adapter, so it
needs no prepared archive or system library. Its reviewed public declarations
spell the compiler-defined counted-text bridge directly because C bindgen
deliberately rejects the reserved `gti_c_string_view` record.
Generated public bindings are direct source units because including a private
bindgen unit does not re-export its declarations. See
[`packages/README.md`](../../packages/README.md) for commands and API limits.

The directly compilable GLAD, SQLite3, cimgui, zlib, libpng, and CLI11 packages
are also published as self-contained repositories under the `gti-lang` Gitea
organization. Consumers pin their complete commit IDs and use the ordinary
locked-Git dependency workflow below. Repository-local path copies remain as
integration fixtures. GLFW, Assimp, and libgit2 manifests select their checked-in
GTI build programs; consumers must explicitly approve their execution. Dependency
acquisition and metadata inspection do not execute those programs.

The composition rules above and the bindgen imports below combine into one
workflow worth naming, because it is what makes a GTI project portable: a
dependency may **vendor a C library** and a consumer needs no system
prerequisite at all.

```toml
# In the dependency package.
[package.native]
include-dirs = ["native"]
c-sources = ["native/checksum.c"]

[bindgen.checksum_c]
header = "native/checksum.h"
```

The dependency carries the C source and header in its own tree, generates its
own GTI declarations for them, and exposes the library through ordinary GTI
functions. The consumer declares only the package dependency — path or pinned
git — and `gti build` acquires it, generates the dependency's bindings inside
its checkout, compiles the vendored C into the dependent build as an isolated
native group, and links it. Nothing is installed on the machine, and nothing
is found by search.

Two existing rules do the load-bearing work and are worth restating here.
Bindgen aliases are per-package, so a consumer cannot reach
`#include <checksum_c/bindings>`; it sees only what the dependency chose to
expose in GTI, which keeps the raw C ABI an implementation detail of the
package that vendors it. And a dependency's native group scopes its include
directories and macros to its own declared sources, so a vendored library
cannot leak header search state into the consumer's sources or into the
generated whole-program translation unit.

`project_cli_workflow` covers the whole path end to end over a pinned git
dependency, including the negative case that the consumer cannot reach the
dependency's bindgen alias.

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
schema 10 publishes each package's source as `path` or
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

Direct compilation also exposes opt-in optimizer observability without
changing the selected backend:

```text
--dump-mir-before <path>
--dump-mir-after <path>
--optimization-remarks
--optimization-report <path>
```

The reusable `CompilationRequest` carries only capture intent for pre-MIR,
post-MIR, and the structured report. `CompilationResult` owns those optional
values; the CLI owns path validation, atomic publication, human formatting,
and JSON serialization. Output paths must be pairwise distinct and cannot
overwrite any loaded source. When capture is not requested, ordinary driver
callers retain no optimizer dump/report strings and compilation output is
unchanged.

### Source Package Registries

`gti add libc` queries the logical `official` registry, resolves a complete
metadata graph and acquires verified source archives before editing the project.
It selects the latest complete compatible direct version even when that package
was already transitively locked, then inserts a caret requirement such as
`libc = "^0.1.0"` in `gti.toml`, preserves
unrelated bytes/comments, and publishes the exact closure to the workspace's
`gti.lock`. `gti add libc@=0.1.0` requests an exact requirement. An already
matching declaration is a no-op; another source/requirement requires an explicit
manifest edit. Hyphens in registry names become underscores in default include
aliases. The table form allows separate identity and alias:

```toml
[dependencies]
libc = "^0.1.0"
cli = { package = "gti-cli", version = "^0.1", registry = "official" }
```

The generic SemVer implementation is shared by registry inputs, metadata and
resolution. Requirements support caret (including bare versions), tilde,
`=major.minor.patch`, and `*`; missing minor/patch parts are zero-filled in
ranges. Caret respects the first nonzero component, so `^0.2.1` excludes
`0.3.0` and `^0.0.3` excludes `0.0.4`. Prereleases require explicit opt-in at
the same major/minor/patch. Build metadata has no precedence. Version core
components are bounded to unsigned 64-bit values; unsupported requirement
operators fail rather than acquiring different semantics in a backend.

`gti fetch` never edits dependency declarations. It first attempts the complete
compatible locked solution, without version-list requests, then backtracks
through descending candidates only if the declarations make that solution
unusable. Sources are downloaded only after metadata resolution succeeds.
`gti fetch --locked` may populate missing exact sources but refuses any lock
change. `--offline` requires cached metadata/archives and may unpack them locally.
Normal registry builds,
checks, metadata and LSP facts require a compatible lock and already cached
complete extracted sources; they neither perform registry HTTP requests nor
create cache files/directories. Existing Git acquisition
policy is unchanged. Registry versions are unified per logical registry/package;
the existing unique canonical package-name and acyclic workspace rules remain
authoritative. Published registry packages currently require registry-only
dependencies; root projects may mix registry, path and pinned-Git sources.

The default logical mapping is owned by one configuration layer:
`official -> gitea-generic -> https://git.internal.howetotech.dev -> gti-lang`.
Override it in `$XDG_CONFIG_HOME/gti/registries.toml` (falling back to
`$HOME/.config/gti/registries.toml`) or a file selected by `GTI_REGISTRY_CONFIG`:

```toml
[registries.official]
backend = "gitea-generic"
url = "https://git.internal.howetotech.dev"
owner = "gti-lang"
token-env = "GTI_REGISTRY_TOKEN"
```

On macOS, authenticate once using `gti registry login` (default `official`) or
`gti registry login <name>`. Normal login prompts for your Gitea username,
hidden password, and an optional hidden six-digit authenticator code (leave it
empty when 2FA is disabled). GTI explicitly creates one named token through
Gitea's token API using Basic authentication, requests only `read:package`,
verifies package-read access to the configured owner, and saves only the token
in macOS Keychain. Passwords and authenticator codes are ephemeral inputs:
never stored in Keychain, config, manifests, caches or reports, and never
accepted in command-line arguments. Subsequent `add` and `fetch` commands use
the saved token automatically. `gti registry login --publish` requests
`write:package` instead; it implies read but does not grant any repository,
user-profile or administrative scope. Gitea's package scope covers the account's
package access, not one owner; the local credential lookup is owner-scoped.

Use `login --token` for hidden entry of an existing personal access token,
including accounts whose SSO/WebAuthn/server policy disallows password-based
API authentication. Token validation and replacement retain the same behavior.
OAuth/browser/device login and refresh are not implemented. Gitea's password
and TOTP authentication policy remains server-owned; the optional 2FA prompt
avoids inferring a challenge from version-specific error messages.

Token names contain a scope fingerprint and a unique 128-bit attempt nonce.
GTI never lists or deletes preexisting account tokens. A failed validation/save
or malformed/uncertain issuance triggers best-effort revocation of only the
attempt's generated token name; confirmed creation is not retried automatically.
If cleanup cannot be confirmed, `GTI-B1727` directs manual removal in the account's
access-token settings. An interruption or lost response can still leave that
named token remotely; a server that consumes TOTP codes may reject cleanup using
the already used code. A failed saved-credential replacement preserves the
previous local login. Repeated successful password logins create new server
tokens; GTI does not silently revoke previous logins on other devices.

`gti registry status [name]` reports the selected credential source without
showing the token or contacting the server. `gti registry logout [name]` removes
only that scoped Keychain record, idempotently; it does not revoke the server's
token. `login --token-stdin` retains its meaning: one preexisting token from a pipeline,
with an optional final LF/CRLF. Avoid commands that expose the token in shell
history or process arguments; use a secret manager's output instead.
Nonterminal login without that option fails rather than reading a secret with
echo enabled. Terminal cancellation restores echo.

An explicitly set `token-env` variable takes precedence over Keychain, for CI
and existing automation. An explicitly empty variable selects anonymous access
and disables Keychain lookup; unset it to use saved login. Login always validates
the newly entered token, ignoring existing stored/environment values, and only
replaces the saved record after validation succeeds. Logout leaves environment
overrides alone and reports that they remain set.

`RegistryCredentialStore` is a backend-neutral protected-storage interface;
`Registry::authentication()` and `validateCredential()` are optional transport
capabilities. Optional `issueCredential()`/`discardIssuedCredential()` let a
backend implement password-to-token exchange and rollback through GTI-owned
ephemeral input and opaque attempt identity, independent of metadata/archive
formats. The shared driver owns validation-before-save and rollback; the CLI
owns prompts and permission intent; Gitea alone owns Basic/OTP headers, token
paths/JSON and scope spellings. Credentials are typed
access tokens in a versioned private record, scoped by the logical registry,
normalized configured endpoint (trailing slashes removed) and owner/realm. The
Keychain service is `dev.gti.registry.credentials.v1`; its account is the full
SHA-256 of a length-framed scope tuple. Changing endpoint or owner deliberately
requires a new login. Package-format/backend changes alone do not alter scope.
No enumeration of unrelated Keychain items or plaintext fallback is performed.
Keychain access errors fail visibly, with an environment-variable alternative;
GTI requests noninteractive OS access. Unlocking/access authorization, stable
code signing and reauthorization after replacing a locally built executable
remain OS concerns. The private Objective-C++ adapter links system frameworks,
not an external secret-storage library. Other hosts currently support environment
tokens and anonymous access only; protected-store adapters and new credential
kinds can be added without changing the resolver or package formats.

Credentials belong only in the OS store or the named environment variable,
not the project, lock, archive, metadata, reports or package cache. Store lookup
is lazy and cached for one acquisition/publication provider lifetime; ordinary
offline builds, checks, metadata and LSP facts never request credentials, nor
does `publish --dry-run`. Anonymous public reads are supported when permitted
by the server; a server that requires sign-in also needs a package-read token
for `add`/`fetch`. The current official server requires authenticated discovery.
Use `gti registry login` on macOS, or set `GTI_REGISTRY_TOKEN` outside the project
for its default mapping.
Publication requires a package-write token. HTTPS uses the existing dynamically loaded
libcurl host transport with certificate verification; redirects are never
followed and response bodies are not included in failures. Plain HTTP is
admitted only on explicit localhost/127.0.0.1 ports for local development tests.
Unsupported registry names/backends/configuration fail closed.
Non-default registry definitions require explicit backend, URL and owner;
their default credential variable is `GTI_REGISTRY_<UPPERCASE_NAME>_TOKEN`
(hyphens become underscores), never the official registry's token. An explicit
`token-env` may override that user-owned credential scope.

Gitea adaptation alone owns its REST paths, response schema and pagination.
`Registry`/`RegistryProvider` expose version enumeration, metadata, archive bytes
and publication; the resolver has no Gitea URL construction. Each version has
`source.gtipkg` and a normalized `package.toml` (`registry-version = 1`, canonical
manifest name/version, public registry name, dependencies and archive SHA-256).
`package.toml` is uploaded last and acts as the completeness marker. A version
without it is not a GTI publication. Existing/partial versions are never resumed
or overwritten; publication preflights files, then the no-overwrite archive PUT
claims the version before final metadata is uploaded. Concurrent losers stop
before publishing metadata. Administrator deletion/replacement is outside this
immutability contract and cannot be made safe by client-side checksums alone.
See [Gitea's Generic Package API](https://docs.gitea.com/usage/packages/generic/).

`gti publish` inventories Git-tracked regular files without running repository
code. The archive contains current working-tree bytes, not machine-local build
outputs, with executable bits from the Git index. Symlinks, submodules, merge
conflicts, credential-bearing `.env` paths, managed build/toolchain directories,
lockfiles, traversal and portable path collisions are rejected. Its manifest
is validated after relocation before any upload. `--name libc` separates the
public registry name from a canonical manifest name such as `gti-libc`.
`--dry-run` reports file count, size and checksum without reading credentials,
uploading, executing native hooks, compiling or retaining a cache entry.

The GTI-owned `.gtipkg` v1 container is deterministic and deliberately bounded,
not tar/zip or a general archive extractor: ASCII magic `GTIPKG1\n`, little-endian
32-bit file count, then records with 32-bit path-byte count, 64-bit content-byte
count, one executable byte (`0`/`1`), UTF-8 path bytes restricted to portable ASCII
in this slice, and exact content bytes. Records are sorted by path. The reader
rejects trailing bytes and invalid/truncated records. Limits are 10,000 regular
files, 4,096 path bytes, 64 MiB per file, 256 MiB per archive, 256 resolved
packages, 1,024 metadata queries, 32 MiB of metadata and 8,192 solver attempts.
No compression, timestamps, ownership,
symlink/device entry or extraction command is interpreted. This narrow format
avoids a platform archive-tool dependency; another codec requires a versioned
GTI protocol change, not a Gitea-dependent parser.

Registry closures use lock-version 2; pure Git closures retain byte-compatible
v1 serialization and both are readable. Registry entries carry logical registry,
public and canonical package names, exact version, archive and metadata SHA-256,
and exact logical dependency edges. Physical Gitea URLs are not package identity.
Archive manifests must match metadata identity and dependency declarations.

The user cache is outside the source tree: `$GTI_PACKAGE_CACHE`, otherwise
`$XDG_CACHE_HOME/gti/packages`, `$HOME/.cache/gti/packages`, or
`$LOCALAPPDATA/gti/packages`. Managed keys hash logical registry/package, version
and archive digest. Metadata, archive bytes and every extracted file are checked
before source loading, including extra files and executable modes on POSIX.
Cache descendants cannot be symlink redirects. Immutable cache files publish
atomically with a no-replace hard link; cache filesystems must support hard links.
Complete source directories publish by rename, and concurrent fetches verify
the winning directory. Changed cached/server content is an integrity error,
left untouched for investigation—not silently replaced or relocked. Removing
only a known affected cache entry and fetching again does not authorize registry
mutation: the old lock checksum still applies.

Native inputs and consumer-approved build programs use the ordinary package
graph and existing approval policy; acquisition executes neither. Direct
`gti source.gti` behavior and frontend/language semantics are unchanged.
Manifest editing uses a source overlay to resolve proposed declarations without
writing them. Both output files are staged before atomic per-file renames; a
failed lock publication restores the original manifest where possible. A crash
between renames can leave a detectable stale lock, not a half-written file.
No cross-file crash-atomic transaction or hostile-local-writer sandbox is claimed.
Name ownership relies on Gitea's `gti-lang` publishing permissions; signatures,
yanking, search, mirrors, multiple versions of one canonical package and general
feature/target solving remain outside this bootstrap.

Driver diagnostics `GTI-B1710`–`GTI-B1723` distinguish invalid requirements,
missing packages, transport/authentication, malformed metadata, missing archives,
resolution conflicts, integrity, unsafe archives, duplicate publication,
configuration, resource limits, storage and conflicting manifest edits.
Existing `GTI-B1701`/`GTI-B1703` retain stale/missing-lock and offline meanings.

### Read-Only Project Facts

`gti/driver/project_facts.h` publishes the resolved project as a read-only
value for consumers that must agree with `gti build` and `gti check` about
what a project means without building, fetching, or mutating project state.
`resolveProjectFacts` answers for the project containing a directory;
`projectFactsFromWorkspace` describes a workspace a caller already resolved.

The published facts are the workspace root and whether a `[workspace]` table
declared it, the resolved package closure with each package's identity,
membership, manifest/package/source roots, selectability, git provenance, and
direct alias edges, the package source-root graph, the selected package's
declared profiles and targets with their own configuration flags, package-level
configuration flags, the profile
a command selects when none is named, and the workspace-graph model identity.
`analysisConfiguration` assembles a declared profile and optional named target
using the same flag composition as the build path. An unknown profile or named
target returns no configuration; omission of a target includes package/profile
flags only. `targetForSource` infers a unique matching entry root or the only
target in a single-target package; ambiguous roots and shared files in a
multi-target package have no inferred target. Profiles also publish their warning groups and
dependency-warning setting; `packageWarnings` resolves the same consumer-owned
package policy used by build plans. Native settings, output layout, and cache
policy are build concerns and are deliberately absent.

Resolution runs no native tool, no git process, and no build, and creates no
directories. It uses the same dependency policy as `gti metadata`: a stored
checkout is still checksum-verified and `gti.lock` remains the coverage
authority, so an unmaterialized or unlocked git dependency is reported as a
diagnostic rather than acquired.

`gti_lsp` consumes these facts before analysis and completion. It passes the
published analysis configuration and package source-root graph to the same
frontend APIs as project compilation, while canonical-path open-document
overlays remain authoritative over disk. A project-resolution failure is
published as the driver's structured diagnostic; a discovery miss retains
standalone-file behavior. Watching `gti.toml` and `gti.lock` invalidates all
open project snapshots without allowing the editor to fetch or build.

The package source-root graph is the fact that makes this a shared answer
rather than a convenience. It cannot be reconstructed from one manifest, so a
consumer that reads `gti.toml` itself resolves no package includes at all
while the build path resolves them, and the two silently disagree. A build
plan's model identity extends the workspace-graph identity these facts publish
with its configuration flags and bindgen imports.

`projectIncludeSearchRoots` derives editor include-discovery authority from
these same facts: the selected package root (excluding nested other packages)
and each direct dependency's source root with its declared alias. The compiler
then discovers and validates declarations; the driver does not interpret GTI
names. Project-resolution errors never authorize a partial dependency search.
The direct compilation driver retains standard-library include advice by
calling the same compiler suggestion service after its loaded-input frontend;
it does not need a second header catalog or a project scan on successful builds.

### Installing Programs

`gti install [target]` builds one executable target exactly as `gti build`
would and copies the result into `<root>/bin`, so it can be run by name.
`gti uninstall <name>...` removes programs it installed, and
`gti install --list` prints them. This first phase supports macOS and Linux;
on Windows both commands exit with a usage error.

```text
$ gti install
  Building  tea (release, arm64-apple-macos)
       ...
  Finished  tea (4.10s)
Installed tea [release, arm64-apple-macos] -> /Users/me/.local/bin/tea
$ gti uninstall tea
Removed tea -> /Users/me/.local/bin/tea
```

- **Source.** The project is resolved from the current directory, or from
  `--path <dir>`. Target selection, `--package`, dependency policy, and every
  build override match `gti build`. Test and library targets are refused, as
  is a `--target` triple other than the host, because the program is placed on
  this machine.
- **Profile.** `release` unless `--profile`/`--release` selects one.
- **Root.** `--root <dir>`, then `GTI_INSTALL_ROOT`, then `$HOME/.local`. The
  default matches the README's toolchain install prefix, so the programs land
  beside `gti` in a directory that is normally already on `PATH`. A relative
  `--root` resolves against the current directory; a relative
  `GTI_INSTALL_ROOT` and the filesystem root are refused. When `<root>/bin` is
  not on `PATH`, install prints a note on stderr.
- **Ownership record.** `<root>/share/gti/installs.toml` records each
  installed program's file name, package, version, target, profile, and
  source package root. It is the ownership boundary, because the default root
  is shared with other tools. Install replaces a program only when the record
  names the same package. A file the record does not name, or one recorded for
  another package, is refused (exit 64) before building, and left untouched,
  unless `--force` is given. Uninstall removes only recorded names. A record
  that cannot be read, or that names a program with a path separator or
  traversal, fails closed (exit 74); it is never treated as empty or obeyed.
- **Publication.** The executable is copied to a staged sibling, made
  `0755`, and renamed over the destination. A running copy of the previous
  program keeps its file, and no reader sees a partial one. The record is
  rewritten through the same stage-and-rename step, before the program is
  published: a record without its file is harmless, while an unrecorded file
  would block the next install. A failed publication tries to restore the
  previous record. An exclusive `flock` on `<root>/share/gti/installs.lock` serializes
  record updates between concurrent `gti` processes.
- **Removal.** Uninstall attempts every name it is given and returns the first
  failure's status: 64 for a name that is not recorded, 74 for a filesystem
  failure. A recorded program whose file is already gone only loses its
  record, with a warning. One whose file is no longer a regular file was
  changed by something else and is left alone, record included.

The driver owns root resolution, the record, and publication
(`gti/driver/install.h`); the CLI owns arguments, the build, and messages.
Installed programs are copies, so `gti clean` does not affect them. Building
still uses the project's own `build/gti` directory and cache.

## Whole-Program Project Cache

`gti build`, `gti run`, and `gti test` use a workspace-local,
content-addressed whole-program cache by default. Declared native C and C++
sources, declared native include directories, declared native library search
directories, content-complete exact link files (regular non-thin archives and
relocatable objects), and `-l` names that resolve to exactly one
content-complete file inside a declared search directory participate in cache
identity exactly and no longer force a bypass. On Apple hosts, named frameworks
also participate when a bounded probe through the selected compiler, target,
and SDK policy produces a linker map naming one exact framework text stub or
binary; that selected file joins by canonical path and content. A dependency
package's composed native group participates on the same terms as the selected
package's own native inputs. The remaining conservative bypasses are opaque
native argument vectors, frameworks for which an exact Apple linker trace is
unavailable, `-l` names that do not resolve inside the declared search
directories or resolve ambiguously within one, link inputs
that are not content-complete (thin archives, linker scripts, shared
libraries, and unrecognized formats), dependency-injecting native environment
search paths, and any input whose exact identity cannot be established. Direct
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
- for an internally enabled dormant library plan, its library/shared kind and
  canonical public-symbol-to-qualified-function export rows (ordinary
  executable/test identities remain byte-compatible);
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
- declared native library search directories as the same full sorted tree
  identities, so any file appearing, changing, or disappearing below a
  declared `-L` root changes the key and a dropped file cannot shadow an
  implicitly resolved library behind a stale entry;
- for every `-l<name>` operand, the concrete file it resolves to: the driver
  searches the declared library directories in the exact order the link
  command emits them, and joins the resolved file's path and content identity;
- for every composed dependency native group, in composed order: the
  contributing package identity, its include and library search directories as
  full tree identities, its validated exact `-D`/`-U` macro spellings, its
  selected C standard, its declared C/C++ sources with the same native
  dependency discovery probe applied under the group's own include scope and
  macros, and its ordered link operands under the same content-complete and
  name-resolution rules;
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

Three narrowings hold by an exact-identity argument, stated here because each
one moved an input out of the bypass list.

- A declared native library search directory is exact for the same reason a
  declared include directory is: its full sorted tree identity records every
  file below it, so any file that could answer a name — the project's own or
  one the toolchain resolves implicitly — is part of the key before the linker
  ever sees it.
- A `-l<name>` operand is exact when the driver searches the declared
  directories in the order the link command emits them and finds exactly one
  modeled candidate: a declared `-L` directory precedes every implicit
  toolchain directory on the command line, so the first declared directory
  holding a candidate is the directory the linker selects from, and that
  file's own content identity then joins the key. Resolution deliberately
  refuses more than it accepts. A directory holding two modeled candidates, or
  any `lib<name>.*` spelling outside the modeled set, is ambiguous rather than
  guessed; a name with no candidate in any declared directory would resolve
  through implicit system directories and bypasses; a `-l:file` exact-name
  selection or a path-bearing spelling means different things to different
  linkers and is never resolved; a search directory that cannot be enumerated
  bypasses rather than being reported as empty. A resolved file still passes
  the content-complete classification, so a name answering with a shared
  library or stub bypasses exactly as an explicit one does.
- A dependency native group is exact because composition already restricts it
  to inputs the driver can identify: package resolution rejects a dependency's
  linker and raw argument vectors outright and admits only exact `-D`/`-U`
  macro spellings, so a group carries declared sources, its own include and
  library directories, and ordered link operands — every one of which reaches
  the same discovery probe, tree identity, and link-input classification the
  selected package's inputs reach. The probe repeats the group's own include
  scope and macros, which is exactly what its object compile uses.

The remaining bypasses are exact-identity boundaries, not native-source
boundaries. Opaque `c-compile-args`, `compile-args`, `link-args`, and
`raw-args` bypass because options such as `-include` and linker scripts can
introduce undeclared inputs outside the probed translation units and the final
generated-C++ link. A named framework is cacheable on Apple only after GTI
links a temporary empty program with the selected compiler and target policy,
requests an Apple linker map, locates the unique requested
`Name.framework/Name.tbd` or binary row, and hashes that exact file and path.
The ordinary native-source dependency probes independently identify any
framework headers actually included by declared C/C++ sources. Probe failure,
an absent or ambiguous map row, or a non-Apple host bypasses rather than
guessing framework search behavior. A link file that is not a content-complete
regular archive or relocatable object (thin
archives, linker scripts, shared libraries, unrecognized formats) bypasses
because its bytes do not determine its full contribution to the link.
Non-empty compiler/SDK/include/library search environment variables likewise
bypass: hashing `CPATH=/path`, for example, does not identify a transitive or
subsequently mutated header below that path. Narrowing any of these requires
establishing exact identity first; widening what the key trusts is never an
acceptable way to obtain a hit.

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
resolved through the shared analysis configuration into the plan's
`TargetInfo`, and the command-line option is an explicit project override.
The same table may set `disabled-warnings = ["unused-mut", ...]`; validated
command-line warning suppressions are unioned with that profile policy and
carried through the resolved build plan. Warning policy does not change emitted
program semantics. Warning groups and resolved dependency-warning policy join
the cache key; changing either forces frontend analysis under the new policy.
Cache hits retain the existing contract: they restore artifacts without running
semantic analysis or replaying its advisory warnings. Use `gti check` or
`--no-cache` to request fresh diagnostics without relying on artifact reuse.
Metadata schema 14 publishes the profile fields for every profile plus the
selected workspace and sorted package/dependency graph. The execution profile
selects static language policy
only; the independently selected runtime profile derives the target/runtime
`threads` capability. Public managed-thread and atomic operations require both
the concurrent execution profile and that capability.

### Dependency warnings

The selected package may silence GTI compiler warnings from other packages for
one profile:

```toml
[profiles.dev]
dependency-warnings = false
```

The Boolean defaults to `true` in every profile. Set it independently in
`[profiles.release]` or a named profile when that command should also be quiet.
Errors are never suppressed. Warnings in the selected package, including its
ordinary source includes, remain visible; use `disabled-warnings` or
`-Wno-<group>` when intentionally suppressing a warning group in your own code.

The driver resolves an immutable `PackageWarningPolicy` from the consumer's
profile and actual workspace package identities before source loading. It
filters only a `Warning` whose primary span belongs to an application
`SourceUnit` owned by a registered non-selected package. This covers local,
locked Git, transitive and workspace-member dependencies without guessing from
`build/gti/deps` path spelling. A selected package's own file under a directory
with that name is still its own code. Dependency-related notes do not hide a
consumer-primary diagnostic. Unowned sources, prelude/standard-library units,
synthetic bindgen packages and native-tool output are outside this policy.

Only the selected package's profile is authoritative; a dependency cannot
silence consumer diagnostics through its own manifest. Workspace `--package`
selection changes the selected identity, so a member's own warnings stay
visible when building that member. The LSP receives the same policy from
read-only `ProjectFacts` and applies the same primary-source classifier when
publishing frontend diagnostics. A dependency opened as its own project is its
selected package, not a globally muted directory. Manifest changes invalidate
editor snapshots. Direct compilation stays manifest-independent and keeps
dependency warnings enabled by default. Metadata schema 14 reports
`dependencyWarnings` on both profiles and resolved output plans.

Arguments after `gti run --` are passed as exact program arguments and become
owned values when the target uses
`main(int, std::vector<std::string>)`. In contrast, arguments after `--` in
direct compilation mode still belong to the native C++ compiler; run the
produced executable separately to supply its program arguments. This routing
rule is driver policy, while the typed `main` contract is compiler semantics.

`gti run --no-build` resolves the selected package, executable target, profile,
and host output path but deliberately skips dependency acquisition, bindgen,
build scripts, source loading, compilation, cache lookup, and publication. It
executes only an already present regular executable inside the symlink-free
managed `build/gti` subtree and fails with an I/O status when that artifact is
missing or unsafe. Compiler, optimization, execution-profile, configuration,
cache, keep-C++, and job overrides are rejected because they cannot describe or
validate an existing artifact. Warning-policy overrides are rejected for the
same reason. The command makes no freshness claim; it is an explicit
development escape hatch for running possibly stale output.

## Current Limits

The cache is whole-program and workspace-local. It does not yet cache individual
parsed units, HIR/MIR bodies, native objects, or remote/shared artifacts.
Git dependencies support pinned full revisions only; mutable branch/tag
selectors and replacement/override authority are not implemented. Registry
bootstrap limits are recorded above; package signing, yanking, search and
multiple canonical versions remain deferred. Native dependency build programs
require explicit consumer approval, not arbitrary implicit execution. Cross compilation
selects a target, a native compiler pair, a sysroot, and a native triple, but
the GTI runtime archive and runtime headers are still discovered once for the
host toolchain, so a cross link needs a target-built runtime supplied through
`GTI_RUNTIME_LIBRARY`/`GTI_RUNTIME_INCLUDE`. `gti metadata` still reports the
host target, and `gti run`/`gti test` refuse a non-host target rather than
modelling a runner or emulator. `gti install` supports macOS and Linux only,
installs from a local project (not a registry or Git source), and installs one
target per invocation.
Project-mode plans and milestone contracts live in
[`docs/plans/build-system.md`](../plans/build-system.md).
The LSP consumes the driver-published project facts for its default host-target
analysis. Explicit editor selection of another workspace package, target,
profile, or cross target is not implemented.
