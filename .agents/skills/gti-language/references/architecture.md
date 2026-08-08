# GTI Compiler Architecture

Read this reference before changing an unfamiliar phase. Confirm details in the
code because this project is evolving.

## Pipeline Map

| Phase | Primary API | Input -> output | Owns |
| --- | --- | --- | --- |
| Source loading | `include/gti/source_loader.h`, `source_graph.h` | entry path + prelude paths -> source-unit dependency graph | file reads, relative includes, canonicalization, load-once behavior, cycles, include placement, direct visibility edges |
| Lexing | `include/gti/lexer.h`, `token.h` | source text -> `vector<Token>` | spelling, literals, byte offsets, source path, line numbers, lexical diagnostics |
| Parsing | `include/gti/parser.h` | tokens -> `Program` | grammar, precedence, AST construction, parse diagnostics, synchronization |
| AST | `include/gti/ast.h` | syntax model | node ownership, `ExprVisitor`, `StmtVisitor`, target-condition structure |
| Semantics | `include/gti/semantic_analyzer.h` | `Program` -> diagnostics + `SemanticModel` | scopes, namespaces, symbols, expression types, mutability, result use, expected rules, runtime binding validation |
| Frontend | `include/gti/frontend.h` | entry source -> `FrontendResult` | shared phase ordering, checked-program ownership, typed HIR, source map, aggregate diagnostics |
| HIR | `include/gti/hir.h` | checked AST + semantics -> `HirProgram` | executable bodies, stable statements/values/bindings, concrete class/callable/destructor instances, resolved edges, ownership-aware generic rechecking |
| Optimization | `include/gti/optimizer.h` | typed HIR -> `OptimizationResult` | target-aware, semantics-preserving decisions keyed by `HirValueId` |
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

The checked AST preserves source structure and semantic side tables. Typed HIR
is the current backend-independent instance representation for generic
monomorphization and stable symbol IDs. Introduce a layout-resolved control-flow
MIR before adding LLVM emission.
See `docs/compiler-architecture.md` for the staged backend roadmap.

## Source And Token Contracts

- `SourceLoader::load()` creates canonical source units, recursively resolves
  include edges, removes include directives, and retains one EOF-terminated
  token stream per unit. The prelude is an implicit dependency of every
  non-prelude unit.
- Quoted includes resolve relative to their source. Angle imports such as
  `<std/array>` resolve only beneath configured GTI standard-library roots and
  retain `std/array` as source-unit metadata for diagnostics. Neither form is
  textual inclusion or a native C++ header lookup.
- `Frontend::analyze()` parses every unit independently and assembles a
  dependency-ordered transitional `Program`. Each unit retains its declaration
  range in `SourceGraph`.
- Semantic visibility includes the declaring unit, direct include edges, and
  prelude units only. Transitive and sibling dependencies do not leak names.
  `GTI-S2024` identifies a hidden declaration and suggests the required direct
  include.
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
- Scoped enums have nominal IDs, fixed integral backing types, and evaluated
  enumerator metadata in both `SemanticModel` and HIR. Enumerator lookup is
  type-scoped, including through type aliases, and never falls back to native
  C++ conversion or name-injection behavior.
- Switch analysis records normalized same-type case constants, rejects
  duplicate labels and reachable arm boundaries, and gives every arm an
  independent scope. HIR retains the subject, grouped labels, constants, and
  arm statement IDs; backends do not reconstruct switch semantics from AST
  spelling.
- Constructors form overload sets by normalized parameter types. Construction
  is an explicit `Type(arguments)` call resolved by one exact match;
  constructor-based implicit conversion and conversion ranking are not part of
  assignability.
- Constructor initializer lists initialize fields in declaration order before
  the body. Fields omitted from the list require declaration initializers, and
  `this` and members are unavailable until the body begins.
- A generated `Type()` is available when no zero-argument constructor is
  declared and every field has a declaration initializer, even when other
  constructor overloads exist. Class-valued variables always require an
  explicit construction expression.
- `SemanticModel` records every class lifecycle explicitly: its declared
  constructor overloads, generated or deleted default constructor, copy/move
  constructors, copy/move assignments, declared or generated destructor,
  active-drop requirement, and field-derived traits. It separately records the
  selected constructor identity for each valid construction call. Copy and move
  lifecycle constructors are compiler-owned.
