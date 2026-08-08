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
source -> shared frontend -> checked AST -> executable typed HIR
       -> HIR optimization pipeline -> C++ backend -> native compiler
```

The frontend and backend contracts, current optimization boundaries, and path
toward an LLVM backend are documented in
[`docs/compiler-architecture.md`](docs/compiler-architecture.md).

The implemented source language supports signed `int8`, `int16`, `int32`, and
`int64` integers, unsigned `uint8`, `uint16`, `uint32`, and `uint64` integers,
the `int`/`uint` aliases for their 32-bit variants, `float`, `bool`, exact 8-bit
`char`, literal-backed `std::string_view`, `nullptr_t`, `expected<T, E>`,
nominal user-defined types, scoped enums, variables, functions, classes,
structs, overloaded explicit constructors, automatic destructors, read-only and
mutable methods, C++-style `public:` and `private:` access labels, constrained
named generic types and functions, restricted member operator overloads, fixed
arrays, local type inference, typed lambdas with explicit immutable value captures,
checked indexing, `Type(value)` numeric
conversions, exact-match function and constructor overloading, blocks,
`if`/`else`, `while`, `for`, explicit `switch`, `break`, `continue`, `return`,
namespaces, namespace aliases, transparent namespace-scoped type aliases,
qualified names, compile-time target conditionals, calls, member access,
assignments, and the arithmetic, modulo, bitwise, comparison, and logical
operators documented in `docs/grammar.ebnf`.

Namespaces use C++-style qualification and can be nested or aliased:

```cpp
namespace engine {
namespace graphics {
void render() {}
}
}

namespace gfx = engine::graphics;

using EntityId = uint64;

int main() {
  engine::graphics::render();
  gfx::render();
  return 0;
}
```

Scoped enums use familiar C++ syntax while avoiding implicit integer
conversions and unqualified enumerator injection:

```cpp
enum class RenderState : uint8 {
  stopped,
  loading = 4,
  running,
};

RenderState state = RenderState::loading;
```

The backing type defaults to `int32` and may be any fixed integral primitive.
Enum values must be referenced through the enum type, including through a
`using` alias, and only compare or pass to functions as the same exact nominal
type. Explicit enumerator values are currently confined to signed integer
literals.

Switch statements keep C++ spelling without C++ fallthrough:

```cpp
switch (state) {
case RenderState::stopped:
  return 0;
case RenderState::loading:
case RenderState::running:
  return 1;
default:
  return 2;
}
```

Subjects are integers, `char`, or scoped enums. Labels must have the exact
subject type and be compile-time literals, explicit integer conversions, or
scoped enumerators. Adjacent labels share one arm, but every executable arm
must end on all paths with `break`, `return`, or a loop-valid `continue`.
Arms have independent lexical scopes even when braces are omitted.

Every reachable path through a non-`void` function or lambda must return a
value. Semantic analysis checks branches, selected target-condition branches,
and loops before backend entry, so missing returns cannot become undefined C++
behavior. A top-level `main` may still reach its closing brace, which has the
C++-familiar and well-defined meaning of returning zero. Until GTI has a
command-line argument type, the entry point must be a definition with signature
`int main()`.

Classes default to private members, while structs default to public members.
Access labels affect every member that follows them, as in C++:

```cpp
class Counter {
  mut int value;

public:
  Counter(int initial) : value(initial) {}

  ~Counter() {
    value = 0;
  }

  int get() {
    return this.value;
  }

  int tick() mut {
    this.value += 1;
    return this.value;
  }
};

struct Point {
  int x = 0;
  int y = 0;
};

