---
name: gti-language
description: Develop and review the GTI C++-like compiled language, including syntax, lexer, parser and AST, semantic rules, optimization and backend architecture, C++ lowering, source loading, target conditionals, standard library, native runtime, CLI, formatter, LSP, diagnostics, tests, grammar, examples, and editor tooling. Use for any GTI language design or implementation task and when onboarding to this repository.
---

# GTI Language

Develop GTI as a C++-familiar compiled language that preserves explicit
control, value semantics, RAII, performance, and native interoperability while
removing avoidable hazards and accidental complexity.

## Start Every Task

1. Run `git status --short`. Preserve unrelated user changes and generated
   artifacts already present in the worktree.
2. Read `docs/grammar.ebnf`, then read
   [references/architecture.md](references/architecture.md) before changing an
   unfamiliar compiler phase.
3. Trace the current behavior through the real implementation. Do not assume a
   C++ feature exists merely because GTI resembles C++.
4. Read [references/change-guide.md](references/change-guide.md) and identify
   every affected layer before editing user-visible syntax or behavior.
5. Establish a focused baseline test when practical, then keep the change
   coherent through implementation, diagnostics, tests, docs, and tooling.

## Language Invariants

- Prefer familiar C++ spelling and precedence unless GTI deliberately adopts a
  safer or simpler rule.
- Keep bindings and parameters immutable by default. Require `mut` for state
  that can change.
- Keep `char` an exact unsigned 8-bit code unit distinct from `uint8`. Give
  string literals the trivial, counted `std::string_view` type over static
  storage; preserve embedded zero bytes and do not reintroduce an unqualified
  `string` primitive. Keep view traversal read-only and bounds checked. Keep
  owning text and formatting in the standard library: `std::string` is a
  source-defined move-only owner over `gti_internal::storage<char>`, with
  explicit allocating `clone()`. Do not expose a dynamic string view until it
  can carry an owner-tied lifetime.
- Keep `auto` initializer-driven and local to variable declarations, including
  loop initializers. Infer one exact complete value type in semantics, retain
  its access and ownership traits in HIR, and require `mut auto` for mutation.
  Do not infer globals, fields, parameters, or returns; reject `auto` reference
  and array declarators and untyped braced initializers. Do not delegate invalid
  ownership copies to a backend.
- Keep lambdas explicit and lexical. Require named immutable value captures,
  explicit parameter and return types, and exact calls. Reject capture defaults,
  reference or init captures, `this`, mutable closure state, noncopyable
  captures, and closure escape until callable interfaces and lifetime analysis
  define those behaviors.
- Keep construction explicit and resolve constructor overloads by one exact
  parameter-type match. Permit declared class and struct bindings to avoid type
  repetition with `Type name{arguments};`; treat it as direct construction, not
  C++ list initialization. Do not extend it to primitives, arrays, references,
  enums, `auto`, copy-list syntax, initializer-list preference, aggregate
  initialization, or parenthesized block declarations. Generate
  default/copy/move/assignment/destruction from frontend lifecycle metadata
  instead of inheriting C++ special-member suppression. Keep fields immutable
  by default in semantics. Keep methods read-only by default and use a trailing
  `mut` only for methods that require a mutable receiver. Permit an ordinary
  method to pair read-only and mutable receiver overloads with otherwise exact
  signatures; read-only receivers select the former and mutable receivers
  prefer the latter.
- Spell the current-object expression `this` for C++ familiarity, while keeping
  it a non-null object receiver with `this.member` access rather than exposing a
  raw pointer. Do not retain `self` as an alias; it is an ordinary identifier.
- Treat `~Type()` as automatic, public, non-throwing cleanup with an implicitly
  mutable receiver. Run it only for an active value before reverse-order field
  destruction. Cleanup-owning classes are noncopyable; generated moves must
  transfer active-drop state and move assignment must clean the old target
  before replacement. Do not expose manual destructor calls.
- Keep enums scoped and nominal. Accept `enum class`, default its backing type
  to `int32`, and require enumerators to be referenced through the enum type.
  Do not inject enumerator names or add implicit integer/bool conversions.
- Keep `switch` exact and non-fallthrough. Permit concrete integers, `char`,
  and scoped enums; require same-type compile-time labels, reject duplicates,
  and require every arm to terminate explicitly. Adjacent labels share an arm,
  and every arm has its own lexical scope. `break` may exit a loop or switch;
  `continue` remains loop-only.
