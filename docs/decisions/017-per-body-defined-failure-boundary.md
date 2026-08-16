# ADR 017: Per-Body Defined-Failure Boundary

Status: Accepted

## Context

Roughly 480 analysis-Ready function bodies cannot move to verified-MIR
emission because they are failure-capable. The hosted
`scalar-failure-callgraph-v1` family already emits failure-capable bodies
from MIR with a transformed private ABI — `bool f(args…, T
*__gti_mir_out_result, ::gti_failure_record_v1 *__gti_mir_failure_record)`
— but only for one closed, acyclic call graph rooted at the hosted entry,
because every caller of a transformed body must use the transformed calling
convention. That closure requirement is the single blocker for per-body
migration of checked bodies.

The two routes also disagree observably on the failure path today. A
compatibility-emitted checked operation calls an inline helper
(`::gti_internal::backend::add` and friends) that prints a fixed `GTI
runtime error: …` line to stderr and calls `std::abort()`. The MIR route
writes an exact `::gti_failure_record_v1` — artifact identity, site, code,
detail, all owned by MIR failure metadata — and the hosted boundary
translates it through `::gti_rt_failure_terminate_v1` into the versioned
structured report and exit status 70 defined by the failure ABI
(`Q-FAIL-01`). The legacy abort is a transitional artifact of the
compatibility backend, not a language contract.

## Decision

1. **Failure-capable bodies migrate per body, not per graph.** A body the
   general emitter's analysis and failure vocabulary admit emits with the
   transformed private ABI under a derived name. Alongside it, the backend
   emits a **boundary wrapper** carrying the body's original name and
   signature: it calls the transformed body, returns the published result on
   success, and on failure hands the record to
   `::gti_rt_failure_terminate_v1` with the program's artifact descriptor.
   Callers — compatibility-emitted or otherwise — keep calling the original
   name unchanged, so no call-graph closure is required.

2. **The defined failure contract wins at every migrated boundary.** When a
   migrated body fails, the process reports through the versioned structured
   failure ABI and exits with status 70, replacing the legacy
   `stderr`+`abort()` behavior for that body. This is a deliberate,
   per-body behavioral migration toward the language's defined contract;
   the legacy abort remains only on bodies still emitted by the
   compatibility backend.

3. **The differential oracle stays strict.** A source whose execution fails
   through a migrated body will show the expected divergence — legacy abort
   on the pure-compatibility route versus structured report and exit 70 on
   the MIR route. That divergence is the migration working as decided, and
   an oracle report of it must be reconciled against this ADR rather than
   treated as a miscompile. Success-path behavior must remain identical and
   is not excused by this ADR.

4. **All record data stays MIR-owned.** Record writes spell exclusively from
   the MIR body (per-instruction failure sites and origins) and the MIR
   program's failure metadata (artifact identity bytes); the general emitter
   never re-derives failure facts from AST, HIR, or semantics. The failure
   status and checked-helper family
   (`mir_failure_status_v1`, `mir_checked_*_v1`) remains the single shipped
   helper surface, with one spelling authority shared between the hosted
   family and the general route.

5. **The hosted family keeps precedence.** Bodies inside the hosted closed
   graph keep the family's direct transformed calls (no wrapper
   indirection); the per-body route takes only bodies the family does not
   select. The family dissolves into the general route in a later phase, at
   which point transformed-to-transformed direct calls become a per-body
   optimization of the wrapper boundary.

## Consequences

- The first bounded slice admits leaf failure-capable bodies — checked
  scalar computation, no calls to failure-capable targets — and each later
  slice widens the failure vocabulary (transformed callee calls, cleanup
  drains with real work, constructors) without revisiting this decision.
- A program that never fails observes no behavioral change; byte-identical
  stdout across the corpus remains the success-path gate.
- Wrapper indirection costs one call per boundary crossing; graphs that
  stay fully transformed avoid it, and the later family dissolution can
  restore direct transformed calls per body.
- The failure helpers and the artifact descriptor must be emitted whenever
  any body uses the transformed ABI, not only when the hosted entry is
  selected.
