# 010: Evaluation Is Left To Right And Cleanup Is Obligation-Ordered

Status: Accepted

## Context

GTI promises deterministic cleanup and backend-independent behavior, but the
transitional C++ emitter still writes several calls and checked-helper operands
as native C++ argument lists. Supported C++ modes do not supply one portable
argument order. HIR preserves source operand vectors and MIR happens to lower
many operands left to right, but neither representation currently owns every
temporary, full-expression boundary, target-place computation, or program-wide
initializer.

This was already observable in ownership checking. A transient borrow and an
overlapping mutation in one call are rejected in both written orders because
accepting the mutation-first form would let the selected C++ backend reverse
them. IIFE or emitter-local hoisting sketches cannot solve that problem safely:
they would also have to reproduce result materialization, parameter lifetime,
partial construction, transient-loan endings, return transfer, and defined-
failure cleanup.

The language therefore needs one rule that composes with the existing place,
ownership, failure, and future concurrency contracts before a backend lowering
is selected.

## Decision

GTI evaluates runtime subexpressions strictly left to right. A callable or
member receiver is evaluated first, arguments initialize parameters left to
right, and invocation follows. Unary, binary, member, index, conversion,
initializer, capture, and concrete pack constituents use the same order.
Logical and conditional expressions retain short-circuiting and evaluate only
the selected operands.

Assignment is target-first. Its place, including receiver and projections, is
formed once before the right operand. Compound assignment additionally
stabilizes the prior target value before the right operand, then performs one
checked operation/conversion and one write.

A full-expression owns an ordered stack of transient obligations. Beginning a
temporary lifetime or transient loan registers an obligation. Transfer to a
binding, parameter, result, subobject, or hidden owner reparents the top-level
obligation. The boundary performs remaining obligations in reverse registration
order. Lexical scopes and partially initialized aggregates apply the same
reverse-successful-initialization principle.

Top-level value results initialize their destination directly. A backend may
use result slots or native elision, but it cannot introduce or remove a
source-observable copy, move, lifetime, or cleanup event. The complete
normative operand, materialization, full-expression, construction, return, and
trace rules are in
[Execution Section 4.2](../language/execution.md#42-evaluation-order).

Program-wide initialization is also language-selected. Implicit preludes are
traversed first in configured order, then the entry graph; direct dependencies
use lexical include order and initialize before requesters; globals and static
fields within a unit use source position. A shared unit initializes once.
Program storage cannot be observed before its step completes. The plan is
inside the hosted containment boundary and is independent of source-unit IDs,
absolute paths, parse worklists, link order, and native pre-`main` behavior.

For the owned-argument entry, hosted validation and checked count conversion
precede program-wide initialization. Strings/vector are then constructed in
native order, after their standard-library units are initialized, and the count
and vector transfer left to right into source `main`. This fixes first-failure
and cleanup order for the compiler-generated operation as well as source calls.

## Phase Ownership

The accepted contract does not add a second evaluation representation. The
implementation migration has these directional owners:

| Fact | Producer and owner | Lifetime and consumers |
| --- | --- | --- |
| Written child order and source boundary syntax | Parser/AST | Snapshot-owned syntax consumed by semantics. |
| Active target branches, resolved operations, ordered borrow validity, full-expression endpoints, and the source-graph-derived program-initialization plan | Semantics | Immutable `SemanticModel` facts for one frontend snapshot; HIR consumes them and must not rediscover them. |
| Concrete receiver/argument/operand roles, destination-materialization intent, and full-expression identity | HIR | Concrete-program facts consumed by MIR; generic reanalysis produces the same roles after substitution. |
| Temporary identity, lifetime start, active-drop/loan obligations, transfer/reparenting, and reverse cleanup | M-LIFE-01 MIR authority | Body-local facts verified on every normal edge; M-FAIL-01 later adds equivalent failure edges. |
| Instruction/CFG order, one-time target-place formation, short-circuit selection, invocation after parameter setup, ordered hosted setup, and one merged program-initialization body | M-EXEC-01 MIR authority | Body-local executable schedule consumed by optimization and a MIR backend. |
| Native sequencing and representation | Matching M-BACK closed-body migration | May choose statements, result slots, or helper calls only when they realize verified MIR exactly. |

`FullExpressionId`, temporary IDs, and program-initialization step IDs are
snapshot/body-local. A structural transformation that changes one of their
regions must rebuild and reverify the affected schedule, obligations, use
indexes, and cleanup edges; it cannot claim those facts were preserved. A
value-only rewrite may retain them only when the verifier proves no lifetime,
effect, failure, or ordering change.

The current `SourceGraph::compilationOrder()` remains a parse/assembly
convenience. The executable program-initialization plan must be built from
source identities and lexical dependency spans and retained explicitly; a
backend cannot treat combined-AST order as the rule.

## Alternatives

- Leave most operands unspecified as C++ does: rejected because borrow
  validity, cleanup traces, failure sites, and happens-before would vary by
  backend or optimization.
- Evaluate calls right to left: deterministic but rejected because it is less
  consistent with source reading, array/capture initialization, and the
  existing HIR/MIR traversal foundation.
- Specify only calls and let each construct choose independently: rejected
  because nested operators, assignments, constructors, and conditions would
  still need an ordering matrix and overlapping lifetime rules.
- Destroy a temporary at its last syntactic use: rejected because it makes
  lifetime depend on analysis strength, breaks a simple transient-loan
  boundary, and complicates observable cleanup.
- Treat every comma operand or argument as a separate full-expression:
  rejected because it would end loans and destroy values before the containing
  operation that consumes them.
- Repair the current emitter with IIFEs or local statement hoisting: rejected
  as semantic authority because those rewrites do not by themselves model
  return slots, partial construction, active drops, or failure cleanup.
- Reuse native static initialization: rejected because it can run before the
  GTI containment boundary and depends on emitted translation-unit/link order.

## Consequences

Source order now determines observable operand effects, temporary destruction,
transient-loan overlap, initializer failure cleanup, and within-thread
sequenced-before. An earlier mutation may eventually be followed by a later
borrow in the same call, while an earlier borrow still conflicts with a later
overlapping mutation because it remains active through the full-expression.
That relaxation occurs only after the matching ordered MIR and production
backend family land.

M-LIFE-01 implements the obligation model; M-EXEC-01 now linearizes a complete
expression family and owns structural ordered-MIR evidence;
M-BACK-01/02 own production runtime traces and removal of compatibility paths.
M-FAIL-01 consumes the same obligations rather than inventing a separate
failure cleanup order.

No source syntax or current executable behavior changes with this design-only
decision. Until the named migrations land, the compatibility emitter and
native-static initialization remain documented nonconformities rather than a
second definition of GTI.