- Keep named generics predictable. Infer function type arguments exactly from
  value arguments; do not introduce conversion-driven deduction,
  specialization, or unconstrained compile-time metaprogramming by accident.
  A type parameter may carry one frontend-owned standard constraint with syntax
  such as `std::numeric T`. Keep the supported identities confined to
  `std::ordered`, `std::numeric`, `std::signed_numeric`, `std::integral`,
  `std::signed_integral`, `std::unsigned_integral`, and
  `std::floating_point`. Check concrete arguments, symbolic implication, and
  every constrained pack element without adding constraint-based overload
  ranking, signature distinction, user-defined concepts, or `requires` clauses.
  Treat them as primitive numeric categories, not structural detection of class
  operators. Permit checked `T(value)` only when T satisfies `std::numeric`.
  Classes and structs may follow type parameters with immutable `uint64` value
  parameters. Confine their arguments to integer literals or an in-scope value
  parameter, and their use to fixed-array extents and nested class arguments.
  Keep value parameters out of functions, packs, defaults, and arbitrary
  constant expressions until those semantics are designed.
- Keep `using Name = Type;` aliases namespace-scoped, transparent, and
  declaration-order independent. Canonicalize aliases before overload,
  ownership, HIR, and backend decisions; reject cycles, generic aliases, and
  reference targets until those features have explicit semantics. Define
  `std::size_t` as `uint64` and `std::ptrdiff_t` as `int64` in the prelude
  rather than as compiler primitives.
- Keep variadic generics confined: one final function or method type pack, one
  matching final immutable by-value parameter pack, and expansion only as the
  final argument to another variadic callable. Preserve exact element types and
  consume a concrete pack as one unit on its first expansion when any element is
  move-only; copyable packs may be expanded repeatedly. Do not add per-element
  pack access before HIR can track independently owned pack places. Continue to
  reject arbitrary expansion contexts, class packs, folds, indexing, multiple
  packs, and forwarding-reference deduction until their semantics are designed.
- Resolve overloads by one unique exact parameter-type match after generic
  substitution. Do not add implicit call conversions, conversion ranking, or
  return-type overloading. Receiver mutability may distinguish methods but not
  free functions. Record the selected function identity in semantics.
- Keep operator overloading member-only and restricted to `operator*`,
  `operator->`, `operator[]`, `operator()`, `operator==`, `operator!=`, and
  contextual `operator bool`. `operator()` may have arbitrary arity but remains
  a non-generic member overload. Resolve exact operands and receiver access in
  semantics, record the selected function identity, and lower a direct method
  call. Do not delegate GTI operator selection to C++, synthesize equality
  candidates, add ADL, or recursively resolve arrow proxies.
- Treat `&&`/`||` as exact lexical aliases of `and`/`or`. Normalize each pair
  to one logical token identity so precedence, contextual boolean conversion,
  short-circuiting, optimization, and backend behavior cannot diverge.
- Keep numeric conversions explicit with `Type(value)`. Preserve checked
  narrowing behavior in every backend instead of emitting unchecked casts.
- Treat fixed arrays as inline bounded values. Keep C++ declarator spelling,
  compile-time length identity, complete initialization, checked indexing, and
  no pointer decay or public raw-data escape. Preserve bounds checks unless an
  optimization proves them unnecessary.
- Require every non-`void` call result to be used. Permit intentional call-site
  suppression only through `[[discard]]`.
- Reject any non-`void` function, method, operator, or lambda that can reach
  the end of its body. Keep this a semantic control-flow guarantee rather than
  relying on C++ warnings; only top-level `main` has an implicit zero return.
  Until a typed command-line argument surface exists, require a defined
  `int main()` with no parameters.
- Model recoverable failure with built-in `expected<T, E>` and explicit
  `unexpected(error)`. Do not add exceptions or implicit propagation syntax.
- Reject invalid GTI in semantic analysis instead of depending on generated C++
  errors.
- Reserve the `__gti_` identifier prefix for compiler-generated backend names;
  reject source identifiers using it before they can collide with lowering.
- Avoid textual macros, order-dependent semantics, hidden conversions, and
  accidental undefined behavior.
- Define integer edge cases at the GTI level. Keep modulo-by-zero and invalid
  shifts checked, and do not lower them to raw undefined C++ operations.
- Keep ownership, lifetime, nullability, and conversions explicit as those
  systems are introduced. Do not inherit unsafe C++ defaults by omission.
- Treat `std::move(value)` as an explicit unary move operation, not a library
  hint. Permit named movable local values and by-value parameters, including
  copyable and generic values; consume the source until valid plain assignment
  reinitializes a `mut` binding. Reject references, globals, fields, captures,
  temporaries, and partial places until their lifetime or initialization state
  has a sound model. Reject direct self-move assignment. Retain explicit
  movement in binding metadata and HIR so a backend cannot silently turn it
  into a copy through physical constness.
