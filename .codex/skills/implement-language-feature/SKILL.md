---
name: implement-language-feature
description: Implement or modify GTI source syntax or language semantics coherently across the compiler, standard library, runtime, diagnostics, tests, examples, specification, formatter, Tree-sitter parser, LSP, and editor tooling. Use for new keywords, declarations, expressions, types, operators, control flow, generics, ownership rules, conversions, lifecycle, target behavior, or changes to existing GTI meaning.
---

# Implement A GTI Language Feature

Treat the compiler frontend as the language authority and touch only the stages
the feature actually affects.

## Establish The Contract

1. Run `git status --short`; preserve unrelated work.
2. Read [`docs/index.md`](../../../docs/index.md), the relevant document under
   [`docs/language/`](../../../docs/language/index.md), and the owning
   architecture document.
3. Trace current behavior in source and tests. Do not infer GTI behavior from
   C++ resemblance or emitted C++.
4. State the syntax, static semantics, runtime behavior, ownership effects,
   failure mode, and deliberate omissions before coding.
5. Classify the slice as non-executable syntax/static-semantics/tooling,
   source-defined library code using existing executable semantics, or
   new/changed executable language behavior.
6. Identify whether the request implements a current plan or changes an
   accepted decision. Update status/rationale explicitly when required.
7. Name the user-facing program, standard-library operation, or workflow this
   slice enables. Prefer a bounded end-to-end capability over generalized
   restriction machinery that has no immediate client.

## Trace Applicable Layers

Use this as an impact checklist, not a requirement to modify every stage:

```text
source spelling
  -> token/lexer
  -> parser + AST + recovery
  -> semantic analysis + SemanticModel/SemanticDatabase
  -> concrete HIR
  -> body-local MIR + verification/effects
  -> optimizer/backend representation
  -> runtime or source-defined stdlib boundary
  -> diagnostics and fix-its
  -> formatter, Tree-sitter, LSP, Neovim syntax
  -> tests, examples, language and architecture docs
```

Search the closest existing construct and follow its visitors/records. Add an
AST node to every relevant visitor and lowering path. Put lookup, type,
conversion, overload, ownership, access, lifecycle, and dispatch decisions in
semantics. Reuse concrete semantic reanalysis for generic instances; do not add
a second type system in HIR/MIR/backend/tooling.

Preserve source-unit identity, precise byte spans, resolved declaration/call
identities, target facts, and ownership/drop information downstream. Reject
invalid GTI before backend entry.

## Enforce The Executable Authority Gate

Do not implement new or changed executable language behavior by adding or
extending a C++ emission path that reads only AST, semantic, or HIR facts.
Treat verified MIR that the production backend ignores as incomplete, not as
backend-independent execution.

For an executable slice, either:

1. lower the complete selected operation/body family through semantic analysis,
   concrete HIR, body-local MIR, effects and verification, then emit that family
   from verified `BackendInput::mir`; or
2. stop and record the missing representation or migration prerequisite under
   `M-EXEC-01`, `M-FAIL-01`, `M-BACK-01`, or `M-BACK-02` in
   [`docs/plans/implementation-sequence.md`](../../../docs/plans/implementation-sequence.md).

Keep one executable authority inside a body. Do not mix a MIR schedule with
AST/HIR recursive emission, reconstruct missing semantics in `CppBackend` or
`CppEmitter`, or call a feature complete because MIR snapshots exist while the
runtime path bypasses them. Add verifier mutations and O0/O1/O3 runtime evidence
for the selected MIR-emitted family; include C++20/C++23 parity when the backend
representation can differ.

While the immediate backend-authority recovery campaign in the implementation
sequence is active, pause unrelated executable language expansion. Treat the
declared recovery phase—not an individual IR row or prompt-sized checkpoint—as
the outcome boundary. Co-deliver the applicable M-EXEC, M-FAIL/Q-FAIL, and
M-BACK work for the largest coherent body-family closure, using reviewable
commits without stopping before its production consumer.

Changes to the compatibility emitter are allowed only to repair, preserve, or
remove already implemented behavior while migration remains incomplete. Label
that scope explicitly. Such a change must not add source behavior, relax a
semantic restriction, create a second execution rule, or count as completion
of a new executable feature. Pure parser recovery, diagnostics, formatting,
Tree-sitter, LSP, metadata, and source-defined library changes that use existing
language semantics do not require a new MIR operation merely because they are
part of a language-feature task. A static change that only rejects programs is
also outside this gate; a relaxation that makes new runtime behavior reachable
is not.

## Library And Runtime Choice

Prefer ordinary GTI in `stdlib/` for public policy. Add a compiler-private
capability only for an invariant ordinary GTI cannot express safely, and bind
it by trusted declaration identity. Use the bounded native/runtime boundary
only for host operations. Never recognize a public standard-library wrapper by
name in the compiler.

## Test And Finish

1. Add focused valid and invalid tests in the owning suite, including recovery
   and concrete generic/ownership composition where applicable.
2. Inspect HIR/MIR and emitted C++ when their representation changes; test
   runtime edges independently.
3. Synchronize formatter, Tree-sitter, semantic tokens, completion, hover, and
   examples only when the change crosses those surfaces. Use
   `$lsp-synchronization` for semantic editor changes.
4. Update `docs/language/` for meaning, `docs/architecture/` for implemented
   boundaries, and `docs/plans/` only for remaining work. Prefer updating an
   existing canonical document.
5. Run focused CTest targets first, then the broad affected workflows described
   in [`docs/architecture/verification.md`](../../../docs/architecture/verification.md).
6. Run `git diff --check`; inspect the full diff for accidental semantic or
   documentation drift. For an executable change, confirm that the diff adds no
   new AST/HIR-only `CppEmitter` behavior and cite the MIR-emitted family that
   owns production execution. Advance `VERSION` when the repository's shipped
   behavior/version policy requires it.
7. Use `$finish-release` to commit completed work and initiate the appropriate
   version/tag/release path. Once GitHub accepts the push or workflow dispatch,
   finish without polling or waiting for asynchronous CI/CD.

Do not use `1.0` as a reason to postpone a capability needed by the systems-
readiness workloads in ADR 012. The target is soft and revisable; accepted
safety, ownership, semantic-authority, and backend-independence rules still
apply to every slice.