- One public `~Type()` declaration may provide automatic cleanup. Its body has
  an implicitly mutable receiver, cannot return, is non-throwing, and runs only
  for the active value before fields are destroyed in reverse order. Declared
  cleanup makes the type noncopyable. Generated moves transfer active-drop
  state, and move assignment cleans the old target before replacing fields.
  Destructors have no expression form and cannot be called manually.
- Methods have read-only receivers by default. A trailing `mut` method may
  mutate mutable fields and can only be called through a mutable receiver.
  A leading `mut` on a reference return requires that mutable receiver and a
  writable place derived from `this`.
  `this` is a non-null object expression with `.` access in GTI; its C++ pointer
  representation remains a backend detail. `self` is an ordinary identifier.
  Private access remains available from methods and constructors of the owning
  type.
- Classes, structs, methods, and functions may declare named type parameters
  directly after their name. A type parameter may carry one standard constraint
  before its name, such as `std::numeric T`. Constraints are semantic identities,
  not name-looked-up library declarations: the supported set and implication
  hierarchy live in `SemanticVisitor`, and argument checking does not rank
  overloads. Classes and structs may follow type parameters with immutable
  `uint64` value parameters. Applied class types are nominal and require exact
  type and value arity. Function type arguments are either explicit or inferred
  exactly from argument types; inference does not use return context or
  conversions.
- Generic bodies are structurally checked with type-parameter identities, then
  fixed generic function and constructor instances are ownership-checked again
  in HIR with concrete substitutions. Member lookup substitutes an applied
  class's arguments into its fields, methods, and constructor overloads. Class
  value arguments are integer literals or enclosing value parameters and may
  become fixed-array extents or nested class arguments. HIR includes them in
  concrete class identity and substitution. Standard unary constraints are
  checked on concrete substitution and symbolic forwarding; user-defined or
  combined constraints, specialization, value generic functions, and arbitrary
  constant expressions remain outside the current generic model. Local `auto`
  inference is initializer-driven and does not participate in generic deduction.
  `SemanticModel::findBinding()` and HIR
  bindings retain its exact type, access mode, and ownership traits; inferred
  move-only copies must fail before backend entry.
- Namespace-scoped `using Name = Type;` declarations are transparent aliases.
  Register their names before resolving targets, diagnose cycles in semantics,
  and canonicalize them before overload, ownership, and HIR decisions. Keep
  reference and generic aliases outside the initial alias layer.
- Lambdas have explicit parameter and return types and named immutable value
  captures. `SemanticModel` records each closure signature, capture declaration,
  capture type and traits, and resolved exact calls. Lambda values remain local
  and non-escaping; reference/default/init captures, `this`, mutable captures,
  and noncopyable captures are rejected.
- A variadic function or method may have one final generic type pack and one
  matching final immutable by-value parameter pack. Calls infer an ordered
  exact type sequence, and a symbolic pack can only be forwarded as the final
  argument to another variadic callable. Keep arbitrary expansion, class packs,
  folds, indexing, multiple packs, and forwarding-reference deduction outside
  this layer; do not defer invalid generic bodies to C++ template errors.
- Move-only fixed generic arguments are supported through concrete HIR
  rechecking. Move-only pack elements remain rejected until HIR models a pack
  parameter as an ordered set of owned bindings.
- Free functions, namespace functions, and methods form overload sets by name.
  A declaration is unique by its normalized parameter types and generic arity;
  return types, parameter names, by-value `mut`, and ordinary method receiver
  mutability do not distinguish signatures. Restricted member operators may
  pair read-only and mutable receiver overloads. Calls require one exact match
  after generic substitution. There is no conversion ranking or
  concrete-over-generic preference.
- Function calls never use assignment compatibility. Convert numeric arguments
  explicitly with `Type(value)`; checked narrowing is represented by a
  `Conversion` AST node and lowered through the backend numeric-cast helper.