mut Counter counter{0};
int next = counter.tick();
Point origin{};
```

Inside an instance method or destructor, `this` names the current object. GTI
uses object member access (`this.value`) because the receiver is not a raw
pointer; the C++ backend handles its pointer representation internally. `self`
is an ordinary identifier and is not retained as a compatibility keyword.

GTI resolves user-defined types nominally and checks member existence,
visibility, signatures, construction, and receiver mutability during semantic
analysis. Construction is always explicit: `Counter value = 1` is invalid,
while both `Counter value = Counter(1)` and `Counter value{1}` are valid. Direct
braces use the declared class type and exact constructor matching; they are not
C++ list or aggregate initialization. Use `mut Counter value{1}` when mutable
methods must be called. Methods are read-only by default and use a trailing
`mut` when they modify mutable fields. Constructors form exact-match overload
sets and never provide implicit conversions. When no zero-argument overload is
declared, the compiler generates `Type()` if every field has a declaration
initializer, even when other overloads are present. Copy/move construction,
copy/move assignment, and destruction are also derived from field lifecycle
traits instead of C++'s special-member suppression rules. Fields remain
immutable by default through GTI semantic checks.

Direct braces are intentionally confined to class and struct bindings:

```cpp
mut Counter value{1};
mut auto inferred = Counter(1);
```

The first form names the type once; the second uses initializer-driven local
inference. GTI does not accept an uninitialized local class binding
`Type value;`, `Type value()`, primitive or array direct braces,
`Type value = {...}`, or `auto value{...}`. This avoids uninitialized class
bindings, the most-vexing-parse ambiguity, list-constructor preference, and
context-free class-template deduction.

The first operator-overloading layer is deliberately limited to the operations
needed by safe pointer and container wrappers:

```cpp
class Handle {
  mut int value = 0;

public:
  int& operator*() { return this.value; }
  mut int& operator*() mut { return this.value; }
  int& operator[](uint64 index) { return this.value; }
  mut int& operator[](uint64 index) mut { return this.value; }
  int operator()(int offset) { return this.value + offset; }
  bool operator==(nullptr_t other) { return false; }
  bool operator!=(nullptr_t other) { return true; }
  operator bool() { return true; }
};
```

GTI currently supports member `operator*`, `operator->`, `operator[]`,
`operator()`, `operator==`, `operator!=`, and contextual `operator bool`.
`operator()` may declare any number of ordinary parameters. Operands and call
arguments match exactly; there are no free operators, implicit conversions,
argument-dependent lookup, rewritten equality candidates, or recursive arrow
proxies. `operator->` must return one checked reference. `operator bool` is used
only by conditions, logical `and`/`or` (equivalently `&&`/`||`), and `!`. Both
logical spellings short-circuit with the same C++ precedence. A leading `mut`
on a method
return makes a `T&` result writable and therefore requires a trailing `mut`
receiver. Operator selection is completed by GTI semantic analysis and lowered
to a private method identity, so the C++ compiler never resolves a GTI operator
overload.

A class or struct may declare one public `~Type()` body. Cleanup runs
automatically, cannot be called manually, has an implicitly mutable receiver,
and executes before fields are destroyed in reverse declaration order. A type
with declared cleanup is noncopyable. Compiler-generated moves transfer an
active-drop state, so moved-from values still tear down their fields without
running the cleanup body twice, and move assignment cleans the old target before
replacement. GTI destructors are implicitly non-throwing and cannot return.

Named type parameters are declared directly on classes, structs, methods, and
functions without a separate C++ `template<typename T>` preamble:

```cpp
class Box<T> {
  T value;

public:
  Box(T value) : value(value) {}
  T& get() { return this.value; }
};

T identity<T>(T value) { return value; }

T minimum<std::ordered T>(T left, T right) {
  if (left < right) { return left; }
  return right;
}

T multiply<std::numeric T>(T left, T right) {
  return T(left * right);
}

Box<int> box = Box<int>(identity(1));
int value = identity<int>(box.get());
```

Class type arguments are always explicit. Function type arguments may be
explicit or inferred exactly from argument types. Each type parameter may have
one built-in capability constraint: `std::ordered`, `std::numeric`,
`std::signed_numeric`, `std::integral`, `std::signed_integral`,
`std::unsigned_integral`, or `std::floating_point`. Constraint checking remains
part of the GTI frontend and applies to concrete arguments, symbolic forwarding,
classes, functions, methods, and every constrained pack element. A constrained
numeric parameter supports checked `T(value)` conversion. This first set
classifies integer and float primitives only; bool, char, string views, and
nominal classes do not satisfy a standard constraint through operator
declarations.

A function or method may use one explicit trailing type pack and parameter pack:

```cpp
void consume<Args...>(Args... values) {}

