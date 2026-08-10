# GTI Compiler Completeness Audit

Status: internal implementation assessment

Assessment date: 2026-08-11

This assessment records a broad cross-feature review of the current compiler.
It is not a normative language contract and does not claim that the compiler is
complete. Its purpose is to preserve the review method, the bug patterns it
exposed, and a reusable checklist for future language work.

## Method and evidence

The review traced representative features through the implemented authority
chain rather than treating successful C++ generation as sufficient:

```text
source -> parser/AST -> semantics -> concrete typed HIR -> MIR -> C++ backend
                              |             |          |
                              +-------------+----------+-> LSP queries
```

The sampled areas included generics, inheritance and virtual dispatch,
construction and emplacement, moves and retained loans, raw pointers and
unsafe operations, conditionals, loops, switches, cleanup, effect
classification, declarator fidelity, and backend evaluation behavior. Evidence
included:

- focused valid and invalid source probes under `/tmp`;
- clean rebuilds of the exact CLI under review before accepting a probe as a
  compiler defect, so a stale local executable cannot create a false finding;
- inspection of semantic selections, concrete HIR instances, printed MIR, and
  emitted C++ for the same program;
- native compilation and execution of selected probes;
- structural regression assertions at semantic, HIR, and MIR boundaries; and
- focused compiler, optimizer, raw-pointer, CLI, and protocol tests as
  appropriate to each patch.

The review also checked whether quality gates exercised the intended tests.
The sanitizer selection had omitted `raw_pointer_pipeline`, so a normal green
raw-pointer test did not prove that the same path ran with sanitizers. The gate
now includes that pipeline.

Green tests are evidence that the covered contracts remain intact. They are
not a completeness proof: a test can exercise correct generated C++ while an
unused or transitional IR layer silently carries the wrong identity.

## Bug families exposed by the review

| Family | Cross-phase failure pattern | Review disposition |
| --- | --- | --- |
| Inherited generic target instance | An inherited call selected the right base method semantically, but HIR reconstructed owner arguments from the derived receiver. This could create a symbolic or incorrectly shaped base instance while MIR still verified and the transitional C++ backend happened to run correctly. | Patched. HIR now consumes the resolved dispatch owner for ordinary calls, operators, and contextual conversions; focused type- and value-generic regressions inspect HIR and MIR directly. |
| Edge-sensitive move state | AST-side move/place state is path-sensitive at ordinary joins but is not fully edge-sensitive. Reachable loop backedges, terminating arms, short-circuit right-hand reachability, and state changes in unreachable statements can otherwise be accepted or rejected incorrectly. | Patched with bounded scope snapshots, explicit loop/switch exit states, fallthrough-aware joins, isolated unreachable tails, and conservative backedge validation. Focused regressions cover `while`, `do`/`while`, classic `for`, `break`, `continue`, terminating arms, and short-circuit paths. This does not replace the planned MIR fixed-point dataflow authority. |
| Raw-pointer pack and ranking | Forwarded variadic/storage arguments could lose the bounded `T*` to `const T*` and `nullptr` compatibility. Multi-argument overload ranking could also sum conversions instead of requiring one candidate to be no worse for every argument, selecting a candidate that did not dominate. | Patched with localized pack compatibility and per-argument dominance across calls, operators, direct constructors, and base constructors. No general raw cast, decay, or pointer-depth conversion is implied. |
| Duplicate retained-loan notes | Distinct generated loans sharing one source span could produce identical related origin notes even when the primary conflict was correct. | Patched by de-duplicating identical message/source spans while preserving the primary error and one useful origin note. |
| Tree-sitter shipped-source drift | The external grammar omitted mutable free-function return spelling that the compiler accepts and the shipped prelude uses. Isolated grammar corpus tests therefore did not prove that all distributed GTI source parsed. | Patched with grammar coverage and a parse-all-shipped-`.gti` gate over standard-library source and examples. |
| Semantic signature presentation | `SignaturePrinter` could lose interface kind and a parameter-pack ellipsis even though semantic identity and the underlying declaration were correct. Hover or completion text could therefore describe a different surface from the compiler. | Patched with focused interface and variadic hover/completion signature regressions. |
| Rainbow-delimiter query drift | The rainbow-delimiter query could drift from the current Tree-sitter node shape while ordinary punctuation highlighting still worked, hiding the failure behind a partially correct editor result. | Patched for compiler constraints, concepts, structured bindings, and discarded expressions, with position-sensitive Node and packaged-Neovim regressions. |
| Sanitizer gate omission | `raw_pointer_pipeline` existed in normal CTest coverage but was absent from the sanitizer test selection. | Patched so raw-memory lowering and verification run in ordinary and sanitizer configurations. |

