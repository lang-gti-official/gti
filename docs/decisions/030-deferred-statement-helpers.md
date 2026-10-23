# 030: Confined Deferred Statement Helpers

Status: Accepted for the assertion foundation.

## Problem

An ordinary `void debug_assert(bool, ...)` still evaluates and transfers its
arguments before entering its body. Neither an empty body nor optional native
inlining establishes a language guarantee that disabled checks, diagnostic
allocations and explicit argument moves are skipped. Requiring call-site
closures would be safe but would change ordinary assertion syntax and expose
capture/loan setup even for disabled checks.

## Decision

`[[deferred]]` opts a namespace-scope void statement helper into typed
expression parameters. Every use substitutes and evaluates the corresponding
caller expression at that point. There is no automatic memoization, eager
parameter storage, escaping thunk, callback conversion or public-name magic.
Definitions own free-name lookup; invocations own argument lookup and access.
Normal semantic flow owns conditional moves, borrows, first failure and
cleanup. HIR lowers the checked expansion to existing statement operations;
verified MIR remains the only execution authority.

The first profile deliberately has no local binders, direct assignment,
lambda bodies, loops, early returns or value-producing helper results. An
ordinary helper called from a selected branch can perform those operations.
This is a structural source-template boundary, not a restriction on the
types accepted by `std::assert`. Supporting richer templates requires hygiene
and local exit/lifetime facts, rather than arbitrary AST substitution or
backend inference. Arguments themselves retain ordinary executable semantics.

`std::assert` and `std::debug_assert` are library clients. Both evaluate an
enabled condition once and force messages only on failure. The debug API uses
an ordinary constant guard so its disabled body is still checked. `NDEBUG`
selects that source constant, independently of optimizer success. Default
release project configuration defines it; direct compilation chooses explicitly.
The public `std::debug_assertions_enabled` constant lets application helpers
use exactly the same policy. Panics reuse the existing tracked caller token,
bounded runtime record, deterministic cleanup and containment contract.

## Alternatives

- Compiler-recognized assertion names: rejected; applications need the same
  capability and public standard-library names are not semantic authority.
- Making ordinary calls lazy: rejected; it would break GTI's deterministic
  eager evaluation and value-parameter cleanup contract.
- General escaping lazy values: not needed for this outcome; they require
  a distinct lifetime, forcing cardinality and storage contract.
- Native optimization as the meaning of a disabled check: rejected; native
  optimization may remove representation work, never define whether source
  effects execute.

## Verification

The delivery requires positive and negative source/IR tests, application-defined
helpers, owner moves and live loans, disabled static errors, source-location
forgeries, C++20/C++23 O0/O1/O3 runtime parity, disabled elision and sanitizers,
normal/double-failure cleanup, editor synchronization and a runnable example.
Successful-path measurements must separate condition work, cold formatting,
allocation counts and code size; a correctness pass alone is not performance
acceptance.