- Permit method reference returns only when the borrow is proven to originate
  from `this`. A leading `mut T&` return requires a trailing mutable receiver
  and a writable returned place. Record the receiver or intrinsic argument that
  owns a borrowed call result, classify it with its access mode, and reject
  retained borrows from temporary storage. Conservatively reject invalidating
  operations on a borrowed move-only root until lexical loan analysis can prove
  the borrow has ended. Do not generalize this into free-function reference
  returns without an explicit lifetime model.
- Derive class and struct ownership traits recursively from substituted field
  types. Reject aggregate copies and use after move in semantics, and use
  recorded binding traits rather than nominal spelling in backends.
- Keep raw pointers, pointer arithmetic, `new`, and `delete` out of ordinary
  safe GTI. Implement familiar `std` ownership and container types as nominal
  GTI classes over restricted `gti_internal` capabilities, and use non-null
  references for borrows. A future explicitly opt-in `dangerous` surface may
  expose selected capabilities, but do not commit its spelling or leak backend
  representation before its contracts are designed. Follow
  `docs/ownership.md`.
- Bind internal capabilities by trusted semantic identity, never by the public
  `std` wrapper that uses them. Adding an intrinsic does not make it a stable
  application API.
- Keep each intrinsic irreducible. It may enforce private allocation, bounds,
  initialization, borrow, and drop invariants, but must not expose stdlib policy
  such as logical size, capacity, engagement, or per-slot state. Store and
  interpret those facts in ordinary nominal GTI classes. Keep public factories
  such as `std::make_unique` on the normal generic call path. Retain a compound
  intrinsic only while current language semantics cannot express it safely, and
  document the missing capability that would allow its removal.
- Treat C++ smart pointers as a C++ backend representation, never as the GTI ABI
  or a C runtime binding. Preserve ownership, transfer, and drop semantics in
  frontend metadata and later HIR/MIR operations.
- Treat `include "path.gti"` as dependency loading, never textual substitution.
  Keep it top-level, relative, canonicalized, load-once, and cycle-checked.
  Resolve `include <std/name>` only beneath the configured GTI standard-library
  root, retain its logical import name, and never consult project or native C++
  include paths.
  Parse source units independently and expose only the current unit, its direct
  includes, and the implicit prelude. Do not leak transitive or sibling
  declarations through the whole-program backend representation. Only the
  entry source unit may declare top-level `main`.
- Keep `#if` restricted to target selection. Do not grow it into a general macro
  processor.
- Keep output and other services out of the parser. Expose ordinary GTI APIs in
  `stdlib/` and cross to the host only through validated runtime bindings.
- Bind runtime services by semantic identity, never by matching public names
  such as `print` in the backend.
- Route compiler errors through `diagnostic.h`. Preserve exact source spans,
  stable phase-specific codes, related declarations, hints, and mechanical
  fix-its when the compiler can state a correction without guessing.
- Keep the C++ backend replaceable. A backend limitation is not automatically a
  language rule.
- Resolve installed resources from the OS-reported executable path, never from
  a bare `argv[0]`. Release binaries must not embed CI checkout paths.

## Phase Ownership

Keep the pipeline ordered and one-directional:

```text
source loading -> lexing -> parsing/AST -> target selection + semantics
               -> typed HIR -> optimization -> backend -> artifact
               -> toolchain driver
```

- Put tokens and spelling recognition in `token.h` and `lexer.h`.
- Put syntax and recovery in `parser.h`; do not resolve names or types there.
- Put syntax structure and visitor contracts in `ast.h`.
- Put name resolution, type rules, mutability, nodiscard, and runtime-binding
  validation in `semantic_analyzer.h`.
- Put canonical units and direct dependency edges in `source_graph.h`. Preserve
  unit identity through semantics and HIR even while the backend consumes one
  dependency-ordered `Program`.
- Put stable enum, class, callable, destructor, binding, statement, and value
  instances plus executable bodies and concrete generic substitutions in
  `hir.h`. Recheck ownership-sensitive generic bodies there through the
  semantic analyzer rather than adding a second type system.
- Enter reusable analysis through `frontend.h`; keep CLI and LSP phase ordering
  identical.
- Put target-independent optimization decisions in `optimizer.h`. Passes
  consume typed HIR values and key results by `HirValueId`; they must not
  re-walk the AST or infer semantics from source spelling.
- Keep backend contracts in `backend.h`; put C++ representation choices in
  `cpp_backend.h` and `cpp_emitter.h`.
