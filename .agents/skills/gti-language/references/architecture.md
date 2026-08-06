# GTI Compiler Architecture

Read this reference before changing an unfamiliar phase. Confirm details in the
code because this project is evolving.

## Pipeline Map

| Phase | Primary API | Input -> output | Owns |
| --- | --- | --- | --- |
| Source loading | `include/gti/source_loader.h` | entry path + prelude paths -> one token stream | file reads, relative includes, canonicalization, load-once behavior, cycles, include placement |
| Lexing | `include/gti/lexer.h`, `token.h` | source text -> `vector<Token>` | spelling, literals, byte offsets, source path, line numbers, lexical diagnostics |
| Parsing | `include/gti/parser.h` | tokens -> `Program` | grammar, precedence, AST construction, parse diagnostics, synchronization |
| AST | `include/gti/ast.h` | syntax model | node ownership, `ExprVisitor`, `StmtVisitor`, target-condition structure |
| Semantics | `include/gti/semantic_analyzer.h` | `Program` -> diagnostics + `SemanticModel` | scopes, namespaces, symbols, expression types, mutability, result use, expected rules, runtime binding validation |
| Frontend | `include/gti/frontend.h` | entry source -> `FrontendResult` | shared phase ordering, checked-program ownership, source map, aggregate diagnostics |
| Optimization | `include/gti/optimizer.h` | checked program -> `OptimizationResult` | target-aware, semantics-preserving middle-end decisions |
| Backend | `include/gti/backend.h`, `cpp_backend.h` | checked program + optimization result -> artifact | replaceable code-generation contract and C++ implementation |
| C++ emission | `include/gti/cpp_emitter.h` | backend input -> C++ text | C++ representation, forward declarations, target-specific output, C++20/C++23 differences |
| Native build | `src/cli/main.cpp` | C++ text + options -> executable | toolchain discovery, generated files, compiler invocation, CLI diagnostics |
| Language service | `src/lsp/main.cpp` | open documents -> LSP messages | live diagnostics, semantic tokens, whole-document formatting requests |
| Formatting | `include/gti/formatter.h` | GTI source -> GTI source | whitespace and layout while preserving comments |

`include/gti/diagnostic.h` is shared infrastructure rather than a compiler
phase. It owns `SourceSpan`, `Diagnostic`, related locations, fix-its, and
`SourceManager`; lexer, source-loader, parser, and semantic errors must use this
model instead of defining phase-local display structures.

The reusable compiler is header-only through the `gti_compiler` CMake interface
target. Executable policy belongs in the CLI and LSP drivers.
The compiler and tools themselves build as C++20; generated programs target
C++23 by default. `json-c` is optional at configure time, and the LSP target is
omitted when it is unavailable.

The checked AST is the current high-level compiler representation. Preserve
source structure and store analysis or optimization results in side tables.
Introduce a typed HIR when generic monomorphization and stable symbol IDs are
needed, then a layout-resolved control-flow MIR before adding LLVM emission.
See `docs/compiler-architecture.md` for the staged backend roadmap.

## Source And Token Contracts

- `SourceLoader::load()` injects the standard-library prelude before the entry
  source, recursively resolves includes, removes include directives from the
  resulting stream, and appends one entry-file EOF token.
- Includes are not AST nodes. Change `SourceLoader` for include semantics, not
  `Parser` or `CppEmitter`.
- The CLI and LSP automatically load `stdlib/prelude.gti`. Tests that call
  `Lexer` and `Parser` directly do not.
- Every token carries `source`, one-based `line`, and a byte `position` local to
  that source. Preserve those fields when synthesizing diagnostics.
- Diagnostic spans use source-local byte offsets with an exclusive end. The
  CLI converts them to one-based source locations, while the LSP converts them
  to zero-based UTF-16 ranges. Do not store display columns in compiler phases.
- Give diagnostics stable phase-prefixed codes (`L`, `I`, `P`, `S`) and attach
  related declarations or include sites when they explain the primary error.
  Add a fix-it only when its replacement is mechanically correct.
