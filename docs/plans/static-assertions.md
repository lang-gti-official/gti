# Static Assertions

Status: Proposal; not implemented or accepted as current GTI semantics.

## Summary

GTI should add a compiler-owned `static_assert` declaration with the familiar
forms:

```gti
static_assert(condition);
static_assert(condition, "explanation");
```

The condition must have exact type `bool` and must be evaluable by GTI's
existing bounded constexpr evaluator. If it evaluates to `false`, compilation
fails with a GTI semantic diagnostic. If it evaluates to `true`, the assertion
has no executable effect and is erased before HIR.

The C++ backend must not evaluate or emit source `static_assert` declarations.
Native C++ constant evaluation, template-instantiation timing, diagnostic text,
and generated-source locations are not GTI language authority.

## User-Facing Outcome

The first client is a package author validating target-owned native layout and
configuration facts close to a C interoperability declaration:

```gti
[[c_abi]] struct NativePoint {
  float x;
  float y;
};

static_assert(sizeof(NativePoint) == uint64_t(8),
              "NativePoint must match the C ABI");
```

The same surface lets ordinary libraries state invariants over constexpr
constants without manufacturing an invalid array extent, selecting an active
`#error` branch, or waiting for the generated C++ compiler to reject an
artifact.

The eventual dependent form supports generic invariants:

```gti
class inline_buffer<T, uint64_t N> {
  static_assert(N <= uint64_t(1024), "inline_buffer capacity is too large");

  T values[N] = {};
};
```

That assertion is checked for each concrete `inline_buffer<T, N>` instance, not
while `N` is still symbolic.

## Current Baseline

Current GTI already has one compiler-owned, resource-bounded constexpr
evaluator. It supports fixed-width integers, `float`, `double`, `bool`, `char`,
`string_view`, `nullptr_t`, checked-integer expected results, scalar constexpr
bindings, non-generic constexpr free functions and static methods, structured
control flow, recursion, and target-owned `sizeof(type)` and `alignof(type)`
facts. Evaluation uses a 4096-step budget and a 64-call-depth limit.

`if constexpr` already requires a frontend-evaluated boolean and sends only the
selected branch into HIR. Active `#error` directives already produce a semantic
diagnostic and disappear before HIR.

The spelling `static_assert` is currently classified as a C++-reserved
identifier rather than a GTI keyword, so all proposed forms are invalid today.
The restriction ledger explicitly lists `static_assert` under `R-CONSTEXPR` and
requires any implementation to use GTI evaluation and source diagnostics.

## Proposed Syntax

The implemented grammar would eventually add:

```ebnf
static-assert-declaration
             = "static_assert" "(" assignment-expression
               [ "," STRING_LITERAL ] ")" ";" ;
```

The declaration is admitted wherever GTI currently admits compile-time
declarations:

- namespace scope, including inside a namespace;
- a complete class, struct, interface, or union body; and
- block scope, including function, method, constructor, destructor, and lambda
  bodies.

Assertions in interfaces and unions are permitted because they introduce no
member, behavior, storage, or layout of their own.

It is not admitted:

- as an expression;
- in a parameter or generic-parameter list;
- in a constructor initializer list;
- in a concept requirement list;
- in an `extern "C"` block; or
- after a class-like forward declaration.

The message is optional. In the first version it must be one ordinary quoted
string literal. GTI does not adopt C++26's arbitrary constant-expression
message family, formatting, interpolation, or concatenated literal rules.

Exactly one condition and at most one message are accepted. A trailing comma is
ill-formed. The terminating semicolon is required.

## Static Semantics

Semantic analysis resolves and type-checks the condition using ordinary GTI
name lookup, access control, exact overload selection, generic substitution,
unsafe rules, and source-order visibility.

The resolved condition must have exact type `bool`. GTI does not apply integer
truthiness, contextual conversion, `operator bool`, or another implicit
conversion for a static assertion:

```gti
static_assert(true);              // valid
static_assert(uint64_t(1) == 1);  // valid: exact bool result
static_assert(1);                 // error: condition is not bool
```

After successful type checking, the condition is evaluated with a fresh
constexpr execution context. It uses the same checked arithmetic, supported
operation set, target facts, step limit, call-depth limit, and deterministic
failure behavior as constexpr bindings and `if constexpr`.

The declaration has three semantic outcomes:

1. **Passed:** evaluation produced exact `bool true`; analysis continues and
   records a passed assertion fact for tooling.
2. **Failed:** evaluation produced exact `bool false`; compilation is invalid
   and the assertion-failure diagnostic is emitted.
