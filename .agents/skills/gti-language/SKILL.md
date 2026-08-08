---
name: gti-language
description: Develop and review the GTI C++-like compiled language and its compiler, including syntax, lexer, parser and AST, semantic analysis, typed HIR, MIR, optimization, C++ lowering, source loading, target conditionals, standard library, runtime, diagnostics, tests, formatter, LSP, CLI, and editor tooling. Use for GTI language design, compiler implementation, feature work, architecture review, debugging, and repository onboarding.
---

# GTI Language

Develop GTI as a C++-familiar compiled language that preserves explicit
control, value semantics, RAII, performance, and native interoperability while
removing avoidable hazards and accidental complexity.

## Begin Every Task

1. Run `git status --short`. Preserve unrelated user changes and generated
   artifacts already present in the worktree.
2. Classify the task and load only the references it needs:
   - For compiler implementation, debugging, semantic metadata, HIR, MIR,
     optimization, or lowering, read
     [references/compiler-internals.md](references/compiler-internals.md).
   - For language design or behavior, read the relevant section of
     [references/language-contract.md](references/language-contract.md) and the
     relevant productions in `docs/grammar.ebnf`. Read the entire grammar only
     for broad design or onboarding.
   - For source loading, parser/AST, backend, standard-library/runtime, CLI,
     LSP, editor, project, or release boundaries, read the relevant section of
     [references/architecture.md](references/architecture.md).
   - Before changing user-visible syntax or behavior, read the matching impact
     section in [references/change-guide.md](references/change-guide.md).
   - For ownership, movement, references, allocation, or destruction, also read
     the relevant section of `docs/ownership.md`.
3. Trace the current behavior through the implementation anchors named by the
   selected reference. Do not infer GTI behavior from C++ resemblance or from
   emitted C++.
4. Establish a focused baseline test when practical. Keep the change coherent
   through implementation, diagnostics, tests, documentation, and affected
   tooling.

## Compiler Direction

Keep the implemented pipeline and its authority one-directional:

```text
source loading + lexing -> parsing/AST -> semantics -> typed HIR -> MIR
                                                      |          |
                                                      +----------+
                                                           |
                                                    FrontendResult
                                                           |
                           typed-HIR optimization decisions + target
                                                           |
                                                         backend
                                                           |
                                             artifact -> toolchain driver
```

- Enter reusable analysis through `Frontend`; keep CLI and LSP phase ordering
  identical.
- Treat the checked AST as syntax and source provenance, `SemanticModel` as the
  authoritative resolved facts, typed HIR as the concrete instance and dispatch
  graph, and MIR as body-local control flow, values, places, resolved calls,
  structured construction, moves, loans, and cleanup.
- Reject invalid GTI in the earliest frontend phase with enough information.
  Do not rely on generated C++ diagnostics for a language rule.
- Keep target-independent decisions out of `CppEmitter`. The current C++
  emitter still traverses AST and consumes semantic and HIR side data as a
  transitional implementation; do not mistake that for the intended boundary.
- Keep backend representation choices out of syntax and semantics. A backend
  limitation is not automatically a language rule.
- Preserve source-unit identity, exact source spans, resolved symbol and
  callable identities, dispatch mode, ownership traits, and source provenance
  through every layer that consumes them.

## Phase Ownership

- Put token identity and spelling recognition in `token.h` and `lexer.h`.
- Put grammar, precedence, construction, and recovery in `parser.h`; put syntax
  structure and visitor contracts in `ast.h`.
- Put name resolution, types, inheritance and dispatch, access, ownership
  validation, overload selection, control-flow rules, and intrinsic validation
  in `semantic_analyzer.h`.
- Put canonical source units and direct dependency edges in `source_graph.h`
  and loading policy in `source_loader.h`.
- Put stable concrete class/base/callable instances, dispatch identity,
  structured constructor initialization, and typed executable values in
  `hir.h`. Reuse semantic instance analysis for concrete generic bodies instead
  of creating a second type system.
- Put body-local CFGs, scalar operations, places, resolved call dispatch,
  structured base construction, use-def indexes, loans, moves, and lexical
  cleanup in `mir.h`.
- Put current typed-HIR optimization decisions in `optimizer.h`; implement new
  control-flow and value-dataflow passes over MIR when their required facts are
  present there.
- Put backend contracts in `backend.h` and C++ representation in
  `cpp_backend.h` and `cpp_emitter.h`. Do not add another backend until MIR owns
  the required ownership, polymorphic layout, ABI, lifetime, and runtime rules.
- Put portable public APIs in `stdlib/`, compiler-private capabilities under
  `gti_internal`, the narrow C ABI in `runtime/include/gti/runtime.h`, and host
  implementation in `runtime/src/`.
- Keep reusable semantic IDE queries in `language_queries.h`; the LSP converts
  protocol data and retains snapshots but does not independently infer compiler
  facts.

## Specialized Boundaries

- For manifests, project commands, profiles, native linking, caching,
  workspaces, dependencies, lockfiles, or package resolution, also use
  `$gti-build-architecture` and read `docs/build-system-proposal.md`.
- For document snapshots, semantic tokens, hover, completion, navigation,
  diagnostics scheduling, or protocol boundaries, also use
  `$gti-lsp-architecture`.
- Keep the direct compiler workflow manifest-independent. Project orchestration
  must construct requests for the same frontend/backend pipeline rather than
  teaching compiler phases about manifests, caches, or dependency acquisition.

## Change Workflow

1. State the GTI rule independently of its C++ representation.
2. Identify the authoritative phase and every downstream consumer using the
   change guide.
3. Add focused positive and negative coverage. Test invalid behavior at the
   phase that owns the rule.
4. Update the relevant grammar production and an example for user-visible
   syntax. Update formatter, Tree-sitter, LSP, and editor files only when the
   change crosses those surfaces.
5. Inspect emitted C++ when representation changes, but verify frontend facts,
   HIR, and MIR separately when they own the behavior.
6. Run focused checks first, then the broad suite in the change guide.

## Diagnostics And Verification

- Route compiler diagnostics through `diagnostic.h`. Preserve exact byte spans,
  stable phase-specific codes, related declarations, actionable hints, and only
  mechanically correct fix-its.
- Keep C++ formatting scoped to task-owned lines. Inspect with
  `git clang-format --diff HEAD` before applying `git clang-format HEAD`; do not
  reformat whole existing files during feature work.
- Use outputs beneath `/tmp` when inspecting generated C++ or executables.
- Advance `VERSION` for shipped compiler, standard-library, runtime, LSP,
  formatter, Tree-sitter, or Neovim behavior. Follow the release section of the
  architecture reference; do not create release tags manually.
- Stage only task-owned files. This repository expects completed changes to be
  committed and pushed unless the user says otherwise.
