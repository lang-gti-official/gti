---
name: finish-release
description: Apply GTI version and release policy to validated work delivered through the agent-workflow PR lifecycle. Use when an implementation is ready to deliver or the user asks to commit, publish or release. Required PR checks precede integration; asynchronous release packaging does not block completion.
---

# Finish A GTI Release

Deliver validated work through [agent-workflow](../agent-workflow/SKILL.md).
The normal implementation outcome is a merged PR and synchronized local main;
explicit PR-only/local-only instructions override integration. Apply release
policy below without bypassing that lifecycle.

## Choose The Delivery Level

- Do not release incomplete work or publish a read-only review or work the user
  asked to leave local. Unfinished implementation checkpoints may be published
  to their task branch and draft PR under agent-workflow.
- Commit completed in-scope repository changes after local validation.
- Treat a change as release-bearing when repository version policy requires a
  `VERSION` advance, when it changes shipped behavior or packaging, or when the
  user explicitly requests a release.
- When `VERSION` stays unchanged but a commit touches a file that
  `scripts/check_release_version.py` classifies as shipped, end that commit's
  message with a `Release-Impact: internal` trailer and one nonempty
  `Release-Reason:` trailer explaining why no user-visible behavior changes.
  Documentation and agent-automation commits need neither, and neither does
  an automatic merge: the policy judges a merge commit only by what differs
  from the automatic merge of its parents, so a reconciliation merge
  that resolved a conflict in a shipped file needs the trailers in its own
  message (`git commit --amend`) before the checkpoint. See
  [the release policy](../../../docs/architecture/build-and-driver.md).

## Commit And Reconcile

1. Run `git status --short`, inspect the complete diff, and identify unrelated
   user work before staging anything.
2. Fetch the remote before publication. If the destination branch advanced,
   reconcile it without discarding local or remote work. Do not force-push or
   rewrite shared history.
   The destination must be GTI's canonical Gitea repository; remove or bypass
   any legacy GitHub push URL before publication.
3. Run the proportionate local build, tests, packaging checks, formatting, and
   release-version checks (`python3 scripts/check_release_version.py
   origin/main HEAD`). Gitea CI is confirmation, not a substitute for local
   gates.
4. Stage only the intended files, review `git diff --cached`, and create a
   focused commit.

## Initiate The Release

Use the repository's Gitea release workflow as the tag and release authority.
For the current workflow, merging a release-bearing PR with an advanced
`VERSION` into canonical Gitea `main` dispatches packaging and publication of the
matching `v<VERSION>` tag and Gitea release. Do not also push a competing tag.

Manual workflow dispatch may rebuild the release for the selected commit, but
the workflow still owns tag creation. Never tag a version mismatch or an
unvalidated tree by hand.

## Finish After Dispatch

Respect required PR checks/reviews before integration; never force a merge or
administratively bypass them. Once integration and local-main sync succeed,
report the PR, commit, intended version when applicable, validation and cleanup.
Do not wait for post-merge packaging or release publication. A merge/sync failure
is an incomplete task and must remain visible; a later asynchronous packaging
failure can be handled separately.