- Semantic flow analysis rejects reachable fallthrough from non-`void`
  functions and lambdas before HIR lowering. It combines reachable `if`
  branches, follows only the active target-condition branch, consumes `break`
  at the nearest switch or loop, and treats only proven non-exiting loops as
  terminating.
  Top-level `main` retains its defined implicit zero return and currently
  requires a body with exact signature `int main()`.
- `SemanticModel` assigns stable per-program function IDs and records the
  selected declaration and instantiated signature for each resolved call.
- Restricted `operator*`, `operator->`, `operator[]`, `operator==`,
  `operator!=`, and contextual `operator bool` declarations are member-only.
  Semantics resolves exact operands and receiver access, records the selected
  function ID and result access, and does not provide ADL, implicit
  conversions, recursive arrow proxies, or synthesized equality candidates.
- Fixed array declarators normalize into semantic array types whose element and
  compile-time extent participate in exact identity. Array elements are places
  whose access follows the containing expression; array values inherit element
  copy, move, and drop traits. There is no pointer decay or raw-data member.
- `SemanticModel` records expression value/place category, read/write access,
  ownership, transferability, and drop requirements alongside resolved types.
  It records equivalent facts for variable and parameter bindings. Preserve
  these facts when introducing references, move checking, HIR, or MIR.
- Class and struct traits are derived recursively from their fields after class
  generic substitution. A nested aggregate containing a unique field is itself
  move-only and participates in the same copy rejection and flow-sensitive move
  tracking as a direct owner handle.
- References and unique owners are source-reachable with conservative lifetime
  and move-state checks. Shared ownership remains semantic groundwork. Keep
  representation choices in the backend and follow the staged limitations in
  `docs/ownership.md`.
- A method may return `T&` only from a place derived from `this`; a `mut T&`
  return additionally requires a mutable method and writable place.
  `ResolvedCallInfo` records whether a borrowed result originates from the
  receiver or an intrinsic argument; call and resolved-operator expressions
  expose the referent as a place with recorded access. Reject retained borrows
  from temporary receivers and reject later invalidation of a borrowed
  move-only root conservatively through the function boundary. Free-function
  reference returns remain unsupported.
- `gti_internal::storage<T>` is the compiler-private move-only owner for
  partially initialized container capacity. Its allocate, capacity, construct,
  borrowed read-only and mutable access, destroy, and relocate calls are
  semantic intrinsics recorded in `ResolvedCallInfo`; keep raw addresses and
  independent deallocation out of GTI source.
- `gti_internal::unique_owner<T>` is the compiler-private handle beneath the
  nominal `std::unique_ptr<T>` class. Allocation, checked read-only and mutable
  borrows, and null observation are semantic intrinsics. Public dereference,
  arrow, comparison, and boolean behavior must continue to resolve through the
  stdlib class operators.
- Treat `gti_internal` as a backend-neutral capability layer for implementing
  safe nominal classes under `std`, not as the public standard library itself.
  Bind capabilities by trusted declaration identity rather than wrapper name.
  A future opt-in `dangerous` API may re-export an audited subset, but its
  syntax and contracts, including any `new`/`delete`-like surface, are not yet
  language commitments.
- Modulo and bitwise operators require integer operands. Binary operations use
  existing integer promotions and safe signed/unsigned common types; shifts
  return the promoted left type and validate literal counts during semantics.
- The lexer normalizes `and`/`&&` to `TokenKind::AND` and `or`/`||` to
  `TokenKind::OR`. Downstream phases must use those identities so both source
  spellings retain identical precedence, boolean rules, and short-circuiting.
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
- Immutable bindings lower to `const` only when their recorded semantic traits
  remain copyable. Move-only owner handles and aggregates stay physically
  movable in C++. Trivial counted string views lower by value.
- Declared constructors lower to `explicit` C++ constructors. The backend emits
  every compiler-owned special member explicitly as `= default` or `= delete`
  from lifecycle metadata, so C++ declaration-order suppression rules cannot
  change GTI semantics. GTI field immutability is frontend-enforced rather than
  represented by C++ `const`, which keeps validated whole-object assignment and
  movement available. Read-only methods lower with a trailing C++ `const`; GTI
  trailing-`mut` methods lower without it.