- The lexer discards comments. The formatter and LSP comment highlighting use
  separate source scanning, so comment-sensitive syntax can require updates in
  more than one scanner.

## Parser And AST Contracts

- `Parser::parse()` recovers after declaration errors and returns all valid
  later declarations. Keep synchronization behavior covered when adding a new
  declaration or statement boundary.
- Code generation stops after any parse error. The LSP may still analyze the
  parser's recovered declarations so independent semantic errors remain visible
  during editing.
- `Parser::parseExpression()` is a focused entry point for tests and tooling.
- AST children use owning smart pointers. Add a visitor method to every visitor
  interface and implementation when adding a concrete node; the compiler should
  fail to build until all required passes handle it.
- Parse all compile-time branches. Syntax errors in inactive branches are still
  errors.
- Keep name lookup, type compatibility, mutability, and call validity out of the
  parser.

## Semantic Contracts

- `SemanticVisitor::check()` first registers namespaces and namespace symbols,
  then analyzes declarations with nested scopes. Preserve this predeclaration
  behavior when adding declarations that may be referenced before definition.
- `ConditionalStmt::activeBranch(TargetInfo)` selects the branch analyzed for a
  target. `CppEmitter` performs the same selection. Pass equivalent target data
  to both phases or semantics and output can diverge.
- Immutable variables require initializers. Parameters and bindings are
  non-assignable unless marked `mut`.
- User-defined classes and structs have nominal identities and collected member
  tables. Classes default to private access, structs default to public access,
  and `public:`/`private:` affect following members.
- A class or struct has at most one declared constructor. Construction is an
  explicit `Type(arguments)` call; constructor-based implicit conversion is not
  part of assignability.
- Constructor initializer lists initialize fields in declaration order before
  the body. Fields omitted from the list require declaration initializers, and
  `self` and members are unavailable until the body begins.
- A synthesized `Type()` is available only when no constructor is declared and
  every field has a declaration initializer. Class-valued variables always
  require an explicit construction expression.
- Methods have read-only receivers by default. A trailing `mut` method may
  mutate mutable fields and can only be called through a mutable receiver.
  Private access remains available from methods and constructors of the owning
  type.
- Classes, structs, methods, and functions may declare named type parameters
  directly after their name. Applied class types are nominal and require exact
  arity. Function type arguments are either explicit or inferred exactly from
  argument types; inference does not use return context or conversions.
- Generic bodies are checked once with type-parameter identities. Member lookup
  substitutes an applied class's arguments into its fields, methods, and
  constructor. Constraints, specialization, non-type parameters, and `auto`
  remain outside the current generic model.
- Modulo and bitwise operators require integer operands. Binary operations use
  existing integer promotions and safe signed/unsigned common types; shifts
  return the promoted left type and validate literal counts during semantics.
- Direct non-`void` call statements are errors unless marked `[[discard]]`.
- `expected<T, E>` is a language type, not a general template facility. Its
  observer surface is checked explicitly in semantics.
- A frontend `main` declaration is not mandatory. Native executable generation
  can still fail at the C++ linker when no entry point exists.

## C++ Backend Contracts

- C++23 is the default and uses `std::expected`; C++20 uses vendored
  `nonstd::expected` with equivalent GTI semantics.
- GTI namespace `std` lowers to `gti_std` because adding user declarations to
  C++ `std` is invalid. Qualified references must be rewritten consistently.
- Immutable bindings lower to `const`; immutable string parameters lower by
  const reference.
- Declared constructors lower to `explicit` C++ constructors. Read-only methods
  lower with a trailing C++ `const`; GTI trailing-`mut` methods lower without
  it.
- Validated named generic declarations lower to C++ template declarations and
  applied types. C++ templates are a backend representation, not the source of
  GTI generic semantics or diagnostics.
- Modulo and shifts lower through generated checked helpers. Dynamic zero
  divisors and invalid counts terminate with a GTI runtime diagnostic; signed
  minimum modulo `-1`, wrapping left shift, and arithmetic signed right shift
  have defined GTI behavior.
- Runtime-bound declarations emit no ordinary function body. Their presence
  causes the runtime adapter header to be included.
