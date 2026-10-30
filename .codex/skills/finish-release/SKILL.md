---
name: finish-release
description: Apply GTI version and release policy to focused-validated work delivered through the agent-workflow merge-queue lifecycle. Use when an implementation is ready to deliver or the user asks to commit, publish or release. Asynchronous release packaging does not block completion.
---

# Finish A GTI Release

Deliver validated work through [agent-workflow](../agent-workflow/SKILL.md).
The normal implementation outcome is a PR in the merge queue, then a merged PR
and synchronized local main;
explicit PR-only/local-only instructions override integration. Apply release
policy below without bypassing that lifecycle.

## Choose The Delivery Level

- Do not release incomplete work or publish a read-only review or work the user
  asked to leave local. Unfinished implementation checkpoints may be published
  to their task branch and draft PR under agent-workflow.
- Commit completed in-scope repository changes after local validation.
- Degraded-mode batch admission is a handoff, not a release. Component fixes
  retain their original PRs and fragments. Do not tag, install or publish a
  stable release from an admitted component.
- Never edit `VERSION` or `CHANGELOG.md` in a task. A change to shipped
  behavior or packaging adds one [changelog fragment](../../../changes/README.md),
  `changes/<task>.<breaking|feature|fix>.md`, holding its user-facing CHANGELOG
  text. Only a release cut (`scripts/cut_release.py`, committed with its
  `Release-Cut: <version>` trailer) changes `VERSION`; run one only when the
  owner asks for a release.
- When a commit touches a file that `scripts/check_release_version.py`
  classifies as shipped but changes nothing a user would observe, either add an
  `internal` fragment or end that commit's message with a
  `Release-Impact: internal` trailer and one nonempty `Release-Reason:` trailer
  explaining why no user-visible behavior changes.
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
   The destination must be GTI's canonical GitHub repository (`origin`,
   `lang-gti-official/gti`). Never publish source branches or PRs to Gitea;
   its `main` is frozen and it keeps only the reference site and packages.
3. Run the focused build, tests, packaging checks, formatting, and
   release-version checks (`python3 scripts/check_release_version.py
   origin/main HEAD`) that `verify-change` selects. The merge queue runs the
   complete suite; do not repeat it locally. Use the
   [execution contract](../../../docs/architecture/agent-workflow.md#bounded-output-analysis-and-waiting)
   for bounded output and waiting.
4. Stage only the intended files, review `git diff --cached`, and create a
   focused commit.

## Initiate The Release

Toolchain releases publish to GitHub Releases; only the `gti add`
package registry stays on Gitea. `.github/workflows/nightly.yml` republishes
the rolling `nightly` prerelease daily from the newest main commit that passed
the merge queue, so a merged change reaches nightly users without any release
step. Stable `v<VERSION>` publication belongs to the later beta/stable release
train and is not automated yet: merging a release-bearing PR publishes no
stable release, so report the intended version. The frozen Gitea workflow
(`.gitea/workflows/release.yml`) no longer runs; never publish to Gitea. Never
tag a version mismatch or an unvalidated tree by hand, do not push a competing
tag, and never move the `nightly` tag yourself.

## Finish After Queueing

Respect required reviews and the merge queue; never force a merge or
administratively bypass them. Once the PR is queued with recorded focused
validation, report the PR, its changelog fragment when applicable, exactly which
checks ran, and that the complete suite is the queue's. After the queue merges
it and local-main sync succeeds, report the merge and local commit. A queue
removal or sync failure keeps the task open and visible; a later asynchronous
packaging failure can be handled separately.
