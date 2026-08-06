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

### Personal Statement

I personally have always tried to refrain from any use of AI in programming. I strongly believe in the long run it will make me a worse programmer.
However, I do from time to time like to keep tabs on the progression of AI agents. Which brings me here.

I have in my spare time tried to learn about compiler internals such as lexers and parsers. Due to many factors I have not really had the time to
actually dive deep into the topic. I am also genuinely building a game engine thought and I have also loved the idea of having an in-house scripting language.
So, I asked codex to extend the compiler I had been writing following a tutorial that I had long abandoned and funnily enough it produced a useable language.

Until the time comes around that I actually find the time to do this myself this will all be replaced but it has been a fun little side project. This code base is not
meant to be taken seriously in any shape or form :).

`gti` implements a small optimizing compiler with a replaceable backend:

```text
source -> shared frontend -> checked program -> optimization pipeline
       -> C++ backend -> native compiler
```

The frontend and backend contracts, current optimization boundaries, and path
toward an LLVM backend are documented in
[`docs/compiler-architecture.md`](docs/compiler-architecture.md).

The implemented source language supports signed `int8`, `int16`, `int32`, and
`int64` integers, unsigned `uint8`, `uint16`, `uint32`, and `uint64` integers,
the `int`/`uint` aliases for their 32-bit variants, `float`, `bool`, `string`,
`expected<T, E>`, nominal user-defined types, variables, functions, classes,
structs, explicit constructors, read-only and mutable methods, C++-style
`public:` and `private:` access labels, named generic types and functions,
fixed arrays, checked indexing, `Type(value)` numeric conversions, exact-match
function overloading, blocks,
`if`/`else`, `while`, `for`, `break`, `continue`, `return`, namespaces,
namespace aliases, qualified names, compile-time target conditionals, calls,
member access, assignments, and the arithmetic, modulo, bitwise, comparison,
and logical operators documented in `docs/grammar.ebnf`.

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

Classes default to private members, while structs default to public members.
Access labels affect every member that follows them, as in C++:

```cpp
class Counter {
  mut int value;

public:
  Counter(int initial) : value(initial) {}

  int get() {
    return self.value;
  }

  int tick() mut {
    self.value += 1;
    return self.value;
  }
};

struct Point {
  int x = 0;
  int y = 0;
};

mut Counter counter = Counter(0);
int next = counter.tick();
Point origin = Point();
```

GTI resolves user-defined types nominally and checks member existence,
visibility, signatures, construction, and receiver mutability during semantic
analysis. Constructor calls are always explicit: `Counter value = 1` is
invalid, while `Counter value = Counter(1)` is valid. Methods are read-only by
default and use a trailing `mut` when they modify mutable fields. A class or
struct without a declared constructor receives `Type()` only when all fields
have declaration initializers.

Named type parameters are declared directly on classes, structs, methods, and
functions without a separate C++ `template<typename T>` preamble:

```cpp
class Box<T> {
  T value;

public:
  Box(T value) : value(value) {}
  T get() { return self.value; }
};

T identity<T>(T value) { return value; }

Box<int> box = Box<int>(identity(1));
int value = identity<int>(box.get());
```

Class type arguments are always explicit. Function type arguments may be
explicit or inferred exactly from argument types. GTI does not currently have
generic constraints, specialization, non-type parameters, or `auto`.

Functions and methods can be overloaded by parameter type without C++'s
conversion-ranking rules:

```cpp
uint64 multiply(uint64 left, uint64 right) { return left * right; }
float multiply(float left, float right) { return left * right; }

uint64 whole = multiply(uint64(6), uint64(7));
float decimal = multiply(1.5, 2.0);
```

A call must have one unique exact match after generic substitution. GTI does
not implicitly widen arguments or choose a preferred overload. Return types,
parameter names, by-value `mut`, and method receiver mutability do not create
distinct signatures. Concrete and generic overloads that both match are
reported as ambiguous.

Numeric conversions are explicit and use familiar functional-cast spelling.
Integer narrowing and float-to-integer conversions are range checked; invalid
dynamic values terminate with a GTI runtime error instead of invoking C++
undefined behavior. Float-to-integer conversion truncates toward zero.

Fixed arrays keep C++ declarator spelling while providing value semantics and
defined bounds behavior:

