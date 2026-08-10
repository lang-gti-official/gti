# GTI Cross-Phase Architecture

Use this reference for boundaries that span compiler phases, the runtime,
tooling, project orchestration, or releases. For the exact implemented phase
order, semantic prepasses, HIR worklist, MIR lowering, and backend transition,
use [compiler-internals.md](compiler-internals.md). That file is the
authoritative implementation pipeline map.

## Contents

- [Repository Boundaries](#repository-boundaries)
- [Source And Token Contracts](#source-and-token-contracts)
- [Parser And AST Contracts](#parser-and-ast-contracts)
- [Semantic And Target Contracts](#semantic-and-target-contracts)
- [C++ Backend Contracts](#c-backend-contracts)
- [Standard Library And Runtime Boundary](#standard-library-and-runtime-boundary)
- [CLI Boundary](#cli-boundary)
- [LSP And Editor Boundary](#lsp-and-editor-boundary)
- [Project Build Boundary](#project-build-boundary)
- [Version And Release Boundary](#version-and-release-boundary)

## Repository Boundaries

- Keep reusable compiler declarations and data models under `include/gti/` and
  non-template implementations under `src/compiler/`. `gti_compiler` is a
  compiled static library; lexer implementation plus MIR integrity, MIR
  printing, effect classification, and the identity optimizer entry point are
  compiled there. Follow
  [the compiler library migration proposal](../../../../docs/compiler-library-migration-proposal.md)
  for further movement.
- Keep `src/cli/` and `src/lsp/` as thin consumers of reusable APIs, and keep
  native build policy in `src/driver/`. Executable and protocol policy must not
  leak into lexer, parser, semantics, HIR, or MIR.
- Build the compiler and tools as C++20. Generated programs target C++23 by
  default; C++20 remains a supported backend mode.
- Treat `include/gti/diagnostic.h` as shared infrastructure rather than a
  compiler phase. It owns `SourceSpan`, `Diagnostic`, related locations,
  fix-its, and `SourceManager`.
- Keep portable public APIs as GTI source under `stdlib/`, compiler-private
  capabilities under `gti_internal`, the narrow native ABI under `runtime/`,
  and C++ representation choices in the backend.

## Source And Token Contracts

- `SourceLoader::load()` canonicalizes source units, resolves includes
  recursively, records explicit dependency edges, removes include directives,
  and retains one EOF-terminated token stream per unit. The prelude is an
  implicit dependency of each non-prelude unit.
- Resolve quoted includes relative to their source. Resolve `<std/name>` only
  beneath configured GTI standard-library roots and retain its logical import
  name for diagnostics. Neither form is textual inclusion or native header
  lookup.
- Parse source units independently. `Frontend::analyze()` assembles a
  dependency-ordered transitional `Program`, while each `SourceUnit` retains its
  declaration range and identity.
- Expose declarations only to their declaring unit, direct include consumers,
  and prelude consumers. Do not infer visibility from the combined whole-program
  AST. Use `GTI-S2024` and the include edge when diagnosing a hidden declaration.
- Keep includes out of the AST. Change `SourceLoader` and `SourceGraph` for
  include behavior rather than teaching `Parser` or `CppEmitter` about it.
- The CLI and LSP load `stdlib/prelude.gti`. Tests that construct `Lexer` and
  `Parser` directly do not receive a prelude automatically.
- Preserve each token's source path, one-based line, and byte offset. Diagnostic
  spans use source-local byte offsets with an exclusive end. Convert to display
  columns or zero-based UTF-16 only at the CLI or LSP boundary.
- Use stable phase-prefixed diagnostic codes (`L`, `I`, `P`, `S`, and backend
  `B` where appropriate), related declarations or include sites, and only
  mechanically correct fix-its.
- The lexer discards comments. The formatter and editor highlighting scan
  comments separately, so comment-sensitive syntax can require multiple scanner
  updates.
- Normalize canonical fixed-width `_t` spellings and compatibility aliases to
  the same primitive token kinds. Diagnostics, semantic printers, standard
  library source, examples, and the formatter use `_t`.

## Parser And AST Contracts

- Keep grammar, precedence, AST construction, and synchronization in
  `Parser`. Keep lookup, type compatibility, mutability, inheritance validity,
  and call selection out of it.
- `Parser::parse()` recovers after declaration errors and retains later valid
  declarations. Add synchronization coverage when introducing a declaration or
  statement boundary.
- Stop code generation after any parse error. The LSP may ask semantics to
  analyze recovered declarations so independent editing diagnostics remain
  visible.
- Keep `Parser::parseExpression()` usable as a focused test and tooling entry
  point.
- Parse every compile-time target branch. Syntax errors in inactive branches
  remain errors.
- AST children own their subtrees. Preserve base specifiers, class/interface
  kind, virtual/pure/override syntax, and structured constructor initializers as
  syntax; semantics decides whether they are valid and what they mean.
- Add each concrete node to every relevant `ExprVisitor` or `StmtVisitor`,
  semantic handler, HIR dispatch, AST printer, and transitional emitter path.
  The change guide lists the full impact.
- Keep `ExternCDecl` as the syntax-preserving linkage-block wrapper and C
  linkage as metadata on each enclosed `FunctionDecl`. Parser recovery must
  stop at the closing linkage brace and preserve following declarations.

## Semantic And Target Contracts

- Use `SemanticModel` side tables as the resolved source of truth. Preserve
  expression type/category/access/traits, binding metadata, selected calls,
  operators and constructors, lifecycle decisions, source symbols, and
  occurrence roles for downstream consumers.
- Resolve inheritance before inherited members, class types, lifecycle, and
  body analysis. Keep explicit public base identity, class/interface kind,
  abstract and polymorphic state, override roots, overload-lookup owner,
  dispatch mode, and structured base construction in semantic metadata.
- Permit at most one state-bearing class/struct base plus interface bases; an
  interface inherits only interfaces. Reject omitted/private inheritance,
  duplicate bases, cycles, and diamonds before HIR. Do not ask C++ to diagnose
  or realize unsupported inheritance shapes.
- Keep interfaces public and behavior-only. Validate pure contracts, exact
  virtual roots and overrides, receiver mutability, operator identity, return
  type, and method-generic restrictions in semantics.
- Keep abstract values unconstructible and prohibit slicing. Derived-to-base
  conversion is limited to explicit reference initialization and reference
  return; ordinary calls retain exact argument matching.
- Use compiler-generated polymorphic destruction and lifecycle metadata rather
  than relying on incidental C++ destructor rules.
- The implemented range-for subset accepts stable lvalue ranges whose ordinary
  exact member protocol yields self-contained iterator/sentinel values or a
  confined imported-standard-library iterator retaining one checked read-only
  storage borrow. Parser-generated core bindings stay absent from semantic
  occurrences and completion, diagnostics map to the source colon, HIR retains
  `RangeFor` provenance, and MIR reuses the normal loop CFG. Follow
  [the iterator/range proposal](../../../../docs/iterator-range-proposal.md)
  before adding fixed-array iteration, temporary ownership, mutable/general
  owner-tied iterators, dedicated iteration loans, or precise invalidation
  rules.
- Keep source-facing tooling facts in `SemanticDatabase`. `SymbolId` values are
  valid only for the immutable `FrontendResult` snapshot that owns them.
- Recheck concrete ownership-sensitive generic bodies through the semantic
  analyzer while HIR discovers instances. Do not implement a second type system
  in HIR, MIR, the backend, or LSP.
- Validate C linkage in semantics: bodyless namespace-scope free functions,
  exact program-global symbols, fixed-width scalar returns/parameters,
  one-level scalar/`void` raw pointers, and the non-retained
  `std::string_view` counted-input exception. Mark pointer-bearing calls unsafe
  while leaving scalar/counting-input calls safe. Preserve linkage and
  `externalSymbol` in `FunctionInfo`, HIR, and MIR. Do not let a backend invent
  native linkage from a bodyless declaration or name spelling.
- Use `ConditionalStmt::activeBranch(TargetInfo)` consistently. Pass equivalent
  target data to semantics, optimization, and emission so selected branches do
  not diverge.
- Follow [language-contract.md](language-contract.md) for language behavior and
  `docs/ownership.md` for ownership and lifetime rules. This architecture file
  intentionally does not duplicate their other feature constraints.

## C++ Backend Contracts

- `BackendInput` carries checked AST, `SemanticModel`, typed HIR, MIR,
  optimization decisions, and target. New backends implement `Backend`; do not
  branch throughout the frontend.
- Follow
  [the optimization architecture proposal](../../../../docs/optimization-architecture-proposal.md)
  for the staged transition from typed-HIR source replacements to one optimized
  MIR snapshot consumed by every backend.
- Treat current AST traversal in `CppEmitter` as transitional. It consumes
  resolved semantic identities, HIR-to-source mappings, and optimization side
  data, while MIR consumption migrates incrementally. Do not infer meaning from
  source spelling inside the emitter.
- Use `std::expected` for C++23 and vendored `nonstd::expected` for C++20 while
  preserving identical GTI semantics.
- Lower GTI namespace `std` to `gti_std`; user declarations cannot be added to
  C++ namespace `std`.
- Lower immutable bindings to physical C++ `const` only when recorded semantic
  facts do not require movable storage. Explicitly consumed values, move-only
  owners, and aggregates must remain physically movable even when the GTI
  binding is semantically immutable.
- Do not lower raw-pointer binding immutability as pointee `const`. Emit
  read-only pointee qualification only from semantic `const T*`, and consume
  validated unsafe-operation facts before emitting address formation, raw
  access, pointer arithmetic, or a pointer-bearing C call.
- Emit compiler-owned special members explicitly from lifecycle metadata rather
  than inheriting C++ suppression rules. Enforce GTI field immutability in the
  frontend rather than through physical field `const`.
- Emit interfaces as C++ classes and every inherited base as public. Emit
  virtual roots, pure declarations, exact overrides, structured base
  construction, and virtual destruction from frontend/HIR metadata. Treat the
  C++ virtual ABI as representation rather than GTI semantics.
- Preserve one dispatch name for a virtual contract while ordinary methods use
  semantic function IDs. Emit the resolved static or virtual dispatch recorded
  by semantics/HIR; do not repeat override or overload selection in C++.
- Represent declared cleanup with an active-drop state and non-throwing cleanup
  helper until MIR and a future backend own the full realization. Generated
  moves transfer the state; move assignment cleans the active target first.
- Treat C++ templates as representation for validated GTI generic instances,
  not as the source of constraints, deduction, diagnostics, or overload
  selection.
- Mangle ordinary functions and methods from resolved function identities and
  emit calls to those identities. Valid root `main`, legacy runtime bindings,
  and C-linkage functions retain their required external names. For C linkage,
  emit the recorded exact symbol and canonical prototype; do not qualify it
  with the GTI namespace. Do not delegate GTI overload, operator, or symbol
  selection to C++.
- Emit range-for through its frontend-resolved core calls, never native C++
  range lookup. Keep generated range, iterator, and sentinel names reserved and
  backend-private.
- Lower explicit numeric conversions, integer arithmetic and mutation, modulo,
  shifts, indexing, owner access, storage access, and other checked operations
  through helpers that preserve GTI edge behavior. Constant frontend rejection
  and dynamic checks must agree.
- Represent fixed arrays privately, currently with `std::array`, without
  exposing pointer decay or a GTI raw-data surface.
- Treat compiler-private storage and unique-owner helpers as backend RAII
  representations for semantic intrinsic operations. Variadic storage
  construction must consume the exact nested constructor selected by semantics
  and carried through HIR/MIR; the backend does not repeat selection. These
  helpers are not GTI ABI commitments.
- Emit no ordinary body for a runtime-bound declaration. Include the runtime
  adapter only when a validated binding requires it.
- Include `<gti/c_abi.h>` only when an emitted C-linkage prototype has a
  string-view input. Convert that argument to `gti_c_string_view` at the call;
  do not expose the backend `std::string_view` representation as a C ABI.

## Standard Library And Runtime Boundary

- Put implicitly available ordinary GTI APIs in `stdlib/prelude.gti`. Put
  optional source units beneath `stdlib/std/` and import them through
  `<std/...>`.
- Keep public policy, ownership ergonomics, logical size/capacity/engagement,
  and container behavior in nominal GTI classes under `std`. Give internal
  capabilities only operations that ordinary GTI cannot yet express safely.
- Bind compiler capabilities by trusted semantic declaration identity, not by
  public wrapper or function spelling. Bind native services through selected
  C-linkage declarations and their recorded exact external symbol.
- Use the public C-compatible record definitions in
  `runtime/include/gti/c_abi.h`, runtime entry prototypes in
  `runtime/include/gti/runtime.h`, and host behavior in `runtime/src/`. Keep
  portable formatting, algorithms, ownership, and policy in GTI source.
- Keep `@runtime("...")` compiler-validated and restricted to its known
  bodyless compatibility set. The prelude's host services now use bounded
  `extern "C"`; neither mechanism is permission to infer native behavior from
  a call-site name.
- Keep string literals as trivial counted `std::string_view` values over static
  storage. Lower a C-linkage string-view parameter through
  `gti_c_string_view { data, length }` as a read-only, non-retained call input.
  Keep `std::string` source-defined over private storage.
- Keep `std::vector<T>` source-defined over private storage as well. Its
  logical size/capacity, growth, checked access, emplacement policy, and
  iterator shape remain ordinary GTI library behavior; no compiler phase may
  recognize the public vector name.

## CLI Boundary

- Let the CLI own argument parsing, diagnostic and command presentation, and
  exit-status policy. Route direct compilation through immutable
  `lang::driver::CompilationRequest` and `NativeCompileRequest` values.
- Let the compiled `gti_driver` layer own build-tree and installation resource
  discovery, generated-artifact lifetime, native command construction, and
  process execution. Keep those dependencies out of `gti_compiler`.
- Resolve installed resources from the OS-reported executable path and
  canonicalize symlinks. Treat bare `argv[0]` and build-tree paths only as
  development fallbacks. Release binaries must not embed CI checkout paths.
- Capture and suppress successful native compiler output by default because its
  locations refer to generated C++. Replay it in verbose mode. Always replay
  failed native compiler output, retain the generated C++ file, and report its
  path.
- Keep direct compilation permanent and manifest-independent. Do not discover
  a nearby project manifest for `gti source.gti`.
- Route `gti build` and `gti run` through the driver-owned manifest parser,
  `ProjectBuildPlan`, and shared `ExecutableBuildRequest`. Route `gti check`
  through the shared frontend-only driver request. Do not duplicate project
  schema or executable-build sequencing in the CLI.
- Let the project resolver own effective package/profile/target `NativeInputs`.
  Match platform fragments against the resolved `TargetInfo`, contain
  structured paths within the package, retain heterogeneous link-operand
  ordering, and pass the resulting value through `ExecutableBuildRequest`.
  Argument arrays are trusted exact argv, not shell text or implicit paths.
- Keep process invocation in `gti_driver`. Native tools use captured output;
  executed project programs inherit standard streams and receive exact argument
  vectors without a shell.
- Keep `gti metadata` read-only and multi-target. It may enumerate plans but
  must not compile or create output directories. Metadata schema 2 includes
  effective native categories and ordered operands. Keep `gti clean` usable
  with a malformed manifest while restricting removal to the discovered
  package's validated, non-symlinked `build/gti` subtree.

## LSP And Editor Boundary

Use `$gti-lsp-architecture` for detailed snapshot, query, scheduling, semantic
token, completion, navigation, and protocol work.

- Enter compiler analysis through `Frontend`. Retain immutable
  `FrontendResult` snapshots and use `language_queries.h`; do not duplicate
  lookup, type rendering, overload selection, or occurrence resolution in the
  LSP driver.
- Snapshot all open buffers as source overrides so root and dependency analysis
  sees coherent text. Reject stale generations and clear invalidated dependency
  diagnostics.
- Keep the JSON-RPC loop responsive. Run full diagnostics on coalesced immutable
  snapshots and avoid repeating analysis for `didSave` after full-sync
  `didChange`.
- Convert source-local byte offsets to zero-based UTF-16 only at the protocol
  boundary. Cache lexical line indexes by document generation.
- Let Tree-sitter own structural syntax and LSP semantic tokens own resolved
  roles. Keep `tree-sitter-gti/`, `queries/gti/`, the semantic-token legend,
  Neovim highlight links, and `syntax/gti.vim` fallback aligned.
- Regenerate `tree-sitter-gti/src/parser.c` after grammar changes. Reject
  `ERROR` or `MISSING` nodes in valid examples.
- Delegate formatting to `lang::Formatter` and honor LSP indentation options.
- Keep `:GTIInfo`, installer resolution, release metadata, parser selection, and
  active LSP version useful for diagnosing released-tool mismatches before
  changing compiler behavior.

## Project Build Boundary

Use `$gti-build-architecture` and `docs/build-system-proposal.md` for manifests,
project commands, profiles, native linking, caches, workspaces, dependencies,
lockfiles, or package resolution. Treat proposal milestones as unimplemented
until present in code.

- Build project mode around the same immutable whole-program compiler request;
  do not create a second frontend/backend pipeline.
- Keep manifest parsing, workspace discovery, planning, artifact storage,
  native process execution, and dependency acquisition in a compiled driver
  layer rather than the compiler frontend library.
- Let `SourceLoader` continue to own source-unit identity and direct visibility.
  A manifest describes roots and targets; it does not flatten declarations.
- Compile one target from one entry source and its complete `SourceGraph` until
  separate compilation and ABI boundaries are deliberately designed.
- Treat profile declarations as definitions, not selection. Plain project
  commands choose `dev`; `--release` is the exact alias for selecting the
  built-in `release` profile, and only selected output directories are created.
- Keep LSP project discovery read-only. It must not fetch, build, clean, run
  hooks, or mutate lockfiles.
- Add caching only after uncached builds are deterministic. Hash every semantic
  and native input, including source contents, configuration, target, toolchain,
  runtime, native inputs, and locked dependencies.

## Version And Release Boundary

- Treat `VERSION` as the single release source of truth. CMake propagates it to
  the CLI and LSP; installers and `:GTIInfo` use it to identify the selected
  toolchain.
- Advance `VERSION` for shipped compiler, standard-library, runtime, LSP,
  formatter, Tree-sitter, Neovim, project, dependency, or native-driver behavior.
- Verify `gti --version`, `gti_lsp --version`, and LSP `serverInfo.version`
  against `VERSION`.
- A `VERSION` change on `main` starts `.github/workflows/release.yml`. Its
  publish job creates the matching tag only after platform packages pass. Do
  not race it with a manual tag.
- Do not describe a release as available until all platform archives and
  checksums are published. Release tests must exercise installed binaries and
  packaged runtime, standard library, parser, metadata, and licenses.
- Lazy configurations using `version = "*"` run the newest released tag, not
  arbitrary `main`. For editor reports, inspect `:GTIInfo`, update the plugin,
  and restart the LSP before diagnosing source behavior.
