---
name: gti-language
description: Develop and review the GTI C++-like compiled language, including syntax, lexer, parser and AST, semantic rules, optimization and backend architecture, C++ lowering, source loading, target conditionals, standard library, native runtime, CLI, formatter, LSP, diagnostics, tests, grammar, examples, and editor tooling. Use for any GTI language design or implementation task and when onboarding to this repository.
---

# GTI Language

Develop GTI as a C++-familiar compiled language that preserves explicit
control, value semantics, RAII, performance, and native interoperability while
removing avoidable hazards and accidental complexity.

## Start Every Task

1. Run `git status --short`. Preserve unrelated user changes and generated
   artifacts already present in the worktree.
2. Read `docs/grammar.ebnf`, then read
   [references/architecture.md](references/architecture.md) before changing an
   unfamiliar compiler phase.
3. Trace the current behavior through the real implementation. Do not assume a
   C++ feature exists merely because GTI resembles C++.
4. Read [references/change-guide.md](references/change-guide.md) and identify
   every affected layer before editing user-visible syntax or behavior.
5. Establish a focused baseline test when practical, then keep the change
   coherent through implementation, diagnostics, tests, docs, and tooling.

## Language Invariants

- Prefer familiar C++ spelling and precedence unless GTI deliberately adopts a
  safer or simpler rule.
- Keep bindings and parameters immutable by default. Require `mut` for state
  that can change.
- Keep constructor calls explicit and resolve constructor overloads by one exact
  parameter-type match. Generate default/copy/move/assignment/destruction from
  frontend lifecycle metadata instead of inheriting C++ special-member
  suppression. Keep fields immutable by default in semantics. Keep methods
  read-only by default and use a trailing `mut` only for methods that require a
  mutable receiver.
- Keep named generics type-based and predictable. Infer function type arguments
  exactly from value arguments; do not introduce conversion-driven deduction,
  specialization, or unconstrained compile-time metaprogramming by accident.
- Resolve overloads by one unique exact parameter-type match after generic
  substitution. Do not add implicit call conversions, conversion ranking, or
  return-type overloading. Record the selected function identity in semantics.
- Keep numeric conversions explicit with `Type(value)`. Preserve checked
  narrowing behavior in every backend instead of emitting unchecked casts.
- Treat fixed arrays as inline bounded values. Keep C++ declarator spelling,
  compile-time length identity, complete initialization, checked indexing, and
  no pointer decay or public raw-data escape. Preserve bounds checks unless an
  optimization proves them unnecessary.
- Require every non-`void` call result to be used. Permit intentional call-site
  suppression only through `[[discard]]`.
- Model recoverable failure with built-in `expected<T, E>` and explicit
  `unexpected(error)`. Do not add exceptions or implicit propagation syntax.
- Reject invalid GTI in semantic analysis instead of depending on generated C++
  errors.
- Avoid textual macros, order-dependent semantics, hidden conversions, and
  accidental undefined behavior.
- Define integer edge cases at the GTI level. Keep modulo-by-zero and invalid
  shifts checked, and do not lower them to raw undefined C++ operations.
- Keep ownership, lifetime, nullability, and conversions explicit as those
  systems are introduced. Do not inherit unsafe C++ defaults by omission.
- Permit read-only method reference returns only when the borrow is proven to
  originate from `self`. Record the receiver or intrinsic argument that owns a
  borrowed call result, classify it as a place, and reject retained borrows
  from temporary storage. Conservatively reject invalidating operations on a
  borrowed move-only root until lexical loan analysis can prove the borrow has
  ended. Do not generalize this into free-function or mutable reference returns
  without an explicit lifetime model.
- Derive class and struct ownership traits recursively from substituted field
  types. Reject aggregate copies and use after move in semantics, and use
  recorded binding traits rather than nominal spelling in backends.
- Keep raw pointers, pointer arithmetic, `new`, and `delete` out of ordinary
  safe GTI. Implement familiar `std` ownership and container types as nominal
  GTI classes over restricted `gti_internal` capabilities, and use non-null
  references for borrows. A future explicitly opt-in `dangerous` surface may
  expose selected capabilities, but do not commit its spelling or leak backend
  representation before its contracts are designed. Follow
  `docs/ownership.md`.
- Bind internal capabilities by trusted semantic identity, never by the public
  `std` wrapper that uses them. Adding an intrinsic does not make it a stable
  application API.
- Treat C++ smart pointers as a C++ backend representation, never as the GTI ABI
  or a C runtime binding. Preserve ownership, transfer, and drop semantics in
  frontend metadata and later HIR/MIR operations.
- Treat `include "path.gti"` as dependency loading, never textual substitution.
  Keep it top-level, relative, canonicalized, load-once, and cycle-checked.
- Keep `#if` restricted to target selection. Do not grow it into a general macro
  processor.
- Keep output and other services out of the parser. Expose ordinary GTI APIs in
  `stdlib/` and cross to the host only through validated runtime bindings.
- Bind runtime services by semantic identity, never by matching public names
  such as `print` in the backend.