- Treat the C++ emitter's AST traversal as transitional. Route any middle-end
  decision it consumes through HIR identities, and move syntax traversal to
  HIR or MIR incrementally without dropping source provenance.
- Treat typed HIR as the backend-independent instance representation and the
  checked AST as its source-provenance layer. Do not add an LLVM-shaped IR
  until ownership, layout, ABI, and generic instantiation rules can be
  represented explicitly in MIR.
- Keep reusable compiler facilities under `include/gti/`; keep `src/cli/` and
  `src/lsp/` as drivers.
- Put semantic IDE facts in `SemanticDatabase` and reusable rendering/query
  logic in `language_queries.h`. The LSP may retain immutable frontend
  snapshots and serialize results, but must not resolve names, infer types, or
  select overloads independently.
- Keep tooling `SymbolId` values snapshot-scoped and connect resolved
  occurrences to exact symbols with explicit roles. Navigation must use those
  identities and fail closed; never search equal identifier spellings.
- Keep the Tree-sitter grammar and queries (`tree-sitter-gti/`, `queries/gti/`)
  and root-level Neovim plugin files (`plugin/`, `lsp/`, `lua/gti/`,
  `ftdetect/`, `ftplugin/`, and `syntax/`) synchronized with LSP behavior.
- Keep the LSP request loop responsive. Run frontend diagnostics on coalesced
  document snapshots, reject stale generations, and avoid repeating analysis
  for `didSave` after full-sync `didChange`.
- Keep semantic-token position conversion linear in the document and cache
  results by document generation. Update the legend, Neovim links, fallback
  syntax, and protocol coverage together when classifications change.
- Keep Tree-sitter responsible for syntax structure and LSP semantic tokens
  responsible for resolved roles. Regenerate `tree-sitter-gti/src/parser.c`
  after grammar changes and reject `ERROR` or `MISSING` nodes in valid examples.
- Keep portable APIs in GTI under `stdlib/`; keep the narrow C ABI and host code
  under `runtime/`.
- Update `formatter.h`, LSP semantic tokens, and the Neovim runtime files when
  new syntax needs formatting or highlighting.

See [references/architecture.md](references/architecture.md) for concrete APIs,
data flow, and cross-phase traps.

## Change Workflow

1. State the language rule independently of its C++ lowering.
2. Choose the owning phase and make the smallest coherent cross-layer change.
3. Add focused positive and negative coverage. Test diagnostics at the GTI
   phase that owns the rule.
4. Update `docs/grammar.ebnf` and an example for user-visible syntax.
5. Update CLI, LSP, formatter, standard library, or runtime only when the change
   crosses those boundaries.
6. Inspect emitted C++ when backend behavior changes, but test source-level
   rejection before emission for invalid programs.
7. Follow the exact impact matrix and checks in
   [references/change-guide.md](references/change-guide.md).

## Released Tooling

Lazy users who configure `{ "LukeHHA/gti", version = "*" }` run the toolchain
downloaded for the latest release tag, not binaries built from `main`. Diagnose
editor reports by checking `:GTIInfo` and the installed
`toolchain/share/gti/VERSION` before changing the source LSP.

Every shipped compiler, standard-library, runtime, LSP, formatter, Tree-sitter,
or Neovim plugin change must advance `VERSION`; CI enforces this with
`scripts/check_release_version.py`. Verify that `gti --version`,
`gti_lsp --version`, and LSP `serverInfo.version` use it, then commit and push
the change. A `VERSION` change on `main` starts `.github/workflows/release.yml`,
which creates the matching `vX.Y.Z` tag after all platform packages pass. Do
not create a second tag manually and do not describe the release as available
until its archives have been published successfully.

## Verification

Keep C++ formatting scoped to the lines owned by the current task. Use
`git clang-format --diff HEAD` to inspect the proposed formatting and
`git clang-format HEAD` to apply it. Do not run `clang-format -i` over complete
existing files during feature work. Inspect `git diff --stat` immediately after
formatting and revert only formatter changes introduced by the current task if
the diff expands beyond the intended lines.

Run the broad suite before completing a compiler change:

```sh
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure
./build/gti examples/07-generics.gti -o /tmp/gti-generics
/tmp/gti-generics
git diff --check
```

Use an output under `/tmp` to keep generated executables out of the worktree.
If `json-c` is unavailable, report that the LSP target and protocol test were
skipped rather than implying full coverage.

After verification, stage only task-owned changes. This repository expects
completed changes to be committed and pushed unless the user says otherwise.
Never include unrelated README edits, binaries, logs, or user work in that
commit.