void forward<Args...>(Args... values) {
  consume(values...);
}
```

Packs may be empty and each element retains its exact type. The first variadic
layer intentionally permits only one final, immutable, by-value pack and only
final call-argument forwarding. It does not include class packs, arbitrary pack
expansion, folds, indexing, or C++ forwarding-reference deduction. GTI does not
currently have user-defined or combined constraints, `requires` clauses,
specialization, constraint-based overload ranking, or pack iteration.

Local type inference uses familiar C++ spelling while preserving GTI's default
immutability:

```cpp
auto count = 1;
mut auto running = count;
running += 1;
```

`auto` requires an initializer with a complete value type and is limited to
local bindings, including `for` initializers. It infers the exact initializer
type without an implicit conversion. Globals, fields, parameters, and return
types remain explicit. Reference bindings and array declarators cannot use
`auto`, and a braced initializer does not provide enough type context on its
own; an already typed fixed-array value can still be copied into an inferred
binding. Inferred owners retain their move-only traits: copying one is rejected
and `auto moved = std::move(owner)` performs an explicit transfer. Semantic and
HIR binding metadata contain the inferred type before backend emission.

Lambdas use the same local binding syntax with narrower capture and lifetime
rules:

```cpp
int offset = 3;
auto add_offset = [offset](int value) -> int {
  return offset + value;
};

int result = add_offset(4);
```

Lambda parameters and return types remain explicit. Capture lists name each
local individually and always take an immutable value snapshot; `[=]`, `[&]`,
`[&value]`, init captures, `this` capture, and mutable lambda call operators are
not supported. A captured type must be copyable. Lambda values may be copied to
another local `auto` binding and called with exact argument types, but cannot
yet be passed to functions, returned, or stored in globals or fields. These
restrictions keep closure lifetimes explicit until callable interfaces and
escape analysis are designed.

Classes and structs may also declare immutable `uint64` value parameters after
their type parameters:

```cpp
class StaticArray<T, uint64 N> {
  T values[N] = {};

public:
  std::size_t size() { return N; }
};

StaticArray<int, 32> values{};
```

Value arguments are currently integer literals or another in-scope value
parameter. They participate in exact type identity, so `StaticArray<int, 4>`
and `StaticArray<int, 8>` are different types. This first layer excludes
function value parameters, value packs, defaults, specialization, and arbitrary
compile-time expressions.

Fixed generic functions, methods, classes, and constructors are instantiated
in typed HIR and ownership-checked with their concrete types. This allows a
move-only type such as `std::unique_ptr<T>` to be a generic argument when the
generic body uses explicit `std::move` transfers. Move-only variadic pack
elements remain outside the current pack layer. Generic constraints are checked
before selection and again through concrete instantiation; the C++ backend emits
ordinary templates and does not define their meaning.

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
parameter names, by-value `mut`, and ordinary method receiver mutability do not
create distinct signatures. The read-only/mutable receiver distinction is
available only to the restricted member operators so wrappers can expose
read-only and writable references. Concrete and generic overloads that both
match are reported as ambiguous.

Numeric conversions are explicit and use familiar functional-cast spelling.
Integer narrowing and float-to-integer conversions are range checked; invalid
dynamic values terminate with a GTI runtime error instead of invoking C++
undefined behavior. Float-to-integer conversion truncates toward zero.

Fixed arrays keep C++ declarator spelling while providing value semantics and
defined bounds behavior:

```cpp
mut int samples[4] = {1, 2, 3, 4};
samples[2] = 10;
std::size_t sample_count = samples.size();

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
with a stable GTI runtime error when the owner is empty. The public pointer is a
nominal class implemented in `stdlib/prelude.gti` over a compiler-private owner
capability. The C++ backend uses `std::unique_ptr` only for that capability's
RAII representation; it is not the GTI type or part of the C runtime ABI.

A method may return `T&` when the returned place is derived from `this`. The
borrow remains tied to the receiver, so storing a result from a temporary
receiver is rejected. Borrowing from a move-only receiver also prevents later
moves, replacement, or mutable method calls in that function. Free-function
reference returns require a broader lifetime model and are not available yet.
Mutable method reference returns use `mut T&`, require a mutable receiver, and
must return a writable place derived from `this`.

The compiler also has a reserved `gti_internal::storage<T>` layer for building
containers in GTI. It owns aligned, partially initialized capacity and provides
checked allocation, construction, receiver-tied borrowed reads, destruction,
and relocation.
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

