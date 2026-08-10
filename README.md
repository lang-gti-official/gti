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
> experiments, but its language and standard library may still make breaking
> changes between releases.

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
  immutable-by-default bindings.
- Classes, structs, interfaces, public inheritance, virtual dispatch, explicit
  construction, and deterministic cleanup.
- Fixed arrays and read-only structured bindings over arrays and flat public
  class or struct fields.
- Non-null read-only and mutable borrows, consumed moves, move-only values, and
  nominal unique ownership without public raw pointers.
- Exact overloads, named and value generics, source-defined unary concepts,
  confined variadic forwarding, and non-escaping callable parameters.
- Independent source units, load-once `#include`, namespaces, aliases, and
  target conditionals without textual preprocessing.
- Bounded `extern "C"` declarations for exact native symbols using fixed-width
  scalars and non-retained counted text inputs.
- Structured, target-selected native inputs in project manifests and a small
  move-only POSIX `std::tcp::socket` ownership wrapper.
- A source-defined standard-library foundation with checked array, string, and
  move-only vector owners, plus project manifests, an LSP, formatter,
  Tree-sitter parser, and self-installing Neovim/Lazy plugin.

GTI deliberately does not import C++ overload ranking, implicit user
conversions, ADL, SFINAE, textual macros, pointer decay, unchecked array access,
or implicit `switch` fallthrough.

## Build and try it

Requirements are CMake 3.20 or newer and a suitable C++ compiler.

```sh
git clone https://github.com/LukeHHA/gti.git
cd gti
cmake -S . -B build
cmake --build build

./build/gti examples/01-basics.gti -o /tmp/gti-basics
/tmp/gti-basics
```

Run the test suite with:

```sh
ctest --test-dir build --output-on-failure
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
```

Native C libraries can be supplied to direct mode after the compiler-argument
separator, for example `gti main.gti -o main -- -lfoo`. Project manifests can
declare contained link files, search paths, libraries, frameworks, and native
arguments under package, profile, or target `native` tables. The exact safe ABI
and manifest-link contracts are documented in
[`docs/native-c-interop.md`](docs/native-c-interop.md).

Manifest-driven executable projects use `gti.toml` and `gti build`. The
[Build System and CLI](https://github.com/LukeHHA/gti/wiki/Build-System-and-CLI)
manual page documents direct options, project profiles, outputs, and current
project-mode boundaries.

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
- [`docs/tcp.md`](docs/tcp.md) specifies the first move-only TCP socket owner
  and its deliberately unconnected POSIX boundary.
- [`docs/`](docs/README.md) contains internal compiler contracts, architecture
  records, and proposals. These files may describe unimplemented work.
- [`spec/`](spec/README.md) is the backend-independent working language
  specification and the intended path to a 1.0 compatibility boundary.

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
| `docs/`, `spec/` | internal design record and working language specification |

## Project status and direction

The dependency-ordered path toward a robust standard library and stable 1.0
release is summarized on the
[Wiki roadmap](https://github.com/LukeHHA/gti/wiki/Current-Limitations-and-Roadmap).
The detailed engineering plan remains in
[`docs/roadmap-to-1.0.md`](docs/roadmap-to-1.0.md).

The project began as a personal compiler-learning experiment and was extended
substantially with Codex. The longer context is preserved in
[Project Background](https://github.com/LukeHHA/gti/wiki/Project-Background).

## License

GTI is distributed under the [MIT License](LICENSE).
