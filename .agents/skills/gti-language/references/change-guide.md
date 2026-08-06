# GTI Change And Verification Guide

Use this reference to scope a change and prove it at the correct boundaries.

## Impact Matrix

### Add A Keyword, Operator, Or Literal

Inspect and usually update:

1. `include/gti/token.h` for `TokenKind`, fixed keyword spelling, and
   `to_string()`.
2. `include/gti/lexer.h` for recognition, literal decoding, and lexical errors.
3. `include/gti/parser.h` for grammar and precedence.
4. `include/gti/ast.h` only if the syntax needs new structure.
5. `semantic_analyzer.h` and `cpp_emitter.h` for meaning and lowering.
6. `formatter.h`, `src/lsp/main.cpp`, and `editor/nvim/syntax/gti.vim` for
   formatting and highlighting.
7. `docs/grammar.ebnf`, focused tests, and an example.

Before implementation, decide operand domains, result type, precedence,
associativity, conversion behavior, and invalid runtime cases. Do not inherit a
C++ undefined edge case such as zero division without defining GTI behavior.
Reuse an existing AST node such as `Binary` when the syntax has the same shape.
Check `CppEmitter::operatorSpelling()` when lowering an operator.

Do not reserve a word if an ordinary library function or existing syntax can
express the feature.

### Add An AST Node

- Add the node and accessors in `ast.h`.
- Extend `ExprVisitor` or `StmtVisitor`.
- Implement every visitor, including semantics, C++ emission, and AST printing
  where applicable.
- Add parser construction plus positive, recovery, semantic, and lowering tests
  appropriate to the node.
- Search visitor coverage with:

```sh
rg -n "visit[A-Za-z]+(Expr|Stmt|Decl)" include/gti
```

### Change A Semantic Rule

- Keep parsing unchanged unless the source grammar changes.
- Put the rejection and GTI-focused message in `semantic_analyzer.h`.
- Add one valid case and focused invalid cases to `tests/compiler_tests.cpp`.
- Assert diagnostic count and meaningful location/message when stable.
- Ensure invalid source is rejected before invoking the native compiler.

### Change C++ Lowering

- Update `cpp_emitter.h` without weakening GTI semantic validation.
- Cover both C++23 and C++20 when `expected` behavior is involved.
- Assert important emitted fragments, then compile a representative `.gti`
  program through the CLI.
- Use `--emit-cpp -o /tmp/output.cpp` when inspecting generated code.

### Change Includes Or Source Loading

- Work primarily in `source_loader.h` and preserve token provenance.
- Test relative resolution, nested includes, duplicate loads, cycles, invalid
  extensions, placement restrictions, and dependency diagnostics as relevant.
- Exercise unsaved root text through the LSP because it supplies entry source in
  memory while dependencies still come from disk.

### Change Target Conditionals

- Keep condition syntax in parser/AST and target values in `target.h`.
- Parse every branch; analyze and emit only the selected branch.
- Test explicit `TargetInfo` values rather than depending only on the host.
- Keep includes forbidden inside conditionals unless the language design is
  deliberately changed.

### Change The Standard Library Or Runtime

- Put user-facing and portable behavior in `stdlib/prelude.gti` or future GTI
  library files.
- Add a runtime binding only for a host service that GTI cannot implement
  portably.
- Update the C ABI header, C++ adapter, implementation, semantic allowlist,
  emitter include detection, CMake installation, and CLI link path together.
- Test that similarly named user functions do not gain runtime privileges.

### Change CLI Behavior

- Update `src/cli/main.cpp`, usage text, and `tests/cli_smoke_test.py`.
- Cover exit status, stderr/stdout ownership, output paths, forwarded arguments,
  and resource discovery where applicable.
- Update README command examples for user-visible options.

### Change LSP, Formatting, Or Highlighting

- Keep diagnostics on the same source-loader/parser/semantic pipeline as the
  CLI.
- Update LSP capability advertisement and `tests/lsp_smoke_test.py` together.
- Keep semantic token enum order identical to the advertised legend.
- Add formatter idempotence coverage and preserve comments and strings.
- Update `plugin/gti.lua` semantic links plus `syntax/gti.vim` fallback syntax
  for new token roles. Check `lsp/gti_lsp.lua` when startup behavior changes.
- Run `tests/nvim_plugin_smoke_test.lua` for changes under `plugin/`, `lsp/`,
  `lua/gti/`, `ftdetect/`, `ftplugin/`, `syntax/`, `lazy.lua`, or `build.lua`.
- Headlessly load editor files when they change:

```sh
XDG_STATE_HOME=/tmp/gti-nvim-state nvim --headless -u NONE -n -i NONE \
  --cmd 'set runtimepath^=.' \
  -c 'filetype plugin on' -c 'syntax on' \
  -c 'edit examples/lang_test.gti' -c 'quitall'
```

### Release Compiler Or Editor Tooling

- Bump the semantic version in `VERSION`; do not duplicate a release version in
  source files.
- Confirm the CLI version and LSP initialization `serverInfo.version` both
  match `VERSION`.
- Run compiler, CLI, LSP protocol, and Neovim plugin tests before tagging.
- Commit and push `main`, then create an annotated tag named exactly
  `v$(cat VERSION)` and push that tag.
- Confirm `.github/workflows/release.yml` publishes all four platform archives
  and checksum files before asking Lazy users to update.
- After publication, `:Lazy sync` updates a `version = "*"` checkout; restart
  `gti_lsp` or Neovim and confirm the selected binary with `:GTIInfo`.

## Test Selection

The CTest suite contains:

- `compiler_pipeline`: in-process lexer, parser, AST, semantics, emitter, target,
  runtime-surface, and formatter tests from `tests/compiler_tests.cpp`.
- `cli_workflow`: command-line behavior and native compilation from
  `tests/cli_smoke_test.py`.
- `lsp_protocol`: initialize, diagnostics, semantic tokens, and formatting from
  `tests/lsp_smoke_test.py`; available only when `json-c` builds `gti_lsp`.

Use focused iteration first:

```sh
cmake --build build -j4
ctest --test-dir build --output-on-failure -R 'compiler_pipeline|lsp_protocol'
```

Then run the full suite:

```sh
cmake --build build -j4
ctest --test-dir build --output-on-failure
./build/gti examples/lang_test.gti -o /tmp/gti-lang-test
/tmp/gti-lang-test
git diff --check
```

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
- Positive and negative behavior is covered.
- Inactive target branches remain syntactically checked.
- CLI and LSP diagnostics agree on phase behavior.
- C++20 and C++23 remain equivalent where required.
- Formatting is idempotent and editor files load.
- Generated C++ compiles for a representative source.
- No unrelated worktree changes are staged.
- The verified task is committed and pushed unless the user requested otherwise.
