# GTI

`gti` implements a small source-to-C++ compiler pipeline:

```text
source loading -> lexer -> parser/AST -> semantic analysis -> C++ emitter
```

The implemented source language supports `int`, `float`, `bool`, `string`,
user-defined types, variables, functions, classes, blocks, `if`/`else`, `while`, `for`,
`return`, namespaces, namespace aliases, qualified names, calls, member access,
assignments, and the expression operators
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
- `editor/` contains LazyVim and Neovim integration.

Variables and parameters are immutable by default and lower to `const` C++.
Use `mut` only for bindings that need to change:

```cpp
int fixedValue = 1;       // const int fixedValue = 1;
mut int frameCount = 0;  // int frameCount = 0;
```

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

# Forward include, optimization, and linker flags to the C++ compiler.
gti main.gti -o main -- -Iengine/include -O2 -Lengine/lib -lengine
```

`GTI_CXX` and then `CXX` are used when `--cxx` is omitted. Install the compiler,
LSP, standard-library prelude, runtime headers, and static runtime library with:

```sh
cmake --install build --prefix ~/.local
```

Installed resources are discovered relative to the `gti` executable. Custom
layouts can set `GTI_STDLIB_PATH`, `GTI_RUNTIME_INCLUDE`, and
`GTI_RUNTIME_LIBRARY` explicitly.

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
is attached.

The compiler deliberately keeps parsing, semantic analysis, and C++ emission
as separate visitors/passes. The next semantic layer should add nominal type
identities, class member tables, and function signatures/overload resolution
before engine APIs are exposed to the language.
