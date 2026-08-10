# GTI Change And Verification Guide

Use this reference to scope a change and prove it at the correct boundaries.

## Contents

- [Impact Matrix](#impact-matrix)
- [Test Selection](#test-selection)
- [Diagnostic Quality](#diagnostic-quality)
- [Completion Checklist](#completion-checklist)

## Impact Matrix

### Add A Keyword, Operator, Or Literal

Inspect and usually update:

1. `include/gti/token.h` for `TokenKind`, fixed keyword spelling, and
   `to_string()`.
2. `include/gti/lexer.h` for the lexer contract and
   `src/compiler/lexer.cpp` for recognition, literal decoding, and lexical
   errors.
3. `include/gti/parser.h` for grammar and precedence.
4. `include/gti/ast.h` and every visitor only if the syntax needs new structure.
5. `semantic_analyzer.h` for meaning, types, selected identities, invalid cases,
   and source-facing semantic facts.
6. `hir.h` for concrete typed values or statements and generic-instance
   discovery.
7. `mir.h` for scalar operations, CFG effects, places, calls, moves, loans,
   cleanup, use indexing, and validation.
8. `optimizer.h`, `backend.h`, and `cpp_emitter.h` for affected optimization or
   representation behavior.
9. `formatter.h`, `src/lsp/main.cpp`, `tree-sitter-gti/grammar.js`,
   `queries/gti/`, and `syntax/gti.vim` for formatting and highlighting.
10. `docs/grammar.ebnf`, focused tests, and an example.

Record an explicit reason when a listed IR or tooling layer is unaffected; do
not omit it because the current C++ emitter can reconstruct source spelling.

Before implementation, decide operand domains, result type, precedence,
associativity, conversion behavior, and invalid runtime cases. Do not inherit a
C++ undefined edge case such as zero division without defining GTI behavior.
Reuse an existing AST node such as `Binary` when the syntax has the same shape.
For an operator, check semantic built-in/member selection, HIR `operation`, MIR
`MirOperation`, validation, checked helpers, optimizer behavior, and
`CppEmitter::operatorSpelling()`.

Do not reserve a word if an ordinary library function or existing syntax can
express the feature.

### Add An AST Node

- Add the node and accessors in `ast.h`.
- Extend `ExprVisitor` or `StmtVisitor`.
- Implement every visitor, including semantics, C++ emission, AST printing, and
  tooling visitors where applicable.
- Add explicit HIR dispatch in `HirLowerer::lowerExpression` or
  `lowerStatement`; an unhandled expression can otherwise retain a default HIR
  kind.
- Add the corresponding MIR operation, instruction, place, or terminator
  lowering and extend MIR validation. Record why MIR is unaffected only for a
  genuinely syntax-only node.
- Add parser construction plus positive, recovery, semantic, and lowering tests
  appropriate to the node.
- Search visitor coverage with:

```sh
rg -n "visit[A-Za-z]+(Expr|Stmt|Decl)" include/gti
rg -n "dynamic_cast<const .*\*>|Hir(Value|Statement)Kind" include/gti/hir.h
rg -n "Hir(Value|Statement)Kind|Mir(Operation|InstructionKind|TerminatorKind)" \
  include/gti/mir.h
```

### Change A Semantic Rule

- Keep parsing unchanged unless the source grammar changes.
- Put the rejection and GTI-focused message in `semantic_analyzer.h`.
- Update the authoritative `SemanticModel` record when downstream phases need
  the result. Do not make HIR, MIR, the backend, or LSP re-run semantic logic.
- Check both symbolic body analysis and concrete instance reanalysis when the
  rule depends on generic substitution, ownership, packs, or class value
  arguments.
- Propagate new semantic facts into HIR and MIR where they affect instances,
  operations, places, calls, moves, loans, or cleanup.
- Add one valid case and focused invalid cases to `tests/compiler_tests.cpp`.
- Assert diagnostic count and meaningful location/message when stable.
- Ensure invalid source is rejected before invoking the native compiler.

### Change HIR Or MIR Lowering

- Read the relevant HIR or MIR internals section in
  [compiler-internals.md](compiler-internals.md).
- Preserve semantic identities and source provenance; never recover them from
  source names or tokens when `SemanticModel` already owns them.
- Keep HIR IDs stable within one program and MIR IDs local to one body. Treat
  zero as no identity.
- For HIR instance changes, test discovery through nested calls, fields,
  constructors, destructors, operators, and generic substitutions as relevant.
- For MIR changes, test block termination, reachability, cleanup on every exit,
  value definitions and indexed uses, projected places, calls, and validator
  rejection.
- Add direct structural assertions before relying on emitted C++ coverage.

### Change Inheritance, Interfaces, Or Virtual Dispatch

- Update tokens, parser/AST, `docs/grammar.ebnf`, formatter, Tree-sitter, and
  editor highlighting together when source syntax changes.
- Keep base validation, cycle and diamond rejection, inherited overload sets,
  exact override matching, abstractness, lifecycle decisions, and call dispatch
  in semantics. Start with `resolveClassInheritance`,
  `resolveInheritedMembers`, `ClassTypeInfo`, `FunctionInfo`, and
  `ResolvedCallInfo`.
- Preserve base instances, abstract/polymorphic state, virtual roots, and
  ordered base/field constructor initializers in HIR. Preserve the same class
  structure, constructor targets, `CallDispatch`, and dispatch owner in MIR.
- Keep public C++ bases, pure virtual syntax, `override`, virtual destruction,
  and base-initializer spelling as backend representation choices. Do not let
  C++ choose whether a GTI call dispatches virtually.
- Extend `testInheritanceAndInterfaces` with valid and invalid semantic cases,
  structural HIR/MIR assertions, and generated-code execution. Add formatter,
  Tree-sitter, CLI, and LSP coverage when their surfaces change.

### Change C++ Lowering

- Update `cpp_emitter.h` only for C++ representation. Add missing language facts
  to semantics, HIR, or MIR first; do not weaken frontend validation or make the
  emitter select overloads, infer ownership, or derive lifetime effects.
- Check whether the change should consume existing HIR/MIR rather than extend
  transitional AST traversal.
- Cover both C++23 and C++20 when `expected` behavior is involved.
- Assert important emitted fragments, then compile a representative `.gti`
  program through the CLI.
- Use `--emit-cpp -o /tmp/output.cpp` when inspecting generated code.

### Change Optimization Or A Backend

- Read
  [the optimization architecture proposal](../../../../docs/optimization-architecture-proposal.md)
  for pass ownership, capability gates, effect classification, analysis
  invalidation, verification, and the C++ backend migration.
- Keep the current compatibility pipeline in `optimizer.h`, growing pass
  infrastructure behind focused `optimization/` headers. Keep representation
  choices behind `Backend` implementations.
- The current constant-folding pass consumes typed HIR. Use MIR for new CFG,
  reachability, propagation, use-def, place, loan, and cleanup analyses when MIR
  contains the required facts.
- Consume typed IR, semantic records, and the selected `TargetInfo`; do not
  infer semantics from emitted C++ spelling.
- Preserve AST ownership and source provenance. Put concrete instance metadata
  in HIR and body-local effects in MIR. Do not add new source-expression side
  tables; the existing HIR replacement table is a migration bridge only.
- Classify memory, trap, call, ownership, loan, and drop effects before a pass
  removes or reorders an operation. Declare analysis invalidation and validate
  rewritten MIR after every changed pass in validation builds.
- Remember that `BackendInput` contains AST, semantics, HIR, MIR, optimizations,
  and target, while the current `CppBackend` still does not consume MIR.
- Add `-O0` preservation coverage and optimized output coverage. Compile the
  result through the CLI at the affected optimization level.
- Define arithmetic overflow and runtime edge cases before constant-folding
  them.
- Add an LLVM backend only after the shared IR expresses generic instances,
  object layout, lifetime operations, calling conventions, and control flow.

### Change Includes Or Source Loading

- Work primarily in `source_loader.h` and `source_graph.h`; preserve canonical
  source-unit identity, dependency spans, declaration ranges, and token
  provenance.
- Test relative resolution, nested includes, duplicate loads, cycles, invalid
  extensions, placement restrictions, and dependency diagnostics as relevant.
- Test direct visibility in both directions: an included file sees its own
  dependencies, while an includer and sibling units do not inherit them.
- Exercise unsaved root and included-source text through the LSP. Included
  buffers must invalidate dependent roots and override older on-disk text.

### Change Target Conditionals

- Keep condition syntax in parser/AST and target values in `target.h`.
- Parse every branch; analyze and emit only the selected branch.
- Test explicit `TargetInfo` values rather than depending only on the host.
- Keep includes forbidden inside conditionals unless the language design is
  deliberately changed.

### Change The Standard Library Or Runtime

- Put user-facing and portable behavior in `stdlib/prelude.gti` or future GTI
  library files.
- Add a native entry only for a host service that GTI cannot implement
  portably, declare it through bounded `extern "C"`, and keep an ordinary GTI
  wrapper between that entry and public `std` policy.
- Update `runtime/include/gti/c_abi.h` for shared C records,
  `runtime/include/gti/runtime.h` for prototypes, `runtime/src/` for host
  behavior, prelude declarations, CMake installation/release validation, and
  driver resource tests together. Use `runtime.hpp` only when a legacy
  compiler-owned binding still requires a C++ adapter.
- Test exact native prototypes, runtime implementation linkage, installed
  headers, and that public wrappers do not expose descriptors, pointer
  retention, or native policy.

### Change Native C Interoperation

- Read `docs/native-c-interop.md` and keep the change within its call-only
  boundary unless the language proposal is deliberately expanded first.
- Update the `extern` token, parser/AST wrapper, declaration recovery, semantic
  `LanguageLinkage` and `externalSymbol`, C symbol collision diagnostics, HIR,
  MIR, MIR printing, backend prototypes/calls, formatter, Tree-sitter grammar
  and queries, Vim syntax, LSP traversal/ranges, grammar, specification, and
  editor tests as one language slice.
- Apply the ABI allowlist to resolved semantic types, not raw spelling. Keep
  returns to `void` or fixed-width integer/float scalars; keep parameters to
  immutable by-value instances of those scalars plus the explicit non-retained
  string-view input case. Reject bool, char, enums, references, arrays, owners,
  generics, definitions, native variables, redeclarations, and overloads before
  backend entry.
- Preserve one program-global exact C symbol from `FunctionInfo` through HIR
  and MIR. A GTI namespace controls source lookup but must not qualify or mangle
  the external symbol.
- For counted text, update and C-compile `runtime/include/gti/c_abi.h`; emit the
  exact `gti_c_string_view` record prototype and convert arguments at the call.
  Document that the native callee cannot retain the data or assume a terminator.
- Test direct native linking with compiler arguments after `--` and project
  linking through package/profile/target `native` tables. Preserve explicit
  target selection, package containment for structured paths, heterogeneous
  link-operand order, exact argv elements, frontend-only `check`, and read-only
  metadata. Any future cache key must include every effective native input.
- Keep legacy `@runtime` support closed and compiler-validated; do not make it a
  competing general FFI or require it for ordinary C symbols.

### Change CLI Behavior

- Keep argument syntax and presentation in `src/cli/main.cpp`; put reusable
  compilation, native process, resource, and artifact behavior in
  `include/gti/driver/` and `src/driver/`. Update `tests/driver_tests.cpp` and
  `tests/cli_smoke_test.py` at their respective boundaries.
- Cover exit status, stderr/stdout ownership, output paths, forwarded arguments,
  and resource discovery where applicable.
- For install discovery changes, test a release-configured installed toolchain
  from outside the checkout and invoke `gti` by basename through `PATH`.
- Update README command examples for user-visible options.

### Change Project Builds, Manifests, Or Packages

- Read `docs/build-system-proposal.md` and use `$gti-build-architecture`.
- Preserve direct mode as a permanent, manifest-independent compatibility
  surface. Test existing source-first commands alongside each project command.
- Keep TOML parsing, project discovery, profiles, native toolchain policy,
  caching, workspaces, and dependency resolution outside the language parser,
  semantics, HIR, and MIR.
- Reuse the existing immutable compilation and native-toolchain requests;
  project commands must not create a second frontend/backend path or place
  manifest semantics directly in `src/cli/main.cpp`.
- Keep one entry source and its `SourceGraph` as the whole-program compilation
  unit until GTI deliberately defines binary modules and a stable ABI.
- Validate manifest paths relative to their package root. Give `clean` and
  cache eviction explicit tool-owned boundaries and refuse broad or unresolved
  deletion targets.
- Give manifest and planning failures stable `GTI-B` diagnostics with exact
  TOML spans where possible. Keep native C++ failures labeled as backend
  failures.
- Make cache keys cover every declared semantic and native input. Verify cache
  invalidation before optimizing cache performance.
- Keep LSP project discovery read-only: opening a document must not fetch,
  build, clean, execute hooks, or mutate a lockfile.
- Add manifest and resolver coverage in `tests/project_tests.cpp`, project CLI
  coverage in `tests/project_cli_smoke_test.py`, and direct compatibility
  coverage in `tests/cli_smoke_test.py`. Exercise package-root, nested-directory,
  installed-toolchain, locked, and offline scenarios as their milestones land.
- Advance `VERSION` when shipped CLI, project, dependency, or native-driver
  behavior changes.

### Change Compiler Library Or Header Boundaries

- Read
  [the compiler library migration proposal](../../../../docs/compiler-library-migration-proposal.md)
  and preserve its phased, behavior-neutral migration contract.
- Keep reusable declarations, value models, templates, and small accessors
  under `include/gti/`; move substantial non-template algorithms to the owning
  subsystem under `src/compiler/`.
- Make the implementation source include its own public header first. Do not
  rely on consumer include order for missing dependencies.
- Keep CLI, LSP, and tests linked to the same `gti_compiler` implementation.
- Preserve exact-version installed headers and `libgti_compiler.a` together;
  update release archive requirements and installed-library smoke coverage when
  the target layout changes.
- Measure implementation-only rebuilds and representative compiler latency.
  Use profiles before retaining a large inline body for performance.
- Do not combine a mechanical header/source migration with semantic,
  diagnostic, generated-code, or CLI behavior changes.

### Change LSP, Formatting, Or Highlighting

- Keep diagnostics on the same source-loader/parser/semantic pipeline as the
  CLI.
- Update LSP capability advertisement and `tests/lsp_smoke_test.py` together.
- Keep semantic token enum order identical to the advertised legend.
- Keep Tree-sitter syntax highlighting available independently of the LSP and
  keep semantic tokens enabled as the type-aware layer above it.
- Regenerate the committed ABI-14 parser with `npm run generate` from
  `tree-sitter-gti/`, run its corpus tests, and parse every valid `.gti` example
  without `ERROR` or `MISSING` nodes when the grammar changes.
- Run `npm run test:highlights` from `tree-sitter-gti/` when structural capture
  queries change. Add position-specific fixture coverage for new or corrected
  roles instead of checking only that the query compiles.
- Add formatter idempotence coverage and preserve comments and strings.
- Update `queries/gti/highlights.scm`, `plugin/gti.lua` semantic links, and
  `syntax/gti.vim` fallback syntax for new token roles. Check
  `lsp/gti_lsp.lua` when startup behavior changes.
- Run `tests/nvim_plugin_smoke_test.lua` for changes under `plugin/`, `lsp/`,
  `lua/gti/`, `queries/gti/`, `ftdetect/`, `ftplugin/`, `syntax/`, `lazy.lua`,
  or `build.lua`.
- Headlessly load editor files when they change:

```sh
XDG_STATE_HOME=/tmp/gti-nvim-state nvim --headless -u NONE -n -i NONE \
  --cmd 'set runtimepath^=.' \
  -c 'filetype plugin on' -c 'syntax on' \
  -c 'edit examples/01-basics.gti' -c 'quitall'
```

### Release Compiler Or Editor Tooling

- Bump the semantic version in `VERSION`; CI rejects shipped compiler, runtime,
  standard-library, LSP, formatter, Tree-sitter, or Neovim plugin changes that
  omit it. Do not duplicate a release version in source files.
- Confirm the CLI version, `gti_lsp --version`, and LSP initialization
  `serverInfo.version` all match `VERSION`.
- Run compiler, CLI, LSP protocol, and Neovim plugin tests before pushing the
  versioned change.
- Commit and push `main`. The `VERSION` path trigger runs the release workflow,
  which creates `v$(cat VERSION)` after every platform package succeeds; do not
  race it with a manual tag push.
- Confirm `.github/workflows/release.yml` publishes all four platform archives
  and checksum files before asking Lazy users to update.
- After publication, `:Lazy sync` updates a `version = "*"` checkout; restart
  `gti_lsp` or Neovim and confirm the selected binary with `:GTIInfo`.

## Test Selection

The CTest suite contains:

- `compiler_library_boundary`: a small direct client proving compiler headers
  resolve their non-inline implementation through `gti_compiler`.
- `compiler_pipeline`: in-process lexer, parser, AST, semantics, emitter, target,
  runtime-surface, and formatter tests from `tests/compiler_tests.cpp`.
- `cli_workflow`: command-line behavior and native compilation from
  `tests/cli_smoke_test.py`.
- `lsp_protocol`: initialize, diagnostics, semantic tokens, and formatting from
  `tests/lsp_smoke_test.py`; available only when `json-c` builds `gti_lsp`.

The Tree-sitter grammar has a separate npm corpus test under
`tree-sitter-gti/`; CI also regenerates the committed parser and rejects drift.

Locate the existing compiler feature group before adding coverage:

```sh
rg -n "^void test[A-Za-z0-9_]+\(\)" tests/compiler_tests.cpp
```

The in-process test executable does not currently expose individual test-case
selection. Keep focused cases with the matching `test...` group, build the one
`gti_tests` target during iteration, and run `compiler_pipeline` before handoff.

Use focused iteration first:

```sh
cmake --build build -j4
ctest --test-dir build --output-on-failure -R 'compiler_pipeline|lsp_protocol'
```

Then run the full suite:

```sh
cmake --build build -j4
ctest --test-dir build --output-on-failure
./build/gti examples/07-generics.gti -o /tmp/gti-generics
/tmp/gti-generics
git diff --check
```

For broader optional bug-finding before pushing substantial language, compiler,
optimizer, standard-library, or backend changes, run:

```sh
python3 scripts/local_language_audit.py --full
```

This local audit checks contract snapshots, `-O0`/`-O3` and C++20/C++23
equivalence, public examples, generated semantic programs, malformed-source
mutations, deterministic emission, and the paired C++ comparison showcase. It
is deliberately absent from CTest, CI, and release workflows; see
[`docs/local-language-audit.md`](../../../../docs/local-language-audit.md).

If the build directory does not exist, configure with `cmake -S . -B build`.
If `json-c` is missing, CMake omits `gti_lsp`; do not treat the absent LSP test as
a pass.

## Diagnostic Quality

- Report errors in the earliest phase that has enough information.
- Name the GTI construct and corrective action; avoid exposing C++ terminology
  unless the error is genuinely from native compilation.
- Assign a stable phase-specific code and attach the narrowest exact span.
- Use related locations for prior declarations and include sites; use hints for
  actionable guidance and fix-its only for unambiguous source replacements.
- Preserve dependency source paths and test both CLI source excerpts and LSP
  UTF-16 ranges when a diagnostic contract changes.
- Add parser synchronization coverage when malformed input should produce more
  than one independent error.
- Keep semantic analysis of recovered declarations available to the LSP, while
  preventing code generation whenever parsing failed.
- Do not suppress an error merely to let generated C++ diagnose it later.

## Completion Checklist

- The grammar and implementation agree.
- The authoritative semantic record exists and downstream phases consume it
  rather than re-inferring it.
- Inheritance, override roots, structured base construction, and dispatch mode
  are preserved explicitly whenever the change affects polymorphic behavior.
- HIR represents every affected concrete instance, value, statement, and
  selected edge.
- MIR represents every affected operation, place, CFG edge, move, loan, use, and
  cleanup effect, and its validator accepts the result.
- Backend code contains representation decisions only; invalid GTI never relies
  on native C++ rejection.
- Positive and negative behavior is covered.
- Inactive target branches remain syntactically checked.
- CLI and LSP diagnostics agree on phase behavior.
- C++20 and C++23 remain equivalent where required.
- Formatting is idempotent and editor files load.
- Generated C++ compiles for a representative source.
- No unrelated worktree changes are staged.
- The verified task is committed and pushed unless the user requested otherwise.
