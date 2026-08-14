# Compiler Architecture Overview

Status: Current implementation.

GTI separates source-language decisions from backend representation. The
shared pipeline is:

```text
SourceLoader -> per-unit Lexer/Parser -> Program
             -> SemanticVisitor -> SemanticModel
             -> HirLowerer      -> HirProgram
             -> MirLowerer      -> MirProgram
             -> OptimizationPipeline
             -> Backend (CppBackend today)
             -> BackendArtifact
             -> gti_driver/native compiler
```

`Frontend::analyze` is declared in `include/gti/frontend.h` and implemented in
`src/compiler/frontend.cpp`; it owns the ordering through MIR. Both the CLI and
LSP enter through that API. `FrontendResult` owns the AST,
semantic model, HIR, MIR, source graph, source manager, diagnostics, and phase
validity together, which keeps AST-address semantic side tables alive.

## Authority By Layer

| Layer | Owns | Must not decide |
| --- | --- | --- |
| Source loader/lexer/parser/AST | source units, tokens, grammar, syntax, recovery, written source structure | types, overloads, ownership validity |
| Semantic analysis | names, scopes, types, conversions, calls, lifecycle, ownership and source-place validity, access, dispatch, target selection and layout-query constants, tooling symbols | backend representation |
| Typed HIR | concrete generic/class/callable instances, executable typed value graphs, full-expression/drop obligations, and preserved semantic place/ownership events | a second type system or a different place relation |
| MIR | body-local CFG, mapped places, values, resolved calls, moves, loans, typed lifecycle cleanup, ownership/lifecycle fixed-point verification, and structural verification | language rules missing from semantics/HIR |
| Optimization | proven transformations over authoritative IR | host-C++ behavior as a proof |
| Backend | representation and artifact generation | name/type/overload/lifetime inference |
| Driver | requests, resources, manifests, artifacts, native tools, processes | GTI parsing or semantics |
| CLI/LSP | presentation or protocol conversion | independent language analysis |

## Current Transition Points

- HIR is the concrete instance authority; MIR is a validated structural
  foundation with normal-exit temporary/drop authority, but it does not yet own
  every D-EXEC ordered materialization schedule, program-initialization step,
  partial-constructor rollback, layout, ABI, or failure cleanup rule.
- M-OWN-01 selected one value-owned place/relation and ownership-state
  authority contract. M-OWN-02 implements its directly owned fixed-array
  slice: semantics records shared constant-index keys/events, HIR carries a
  body-qualified domain, and MIR verifies reachable available/moved/restored
  state. Dynamic indexes and raw/opaque provenance remain conservative;
  M-LIFE-01 separately supplies typed normal-exit drop obligations.
- Optimization still has two paths: HIR constant replacements affect C++
  emission, while the MIR path currently verifies and returns an unchanged
  owned snapshot.
- `BackendInput` carries AST, semantics, HIR, optimized MIR, and HIR
  replacements. `CppBackend` currently emits by traversing the AST with
  semantic/HIR side data and does not consume MIR bodies.
- Substantial compiler algorithms compile behind public declarations.
  `gti_compiler` owns source loading through optimization and compiler support;
  the separate `gti_cpp_backend` archive owns C++ representation and emission.
  Headers retain data models, templates, abstract contracts, and small value
  operations.

Those are implemented limitations, not permission for later stages to invent
semantics. Their future work is tracked under [`docs/plans/`](../plans/).

## Ownership And Lifetime

IDs and raw AST pointers are snapshot-local. `FrontendResult` must outlive every
consumer of `SemanticModel` records. HIR IDs are stable only within one
`HirProgram`; MIR block/value/place/loan IDs are body-local. Indexes or caches
must store durable summaries rather than pointers into a discarded snapshot.

## Source Map

- `include/gti/frontend.h`, `src/compiler/frontend.cpp`: phase orchestration
  contract and compiled implementation.
- `include/gti/{token,lexer,parser,ast}.h`,
  `src/compiler/{token,lexer,parser,ast_printer}.cpp`: frontend syntax records,
  keyword storage, scanning/parsing, and AST presentation.
- `include/gti/{diagnostic,source_graph,source_loader}.h`,
  `src/compiler/{diagnostic,source_graph,source_loader}.cpp`: source snapshots,
  include graphs, and diagnostic/source-location support.
- `include/gti/semantic_analyzer.h`,
  `src/compiler/{semantic_analyzer,semantic_model,semantic_type_printer}.cpp`:
  semantic records/query contract plus compiled analysis, snapshot operations,
  place/type policy, and type presentation.
- `include/gti/{hir,mir}.h`,
  `src/compiler/{hir_model,hir_lowering,mir_lowering,mir}.cpp`: public IR records
  plus compiled query operations, HIR/MIR lowering, and MIR verification.
- `include/gti/optimizer.h`, `src/compiler/optimizer.cpp`: current optimizer
  entry points.
- `include/gti/{constant_evaluator,formatter,language_queries}.h`,
  `src/compiler/{constant_evaluator,formatter,language_queries}.cpp`: shared
  constant evaluation and editor-facing formatting/query implementations.
- `include/gti/{backend,cpp_backend,cpp_emitter}.h`,
  `src/compiler/{cpp_backend,cpp_emitter}.cpp`: backend contracts and compiled
  C++ representation.
- `include/gti/driver/`, `src/driver/`: build and native orchestration.

Read the focused architecture document before modifying a layer. Use
[`docs/language/`](../language/index.md) for language meaning and
[`docs/plans/`](../plans/roadmap-to-1.0.md) for unfinished systems.
