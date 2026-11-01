---
name: llvm-campaign
description: Implement or coordinate an assigned milestone of GTI's active O-LLVM production-backend campaign. Use for campaign workers and integration, not unrelated compiler work, ordinary LLVM questions, or a new backend campaign after O-LLVM-16 closes.
---

# GTI LLVM Campaign

This temporary skill coordinates milestone-owned parallel implementation of the
[LLVM production campaign](../../../docs/plans/implementation-sequence.md#llvm-production-backend-campaign).
It does not replace the language specification, implemented backend architecture,
or the reviewed LLVM-C01 through LLVM-C09 contracts. Retire it when O-LLVM-16
closes; retain durable engineering rules in their canonical documents.

## Establish The Assignment

Read `AGENTS.md`, `docs/index.md`, the assigned milestone, the
[shared contracts](../../../docs/plans/implementation-sequence.md#shared-cross-milestone-contracts),
and the relevant [native validation gates](../../../docs/architecture/verification.md#native-llvm-admission-gates).
Use `$agent-workflow`, `$compiler-architecture-review` and `$verify-change`;
load language/build/diagnostic skills only when those boundaries change.

Before edits, return a short assignment card to the coordinator:

- milestone ID and concrete supported program or necessary infrastructure outcome;
- base SHA, task branch/worktree, existing overlapping PRs;
- input facts, owning layer, contract IDs and unresolved prerequisites;
- exclusive files, shared registration patches needed, explicit exclusions;
- admission/rejection shapes, risk tier, focused tests and platform/cost gates;
- milestone exit gates, useful checkpoints and actual prerequisite blockers.

The default assignment is one complete O-LLVM milestone. Use smaller checkpoints
to review and integrate progress, not as artificial stopping points requiring a
new prompt. Continue routine design, implementation and focused verification
within the agreed ownership. A missing prerequisite blocks its dependent shapes,
not independent work elsewhere in the milestone. Never mark the whole milestone
complete from a prototype or one admitted profile.

The coordinator grants write scopes, not authority to redesign shared contracts.
Inspect current source and tests before accepting stale milestone assumptions.
Report cross-owner prerequisites and request specific shared changes rather than
silently widening the task or duplicating another owner's work.

## Parallel Ownership

Each worker uses its own branch, worktree and build tree from current canonical
main. No workers compile different sources into one mutable build tree. Publish
coherent checkpoints and a draft PR; retain live run IDs and exact evidence.

Follow the current [wave assignments](../../../docs/plans/implementation-sequence.md#llvm-campaign-waves-and-validation).
Workers edit only their agreed files. The coordinator owns shared registrations
in `body.cpp`, admission/coverage inventories, root/test CMake lists, common plan
status and changelog-fragment coordination unless a specific bounded ownership
transfer is recorded. Only the release train edits VERSION and CHANGELOG.md.
Propose those patches with exact insertion points; do not independently rewrite
shared dispatch/planning files. The coordinator may transfer a specific shared
section or registration to its milestone owner, recording the boundary so other
workers do not edit it. Integrate these changes on the component branch before
its final exact-source checks. Meanwhile the
worker can exercise its isolated harness directly; an unregistered or unwired
test/helper is not a completed production slice.

Do not duplicate an active external PR or take over its files. A dependency
branch must be explicitly named; never merge unrelated open PRs into a worker.
Request a scope transfer with the affected files and contract first. Continue
independent work instead of making every worker wait on another worker.

The current wave uses `gpt-6.1-sol` with `ultra` reasoning, as requested by the
owner. No nested delegation or next-wave expansion without coordinator approval.
If that model/effort is unavailable, report it instead of silently substituting.

## Implementation Boundaries

- Follow [the backend development policy](../../../docs/decisions/031-native-llvm-backend-foundation.md#backend-development-policy-2026-10-25):
  LLVM is primary; C++ is deprecated as primary and bug-fix-only through v1.0.
  Do not extend C++ merely to make an LLVM feature comparable. Keep supported
  comparator regressions and independent native evidence for documented gaps;
  neither post-1.0 C++ catch-up nor changing today's CLI default is automatic.
- Consume verified lowered identities, MIR publication/cleanup/effects and exact
  target policy. Add genuinely missing facts to their producer and verifier;
  never rediscover source meaning in the backend or add public-type name magic.
- Keep storage, body values and ABI transport distinct. Optimize known cases
  without mandatory heap allocation, flags, copying or erased dispatch.
- C04 approves one ABI authority, but physical failure transport remains open.
  Do not entrench status adapters or implement a production unwinder by assumption.
  A comparison prototype is not production coverage or a completed C04 decision.
- Unknown required shapes reject before artifact publication. Never widen
  admission just because a type layout exists, fabricate a body, or use a hidden
  C++ fallback. Independent source semantics and cleanup are the oracle; a known
  C++ comparator defect must be explicit, not copied into LLVM or called a pass.
- Record implemented behavior only after execution/rejection evidence. Keep
  proposed designs, missing shapes and the acceptance still owed in the plan.

## Validation Without Duplicate Full Suites

Use the [wave validation ladder](../../../docs/plans/implementation-sequence.md#llvm-campaign-waves-and-validation)
and [risk-based policy](../../../docs/architecture/agent-workflow.md#risk-based-batch-validation).
The named gates are a lower bound, not permission to skip an affected invariant.

1. During edits, run the reproducer/new fixture and exact owning checks. Build
   only required targets with an optimized GTI compiler. Test supported behavior,
   rejection and failure/cleanup where relevant, not merely generated IR text.
2. Before handoff, validate the published source, affected subsystem/consumers,
   dependency boundary, smoke and required platform/performance cases. Reuse
   existing registered matrices rather than inventing a weaker fast substitute.
   Report every skip, missing prerequisite and pending gate separately.
3. The coordinator selects independent completion or combined milestone
   integration from the changed invariant. Workers do not start full CTest,
   all-example corpora, full audits or duplicate sanitizer campaigns on their
   own. Combined batches retain their mandatory full/platform checks and
   ten-minute ready-collection limit; this skill does not weaken that protocol.

Use `--parallel 2`/`-j2` for builds and `--parallel 1` for CTest. Full builds and
broad tests use `build_window.py`; T2/T3 admission uses its required reservation.
On this campaign's Mac mini, run macOS validation locally; do not SSH back to
the `mini` runner. Use `remote_runner.py` on an idle appropriate Linux host;
coordinate host allocation rather than creating ad hoc remote checkouts.
Focused independent commands can run concurrently. Never increase window
capacity, steal a reservation, or restart a quiet live run.

For generated execution, preserve required optimization levels and cleanup/
source-attribution assertions. Run representative optimized cost probes with
noise controlled; do not make timing claims while other work saturates the
measurement host. Inspect native C oracles on each targeted ABI. A macOS-only
result is not Linux ABI evidence. CI does not substitute for the milestone's
required targeted platform/performance evidence; the normal full integration
CI gate still applies before merge.

## Checkpoints And Completion

At a checkpoint publish the exact branch/PR and return: changed paths,
admitted/pending shapes, shared patches, check commands/results/logs and live
handles, platform/cost evidence, risks and next prerequisite. Identify whether
the result is exploratory, ready for integration, or integrated. Do not merge
or mark production support complete from component evidence alone. Integration
and main synchronization belong to the designated coordinator for this wave.
Continue the assigned milestone after intermediate checkpoints unless explicitly
paused or all remaining work needs another owner's prerequisite. Stop at its
review/integration boundary with outstanding gates explicit; do not claim
completion or start a different milestone merely to fill a wait.