3. **Not evaluable:** typing failed or bounded constexpr evaluation could not
   produce a boolean; compilation is invalid and the ordinary constexpr
   failure category explains the unsupported expression, runtime dependency,
   checked-operation failure, or resource limit.

`unsafe` does not make an otherwise non-constant assertion evaluable and does
not suppress checked operations.

## Names And Permitted Dependencies

A non-dependent assertion may use declarations visible at its lexical position,
including:

- literals and supported scalar operators;
- earlier constexpr bindings;
- available non-generic constexpr free functions and static methods;
- supported enum constants and checked-integer results; and
- supported target-owned `sizeof(type)` and `alignof(type)` queries.

A block-scope assertion may use an earlier local `constexpr` binding. It cannot
use an ordinary local, an ordinary function parameter, `this`, an instance
field, a runtime/native/intrinsic call, or another expression outside the
current constexpr evaluator:

```gti
int verify(int runtime_value) {
  constexpr int expected = 4;
  static_assert(expected == 4);       // valid
  static_assert(runtime_value == 4);  // error: runtime dependency
  return runtime_value;
}
```

Declaring the enclosing function `constexpr` does not make its ordinary
parameters constant at declaration analysis time. A parameter-dependent
assertion is therefore invalid unless that parameter is a GTI generic value
parameter handled by the dependent-assertion rules below.

## Conditional And Reachability Rules

An assertion is evaluated only when it belongs to an active configuration
branch. An assertion under an inactive `#if`, `#ifdef`, or `#ifndef` branch is
not analyzed for that target, matching current configuration semantics.

An assertion in the discarded branch of `if constexpr` is not evaluated. The
selected branch is analyzed normally.

Ordinary runtime reachability does not suppress a static assertion. Both arms
of an ordinary `if`, a loop body, a switch arm, and code following a return are
part of the compiled declaration and must satisfy their assertions even when a
particular runtime execution would not visit them:

```gti
int run(bool enabled) {
  if (enabled) {
    static_assert(false, "this fails compilation regardless of enabled");
  }
  return 0;
}
```

This keeps static assertions as declaration-time obligations rather than
control-flow operations.

## Generic-Dependent Assertions

The complete design distinguishes a non-dependent assertion from one whose
condition refers to an enclosing generic type or value parameter.

During symbolic generic analysis:

- name lookup, syntax-independent type rules, and the exact final `bool` type
  are checked as far as symbolic facts permit;
- a condition that can already be evaluated is evaluated immediately, even in
  an otherwise generic declaration; and
- a genuinely dependent condition is retained as a deferred semantic
  obligation rather than reported as an unsupported constexpr expression.

Concrete generic reanalysis substitutes the canonical type and value arguments,
reanalyzes the condition with the existing instance model, and evaluates the
assertion. Each canonical concrete class, function, method, or constructor
instance evaluates each dependent assertion once. Repeated uses of the same
instance do not create duplicate diagnostics.

A dependent assertion in a generic declaration that is never concretely used
does not fail merely because its condition remains symbolic. Independent
syntax, name, access, type, and non-dependent assertion errors still invalidate
the declaration immediately.

When a concrete assertion fails, the primary diagnostic points to the assertion
condition in the generic declaration. Related information identifies the
concrete type/value substitution and the first source use that required the
failing instance.

This rule is GTI-owned and does not copy historical C++ behavior around
`static_assert(false)` in uninstantiated templates. A literal false assertion is
non-dependent and fails during symbolic declaration analysis. A library that
intends a per-instance assertion must make the condition genuinely depend on a
generic parameter.

## Staged Delivery

The feature should ship in two coherent slices.

### Slice A: non-dependent assertions

The first slice accepts assertions whose complete condition can be evaluated
during ordinary semantic analysis. It covers namespace, class-like, and block
placement, target layout assertions, active/inactive configuration branches,
`if constexpr`, diagnostics, formatting, Tree-sitter, and LSP publication.

If an assertion refers to an enclosing generic parameter in Slice A, the
compiler emits a focused restriction diagnostic explaining that dependent
static assertions require concrete-instance support. It must not emit C++ and
hope native instantiation performs the check.

### Slice B: dependent assertions

The follow-on slice adds the retained obligation, canonical-instance
deduplication, substitution display, instantiation-related information, and
concrete class/function/constructor reanalysis coverage described above.

Slice B should land with a demonstrated generic client such as a bounded inline
capacity or a target-layout relationship over a supported generic field family.
It must not broaden constexpr execution to arbitrary aggregate values merely to
evaluate an assertion.

## Diagnostics And Recovery

The parser should recover malformed assertions at the next comma, right
parenthesis, semicolon, directive boundary, or enclosing brace as appropriate,
then continue parsing later declarations.

