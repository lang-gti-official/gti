# 030: Confined Typed Statement Macros

Status: Accepted for the assertion foundation; public syntax and local binding
profile revised 2026-10-23.

## Problem

An ordinary `void debug_assert(bool, ...)` still evaluates and transfers its
arguments before entering its body. Neither an empty body nor optional native
inlining establishes a language guarantee that disabled checks, diagnostic
allocations and explicit argument moves are skipped. Requiring call-site
closures would be safe but would change ordinary assertion syntax and expose
capture/loan setup even for disabled checks.

## Decision

`#macro name(typed parameters) { ... }` declares a namespace-scope statement
macro with an implicit void result and typed expression parameters. Calls keep
ordinary `name(...)` or `namespace::name(...)` spelling. Every use substitutes
and evaluates the corresponding
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

## Public Syntax Revision: 2026-10-23

The original assertion foundation shipped the `[[deferred]] void name(...)`
spelling. The owner selected `#macro` as the clearer C++-familiar declaration
form, retaining ordinary calls and typed parameters rather than adopting
token-pattern matching or an invocation sigil. The attribute is removed, not
retained as an alias. Its migration diagnostic explains the replacement.
The same typed statement-expansion engine remains authoritative; this revision
does not add a second macro evaluator or change argument forcing semantics.

Namespaces, generic final packs, defaults, constraints and caller tracking
remain available. Macro/function overload mixtures at one qualified name are
rejected so overload selection cannot silently switch eager versus expression
argument evaluation. This deliberately differs from C replacement macros:
`#define` stays valueless configuration, macros cannot rewrite arbitrary tokens,
and ordinary visibility, hygiene, ownership and source diagnostics apply.

The syntax revision alone did not grant local binders or loops. The following
revision establishes their independent hygiene and lifetime contract.

## Hygienic Local Bindings and Loops: 2026-10-23

Automatic value/reference locals and structured bindings are ordinary GTI
bindings, identified separately for each expansion. An initializer explicitly
forces its expression once when reached; subsequent reads use that binding.
There is still no implicit memoization, clone or lifetime extension. Local
mutation and ordinary scoped loops reuse checked places and MIR control flow.
Break and continue must target a loop belonging to the macro, never the caller.
Returns, introduced propagation, static locals, lambda definitions, generated
declarations and value-producing expansions remain separate goals.

Hygiene is structural, not name mangling: definition tokens see only their
own lexical expansion frames before definition-scope lookup; substituted
argument tokens retain their invocation's lexical scope boundary and access
context. A syntax-only shadowing walk prevents substitution across a local
binder. Ordinary semantic symbols, ownership and loans remain authoritative;
HIR/MIR preserve their identities and deterministic cleanup. The backend does
not rediscover bindings from spellings or introduce a thunk.

Diagnostics retain argument source sites and bounded nested invocation and
declaration traces. The read-only expansion query explains the checked syntax
using display-only local labels; it is not executable generated source or a
second semantic evaluator. Rename follows exact written binder identity, not
same-spelled locals in another scope.

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
