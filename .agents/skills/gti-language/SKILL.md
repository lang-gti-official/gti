---
name: gti-language
description: Guide development and review of the GTI language, compiler, standard library, CLI, and LSP. Use when changing GTI syntax, semantics, AST nodes, diagnostics, lowering, native bindings, standard-library boundaries, examples, grammar, or editor tooling.
---

# GTI Language

Develop GTI as a C++-like compiled language that preserves C++'s performance,
explicit control, value semantics, RAII, and native ecosystem interoperability
while removing avoidable hazards and accidental complexity.

## Core Principles

- Prefer familiar C++ spelling and behavior unless GTI intentionally makes a
  rule safer, simpler, or easier to diagnose.
- Keep bindings and parameters immutable by default. Require `mut` where state
  must change.
- Require non-`void` function results to be used by default. Permit intentional
  call-site suppression only through the explicit `[[discard]]` attribute.
- Model recoverable failure with the built-in `expected<T, E>` type and
  explicit `unexpected(error)` values. Do not add exceptions or implicit error
  propagation syntax.
- Keep the parser limited to language syntax. Facilities such as output belong
  in ordinary standard-library functions.
- Reject invalid programs during GTI semantic analysis instead of relying on
  generated C++ diagnostics.
- Make ownership, lifetime, and nullability explicit as those systems are
  introduced. Do not inherit unsafe C++ defaults accidentally.
- Avoid textual macros, order-dependent behavior, hidden conversions, and
  undefined behavior as language features.
- Keep compiler phases separate: source loading, lexing, parsing and AST,
  semantics, lowering, then the native compiler.
- Preserve source provenance and actionable diagnostics through every phase.
- Keep the C++ backend replaceable. Do not expose a backend-only restriction as
  a GTI rule without a language-level reason.
- Write portable standard-library behavior in GTI. Cross into the native
  runtime only through validated runtime bindings backed by a narrow C ABI.
- Bind runtime services by semantic identity, never by matching user-facing
  function names such as `print` in a backend.
- Add features coherently across grammar, implementation, diagnostics, tests,
  examples, and editor tooling.

## Dependency Policy

- Treat `include "path.gti"` as source dependency loading, not text
  substitution.
- Resolve paths relative to the including file, canonicalize them, load each
  file once, reject cycles, and allow directives only at top level.
- Do not add macros or conditional text preprocessing to `include`.
- Leave room for a future module or import system to provide namespaces,
  explicit APIs, separate compilation, and package boundaries. `include` is an
  intentionally small bootstrap mechanism.

## Change Workflow

1. Read `docs/grammar.ebnf` and the affected compiler phases.
2. State the semantic rule and how it improves or deliberately preserves C++.
3. Update only the required frontend, semantic, lowering, CLI, and LSP layers.
4. Add positive and negative tests plus an example for user-visible syntax.
5. Build, run `ctest`, and compile the example through the CLI.
6. Use `rg` to ensure removed tutorial constructs and stale paths are gone.

## Repository Boundaries

- Put reusable compiler code in `include/gti`.
- Keep executable drivers in `src/cli` and `src/lsp`.
- Put ordinary library APIs and runtime bindings in `stdlib`.
- Put host-platform implementations and their C ABI in `runtime`.
- Keep language examples in `examples` and the implemented grammar in `docs`.
- Treat generated C++ as an intermediate representation, not the GTI language
  specification.
- Target C++23 by default and lower expected values to `std::expected`. Keep
  the vendored C++20 compatibility backend semantically equivalent.