- Generated C++ is an implementation artifact, not the language specification.
  Do not expose C++ quirks as GTI behavior without a language-level reason.

## Standard Library And Runtime Boundary

- `stdlib/prelude.gti` contains ordinary GTI APIs. Public APIs live under
  `std`; compiler-owned declarations live under `gti_internal`.
- `@runtime("...")` is a compiler-validated, bodyless declaration for a known
  native service. Do not make it a general foreign-function escape hatch by
  accident.
- `runtime/include/gti/runtime.h` defines the narrow C ABI.
- `runtime/include/gti/runtime.hpp` adapts C ABI calls to emitted C++ types.
- `runtime/src/` implements host behavior. Keep portable formatting, algorithms,
  and policy in GTI where possible.

## CLI, LSP, And Editor Boundaries

- The CLI owns argument parsing, installation/build-tree resource discovery,
  temporary C++ output, native compiler arguments, and process execution.
- `include/gti/executable_path.h` resolves the running CLI or LSP executable
  through the host OS and canonicalizes symlinks. Installed resource discovery
  must work when the tool was launched by basename through `PATH`; `argv[0]`
  and build-tree paths are fallbacks for unsupported development hosts only.
- Release builds intentionally contain no build-machine resource paths. Their
  standard library, runtime, and vendor files must resolve relative to the
  installed executable or through an explicit environment override.
- The CLI renders shared diagnostics with code, source excerpt, underline,
  related notes, and help. A failed native compiler invocation retains its
  generated C++ file and reports that path for backend investigation.
- The CLI and LSP both use `Frontend`; do not duplicate source loading, parsing,
  and semantic phase ordering in either driver.
- LSP diagnostics retain document versions, exact UTF-16 ranges, stable codes,
  severity, related information, and serialized fix-it data. Diagnostics for
  includes are published on the included file URI and cleared when stale.
- Included sources are currently read from disk even when the same file has an
  unsaved editor buffer. Supporting a source-provider overlay and dependency
  reanalysis is a separate change; do not silently claim unsaved dependency
  edits are included in analysis.
- LSP semantic classification is token-based and contains declaration
  heuristics. Update the advertised legend and protocol tests together.
- LSP formatting delegates to `lang::Formatter` and honors `tabSize` and
  `insertSpaces`.
- `plugin/gti.lua` registers `.gti`, starts the server, maps semantic highlight
  groups, and exposes `:GTIInfo`. `lsp/gti_lsp.lua` supplies the native Neovim
  0.11 server configuration.
- `ftdetect/`, `ftplugin/`, and `syntax/` provide file detection, fallback
  highlighting, comments, and C-style indentation.
- `lazy.lua` is the plugin spec and `build.lua` invokes
  `lua/gti/installer.lua`. The installer downloads the archive matching the
  checked-out release's `VERSION` into the plugin-private `toolchain/`.
- `lua/gti/toolchain.lua` resolves explicit environment overrides first, then
  the release-installed binaries, then `PATH`, then local development builds.

## Version And Release Boundary

- `VERSION` is the release source of truth. CMake propagates it to the CLI and
  LSP, while the installer uses it to construct release archive URLs.
- Lazy specs using `version = "*"` select the newest semantic-version tag. A
  commit on `main` is therefore not delivered to those users until a matching
  tag has completed `.github/workflows/release.yml`.
- Release archives include `gti`, `gti_lsp`, runtime and compiler support files,
  the standard library, `VERSION`, and licenses. Release CI tests the installed
  binaries rather than only the build tree.
- For an editor report, compare `:GTIInfo`, the plugin checkout's `VERSION`, and
  `toolchain/share/gti/VERSION`. Restart the LSP after Lazy updates the plugin so
  Neovim does not retain the previous process.

## Current Non-Goals

Do not assume support for constructor or function overloading, generic
constraints, specialization, non-type generic parameters, pointers,
references, arrays, destructors, inheritance, exceptions, textual macros,
implicit error propagation, modules, separate compilation, or a stable ABI.
Check `docs/grammar.ebnf` for the implemented surface before designing around a
C++ feature.