The common cause is not one faulty subsystem. It is incomplete preservation of
an already-resolved fact while a feature crosses symbolic analysis, concrete
instantiation, body-local IR, diagnostics, and a transitional backend.

## Reusable cross-phase feature checklist

Use this checklist for a new feature and again when combining existing
features. A feature is not thorough merely because each item worked in
isolation.

| Boundary | Required questions and evidence |
| --- | --- |
| Syntax and source provenance | Does the parser retain the complete source shape and exact spans? Do recovery, formatter, Tree-sitter, and editor syntax agree when syntax changed? Parse every shipped standard-library and example source, not only focused grammar fixtures. |
| Symbolic semantics | Are types, value category, mutability, ownership, control-flow state, overload identity, dispatch, and diagnostics decided once in semantics? Are invalid forms rejected before the backend? |
| Concrete reanalysis | Are generic class, function, constructor, destructor, callable, and pack bodies rechecked after substitution? Are completed types revalidated for `void`, references, raw-pointer limits, copy/move traits, and access? |
| Selected identities | Does every call, operator, constructor, base initializer, virtual root, intrinsic, and C symbol retain its selected ID plus the exact owner and substituted signature? Test inherited and nested generic owners, not only direct receivers. |
| Typed HIR | Is there exactly one correct concrete instance for each requested identity? Do body bindings and values contain substituted types, ordered operands, unsafe classification, borrow origin, and source provenance without reconstructing semantics? |
| MIR | Are CFG edges, values, places, projections, calls, moves, loans, drops, cleanup, raw-memory access, and effects explicit enough for the operation? Are use indexes and reachability rebuilt and verified? |
| Cross-IR verification | Do target IDs exist and agree with receiver, owner, arguments, result, dispatch, linkage, and constructor metadata? Can any unresolved type parameter leak into a concrete body? Do definitions dominate every use? |
| Backend | Does emission consume frontend facts rather than repeat selection? Inspect declarators and emitted operations, compile the artifact, and execute edge cases at representative optimization levels. Do not accept a native compiler error as a GTI diagnostic. |
| Tooling | Does the LSP consume the same frontend snapshot for diagnostics and semantic queries? Test incomplete source separately from valid-source compiler behavior, and verify lexical fallbacks do not claim semantic precision. |
| Regression matrix | Add focused positive, negative, concrete-generic, IR-structural, emitted-code, and execution coverage. Combine the feature with inheritance, moves, references/loans, raw pointers, control-flow joins, cleanup, and target selection where applicable. |
| Contracts | Update the implemented grammar/current contract and backend-independent specification where behavior changed. Record temporary implementation limits explicitly rather than allowing backend behavior to become accidental semantics. |

## Deferred architecture, not review-sized refactors

The review deliberately did not fold the following work into local bug fixes:

- **Flow authority:** semantic analysis still owns move and loan decisions with
  AST-oriented flow snapshots. MIR should eventually own reusable edge-based
  move/initialization/loan dataflow, with semantics producing the facts needed
  to seed it. Do not create a second ad hoc semantic flow engine meanwhile.
- **MIR callable verification:** `MirFunctionInstance` does not yet retain the
  concrete callable owner and full parameter signature. Program verification
  therefore cannot soundly cross-check every call receiver, argument, result,
  constructor target, lambda target, or inherited owner. MIR also lacks a
  dominance/use-before-definition verification pass.
- **Executable lifetime detail:** general temporaries, partial construction,
  complete active-drop transitions, and actual base/field construction are not
  yet fully executable MIR. Existing constructor and lifecycle metadata must be
  preserved until that migration is designed.
- **Evaluation order:** GTI still has an explicit specification gap for the
  order of ordinary operands, call arguments, initialization subexpressions,
  temporaries, and cleanup. C++ helper-call argument order must not become the
  language rule accidentally.
- **LSP recovery:** valid documents share compiler semantics, but incomplete
  documents still need bounded lexical and recovered-AST fallbacks. Those
  fallbacks cannot promise complete rename, references, or type-aware results.
- **Compiler implementation boundaries:** substantial compiler logic remains
  in large headers. Its staged migration is already owned by
  [`compiler-library-migration-proposal.md`](compiler-library-migration-proposal.md)
  and should not be mixed into correctness patches.

The safe near-term policy is to patch reproducible cross-phase loss narrowly,
add the missing structural regression, and record the larger authority or
representation gap here and in its owning proposal.
