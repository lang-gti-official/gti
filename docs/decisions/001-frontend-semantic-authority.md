# 001: The Frontend Owns Language Semantics

Status: Accepted

## Context

GTI currently lowers to C++, but generated C++ cannot safely define GTI name
lookup, conversions, overloads, ownership, dispatch, checked arithmetic, or
failure. CLI, LSP, HIR, MIR, and future backends also need the same answers.

## Decision

The lexer/parser/AST preserve syntax; semantic analysis selects language
meaning and records it in `SemanticModel`. Downstream IRs and tools consume
those records. Invalid GTI is diagnosed before backend generation. The C++
emitter may choose representation but may not repeat semantic selection.

## Alternatives

- Rely on C++ compilation and diagnostics: rejected because it leaks host rules
  and prevents a backend-independent language.
- Let each consumer infer the facts it needs: rejected because compiler, LSP,
  optimizer, and backend behavior would diverge.

## Consequences

`FrontendResult` must own syntax and semantic side tables together. New features
must preserve resolved identities through HIR/MIR. The transitional emitter can
continue walking AST, but it must consume frontend decisions until MIR becomes
complete enough to replace that path.