- Declared cleanup lowers through a private active-drop flag and non-throwing
  cleanup helper. Generated C++ moves transfer that state explicitly so moved
  sources still destroy their fields but do not repeat the GTI destructor body.
  This is the current C++ representation of a frontend drop rule that belongs
  in MIR for a future LLVM backend.
- Validated named generic declarations and confined function packs lower to C++
  template declarations and applied types. C++ templates are a backend
  representation, not the source of GTI generic semantics or diagnostics.
  Standard GTI constraints are already enforced by the frontend and do not
  lower to C++ concepts or participate in C++ overload resolution.
  Forwarded by-value packs copy copyable elements and transfer noncopyable
  movable elements; do not expose forwarding-reference deduction in GTI.
- The C++ backend mangles ordinary function and method names from semantic
  function IDs and emits calls through the recorded selected declaration.
  C++ overload resolution is not part of GTI semantics. Runtime bindings and a
  valid root `main` retain their required external names.
- Resolved GTI operators lower to direct calls to those mangled method IDs.
  They do not lower as C++ operator declarations or rely on C++ overload
  resolution.
- Explicit numeric conversions lower through checked generated helpers.
  Integer and float narrowing must not invoke C++ undefined behavior.
- Fixed arrays currently lower to backend-private `std::array`
  representations. Indexed access goes through a generated bounds helper, and
  `size()` lowers to the compile-time extent without a stored GTI length.
- Modulo and shifts lower through generated checked helpers. Dynamic zero
  divisors and invalid counts terminate with a GTI runtime diagnostic; signed
  minimum modulo `-1`, wrapping left shift, and arithmetic signed right shift
  have defined GTI behavior.
- Compiler-private storage currently lowers to an aligned C++ RAII helper that
  tracks live slots. Its checked read returns a `const T&` tied to the storage.
  Treat allocation, borrowing, construction, destruction, and relocation as
  semantic operations so MIR and LLVM can replace that helper.
- Runtime-bound declarations emit no ordinary function body. Their presence
  causes the runtime adapter header to be included.
- Generated C++ is an implementation artifact, not the language specification.
  Do not expose C++ quirks as GTI behavior without a language-level reason.

## Standard Library And Runtime Boundary

- `stdlib/prelude.gti` contains implicitly available ordinary GTI APIs.
  Optional units live under `stdlib/std/`, are installed as source, and are
  imported directly with `<std/...>`. Public APIs live under `std`;
  compiler-owned declarations live under `gti_internal`.
- Keep safe policy, ownership ergonomics, and container behavior in nominal
  GTI classes under `std`. Internal capabilities provide only the primitive
  operations those classes cannot express safely yet.
- `@runtime("...")` is a compiler-validated, bodyless declaration for a known
  native service. Do not make it a general foreign-function escape hatch by
  accident.
- `runtime/include/gti/runtime.h` defines the narrow C ABI.
- `runtime/include/gti/runtime.hpp` adapts C ABI calls to emitted C++ types.
- `runtime/src/` implements host behavior. Keep portable formatting, algorithms,
  and policy in GTI where possible.
- `char` is a distinct unsigned 8-bit GTI code-unit type. String literals have
  static storage and the canonical trivial type `std::string_view`, defined by
  the prelude over compiler-private `gti_internal::text_view`. The C++ backend
  represents that type as `std::string_view`; the stdout adapter alone exposes
  its data and length to the C ABI. View indexing is checked and read-only.
  `std::string` is a source-defined move-only class over
  `gti_internal::storage<char>` with explicit allocating `clone()`, not a
  compiler-recognized public type. Dynamic owner-backed views remain deferred
  until their lifetime can be tied to the owner in semantics and HIR.

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
  related notes, and help. Successful native compiler output is captured and
  suppressed by default because its locations refer to generated C++; verbose
  mode replays it. A failed native compiler invocation always replays its
  output, retains its generated C++ file, and reports that path for backend
  investigation.
- The CLI and LSP both use `Frontend`; do not duplicate source loading, parsing,
  and semantic phase ordering in either driver.
- LSP diagnostics retain document versions, exact UTF-16 ranges, stable codes,
  severity, related information, and serialized fix-it data. Diagnostics for
  includes are published on the included file URI and cleared when stale.