Quoted paths are resolved relative to the including file and must name a
`.gti` file. Compiler-managed standard-library units use C++-familiar angle
spelling without a file extension:

```cpp
include <std/array>
```

This resolves only against the installed GTI standard-library root; it never
searches project files or native C++ headers. Paths are canonicalized, each
source file is loaded once, and dependency cycles are rejected. Every file is
lexed and parsed as an independent source unit. A unit can use declarations
from itself, files it includes directly, and the implicit standard prelude.
Dependencies of an included file are private to that file; include them
directly when their declarations are also needed.

This is an early source-graph phase, not C++ textual inclusion: it does not
provide macros, conditional preprocessing, repeated copy-and-paste expansion,
or include guards. A trailing semicolon is accepted but not required. The
compiler currently analyzes the complete graph and emits one program; export
syntax, binary modules, and separate compilation are deliberately deferred.
Only the entry source file may declare the top-level `main` entry point.

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

The automatically loaded GTI standard library provides counted text output:

```cpp
std::print("without newline");
std::println("with newline");
```

These are ordinary GTI functions. Their final byte write uses the
`stdout.write` runtime binding and the C ABI implemented under `runtime/`; the
compiler does not recognize `print` as syntax.

String literals have the trivial `std::string_view` type and point at static
literal storage. The view carries its byte length, preserves embedded `\0`
bytes, copies without allocation, and provides `size()`, `empty()`, and checked
read-only indexing. `char` is a distinct unsigned 8-bit code unit rather than an
integer alias. The former unqualified `string` primitive is removed.

Owning text is an explicitly imported, source-defined standard-library class:

```cpp
include <std/string>

mut std::string name = std::string("GTI");
name.push_back(' ');
name.append("runtime");
mut std::string copy = name.clone();
copy[0] = 'g';
```

`std::string` is move-only because its backing storage is uniquely owned.
Allocating duplication is explicit through `clone()` instead of being hidden in
ordinary assignment. It deliberately does not produce a dynamic
`std::string_view` until borrowed views have owner-tied lifetime semantics.
Formatting and formatted output remain later standard-library layers.

Optional standard-library facilities are imported explicitly. `std::array` is
implemented in GTI over bounded fixed-array storage:

```cpp
include <std/array>

int initial[3] = {1, 2, 3};
mut std::array<int, 3> values = std::array<int, 3>(initial);
values[1] = 4;
```

Its size remains part of exact type identity, indexing retains GTI bounds
checks, and the compiler does not treat the public `std::array` name specially.

## Repository layout

- `include/gti/` contains the reusable compiler frontend, AST, analysis, and
  backend interfaces.
- `src/cli/` and `src/lsp/` contain the two executable entry points.
- `tests/` contains compiler, CLI, and LSP tests.
- `examples/` contains GTI source programs.
- `docs/` contains the language grammar and compiler design contracts.
- `stdlib/` contains the implicit prelude and explicitly imported ordinary GTI
  library units.
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
expected<int, std::string_view> load(bool fail) {
  if (fail) {
    return unexpected("load failed");
  }
  return 42;
}