- Route compiler errors through `diagnostic.h`. Preserve exact source spans,
  stable phase-specific codes, related declarations, hints, and mechanical
  fix-its when the compiler can state a correction without guessing.
- Keep the C++ backend replaceable. A backend limitation is not automatically a
  language rule.
- Resolve installed resources from the OS-reported executable path, never from
  a bare `argv[0]`. Release binaries must not embed CI checkout paths.

## Phase Ownership

Keep the pipeline ordered and one-directional:

```text
source loading -> lexing -> parsing/AST -> target selection + semantics
               -> optimization -> backend -> artifact -> toolchain driver
```

- Put tokens and spelling recognition in `token.h` and `lexer.h`.
- Put syntax and recovery in `parser.h`; do not resolve names or types there.
- Put syntax structure and visitor contracts in `ast.h`.
- Put name resolution, type rules, mutability, nodiscard, and runtime-binding
  validation in `semantic_analyzer.h`.
- Enter reusable analysis through `frontend.h`; keep CLI and LSP phase ordering
  identical.
- Put target-independent optimization decisions in `optimizer.h` and require
  checked semantic information for every transformation.
- Keep backend contracts in `backend.h`; put C++ representation choices in
  `cpp_backend.h` and `cpp_emitter.h`.
- Treat the checked AST as the current high-level representation. Do not add an
  LLVM-shaped IR until ownership, layout, ABI, and generic instantiation rules
  can be represented explicitly.
- Keep reusable compiler facilities under `include/gti/`; keep `src/cli/` and
  `src/lsp/` as drivers.
- Keep the Tree-sitter grammar and queries (`tree-sitter-gti/`, `queries/gti/`)
  and root-level Neovim plugin files (`plugin/`, `lsp/`, `lua/gti/`,
  `ftdetect/`, `ftplugin/`, and `syntax/`) synchronized with LSP behavior.
- Keep the LSP request loop responsive. Run frontend diagnostics on coalesced
  document snapshots, reject stale generations, and avoid repeating analysis
  for `didSave` after full-sync `didChange`.
- Keep semantic-token position conversion linear in the document and cache
  results by document generation. Update the legend, Neovim links, fallback
  syntax, and protocol coverage together when classifications change.
- Keep Tree-sitter responsible for syntax structure and LSP semantic tokens
  responsible for resolved roles. Regenerate `tree-sitter-gti/src/parser.c`
  after grammar changes and reject `ERROR` or `MISSING` nodes in valid examples.
- Keep portable APIs in GTI under `stdlib/`; keep the narrow C ABI and host code
  under `runtime/`.
- Update `formatter.h`, LSP semantic tokens, and the Neovim runtime files when
  new syntax needs formatting or highlighting.

See [references/architecture.md](references/architecture.md) for concrete APIs,
data flow, and cross-phase traps.

## Change Workflow

1. State the language rule independently of its C++ lowering.
2. Choose the owning phase and make the smallest coherent cross-layer change.
3. Add focused positive and negative coverage. Test diagnostics at the GTI
   phase that owns the rule.
4. Update `docs/grammar.ebnf` and an example for user-visible syntax.
5. Update CLI, LSP, formatter, standard library, or runtime only when the change
   crosses those boundaries.
6. Inspect emitted C++ when backend behavior changes, but test source-level
   rejection before emission for invalid programs.
7. Follow the exact impact matrix and checks in
   [references/change-guide.md](references/change-guide.md).

## Released Tooling

Lazy users who configure `{ "LukeHHA/gti", version = "*" }` run the toolchain
downloaded for the latest release tag, not binaries built from `main`. Diagnose
editor reports by checking `:GTIInfo` and the installed
`toolchain/share/gti/VERSION` before changing the source LSP.

When a compiler or LSP change must reach release-installed users, update
`VERSION`, verify that both `gti --version` and LSP `serverInfo.version` use it,
commit and push the change, then create and push the matching annotated `vX.Y.Z`
tag. Pushing `main` without a new tag does not update `version = "*"` clients.
The tag starts `.github/workflows/release.yml`; do not describe the release as
available until its archives have been published successfully.

## Verification

Keep C++ formatting scoped to the lines owned by the current task. Use
`git clang-format --diff HEAD` to inspect the proposed formatting and
`git clang-format HEAD` to apply it. Do not run `clang-format -i` over complete
existing files during feature work. Inspect `git diff --stat` immediately after
formatting and revert only formatter changes introduced by the current task if
the diff expands beyond the intended lines.

Run the broad suite before completing a compiler change:

```sh
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure
./build/gti examples/07-generics.gti -o /tmp/gti-generics
/tmp/gti-generics
git diff --check
```

Use an output under `/tmp` to keep generated executables out of the worktree.
If `json-c` is unavailable, report that the LSP target and protocol test were
skipped rather than implying full coverage.

After verification, stage only task-owned changes. This repository expects
completed changes to be committed and pushed unless the user says otherwise.
Never include unrelated README edits, binaries, logs, or user work in that
commit.
