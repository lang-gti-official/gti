# GTI

<p align="center">
  <a href="https://github.com/lang-gti-official/gti/actions/workflows/ci.yml"><img src="https://github.com/lang-gti-official/gti/actions/workflows/ci.yml/badge.svg?event=merge_group" alt="CI status"></a>
  <a href="https://github.com/lang-gti-official/gti/blob/main/LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="MIT license"></a>
</p>

<p align="center">
  <img src="assets/branding/gti-icon.png" alt="GTI language icon" width="160">
</p>

GTI is an independent, experimental, statically typed systems language with
strong C++ syntax familiarity. It keeps explicit control, value semantics,
deterministic cleanup, native code generation, and predictable performance while
giving ownership, conversions,
failure, and source visibility smaller rules. The goal is a fully usable systems
language with excellent safety and a standard library expressed in ordinary GTI
over general language facilities and necessary compiler/runtime mechanisms.

GTI is not a C++ wrapper. Its syntax is intentionally familiar, but its ownership,
lifetime, failure, and execution semantics are its own. Neither generated C++
nor LLVM representation is the language contract.

**Backend direction:** LLVM is the primary development backend. C++ is deprecated
as the primary backend and receives bug fixes only through v1.0, retaining its
existing supported behavior as a comparison oracle. C++ catch-up may be
reconsidered after v1.0; it is not promised or automatic. Limited development
capacity is focused on LLVM rather than maintaining feature parity in two
backends. See [the backend policy](docs/decisions/031-native-llvm-backend-foundation.md#backend-development-policy-2026-10-25).

The released compiler checks a complete GTI source graph, builds typed HIR and
MIR, and seals a backend-neutral `LoweredProgram`. On macOS arm64 and Linux
x86_64 the default LLVM backend emits a native object directly. Other targets
keep the C++ backend,
which invokes a native C++ compiler, as their default until LLVM passes its
promotion gates there. `--backend llvm|cpp` selects either backend on any
target. A shape LLVM does not support yet reports `GTI-B0003`; there is no
silent fallback to C++:

```text
source graph -> frontend -> checked AST -> typed HIR/MIR
             -> GTI optimization -> LoweredProgram -> LLVM backend -> native object (macOS arm64 and Linux x86_64 default)
                                                 \-> C++ backend -> native compiler (default elsewhere)
```

> [!IMPORTANT]
> GTI is a personal, AI-assisted, pre-1.0 language project. It is usable for
> experiments, but a documented 0.x minor release may still change draft
> language or standard-library meaning. Patch releases do not intentionally
> break source. The 1.0 scope remains a soft systems-readiness goal until that
> release is published; GTI 1.0 then freezes Edition 1 under the published
> [compatibility policy](docs/decisions/011-language-compatibility-and-editions.md).

This branch contains the unreleased phase 2 module-only release candidate. GTI
`#include` is rejected; the examples below use explicit imports. The change is
held from main and release until production verification completes. See the
[migration guide](docs/language/lexical-and-syntax.md#migrating-existing-source).

## A small example

```gti
import std.string;

class Counter {
  mut int value = 0;

public:
  Counter(int initial) : value(initial) {}

  int increment() mut {
    this.value += 1;
    return this.value;
  }
};

int main() {
  mut Counter counter{2};
  if (counter.increment() == 3) {
    std::println("hello from GTI");
  }
  return 0;
}
```

## What GTI currently provides

For a readable overview of recent capabilities and fixes, see the
[changelog](CHANGELOG.md).

The [reference website source](https://git.internal.howetotech.dev/gti-lang/gti-reference)
lives in its own organisation repository, including the
[import/export reference](https://git.internal.howetotech.dev/gti-lang/gti-reference/src/branch/main/content/language/modules/page.mdx).
See [reference documentation ownership](docs/architecture/reference-documentation.md)
for the compiler API boundary and why the former in-tree website was removed.

- Explicit fixed-width integer types, checked conversions and indexing, and
  immutable-by-default bindings, plus explicit wrapping, saturating, and
  checked-result add/subtract/multiply operations, direct base-10
  `std::print`/`std::println`, owning `std::to_string` conversion, and
  recoverable sequential heterogeneous formatting for booleans, characters,
  integers, binary32 floats, and owned or borrowed text through `std.format`.
- Exact IEEE-754 binary32 `float` and binary64 `double`, with width-selecting
  literals, deterministic promotion and conversion, frontend constant
  evaluation, and bit-exact native emission.
- Frontend-computed `sizeof(type)` and `alignof(type)` for primitives,
  one-level raw pointers, integral scoped enums, aliases, positive concrete
  fixed arrays, passive unions, and passive `[[c_abi]]` records, using the
  selected GTI target layout rather than native C++ queries.
- Classes, structs, interfaces, public inheritance, virtual dispatch, explicit
  construction, and deterministic cleanup.
- Passive native unions with lexically unsafe member access, plus closed
  payload enums with exact construction and exhaustive `switch` patterns.
- Fixed arrays and read-only structured bindings over arrays and flat public
  class or struct fields.
- Non-null read-only and mutable borrows, consumed moves, move-only values,
  nominal unique ownership, single-origin read-only owner dependencies across
  ordinary helpers, bounded exclusive reborrows over stable places, and
  lexically gated one-level raw pointers for native wrappers.
- Exact overloads, named and value generics, source-defined multi-parameter
  concepts, bounded validity-only trailing `requires`, confined variadic
  forwarding and ordered read-only call-pack folds, confined read/mut-callable
  parameters with exact contextual
  results, and cleanup-free consuming `operator() &&` callables with
  path-checked at-most-once invocation. Typed lambdas support immutable copy
  captures and explicit `[target = std::move(source)]` owned captures with
  checked closure movement and cleanup. An exact direct generic parameter of a
  free function may also move a lambda through the same-type result or into the
  exact field of a bounded generic owner without type erasure.
- Contextual brace arguments for any exact by-value fixed-array parameter,
  including inferred array extents on functions, methods, and constructors;
  these are owned arrays rather than C++ `std::initializer_list` values.
- Independent source units, explicit modules and imports, namespaces, aliases, and
  target/configuration conditionals with valueless source, CLI, and manifest
  flags but no textual preprocessing.
- Bounded `extern "C"` declarations for exact native symbols using fixed-width
  integer and floating scalars, layout-stable `[[c_abi]]` records by value,
  nominal pointer-only `[[c_opaque]]` handles, one-level
  scalar/record/`void` pointers, NUL-terminated `c_string`, bounded
  pointer-plus-count returns, exact named same-thread callbacks, and
  non-retained counted text inputs, plus a
  compiler-generated bridge header usable unchanged from C17 or C++20/C++23
  adapter sources. Native C++ classes and RAII may stay behind that C-linkage
  adapter without becoming GTI ABI types.
- A hosted `main(int, std::vector<std::string>)` form that copies native
  command-line arguments into GTI-owned strings instead of exposing `char**`.
- Structured, target-selected native inputs—including automatically compiled C
  and C++ sources—in project manifests and a small move-only POSIX
  `std::tcp::socket` ownership wrapper.
- A source-defined standard-library foundation with checked array, string, and
  move-only vector owners, plus project manifests, an LSP, formatter,
  Tree-sitter parser, and self-installing Neovim/Lazy plugin.

GTI deliberately does not import C++ overload ranking, implicit user
conversions, ADL, SFINAE, textual macros, pointer decay, unchecked array access,
or implicit `switch` fallthrough.

## Build and try it

Requirements are CMake 3.20 or newer, Ninja, a suitable C++ compiler, and LLVM
18 or newer, matching libclang, and **RTTI enabled** (distribution packages such as
Debian `llvm-*-dev` and Homebrew `llvm@*` are; upstream LLVM defaults to off).
CMake uses a compatible system LLVM by default and reports an RTTI mismatch
at configure time. `-DGTI_BUNDLE_LLVM=ON` instead downloads and builds the
pinned LLVM release used for self-contained toolchains, with RTTI enabled
automatically; that mode also requires an explicit `-DCMAKE_BUILD_TYPE`,
which LLVM's own build insists on. Release builds force that acquisition
mode. These are two ways to supply one mandatory dependency. The compiler core
links only the narrowly approved support libraries (`LLVMSupport`,
`LLVMTargetParser`, and `LLVMDemangle`). The independent LLVM backend
additionally owns the exact host `Core`/`nativecodegen`/`Passes` closure and
currently emits the documented scalar executable profile. See
[ADR 006](docs/decisions/006-llvm-support-adoption.md) and
[ADR 031](docs/decisions/031-native-llvm-backend-foundation.md) for the two
boundaries.

```sh
git clone https://github.com/lang-gti-official/gti.git gti
cd gti
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4

./build/gti examples/001-basics.gti -o /tmp/gti-basics
/tmp/gti-basics

# Emit declarations for a C or C++ native shim.
./build/gti examples/049-native-bridge-header.gti \
  --emit-native-header -o /tmp/gti_native.h

# See the nominal opaque-handle wrapper boundary.
./build/gti examples/050-opaque-native-handles.gti \
  -o /tmp/gti-opaque-handles
```

The canonical repository is
https://github.com/lang-gti-official/gti: source, pull requests, issues and CI live there, and every change reaches
`main` through its merge queue, and toolchain releases publish to its GitHub
Releases. Only the `gti add` package registry and the reference website stay on
Gitea (`git.internal.howetotech.dev`); the earlier Gitea source repository is
frozen.

Run the test suite with:

```sh
ctest --test-dir build --parallel 4 --output-on-failure
```

Repeated local builds can use `sccache` or `ccache` through CMake's standard
compiler-launcher interface. This caches compiler outputs without changing
GTI's build graph or artifact semantics:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER_LAUNCHER=sccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=sccache
```

Test-enabled builds always use a GTI compiler executable built with host
optimization because many tests compile GTI programs repeatedly. This does
not remove the GTI `-O0` cases exercised by the test matrix.
Single-configuration builds default to `Release`; an explicit `Debug`
configuration is rejected while
`BUILD_TESTING=ON` or `GTI_BUILD_FUZZERS=ON`. Use `RelWithDebInfo` for
optimized, debuggable test runs, or create a separate `Debug` tree with both
testing and fuzzers disabled for debugger-only work. `Release`,
`RelWithDebInfo`, and `MinSizeRel` are the supported test configurations.

First-party C and C++ sources use the exact clang-format release recorded in
`.clang-format-version`. Verify or apply the repository format with:

```sh
python3 scripts/clang_format.py --check
python3 scripts/clang_format.py --write
```

Set `GTI_CLANG_FORMAT` when the pinned executable is not named
`clang-format`. The equivalent configured build targets are `format-check` and
`format`. Generated, vendored, and fixture sources are outside this formatting
scope.

Install the command-line toolchain with:

```sh
cmake --install build --prefix ~/.local
export PATH="$HOME/.local/bin:$PATH"
gti --version
```

Direct compilation remains the simplest workflow:

```sh
gti main.gti -O2 -o main
./main

# Seed a valueless flag used by #ifdef FEATURE or #if defined(FEATURE).
gti main.gti -D FEATURE -o main-with-feature
```

Optimizer inspection is explicit and does not change ordinary output:

```sh
gti main.gti -O1 \
  --dump-mir-before build/main.before.mir \
  --dump-mir-after build/main.after.mir \
  --optimization-remarks \
  --optimization-report build/main.optimization.json \
  -o main
```

GTI remarks and the versioned deterministic JSON report describe verified MIR
decisions; native Clang/GCC optimization remarks remain a separate backend
facility.

GTI defaults to single-threaded execution semantics. Use
`--execution-profile concurrent` to apply the adopted concurrent-profile
global/static checks and expose the bounded production `std::thread`, ordered
scalar `std::atomic<T>`, and scoped `std::mutex<T>` surfaces on a target with
thread support. Concurrent process-wide state must be immutable and
share-capable; synchronized interior mutation uses the atomic or mutex wrapper.
A project profile selects the same policy with
`execution-profile = "concurrent"`.

Native libraries can be supplied to direct mode after the compiler-argument
separator, for example `gti main.gti -o main -- -lfoo`. Project manifests can
declare contained C and C++ sources, link files, search paths, libraries,
frameworks, and native arguments under package, profile, or target `native`
tables. For example, `c-sources = ["native/helper.c"]` and
`cpp-sources = ["native/support.cpp"]` make `gti build` compile and link both
definitions automatically; `--cc` and `--cxx` override the selected compilers.
The native ABI and manifest-link contracts are documented in
[`docs/language/native-c-interop.md`](docs/language/native-c-interop.md); the
bounded low-level surface is specified in
[`docs/language/raw-pointers.md`](docs/language/raw-pointers.md).

Manifest-driven executable and test targets use `gti.toml`; `gti build` builds
one selected target and `gti test` runs all declared test targets or one named
test. A project can seed configuration flags with
`defines = ["FEATURE"]` under `[build]`, and a repeatable `-D NAME` on
`build`/`check`/`run`/`test` adds invocation-specific flags. The [build and
driver documentation](docs/architecture/build-and-driver.md) covers direct
options, project profiles, outputs, and current project-mode boundaries.

Local multi-package development is available without a registry or network.
A workspace root declares `[workspace].members`; packages declare source-only
dependencies such as `math = { path = "../math" }` under `[dependencies]`, and
source uses `import math.add;` for the dependency's registered `add` module. Run a command inside a member to select it or
use `--package <name>` from the workspace root. Only direct aliases are visible,
and dependency cycles or duplicate package identities fail before compilation.
Pinned git dependencies are declared as
`math = { git = "https://example.com/math.git", rev = "<full commit>" }`;
`gti fetch` acquires them and writes `gti.lock`, builds verify the recorded
checksums before loading source, and `--offline`/`--locked` keep compilation
network-free. Branch and tag selectors are deliberately rejected, and a
registry is available through `gti add libc` and version requirements such as
`libc = "^0.1.0"`. `gti fetch` prefers compatible exact lock entries and verifies
downloaded archives; registry-backed builds and editor analysis never fetch
implicitly. `gti publish --dry-run` validates a source package without uploading,
and `gti publish --name libc` publishes a Git-tracked package inventory. The
[registry documentation](docs/architecture/build-and-driver.md#source-package-registries)
describes configuration, publication, cache integrity and the bootstrap limits.

On macOS, `gti registry login` prompts for your Gitea username/password and an
optional 2FA code, creates a package-read token and saves only that token in
Keychain. Use `gti registry login --publish` for package-write access, or
`--token`/`--token-stdin` to supply an existing token;
`gti add`/`fetch`/`publish` then use it automatically. `gti registry status`
shows the source without showing the secret, and `gti registry logout` removes
the saved credential. Configured `token-env` variables override saved login
for CI; an explicitly empty value disables it. Other hosts currently use
environment tokens, without a plaintext fallback.

The language server reads this same resolved package graph without fetching or
building. Once `gti fetch` has materialized a locked Git or registry dependency, package
diagnostics, hover, definition, semantic tokens, and member completion use it;
unsaved GTI dependency buffers override the stored source for editor analysis.

Project `build`, `run`, and `test` commands use a verified local whole-program
cache under `build/gti/cache/v2`. An unchanged eligible build restores the
executable without rerunning the frontend/backend or native compiler. Declared
native C/C++ sources — including a dependency package's — declared include and
library search directories, exact archive/object link files, and `-l` names
that resolve to exactly one content-complete file inside a declared search
directory participate in the cache through compiler dependency discovery and
full-tree identity. On Apple hosts, frameworks participate when a linker-map
probe identifies and hashes the exact selected framework text stub or binary;
an unavailable or ambiguous trace still bypasses. Opaque native argument
vectors, `-l` names that resolve outside the declared search directories or
ambiguously within them, non-content-complete link inputs (thin archives,
linker scripts, shared libraries), time-and-date preprocessor macros, and
dependency-injecting environment search paths conservatively bypass. Use
`--verbose` to see the cache identity and hit/miss/bypass reason, or
`--no-cache` to bypass both lookup and publication for verification. Direct
`gti file.gti` compilation and `gti check` remain uncached.

`gti run --no-build [-- <arguments>]` launches the already-published executable
for the selected package, target, and profile without fetching dependencies,
running build scripts, compiling, or consulting the cache. It rejects missing
or unsafe managed artifacts and deliberately does not claim the executable is
fresh; use ordinary `gti run` when source changes must be reflected.

Build status and diagnostics use colour automatically in terminals. Use
`gti build --color=never` to disable it, `--color=always` to retain it in captured
output, or set `NO_COLOR=1` to disable automatic colour. Redirected output is
plain by default; JSON output and your program's output are not decorated.

`gti build --all` builds every declared target concurrently with a bounded job
count (`--jobs <count>`), deterministic name-ordered output, and byte-identical
artifacts whether built serially or in parallel.

`gti build`, `check`, `run`, and `test` accept `--target <triple>`. Omitting it
selects the build host, so existing manifests and commands are unaffected. A
non-host target resolves a declared toolchain rather than silently reusing host
compilers:

```toml
[toolchains.aarch64-unknown-linux-gnu]
cxx = "aarch64-linux-gnu-g++"        # required
cc  = "aarch64-linux-gnu-gcc"        # needed for C unless --cc supplies it
sysroot = "/opt/sysroots/aarch64"    # optional
native-target = "aarch64-linux-gnu"  # optional; emitted as --target=
```

Building for a target with no declared table fails closed. `gti check`
analyzes any supported target without one, since it invokes no native tool,
and `gti run`/`gti test` refuse a non-host target rather than modelling a
runner. Host and cross plans of one package never share a cache entry or an
output path. The installed GTI runtime archive and headers are still
discovered for the host, so a cross *link* needs a target-built runtime
supplied through `GTI_RUNTIME_LIBRARY`/`GTI_RUNTIME_INCLUDE`.

`gti build --build-report <path>` records a versioned provenance sidecar: the
resolved package, target, profile, and selected target triple, the SHA-256
content identity of every
consumed GTI source unit and toolchain material, the native compiler identity
and exact command, and the produced executable's own bytes.
`gti verify --build-report <path>` rebuilds what that record describes and
names every recorded identity the tree no longer reproduces, exiting `70` on
any divergence. A successful verification proves that this tree, with this
compiler on this host, consumed exactly the recorded inputs and produced a
byte-identical executable. It is not a hermeticity proof: implicit C++
standard-library and platform SDK headers are still treated as stable for an
unchanged native compiler identity.

Create a new manifest-driven executable package with:

```sh
gti new hello
cd hello
gti run
```

Declare an independent whole-program test with `kind = "test"`, then run it
with `gti test` (or `gti test <name>`). Runtime failures are reported per target
without preventing later tests from running.

On macOS and Linux, `gti install` builds an executable target with the
`release` profile and copies it into `~/.local/bin`, beside the toolchain, so
you can run it by name. `gti uninstall <name>` removes it, and
`gti install --list` shows what is installed. Use `--root <dir>` or
`GTI_INSTALL_ROOT` for another location and `--path <dir>` to install a project
other than the current one. Install only replaces or removes programs it
installed; `--force` replaces anything else at the destination:

```sh
gti install
hello
gti uninstall hello
```

Use `gti init` to initialize an existing directory. It preserves an existing
`src/main.gti` and refuses to replace an existing `gti.toml`; `--name <name>`
overrides the package name derived from the directory. Add `--git` to create a
standalone Git repository and a canonical `.gitignore` containing `/build/`:

```sh
gti init --git
gti init path/to/project --name project --git
```

The option refuses to modify an existing `.git` entry or `.gitignore`.
Repository-local Git environment overrides such as `GIT_DIR` or
`GIT_WORK_TREE` must be unset, so initialization cannot target another repository.

Create an editable formatter configuration in the current directory with
`gti format init`, or pass an existing destination directory explicitly. The
command refuses to replace an existing `.gti-format`:

```sh
gti format init
gti format init path/to/project
```

Generate GTI bindings for an existing C header with `gti bindgen`; the
bounded importable surface and the skip report are described in
[docs/language/native-c-interop.md](docs/language/native-c-interop.md):

```sh
gti bindgen vendor/engine.h -o src/engine_bindings.gti --namespace engine
gti bindgen vendor/engine.h --target x86_64-pc-windows-msvc
```

The host target is used by default. Select `--target` when generating for a
different supported 64-bit target so C implementation-defined widths such as
`long` use the correct LP64 or LLP64 model.

## Install and select GTI with gtiup

`gtiup` manages matching compiler, LSP, standard library, runtime and parser
versions on macOS arm64 and Linux x64. It is a standalone C++ binary with its
own [repository, CI and downloads](https://github.com/lang-gti-official/gtiup),
independent of compiler builds. Install it without Python or a GTI checkout:

```sh
curl --proto '=https' --tlsv1.2 -fsSL \
  https://raw.githubusercontent.com/lang-gti-official/gtiup/main/install.sh \
  -o /tmp/gtiup-install.sh
sh /tmp/gtiup-install.sh
export PATH="$HOME/.gtiup/bin:$PATH" # also add this to your shell configuration
# Choose explicitly; this downloads the selected complete toolchain.
gtiup default nightly
```

The bootstrap installs only the manager and command proxies; it does not edit
shell files or replace existing commands. For a custom location set `GTIUP_HOME`
before bootstrap and keep it set when using that installation.

```sh
gtiup install preview       # combined features, before main's full review/CI
gtiup install beta          # newest published beta
gtiup install stable        # newest published stable
gtiup update nightly       # replace this channel's selected build atomically
gtiup list
gtiup show
gti +preview --version
gtiup run nightly gti_lsp --version
gtiup which gti             # real executable, useful to editor integrations
```

A channel must have a published toolchain before it can be installed. There is
no fallback to another channel if beta/stable/preview is not yet published.
`gtiup install 0.12.0` pins a published stable version; a beta can be pinned as
`0.12.0-beta.1`. Put a channel or version on one line in a project's
`gti-toolchain` file to select it for that directory and its descendants.
Selection precedence is command `+channel` (or `gtiup run`),
`GTIUP_TOOLCHAIN`, the nearest `gti-toolchain`, then the user default.
Commands never download implicitly; install the selected channel first.

Preview downloads come from the rolling `preview` prerelease. Each includes the
exact combined commit and feature inclusion manifest; preview is separate from
nightly's successful full-integration requirement. Published archive checksums
and embedded source/version identity are checked before activation. A failed
or interrupted update leaves the previous selection intact. Stable yanks and
beta withdrawals are checked on install/update; offline invocation uses the
already installed version. These downloads use HTTPS and checksum verification;
`gtiup` does not independently verify GitHub attestations.

`gtiup uninstall <channel-or-version>` removes that selection (choose another
default first). Immutable directories are retained so running compiler/LSP
processes can still access their resources. `gtiup gc` removes unreferenced
managed directories when you explicitly request it; close running GTI/LSP
processes before garbage collection. Existing developer builds and Neovim
configurations are not modified.

## Neovim and LazyVim

Use `gtiup` to install and select the toolchain, and the Lazy plugin to configure
Neovim. The language server shares the compiler's frontend and standard library;
`gtiup` installs the compiler, LSP, runtime, standard library and Tree-sitter
parser together. Neovim does not need a separate LSP installer.

After installing `gtiup` and a toolchain, use:

```lua
return {
  {
    "https://github.com/lang-gti-official/gti.git",
    name = "gti",
    version = false,
    build = false,
    init = function()
      vim.g.gti_toolchain = "gtiup"
    end,
  },
}
```

Keep `gtiup` on Neovim's `PATH`, or set `GTIUP_PATH` to its executable. Managed
mode resolves `gtiup which gti_lsp` from each LSP project's root, so the manager's
`GTIUP_TOOLCHAIN`, ancestor `gti-toolchain` file and user default selection apply.
It never installs a missing channel automatically or falls back to an old
plugin-local compiler. `:GTIInfo` reports the selected compiler, LSP and parser.
Restart the LSP after changing a project's selection. Existing `GTI_PATH`,
`GTI_LSP_PATH` and `GTI_TREE_SITTER_PATH` overrides remain authoritative; remove
old overrides when migrating if they should no longer pin a developer build.

Old `toolchain/` installations are preserved but ignored in managed mode; the
plugin no longer prepends their binaries to `PATH`. The editor can launch
separate LSP versions for separate projects. Neovim loads one GTI Tree-sitter
grammar per process: a project selecting a different parser uses regex syntax
and asks you to open it in a separate Neovim instance. Query incompatibility
also falls back to regex syntax, while compiler-backed LSP features remain
available.

The legacy self-installing Lazy configuration remains available for existing
setups. Without managed mode it downloads the matching released compiler,
language server, standard library, runtime, and native Tree-sitter parser:

```lua
return {
  {
    "https://github.com/lang-gti-official/gti.git",
    name = "gti",
    version = "*",
  },
}
```

Run `:Lazy sync`, restart Neovim, and open a `.gti` file. Mason and
`nvim-lspconfig` are not required. `:GTIInfo` reports the active plugin,
toolchain, LSP, and parser versions.

Code completion also offers snippets: type `cl` for a class skeleton, `ct` or
`dt` inside a class for its constructor or destructor, and `method`/`mutmethod`
for read-only/mutable methods. Structs, generic classes, interfaces, enums,
namespaces, aliases, fields, functions and common control-flow templates are
included. Select the snippet completion, then use your completion frontend's
tab-stop navigation to edit its placeholders; class names are linked to their
constructor names. The completion client must advertise LSP snippet support and
provide a snippet expansion engine. Ordinary symbol completion still works
without that capability. These are editor templates, not additional GTI syntax.

The editor normally analyses the project's `dev` profile. Use `:GTIProfile release`
or `:GTIProfile dev` to switch without rebuilding or restarting the server;
`:GTIProfile!` restores the project default. This updates profile defines,
diagnostics, completion and inactive-code highlighting together. The choice is
independent of the last `gti build` invocation. For a persistent selection:

```lua
vim.lsp.config("gti_lsp", {
  settings = { gti = { profile = "release" } },
})
```

Other clients can use the same `gti.profile` setting or
`initializationOptions = { profile = "release" }`. Inactive directive branches
use a grey comment-style default; themes can override
`@lsp.mod.inactiveCode.gti`.

Use the complete `https://` URL, and write `version = "*"` without a
backslash. The plugin downloads toolchains from GitHub Releases: a daily
`nightly` for Linux x64 and Apple Silicon (see
[GitHub nightly publication](docs/architecture/build-and-driver.md#github-nightly-publication)).
Select it before the spec loads:

```lua
vim.g.gti_channel = "nightly"
```

The default stable channel needs a published `v<VERSION>` release matching the
checkout. Stable releases are not yet published on GitHub, so a stable install
stops with an error naming the missing tag and the nightly channel; it never
installs a different version. The selected tag must have a matching toolchain
archive; syncing Git tags does not create or copy release binaries. A tag cut
before the switch to GitHub Releases runs its own older installer, which
downloads from the frozen Gitea releases. Otherwise build locally and set
`GTI_PATH` and `GTI_LSP_PATH` to those executables (and `GTI_TREE_SITTER_PATH`
to the built parser).
When supplying your own toolchain, set `build = false` in the Lazy spec to
disable its release-download hook.

### Local macOS installation without a release runner

From this checkout, run:

```sh
python3 scripts/install_neovim_local.py --dry-run --replace-config --replace-bin
python3 scripts/install_neovim_local.py --replace-config --replace-bin
```

These commands select early combined preview by default, once `preview` has been
published. `--preview` remains an explicit alias. To build this checkout instead:

```sh
python3 scripts/install_neovim_local.py --local --dry-run
python3 scripts/install_neovim_local.py --local --replace-config --replace-bin
```

Preview may contain PRs still awaiting full validation and review. The script
pins the canonical preview commit in an isolated, persistent source checkout
and installs its compiler, LSP and parser together. It reports the source
commit and preserves your current checkout, including uncommitted edits.
Preview dry run only resolves/reports the source; it performs no fetch, build
or destination inspection. A missing preview fails without installing anything;
there is no silent fallback. Use `--local` to return to this checkout’s toolchain,
including with `--activate-only`. The
[preview workflow](docs/architecture/agent-workflow.md#early-combined-preview)
describes admission checks and the automated examples/platform coverage.

Requires Python 3.10+, Neovim 0.11+, CMake, Ninja, a supported installed LLVM
(including libclang), and the Xcode command-line tools. It discovers Homebrew
LLVM 18 or newer (also checking retained Homebrew versions) or accepts
`--llvm-dir /path/to/lib/cmake/llvm`; it installs no prerequisites
automatically. Builds use all detected CPUs at normal priority by default
(`--jobs N` to limit parallelism).

After a Homebrew upgrade, rerun the script to reconfigure and rebuild against
the selected LLVM and matching Clang package. An existing build keeps its
generator; moving Homebrew aliases are resolved again rather than retaining
stale dependency paths. New LLVM majors are not rejected by an arbitrary
ceiling, but must still satisfy GTI's API, RTTI, and link-surface requirements.

The script builds compiler/LSP/parser, stages the complete `gti_toolchain`
component, verifies a checksummed local archive with the Neovim plugin smoke
test, then activates it with the same installer used for downloaded releases.
It installs `gti` and `gti_lsp` symlinks in `~/.local/bin` to the complete
toolchain. Existing different commands require `--replace-bin` and are saved
beside the originals before replacement. Use `--bin-dir` for another command
directory or `--no-terminal` for editor-only setup. The script reports when
PATH selects another compiler; it does not edit shell configuration. Add the
command directory to PATH if needed, and run `rehash` in existing zsh sessions.
It writes `lua/plugins/gti.lua` under Neovim's active config directory, pointing
Lazy at this checkout with downloads disabled. An existing different config
requires `--replace-config` and is backed up beside the original. Use `--config`
for another Lazy spec location. It never edits `init.lua` or installs Lazy;
your configuration must already import that plugins directory.

If a Homebrew upgrade removed the supported LLVM build prerequisites but the
portable toolchain is already installed, restore terminal/editor activation
without rebuilding:

```sh
python3 scripts/install_neovim_local.py --activate-only --replace-config --replace-bin
```

This validates and activates the installed version; it does not rebuild it to
the checkout's VERSION. For a new build, install supported LLVM (for example
`brew install llvm`) and rerun the normal installer. LLVM 23.1.2 is included in
the current system-build compatibility evidence; official release archives
continue to use the pinned bundled version.

Restart Neovim, run `:Lazy sync`, open a `.gti` file, and check `:GTIInfo`.
Re-run the script after pulling changes. This system-LLVM build uses the same
editor layout but is not an official release. Installation bundles matching
non-Apple runtime libraries, their licenses, and loader-relative references,
so the installed compiler and LSP survive Homebrew upgrades or cleanup. The
toolchain can be relocated on a compatible Mac with the same architecture and
macOS requirements; this does not create a universal binary or change its
deployment target. Keep supported LLVM installed to rebuild and use the system-
LLVM developer SDK. `--build-dir` may reuse a Release/LSP-enabled build
from this checkout without switching its CMake generator.

See the [LSP architecture](docs/architecture/lsp.md) for the implemented
semantic, formatting, snapshot, and protocol boundaries.

## Documentation

- The [documentation map](docs/index.md) indexes the language, architecture,
  decisions, and active implementation plans.
- [`examples/`](examples/README.md) contains numbered programs for implemented
  features.
- [`examples/gti-vs-cpp/`](examples/gti-vs-cpp/README.md) contains paired,
  machine-verifiable comparisons with C++.
- [`docs/language/tcp.md`](docs/language/tcp.md) specifies the first move-only TCP socket owner
  and its deliberately unconnected POSIX boundary.
- [`docs/index.md`](docs/index.md) maps current compiler architecture, language
  semantics, design decisions, and clearly separated future plans.
- [`docs/language/`](docs/language/index.md) is the backend-independent working
  language specification and intended path to a 1.0 compatibility boundary.

The Wiki documents released user behavior. Repository proposals are design
inputs, not promises that a feature has shipped.

## Repository map

Optional native bindings for GLFW, GLAD, SQLite3, cimgui, zlib, libpng, Assimp and libgit2 live
under [`packages/`](packages/README.md). Prepare the pinned sources with
`python3 packages/prepare.py`, then use them as GTI path dependencies. They are
separate from the standard library and require no compiler-specific handling.

| Path | Responsibility |
| --- | --- |
| `include/gti/`, `src/compiler/` | compiler frontend, semantic model, HIR, MIR, optimization, and backend |
| `include/gti/driver/`, `src/driver/` | reusable compilation, project, artifact, and native toolchain driver |
| `src/cli/`, `src/lsp/` | command-line and language-server entry points |
| `stdlib/`, `runtime/` | public GTI library source, private capabilities, and narrow host runtime |
| `packages/` | optional native-library bindings, pinned source preparation and package consumers |
| `tree-sitter-gti/`, `queries/gti/` | parser and Neovim structural queries |
| `tests/`, `examples/` | verification and user programs |
| `docs/` | compiler architecture, language specification, decisions, plans, and agent routing |

## Project status and direction

The current engineering direction is maintained in the
[`systems-readiness roadmap`](docs/plans/roadmap-to-1.0.md). The
[language restriction ledger](docs/plans/language-alignment.md) records current
limitations separately from that outcome roadmap.

Development now prioritizes coherent user-facing systems capabilities over
expanding restriction machinery in isolation. The `1.0` label is a soft,
revisable readiness goal: GTI should use it only when the language is
full-featured enough for serious systems programming, not as a feature cutoff
that pushes essential work into an automatic later bucket. The existing
ownership, safety, semantic-authority, and backend-independence work remains
the foundation for those outcomes.

The project began as a personal compiler-learning experiment and was extended
substantially with Codex.

## License

GTI is distributed under the [MIT License](LICENSE).
