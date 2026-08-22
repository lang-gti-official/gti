# GTI

<p align="center">
  <a href="https://github.com/LukeHHA/gti/actions/workflows/ci.yml"><img src="https://github.com/LukeHHA/gti/actions/workflows/ci.yml/badge.svg" alt="CI status"></a>
  <a href="https://github.com/LukeHHA/gti/actions/workflows/release.yml"><img src="https://github.com/LukeHHA/gti/actions/workflows/release.yml/badge.svg" alt="Release status"></a>
  <a href="https://github.com/LukeHHA/gti/releases/latest"><img src="https://img.shields.io/github/v/release/LukeHHA/gti?sort=semver" alt="Latest release"></a>
  <a href="https://github.com/LukeHHA/gti/blob/main/LICENSE"><img src="https://img.shields.io/github/license/LukeHHA/gti" alt="MIT license"></a>
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

- Explicit fixed-width integer types, checked conversions and indexing, and
  immutable-by-default bindings, plus explicit wrapping, saturating, and
  checked-result add/subtract/multiply operations, direct base-10
  `std::print`/`std::println`, owning `std::to_string` conversion, and
  recoverable sequential integral formatting through `<std/format>`.
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

Requirements are CMake 3.20 or newer, a suitable C++ compiler, and LLVM 18
through 22 **built with RTTI enabled** (distribution packages such as Debian
`llvm-*-dev` and Homebrew `llvm@*` are; upstream LLVM defaults to off).
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
git clone https://github.com/LukeHHA/gti.git
cd gti
cmake -S . -B build
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
cmake -S . -B build \
  -DCMAKE_C_COMPILER_LAUNCHER=sccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=sccache
```

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

GTI defaults to single-threaded execution semantics. Use
`--execution-profile concurrent` to apply the adopted concurrent-profile
global/static checks; this requires immutable share-capable process-wide state
but does not yet expose threads or atomics. A project profile selects the same
policy with `execution-profile = "concurrent"`.

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
`build`/`check`/`run`/`test` adds invocation-specific flags. The [Build System and
CLI](https://github.com/LukeHHA/gti/wiki/Build-System-and-CLI) manual page
documents direct options, project profiles, outputs, and current project-mode
boundaries.

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

Project `build`, `run`, and `test` commands use a verified local whole-program
cache under `build/gti/cache/v2`. An unchanged eligible build restores the
executable without rerunning the frontend/backend or native compiler. Declared
native C/C++ sources, declared include directories, and exact archive/object
link files participate in the cache through compiler dependency discovery;
opaque native argument vectors, name-resolved libraries and frameworks,
library search directories, non-content-complete link inputs (thin archives,
linker scripts, shared libraries), time-and-date preprocessor macros, and
dependency-injecting environment search paths conservatively bypass. Use
`--verbose` to see the cache identity and hit/miss/bypass reason, or
`--no-cache` to bypass both lookup and publication for verification. Direct
`gti file.gti` compilation and `gti check` remain uncached.

`gti build --all` builds every declared target concurrently with a bounded job
count (`--jobs <count>`), deterministic name-ordered output, and byte-identical
artifacts whether built serially or in parallel.

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

## Neovim and LazyVim

GTI is a standard Lazy plugin. It downloads the matching released compiler,
language server, standard library, runtime, and native Tree-sitter parser:

```lua
return {
  { "LukeHHA/gti", version = "*" },
}
```

Run `:Lazy sync`, restart Neovim, and open a `.gti` file. Mason and
`nvim-lspconfig` are not required. `:GTIInfo` reports the active plugin,
toolchain, LSP, and parser versions.

See [Neovim and LSP](https://github.com/LukeHHA/gti/wiki/Neovim-and-LSP) for
installation details, semantic features, formatting, Tree-sitter,
rainbow-delimiters support, external toolchains, and troubleshooting.

## Documentation

- The [GTI Wiki](https://github.com/LukeHHA/gti/wiki) is the user-facing
  language and toolchain manual.
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

| Path | Responsibility |
| --- | --- |
| `include/gti/`, `src/compiler/` | compiler frontend, semantic model, HIR, MIR, optimization, and backend |
| `include/gti/driver/`, `src/driver/` | reusable compilation, project, artifact, and native toolchain driver |
| `src/cli/`, `src/lsp/` | command-line and language-server entry points |
| `stdlib/`, `runtime/` | public GTI library source, private capabilities, and narrow host runtime |
| `tree-sitter-gti/`, `queries/gti/` | parser and Neovim structural queries |
| `tests/`, `examples/` | verification and user programs |
| `docs/` | compiler architecture, language specification, decisions, plans, and agent routing |

## Project status and direction

The current engineering direction is maintained in the
[`systems-readiness roadmap`](docs/plans/roadmap-to-1.0.md). The
[Wiki roadmap](https://github.com/LukeHHA/gti/wiki/Current-Limitations-and-Roadmap)
provides a user-facing summary of released limitations and may be updated on a
different cadence.

Development now prioritizes coherent user-facing systems capabilities over
expanding restriction machinery in isolation. The `1.0` label is a soft,
revisable readiness goal: GTI should use it only when the language is
full-featured enough for serious systems programming, not as a feature cutoff
that pushes essential work into an automatic later bucket. The existing
ownership, safety, semantic-authority, and backend-independence work remains
the foundation for those outcomes.

The project began as a personal compiler-learning experiment and was extended
substantially with Codex. The longer context is preserved in
[Project Background](https://github.com/LukeHHA/gti/wiki/Project-Background).

## License

GTI is distributed under the [MIT License](LICENSE).
