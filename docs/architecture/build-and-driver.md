# Build And Driver Architecture

Status: Current implementation. Local and pinned-Git dependencies,
deterministic workspaces, lockfiles, explicit dependency acquisition, and
consumer-approved native package build scripts are implemented. Finer-grained
GTI compilation remains a plan. Source-package registries, SemVer resolution,
explicit add/fetch and deterministic publication are implemented behind a
backend-independent protocol; Gitea Generic Packages is the official bootstrap
backend.

The built-in `release` project's default configuration defines `NDEBUG`,
disabling source-defined `std::debug_assert` evaluation; `dev` leaves it
inactive. An explicit profile `defines` array replaces that profile default,
so `defines = []` deliberately keeps debug assertions enabled even in a
release profile. Refining other release fields preserves its default defines.
Direct compilation does not infer this switch from `-O`: use `-D NDEBUG` to
disable debug assertions independently of optimization. The resolved flags
remain shared by compiler, cache identity, metadata and LSP analysis. Ordinary
`std::assert` is never disabled by this flag.

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
- `gti_llvm_backend` is an independent `LoweredProgram` consumer. Its public
  interface is LLVM-free; its private implementation owns LLVM IR/native
  code-generation libraries and host initialization. Its implemented native profile and fail-closed admission are documented in
  [Backend](backend.md#llvm-coverage-and-admission).
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

The installed `gti_compiler`, `gti_cpp_backend`, and `gti_llvm_backend`
archives form the public compiled backend boundary. One private
`gti_semantic_analysis` object target has
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
with only that value. `CompilationRequest::withBackend` and
`compileToSelectedBackend` add immutable built-in executable selection while
preserving `compileToCpp` for explicit library clients. If construction or
artifact generation fails, the driver
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

Both executable backend libraries depend on `gti_compiler`, and `gti_driver`
depends privately on them for selected native paths; reverse dependencies are
forbidden. `gti_llvm_backend` privately links the exact LLVM
`Core`/`nativecodegen` closure while `gti_compiler` retains its support-only
surface. The configured component manifest and link-surface test prevent that
closure from leaking onto other targets.
All are installed static exact-version libraries without a stable cross-version
compiler ABI promise. Installed consumers use `find_package(GTI CONFIG)` and
the `GTI::compiler`, `GTI::cpp_backend`, `GTI::llvm_backend`, `GTI::driver`,
and `GTI::runtime` targets rather than linking archive paths by hand. Those
targets carry their required LLVM link dependencies. Self-contained release packages
include the pinned LLVM archives; a package produced against system LLVM
requires that exact LLVM CMake package on the consumer machine. A CMake project
consuming a system-LLVM GTI package must enable both C and C++ before
`find_package(GTI CONFIG)`, because some supported LLVM distributions run C
feature probes while loading their package configuration. The bundled release
package does not load a downstream LLVM package.

## Published Toolchain Channels

Source lives on GitHub (`lang-gti-official/gti`), and toolchain releases
publish to its GitHub Releases through `.github/workflows/nightly.yml`, described
under [GitHub nightly publication](#github-nightly-publication), and the
[release train](#github-release-train) (`train.yml`, `beta.yml`, `backport.yml`,
`rollback.yml`). The plugin downloads them from
`https://github.com/lang-gti-official/gti/releases/download/<tag>/<asset>`.
A Lazy checkout runs its own installer, so a Git tag cut before this switch
still downloads from the frozen self-hosted Gitea release (`gti-lang/gti`),
whose archives no longer advance.

The `nightly` channel reads the rolling `nightly` release. The default `stable`
channel downloads `v<VERSION>` for the checkout's `VERSION`, which the release
train publishes. A checkout whose `VERSION` has no stable release (main, or a
beta tag) fails with an error that names that tag and the nightly channel
(`vim.g.gti_channel = "nightly"` or `GTI_CHANNEL=nightly`). Before a stable
download, and before reusing an installed stable toolchain, the installer reads
the [release status](#release-status-and-rollback) and refuses a yanked version
with its reason and the newest stable version that is not yanked; an
unreadable status stops a new installation and keeps an installed one. A
locally built archive (`scripts/install_neovim_local.py`) is installed offline
without that check. The installer never falls back to Gitea, to another
version, or to a nightly build on its own, and has no beta channel: a beta's
archives are downloaded from its prerelease.
The plugin uses a full HTTPS Git URL, not owner/repository shorthand. Archive
shapes exist for Linux x64, Linux arm64, and macOS arm64, but an archive is
published only after native validation on its platform. Full integration produces the GitHub nightly
packages for `linux-x64` and `darwin-arm64`. `linux-arm64` is not packaged: the
runtime's raw atomic lanes (`runtime/src/atomic_lane.h`) have no reviewed
linux-aarch64 compiler profile, so no admitted compiler exists for it. TODO:
add the `ubuntu-24.04-arm` matrix entry once a runtime-safety review admits a
linux-aarch64 profile. Do not infer a platform's availability from a tag or
serve an older asset for a platform the current build did not produce.
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
publication authority. Discovery accepts LLVM 18 or newer and considers
retained versions without assuming
an Intel or Apple Silicon installation prefix. Existing build generators are
preserved while configuration/install rules are refreshed.

The default (also explicitly `--preview`) selects the canonical GitHub `preview` commit directly and builds
CLI, LSP and parser together in a persistent detached worktree under the
primary checkout's sibling `gti-preview-installs/<full-commit>`. It never
switches or resets the calling checkout or incorporates its uncommitted work.
An existing source worktree must remain clean, detached and at that exact
commit; modifications or new commits cause refusal. The complete installation
and Lazy config point at that selected source. The staged archive includes
`share/gti/PREVIEW_SOURCE`, and output reports commit and version. This selects
the user's active ordinary tool commands using the existing backup/activation
rules; it does not create a second set of executable names.

`--local` builds this checkout instead. Preview with `--activate-only` is rejected
because source selection requires a new build; use `--local --activate-only`
to activate this checkout. The default dry run (also `--preview --dry-run`) only
resolves and reports the remote source (no fetch or local writes); unlike the
`--local --dry-run`, it cannot inspect the selected checkout's build/prerequisites
until that checkout exists. A missing preview branch fails without installing
anything. The preview branch is populated only by the separate
[combined preview workflow](agent-workflow.md#early-combined-preview), and can
contain PRs still awaiting full validation/review. Preview is not a release
channel and never publishes stable/nightly packages. Installation is opt-in.

There is no maximum-major admission check: system packages must provide the
required APIs, RTTI, approved support targets, and an exactly matching Clang
package. LLVM 23.1.2 has been built and tested; admission of a later release is
not a substitute for compatibility validation. The bundled release pin is
unchanged. The local installer resolves cached Homebrew aliases on every run
and supplies the matching concrete LLVM/Clang CMake paths. It permits an
explicit dependency change in the same build directory while retaining the
source-checkout and optimized-build checks. A raw developer executable can
stop loading after Homebrew replaces a dylib ABI; reconfigure and rebuild it,
rather than substituting a differently versioned dylib. Installed toolchains
remain insulated from that upgrade by the runtime bundle described below.

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

### Managed toolchains and preview distribution

[`gtiup`](https://github.com/lang-gti-official/gtiup) is a standalone C++
toolchain manager maintained, tested and released in its own repository. Its
manager binaries and installer need neither Python nor a compiler. Its CI never
builds GTI or LLVM; compiler archive publication stays in this repository. The bootstrap
places `gtiup`, `gti` and `gti_lsp` proxies in `$GTIUP_HOME/bin` (default
`~/.gtiup/bin`). Installation is explicit; neither compiler invocation nor
Neovim downloads a missing selection. The manager resolves command overrides,
`GTIUP_TOOLCHAIN`, the nearest ancestor `gti-toolchain` file, then the persisted
user default. Explicit version tags remain separate from moving channel
selections. The selected compiler and language server resolve to real paths in
one immutable, checksum-named directory with their matching packaged resources.

Downloads use fixed canonical GitHub release URLs. The manager checks archive
checksums, source identity (`BUILD_ID` for rolling channels), packaged VERSION,
and preview inclusion metadata before atomically updating settings under a
process lock. Extraction rejects path traversal, special files, hard links,
symlink-parent writes and symlink targets containing parent traversal. Install
and update fail closed on absent or revoked releases; they never substitute an
older or different channel. Existing installations stay usable offline; release
revocations are checked during download, not every compiler invocation.
GitHub publication attestations are available separately; the manager's trust
boundary is HTTPS downloads plus published checksums and embedded identity.
Updates retain old immutable directories; explicit `gc` removes unreferenced
managed directories after the user has stopped processes using them.

`.github/workflows/preview.yml` builds downloadable packages after a canonical
`preview` branch push, or an explicit dispatch from main. It selects the exact
current preview source with `.gti-preview.json`, uses the reviewed production
configuration on Linux x64 and macOS arm64, runs combined `preview_admission`
(included feature checks and automated examples) and installed-toolchain checks
on each platform, then packages that validated stage. The separate `preview`
prerelease uses `gti-preview-<platform>.tar.gz` names, checksums, embedded and
release `BUILD_ID`, `PREVIEW_SOURCE`, inclusion metadata and run/attempt-bound
provenance. Publication verifies both platforms, attests archives, rechecks
current preview, and uses an exact lease on its own rolling tag. BUILD_ID is
uploaded last so a racing installer rejects inconsistent assets.

This does not qualify preview artifacts for nightly or advance main. Preview
can include features whose original PR review and complete integration are
pending. Nightly still requires successful full CI on its selected main source;
beta/stable promotion is unchanged. Preview source admission and package
publication are asynchronous: a source branch can advance before downloadable
packages finish, and an older packaging run then refuses stale publication.
A workflow retry must rerun both package jobs in the same attempt. Initial
commissioning requires this orchestration on main and a published preview source.
Neovim managed mode delegates compiler/LSP/parser selection to gtiup; it installs
only editor configuration and never an independent language-server version.

### GitHub nightly publication

`.github/workflows/nightly.yml` runs daily at 02:00 UTC and on manual dispatch.
It publishes only the rolling `nightly` channel. Stable `v<VERSION>` releases
come from the [release train](#github-release-train), which cuts the commit
this workflow published.

1. **Build and validate in integration.** Full `ci.yml` integration builds
   production archives on `ubuntu-24.04` (`linux-x64`) and `macos-26`
   (`darwin-arm64`). Each uses the reviewed compiler from
   `scripts/configure_ci_toolchain.sh`, configures
   `-DCMAKE_BUILD_TYPE=Release -DGTI_RELEASE_BUILD=ON` with pinned bundled
   LLVM/Clang, builds, stages `gti_toolchain` into a fresh directory, and runs
   the installed CLI/LSP/project, process-resource, managed-thread,
   atomic-runtime and installed-library consumer checks against that stage.
   macOS installation generates and verifies `share/gti/host-runtime.json`;
   `cmake/PackageRelease.cmake` refuses a macOS stage without it. The stage
   becomes `gti-nightly-<platform>.tar.gz` with a `.sha256` sidecar and
   `share/gti/BUILD_ID` naming the exact source commit. Full integration also
   runs the complete suite in cost-balanced shards on the same bundled-release
   configuration, reusing the packaging job's object cache, and retains the
   separate sanitizer gates and system-LLVM fast checks, including the Linux
   system-LLVM installed consumers. The coverage gate proves each platform's inventory
   ran exactly once. Packaging is a required part of the full `CI gate`.
2. **Select validated artifacts.** Nightly runs only from main's own definition
   in `lang-gti-official/gti`. It takes main head, or a dispatched exact 40-hex
   commit on main. `scripts/resolve_release_source.py select-nightly` requires
   successful full CI for that exact commit and its retained production
   artifacts. Normally this is the `merge_group` run of
   `.github/workflows/ci.yml` on `gh-readonly-queue/main/...`; a full main-only
   CI dispatch also qualifies for commissioning or artifact recovery. PR runs,
   automation-only passes and merge exceptions alone are not release evidence.
   Scheduled selection skips unchanged main. There is no fallback that stamps
   an older compiler with a new commit when the selected source has no artifacts.
3. **Download and verify.** Nightly downloads the artifacts from the selected
   successful CI run and attempt. Their manifest binds source commit, platform,
   bundled-release configuration, run/attempt and archive checksums. Missing,
   expired, mixed or mismatched artifacts stop publication. Nightly neither
   rebuilds the compiler nor reruns the full suite. A new full CI dispatch on
   main can recover unavailable artifacts while preserving the same validation
   obligations; a fixture-only rehearsal cannot produce publishable evidence.
4. **Publish.** One serialized job (`gti-nightly-publication`, never
   cancelled in progress) holds the workflow's only write token. It verifies
   every archive's checksum and stamped `BUILD_ID`, writes the `BUILD_ID`
   asset, and admits the build (`resolve_release_source.py admit-nightly`) only
   if the commit is still main head and is newer than or equal to the commit
   the `nightly` tag names. Equality permits a failed publish job or an explicit
   dispatch to finish interrupted uploads after the tag moved; scheduled
   selection still skips unchanged main. An older commit is skipped, and a
   nightly tag outside main's history is an error. It then attests build provenance, moves
   the `nightly` tag with `git push --force-with-lease=refs/tags/nightly:<value
   admission read>` (an empty lease requires the tag not to exist), creates or
   edits the `nightly` prerelease, uploads archives and checksums before
   `BUILD_ID`, and only after every current asset is present removes any asset
   this build did not produce.

The installer contract is unchanged: it reads the `nightly` release's
`BUILD_ID` asset, downloads `gti-nightly-<platform>.tar.gz` and its `.sha256`,
and requires the extracted `share/gti/BUILD_ID` to match. A download that races
publication fails that identity or checksum check and installs nothing. Only
the moving `nightly` tag exists; dated per-build tags are not created, because
no client reads them and each would retain full archives.

`.gitea/workflows/release.yml` is the frozen predecessor. It built Linux x64
on each Gitea `main` push, archived stable before the nightly `BUILD_ID` stamp
when `VERSION` changed, and pruned nightly assets through
`scripts/sync_release_assets.py`. Gitea `main` no longer advances, so it does
not run; it stays only as the record of that contract. Manual macOS uploads to
Gitea releases are retired: GitHub-hosted `macos-26` runners package Apple
Silicon automatically.

Release archives explicitly use GNU tar format so macOS extended attributes do
not become AppleDouble companion files after extraction. This preserves the
installed runtime-header tree's content identity for relocated native cache
entries; the cache still hashes every real file in that tree.

### GitHub release train

`scripts/release_train.py` holds every train decision (unit tests:
`tests/release_train_test.py`; workflow shapes: `tests/ci_release_test.py`).
The workflows only pass validated values between those checks and `git push`
or `gh`. Every train ref is created by a plain push that fails rather than
overwrites; no tag or branch is ever forced, moved or deleted.

1. **Cut** (`train.yml`, job `cut`). The schedule fires daily at 04:00 UTC and
   the train admits every other UTC day (epoch-day parity), a 48-hour cadence
   without month-end drift; a dispatch always runs. The source is the commit
   the `nightly` tag names. It must be on main, carry the same successful full CI
   evidence nightly selection requires (`release_artifacts.py
   release_evidence`), have the complete uploaded nightly asset set with GitHub's
   digest of the final `BUILD_ID` matching that commit, and have been on main
   at least 24 hours, measured from its
   committer time, which the queue sets when it lands the commit. A dispatch's
   `min_age_hours` may lower the age for the owner's bootstrap; nothing lowers
   the full CI evidence requirement. The train skips with a notice when a cut already landed
   after that nightly, when no fragment is pending, or when only fixes are:
   a train cut opens the next minor line, and patch releases come only from
   backports. It runs `cut_release.py` on a worktree of the nightly commit,
   commits the result with its `Release-Cut:` trailer, pushes
   `release-cut/<version>` and opens the pull request with the gti-rollup App
   token (a `GITHUB_TOKEN` PR would get no PR checks). It labels the PR
   `rollup=never` and `release-cut`, waits up to 150 minutes for its `CI gate`
   and queues its exact head with `enqueuePullRequest`. An open cut PR is
   resumed instead of cutting again; one the queue rejected stops the train
   until it is closed (delete its branch with it). The PR body declares
   `release_version_policy` and `cut_release_policy` as its `Validation:` T1.
   The App has no Workflows permission and the cut touches only `VERSION`,
   `CHANGELOG.md` and `changes/`.
2. **Beta** (`beta.yml`). It runs on `push` to `main` and `beta/**`. A merge
   queue merge into main is a push made by GitHub, not with `GITHUB_TOKEN`, so
   it triggers workflows, as does merging a backport PR; only the train's own
   `GITHUB_TOKEN` tag and branch pushes trigger nothing. A daily 05:30 UTC
   schedule and dispatch are fallbacks, and selection is idempotent. On main,
   selection takes the newest first-parent commit whose `VERSION` differs from
   its first parent's and requires exactly one `Release-Cut: <that version>`
   commit among the commits it delivered. A `VERSION` change without one, such
   as imported history, is not a train cut. The beta is built from that cut
   commit, not from the queue's merge onto a newer main: the cut changes only
   release metadata on top of its full-CI-verified parent, the soaked nightly,
   so the beta is exactly the code nightly users ran. The cut's parent needs
   full CI artifact evidence (queue or main recovery dispatch); the landing
   merge separately needs ordinary merge-queue evidence. Retained source
   artifacts are required for this admission too; expiry stops the train.
   `beta/<major>.<minor>` must not
   already hold another commit. On a `beta/X.Y` push, selection verifies the
   backports since the line's newest beta, refuses pending user-visible
   fragments, publishes nothing when `VERSION` is already stable, and numbers
   the next beta. Beta retains its own bundled production build, installed-
   toolchain checks, complete release-suite shards and merge-shards gate on the
   beta commit. The nightly artifact-reuse path does not relabel a nightly
   package as a versioned beta: the release cut changes the embedded version
   and archive identity. The
   serialized publish job (the only one that writes) re-verifies the asset set,
   admits the beta (the tag is absent or already at that commit, and a newer
   push did not supersede it), attests build provenance, pushes
   `v<VERSION>-beta.<K>`, replaces any leftover draft, creates a draft
   prerelease that is never latest, uploads, publishes it, and only then
   creates `beta/<major>.<minor>`, so a refused branch push cannot block the
   published beta.
3. **Backport** (`backport.yml`). A pull request into `beta/*` labelled
   `backport` runs `Backport provenance` with main's own copy of the checker,
   so the PR cannot weaken it. Every commit must be a `git cherry-pick -x` of a
   commit already on main, with the same `git patch-id`, or a
   `Backport-Adjusted: <reason>` trailer explaining a resolved conflict. Merges
   and `VERSION` changes are refused, except one final release cut that
   advances only the patch version and changes only release metadata.
   Pending user-visible fragments require that cut, so a beta's `VERSION` and
   CHANGELOG always describe its content: a user-visible backport becomes
   `vX.Y.(Z+1)-beta.1`, an internal one `vX.Y.Z-beta.(K+1)`, or nothing once
   `X.Y.Z` is stable. The complete suite then runs on the release configuration
   in cost-balanced shards, and `Backport gate` requires both. The main
   ruleset and merge queue do not cover `beta/*`; the proposed `beta` ruleset
   requires a PR merged by rebase (picks keep their `-x` lines), linear
   history, an up-to-date branch, `CI gate` and `Backport gate`.
4. **Promotion** (`train.yml`, job `promote`). Candidates are versions with a
   beta newer than every stable tag; a version a higher patch beta on its line
   supersedes is skipped. From the highest down, the version's newest beta
   must be a published, not withdrawn prerelease that has soaked 48 hours
   since publication, and no open issue labelled `release-blocker` may name
   the version in its title or body (`0.494.0`, `v0.494.0` or any
   `0.494.0-beta.K`; `10.494.0` or `0.494.0.1` do not count). A dispatch's
   `soak_hours` may lower the soak; the stable notes record it. The job waits
   for the protected `stable` environment's approval and then re-admits the
   beta. That recheck rejects the selected beta if a newer stable release or
   a newer patch beta on the same line arrived while approval was pending.
   It downloads the beta's assets, verifies their checksums, channel
   neutrality and the beta workflow's provenance attestation, pushes
   `v<VERSION>` at the beta's commit, uploads the same files to a draft,
   reads the draft's assets back through the API and compares every archive
   byte and member before publishing the release as latest, and records the
   version in the release status. Promotion never rebuilds or repackages.

**Same bytes.** A stable-channel archive from `cmake/PackageRelease.cmake` is
named `gti-v<VERSION>-<platform>.tar.gz`, identifies itself only by
`share/gti/VERSION`, and carries no `BUILD_ID`; the binaries embed only the
CMake project `VERSION` (`GTI_VERSION`). Nothing in it names a channel, and a
beta is built from the release cut whose `VERSION` is the stable version, so
the beta publishes the stable archive itself and promotion republishes the
same files: nothing is re-stamped. `release_train.py same-bytes` proves it per
file, and packaging refuses a stage that nightly archive production already stamped
with `BUILD_ID`. Only the release around the files differs: tag, title, notes
and prerelease flag.

#### Release status and rollback

The `release-status` branch holds one document, `release-status.json`
(schema 1): `stable`, the published stable versions in ascending order;
`yanked`, entries of `version`, `reason` and `at`; and `withdrawn`, beta tags
with a reason and time. Promotion and `rollback.yml` append to it with
fast-forward commits, never forced, so its history is the audit log. It is a
branch, not a release asset, because GitHub release immutability locks a
published release's assets. The installer reads it from
`https://raw.githubusercontent.com/lang-gti-official/gti/release-status/release-status.json`.

`rollback.yml` is dispatch-only and runs behind the `stable` environment.
`withdraw-beta` records the beta as withdrawn, so promotion never selects it,
and edits its prerelease's title and notes. The prerelease stays as evidence:
deleting an immutable release would also retire its tag name for good.
`yank-stable` records the yank, banners the release notes and marks the
newest non-yanked stable release latest. The yanked tag stays, so Lazy's
`version = "*"` can still resolve it until a newer stable exists; the
installer then refuses it, names the newest non-yanked stable version, and
never installs that one instead on its own.

Lazy's `version = "*"` resolves the highest plain `vX.Y.Z` tag. Its semver
range requires a candidate's prerelease to equal the range's own, which `*`
has none of, so `v0.494.0-beta.1` is never selected and `nightly` does not
parse as a version (lazy.nvim `lua/lazy/manage/semver.lua`, `Range:matches`).
Stable tags therefore stay plain `v0.N.x`, above the imported `v0.332.0`.
Pinning `version = "0.494.0-beta.1"` selects that beta's commit, whose stable
channel looks for the not yet published `v0.494.0`.

Stable versions describe user-visible toolchain behavior. Pull requests never
edit `VERSION` or `CHANGELOG.md`; a shipped change adds a
[changelog fragment](../../changes/README.md) `changes/<name>.<kind>.md`
(`breaking`, `feature`, `fix` or `internal`), and a release cut
(`scripts/cut_release.py`) assembles the pending fragments into one CHANGELOG
section and the next version. Only a commit carrying `Release-Cut: <version>`,
matching the `VERSION` it writes, may change `VERSION`; concurrent pull requests
therefore never contend for a version number. Internal compiler or runtime
work, tests, and documentation alone retain `VERSION` and reach users through
nightly. `scripts/check_release_version.py` conservatively identifies
potentially shipped files, including deletions and moves. A range with shipped
changes passes when it adds a valid fragment or a release cut; otherwise each
affected commit delivered by the checked range must carry these Git trailers. A merge commit is judged only by what differs from the
automatic merge of its parents (`git merge-tree`), which is its conflict
resolution: a hosted pull-request merge or an upstream reconciliation merge
introduces nothing, even when both sides touched the same files, carries no
trailers, and relies on the branch commits it delivers, each judged on its
own. CI judges a pull request from its merge base with `main`, not from the
tip of `main`.

```text
Release-Impact: internal
Release-Reason: Explain why no user-visible behavior or public API changes.
```

This is an explicit author/reviewer impact assertion, not automated proof that
a change is invisible. User-visible fixes still need a `fix` fragment and new
or changed surface a `feature` or `breaking` one; the cut turns them into a
patch or minor increment. Missing, duplicate,
malformed, or reason-free declarations fail the check, and a later internal
commit cannot excuse earlier unclassified changes. Versions may not decrease;
an internal declaration cannot authorize version churn. Documentation-only
changes need no trailer. A `VERSION` advance on main publishes a beta through
the release train, and stable follows only by promotion; never push a `v*` tag
by hand.

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
time instead, so a long native link does not look like a hang. Project `check`
uses `Checking` for its initial banner and overall elapsed-time rows instead.
It still reports `Lowering` under `--verbose`; checking does not generate or
link an executable, and progress labels do not alter analysis or caching.

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

Native dependency `Built`/`Reused` summaries are durable messages, including
under `--progress=never`. They and captured verbose build-script output go
through the renderer's synchronized message writer: it clears any unfinished
live row, writes complete lines, and leaves subsequent telemetry enabled.
Captured output missing its final newline receives one before the next summary.
This coordination does not add terminal controls to redirected or plain output.

Timings use a monotonic clock. Completed phase rows show a duration for slower
work (at least 100 ms in terminals, one second in logs); `Finished`/`Failed`
always includes total elapsed time. A heartbeat means the operation remains
pending, not that the compiler or native subprocess is proven to be advancing.
There are no estimated overall percentages or completion times.

Speculative module probes and proposed import-fix checks do not inherit the
user compilation's progress observer. Import suggestions and their validated
diagnostic fixes remain available, but internal recovery analyses do not emit
additional top-level `Loading`, `Parsing` or `Checking` sequences, including
under `--verbose`.

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
saved at <path>`, or `backend 'llvm' representation` for a native object: its
format is relevant when inspecting evidence, not as the language's public
compilation model. That line is emitted only when retention
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

### Compile-time telemetry

`gti --time-trace <path>` writes a Chrome Trace Format profile of one
compilation through LLVM's time profiler. Every `PhaseTimeScope` in the
compiler contributes a nested, named region; tracing costs nothing when it is
off. The regions are the driver phases (`gti-source-load`, `gti-parse`,
`gti-semantics`, `gti-hir-lowering`, `gti-failure-metadata`,
`gti-mir-lowering`, `gti-optimize`, `gti-lowered-program`, `gti-backend`),
each optimizer pass (`mir-pass` with the pass name as detail, plus
`hir-compatibility-folding`, `mir-verify-input` and
`mir-verify-output-coherence`), the lowered-program builder's verification
and collection steps (`lowered-verify-*`, `lowered-collect`), the backend
(`backend-verify-lowered-program`, `backend-plan`, `backend-plan-program`,
`backend-write`), HIR's concrete instance analyses (`hir-instance-analysis`
with the declaration as detail, `hir-verify-plans`), staged constant
evaluation (`constexpr-staged-*`, `constexpr-for`), the safety analysis
(`mir-safety-analysis`) and the optimization coherence verifier's replays
(`coherence-*`). Regions nest, so a viewer's self time, not the sum of
totals, attributes cost; a region that runs many times (instance analyses,
staged evaluations) is best read as a count and a total.

## Direct Mode

Executable requests explicitly require a compiler-selected global `main`
definition in the entry source unit. `ExecutableBuildRequest` enables this
policy, as does project `check`; it is diagnosed by the shared frontend as
`GTI-S2094`, not delegated to the native linker. Default library compilation
requests and direct/project emit-only modes remain valid without an entry.
Externally supplied native entry points are not a GTI executable contract.

Direct mode accepts one entry `.gti` source and remains manifest-independent.
Its source graph produces one whole-program backend artifact: a native object
from LLVM, or generated C++ and one native compiler invocation from C++.
`--backend cpp|llvm` selects the immutable executable backend. Without it the
target's default applies, `defaultBackend(TargetInfo)`: LLVM on macOS arm64 and
Linux x86_64, C++ elsewhere. The LLVM choice emits native objects for the admitted shapes
documented in [backend architecture](backend.md#llvm-scalar-execution).
Unsupported required shapes report `GTI-B0003` before publication, without a
C++ fallback. `--std` and `--keep-cpp` are C++-only: without `--backend` they
select C++, and with `--backend llvm` they are rejected. Explicit emit modes
keep their dedicated backend entry points rather than accepting `--backend`.
Repeatable `-D NAME` options seed valueless GTI configuration flags
for the invocation. Repeatable `-Wno-<group>` options (or the equivalent
`--disable-warning <group>`) add validated warning groups to the immutable
`CompilationRequest`; for example, `-Wno-unused-mut` suppresses `GTI-S2096`.
Accepted native arguments after `--` remain exact argv values; native
`-DNAME[=VALUE]` operands there are compiler arguments and do not define GTI
flags.

Backend source artifacts enter `NativeCompileRequest`, which applies the
selected C++ standard and generated-source floating-point policy before
linking. Backend object artifacts instead enter `NativeLinkRequest`: the
driver publishes the opaque bytes atomically, then links them with the same
target policy, declared native objects, ordered libraries/frameworks, and GTI
runtime without adding source-language or code-generation flags. The LLVM
backend produces these binary artifacts directly. Project requests use the
backend-aware object cache described below; direct requests remain uncached.
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
consumer resolves imports again. Its aggregate digest uses stable logical
unit order, role and entry/prelude flags, consumed content size/hash, ordered
prelude roots, and sorted logical dependency edges. Canonical paths and
ephemeral source-unit identifiers remain explicit sidecar facts but do not
enter that relocation-stable aggregate; an external-path logical identity
remains path-sensitive by design.

When capture is requested, the real generated-C++ compile/link invocation adds
a driver-owned depfile and bypasses whole-program cache reuse so the report
always describes a performed invocation. The existing native depfile parser
selects only dependencies under the resolved runtime include root, and only its
`gti/...` subtree is classified, even when an installed toolchain shares that
include directory with other headers. The sidecar records those used headers,
the exact runtime archive input, their content hashes, the native compiler
executable identity, and exact native argv. Its runtime aggregate hashes header
kind/logical name and content facts plus archive content facts, while retaining
canonical paths only in the individual records. It does not claim which archive
members the linker selected.

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
Metadata schema 15 reports `debugInfo` in each profile and resolved output plan.
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

Debug builds use the executable cache like any other build. Their artifacts
embed the loaded units' absolute paths (line directives, native DWARF, the
symbol bundle), so a debug build is keyed on those paths as well as on
logical names, and on Darwin on the `dsymutil` identity; a moved checkout is a
miss where a plain build would hit. A cache entry of a Darwin debug build
keeps a copy of the published `.dSYM` bundle with a digest over its files,
and a hit restores the bundle through the same owned-bundle publication a
fresh build uses. Debug builds are not promised byte-reproducible native
artifacts.
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

`build`, `run`, `test`, and `install` accept `--backend cpp|llvm`. Each profile
may set `backend = "cpp"|"llvm"`, and an explicit command-line choice overrides
the selected profile. With neither, C++ retention (`keep-cpp` or `--keep-cpp`)
or an explicit `--std` selects C++, and otherwise the target's default applies:
LLVM on macOS arm64 and Linux x86_64, C++ elsewhere. The immutable build plan carries that
choice into the same executable driver used by direct mode, including parallel
`--all` children. LLVM intermediates use `.gti.o`, never `.gti.cpp`. C++
retention and an explicit `--std` are rejected for an explicitly selected LLVM
plan. Backend selection is not a frontend analysis policy; `check` and
emit-only commands do not accept the executable-backend override. The project
cache keeps C++ and object artifacts under separate schemas, so a cached build
from before the default changed is never restored for the other backend.

Metadata schema 15 exposes profile/output `backend` and output
`generatedArtifact`; the compatibility `generatedCpp` field is empty for LLVM.
Native C/C++ dependency objects and consumer-approved package build commands
still join the normal native link and cache identity. GTI-authored host build
programs use the effective backend too: an unsupported LLVM build program
rejects rather than silently generating C++. LLVM host-program compilation uses
the exact executable cache. The older script-output state does not identify
transitive GTI, backend or runtime inputs, so an LLVM host program's output is
reused only when its state also records the content digest of the exact host
executable that produced it; any other host program reruns. Command build
programs retain their existing declared-input contract.

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

Findings located inside the standard library are not reported to an
application by default: the audit still analyses library bodies, because
application provenance flows through them, but a finding there describes the
library's own implementation (see the raw-pointer language document). A
profile opts in with `raw-pointer-audit-library-findings = true`, and
`--raw-pointer-audit-library-findings` / `--no-raw-pointer-audit-library-findings`
override the profile on the same commands. The choice is part of the project
model identity and the build provenance, so a cached artifact is never reused
across the two reporting policies.
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
schema 15 includes the additive `rawPointerAudit` field on profiles and plans.
Build-report schema 2 includes `raw_pointer_audit` (older reports imply `off`)
and the additive boolean `raw_pointer_audit_library_findings` (older reports
imply `false`); `verify` replays that policy unless explicitly overridden and
compares it as a separate provenance fact. The benchmark and compile-time
harnesses decode build reports with an exact key set, so every additive report
field must also be admitted in `scripts/benchmark_compare.py`.

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

Annotated test targets compile one suite, then execute each semantically selected
`[[test]] void` function in a separate captured child process. The driver retains
the sorted qualified-name catalog from compilation, including on whole-program
cache hits; cache schema `gti-build-cache-v4` stores and verifies its digest.
Test mode is a distinct cache input. The reusable `runTestSuites` driver owns
selection, the bounded scheduler and result accounting; the CLI owns options
and presentation. Legacy test executables with `main` and no annotated functions
execute once, using their target name as their test identity.
In human mode these legacy programs inherit stdout and stderr, preserving their
stream routing and existing coloured Passed/Failed target status. Their streams
may interleave with concurrent children when `--jobs` exceeds one. JSON mode
captures legacy output so stdout remains a stream of report events. Reusable
driver callers capture both forms by default; `inheritLegacyOutput` opts into
the human CLI policy without changing deadlines or process-group ownership.

`gti test --filter <text>` selects qualified names by case-sensitive substring;
`--exact` requires equality and a filter. Repeated `--skip <text>` exclusions
apply after the filter. `--list` builds/discovers the selected suites and lists
selected tests without launching them. Empty selections fail unless
`--allow-empty` is explicit, including in list mode. `--jobs N` bounds concurrent
test children (default one); suite builds remain sequential. Results are streamed
in completion order; single-job runs retain target/declaration order. The first
failing status always follows stable target/declaration order, independent of
completion order. All selected tests continue after a child failure. CLI observers
consume and release captured output after each completion, rather than retaining
all suite output in memory. Reusable driver callers may retain it explicitly
through the default `retainOutput` option. Metadata remains in declaration order.

The per-child monotonic deadline defaults to five minutes. `--timeout Nms|Ns|Nm`
overrides it; `0` explicitly disables it. The POSIX process owner creates a new
process group, polls all active children, and kills the entire group on timeout
before reaping the child. On ordinary completion `waitid(WNOWAIT)` observes the
exit without releasing the child's identity; group cleanup then precedes the
final reap, so a recycled PID cannot receive that cleanup signal. The caller
must retain exclusive wait ownership; external reaping or SIGCHLD auto-reaping
is unsupported, and observed lost ownership fails without signalling the
released numeric ID. The group is also terminated on ordinary completion so
background descendants do not escape the test lifetime. Descendants that
explicitly detach into a new session/group are outside this boundary; process
isolation is not an operating-system sandbox. Timeout status is 124 and forced
termination does not promise source destructor cleanup. Normal GTI panic cleanup
is unchanged. During execution the CLI records SIGINT/SIGTERM (and broken report
pipes) in scoped signal handlers; the driver stops admission, kills/reaps active
groups, and reports an incomplete run with status 128+signal. Unstarted selected
cases do not receive fabricated completion records. Uncatchable parent death is
outside this guarantee. Launch setup/exec failure uses a close-on-exec handshake, distinct
from a program returning 127; signal status is derived from `waitpid`, not
inferred from an integer exit value. Capture seek/size/read/premature-EOF/close
errors produce driver diagnostics and runner errors, preserving available output
and genuine nonzero child statuses; an otherwise successful child becomes status
74. A valid empty capture still passes. Windows currently rejects bounded isolated
execution explicitly; the shipped macOS and Linux platforms implement it.

`--format json` emits UTF-8 JSON Lines exclusively on stdout; human build
progress and diagnostics stay on stderr. Every event has `schema_version: 1`.
`suite_discovered` records the target and every discovered test with `selected`
and `skip_reason` (`filtered`, `skipped`, or null). `test_started` records each
launch attempt; `test_finished` records its target/name, status (`passed`,
`failed`, `crashed`, `timed_out`, `runner_error`), exit code, optional signal,
duration, captured output and optional diagnostic. A failed launch is a completed
runner error. Assertion/panic failures and ordinary nonzero exits are `failed`;
the runner does not infer an assertion category from status 70. Invalid UTF-8
output bytes become U+FFFD; valid Unicode and embedded control bytes are escaped
without allowing extra events. Completion events follow observed completion
order and are identified by target/name, not line position.

`run_finished` includes discovered, selected, started, completed, passed, failed,
crashed, timed_out, runner_errors and skipped counts, plus `list_only`, `complete`,
exit_code and diagnostic. `skipped` includes all discovered-but-unselected tests.
For execution, complete requires one start/completion per selected identity and
consistent terminal counts; failure outcomes still constitute completed runs.
Listing is explicitly complete with zero launches. Invalid catalogs, unexpected
empty selections and build/resolution failures produce incomplete nonzero
summaries. Argument parsing errors precede the event protocol. Output-stream
failure returns nonzero even when all selected tests passed. These records
establish runner completion,
not proof against a compiler silently miscompiling both a test and its checks;
independent compiler conformance oracles remain necessary.

The frontend validates the attributes and selected-package source identities,
then generates a dispatcher as ordinary GTI source in the suite's lexical
entry context. A second semantic pass resolves that dispatcher before existing
HIR/MIR lowering and verification. The hosted argv adapter, selected calls and
cleanup remain existing MIR operations; neither the backend nor process runner
infers source test semantics. The generated source is request-local and does
not edit the user's root. Test builds load the standard owned-argument units
needed by that hosted dispatcher. This staging deliberately incurs a second
semantic pass for annotated suites, while native compilation still runs once.
The compiler-owned entry resolves the selected package's annotated declarations
by their semantic identities across unit/module visibility boundaries.
This permission is attached to fresh generated syntax and exact semantic test
identities; ordinary user bodies retain their normal visibility rules.

The cost model is one GTI/native build per suite and one process launch per
selected function, including normal hosted argument ownership and capture of
that child's output. Successful cases do not enter the panic path. Isolation
intentionally repeats global initialization and teardown; process-local state is
not shared between cases. The initial dispatcher compares qualified-name selectors
once per child. No test-specific runtime, extra worker thread, or exception
mechanism is involved; the parent polls a bounded set of child handles. Ordinary non-test builds do not run test discovery or
the second semantic pass. Build/run comparisons must retain equivalent process
isolation rather than compare this workflow with an in-process test harness.

The verified hosted `ReturnEntry` adapter preserves returned integers in
0..255 and maps every other integer to status 1, after invocation cleanup.
Consequently, a test returning a nonzero failure count such as 256 cannot be
reported as passed because the host truncates its process status. The defined
runtime-failure status remains 70.

The driver supports an infallible LLVM `library` target. A target must declare
`linkage = "shared"` and at least one
`[[targets.<name>.exports]]` table mapping an exact qualified GTI function to
an explicit C symbol. `ProjectTarget`, `ProjectBuildPlan`, `ProjectFacts`, and
`CompilationRequest` retain that policy and all declaration spans; the build
plan's model identity canonicalizes export rows by public symbol. Its planned
output uses the target platform's library spelling inside a fresh immutable
generation directory. A library build uses the normal selected-backend
pipeline and one loaded frontend snapshot; it needs the LLVM backend, which is
the default on macOS arm64 and Linux x86_64 and `--backend llvm` elsewhere. Unsupported targets,
fallible closures and the C++ backend reject. A host
loads the printed library path and compiles against its adjacent generated
header. Library targets cannot be run/tested as executables.

The embedding SDK now separates generation from publication:
`generateLlvmEmbedding` produces a matched native object, generated header and
exact public-symbol list from one lowered program. `publishEmbeddingLibrary`
in `gti_driver` links that object with the PIC runtime through the existing
native command machinery. On macOS arm64 and Linux x86_64 it emits an explicit
export allowlist and requires all native references to resolve. A new immutable
generation directory containing only the library and header is atomically
published after linking succeeds. Publication uses an exclusive rename, so an
existing destination also rejects when it was created while the linker ran.
Hosts may retain an older loaded generation while building another. Native
failure removes private staging artifacts without replacing an existing
destination. The project driver uses this same publisher to stage a complete
generation before exposing it. Its `embedding-v1` cache extends the existing
LLVM/toolchain identity with linkage, ABI and exact export requests, checks
both object and header payloads, and restores a complete new generation on a
hit. Missing, changed or non-regular companion files invalidate the entry.
Reports include the library plus `embedding_header` path/size/SHA-256; report
capture retains the existing real-invocation cache bypass. Report replay ignores
only the SDK-generated object/export-map locations within its private link
directory; user operands, linker policy and artifact/header content remain
compared. Debug generations include the matching native symbols, collected
before removing the link object and restored on cache hits. `gti clean` removes
managed generations. Hosts remain responsible for ending use before cleanup
or unload. Fallible exports and the full installed/editor embedding campaign
are not implied by this infallible project workflow.

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
the shared frontend request. Metadata schema 15 reports that effective `defines`
array beside each target/profile output plan.
Editor profile selection uses these same facts: `gti_lsp` consumes the named
profile selected by `settings.gti.profile`/initialization options, or the normal
driver default when none is selected. Defines, execution model, raw-pointer
audit and warning policy change together without invoking a build.

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
required standard string and vector imports. `init` continues to preserve an
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
each runtime header, the runtime archive, the runtime
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
`import alias.bindings;`: the generated file declares `export module bindings;`
and explicitly exports the imported declarations. Its directory is a synthetic
package source root with a registered `bindings` module, joined to the declaring
package by an ordinary alias edge, so
resolution, visibility, and cache identity follow the existing package
mechanism and the alias shares the dependency-alias namespace (a collision
is a manifest diagnostic). Every package in the resolved graph generates its
own imports; aliases do not leak to dependents. The read-only project-facts path
registers these same synthetic owners without generating files or fetching
dependencies. Before generation, resolution reports the missing artifact and
asks for `gti check` or `gti build`; after generation, editor analysis uses it.

Direct `gti bindgen` accepts `--module net.http`. Without that option it derives
a valid module name from the output file stem, or the header stem when writing
to stdout, replacing punctuation and protecting reserved names. Distinct output
names can therefore coexist in one direct compilation. Project bindgen pins the
registered name `bindings`; namespaces remain an independent API choice.

Imported enums retain the selected native integer family and evaluated values,
including unsigned high-bit flags and supported 64-bit ranges. See
[enum representation](../language/native-c-interop.md#imported-enum-representation)
for the current reader profiles and ABI-option boundary. Existing handwritten
`int32_t` arguments may need an explicit conversion after regenerating older
bindings; that type change corrects the native declaration, not GTI's conversion
rules. The runnable `examples/native-enum-bindings` project exercises this path.

Callback-slot imports likewise preserve pointer depth: a native `Callback*`
must regenerate as a slot pointer, not a callback value. Function typedefs,
text-storage aliases and fixed-array aliases use the same importer in direct
and manifest modes; see [composable typedef imports](../language/native-c-interop.md#composable-typedef-imports)
and `examples/native-callback-slots`. Release identity invalidates stale
generated files when these import rules change.

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
requires Clang specifically. Editor analysis of a package import whose
bindings have never been generated reports an unresolved import until the
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
`gti_driver`. `SourceLoader` remains the only source dependency resolver. In a
project compilation, `import math.add;` resolves `math` through the importing
package's direct dependency aliases and `add` through that dependency's module
map. The selected interface remains ordinary untrusted GTI source. Transitive
aliases do not leak, and quoted imports cannot cross an owning package boundary
to bypass the manifest edge. Direct mode receives no package graph and therefore
rejects package imports without consulting nearby manifests. GTI `#include` is
rejected with `GTI-I0014` and does not load a dependency. An immutable source-owner
registry gives packages, generated bindings, and the configured toolchain the same
containment and public-map rules; compiler trust remains a separate provenance fact.

### Source Module Registration

Project resolution discovers source modules under `[package].source-root`
(`src` by default). Leading `export module name;` declarations supply interfaces;
leading `module name;` declarations supply private implementation membership.
Filenames do not determine logical names or namespaces. This discovery applies
to the root package and declared dependency packages alike; direct single-file
compilation remains manifest-independent.

The compiler lexer reads a small prefix through the leading declaration for
discovery, using the same header parser as source loading. It extends that
prefix when a comment, token or UTF-8 sequence crosses a read boundary; it does
not read a large unused body into memory. An unused module body is not parsed,
checked or lowered. Directory traversal and implementation order are sorted,
and overlapping roots do not duplicate files. Within each selected root, hidden
entries, `build` directories, directory symlinks and nested packages are not
traversed. Explicit roots can select contained hidden/generated directories or
directory aliases, but cannot cross a nested package boundary. Source
file symlinks must stay inside their owning package. A missing default `src`
directory is allowed for projects using explicit paths elsewhere.
A nested `gti.toml` remains a package boundary even when it is a broken
symlink; a damaged child manifest never makes its private sources part of the
parent package.

Optional settings customize discovery; they do not enable it:

```toml
[module-discovery]
roots = ["src", "generated"]
exclude = ["src/core/windows", "src/experimental.gti"]
```

Roots replace the default and must be existing package-contained directories.
Exclusions are package-relative file or directory paths, not glob patterns;
they apply before module membership is collected. `roots = []` selects fully
explicit registration. Excluding a file symlink excludes that entry, without
excluding its independently discovered target or requiring that target to exist
or belong to the package. Exclusions also work through contained directory
aliases used as explicit roots. Unexcluded file links still undergo all package
and duplicate-file checks. Platform/backend folder names have no implicit meaning:
select the desired roots/exclusions or explicit implementation lists before
building. This first implementation does not add conditional discovery tables.

All discovered interfaces enter the package's public module map; only their
explicitly exported declarations become visible to consumers. Private
implementation files never become independently importable public interfaces.
Discovery cannot cross a nested package boundary or grant a dependency new
aliases. Saved header changes, file creation/removal and implementation
membership changes are reflected on the next project resolution and in its
model/cache identity. Loaded source bytes retain their existing cache checks.

Source packages can also declare explicit selections independently of file layout:

```toml
[modules]
main = "src/api.gti"
geometry = "src/geometry_api.gti"
```

Each key is a dot-separated sequence of non-reserved GTI identifiers. The
`std` prefix is reserved for the configured standard library (`import std.vector;`).
Dotted TOML keys must be quoted, for example `"net.http" = "src/http.gti"`. Each value is a package-relative
path to an existing regular `.gti` interface. Canonical paths must remain inside
the package, including through symlinks. Duplicate physical files, portable
case-fold name collisions, invalid names/types and missing paths are rejected
by manifest loading. The frontend additionally requires the file to begin with
`export module` using the registered name. Paths need not be under `source-root`;
an explicit selection wins over discovered interfaces with that exact name.
Without an explicit selection, duplicate interfaces and portable case-fold name
collisions are errors. An explicitly registered file is not also discovered
under another name; the frontend still checks that its header matches its
registration. For example, the selected geometry interface is:

```gti
// src/geometry_api.gti
export module geometry;
export namespace geometry {
  int answer() { return 42; }
}
```

The owning package writes `import geometry;`. Consumers write
`import dependency.geometry;`, using their declared dependency alias. The
shorter `import dependency;` selects the dependency's `main` module. An
exact local-name/dependency-alias-prefix collision is an error. For example,
`import net.http;` is ambiguous if both local module `net.http` and direct alias
`net` exposes module `http`; directory structure never resolves that ambiguity. Only direct
dependency aliases resolve; re-exporting declarations never leaks aliases.
Direct compilation remains manifest-independent and can write a relative
quoted import instead.

An optional table selects bodies separately from interfaces:

```toml
[module-implementations]
geometry = ["src/geometry_impl.gti"]
```

Each key must name a discovered or explicitly selected interface. Values are arrays of existing,
package-contained, relative `.gti` file paths. Duplicate physical registration,
interface/implementation overlap, non-files and symlink escapes are rejected.
An implementation begins with `module geometry;`, matching its registered
interface name. An explicit list replaces automatic implementation discovery
for that module, including when the list is empty. Its interface is implicitly available through shared module
identity; imports are still required for other dependencies used by its bodies.
Importing the registered interface automatically loads these files, including
when it is reached through a dependency package. Unused module registrations
do not load bodies. Consumers gain no visibility into implementation helpers.

Registration names, paths and order participate in project-model identity;
loaded implementation bytes participate in source keys, reports and editor
snapshot invalidation. Private implementation roots are traversed before the
entry root in deterministic registration/discovery order, with their own
ordinary source dependencies first. Existing initialization/lifetime rules
still apply. No implementation file needs
to be imported directly by a consumer, and no independently linked GTI object is added.
Direct mode still does not discover a manifest: use explicit source edges or
project mode for registered implementations. `examples/window-factory` is a
runnable project illustrating this arrangement: its factory and private concrete
types share the `window` module across three registered implementation files.
`examples/module-discovery` demonstrates the same shared module identity with
one interface and two implementation files discovered without either table;
`module_discovery_runtime` owns its runnable and cache-invalidation coverage.

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
graph). The GTI runtime archive is the final link input, after every operand
and every argument forwarded after `--`. Order-sensitive linkers such as GNU ld
search an archive only for symbols that are already undefined, so this order
lets native objects, libraries and forwarded sources call runtime functions or
replace runtime definitions, as Apple's ld64 always allows. Target-scope native tables on a dependency do not compose; they apply
only when that package is built directly. Dependency native groups now use the
same scoped dependency-discovery and exact-input cache contract as root native
inputs; a group is not by itself a cache bypass. Unmodeled inputs retain the
conservative bypass rules in [Whole-Program Project Cache](#whole-program-project-cache).

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
import std.build;
import std.string;
import std.vector;

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
Generated public bindings are explicit module interfaces. A package facade
exposes a private bindgen dependency only through deliberate exports or
`export import`; an ordinary import does not re-export its declarations. See
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
dependency may **vendor a C library** so its consumer needs no separately
installed copy of that library. The native toolchain and any declared platform
dependencies remain prerequisites.

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
git — and, with its lock/acquisition requirements satisfied, `gti build`
generates the dependency's bindings in the managed build tree, compiles the
vendored C into the dependent build as an isolated
native group, and links it. No separately installed checksum library is
searched for or installed on the machine by this workflow.

Two existing rules do the load-bearing work and are worth restating here.
Bindgen aliases are per-package, so a consumer cannot reach
`import checksum_c.bindings;`; it sees only what the dependency chose to
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
schema 10 introduced each package's source as `path` or
`git+<url>#<rev>#<checksum>`. A fetched package's own path dependencies must
stay inside its checkout; its git dependencies join the same locked closure
transitively. The workspace model identity carries the git source identity,
so two revisions of one `name@version` never share cache state. Because the
store lives under `build/gti`, `gti clean` removes it; rebuilding then
requires either re-acquisition or a fresh `gti fetch`.

Direct mode also exposes `--emit-native-header`. It runs the same semantic
analysis, optimization compatibility check, and MIR verification as C++
emission, then selects `NativeHeaderBackend` instead of `CppBackend`. The mode
writes one `.h` artifact, defaults to `<entry>.native.h`, applies the loaded
source overwrite guard, and performs no native compilation or linking. It is
mutually exclusive with `--emit-cpp`, `--keep-cpp`, and trailing compiler
arguments. Automatic placement of this generated artifact into project-native
include paths is a later build-system convenience; current projects may check
in or explicitly regenerate the header and list its directory under the
existing native include settings.

Direct mode also exposes `--emit-mir`. It runs the shared semantic analysis,
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
manifest edit. Hyphens in registry names become underscores in default dependency
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
- the source-level debug policy and, when it is on, every loaded unit's path
  as loaded plus (on Darwin) the `dsymutil` command, executable content and
  version, because debug artifacts embed those paths and the bundle is that
  tool's output;
- for an internally enabled dormant library plan, its library/shared kind and
  canonical public-symbol-to-qualified-function export rows (ordinary
  executable/test identities remain byte-compatible);
- the ordered `SourceGraph::preludeRoots()` logical identities, followed by
  SHA-256 content identities for every loaded GTI unit, ordered logical source
  edges, and standard-library import names;
- the native C++ compiler command, resolved executable content, and `--version`
  output (plus the same identity for the C compiler when C sources are
  declared);
- runtime headers/archive;
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
  resolution is captured as the compiler saw it at lookup time (memoized as
  described below, so an unchanged source does not rerun the preprocessor);
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

Discovery is memoized beneath `build/gti/cache/native-discovery/`, one
`gti-native-discovery-v1` record per probe, keyed on the native environment
values, the compilers' command, executable content and version, and the exact
probe argument vector with its staged output paths blanked. A record holds the
preprocessed unit's digest, every discovered file with its content digest and
a stat stamp (size, modification time, inode), and the entry-name digest of
every directory where a new file could change include resolution: the parent
of every discovered file (quote lookup) and every declared and toolchain
include directory (angle lookup). At lookup the driver relists those
directories and checks every dependency: an unchanged stamp that settled at
least two seconds before the record was written vouches for its digest,
anything else is rehashed and must match. A record that holds contributes
exactly the identity bytes the probe would have and the preprocessor is not
run; otherwise the probe runs and rewrites the record. Writing a record is
best effort; a missing or unparseable one only costs a probe. The residual
difference from re-running the preprocessor is a new file in a system include
directory that contributed nothing before and now shadows another system
header; toolchain changes of that kind are the compiler identity's domain.

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
and project executable requests resolve one canonical standard-library prelude.
Test-suite requests additionally select the owned-argument units needed by the
dispatcher. Secondary prelude roots have distinct configuration-order logical
identities. There is no user free-form override.

Schema-v4 entries live beneath `build/gti/cache/v4/<sha256>/` and begin with the
`gti-build-cache-v4` metadata marker. Older entries and metadata are not
interpreted under this schema. Each v4 entry contains generated C++, the
executable, for a Darwin debug build a `symbols.dSYM` copy of the published
bundle, and strict
metadata recording the digest and size of the first two and a digest over the
bundle's files (or `symbols none`), followed by the digested sorted test catalog.
Executable permissions are recorded as well. Metadata is published last and
acts as the commit marker. A hit verifies every digest and the executable mode
before atomically copying the executable to the requested output; it
also restores generated C++ when `keep-cpp` is active. Missing entries rebuild
normally. Incomplete or corrupt entries are diagnosed, never executed, and
are replaced only after a successful rebuild. Cache-publication failure does
not discard a successfully published program. Deleting only `build/gti/cache`
does not modify sources or published target artifacts; `gti clean` deliberately
removes the entire validated `build/gti` subtree, including both.

Native-object storage has a separate schema-v5 representation under
`build/gti/cache/v5/<sha256>/`, marked `gti-build-cache-v5`. Its metadata
requires `artifact object .o`, and `artifact.o` preserves opaque binary bytes,
including NULs, rather than labeling them generated C++. Lookup verifies the
object and executable digests, permissions and test catalog before restoration;
native metadata and payload leaves must be regular files, not symlinks. A
restored backend result retains object kind and `.o` extension. C++ continues
to use the unchanged v4 identity and metadata format.

LLVM reuse additionally consumes a producer-owned query of the same target
machine factory used for object emission: resolved triple, data layout, CPU,
features, relocation/code model, code-generation optimization and private
execution ABI revision. The key includes the backend identity schema and LLVM
version plus the exact containing compiler executable's content; the build
requires the LLVM backend closure to be statically linked, so those bytes also
identify the actual LLVM implementation, including dirty builds. Existing
source/dependency, runtime-content, native-toolchain and debug/policy inputs
remain part of the key. An unavailable required identity bypasses; no CPU,
feature, relocation or ABI fact is guessed. LLVM debug requests use the composed
O-LLVM-15 source/parameter and instruction/terminator metadata hooks; ordinary
local lexical scopes, receiver/pack bindings and optimized inline availability
remain dependent on their owning verified producer facts. The driver does not
infer those facts from the AST or emitted representation.

Object identity schema `gti-llvm-object-identity-v2` additionally records the
actual debug boolean and stable diagnostic instrumentation spelling. The
LLVM-free API exposes None, Address, UndefinedMemory,
AddressAndUndefinedMemory and Thread; UndefinedMemory denotes only original
generated load/store null/alignment checks, not complete UBSan. Immutable driver
requests select these policies through `withLlvmInstrumentation`, after explicit
LLVM backend selection. Object generation and cache identity use one options
mapping. Address, UndefinedMemory and their combination use the generated LLVM
pipeline and matching native runtime plan; unknown values and C++ combinations
reject. Thread remains gated on communicating-runtime provenance.
The default None policy retains current execution. No CLI or manifest selector
is exposed by this typed API checkpoint, and binary cache payload v5 is unchanged.

For a non-None request the driver resolves matching upstream Clang and compiler-rt
before source loading, cache restoration or publication. It records the selected
compiler and implementation files, reported flat default configuration, resource
and runtime roots, exact runtime files, native link plan and SDK library/settings
identity. On Linux the actual Clang link plan determines one installed runtime
directory beneath its resource library root; distributions may report a missing
per-target directory while selecting an installed alternate layout. Multiple
runtime roots or links escaping the resource tree reject rather than guessing.
It replays that identified configuration explicitly; a different Clang
version, opaque configuration/dependency graph, or unavailable runtime rejects.
Native preprocessing, C/C++ object compilation and final linking consume the same
resolved arguments. Bounded UndefinedMemory compiles native providers with only
null/alignment checks; `-fsanitize=undefined` selects its link runtime. None adds no
runtime query or dependency. Thread still needs verified communicating-runtime
provenance, and instrumented report replay/external native-link provenance remain
prerequisites. The isolated compatibility probe is C ABI source, never generated C++ for a GTI
body. Native runtime link/start controls do not establish generated GTI
instrumentation acceptance. The generated-memory probe separately exercises O0
and O3 valid access/cleanup, native-runtime identity, cache separation/restoration,
failed publication and invalid loads executed by GTI-generated instructions.
It does not claim complete UBSan, foreign-library or Thread coverage.
The explicit `llvm_native_runtime_identity` fixture is enabled
with `GTI_LLVM_RUNTIME_TEST_COMPILER` set to a matching Clang++; it fails on a
mismatch rather than counting a missing runtime pair as passing coverage.

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
Metadata schema 15 publishes the profile fields for every profile plus the
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
dependency warnings enabled by default. Metadata schema 15 reports
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

The executable cache is whole-program and workspace-local. It does not yet
persist individual parsed units, HIR/MIR bodies, independently reusable native
objects, or remote/shared
artifacts. The separate in-process frontend cache used by editor analysis is
not a project executable-cache entry.
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
Project and direct LLVM selection share the currently admitted executable
profile; they do not imply full-language backend promotion. Source-level native
C calls, generated embedding wrappers, automatic test dispatchers and further
families retain their owning backend admission gates. Debug companion production
consumes O-LLVM-15 metadata. On Darwin a debug cache entry must contain its
companion: metadata that suppresses it is corruption. Cold and cache-hit paths
validate companion ownership and source/output containment before publication.
They stage the executable and symbols, retain the previous GTI-owned companion
until the executable rename commits, and restore that companion if executable
publication fails. Each file/directory rename is atomic; the two artifacts are
not a simultaneous atomic snapshot. Native C/C++ provider compilation and its
dependency probes receive the same source-debug flag.
LLVM cross-target admission remains host-only pending initialized target,
runtime/sysroot and execution evidence. LLVM executable requests reject explicit
native target/sysroot policies and, on Darwin, `MACOSX_DEPLOYMENT_TARGET`,
`SDKROOT` or `DEVELOPER_DIR` overrides before cache lookup or publication until
the backend factory and runtime artifacts share a verified policy. Recognized
Darwin deployment options in final native-link arguments are rejected too,
including compiler minimum-version/target-OS options and linker forwarding
through `-Wl,` or `-Xlinker=`. A cache bypass alone would not make those
inconsistent objects safe to publish. This avoids
publishing a lower-deployment executable containing newer-deployment objects.
System-LLVM installed evidence does not
substitute for the pinned bundled-LLVM packaging gate.
The LSP consumes the driver-published project facts for host-target analysis and
supports a named editor profile, with source-based target inference. Explicit
editor selection of another workspace package, target, or cross target is not
implemented.


## Compiled library bundle foundation

The development API in `gti/driver/library_bundle.h` implements a bounded,
deterministic `.gtilib` byte container, an identity envelope, inspection by
validated decoding, and exact compatibility comparison. The container remains
independent of project builds and caches. Its object bytes are opaque: successful
decoding proves neither native-object validity nor GTI semantic consumability.
The separate scalar driver API below supplies the first executable importer and
performs additional compiler/backend validation.
There is no new CLI flag or manifest mode. Ordinary source stays `.gti`;
`.gtimeta` is reserved but not emitted because no independently usable semantic
metadata artifact exists yet. The existing engine embedding library mode is a
different C ABI contract and retains its current behavior.

### Provisional wire format version 2

All integers are unsigned 32-bit little endian. A blob is its byte length followed
by exactly those bytes; there is no padding, compression, timestamp, platform
permission, host-width integer or native structure dump. The complete layout is:

1. Eight bytes `GTILIB\r\n`, container version `2`, envelope schema `1`.
2. Six blobs: toolchain SHA-256, canonical target triple, configuration SHA-256,
   runtime SHA-256, logical library SHA-256, complete source-input SHA-256.
3. One interface-metadata blob, limited to 1 MiB; empty for opaque container-only use.
4. Dependency count, then pairs of logical library SHA-256 and complete dependency
   bundle SHA-256, sorted strictly by logical library identity.
5. Object count, then name/data blob pairs sorted strictly by name.
6. Exactly 64 lowercase ASCII hexadecimal bytes: SHA-256 of all preceding bytes.

Hash fields are exactly 64 lowercase hexadecimal bytes. The target is a
caller-normalized ASCII triple (at most 255 bytes); the codec checks its bounded
lexical form, not target support or normalization. Callers must derive identities
from independently resolved inputs; an artifact cannot authenticate its own
compatibility. `requireLibraryBundleCompatibility` compares all six identities
and the complete dependency set, independent of the caller's dependency order.
A checksum detects damage, not malicious replacement or an untrusted producer.
Registry/package trust and native object admission remain separate obligations.

Each dependency and object list is limited to 4096 entries. At least one nonempty
object is required. Objects are limited to 64 MiB each and the entire serialized
bundle to 256 MiB. Names are lowercase ASCII letters/digits/underscore/hyphen
followed by `.o`, at most 255 bytes, with reserved Windows device basenames
rejected. They are logical keys, never filesystem paths. The writer sorts without
copying object payloads; the reader rejects noncanonical ordering, duplicates,
self-dependencies, trailing data, truncated lengths, excessive counts/bytes,
unsupported versions and checksum mismatches. Errors have typed categories;
there are no newly allocated compiler diagnostic codes at this development API.

The API accepts/returns owned bytes and performs no I/O, extraction, subprocess
execution, or publication. This deliberately avoids symlink/overwrite races.
A future file adapter must bound the read before allocation, use private staging,
validate objects with the selected backend, and atomically publish through the
existing artifact layer. It must not reinterpret member names as output paths.
The bounded reader may hold the input and decoded copy simultaneously; lazy
validated views/indexing are a future measured improvement, not a present claim.

`library_bundle` tests the codec and compatibility contract. The design decision
and downstream proof obligations are in [ADR 032](../decisions/032-compiled-library-bundles.md)
and the [separate-compilation stage plan](../plans/build-system.md#compiled-library-and-separate-compilation-stages).


### Executable typed separate-compilation profile

`gti/driver/compiled_scalar_library.h` provides an LLVM producer and consumer
for compiled GTI targets. Its development API names retain “scalar” for source
compatibility; project commands expose the typed profile through `gti-library`,
explicit artifact imports and same-manifest `compiled-dependencies` edges.
C embedding retains its separate request and ABI. Runnable examples cover
[primitive APIs](../../examples/compiled-primitive-library/README.md),
[value types](../../examples/compiled-value-types/README.md),
[borrowing](../../examples/compiled-borrowing/README.md),
[namespaced APIs](../../examples/compiled-namespaces/README.md), and
[dependency builds](../../examples/compiled-dependencies/README.md).
[Type forwarding](../../examples/compiled-type-forwarding/README.md) composes
dependency-owned records and enums through multiple compiled libraries.

The compiler-owned `CompiledScalarUnitBuilder::selectExports` projects selected
root-module function names, including all exported overloads. Names may be
namespace-qualified; explicit exports and exports inherited from enclosing
namespace declarations are preserved. Reopening a namespace does not export its
private siblings. Functions must be nongeneric, without default arguments. Parameters and
results admit bool, char, fixed integers, float/double, concrete value records,
integral enums and checked references to those values; void results remain valid.
Constexpr functions, entry functions and unsafe/private capabilities reject.
A target may contain multiple source modules. Its exposed nominal closure can
belong to the root module or to verified compiled dependencies; ordinary non-root
source modules still require a logical ownership mapping. There are at most 256
exported overloads and 64 nominal declarations per interface.

Qualified declaration names retain their complete namespace path, with at most
1,024 bytes and 64 identifier components. Same-leaf declarations in different
namespaces remain distinct. The virtual interface reconstructs namespace blocks
and emits collision-free, module-private type aliases at root scope. Record fields,
enum defaults and callable signatures use these aliases so relative lookup in a
shadowing namespace cannot change a producer-resolved type. Aliases carry no new
type identity or native wrapper; imported declarations still undergo canonical
metadata comparison. Nested class-owned types require owner metadata and reject.

Nominal metadata preserves each declaration's defining package, target and module,
qualified name, export visibility,
class/struct kind, field order/type/access/mutability, supported scalar constant
field defaults, enum underlying types and enumerators. Nested plain value fields
use the referenced record's generated default. Explicit defaulted constructors
retain their access. A sealed declaration-complete class summary retains these
constructors and the presence of other source members before executable
reachability pruning. Consumer-local designated construction is regenerated from
fields and does not become an exported source member. IDs are remapped into each fresh
compiler session. The producer and reachable imported lowered declarations are
independently projected and compared against the canonical metadata; source-session IDs are
never an ABI. Records require generated structural value lifecycles. Bases, custom
constructors/destructors, member functions, static members, stored references,
payload enums and generic definitions require further contracts and reject.
Field defaults are bounded resolved scalar constants or signed literals, not
serialized expression bodies. This includes finite floating literals and source
character literals; arbitrary runtime initializer expressions are not admitted.

Checked reference parameters preserve read/mutable access. A reference result
must identify exactly one whole reference parameter and its permitted access.
The producer's semantic/MIR return summary must agree; projected/conditional,
global and aggregate-carried origins reject. The virtual declaration publishes
`[[borrowed_from(argN)]]`, and ordinary semantic/MIR loan rules check consumer
escape, mutation and aliasing. This does not yet export custom ownership/drop
recipes or a general lifetime dependency graph.

Callable metadata profile `gti.callable-interface.3` contains key-sorted,
length-framed records: persistent function key, result recipe, common nominal
metadata, borrow origin, parameter ordinal and access. Duplicate overload keys
reject even when results differ. `gti.nominal-interface.2` stores the bounded
declaration closure. Development codec entry points expose the same canonical
validation used by the importer. Unknown profiles, malformed/noncanonical records,
unresolved recipes and incomplete borrow contracts reject. No AST pointers,
numerical instance IDs or arbitrary source bodies enter the interface.

Dependency-owned declarations retain their original identity when used as function
parameters/results or nested fields. The consumer requires the defining artifact
in the verified transitive dependency receipts and imports its virtual interface;
it does not duplicate that type in the forwarding library. Missing or inconsistent
owners, receipts or reachable layouts reject. Type dependencies receive private
virtual paths when the project graph supplied only a transitive artifact. Relative
imports preserve source-owner boundaries and quote relocated paths safely. These
ordinary imports do not re-export dependency names: consumers explicitly import
the defining interface when naming those declarations. Whole-module re-export
contracts and colliding fully qualified nominal names remain separate work.

`LoweredProgram` retains logical owner/source-unit bindings independently of
callable reachability, including a library used only for its types. Serialization
profile `lowered-program-v31` records these bindings. Wire metadata is validated
completely; concrete declaration comparison follows HIR reachability. An unused
import may lack an instantiated type or function, but every reached GTI declaration
must still bind to its exact validated callable contract. No new runtime adapter,
ownership rule or allocation is introduced by forwarding a nominal type.

The consumer validates envelope compatibility, dependency receipts and native
objects, then projects metadata into a virtual source interface for the ordinary
loader/parser/type checker. The loader's compiled-interface marker is part of
semantic cache context and analysis seals and cannot be authored in source.
Ordinary unresolved declarations still diagnose. Interface paths must be absent,
absolute and normalized. No facade or producer implementation source is written
or reopened by explicit artifact imports; unused imported declarations are valid.

After lowering, `CompiledScalarUnitBuilder` binds concrete exported and imported
instances together. The complete plan is sealed in `LoweredProgram` serialization
v30, including source-unit identity, nominal metadata, result recipe and borrow
contract. LLVM uses v3 SHA-256 symbols derived from the complete callable contract.
An external wrapper preserves GTI's status/result/failure ABI; aggregate results
forward the caller's destination, and fallible bodies forward the original failure
record. Consumers do not infer purity, failure freedom or ownership from object
code. These symbols do not promise cross-version ABI stability.

Each bundle contains one `unit.o` and exact direct-dependency digest receipts.
Object validation checks relocatable format, target/layout, exact exported
symbols and unresolved symbols limited to declared imported GTI callables.
Transitive imports contribute their objects without requiring a source facade.
Program initialization, TLS and hosted producer entry remain unsupported;
producer cleanup needing external runtime services still requires a broader
runtime-closure contract. Consumer cleanup and checked failure propagation retain
their normal behavior. The driver owns bounded reads, native staging and atomic
publication. Unsupported producer shapes use existing backend-limit diagnostics;
malformed artifacts use existing driver validation diagnostics.

Compatibility binds compiler executable, runtime archive, LLVM/backend ABI,
target/layout/CPU/features, optimization, execution profile and configuration.
Logical package/target labels identify a library independently of interface edits.
Checksums establish integrity, not producer authentication. Explicit artifacts
need no producer sources; automatic source-target builds additionally compare a
source-graph fingerprint and exact dependency digests before reuse. The following
subsection describes that conservative freshness boundary.

`compiled_scalar_library_cost` and `compiled_primitive_cost` compare imported
calls with an equivalent external ABI baseline and record runtime, child peak
RSS, build latency and object/bundle/executable size. The latter includes a
by-value Point record. These allocation-free operations do not claim parity with
whole-program inlining or complete F5 performance acceptance.


### Compiled-library project requests

A producer declares a reusable target and explicitly selects its public API:

```toml
[targets.numbers]
kind = "gti-library"
root = "library.gti"
functions = ["choose"]
```

`gti build numbers --release` publishes `numbers.gtilib` beneath the ordinary
profile/target output directory. `functions` contains 1–256 unique identifiers,
optionally qualified by namespaces with `::`, such as `geometry::choose`.
The root must declare a module interface. This profile automatically
selects LLVM when no backend override is supplied, retains all application
bodies for verified export binding, and rejects unsupported source signatures.
The target is not runnable or installable as an executable. Self-contained
producers cannot declare native inputs, build scripts or bindgen imports.

An executable or another compiled library can explicitly select a distributed artifact:

```toml
[targets.app]
kind = "executable"
root = "main.gti"

[targets.app.compiled-library]
package = "compiled-numbers"
target = "numbers"
path = "dependencies/numbers.gtilib"
interface = "api.gti"
```

The consumer source imports `"api.gti"`. The artifact path is relative to its
manifest and must name a regular `.gtilib` file; explicitly selected artifacts
may live outside the consumer package. The virtual interface must be an absent
`.gti` path inside the package after canonical resolution. The artifact's
package/target identity must match the manifest selection. Neither the virtual
interface nor the producer source is written to disk by import.

Several independent bundles use repeated tables instead:

```toml
[[targets.app.compiled-libraries]]
package = "left-package"
target = "left"
path = "dependencies/left.gtilib"
interface = "left.gti"

[[targets.app.compiled-libraries]]
package = "right-package"
target = "right"
path = "dependencies/right.gtilib"
interface = "right.gti"
```

The singular and plural forms are mutually exclusive. A plural list contains
1–64 libraries, with distinct package/target owners and canonical facade paths.
The combined interface contains at most 256 declarations. Every bundle is
validated even when its facade is unused; unused selections are allowed. Each
virtual module has a distinct logical-library-derived identity. Ordinary source
visibility and overload ambiguity still apply when several facades are imported.
The immutable `compiledImports()` list is canonical; legacy singleton accessors
reject multi-import requests instead of silently losing inputs. Metadata emits
`compiledLibraries` and retains `compiledLibrary` for singleton compatibility.
A producer records each direct dependency by logical library identity and SHA-256
digest of the complete bundle. Consumers must select the complete artifact
closure, with exact receipt matches and one bundle per logical owner. Missing
dependencies, conflicting digests and dependency cycles reject before frontend
entry. Runnable independent-library coverage is in
[`compiled-libraries`](../../examples/compiled-libraries/README.md) and
[`compiled-primitive-library`](../../examples/compiled-primitive-library/README.md).

`ProjectBuildPlan` carries resolved identities, function selection and bounded
artifact inputs into immutable `ExecutableBuildRequest` values. CLI code only
constructs the request and presents the result. The driver reads at most the
256 MiB aggregate bundle limit, rejects incomplete/growing reads, uses the
scalar compiler path once for all imported facades, and links validated object bytes from the private output staging
directory. Producer publication and consumer linking retain the managed-output
and atomic-replacement rules: a failure preserves the prior good artifact,
outputs cannot overwrite any selected bundle/interface, and staged imported
objects are removed on success or failure.

Source dependencies can instead name library targets in the same manifest:

```toml
[[targets.app.compiled-dependencies]]
target = "numbers"
interface = "api/numbers.gti"
```

Both executables and compiled library targets accept these tables alongside
explicit artifact selections. The reusable project resolver constructs a bounded,
immutable dependency DAG, rejects cycles, non-library targets and conflicting
owners/facades, and returns each dependency once in topological order. Build and
run commands execute those plans through the ordinary driver before the selected
target. Shared transitive objects link once; only directly selected interfaces
enter the source graph. Metadata exposes the declared source edges as
`compiledDependencies`. Workspace/package-qualified source target edges remain
future work; existing source package resolution is unchanged.

Successful library outputs are reused when their compiler/runtime/configuration,
complete loaded source graph, export selection and direct dependency digests still
match. Reuse reads and hashes source inputs, validates the existing bundle and
native object, and avoids semantic analysis and native emission. A dependency
change rebuilds its parents; missing or corrupt generated artifacts rebuild from
available source. A failed dependency stops the application build and preserves
the failing target and application outputs; previously completed prerequisites
remain published. Explicit artifacts still require no producer source.
The [`compiled-dependencies`](../../examples/compiled-dependencies/README.md)
example and `compiled_dependency_project` gate cover a diamond graph, freshness,
receipt mismatches, failures and source-free distribution of the complete closure.

Consumer requests retain whole-program cache bypass; compiled library output reuse
is reported as a cache hit only after those complete checks. Build reports,
`--emit-mir`, debug/instrumented builds and artifact-aware LSP
analysis need follow-on query/provenance support; unsupported CLI requests
reject explicitly. Ordinary source projects and direct compilation retain their
existing paths. The `compiled_library_project` gate owns the runnable project
example, deleted-producer execution, consumer-only edits, manifest identities,
bounded inputs and failure/publication controls on both native platforms.


### Read-only compiled-library checking

`gti check numbers --release` validates a scalar producer's selected exports,
source semantics and lowered binding contract without producing
a bundle. `gti check app --release` validates a consumer against its explicitly
selected artifact closure, including when the complete producer source tree is absent.
For source target edges, check requires previously built artifacts and diagnoses a
missing bundle with a request to build it. Checking never builds dependencies.
Use the same profile and configuration as the distributed artifact: compatibility
remains exact, including optimization. Ordinary check's CLI option rules remain
unchanged; select profiles rather than adding optimization override flags.

`checkCompiledScalarProject` consumes the immutable `ProjectBuildPlan` and
resolved `ToolchainLayout`. It shares bounded file reads, metadata decoding,
compiler/runtime/configuration checks, native object admission and virtual-source
preparation with the build path. The imported graph marker reaches the same
frontend, and both producer and consumer reuse the compiler-owned scalar binding
projection. A validation-only backend verifies the bound lowered program and
returns no public artifact. Checking neither constructs the LLVM emission backend
nor invokes a native compiler/linker, writes a facade, publishes an object or
bundle, creates a build/cache directory, or replaces an existing output.

Checking returns `CheckResult` with owned diagnostic sources. Frontend failures
retain their ordinary structured diagnostics; lowered profile failures retain
the existing backend limitation diagnostic and use `ValidationFailure`.
Artifact/configuration failures use `ArtifactFailure` with a driver message.
The ordinary source-only checker still uses its existing frontend path.

This is source and artifact-contract validation, not certification of successful
native emission or linking. Producer native-object admission still runs when a
bundle is built, and consumer native dependencies are linked only by build.
Checking does not execute build scripts. Producer native inputs/scripts remain
unsupported; compiled-library checks with bindgen imports reject before any
binding generation. LSP snapshot ingestion, provenance reports, emitted MIR and
automatic dependency/cache orchestration retain their separate prerequisites.
