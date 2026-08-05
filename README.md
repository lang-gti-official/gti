# GTI

<p align="center">
  <img src="assets/branding/gti-icon.png" alt="GTI language icon" width="160">
</p>

`gti` implements a small source-to-C++ compiler pipeline:

```text
source loading -> lexer -> parser/AST -> semantic analysis -> C++ emitter
```

The implemented source language supports `int`, `float`, `bool`, `string`,
`expected<T, E>`, user-defined types, variables, functions, classes, blocks,
`if`/`else`, `while`, `for`, `return`, namespaces, namespace aliases,
qualified names, calls, member access, assignments, and the expression operators
documented in `docs/grammar.ebnf`.

Namespaces use C++-style qualification and can be nested or aliased:

```cpp
namespace engine {
namespace graphics {
void render() {}
}
}

namespace gfx = engine::graphics;

int main() {
  engine::graphics::render();
  gfx::render();
  return 0;
}
```

Source files can depend on other GTI files with a top-level include directive:

```cpp
include "math.gti"
```

The path is resolved relative to the including file and must name a `.gti`
file. Paths are canonicalized, each source file is loaded once, and dependency
cycles are rejected. This is an early source-loading phase, not C++ textual
inclusion: it does not provide macros, conditional preprocessing, or repeated
copy-and-paste expansion. A trailing semicolon is accepted but not required.

`print` is not a keyword or a built-in statement. It remains an ordinary
identifier; output is provided by standard-library functions without coupling
I/O behavior to the parser or C++ backend.

The automatically loaded GTI standard library now provides string output:

```cpp
std::print("without newline");
std::println("with newline");
```

These are ordinary GTI functions. Their final byte write uses the
`stdout.write` runtime binding and the C ABI implemented under `runtime/`; the
compiler does not recognize `print` as syntax.

## Repository layout

- `include/gti/` contains the reusable compiler frontend, AST, analysis, and
  backend interfaces.
- `src/cli/` and `src/lsp/` contain the two executable entry points.
- `tests/` contains compiler, CLI, and LSP tests.
- `examples/` contains GTI source programs.
- `docs/` contains the language grammar.
- `stdlib/` contains ordinary GTI library functions and runtime declarations.
- `runtime/` contains the narrow C ABI used for host-platform operations.
- `vendor/` contains pinned compatibility code required by older C++ targets.
- `editor/` contains LazyVim and Neovim integration.

Variables and parameters are immutable by default and lower to `const` C++.
Use `mut` only for bindings that need to change:

```cpp
int fixedValue = 1;       // const int fixedValue = 1;
mut int frameCount = 0;  // int frameCount = 0;
```

Non-`void` function results must also be used by default. Store, pass, return,
or use the result in another expression. When ignoring a result is deliberate,
mark that call site explicitly:

```cpp
[[discard]] calculate_unused_value();
```

`[[discard]]` is valid only on a non-`void` function call. GTI removes the
attribute when lowering to C++; the rule is enforced during semantic analysis.

## Recoverable errors

GTI uses the built-in `expected<T, E>` type for recoverable errors without
language-level exceptions or implicit propagation syntax:

```cpp
expected<int, string> load(bool fail) {
  if (fail) {
    return unexpected("load failed");
  }
  return 42;
}

int main() {
  expected<int, string> result = load(false);
  if (!result) {
    std::println(result.error());
    return 1;
  }
  return result.value() - 42;
}
```

`return value;` constructs success, `return unexpected(error);` constructs an
error, and bare `return;` constructs success for `expected<void, E>`. See
`docs/expected.md` for the supported observer surface.

Build the compiler and compile the sample into a native executable:

```sh
cmake -S . -B build
cmake --build build
./build/gti examples/lang_test.gti -o lang_test
./lang_test
```

Run the compiler tests with `ctest --test-dir build --output-on-failure`.

The output path defaults to the source filename without `.gti`, so this also
builds `lang_test`:

```sh
./build/gti examples/lang_test.gti
```

Useful CLI options:

```sh
# Inspect the generated C++ without compiling it.
gti main.gti --emit-cpp -o main.cpp

# Build an executable and retain main.gti.cpp beside it.
gti main.gti -o main --keep-cpp

# Select a compiler and show the command being run.
gti main.gti -o main --cxx clang++ --verbose

# Target the vendored expected compatibility implementation instead of C++23.
gti main.gti -o main --std c++20

# Forward include, optimization, and linker flags to the C++ compiler.
gti main.gti -o main -- -Iengine/include -O2 -Lengine/lib -lengine
```

Generated programs target C++23 by default. Pass `--std c++20` to use the
vendored `nonstd::expected` implementation. `GTI_CXX` and then `CXX` are used
when `--cxx` is omitted. Install the compiler, LSP, standard-library prelude,
runtime headers, compatibility headers, and static runtime library with:

```sh
cmake --install build --prefix ~/.local
```

Installed resources are discovered relative to the `gti` executable. Custom
layouts can set `GTI_STDLIB_PATH`, `GTI_RUNTIME_INCLUDE`,
`GTI_RUNTIME_LIBRARY`, and `GTI_VENDOR_INCLUDE` explicitly.

## LazyVim and LSP

The `gti_lsp` target provides parser and semantic diagnostics plus semantic
highlighting over the Language Server Protocol. It uses `json-c`, discovered
through `pkg-config`, and is built by default with the compiler.

When the config below is symlinked, it automatically finds `build/gti_lsp`.
For another build directory, make the server discoverable through `PATH` or:

```sh
export GTI_LSP_PATH="$PWD/build/gti_lsp"
```

Link the LazyVim client configuration and the syntax runtime files into your
Neovim configuration. Run these commands from the GTI repository root
(the `test` command prevents creating dangling links from another directory):

```sh
test -f "$PWD/editor/lazyvim/gti.lua" || { echo "Run this from the GTI repository root"; exit 1; }
mkdir -p ~/.config/nvim/lua/plugins
mkdir -p ~/.config/nvim/ftdetect ~/.config/nvim/ftplugin ~/.config/nvim/syntax
ln -sf "$PWD/editor/lazyvim/gti.lua" ~/.config/nvim/lua/plugins/gti.lua
ln -sf "$PWD/editor/nvim/ftdetect/gti.vim" ~/.config/nvim/ftdetect/gti.vim
ln -sf "$PWD/editor/nvim/ftplugin/gti.vim" ~/.config/nvim/ftplugin/gti.vim
ln -sf "$PWD/editor/nvim/syntax/gti.vim" ~/.config/nvim/syntax/gti.vim
```

Restart Neovim and open any `.gti` file. `:LspInfo` should show `gti_lsp`
attached. The syntax file supplies immediate keyword, literal, operator, string,
and comment highlighting; semantic tokens refine identifiers while the server
is attached. The same LazyVim configuration registers `.gti` with a blue
C++-style icon in `mini.icons`, which is used by Neo-tree, Telescope, and other
LazyVim interfaces. Terminal icons are Nerd Font glyphs rather than image
files, so the terminal must use a Nerd Font 3.0 or newer. The full project icon
is available at `assets/branding/gti-icon.png`.

The compiler deliberately keeps parsing, semantic analysis, and C++ emission
as separate visitors/passes. The next semantic layer should add nominal type
identities, class member tables, and function signatures/overload resolution
before engine APIs are exposed to the language.