int main() {
  expected<int, std::string_view> result = load(false);
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

# Select a compiler and show its command and native output.
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
the native compiler. Successful builds suppress native compiler output because
it refers to generated C++ rather than GTI source; `--verbose` prints the
command and replays that output for backend investigation. Native compiler
output is always shown on failure, together with the retained generated C++
path. Install the compiler, LSP, complete standard-library tree, runtime
headers, compatibility headers, and static runtime library with:

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
`GTI_VENDOR_INCLUDE` explicitly. `GTI_STDLIB_PATH` should name the standard
library root; a direct path to `prelude.gti` remains accepted for compatibility.

Compiler diagnostics include a stable error code, exact source underline,
related declaration or include locations, and actionable help when available.
If the native C++ compiler rejects generated output, `gti` retains the temporary
`.cpp` file and prints its path so the backend failure can be inspected.

## LazyVim and Neovim

The `gti_lsp` target provides lexical, include, parser, and semantic diagnostics,
semantic highlighting, compiler-owned semantic hover, and whole-document
formatting over the Language Server Protocol. Hover shows GTI signatures,
inferred `auto` types, bindings, and the exact overload or constructor selected
by semantic analysis. Diagnostics carry exact UTF-16 ranges, stable codes,
related locations, document versions, and machine-readable fix data where the
compiler knows an unambiguous correction. Included-file errors are published
against the included file, while a missing include is reported on its directive
in the including document. Editing diagnostics are analyzed
from coalesced document snapshots off the protocol request loop, so formatting
and highlighting requests remain responsive while the compiler checks a larger
file. Open included files are analyzed from their unsaved buffers, and changes
invalidate every open root that depends on that source.

The release toolchain includes a native GTI Tree-sitter parser. Tree-sitter
provides immediate structural highlighting, indentation queries, and folds;
LSP semantic tokens remain enabled on top for resolved symbol roles such as
types, namespaces, functions, methods, properties, type parameters, and
immutable bindings. Its capture names follow Neovim's C/C++ taxonomy for
ordinary variables, control flow, return statements, logical operators,
generic brackets, and punctuation, so C++-oriented themes can style GTI with
the same highlight groups. The regex syntax file is retained only as a fallback
when no native parser is available. Release builds link `json-c` into
`gti_lsp`, so users do not need to install `json-c`, a separate Tree-sitter
grammar, Mason, or `nvim-lspconfig`.

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
   runtime library, headers, standard-library sources, and licenses inside the
   plugin's private `toolchain/` directory.

The plugin registers the `.gti` filetype, loads the bundled Tree-sitter parser,
starts structural highlighting, enables `gti_lsp` through Neovim's native
`vim.lsp.config` mechanism, and adds the bundled `gti` and `gti_lsp` binaries to
Neovim's process environment. `:LspInfo` should show `gti_lsp` attached;
`:GTIInfo` shows the plugin, installed toolchain, compiler, language server,
running LSP, and parser versions. It reports a mismatch explicitly instead of
leaving stale grammar diagnostics to look like compiler failures. The plugin
also warns when an attached `gti_lsp` does not match its checked-out release.
LazyVim's `<leader>cf` command and format-on-save path use the LSP formatter.
Formatting follows C++ layout conventions and honors the buffer's indentation
width and spaces-versus-tabs setting; the GTI filetype defaults to two spaces
and uses Tree-sitter indentation when `nvim-treesitter` exposes its indent
engine.

The formatter also discovers the nearest `.gti-format` from the source file's
directory upward. The first project option controls checked-reference spacing
using clang-format-compatible names:

```yaml
ReferenceAlignment: Middle
```

Use `Left` for `int& value`, `Right` for `int &value`, or `Middle` for the
default `int & value`. Bitwise `&` expressions remain spaced as binary
operators.

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
and release archive names. Shipped compiler, standard-library, LSP,
Tree-sitter, formatter, runtime, or Neovim plugin changes must advance it.
`scripts/check_release_version.py` enforces that rule in CI.

Pushing a `VERSION` change to `main` starts `.github/workflows/release.yml`
automatically. It validates the matching `vX.Y.Z` tag name, builds and tests
four platforms, checks that `gti_lsp` has no dynamic `json-c` dependency,
stages the installed toolchain and native Tree-sitter parser, and publishes
each `.tar.gz` plus its `.sha256` file. The publish job creates the matching
tag, so releasing editor tooling no longer depends on a separate manual tag
command. Explicit matching tag pushes and manual workflow runs remain
supported for recovery. Packaging fails if the resolved tag and `VERSION`
disagree or if a required toolchain file is missing. Normal pushes and pull
requests run the compiler, CLI, LSP, and release-version policy tests through
`.github/workflows/ci.yml`.

GTI is distributed under the MIT License; see `LICENSE`.

The compiler deliberately keeps frontend analysis, optimization, backend code
generation, and native toolchain invocation as separate stages. The semantic
model records value categories, access, ownership, transferability, lexical
drop requirements, resolved constructor overloads, and explicit class lifecycle
policy. [`docs/ownership.md`](docs/ownership.md) defines the staged reference,
owner, and internal storage design. GTI can now express vector-style allocation,
relocation, class-defined cleanup, active-drop movement, move-only aggregate
lifecycle, and receiver-tied element borrows without exposing raw pointers. The next
container milestone is implementing the nominal `std::vector` policy and API in
ordinary GTI over those capabilities.
