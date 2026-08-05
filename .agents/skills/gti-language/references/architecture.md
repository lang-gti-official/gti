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
| Semantics | `include/gti/semantic_analyzer.h` | `Program` -> diagnostics | scopes, namespaces, symbols, types, mutability, result use, expected rules, runtime binding validation |
| Lowering | `include/gti/cpp_emitter.h` | valid `Program` -> C++ text | C++ representation, forward declarations, target-specific output, C++20/C++23 differences |
| Native build | `src/cli/main.cpp` | C++ text + options -> executable | toolchain discovery, generated files, compiler invocation, CLI diagnostics |
| Language service | `src/lsp/main.cpp` | open documents -> LSP messages | live diagnostics, semantic tokens, whole-document formatting requests |
| Formatting | `include/gti/formatter.h` | GTI source -> GTI source | whitespace and layout while preserving comments |

The reusable compiler is header-only through the `gti_compiler` CMake interface
target. Executable policy belongs in the CLI and LSP drivers.
The compiler and tools themselves build as C++20; generated programs target
C++23 by default. `json-c` is optional at configure time, and the LSP target is
omitted when it is unavailable.

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
- The lexer discards comments. The formatter and LSP comment highlighting use
  separate source scanning, so comment-sensitive syntax can require updates in
  more than one scanner.

## Parser And AST Contracts

- `Parser::parse()` recovers after declaration errors and returns all valid
  later declarations. Keep synchronization behavior covered when adding a new
  declaration or statement boundary.
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
- Every field currently requires an initializer. `self` is valid only in a
  method body, and private access is permitted from methods of the owning type.
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
- The LSP reruns source loading, parsing, and semantics for diagnostics. Keep its
  phase ordering consistent with the CLI.
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

Do not assume support for general templates, pointers, references, arrays,
constructors, destructors, inheritance, exceptions, textual macros,
implicit error propagation, modules, separate compilation, or a stable ABI.
Check `docs/grammar.ebnf` for the implemented surface before designing around a
C++ feature.
