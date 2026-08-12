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

`Frontend::analyze` in `include/gti/frontend.h` owns the ordering through MIR.
Both the CLI and LSP enter through that API. `FrontendResult` owns the AST,
semantic model, HIR, MIR, source graph, source manager, diagnostics, and phase
validity together, which keeps AST-address semantic side tables alive.

## Authority By Layer

| Layer | Owns | Must not decide |
| --- | --- | --- |
| Source loader/lexer/parser/AST | source units, tokens, grammar, syntax, recovery, written source structure | types, overloads, ownership validity |
| Semantic analysis | names, scopes, types, conversions, calls, lifecycle, ownership and source-place validity, access, dispatch, target selection, tooling symbols | backend representation |
| Typed HIR | concrete generic/class/callable instances, executable typed value graphs, and preserved semantic place/ownership events | a second type system or a different place relation |
| MIR | body-local CFG, mapped places, values, resolved calls, moves, loans, cleanup, ownership-state fixed-point verification, and structural verification | language rules missing from semantics/HIR |
| Optimization | proven transformations over authoritative IR | host-C++ behavior as a proof |
| Backend | representation and artifact generation | name/type/overload/lifetime inference |
| Driver | requests, resources, manifests, artifacts, native tools, processes | GTI parsing or semantics |
| CLI/LSP | presentation or protocol conversion | independent language analysis |

## Current Transition Points

- HIR is the concrete instance authority; MIR is a validated structural
  foundation but does not yet own every D-EXEC full-expression schedule,
  temporary, program-initialization step, layout, ABI, or active-drop rule.
- M-OWN-01 has selected one value-owned place/relation and ownership-state
  authority contract. The current semantic pointer/SymbolId place forms and
  MIR-private canonical-place helpers remain the implemented bounded slice;
  M-OWN-02 must replace or delegate them before indexed partial movement is a
  current feature.
- Optimization still has two paths: HIR constant replacements affect C++
  emission, while the MIR path currently verifies and returns an unchanged
  owned snapshot.
- `BackendInput` carries AST, semantics, HIR, optimized MIR, and HIR
  replacements. `CppBackend` currently emits by traversing the AST with
  semantic/HIR side data and does not consume MIR bodies.
- Much compiler implementation remains in headers. `gti_compiler` currently
  compiles the lexer, formatter configuration, MIR repair/verification,
  MIR printer, effect classification, the optimizer facade, checked integer
  arithmetic, HIR concrete-instance de-duplication, target-triple parsing,
  and tool-process support (crash handling and compile-time telemetry).

Those are implemented limitations, not permission for later stages to invent
semantics. Their future work is tracked under [`docs/plans/`](../plans/).

## Ownership And Lifetime

IDs and raw AST pointers are snapshot-local. `FrontendResult` must outlive every
consumer of `SemanticModel` records. HIR IDs are stable only within one
`HirProgram`; MIR block/value/place/loan IDs are body-local. Indexes or caches
must store durable summaries rather than pointers into a discarded snapshot.

## Source Map

- `include/gti/frontend.h`: phase orchestration and owned result.
- `include/gti/{token,lexer,parser,ast}.h`, `src/compiler/lexer.cpp`: frontend
  syntax layers.
- `include/gti/semantic_analyzer.h`: semantic model and analysis.
- `include/gti/hir.h`, `include/gti/mir.h`, `src/compiler/mir.cpp`: IRs and
  lowering/verification.
- `include/gti/optimizer.h`, `src/compiler/optimizer.cpp`: current optimizer
  entry points.
- `include/gti/{backend,cpp_backend,cpp_emitter}.h`: backend contract and C++
  representation.
- `include/gti/driver/`, `src/driver/`: build and native orchestration.

Read the focused architecture document before modifying a layer. Use
[`docs/language/`](../language/index.md) for language meaning and
[`docs/plans/`](../plans/roadmap-to-1.0.md) for unfinished systems.
