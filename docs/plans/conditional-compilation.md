# Conditional Compilation And Configuration Flags

> **Plan status:** proposal. Nothing in the *Proposed surface* section below is
> implemented. The *Verified baseline* section describes behaviour that exists
> today and was read from source, not inferred.

This plan extends GTI's existing compile-time conditional to carry named
configuration flags — `#define DEBUG`, `#ifdef DEBUG` — so that a unit can vary
by build configuration as well as by target triple. It also specifies the LSP
work that makes inactive branches visibly inactive in the editor.

Function-like macros, token pasting, stringification, and textual substitution
are **out of scope and rejected by design**. See *Decisions*.

## Why this matters

Platform independence is the stated motivation, and half of it already works:
`target.os` and `target.arch` cover *where the code is being built for*. What
is missing is *how it is being built* — debug instrumentation, optional
subsystems, feature flags a downstream project selects. Today the only way to
express those is to ship different source, which is why
`stdlib/std/tcp.gti` opens by `#error`-ing on Windows rather than selecting an
implementation.

A systems language needs both axes. The target axis is a property of the
machine; the flag axis is a property of the build.

## Verified baseline

Read from source at the time of writing. This is what a new implementation
must extend rather than replace.

| Directive | Token | Notes |
| --- | --- | --- |
| `#if` | `HASH_IF` | condition restricted to `target.<property>` |
| `#elif` | `HASH_ELIF` | |
| `#else` | `HASH_ELSE` | |
| `#endif` | `HASH_ENDIF` | |
| `#error` | `HASH_ERROR` | active branch reports `GTI-S2047` |
| `#include` | `HASH_INCLUDE` | may not appear inside a conditional |

Ownership by layer, which the phase-authority rule requires this plan to
preserve:

- **`src/compiler/lexer.cpp`** (`Lexer::directive`, line 282) recognises the
  six spellings and rejects anything else as `GTI-L0003`. It performs no
  evaluation.
- **`src/compiler/source_loader.cpp`** splices `#include` and tracks
  `conditionalDepth` for balance checking (`GTI-I0002`, and the unterminated
  case at line 365). It does not evaluate conditions.
- **`src/compiler/parser.cpp`** (`conditionalCompilation`, line 1195;
  `compileCondition`, line 1237) parses **every** branch and builds a
  `ConditionalStmt` holding a `ConditionalBranch` list.
- **`src/compiler/semantic_analyzer.cpp`** (`visitConditionalStmt`, line 632)
  selects one branch via `stmt.activeBranch(target)` and analyses only that.
- **`src/compiler/language_queries.cpp`** (lines 1343, 1510) walks only the
  active branch for outline and tooling queries.

Three properties follow from that structure, all recorded in
[`docs/language/grammar.ebnf`](../language/grammar.ebnf) lines 85–90:

1. **Every branch must be syntactically valid GTI.** Inactive branches are
   parsed, not skipped as text.
2. **Only the selected branch is analysed and emitted.**
3. **Directives do not lower to C++ preprocessor directives.** The backend
   never sees them.

The condition grammar today is exactly:

```
target . ( os | vendor | arch )  ( == | != )  STRING
```

No boolean operators, no grouping, no negation beyond `!=`.

## Decisions

### D1. Flags carry no value. There is no substitution.

`#define NAME` defines a flag. `#define NAME 1024` is **rejected**.

GTI already has `constexpr` for named constants, and it is the better
mechanism: typed, scoped, visible to semantics, and reportable by the LSP. An
object-like macro would be a second and worse constant system whose values
bypass the type checker entirely.

More decisively, substitution would destroy property (1) above. Today an
inactive branch is parsed GTI. Under textual substitution the token stream
depends on the define set, so an inactive branch could not be parsed at all —
which is precisely why clangd cannot reliably analyse inactive regions in C++.
Keeping flags valueless is what makes the LSP work in this plan tractable.

If a value is genuinely needed, the supported spelling is a flag selecting a
`constexpr`:

```gti
#ifdef LARGE_BUFFERS
constexpr uint64_t BUFFER_SIZE = 65536;
#else
constexpr uint64_t BUFFER_SIZE = 4096;
#endif
```

### D2. Definitions are resolved before parsing, and are unit-ordered.

The define set is computed by the source loader, in token order, before the
parser runs. A flag is visible to every conditional after its `#define` in the
spliced token stream, and not before.

### D3. A `#define` inside a conditional branch takes effect only if that
branch is selected.

This is the one place where the existing architecture and C diverge in a way
that matters. Because the parser retains all branches, a naive implementation
would let a `#define` in a dead branch leak. It must not. The loader evaluates
conditions as it walks, so a nested `#define` is only admitted on a live path.

### D4. Include-order visibility, matching C.