```cpp
mut int samples[4] = {1, 2, 3, 4};
samples[2] = 10;
uint64 sample_count = samples.size();

int grid[2][2] = {{1, 2}, {3, 4}};
```

The extent is compile-time type information, so `int[4]` and `int[5]` are
distinct exact types and `size()` stores no runtime field. Arrays are inline,
contiguous values: they can be copied, moved, assigned, passed, and returned
according to their element type. They never decay to pointers and do not expose
`.data()`. Non-empty initializers require exactly the declared number of
elements; `{}` value-initializes every element. Indexing is checked, with
constant failures diagnosed by the frontend and dynamic failures reported as a
defined GTI runtime error.

GTI provides non-null borrows and move-only heap ownership without public raw
pointers, `new`, or `delete`:

```cpp
void update(mut Entity& entity) {
  entity.tick();
}

std::unique_ptr<Entity> create_entity(int id) {
  std::unique_ptr<Entity> entity = std::make_unique<Entity>(id);
  return std::move(entity);
}
```

`T&` is read-only and `mut T&` permits mutation of the borrowed value. A
reference can bind only an addressable place and cannot be null. Unique owners
must be transferred explicitly with `std::move`; copying and use after move are
compile errors. `owner->member` and `*owner` perform checked access, terminating
with a stable GTI runtime error when the owner is empty. The C++ backend uses
`std::unique_ptr` as its RAII representation, while ownership remains a GTI
semantic rule rather than part of the C runtime ABI.

The compiler also has a reserved `gti_internal::storage<T>` layer for building
containers in GTI. It owns aligned, partially initialized capacity and provides
checked allocation, construction, copied reads, destruction, and relocation.
Classes containing this storage automatically become move-only, including
nested and generic aggregates; copies and use after move are diagnosed before
code generation. Explicit `std::move` transfers the complete aggregate.
The C++ backend uses a private RAII representation; raw addresses, pointer
arithmetic, and manual deallocation remain unavailable to GTI source. See
[`docs/ownership.md`](docs/ownership.md) for the internal contract and remaining
steps toward `std::vector`.

Source files can depend on other GTI files with a top-level include directive:

```cpp
include "math.gti"
```

The path is resolved relative to the including file and must name a `.gti`
file. Paths are canonicalized, each source file is loaded once, and dependency
cycles are rejected. This is an early source-loading phase, not C++ textual
inclusion: it does not provide macros, conditional preprocessing, or repeated
copy-and-paste expansion. A trailing semicolon is accepted but not required.

## Compile-time target selection

GTI provides restricted compile-time branching without textual macros:

```cpp
#if target.vendor == "apple"
void create_window() { /* Apple implementation */ }
#elif target.os == "windows"
void create_window() { /* Windows implementation */ }
#else
void create_window() { /* Other implementation */ }
#endif
```

Conditions support `==` and `!=` against `target.os`, `target.vendor`, and
`target.arch`. Directives may surround declarations, class members, or block
items. Every branch must contain syntactically valid GTI, while only the active
branch is semantically analyzed and lowered. GTI resolves the branch itself;
it does not emit C++ preprocessor directives. Conditional `include` directives
are deliberately rejected.

The initial implementation selects the host where the GTI compiler was built.
Current values include `macos`, `windows`, and `linux` for `target.os`; `apple`,
`pc`, and `unknown` for `target.vendor`; and `arm64`, `x86_64`, `x86`, and
`unknown` for `target.arch`. Explicit cross-compilation targets will be added
with the future target-toolchain model.

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
- `docs/` contains the language grammar and compiler design contracts.
- `stdlib/` contains ordinary GTI library functions and runtime declarations.
- `runtime/` contains the narrow C ABI used for host-platform operations.
- `vendor/` contains pinned compatibility code required by older C++ targets.
- `tree-sitter-gti/` contains the GTI Tree-sitter grammar and generated C
  parser; `queries/gti/` contains structural editor queries.
- `ftdetect/`, `ftplugin/`, `syntax/`, `lsp/`, `plugin/`, and `lua/gti/`
  form the standard Neovim runtime and its toolchain installer.

Variables and parameters are immutable by default and lower to `const` C++.
Use `mut` only for bindings that need to change:

```cpp
int fixedValue = 1;       // const std::int32_t fixedValue = 1;
mut int frameCount = 0;  // std::int32_t frameCount = 0;
```