Required parser diagnostics include:

- missing `(`, condition, `)`, or semicolon;
- a non-string message;
- too many arguments;
- a trailing comma; and
- use in a syntactically excluded context.

Semantic diagnostics distinguish:

- a condition whose resolved type is not exact `bool`;
- a well-typed condition that the bounded constexpr evaluator cannot evaluate;
- a false assertion with no user message;
- a false assertion with a user message;
- a generic-dependent assertion before Slice B is available; and
- a concrete generic assertion failure after Slice B is available.

A false assertion uses the condition expression as its primary span. Without a
message, its wording is simply `Static assertion failed.` With a message, the
decoded text is appended after that stable prefix. Evaluation failures point at
the smallest failing subexpression when the evaluator provides one.

Generic failures add related information for the concrete substitution and
first requiring use. They do not relocate the primary error into generated C++
or report once per backend/compiler invocation.

The implementation should allocate a dedicated semantic diagnostic code for a
false assertion. Existing constexpr-evaluation failures remain in the
constexpr diagnostic family. Exact codes should be selected when implementation
lands so parallel diagnostic work cannot collide with this proposal.

No automatic fix-it is generally safe: deleting the assertion, changing the
condition, or changing the program invariant have different meanings. LSP code
actions must therefore not guess a replacement from diagnostic text.

## AST And Semantic Representation

The lexer promotes `static_assert` from `CPP_RESERVED` to a dedicated keyword
token and removes it from the remaining C++-reserved identifier table.

The parser creates a `StaticAssertDecl` AST node owning:

- the keyword token;
- both parenthesis tokens;
- the condition expression;
- the optional comma and message tokens;
- the terminating semicolon; and
- the complete source extent used by tooling and recovery.

Every statement visitor handles the node. The semantic model records an
assertion result keyed by declaration identity, including passed, failed, or
deferred state and any concrete generic substitution required for a deferred
result.

The AST node is not a binding and introduces no symbol or scope. Its condition
may create ordinary tooling occurrences for referenced symbols.

## HIR, MIR, Backend, And Runtime Boundary

Static assertions are non-executable syntax and static semantics. After a
successful frontend check:

- HIR lowering skips the declaration, like an active compile-time guard that
  has already succeeded;
- MIR contains no instruction, place, effect, control-flow edge, or assertion
  record;
- optimization has nothing to preserve or remove;
- `LoweredProgram` and backend input contain no source assertion;
- native-header generation does not reproduce it; and
- the C++ backend emits no corresponding `static_assert`.

This erasure is deliberate. Emitting a second C++ assertion would create two
semantic authorities and could make a valid GTI program depend on native C++
constant-expression or template rules.

Existing compiler-generated C++ layout guards remain permitted as backend
integrity checks. They are derived from verified representation facts and are
not a translation or second evaluation of a source `static_assert`.

No runtime support, failure descriptor, ownership transition, cleanup action,
ABI change, or executable-authority migration is required.

## Formatter, Tree-sitter, And LSP

The formatter preserves both accepted source forms and emits one space after a
message comma:

```gti
static_assert(sizeof(NativePoint) == uint64_t(8));
static_assert(ready, "configuration must be ready");
```

It does not wrap or rewrite the user message. Normal expression formatting
applies to the condition, and formatting remains idempotent for incomplete
assertions.

Tree-sitter adds a named `static_assert_declaration` node at declaration,
class-member, and block-item positions. The keyword receives keyword
highlighting, the condition uses ordinary expression nodes, and the message
uses ordinary string highlighting.

The LSP:

- publishes compiler-owned parse and semantic diagnostics unchanged;
- classifies `static_assert` as a keyword;
- provides hover/definition/references for names used by the condition through
  existing semantic occurrences;
- creates no document symbol for the assertion itself;
- formats through the compiler formatter; and
- clears an assertion diagnostic when a newer document version passes or
  removes it.

No LSP-specific constexpr evaluator or message-matching code action is allowed.

## Differences From C++

GTI deliberately keeps a smaller rule than C++:

- the condition must already have exact type `bool`; it is not contextually
  converted;
- the optional message is an ordinary quoted string literal;
- dependent timing follows GTI concrete-instance reanalysis rather than C++
  template rules or defect-report history;
- evaluation uses GTI checked arithmetic and target facts;
- failure uses stable GTI diagnostics and original GTI spans; and
- no C++ assertion is emitted.

Like C++, the declaration has no runtime or ABI effect and may appear at
namespace, class-like, or block scope.

## Deliberate Omissions

This proposal does not add:

- runtime `assert` or debug-only assertion policy;
- contracts, preconditions, postconditions, or assumptions;
- contextual conversion to bool;
- arbitrary constant-expression messages;
- formatted or interpolated messages;
- warning-only assertions;
- reflection over assertions;
- assertion values in HIR/MIR;
- a way to suppress checked constexpr failures with `unsafe`; or
- arbitrary aggregate/generic constexpr execution unrelated to the asserted
  condition.

`#error` remains the right tool for an unconditional active configuration
failure. `static_assert` is the right tool when a GTI constant expression
decides validity.

## Implementation Plan

### 1. Syntax and AST

- Add the keyword token and remove `static_assert` from the residual C++ keyword
  table.
- Parse the declaration in namespace, class-member, and block-item funnels.
- Add `StaticAssertDecl`, source extent, visitors, and focused recovery.
- Add lexer, parser, malformed-input, and inactive-configuration tests.

### 2. Non-dependent semantics

- Resolve the condition normally and require exact `bool`.
- Evaluate it with a fresh existing constexpr context.
- Record the semantic result and emit the dedicated false-assertion diagnostic.
- Reuse existing constexpr failure details for non-evaluable conditions.
- Skip successful assertions in HIR and prove absence from MIR/lowered/backend
  output.

### 3. Tooling and documentation

- Add formatter preservation/idempotence and incomplete-source recovery.
- Add Tree-sitter grammar, corpus, highlight, and indentation coverage.
- Add LSP diagnostics, semantic tokens, hover/reference, stale-version, and
  formatting coverage.
- Update the implemented grammar, static semantics, frontend, semantic,
  diagnostics, formatting, LSP, verification, roadmap, and restriction ledger.

### 4. Dependent assertions

- Detect generic dependency without treating all evaluation failure as
  dependency.
- Retain deferred obligations in semantic instance analysis.
- Substitute and evaluate through canonical concrete reanalysis.
- Deduplicate by concrete instance identity and attach the first requiring use.
- Cover class, function, method, and constructor type/value substitutions plus
  unused declarations and repeated instances.

### 5. Release

- Advance the minor version when Slice A ships because it adds user-visible
  syntax and diagnostics.
- Advance the minor version again if Slice B ships separately because it admits
  previously rejected generic programs.
- Do not change `VERSION` for this proposal alone.

## Verification Plan

Focused positive coverage for Slice A includes:

- both message forms in the global namespace, a named namespace, class-like
  bodies, and block scope;
- every supported constexpr scalar family;
- supported `sizeof`/`alignof` native-layout facts across synthetic targets;
- earlier namespace/static/local constexpr dependencies;
- nested constexpr function calls and deterministic resource boundaries;
- active/inactive configuration branches and selected/discarded
  `if constexpr` branches;
- formatter, Tree-sitter, semantic-token, occurrence, and LSP clearing behavior;
  and
- absence from HIR, MIR, lowered-program printing, native headers, and emitted
  C++.

Focused negative coverage includes every parser recovery case, non-`bool`
conditions, runtime dependencies, unsupported operations, checked constexpr
failure, step/depth exhaustion, false assertions with escaped/empty messages,
and placement in `extern "C"` or other excluded contexts.

Slice B adds symbolic-valid/concrete-pass, symbolic-valid/concrete-fail, unused
dependent declarations, immediately failing non-dependent assertions inside a
generic declaration, type and value substitutions, multiple use sites,
canonical-instance deduplication, and exact related-information spans.

The implementation should run the focused compiler pipeline first, followed by
formatter, Tree-sitter, LSP protocol, layout-query, native-record, HIR/MIR,
lowered-program, backend-boundary, CLI, and installed-toolchain checks selected
by the canonical verification guide. No runtime matrix is required solely for
an erased assertion, but existing runtime/corpus gates must confirm its absence
does not change accepted-program behavior.

## Acceptance Criteria

The proposal is ready for implementation when maintainers agree that:

1. `static_assert` is a declaration with exact-`bool` GTI constexpr semantics;
2. an optional message is limited to one quoted string literal;
3. namespace, class-like, and block scopes admit it while `extern "C"` does not;
4. configuration and `if constexpr` selection suppress only inactive/discarded
   assertions, not ordinary runtime-unreachable assertions;
5. Slice A rejects generic-dependent conditions explicitly;
6. Slice B evaluates dependent conditions once per canonical concrete instance;
7. failures retain original GTI spans and useful instantiation context;
8. successful assertions disappear before HIR and are never emitted as C++;
   and
9. implementation updates all compiler/tooling documentation and advances the
   minor version for each separately shipped language-surface slice.
