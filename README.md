# GTI

<p align="center">
  <a href="https://git.internal.howetotech.dev/gti-lang/gti/actions/workflows/ci.yml"><img src="https://git.internal.howetotech.dev/gti-lang/gti/actions/workflows/ci.yml/badge.svg?branch=main" alt="CI status"></a>
  <a href="https://git.internal.howetotech.dev/gti-lang/gti/actions/workflows/release.yml"><img src="https://git.internal.howetotech.dev/gti-lang/gti/actions/workflows/release.yml/badge.svg?branch=main" alt="Release status"></a>
  <a href="https://git.internal.howetotech.dev/gti-lang/gti/src/branch/main/LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="MIT license"></a>
</p>

<p align="center">
  <img src="assets/branding/gti-icon.png" alt="GTI language icon" width="160">
</p>

GTI is an experimental, statically typed language with C++-familiar syntax. It
keeps explicit control, value semantics, deterministic cleanup, native code
generation, and predictable performance while giving ownership, conversions,
failure, and source visibility smaller rules.

The released compiler checks a complete GTI source graph, builds typed HIR and
MIR, lowers it through a C++ backend, and invokes a native C++ compiler:

```text
source graph -> frontend -> checked AST -> typed HIR/MIR
             -> GTI optimization -> C++ backend -> native compiler
```

> [!IMPORTANT]
> GTI is a personal, AI-assisted, pre-1.0 language project. It is usable for
> experiments, but a documented 0.x minor release may still change draft
> language or standard-library meaning. Patch releases do not intentionally
> break source. The 1.0 scope remains a soft systems-readiness goal until that
> release is published; GTI 1.0 then freezes Edition 1 under the published
> [compatibility policy](docs/decisions/011-language-compatibility-and-editions.md).

## A small example

```gti
#include <std/string>

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

- Explicit fixed-width integer types, checked conversions and indexing, and
  immutable-by-default bindings, plus explicit wrapping, saturating, and
  checked-result add/subtract/multiply operations, direct base-10
  `std::print`/`std::println`, owning `std::to_string` conversion, and
  recoverable sequential heterogeneous formatting for booleans, characters,
  integers, binary32 floats, and owned or borrowed text through `<std/format>`.
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
- Independent source units, load-once `#include`, namespaces, aliases, and
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
18 through 22 **built with RTTI enabled** (distribution packages such as
Debian `llvm-*-dev` and Homebrew `llvm@*` are; upstream LLVM defaults to off).
CMake uses a compatible system LLVM by default and reports an RTTI mismatch
at configure time. `-DGTI_BUNDLE_LLVM=ON` instead downloads and builds the
pinned LLVM release used for self-contained toolchains, with RTTI enabled
automatically; that mode also requires an explicit `-DCMAKE_BUILD_TYPE`,
which LLVM's own build insists on. Release builds force that acquisition
mode. These are two ways to supply one mandatory dependency, not separate
LLVM and non-LLVM compiler implementations. GTI links only the narrowly
approved support libraries (`LLVMSupport`, `LLVMTargetParser`, and
`LLVMDemangle`); LLVM is not GTI's code-generation backend. See
[ADR 006](docs/decisions/006-llvm-support-adoption.md) for the boundary.

```sh
git clone https://git.internal.howetotech.dev/gti-lang/gti.git gti
cd gti
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4

./build/gti examples/01-basics.gti -o /tmp/gti-basics
/tmp/gti-basics

# Emit declarations for a C or C++ native shim.
./build/gti examples/49-native-bridge-header.gti \
  --emit-native-header -o /tmp/gti_native.h

# See the nominal opaque-handle wrapper boundary.
./build/gti examples/50-opaque-native-handles.gti \
  -o /tmp/gti-opaque-handles
```

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
source uses `#include <math/add>`. Run a command inside a member to select it or
use `--package <name>` from the workspace root. Only direct aliases are visible,
and dependency cycles or duplicate package identities fail before compilation.
Pinned git dependencies are declared as
`math = { git = "https://example.com/math.git", rev = "<full commit>" }`;
`gti fetch` acquires them and writes `gti.lock`, builds verify the recorded
checksums before loading source, and `--offline`/`--locked` keep compilation
network-free. Branch and tag selectors are deliberately rejected, and a
registry remains a later phase.

The language server reads this same resolved package graph without fetching or
building. Once `gti fetch` has materialized a locked Git dependency, package
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

Use `gti init` to initialize an existing directory. It preserves an existing
`src/main.gti` and refuses to replace an existing `gti.toml`; `--name <name>`
overrides the package name derived from the directory.

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

## Neovim and LazyVim

GTI is a standard Lazy plugin. It downloads the matching released compiler,
language server, standard library, runtime, and native Tree-sitter parser:

```lua
return {
  {
    "https://git.internal.howetotech.dev/gti-lang/gti.git",
    name = "gti",
    version = "*",
  },
}
```

Run `:Lazy sync`, restart Neovim, and open a `.gti` file. Mason and
`nvim-lspconfig` are not required. `:GTIInfo` reports the active plugin,
toolchain, LSP, and parser versions.

Use the complete `https://` URL for Gitea, and write `version = "*"` without
a backslash. The selected tag must have a matching toolchain archive under
Gitea Releases; syncing Git tags does not create or copy release binaries.
Older tags from before the hostname migration still contain the old installer.
The current Linux-only runner cannot publish macOS binaries. On macOS, build
locally and set `GTI_PATH` and `GTI_LSP_PATH` to those executables (and
`GTI_TREE_SITTER_PATH` to the built parser), or provision a native macOS runner.
When supplying your own toolchain, set `build = false` in the Lazy spec to
disable its release-download hook.

### Local macOS installation without a release runner

From this checkout, run:

```sh
python3 scripts/install_neovim_local.py --dry-run --replace-config
python3 scripts/install_neovim_local.py --replace-config
```

Requires Python 3.10+, Neovim 0.11+, CMake, Ninja, a supported installed LLVM
(including libclang), and the Xcode command-line tools. It discovers Homebrew
LLVM or accepts `--llvm-dir /path/to/lib/cmake/llvm`; it installs no prerequisites
automatically. Builds use all detected CPUs at normal priority by default
(`--jobs N` to limit parallelism).

The script builds compiler/LSP/parser, stages the complete `gti_toolchain`
component, verifies a checksummed local archive with the Neovim plugin smoke
test, then activates it with the same installer used for downloaded releases.
It writes `lua/plugins/gti.lua` under Neovim's active config directory, pointing
Lazy at this checkout with downloads disabled. An existing different config
requires `--replace-config` and is backed up beside the original. Use `--config`
for another Lazy spec location. It never edits `init.lua` or installs Lazy;
your configuration must already import that plugins directory.

Restart Neovim, run `:Lazy sync`, open a `.gti` file, and check `:GTIInfo`.
Re-run the script after pulling changes. This system-LLVM build uses the same
editor layout but is local to this Mac, not a portable official release. Keep
its Homebrew LLVM installed. `--build-dir` may reuse a Release/LSP-enabled build
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