Integer widths are explicit and lower to the corresponding C++ `<cstdint>`
type. `int` is exactly `int32`, and `uint` is exactly `uint32`, providing
portable 32-bit defaults:

```cpp
int8 small = 127;
int16 medium = small;             // implicit widening is safe
int count = 2147483647;           // the same type as int32
int64 large = 9223372036854775807;
uint8 byte = 255;
uint64 mask = 18446744073709551615;
```

An integer literal may initialize any width when its value fits. Other integer
expressions convert implicitly only when every possible source value fits the
destination. As in C++, all 8- and 16-bit arithmetic promotes to `int32`.
Signed/unsigned expressions are accepted when the conversion is safe, such as
`int64 + uint32`, or when a nonnegative literal fits the unsigned operand.
Potentially negative values are never silently reinterpreted as unsigned.

Integer bit operations use familiar C++ spelling and precedence:

```cpp
int flags = ((value & 15) | 16) ^ 2;
int shifted = (flags << 2) >> 1;
int inverted = ~shifted;
int bucket = inverted % 7;
```

These operators accept integers only. Modulo and binary bitwise operations use
the same promotion and safe signed/unsigned rules as arithmetic. Shifts return
the promoted left type. Shift counts must be nonnegative and smaller than that
type's width. Dynamic modulo-by-zero and invalid shift counts terminate with a
GTI runtime error instead of invoking C++ undefined behavior. Left shift wraps
by bit pattern, signed right shift is arithmetic, and signed minimum modulo
`-1` is defined as `0`. Compound forms such as `%=`, `&=`, and `<<=` are not
implemented yet.

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
./build/gti examples/01-basics.gti -o /tmp/gti-basics
/tmp/gti-basics
```

Run the compiler tests with `ctest --test-dir build --output-on-failure`.

The output path defaults to the source filename without `.gti`, so this builds
`examples/01-basics`:

```sh
./build/gti examples/01-basics.gti
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

# Run GTI optimizations and request the matching native optimization level.
gti main.gti -O2 -o main

