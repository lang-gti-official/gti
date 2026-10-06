#!/usr/bin/env python3
"""GTI task branches: published checkpoints, verified PR merges, safe cleanup."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
import uuid

CANONICAL = "https://git.internal.howetotech.dev/gti-lang/gti.git"
API = "https://git.internal.howetotech.dev/api/v1/repos/gti-lang/gti"
REPOSITORY = "gti-lang/gti"
NAME = re.compile(r"[a-z0-9][a-z0-9-]{0,63}\Z")


class WorkflowError(RuntimeError):
    pass


def git(root, *args, check=True):
    result = subprocess.run(["git", "-C", str(root), *args], capture_output=True,
                            text=True, check=False)
    if check and result.returncode:
        raise WorkflowError(result.stderr.strip() or result.stdout.strip())
    return result.stdout.strip() if check else result


def clean(root):
    if git(root, "status", "--porcelain"):
        raise WorkflowError(f"Uncommitted changes in {root}; preserve and commit them first")


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class Gitea:
    def __init__(self, primary):
        token = os.environ.get("GTI_GITEA_TOKEN") or os.environ.get("GITEA_TOKEN")
        if not token:
            env_file = Path(primary) / ".env"
            if env_file.exists():
                for line in env_file.read_text().splitlines():
                    key, sep, value = line.partition("=")
                    if sep and key.strip() in {"GTI_GITEA_TOKEN", "GITEA_TOKEN"}:
                        token = value.strip().strip("\"'")
                        break
        if not token:
            raise WorkflowError("Set GTI_GITEA_TOKEN or GITEA_TOKEN (or put it in the primary checkout's .env)")
        self.token = token

    def request(self, method, path, payload=None):
        data = None if payload is None else json.dumps(payload).encode()
        req = urllib.request.Request(API + path, data=data, method=method,
                                     headers={"Authorization": "token " + self.token,
                                              "Content-Type": "application/json"})
        try:
            with urllib.request.build_opener(NoRedirect).open(req, timeout=30) as result:
                body = result.read()
        except (urllib.error.HTTPError, urllib.error.URLError) as error:
            # Do not print response bodies, credentials, or unknown redirects.
            status = getattr(error, "code", "unavailable")
            raise WorkflowError(f"Gitea {method} {path} failed ({status}); use status and retry") from error
        return json.loads(body) if body else None


class Workflow:
    def __init__(self, repo, api=None, enforce_origin=True):
        self.repo = Path(git(repo, "rev-parse", "--show-toplevel")).resolve()
        self.common = Path(git(repo, "rev-parse", "--path-format=absolute", "--git-common-dir")).resolve()
        self.primary = self.common.parent
        if self.common.name != ".git":
            raise WorkflowError("Use a regular checkout with a primary .git directory")
        if enforce_origin and (git(self.repo, "remote", "get-url", "origin") != CANONICAL or
                               git(self.repo, "remote", "get-url", "--push", "--all", "origin") != CANONICAL):
            raise WorkflowError("origin must be canonical GTI Gitea; credentials are never sent to other hosts")
        self.state_dir = self.common / "gti-agent-workflow"
        self.state_dir.mkdir(exist_ok=True)
        self.api = api or Gitea(self.primary)

    @contextmanager
    def lock(self):
        path = self.state_dir / "lock"
        try:
            path.mkdir()
        except FileExistsError as error:
            raise WorkflowError(f"Another workflow operation holds {path}; if interrupted, inspect its owner before removing the lock") from error
        try:
            (path / "owner.json").write_text(json.dumps({"pid": os.getpid(), "time": self.now()}))
            yield
        finally:
            (path / "owner.json").unlink(missing_ok=True)
            path.rmdir()

    @staticmethod
    def now():
        return datetime.now(timezone.utc).isoformat()

    def file(self, task):
        if not NAME.fullmatch(task):
            raise WorkflowError("Task names must be lowercase letters, digits and hyphens, up to 64 characters")
        return self.state_dir / (task + ".json")

    def load(self, task):
        try:
            state = json.loads(self.file(task).read_text())
        except FileNotFoundError as error:
            raise WorkflowError(f"Unknown task {task}; use start --adopt to register a preserved branch") from error
        return state

    def save(self, state, phase=None):
        if phase:
            state["phase"] = phase
        state["updated_at"] = self.now()
        destination = self.file(state["task"])
        temporary = destination.with_suffix(".tmp")
        temporary.write_text(json.dumps(state, indent=2) + "\n")
        temporary.replace(destination)

    def fetch(self):
        git(self.primary, "fetch", "origin", "main")
        return git(self.primary, "rev-parse", "origin/main")

    def ancestor(self, older, newer):
        return git(self.primary, "merge-base", "--is-ancestor", older, newer, check=False).returncode == 0

    def tree(self, state):
        root = Path(state["worktree"])
        if not root.is_dir() or Path(git(root, "rev-parse", "--show-toplevel")).resolve() != root:
            raise WorkflowError("Task worktree is missing or has changed identity")
        if git(root, "branch", "--show-current") != state["branch"]:
            raise WorkflowError("Task worktree is checked out on a different branch")
        if Path(git(root, "rev-parse", "--path-format=absolute", "--git-common-dir")).resolve() != self.common:
            raise WorkflowError("Task belongs to another repository")
        return root

    def pull(self, state):
        if not state.get("pr"):
            raise WorkflowError("Task has no PR; checkpoint the first meaningful change")
        pr = self.api.request("GET", f"/pulls/{state['pr']}")
        if not self.matches(pr, state["branch"]):
            raise WorkflowError("PR branch identity changed; do not merge or clean up")
        return pr

    @staticmethod
    def matches(pr, branch):
        return (pr.get("head", {}).get("ref") == branch and
                pr.get("base", {}).get("ref") == "main" and
                all((pr.get(side, {}).get("repo") or {}).get("full_name") == REPOSITORY
                    for side in ("head", "base")))

    def find_pr(self, branch):
        found = []
        page = 1
        while True:
            prs = self.api.request("GET", f"/pulls?state=all&limit=50&page={page}")
            found.extend(p for p in prs if self.matches(p, branch))
            if len(prs) < 50:
                break
            page += 1
        if len(found) > 1:
            raise WorkflowError("More than one PR uses this task branch")
        return found[0] if found else None

    def ensure_pr(self, state):
        if state.get("pr"):
            return self.pull(state)
        # Recover an interrupted create, and include closed PRs to avoid reuse.
        pr = self.find_pr(state["branch"])
        created = pr is None
        if created:
            pr = self.api.request("POST", "/pulls", {
                "base": "main", "head": state["branch"],
                "title": "WIP: " + state["title"],
                "body": f"Workflow task `{state['task']}`. Implementation and validation are in progress."})
        if not self.matches(pr, state["branch"]):
            raise WorkflowError("Created PR has unexpected repository/branch identity")
        state.update(pr=pr["number"], pr_url=pr["html_url"])
        self.save(state)
        if pr["state"] != "open" or pr.get("merged"):
            raise WorkflowError("This task's PR is closed; do not reuse its branch")
        if created and not pr.get("draft"):
            raise WorkflowError("Gitea did not recognize WIP: as a draft; mark the PR draft before continuing")
        return pr

    def start(self, task, title, worktree, branch=None, adopt=False):
        path = self.file(task)
        if path.exists():
            state = self.load(task)
            if title != state["title"] or str(Path(worktree).resolve()) != state["worktree"]:
                raise WorkflowError("Existing task has a different title or worktree")
            root = Path(state["worktree"])
            if state["phase"] == "starting" and not root.exists():
                ref = "refs/heads/" + state["branch"]
                if git(self.primary, "show-ref", "--verify", ref, check=False).returncode:
                    git(self.primary, "worktree", "add", "-b", state["branch"], str(root), state["base"])
                else:
                    git(self.primary, "worktree", "add", str(root), state["branch"])
            self.tree(state)
            return self.checkpoint(task)
        base = self.fetch()
        root = Path(worktree).resolve()
        if root == self.primary or self.primary in root.parents:
            raise WorkflowError("Use a separate worktree outside the primary checkout")
        branch = branch or "codex/" + task + "-" + uuid.uuid4().hex[:8]
        if not branch.startswith("codex/") or git(self.primary, "check-ref-format", "--branch", branch, check=False).returncode:
            raise WorkflowError("Use a valid codex/ task branch")
        for record in self.state_dir.glob("*.json"):
            other = json.loads(record.read_text())
            if other.get("branch") == branch or other.get("worktree") == str(root):
                raise WorkflowError("Another task already owns this branch or worktree")
        state = {"schema": 1, "task": task, "title": title, "branch": branch,
                 "worktree": str(root), "base": base, "phase": "starting"}
        if root.exists():
            if not adopt:
                raise WorkflowError("An existing worktree requires --adopt")
            self.tree(state)
            clean(root)
            recovered = self.find_pr(branch)
            if recovered:
                state.update(pr=recovered["number"], pr_url=recovered["html_url"])
                head = git(root, "rev-parse", "HEAD")
                if recovered.get("merged"):
                    if head != recovered["head"]["sha"]:
                        raise WorkflowError("Merged branch has new commits; preserve them as a new task")
                    state.update(merged_head=head, merge_commit=recovered["merge_commit_sha"])
                    self.save(state, "merged")
                    return self.sync(task)
                if recovered["state"] == "closed":
                    self.save(state, "abandoned")
                    return state
                if not self.ancestor(recovered["head"]["sha"], head):
                    raise WorkflowError("Local branch diverges from its published PR; preserve and reconcile it before adoption")
                state["base"] = git(root, "merge-base", head, base)
            elif not self.ancestor(base, git(root, "rev-parse", "HEAD")):
                raise WorkflowError("Adopted branch must include current origin/main; reconcile it first")
        else:
            if git(self.primary, "show-ref", "--verify", "refs/heads/" + branch, check=False).returncode == 0:
                raise WorkflowError("Branch already exists; do not reuse another task's work")
            # Persist identity before mutations so interruptions can be resumed.
            self.save(state)
            git(self.primary, "worktree", "add", "-b", branch, str(root), base)
        self.save(state, "active")
        return self.checkpoint(task)

    def checkpoint(self, task, message=None, paths=()):
        state = self.load(task)
        if state["phase"] in {"merged", "synced", "completed", "abandoned"}:
            raise WorkflowError("Task is already integrated or abandoned")
        root = self.tree(state)
        if message:
            if not paths:
                raise WorkflowError("Checkpoint commits require explicit file paths")
            if git(root, "diff", "--cached", "--name-only"):
                raise WorkflowError("Index already contains staged work; review and commit it explicitly")
            for item in paths:
                target = (root / item).resolve()
                if target == root or root not in target.parents or target.is_dir() or item in {".", "*"}:
                    raise WorkflowError("Stage explicit in-worktree paths, never the repository root")
            git(root, "--literal-pathspecs", "add", "--", *paths)
            git(root, "diff", "--cached", "--check")
            git(root, "commit", "-m", message)
        clean(root)
        if state.get("pr") and self.pull(state)["state"] != "open":
            raise WorkflowError("PR is closed; do not publish more work to this branch")
        git(root, "push", "-u", "origin", "HEAD:refs/heads/" + state["branch"])
        state["published_head"] = git(root, "rev-parse", "HEAD")
        state.pop("verified", None)
        self.save(state, "active")
        if git(root, "rev-list", "--count", state["base"] + "..HEAD") != "0":
            self.ensure_pr(state)
        return state

    def reconcile(self, task):
        state = self.load(task)
        if state["phase"] in {"merged", "synced", "completed", "abandoned"} or (state.get("pr") and self.pull(state)["state"] != "open"):
            raise WorkflowError("Task is integrated or closed; do not mutate its branch")
        root = self.tree(state)
        clean(root)
        base = self.fetch()
        state.pop("verified", None)
        self.save(state, "reconciling")
        result = git(root, "merge", "--no-edit", "origin/main", check=False)
        if result.returncode:
            self.save(state, "blocked")
            raise WorkflowError("Upstream merge needs resolution in the task worktree; resolve, commit and checkpoint it")
        state["base"] = base
        self.save(state, "active")
        return self.checkpoint(task)

    def verify(self, task, checks):
        state = self.load(task)
        if state["phase"] in {"merged", "synced", "completed", "abandoned"} or (state.get("pr") and self.pull(state)["state"] != "open"):
            raise WorkflowError("Task is integrated or closed; do not validate it as new work")
        root = self.tree(state)
        clean(root)
        base = self.fetch()
        head = git(root, "rev-parse", "HEAD")
        if not self.ancestor(base, head):
            raise WorkflowError("main advanced; reconcile before verification")
        if state.get("published_head") != head:
            raise WorkflowError("Checkpoint the current head before verification")
        if not isinstance(checks, list) or not checks or any(not isinstance(c, list) or not c or
                             any(not isinstance(a, str) or not a for a in c) for c in checks):
            raise WorkflowError("Checks must be a nonempty JSON array of argv arrays")
        state.pop("verified", None)
        self.save(state, "verifying")
        for command in checks:
            result = subprocess.run(command, cwd=root, check=False)
            if result.returncode:
                self.save(state, "blocked")
                raise WorkflowError("Validation failed; correct it and verify again")
        clean(root)
        if git(root, "rev-parse", "HEAD") != head or self.fetch() != base:
            self.save(state, "active")
            raise WorkflowError("Head or main changed during validation; reconcile and verify again")
        state["verified"] = {"head": head, "base": base, "checks": checks, "at": self.now()}
        self.save(state, "ready")
        return state

    def protection(self, configure=False):
        rules = self.api.request("GET", "/branch_protections")
        rule = next((r for r in rules if r.get("rule_name", r.get("branch_name")) == "main"), None)
        if configure:
            payload = {k: v for k, v in (rule or {}).items()
                       if k not in {"created_at", "updated_at", "branch_name"}}
            payload.update({"rule_name": "main", "enable_push": False,
                       "enable_force_push": False, "block_on_outdated_branch": True,
                       "block_admin_merge_override": True, "enable_bypass_allowlist": False})
            rule = self.api.request("PATCH" if rule else "POST",
                                    "/branch_protections/main" if rule else "/branch_protections", payload)
        if not rule or rule.get("enable_push") or rule.get("enable_force_push") or not rule.get("block_on_outdated_branch") or not rule.get("block_admin_merge_override") or rule.get("enable_bypass_allowlist"):
            raise WorkflowError("main protection must deny direct/force pushes and bypasses, and block outdated/admin-override merges; use protection --configure")
        return rule

    def finish(self, task, body, pr_only=False):
        state = self.load(task)
        pr = self.pull(state)
        if not pr.get("merged"):
            root = self.tree(state)
            clean(root)
            verified = state.get("verified", {})
            head, base = git(root, "rev-parse", "HEAD"), self.fetch()
            if verified.get("head") != head or verified.get("base") != base or pr["head"]["sha"] != head:
                raise WorkflowError("Current head/base/PR differ from validation; checkpoint, reconcile and verify again")
            if not body.strip():
                raise WorkflowError("Provide the final reviewable PR description")
            self.api.request("PATCH", f"/pulls/{state['pr']}",
                             {"title": state["title"], "body": body})
            if pr_only:
                self.save(state, "ready")
                return state
            self.protection()
            # The server's outdated-branch rule closes the fetch/merge race.
            self.api.request("POST", f"/pulls/{state['pr']}/merge", {
                "do": "merge", "head_commit_id": head, "force_merge": False,
                "delete_branch_after_merge": False})
            pr = self.pull(state)
            if not pr.get("merged"):
                raise WorkflowError("Merge is not confirmed; leave the task active and retry finish")
        state["merged_head"] = pr["head"]["sha"]
        state["merge_commit"] = pr["merge_commit_sha"]
        self.save(state, "merged")
        return self.sync(task)

    def sync(self, task):
        state = self.load(task)
        pr = self.pull(state)
        if not pr.get("merged"):
            raise WorkflowError("PR has not merged; do not advance local main")
        base = self.fetch()
        if not self.ancestor(pr["head"]["sha"], base) or not self.ancestor(pr["merge_commit_sha"], base):
            raise WorkflowError("Merged PR commits are not reachable from current remote main")
        if git(self.primary, "branch", "--show-current") != "main":
            raise WorkflowError("Primary checkout is not on main; do not change another task's branch")
        old = git(self.primary, "rev-parse", "HEAD")
        if not self.ancestor(old, base):
            backup = "refs/gti-agent-workflow/recovery/" + task + "-main"
            git(self.primary, "update-ref", backup, old)
            self.save(state, "sync-blocked")
            raise WorkflowError(f"Local main has unique commits preserved at {backup}; reconcile them through a separate PR")
        # Git can overwrite ignored untracked files when a new tracked path
        # arrives. Detect file/directory/symlink collisions before fast-forward.
        incoming = git(self.primary, "diff", "--name-only", "--diff-filter=ACMRT", "-z", old, base)
        for relative in filter(None, incoming.split("\0")):
            target = self.primary / relative
            for candidate in (target, *target.parents):
                if candidate == self.primary:
                    break
                if not candidate.exists() and not candidate.is_symlink():
                    continue
                if candidate != target and candidate.is_dir() and not candidate.is_symlink():
                    continue
                name = str(candidate.relative_to(self.primary))
                tracked = git(self.primary, "--literal-pathspecs", "ls-files", "--error-unmatch", "--", name, check=False)
                if tracked.returncode:
                    self.save(state, "sync-blocked")
                    raise WorkflowError(f"Untracked/ignored path {name} would be overwritten; preserve it before retrying sync")
        # Git preserves unrelated dirty files and refuses overlapping edits.
        result = git(self.primary, "merge", "--ff-only", "--no-overwrite-ignore", "origin/main", check=False)
        if result.returncode:
            self.save(state, "sync-blocked")
            raise WorkflowError("PR merged, but local edits block main sync; back up overlapping files and reconcile them before retrying sync")
        state["local_main"] = git(self.primary, "rev-parse", "HEAD")
        self.save(state, "synced")
        return state

    def cleanup(self, task):
        state = self.load(task)
        pr = self.pull(state)
        main = self.fetch()
        if state["phase"] not in {"synced", "completed"} or not pr.get("merged"):
            raise WorkflowError("Only a merged and locally synced task may be cleaned up")
        head = pr["head"]["sha"]
        if not self.ancestor(head, main):
            raise WorkflowError("Task head is no longer integrated")
        if git(self.primary, "branch", "--show-current") != "main" or not self.ancestor(head, git(self.primary, "rev-parse", "HEAD")):
            raise WorkflowError("Primary main no longer contains this task; sync before cleanup")
        branch_ref = "refs/heads/" + state["branch"]
        branch = git(self.primary, "rev-parse", "--verify", branch_ref, check=False)
        if branch.returncode == 0 and branch.stdout.strip() != head:
            raise WorkflowError("Branch acquired new commits; preserve them before cleanup")
        # Conditional delete prevents losing remote work pushed after the merge.
        remote = git(self.primary, "ls-remote", "origin", branch_ref)
        if remote and remote.split()[0] != head:
            raise WorkflowError("Remote branch acquired new commits; preserve them before cleanup")
        root = Path(state["worktree"])
        if root.exists():
            self.tree(state)
            clean(root)
            if git(root, "ls-files", "--others", "--ignored", "--exclude-standard"):
                raise WorkflowError("Worktree contains ignored files; inspect/preserve them and archive a managed worktree with the app before cleanup")
            git(self.primary, "worktree", "remove", str(root))
        listed = git(self.primary, "worktree", "list", "--porcelain")
        if "worktree " + str(root) + "\n" in listed + "\n":
            raise WorkflowError("Worktree is still registered; archive/remove it before cleanup")
        if remote:
            git(self.primary, "push", "--force-with-lease=" + branch_ref + ":" + head,
                "origin", ":" + branch_ref)
        if branch.returncode == 0:
            # Main reachability above is authoritative; never force-delete.
            git(self.primary, "branch", "-d", state["branch"])
        self.save(state, "completed")
        return state

    def abandon(self, task, reason):
        if not reason.strip():
            raise WorkflowError("Record why work is abandoned or superseded")
        state = self.load(task)
        if state["phase"] == "abandoned":
            return state
        if state.get("pr"):
            pr = self.pull(state)
            if pr.get("merged"):
                raise WorkflowError("Merged work cannot be abandoned")
            if pr["state"] == "closed":
                root = self.tree(state)
                clean(root)
                if git(root, "rev-parse", "HEAD") != pr["head"]["sha"]:
                    raise WorkflowError("Closed PR has unpublished local work; preserve it in a new task")
                state["reason"] = reason
                self.save(state, "abandoned")
                return state
        state = self.checkpoint(task)
        self.ensure_pr(state)
        pr = self.pull(state)
        state["reason"] = reason
        self.save(state, "abandoning")
        self.api.request("PATCH", f"/pulls/{state['pr']}", {
            "state": "closed", "body": pr.get("body", "") + "\n\nPreserved, unmerged: " + reason})
        self.save(state, "abandoned")
        return state

    def status(self, task=None):
        records = [self.load(task)] if task else [json.loads(p.read_text()) for p in sorted(self.state_dir.glob("*.json"))]
        for state in records:
            if state.get("pr"):
                pr = self.pull(state)
                state["remote_state"] = "merged" if pr.get("merged") else pr["state"]
        return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=".")
    sub = parser.add_subparsers(dest="command", required=True)
    start = sub.add_parser("start")
    start.add_argument("task")
    start.add_argument("--title", required=True)
    start.add_argument("--worktree", required=True)
    start.add_argument("--branch")
    start.add_argument("--adopt", action="store_true")
    checkpoint = sub.add_parser("checkpoint")
    checkpoint.add_argument("task")
    checkpoint.add_argument("--message")
    checkpoint.add_argument("--paths", nargs="+", default=[])
    for name in ("reconcile", "sync", "cleanup"):
        sub.add_parser(name).add_argument("task")
    verify = sub.add_parser("verify")
    verify.add_argument("task")
    verify.add_argument("--checks-file", required=True)
    finish = sub.add_parser("finish")
    finish.add_argument("task")
    finish.add_argument("--body-file", required=True)
    finish.add_argument("--pr-only", action="store_true")
    abandon = sub.add_parser("abandon")
    abandon.add_argument("task")
    abandon.add_argument("--reason", required=True)
    sub.add_parser("status").add_argument("task", nargs="?")
    sub.add_parser("protection").add_argument("--configure", action="store_true")
    args = parser.parse_args()
    try:
        workflow = Workflow(args.repo)
        with workflow.lock():
            if args.command == "start":
                result = workflow.start(args.task, args.title, args.worktree, args.branch, args.adopt)
            elif args.command == "checkpoint":
                result = workflow.checkpoint(args.task, args.message, args.paths)
            elif args.command == "verify":
                result = workflow.verify(args.task, json.loads(Path(args.checks_file).read_text()))
            elif args.command == "finish":
                result = workflow.finish(args.task, Path(args.body_file).read_text(), args.pr_only)
            elif args.command == "abandon":
                result = workflow.abandon(args.task, args.reason)
            elif args.command == "protection":
                result = workflow.protection(args.configure)
            else:
                result = getattr(workflow, args.command)(args.task)
        print(json.dumps(result, indent=2))
        return 0
    except (WorkflowError, OSError, ValueError) as error:
        print("agent-workflow: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
