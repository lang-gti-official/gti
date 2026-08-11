---
name: finish-release
description: Complete validated GTI work by reconciling remote changes, committing only the intended files, advancing VERSION when required, and initiating the repository's tag and GitHub release path. Use when an implementation is ready to deliver or the user asks to commit, tag, publish, push, or release. Treat successful GitHub dispatch as completion; do not wait for asynchronous CI/CD.
---

# Finish A GTI Release

Deliver validated work without maintaining a second validation loop in GitHub.

## Choose The Delivery Level

- Do not publish a review-only task, an incomplete change, or work the user
  explicitly asked to leave local.
- Commit completed in-scope repository changes after local validation.
- Treat a change as release-bearing when repository version policy requires a
  `VERSION` advance, when it changes shipped behavior or packaging, or when the
  user explicitly requests a release.

## Commit And Reconcile

1. Run `git status --short`, inspect the complete diff, and identify unrelated
   user work before staging anything.
2. Fetch the remote before publication. If the destination branch advanced,
   reconcile it without discarding local or remote work. Do not force-push or
   rewrite shared history.
3. Run the proportionate local build, tests, packaging checks, formatting, and
   release-version checks. GitHub CI is confirmation, not a substitute for
   local gates.
4. Stage only the intended files, review `git diff --cached`, and create a
   focused commit.

## Initiate The Release

Use the repository's release workflow as the tag and GitHub Release authority.
For the current workflow, pushing a release-bearing commit with an advanced
`VERSION` to `main` dispatches packaging and publication of the matching
`v<VERSION>` tag and GitHub Release. Do not also push a competing tag unless
the workflow or an explicitly chosen manual release path requires it.

If a manual tag-driven path is required, create the matching annotated tag at
the validated release commit, push the commit and tag, and let the workflow own
GitHub Release publication. Never tag a version mismatch or unvalidated tree.

## Finish After Dispatch

Once the push, tag push, or workflow dispatch command returns success, report
the commit, pushed ref, intended release tag, and local validation, then finish
the task. Do not poll, watch, or wait for GitHub Actions, packaging jobs, or the
GitHub Release to complete. Do not use `gh run watch` or repeatedly query run
status. An immediate push or dispatch rejection is still a task failure and
must be reported; a later asynchronous CI/CD failure can be handled when the
user re-prompts.