# Forward include and linker flags directly to the C++ compiler.
gti main.gti -o main -- -Iengine/include -Lengine/lib -lengine
```

Generated programs target C++23 by default. Pass `--std c++20` to use the
vendored `nonstd::expected` implementation. `GTI_CXX` and then `CXX` are used
when `--cxx` is omitted. Optimization defaults to `-O0`; `-O1`, `-O2`, and
`-O3` enable safe GTI constant folding and pass the same optimization level to
the native compiler. Install the compiler, LSP, standard-library prelude,
runtime headers, compatibility headers, and static runtime library with:

```sh
cmake --install build --prefix ~/.local
export PATH="$HOME/.local/bin:$PATH"
gti --version
```

Add the `PATH` export to `~/.zshrc`, `~/.bashrc`, or the equivalent startup file
to make it persistent. The installed `gti` and `gti_lsp` commands discover
their resources from the actual executable location, including when launched
by basename through `PATH` or through a symlink. Custom layouts can set
`GTI_STDLIB_PATH`, `GTI_RUNTIME_INCLUDE`, `GTI_RUNTIME_LIBRARY`, and
`GTI_VENDOR_INCLUDE` explicitly.

Compiler diagnostics include a stable error code, exact source underline,
related declaration or include locations, and actionable help when available.
If the native C++ compiler rejects generated output, `gti` retains the temporary
`.cpp` file and prints its path so the backend failure can be inspected.

## LazyVim and Neovim

The `gti_lsp` target provides lexical, include, parser, and semantic diagnostics,
semantic highlighting, and whole-document formatting over the Language Server
Protocol. Diagnostics carry exact UTF-16 ranges, stable codes, related
locations, document versions, and machine-readable fix data where the compiler
knows an unambiguous correction. Included-file errors are published against the
included file rather than the entry document. Editing diagnostics are analyzed
from coalesced document snapshots off the protocol request loop, so formatting
and highlighting requests remain responsive while the compiler checks a larger
file. Open included files are analyzed from their unsaved buffers, and changes
invalidate every open root that depends on that source.

The release toolchain includes a native GTI Tree-sitter parser. Tree-sitter
provides immediate structural highlighting, indentation queries, and folds;
LSP semantic tokens remain enabled on top for resolved symbol roles such as
types, namespaces, functions, methods, properties, type parameters, and
immutable bindings. The regex syntax file is retained only as a fallback when
no native parser is available. Release builds link `json-c` into `gti_lsp`, so
users do not need to install `json-c`, a separate Tree-sitter grammar, Mason,
or `nvim-lspconfig`.

GTI is a standard Lazy plugin. Add one file such as
`~/.config/nvim/lua/plugins/gti.lua` to LazyVim:

```lua
return {
  { "LukeHHA/gti", version = "*" },
}
```

Run `:Lazy sync`, restart Neovim, and open a `.gti` file. `version = "*"`
selects the latest tagged GTI release instead of an arbitrary commit on
`main`. Lazy runs the repository's `build.lua` hook after install and update.
That hook:

1. Reads the checked-out `VERSION`.
2. Selects the release archive for the host OS and CPU.
3. Downloads the archive and its adjacent SHA-256 file from the matching
   GitHub release.
4. Verifies the checksum and archive layout.
5. Atomically installs the compiler, language server, Tree-sitter parser,
   runtime library, headers, standard-library prelude, and licenses inside the
   plugin's private `toolchain/` directory.

The plugin registers the `.gti` filetype, loads the bundled Tree-sitter parser,
starts structural highlighting, enables `gti_lsp` through Neovim's native
`vim.lsp.config` mechanism, and adds the bundled `gti` and `gti_lsp` binaries to
Neovim's process environment. `:LspInfo` should show `gti_lsp` attached;
`:GTIInfo` shows the active compiler, language server, parser, and installed
version. LazyVim's `<leader>cf` command and format-on-save path use the LSP
formatter. Formatting follows C++ layout conventions and honors the buffer's
indentation width and spaces-versus-tabs setting; the GTI filetype defaults to
two spaces and uses Tree-sitter indentation when `nvim-treesitter` exposes its
indent engine.

Automatic binary installation currently supports:

- macOS on Apple Silicon (`darwin-arm64`)
- macOS on Intel (`darwin-x64`)
- Linux on ARM64 (`linux-arm64`)
- Linux on x86-64 (`linux-x64`)

It requires Neovim 0.11 or newer, `tar`, and either `curl` or `wget`. Compiling
a GTI program also requires a C++ compiler available through `GTI_CXX`, `CXX`,
or `PATH`; generated programs target C++23 by default and can use the vendored
C++20 compatibility path with `gti --std c++20`.

To use tools built elsewhere, set `GTI_LSP_PATH`, `GTI_PATH`, and optionally
`GTI_TREE_SITTER_PATH`. Resolution prefers those overrides, then the
Lazy-installed toolchain, then `PATH`, and finally this repository's `build/`
directory for local development. Set `build = false` in the Lazy spec when
deliberately skipping the released toolchain download:

```lua
return {
  {
    "LukeHHA/gti",
    version = "*",
    build = false,
  },
}
```

## Releases

`VERSION` is the source of truth for CMake, the CLI version, Lazy's installer,
and release archive names. A tag must be exactly `v` followed by that value.
For example, after changing `VERSION` to `0.4.0`, committing it, pushing it, and
waiting for CI to pass:

```sh
git tag -a v0.4.0 -m "GTI v0.4.0"
git push origin v0.4.0
```

The tag starts `.github/workflows/release.yml`. It builds and tests four
platforms, checks that `gti_lsp` has no dynamic `json-c` dependency, stages the
installed toolchain and native Tree-sitter parser, and publishes each `.tar.gz`
plus its `.sha256` file to a GitHub release. Packaging fails if the tag and
`VERSION` disagree or if a required toolchain file is missing. Normal pushes
and pull requests run the compiler, CLI, and LSP test suite through
`.github/workflows/ci.yml`.

GTI is distributed under the MIT License; see `LICENSE`.

The compiler deliberately keeps frontend analysis, optimization, backend code
generation, and native toolchain invocation as separate stages. Explicit
constructors and receiver mutability are now implemented. Named generic classes
and functions provide the type-level foundation for containers. The semantic
model now records value categories, access, ownership, transferability, and
lexical drop requirements. [`docs/ownership.md`](docs/ownership.md) defines the
staged reference, owner, and internal storage design. The compiler can now
express vector-style allocation and relocation without exposing raw pointers;
aggregate ownership traits now propagate through container classes. Self-tied
element borrows remain before a GTI-native `std::vector` becomes public.