- The JSON-RPC request loop owns document snapshots and lightweight requests.
  Full frontend diagnostics run on a worker against immutable snapshots;
  pending edits are coalesced per root, and generation checks prevent stale
  results from replacing newer diagnostics. Full synchronization already
  delivers saved text through `didChange`, so `didSave` must not repeat the
  same analysis.
- Every analysis request snapshots all open file buffers as canonical-path
  source overlays. The source loader uses those overlays for includes, and the
  LSP tracks the loaded dependency URIs per root. An edit or close invalidates
  previous dependent diagnostics and schedules those roots against a coherent
  snapshot; generation checks retry if a dependency changed during analysis.
- LSP semantic classification is token-based and contains declaration
  heuristics. Position lookup uses a per-source line index and completed token
  streams are cached by document generation; do not reintroduce source-prefix
  scans for every token. Update the advertised legend and protocol tests
  together.
- `tree-sitter-gti/` owns the structural grammar. Its generated ABI-14 C parser
  is built as `gti.so`, shipped under `share/gti/parser/`, and loaded directly
  by the Neovim plugin. `queries/gti/` owns syntax highlighting, indentation,
  and folds, while LSP semantic tokens add resolved symbol roles. Keep GTI's
  structural capture taxonomy aligned with Neovim's C/C++ queries where the
  constructs have the same role. In particular, retain the low-priority
  `@variable` capture for ordinary identifiers so a theme never depends on the
  LSP merely to highlight expression references. Keep `syntax/gti.vim` as a
  small regex fallback rather than a second parser.
- LSP formatting delegates to `lang::Formatter` and honors `tabSize` and
  `insertSpaces`.
- `plugin/gti.lua` registers `.gti`, loads Tree-sitter, starts the server, maps
  semantic highlight groups, and exposes `:GTIInfo`. `lsp/gti_lsp.lua` supplies
  the native Neovim 0.11 server configuration.
- `ftdetect/`, `ftplugin/`, and `syntax/` provide file detection, fallback
  highlighting, comments, and fallback C-style indentation.
- `lazy.lua` is the plugin spec and `build.lua` invokes
  `lua/gti/installer.lua`. The installer downloads the archive matching the
  checked-out release's `VERSION` into the plugin-private `toolchain/`.
- `lua/gti/toolchain.lua` resolves explicit environment overrides first, then
  release-installed artifacts, then `PATH`, then local development builds. It
  resolves the parser beside an installed executable when needed.

## Version And Release Boundary

- `VERSION` is the release source of truth. CMake propagates it to the CLI and
  LSP, while the installer uses it to construct release archive URLs. Both
  executables expose `--version`, and `:GTIInfo` compares those values with the
  plugin, installed metadata, and active LSP handshake.
- Lazy specs using `version = "*"` select the newest semantic-version tag. A
  commit on `main` is therefore not delivered to those users until a matching
  tag has completed `.github/workflows/release.yml`.
- CI requires release-sensitive source or editor changes to advance `VERSION`.
  Pushing that version change to `main` runs the release workflow directly;
  its publish job creates the matching tag only after all packages pass.
- Release archives include `gti`, `gti_lsp`, `gti.so`, runtime and compiler
  support files, the standard library, `VERSION`, and licenses. Release CI tests
  the installed binaries rather than only the build tree.
- For an editor report, run `:GTIInfo` and resolve any reported mismatch before
  changing compiler behavior. Restart the LSP after Lazy updates the plugin so
  Neovim does not retain the previous process.

## Current Non-Goals

Do not assume support for user-defined or combined generic constraints,
`requires` clauses, constraint-based overload ranking, specialization, value
generic functions, value packs, arbitrary compile-time evaluation, raw pointers,
escaping or stored references, escaping or stored lambdas, reference/default
lambda captures, dynamic arrays, custom copy/move lifecycle
declarations, inheritance, exceptions, textual macros, implicit error
propagation, named modules, exports, separate compilation, or a stable ABI. The
implemented source-unit graph remains a whole-program include model rather than
a binary module system.
Check `docs/grammar.ebnf` for the implemented surface before designing around a
C++ feature.
