# Agent Implementation Workflow

Status: Repository automation and the owner's implementation-task contract.

An implementation normally ends with a Gitea PR merged into `main`, the primary
local checkout synchronized, and the task's branches/worktree cleaned up. This
makes the user's local installer consume integrated work. Review-only, PR-only,
local-only and installation instructions override the appropriate default.
`AGENTS.md` provides standing authorization; the `agent-workflow` skill supplies
agent procedure and `finish-release` supplies version/publication policy.

## Task identity and operations

`scripts/agent_workflow.py` uses Python's standard library and Git. One task
owns a unique `codex/` branch, a separate worktree and one PR. Gitea PRs and
published commits are authoritative inventory; token-free JSON records under
the common Git directory's `gti-agent-workflow/` retain operation state and
recovery pointers. They are shared across worktrees, never committed, and
contain no credentials. HTTP redirects are refused. Canonical fetch and push
URLs are both checked before credentials or code are sent.

All shared operations hold an atomic directory lock with PID/time metadata;
an interrupted lock must be inspected before its exact directory is removed.
This serializes agents sharing the repository. Gitea's up-to-date protection
also closes the race with other machines. It does not lock ordinary editing.

| Command | Result |
| --- | --- |
| `start <task> --title <text> --worktree <path> [--branch codex/<name>] [--adopt]` | Fetch main, create or register a clean worktree, publish its branch; rediscover a PR after the first meaningful commit. |
| `checkpoint <task> [--message <text> --paths <files...>]` | Commit explicitly scoped paths or publish an existing clean commit; create a draft PR on the first meaningful checkpoint. |
| `reconcile <task>` | Merge current main into the task branch, preserve conflicts for resolution, invalidate old validation and publish. |
| `verify <task> --checks-file <json>` | Execute nonempty argv arrays without a shell; record passing checks against the published clean head and current remote main. |
| `finish <task> --body-file <text> [--pr-only]` | Publish the final PR description, mark ready, verify identities/protection, merge the exact head without bypasses and synchronize local main. PR-only stops ready for review. |
| `sync <task>` | Confirm merged-head/merge-commit reachability, fast-forward primary main, preserving unrelated local edits. |
| `cleanup <task>` | After confirmed merge/sync, remove a plain clean worktree with no ignored files and delete only unchanged integrated branches. |
| `abandon <task> --reason <text>` | Publish the final checkpoint and close its PR with a reason, preserving branch and worktree. |
| `status [task]` | Read task records and current remote PR state. |
| `protection [--configure]` | Audit, or establish the owner-approved PR-only/up-to-date main rule, retaining existing review/status requirements. |

Checkpoint paths are literal files, including exact identities for deleted
files: Git pathspec magic, deleted-directory prefixes, directory/root staging,
and pre-existing staged work cannot silently include another task's changes.
New task starts reject any already-published branch/PR identity. First publication
uses a compare-and-create lease, so a concurrently created branch is preserved.

`--repo <checkout>` precedes the command when invoked elsewhere. Credentials
come from `GTI_GITEA_TOKEN`, `GITEA_TOKEN`, or the primary checkout's ignored
`.env`; the helper parses the token key without executing the file. A worktree
never needs its own credential file.

## Integration and validation

The first branch push can contain no changes; draft creation waits for a real
commit because unchanged branches cannot form meaningful PRs. Gitea's `WIP:`
title marks drafts. The helper verifies that the instance recognizes the draft.
An interrupted PR-create request is recovered by branch lookup before another
create. Head and base repository identities must both be canonical GTI, so a
similarly named fork branch cannot be adopted or modified. Closed task branches
are not reused.

Checkpoint publication is distinct from validation. The agent selects suitable
checks using the owning skill and `verify-change`, with optimized compilers,
required examples and cross-phase evidence. Verification records are operational
evidence, not a security attestation or a replacement for code review. Each
record includes executed commands, head, base and time. Any changed commit,
dirty source or advancing main requires reconciliation/revalidation. Failed
checks never mark the task ready.

Finish uses a merge commit, `head_commit_id` and `force_merge=false` on Gitea.
The main protection rule denies direct/force pushes, disables bypass allowlists,
blocks admin overrides and rejects outdated branches. Existing required checks
and reviews are preserved and enforced by Gitea. Pending/rejected requirements
leave work unmerged; an agent resumes after they change rather than claiming
completion. Release workflows run after integration and need not finish before
local installation. VERSION follows normal repository policy; workflow-only
automation does not change the packaged compiler version.

## Preservation and cleanup

Fast-forward sync leaves unrelated primary edits in place and refuses overlap.
Incoming paths are checked for untracked/ignored collisions, and Git receives
`--no-overwrite-ignore` so a newly tracked path cannot overwrite ignored data.
The agent backs up only the overlapping intended files and reconciles them
before retrying. Divergent primary commits receive a named recovery ref and
must be integrated through a separate PR; no reset or hidden merge rewrites
the primary checkout. Report merge success separately from blocked local sync.

Cleanup requires remote PR integration, local main reachability and unchanged
branch tips. Remote deletion uses a compare-and-delete lease for the merged SHA;
this is a conditional deletion, never a rewritten shared history. New source,
untracked files, unexpected commits or ignored files stop removal. Managed app
worktrees should be archived with the app after preserving needed ignored data,
then the helper can finish branch cleanup. A recovery snapshot retained by that
archive remains accessible through the chat attachment. Unmerged abandoned work
is retained indefinitely until a separate explicit disposition.

Interrupted operations are retried using the same task. Task records survive
merge rejection and sync failure. A confirmed remote merge resumes at sync;
cleanup tolerates branches already deleted. If records are lost, preserve and
adopt the published branch/worktree, rediscover the PR, and confirm the remote
merge before proceeding. Recovery adoption accepts an existing open/ready PR
even when main advanced, allowing helper reconciliation without first requiring
an unknown task to reconcile itself. Already merged tasks resume sync; lost
abandonment responses are confirmed remotely and do not republish closed work.
Server API rejections are reported without secrets.
