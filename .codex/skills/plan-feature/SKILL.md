---
name: plan-feature
description: Plan or design a new GTI language, standard-library, compiler, or tooling feature as a draft RFC and proposal PR in lang-gti-official/rfcs. Use when asked to plan, design, propose, or revise a feature proposal; honor discussion-only, local-only, and no-publication requests. Do not create new RFCs for ordinary bug-fix plans, implementation checklists for accepted designs, or read-only reviews.
---

# Plan A GTI Feature

Turn a new feature-planning request into a durable draft in
[lang-gti-official/rfcs](https://github.com/lang-gti-official/rfcs). The owner's
standing instruction authorizes saving the proposal and publishing a draft
proposal PR. It does not authorize merging it, accepting the design, or
implementing the feature. Follow the
[planning contract](../../../docs/architecture/agent-workflow.md#feature-proposals-and-planning).

## Route the request

- Plan/design/propose a new capability: create or update its RFC and draft PR.
- Revise an existing proposal: update that RFC, preserving its ID and history.
  Use its existing proposal branch/PR when appropriate; otherwise open an update
  PR. Preserve other contributors' edits and coordinate before changing their
  active work. A material revision to an accepted design needs a new explicit
  decision; do not carry old acceptance over to changed semantics.
- Plan implementation of an accepted design: use the implementation repository's
  existing plan or task record, linked to the RFC. Do not invent a second RFC.
- Plan a bug fix or review a design: use ordinary planning/review unless a new
  capability or policy design is actually requested.
- Discussion-only, local-only, no-publication, or an explicitly requested
  destination overrides the publication default. Do not turn a casual question
  about a feature into a proposal PR.

## Establish the design

1. Read GTI's `docs/index.md`; search the RFC catalog and open proposal PRs,
   relevant GTI issues, specifications, ADRs, and existing plans. Reuse an
   existing proposal when it covers the requested design. Verify current
   implementation claims against source and tests; link evidence and historical
   decisions instead of copying or erasing them.
2. Locate a separate checkout of `lang-gti-official/rfcs`, or clone it if needed.
   Inspect its status, preserve unrelated work, and read its current `AGENTS.md`,
   `README.md`, `CONTRIBUTING.md`, and `templates/feature.md`. Use a dedicated
   `codex/proposal-<slug>` branch from its current main for a new proposal; do not
   register an RFC with the GTI compiler's lifecycle helper.
3. Use the RFC repository's generator from that checkout:

   ```sh
   python3 scripts/rfcs.py new <slug> --title '<feature title>' --author '<author>' --area <area>
   ```

   Select its actual area (`language`, `stdlib`, `compiler`, `tooling`, or
   `process`). Identify the author from known contributor information; never
   invent authors or approvals. The generator assigns an RFC number, independent
   of the PR number. Numbers remain provisional until merge; follow its collision
   policy and never renumber a merged RFC.
4. Develop the generated document using every template section. State the user
   outcome, current behavior, proposed rules, examples, alternatives, compatibility,
   prerequisites, safety invariants, and unresolved decisions. Explain performance
   costs and the evidence needed for acceptance without inventing measurements.
   Give coherent implementation slices and validation criteria, not a claim that
   the feature already works. Keep status `draft` for a new proposal.
5. Apply relevant GTI design guidance, including `$compiler-architecture-review`
   and the contract sections of language/build/LSP skills as appropriate, without
   starting their implementation lifecycle. Preserve phase authority, general
   ownership/safety rules, and LLVM-first development; C++ feature parity is not
   a new-feature acceptance gate. Link existing plans for implementation tracking.

## Validate and publish the draft

1. In the RFC checkout, regenerate the catalog and run its document checks:

   ```sh
   python3 scripts/rfcs.py index
   python3 scripts/rfcs.py check
   git diff --check
   ```

   Review the design for internal consistency and honest status. Run the RFC
   tooling tests only if that tooling changes; do not build the compiler or run
   compiler CI merely to save a design.
2. Commit and push the proposal branch, then open a draft PR in
   `lang-gti-official/rfcs`. Set the document's `Proposal` field to its PR URL,
   update its `Updated` date, regenerate/check the index, and publish the backlink
   update. Attach the PR to the chat when the client supports it.
3. Stop at the draft proposal unless the user requests a further stage. A green
   document check or merged document is not design acceptance, implementation,
   or release evidence. Acceptance and implementation status follow the RFC
   repository's explicit decision and evidence rules.
4. Report the RFC file link, proposal PR link, draft status, and the substantive
   open questions or tradeoffs. When publication was restricted, report where
   the requested local/conversation draft was left instead.
