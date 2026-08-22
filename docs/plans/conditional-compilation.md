# Conditional Compilation And Configuration Flags

> **Plan status:** implemented by `S-CFG-01` for GTI 0.292.0. The language
> contract now lives in
> [`programs-and-targets.md`](../language/programs-and-targets.md) and the
> canonical grammar. This document retains the design rationale and delivery
> evidence.

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

## Prior baseline

This was the implemented baseline before `S-CFG-01` and is retained to explain
the architecture the slice extended rather than replaced.

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

## Implemented surface

### Directives

```
#define NAME
#undef NAME
#ifdef NAME          ==  #if defined(NAME)
#ifndef NAME         ==  #if !defined(NAME)
```

`NAME` is an ordinary identifier. Conventionally uppercase; not enforced.

### Condition grammar

The condition language grew from a single comparison to a boolean expression.
The canonical EBNF in `docs/language/grammar.ebnf` now uses this precedence:

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

Predefined flags are deliberately absent. The target axis already covers
platform identification and adding `__GTI__`-style predefines would create two
overlapping ways to ask the same question.

## Diagnostics

The delivered diagnostic contract is:

| Code | Phase | Condition |
| --- | --- | --- |
| `GTI-L0012` | Lexer | `#define`/`#undef`/`#ifdef`/`#ifndef` not followed by an identifier |
| `GTI-I0011` | Loader | `#define` names a flag with a replacement list — "Configuration flags carry no value; use a `constexpr` selected by `#ifdef`." |
| `GTI-P0003` | Parser | malformed boolean condition — unbalanced parentheses, missing operand |
| `GTI-P0004` | Parser | `defined` used outside a compile-time condition |
| `GTI-S2075` | Semantics | a flag is referenced by a condition but defined nowhere in the compilation — warning, not error; catches the misspelling C silently treats as false |

The optional `--strict-defines` mode and its proposed `GTI-I0012` diagnostic
did not ship; ordinary `#undef` of an absent flag is intentionally a no-op.

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

Two candidates were considered:

1. **Semantic token modifier** — add `inactiveCode` to the `tokenModifiers`
   legend in `src/lsp/main.cpp` (line 1483, currently `declaration`,
   `definition`, `readonly`, `defaultLibrary`, `functionScope`, `static`).
   Clients map it to `@lsp.mod.inactiveCode`, which the user styles.
2. **`DiagnosticTag.Unnecessary`** — standard LSP since 3.15, renders as faded
   in most clients with no configuration.

The implementation uses the semantic token modifier, with `DiagnosticTag`
rejected rather than kept as a fallback. Inactive code is not a diagnostic; publishing
one per inactive branch would fill the diagnostic list and the quickfix window
with entries the user cannot act on. The legend already exists, so the modifier
costs one string.

Note the client-support caveat: clients that do not request semantic tokens get
no greying and must not be broken by its absence. The
[LSP evolution plan](lsp-evolution.md) acceptance checks apply.

### Delivered work

1. `SourceUnit` retains loader-resolved inactive token spans plus exact
   configuration operator/flag-name roles. The semantic token adapter applies
   `inactiveCode` and classifies flag names as macros without widening semantic
   analysis or reconstructing directive syntax.
2. `#define` and `defined` use precise Tree-sitter captures; other directive
   keywords retain their LSP macro classification. Other inactive identifiers
   use lexical variable classification only; semantic queries degrade cleanly
   when no resolved occurrence exists.
3. The LSP watches `gti.toml`, reloads `[build].defines`, and invalidates every
   open document's analysis and token cache when the manifest changes. A source
   edit already follows the ordinary document-generation invalidation path.

### Acceptance

Open a file containing a `#if target.os == "..."` chain with a non-matching
arm and a `#ifdef` arm. The non-selected arms render faded, the selected arm
renders normally, nesting is correct, and toggling a flag in `gti.toml` flips
the rendering after a reload.

## Implementation phases

All six phases landed as one coherent vertical slice. The dependency order is
retained below as implementation history.

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

## Verification coverage

- `compiler_pipeline` covers every new diagnostic, boolean/source/include
  selection under multiple flag sets, externally seeded `#undef`, dead-branch
  isolation, inactive-branch syntax rejection, emitted C++, and formatting.
- `project_model`, `cli_workflow`, and `project_cli_workflow` cover manifest
  validation/canonicalization, plan/cache identity, metadata schema 9, direct
  and project `-D`, and successful-warning presentation.
- Tree-sitter corpus/query gates cover the syntax and highlighting.
- `lsp_protocol` asserts the `inactiveCode` modifier on nested inactive spans
  and flips the selected arm after a watched `gti.toml` define change.
- `mir_census_regression` remains unchanged: the feature adds no MIR bodies and
  leaks no directive into the backend.

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
single queue, where **`S-CFG-01`** is recorded complete.

[`docs/plans/language-alignment.md`](language-alignment.md) carries the
matching restriction row **`R-BUILD-CONFIG`** as closed. Implemented behaviour
is specified in `docs/language/programs-and-targets.md` and
`docs/language/grammar.ebnf`; this plan is rationale and historical evidence.
