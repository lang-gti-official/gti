# GTI Language Specification

Status: Working Draft describing the current language, with explicit gaps.

This directory is GTI's backend-independent language record. It states what GTI
constructs mean; compiler implementation details belong in
[`docs/architecture/`](../architecture/overview.md), and unimplemented designs
belong in [`docs/plans/`](../plans/roadmap-to-1.0.md).

The source code and tests are the authority for the current compiler when this
draft disagrees with implementation. Such disagreements are documentation or
implementation bugs to resolve explicitly, not permission to infer semantics
from generated C++.

## Reading Order

1. [Scope and conformance](scope-and-conformance.md)
2. [Lexical and syntactic structure](lexical-and-syntax.md)
3. [Implemented grammar](grammar.ebnf)
4. [Static semantics](static-semantics.md)
5. [Execution semantics](execution.md)
6. [Ownership and lifetimes](ownership-and-lifetimes.md)
7. [Programs and targets](programs-and-targets.md)
8. [Standard library](standard-library.md)

Focused current contracts add detail for
[concepts](concepts.md), [expected values](expected.md),
[ranges](ranges.md), [raw pointers](raw-pointers.md),
[native C interoperation](native-c-interop.md), [I/O](io.md), and
[TCP ownership](tcp.md).

## Authority

- `grammar.ebnf` records accepted syntax; parser and lexer code decide current
  implementation when the grammar has drifted.
- Language documents record backend-independent meaning and known gaps.
- Architecture documents explain how the reference compiler realizes those
  rules.
- Plans are non-canonical until implementation, tests, and these language docs
  are updated.
- C++ behavior, generated helper spelling, and declaration-only standard
  library scaffolds are never language definitions.

The normative words **must**, **must not**, **required**, **shall**, and
**shall not** state requirements. **May** grants permission. **Should** states a
recommendation. Examples and rationale are non-normative unless stated
otherwise.

## Design Principles

GTI keeps C++-familiar spelling where it can provide a smaller complete rule.
Ownership, lifetime, nullability, conversions, failure, and evaluation are GTI
semantics rather than backend consequences. Invalid GTI should be rejected by
the frontend. Public library policy should be ordinary GTI source; only
irreducible capabilities belong behind a compiler/runtime boundary.

## Maintenance

A user-visible language change should update the grammar when syntax changes,
the owning language document, positive and negative tests, affected
architecture docs, and tooling surfaces. Before 1.0, a documented minor
release may make draft-breaking changes; a patch release does not intentionally
do so. The 1.0 scope is a soft systems-readiness goal until publication; GTI
1.0 then freezes Edition 1 under
[Scope Section 1.6](scope-and-conformance.md#16-compatibility) and
[ADR 011](../decisions/011-language-compatibility-and-editions.md). Current
restrictions, their reasons, readiness roles, user-facing clients, and owning
plan rows are tracked in the maintained
[language restriction ledger](../plans/language-alignment.md), not inside this
specification index.