`#include` is spliced by the loader, so a flag defined before an include is
visible inside it. This matches the C mental model the syntax already implies.
The existing rule that includes may not appear inside a conditional is
retained, which keeps the include graph independent of the define set — a
property the build system and LSP both rely on.

### D5. Directives still do not reach the backend.

Unchanged from today. Emitted C++ contains the selected branch and nothing
else. No `#define` in GTI source becomes a `#define` in generated C++.

## Proposed surface

### Directives

```
#define NAME
#undef NAME
#ifdef NAME          ==  #if defined(NAME)
#ifndef NAME         ==  #if !defined(NAME)
```

`NAME` is an ordinary identifier. Conventionally uppercase; not enforced.

### Condition grammar

The condition language grows from a single comparison to a boolean expression.
Proposed EBNF, to be added to `docs/language/grammar.ebnf` alongside the
existing `target-condition`:

```ebnf
compile-condition   = compile-or ;
compile-or          = compile-and { "||" compile-and } ;
compile-and         = compile-unary { "&&" compile-unary } ;
compile-unary       = [ "!" ] compile-primary ;
compile-primary     = "defined" "(" IDENTIFIER ")"
                    | target-comparison
                    | "(" compile-condition ")" ;
target-comparison   = "target" "." target-property ( "==" | "!=" ) STRING ;
```

This subsumes the current grammar without changing the meaning of any existing
condition. Both axes compose:

```gti
#if target.os == "linux" && defined(USE_EPOLL)
  // ...
#elif target.os == "macos" && defined(USE_KQUEUE)
  // ...
#else
  #error "no supported event backend selected"
#endif
```

Deliberately absent: arithmetic, `#if VERSION > 3`, and any comparison against
a flag's value — there are no values (D1).

### Where definitions come from

Three sources, in increasing precedence:

| Source | Spelling | Scope |
| --- | --- | --- |
| Project manifest | `[build] defines = ["DEBUG"]` in `gti.toml` | whole project |
| Command line | `gti -D NAME file.gti` | whole invocation |
| Source | `#define NAME` | from that point in the unit |

A later `#define` of an already-defined flag is a no-op, not an error.
`#undef` of an undefined flag is likewise a no-op, matching C.

Predefined flags are **not** proposed here. The target axis already covers
platform identification and adding `__GTI__`-style predefines would create two
overlapping ways to ask the same question.

## Diagnostics

Proposed codes. The next free value in each family was checked against the
tree: `GTI-L` is allocated to 0011, `GTI-I` to 0010, `GTI-P` to 0002, `GTI-S`
to 2074.

| Code | Phase | Condition |
| --- | --- | --- |
| `GTI-L0012` | Lexer | `#define`/`#undef`/`#ifdef`/`#ifndef` not followed by an identifier |
| `GTI-I0011` | Loader | `#define` names a flag with a replacement list — "Configuration flags carry no value; use a `constexpr` selected by `#ifdef`." |
| `GTI-I0012` | Loader | `#undef` without a matching visible `#define` in a `--strict-defines` build (opt-in; a no-op otherwise) |
| `GTI-P0003` | Parser | malformed boolean condition — unbalanced parentheses, missing operand |
| `GTI-P0004` | Parser | `defined` used outside a compile-time condition |
| `GTI-S2075` | Semantics | a flag is referenced by `#ifdef` but defined nowhere in the project — warning, not error; catches the misspelling C silently treats as false |

`GTI-S2075` is the one with no C equivalent and the highest practical value.
`#ifdef DEBGU` in C is silently false forever. GTI can see the whole define set
and say so.

## LSP: inactive region rendering

### Why GTI can do this better than clangd

clangd greys inactive regions using line ranges recovered from the
preprocessor, because in C++ the inactive text was never parsed and often
*cannot* be. GTI parses every branch into `ConditionalStmt` already, so the
inactive extent is a set of real AST nodes with exact source spans. The
rendering can be precise rather than line-granular, and it stays correct for
nested conditionals with no extra machinery.

This is a direct dividend of D1 — it only holds because flags carry no value.

### Mechanism

Two candidates, both viable in Neovim:

1. **Semantic token modifier** — add `inactiveCode` to the `tokenModifiers`
   legend in `src/lsp/main.cpp` (line 1483, currently `declaration`,
   `definition`, `readonly`, `defaultLibrary`, `functionScope`, `static`).
   Clients map it to `@lsp.mod.inactiveCode`, which the user styles.
2. **`DiagnosticTag.Unnecessary`** — standard LSP since 3.15, renders as faded
   in most clients with no configuration.

**Recommendation: the semantic token modifier**, with `DiagnosticTag` rejected
rather than kept as a fallback. Inactive code is not a diagnostic; publishing
one per inactive branch would fill the diagnostic list and the quickfix window
with entries the user cannot act on. The legend already exists, so the modifier
costs one string.

Note the client-support caveat: clients that do not request semantic tokens get
no greying and must not be broken by its absence. The
[LSP evolution plan](lsp-evolution.md) acceptance checks apply.

### Work required

The blocker is that the tooling layer currently walks **only** the active
branch — `language_queries.cpp` lines 1343 and 1510 call `activeBranch(target)`
and recurse into it alone. Inactive branches are in the AST but invisible to
every query.

1. Extend the tooling walk to visit inactive branches, tagging their tokens
   with the new modifier. Semantics must still analyse only the active branch:
   this is a tooling-layer traversal, not a semantic one, and must not be
   implemented by widening `visitConditionalStmt`.
2. Ensure inactive-branch tokens are lexically classified only. An inactive
   branch has no resolved types, so requesting hover or go-to-definition inside
   one must degrade cleanly rather than fabricate a result.
3. Recompute on define-set change. Editing `gti.toml` defines, or a `#define`
   earlier in the file, must invalidate the token cache for the unit.

### Acceptance

Open a file containing a `#if target.os == "..."` chain with a non-matching
arm and a `#ifdef` arm. The non-selected arms render faded, the selected arm
renders normally, nesting is correct, and toggling a flag in `gti.toml` flips
the rendering after a reload.

## Implementation phases

Each phase is independently landable and independently verifiable. Phases 1–3
are compiler-side and have no LSP dependency; phase 5 depends on 1–3 only.

| Phase | Scope | Exit gate |
| --- | --- | --- |
| **C1** | Boolean condition grammar (`&&`, `\|\|`, `!`, parentheses) over the *existing* `target.*` primary. No flags yet. | Existing conditionals unchanged; new operator tests pass; `grammar.ebnf` updated |
| **C2** | `#define`/`#undef` lexing and the loader-side define set, including D3 (no leak from dead branches) | A flag defined in a dead branch is not visible after `#endif` |
| **C3** | `defined()`, `#ifdef`, `#ifndef` as condition primaries | Full matrix of flag/target combinations selects the right branch |
| **C4** | Define sources: `-D` on the CLI, `[build] defines` in `gti.toml`, documented precedence | Same source compiles two ways under two flag sets; `gti.toml` round-trips |
| **C5** | LSP inactive-region modifier and the tooling-layer walk | The acceptance scenario above |
| **C6** | `GTI-S2075` unknown-flag warning | Misspelled `#ifdef` is reported; correctly spelled one is silent |

C1 is deliberately first and flag-free: it is the only phase that touches the
existing condition parser, so landing it alone keeps the blast radius on
already-shipped conditionals small and testable.

## Tests

- **Parser/loader unit tests** for each new diagnostic code, following the
  fixture pattern in `tests/fixtures/`.
- **Selection matrix**: a fixture compiled under several define sets, asserting
  which branch reached the output. This is the core correctness test and should
  assert on emitted C++, not on a log.
- **Dead-branch define isolation** (D3) as an explicit named test — this is the
  rule most likely to regress silently.
- **Syntactic validity of inactive branches**: a fixture whose inactive branch
  contains a deliberate syntax error must still fail. This guards property (1)
  against an implementation that starts skipping tokens as an optimisation.
- **LSP**: extend `tests/lsp_smoke_test.py` with a semantic-token request over
  a conditional fixture, asserting the modifier appears on inactive spans and
  not on active ones.
- **No corpus movement**: `tests/mir_census_test.py` must be unchanged by C1–C4.
  These phases add no MIR bodies; a census delta means something leaked into
  the backend.

## Explicitly out of scope

Rejected by design, not deferred:

- Function-like macros, token pasting (`##`), stringification (`#`)
- Object-like macros with replacement lists (D1)
- `#pragma`, `#line`, `#warning`
- Conditional `#include` (the existing restriction stands, D4)
- Arithmetic or ordered comparison in conditions
- Predefined compiler-identity flags

Deferred, not rejected:

- Include guards via `#ifndef`/`#define`. These would work mechanically once
  C2 and C3 land, but GTI already resolves includes through a dependency graph
  with occurrence tracking, so the idiom would be redundant. Whether to endorse
  or diagnose it is a separate decision.

## Registration

This plan owns no work queue of its own.
[`docs/plans/implementation-sequence.md`](implementation-sequence.md) is the
single queue, and this work is registered there as **`S-CFG-01`**, order 4,
state *planned*, prerequisite `M-BACK-02` done. It must not preempt the active
backend-authority campaign.

[`docs/plans/language-alignment.md`](language-alignment.md) carries the
matching restriction row **`R-BUILD-CONFIG`** (class *choice*, readiness
*design-first*), recording that build-configuration variance is currently
inexpressible, with this document as the reconsideration evidence.

Once any phase lands, the implemented behaviour moves to
`docs/language/programs-and-targets.md` and `docs/language/grammar.ebnf`, per
the rule that `docs/plans/` describes future work only.
