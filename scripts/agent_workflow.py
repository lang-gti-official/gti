#!/usr/bin/env python3
"""GTI task branches: published checkpoints, verified GitHub PR merges, a Gitea mirror, safe cleanup."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

CANONICAL = "https://github.com/lang-gti-official/gti.git"
REPOSITORY = "lang-gti-official/gti"
API = "https://api.github.com/repos/" + REPOSITORY
GRAPHQL = "https://api.github.com/graphql"
# Gitea keeps a complete copy of main and the tags as a backup; it hosts no
# task branches, PRs, or issues. `sync` pushes there after every integration.
MIRROR_REMOTE = "gitea"
MIRROR = "https://git.internal.howetotech.dev/gti-lang/gti.git"
NAME = re.compile(r"[a-z0-9][a-z0-9-]{0,63}\Z")
LOCK_WAIT_SECONDS = 30.0


def load_build_window():
    """Import the sibling build-window module without requiring scripts/ on sys.path."""
    path = Path(__file__).resolve().with_name("build_window.py")
    spec = importlib.util.spec_from_file_location("gti_build_window", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


build_window = load_build_window()


def load_remote_runner():
    path = Path(__file__).resolve().with_name("remote_runner.py")
    spec = importlib.util.spec_from_file_location("gti_remote_runner", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


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


class GitHub:
    """GitHub REST (plus one GraphQL mutation) for the canonical repository."""

    def __init__(self, primary):
        token = (os.environ.get("GTI_GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
                 or os.environ.get("GITHUB_TOKEN"))
        if not token:
            env_file = Path(primary) / ".env"
            if env_file.exists():
                for line in env_file.read_text().splitlines():
                    key, sep, value = line.partition("=")
                    if sep and key.strip() in {"GTI_GITHUB_TOKEN", "GH_TOKEN", "GITHUB_TOKEN"}:
                        token = value.strip().strip("\"'")
                        break
        if not token:
            # The authenticated GitHub CLI is the ordinary developer credential.
            result = subprocess.run(["gh", "auth", "token"], capture_output=True, text=True, check=False)
            token = result.stdout.strip() if result.returncode == 0 else ""
        if not token:
            raise WorkflowError("Authenticate the GitHub CLI (gh auth login) or set GTI_GITHUB_TOKEN")
        self.token = token

    def request(self, method, path, payload=None, url=None):
        data = None if payload is None else json.dumps(payload).encode()
        req = urllib.request.Request(url or API + path, data=data, method=method,
                                     headers={"Authorization": "Bearer " + self.token,
                                              "Accept": "application/vnd.github+json",
                                              "X-GitHub-Api-Version": "2022-11-28",
                                              "Content-Type": "application/json"})
        try:
            with urllib.request.build_opener(NoRedirect).open(req, timeout=30) as result:
                body = result.read()
        except urllib.error.HTTPError as error:
            if error.code == 404 and method == "GET" and path.startswith("/branches/"):
                return None  # an unprotected branch, not a failure
            # Do not print response bodies, credentials, or unknown redirects.
            raise WorkflowError(f"GitHub {method} {path} failed ({error.code}); use status and retry") from error
        except urllib.error.URLError as error:
            raise WorkflowError(f"GitHub {method} {path} failed (unavailable); use status and retry") from error
        return json.loads(body) if body else None

    def ready(self, pr):
        """Leave draft state; REST cannot, so this is the one GraphQL call."""
        result = self.request("POST", "/graphql", {
            "query": "mutation($id: ID!) { markPullRequestReadyForReview(input: {pullRequestId: $id})"
                     " { pullRequest { isDraft } } }",
            "variables": {"id": pr["node_id"]}}, url=GRAPHQL)
        if (result or {}).get("errors") or (result or {}).get("data", {}).get(
                "markPullRequestReadyForReview", {}).get("pullRequest", {}).get("isDraft", True):
            raise WorkflowError("GitHub did not mark the PR ready for review; use status and retry")


class Workflow:
    def __init__(self, repo, api=None, enforce_origin=True):
        self.repo = Path(git(repo, "rev-parse", "--show-toplevel")).resolve()
        self.common = Path(git(repo, "rev-parse", "--path-format=absolute", "--git-common-dir")).resolve()
        self.primary = self.common.parent
        if self.common.name != ".git":
            raise WorkflowError("Use a regular checkout with a primary .git directory")
        if enforce_origin and (git(self.repo, "remote", "get-url", "origin") != CANONICAL or
                               git(self.repo, "remote", "get-url", "--push", "--all", "origin") != CANONICAL):
            raise WorkflowError("origin must be canonical GTI GitHub; credentials are never sent to other hosts")
        if enforce_origin and self.mirror_remote() and (
                git(self.repo, "remote", "get-url", MIRROR_REMOTE) != MIRROR or
                git(self.repo, "remote", "get-url", "--push", "--all", MIRROR_REMOTE) != MIRROR):
            raise WorkflowError(f"remote {MIRROR_REMOTE} must be the GTI Gitea mirror; fix its URLs before syncing")
        self.state_dir = self.common / "gti-agent-workflow"
        self.state_dir.mkdir(exist_ok=True)
        self.api = api or GitHub(self.primary)
        # Heavy validation runs inside the shared build window, never under
        # the metadata lock, so peers can register and inspect tasks meanwhile.
        self.window = build_window.Window(build_window.directory_for(self.common))
        self.lock_wait = LOCK_WAIT_SECONDS

    @contextmanager
    def lock(self):
        """Short repo-wide lock for metadata operations; waits briefly, never removes a live lock."""
        path = self.state_dir / "lock"
        deadline = time.monotonic() + self.lock_wait
        while True:
            try:
                path.mkdir()
                break
            except FileExistsError as error:
                if time.monotonic() >= deadline:
                    raise WorkflowError(f"Another workflow operation holds {path}; if interrupted, inspect its owner before removing the lock") from error
                time.sleep(0.2)
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

    def load(self, task, legacy=False):
        try:
            state = json.loads(self.file(task).read_text())
        except FileNotFoundError as error:
            raise WorkflowError(f"Unknown task {task}; use start --adopt to register a preserved branch") from error
        if not legacy and self.is_legacy(state):
            raise WorkflowError("Task record predates the GitHub move; its PR lived on Gitea. Push the branch to origin "
                                "and re-register it with start --adopt (the old record is kept as *.legacy-gitea.json)")
        return state

    @staticmethod
    def is_legacy(state):
        return state.get("host") != "github"

    def mirror_remote(self):
        remotes = git(self.primary, "remote", check=False).stdout.split()
        return MIRROR_REMOTE if MIRROR_REMOTE in remotes else None

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
        head = urllib.parse.quote(REPOSITORY.split("/")[0] + ":" + branch, safe="")
        while True:
            prs = self.api.request("GET", f"/pulls?state=all&per_page=100&page={page}&head={head}")
            found.extend(p for p in prs if self.matches(p, branch))
            if len(prs) < 100:
                break
            page += 1
        if len(found) > 1:
            raise WorkflowError("More than one PR uses this task branch")
        # List entries omit `merged`; the single-PR view carries it.
        return self.api.request("GET", f"/pulls/{found[0]['number']}") if found else None

    def ensure_pr(self, state):
        if state.get("pr"):
            return self.pull(state)
        # Recover an interrupted create, and include closed PRs to avoid reuse.
        pr = self.find_pr(state["branch"])
        created = pr is None
        if created:
            pr = self.api.request("POST", "/pulls", {
                "base": "main", "head": state["branch"], "draft": True,
                "title": state["title"],
                "body": f"Workflow task `{state['task']}`. Implementation and validation are in progress."})
        if not self.matches(pr, state["branch"]):
            raise WorkflowError("Created PR has unexpected repository/branch identity")
        state.update(pr=pr["number"], pr_url=pr["html_url"])
        self.save(state)
        if pr["state"] != "open" or pr.get("merged"):
            raise WorkflowError("This task's PR is closed; do not reuse its branch")
        if created and not pr.get("draft"):
            raise WorkflowError("GitHub did not create a draft PR; mark the PR draft before continuing")
        return pr

    def start(self, task, title, worktree, branch=None, adopt=False):
        path = self.file(task)
        if path.exists() and self.is_legacy(self.load(task, legacy=True)):
            # A Gitea-era record steps aside; its branch is re-registered on GitHub.
            if not adopt:
                raise WorkflowError("Task record predates the GitHub move; re-register its worktree with start --adopt")
            path.replace(path.with_suffix(".legacy-gitea.json"))
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
            if self.is_legacy(other):
                continue  # a Gitea-era record no longer owns anything
            if other.get("branch") == branch or other.get("worktree") == str(root):
                raise WorkflowError("Another task already owns this branch or worktree")
        state = {"schema": 2, "host": "github", "task": task, "title": title, "branch": branch,
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
            if git(self.primary, "ls-remote", "origin", "refs/heads/" + branch) or self.find_pr(branch):
                raise WorkflowError("Remote branch or PR already uses this identity; adopt preserved work or choose a new branch")
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
                if not target.exists():
                    names = git(root, "--literal-pathspecs", "ls-files", "-z", "--", item)
                    if list(filter(None, names.split("\0"))) != [target.relative_to(root).as_posix()]:
                        raise WorkflowError("Deleted paths must name one exact tracked file, not a directory or prefix")
            git(root, "--literal-pathspecs", "add", "--", *paths)
            git(root, "diff", "--cached", "--check")
            git(root, "commit", "-m", message)
        clean(root)
        existing = self.pull(state) if state.get("pr") else self.find_pr(state["branch"])
        if existing and existing["state"] != "open":
            raise WorkflowError("PR is closed; do not publish more work to this branch")
        ref = "refs/heads/" + state["branch"]
        remote = git(root, "ls-remote", "origin", ref)
        if not remote:
            # Compare-and-create only: do not advance a concurrently created ref.
            git(root, "push", "-u", "--force-with-lease=" + ref + ":", "origin", "HEAD:" + ref)
        else:
            git(root, "push", "-u", "origin", "HEAD:" + ref)
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

    def verify(self, task, checks, remote=False):
        """Record the attempt under the lock, run the checks in a build-window suite slot, then record the result.

        The metadata lock is held only while task records change, so a peer's
        `start`, `status`, `checkpoint` or `cleanup` never waits on validation.
        Local checks hold one `suite` slot. Remote plans acquire their build/
        suite slots on the configured host through remote_runner, not here.
        """
        with self.lock():
            state = self.load(task)
            if state["phase"] == "verifying":
                raise WorkflowError("Verification is already running; resume its existing session or remote run instead of submitting a duplicate")
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
            runner = load_remote_runner() if remote else None
            if remote:
                try:
                    runner.validate_steps(checks)
                except runner.RunnerError as error:
                    raise WorkflowError(str(error)) from error
            elif not isinstance(checks, list) or not checks or any(not isinstance(c, list) or not c or
                                 any(not isinstance(a, str) or not a for a in c) for c in checks):
                raise WorkflowError("Checks must be a nonempty JSON array of argv arrays")
            state.pop("verified", None)
            self.save(state, "verifying")
        remote_evidence = None
        try:
            if remote:
                # No laptop suite slot: it performs only snapshot/transport.
                # The remote worker owns the same authoritative window there.
                destination = self.state_dir / "remote-results" / task / uuid.uuid4().hex
                with tempfile.TemporaryDirectory(prefix="gti-remote-verification-") as temporary:
                    plan = Path(temporary) / "steps.json"
                    plan.write_text(json.dumps(checks))
                    try:
                        code = runner.main(["--repo", str(root), "run", "--steps-file", str(plan),
                                            "--results", str(destination)])
                    except (runner.RunnerError, OSError, ValueError) as error:
                        self.record(task, "verifying", "blocked")
                        raise WorkflowError(str(error)) from error
                if code:
                    self.record(task, "verifying", "blocked")
                    raise WorkflowError(f"Remote validation failed ({code}); inspect {destination}")
                try:
                    report = json.loads((destination / "report.json").read_text())
                except (OSError, ValueError) as error:
                    self.record(task, "verifying", "blocked")
                    raise WorkflowError("Remote validation report is missing or unreadable") from error
                if (not isinstance(report, dict) or not isinstance(report.get("source"), dict)
                        or not isinstance(report.get("final_source"), dict)
                        or report.get("state") != "passed" or report.get("exit_code") != 0
                        or report.get("source", {}).get("head") != head
                        or report.get("source", {}).get("status") != ""
                        or report["final_source"].get("head") != head
                        or report["final_source"].get("status") != ""):
                    self.record(task, "verifying", "blocked")
                    raise WorkflowError("Remote evidence does not match the clean published head")
                remote_evidence = {"directory": str(destination), "id": report["id"],
                                   "snapshot_sha256": report["source"]["snapshot_sha256"]}
            else:
                with self.window.acquire("suite", [sys.executable, __file__, "verify", task],
                                     worktree=str(root), checks=checks):
                    for command in checks:
                        result = subprocess.run(command, cwd=root, check=False)
                        if result.returncode:
                            self.record(task, "verifying", "blocked")
                            raise WorkflowError("Validation failed; correct it and verify again")
        except build_window.WindowError as error:
            self.record(task, "verifying", "active")
            raise WorkflowError(str(error)) from error
        with self.lock():
            state = self.load(task)
            clean(root)
            if (state["phase"] != "verifying" or state.get("published_head") != head or
                    git(root, "rev-parse", "HEAD") != head or self.fetch() != base):
                self.save(state, "active")
                raise WorkflowError("Head or main changed during validation; reconcile and verify again")
            state["verified"] = {"head": head, "base": base, "checks": checks, "at": self.now()}
            if remote_evidence is not None:
                state["verified"]["remote"] = remote_evidence
            self.save(state, "ready")
            return state

    def record(self, task, expected, phase):
        """Move a task from `expected` to `phase` under the lock, leaving concurrent changes alone."""
        with self.lock():
            state = self.load(task)
            if state["phase"] == expected:
                self.save(state, phase)

    def protection(self, configure=False, batch_gate=False):
        rule = self.api.request("GET", "/branches/main/protection")
        if configure:
            checks = (rule or {}).get("required_status_checks") or {}
            contexts = list(checks.get("contexts") or [])
            if batch_gate and "Validation gate" not in contexts:
                contexts.append("Validation gate")
            reviews = (rule or {}).get("required_pull_request_reviews") or {}
            payload = {
                # Up-to-date branches only: this closes the fetch/merge race.
                "required_status_checks": {"strict": True, "contexts": contexts},
                # Admins follow the same rules; nobody bypasses them.
                "enforce_admins": True,
                # A pull request is the only way into main; existing review
                # requirements are kept, otherwise none are added.
                "required_pull_request_reviews": {
                    "dismiss_stale_reviews": bool(reviews.get("dismiss_stale_reviews", False)),
                    "require_code_owner_reviews": bool(reviews.get("require_code_owner_reviews", False)),
                    "required_approving_review_count": int(reviews.get("required_approving_review_count", 0)),
                    "require_last_push_approval": bool(reviews.get("require_last_push_approval", False))},
                "restrictions": None,
                "allow_force_pushes": False,
                "allow_deletions": False,
                "required_linear_history": bool(((rule or {}).get("required_linear_history") or {}).get("enabled", False)),
                "required_conversation_resolution": bool(
                    ((rule or {}).get("required_conversation_resolution") or {}).get("enabled", False)),
                "block_creations": False,
                "lock_branch": False}
            if checks.get("checks"):
                payload["required_status_checks"]["checks"] = checks["checks"]
            rule = self.api.request("PUT", "/branches/main/protection", payload)

        def enabled(name):
            return bool(((rule or {}).get(name) or {}).get("enabled", False))

        if (not rule or not enabled("enforce_admins") or enabled("allow_force_pushes") or enabled("allow_deletions")
                or not (rule.get("required_status_checks") or {}).get("strict")
                or not rule.get("required_pull_request_reviews")):
            raise WorkflowError("main protection must require pull requests and up-to-date branches, bind admins, and deny "
                                "force pushes and deletion; use protection --configure")
        return rule

    def finish(self, task, body, pr_only=False):
        state = self.load(task)
        pr = self.pull(state)
        if not pr.get("merged"):
            if state.get("admission") or state.get("integration_batch"):
                raise WorkflowError("Component is ready for integration, not directly mergeable; finish its validated batch or batch withdraw first")
            if state.get("batch"):
                from validation_batch import Batches
                Batches(self).before_finish(state)
            root = self.tree(state)
            clean(root)
            if (root / ".gti-preview.json").exists():
                raise WorkflowError("Preview candidates cannot merge wholesale into main; finish each original task PR")
            verified = state.get("verified", {})
            head, base = git(root, "rev-parse", "HEAD"), self.fetch()
            if git(root, "log", "--format=%H", base + ".." + head, "--", ".gti-preview.json"):
                raise WorkflowError("Preview history cannot merge wholesale into main; finish each original task PR")
            if verified.get("head") != head or verified.get("base") != base or pr["head"]["sha"] != head:
                raise WorkflowError("Current head/base/PR differ from validation; checkpoint, reconcile and verify again")
            if not body.strip():
                raise WorkflowError("Provide the final reviewable PR description")
            # Body edits are CI routing events. Retrying a merge while checks
            # run must not cancel and restart that same validation campaign.
            if pr.get("title") != state["title"] or pr.get("body") != body:
                self.api.request("PATCH", f"/pulls/{state['pr']}",
                                 {"title": state["title"], "body": body})
            if pr.get("draft"):
                self.api.ready(pr)
            if pr_only:
                self.save(state, "ready")
                return state
            self.protection()
            # `sha` pins the exact validated head; the branch rule's up-to-date
            # requirement closes the fetch/merge race on the server.
            self.api.request("PUT", f"/pulls/{state['pr']}/merge", {
                "merge_method": "merge", "sha": head})
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
        state["mirror"] = self.mirror()
        self.save(state, "synced")
        return state

    def mirror(self):
        """Push main and the tags to the Gitea backup. A failure is reported, never hidden, and does not undo sync."""
        remote = self.mirror_remote()
        if not remote:
            return {"remote": None, "pushed": False, "at": self.now(), "error": f"no {MIRROR_REMOTE} remote"}
        # main is never forced: a merge that landed only on the mirror must surface
        # as a rejected push, not vanish. Tags are forced so a moved `nightly` follows.
        result = git(self.primary, "push", remote, "refs/heads/main:refs/heads/main", check=False)
        if result.returncode == 0:
            result = git(self.primary, "push", "--force", remote, "refs/tags/*:refs/tags/*", check=False)
        error = None if result.returncode == 0 else (result.stderr.strip() or "push failed")
        if error:
            print(f"agent-workflow: mirror to {remote} failed: {error}", file=sys.stderr)
        return {"remote": remote, "pushed": result.returncode == 0, "at": self.now(), "error": error}

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

    def retire_empty(self, state, reason):
        """Retire a task without a PR whose branch holds nothing beyond main.

        Abandonment keeps unmerged work behind a closed PR, but GitHub refuses
        a PR without commits and an empty branch has nothing to keep. Every
        check runs before the first removal, and each removal tolerates a piece
        an interrupted attempt already removed.
        """
        main = self.fetch()
        ref = "refs/heads/" + state["branch"]
        local = git(self.primary, "rev-parse", "--verify", ref, check=False)
        remote = git(self.primary, "ls-remote", "origin", ref)
        root = Path(state["worktree"])
        tips = {remote.split()[0]} if remote else set()
        if local.returncode == 0:
            tips.add(local.stdout.strip())
        if root.exists():
            self.tree(state)
            tips.add(git(root, "rev-parse", "HEAD"))
        if any(not self.ancestor(tip, main) for tip in tips):
            return False
        if root.exists():
            clean(root)
            if git(root, "ls-files", "--others", "--ignored", "--exclude-standard"):
                raise WorkflowError("Worktree contains ignored files; inspect/preserve them before abandoning")
        if local.returncode == 0 and (
                git(self.primary, "branch", "--show-current") != "main"
                or not self.ancestor(local.stdout.strip(), git(self.primary, "rev-parse", "HEAD"))):
            raise WorkflowError("Primary main does not contain this task's branch; sync before abandoning")
        if root.exists():
            git(self.primary, "worktree", "remove", str(root))
        if remote:
            # Conditional delete: a commit pushed meanwhile keeps the branch.
            git(self.primary, "push", "--force-with-lease=" + ref + ":" + remote.split()[0],
                "origin", ":" + ref)
        if local.returncode == 0:
            git(self.primary, "branch", "-d", state["branch"])
        state["reason"] = reason
        self.save(state, "abandoned")
        return True

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
        elif self.retire_empty(state, reason):
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
        records = [self.load(task, legacy=True)] if task else [
            json.loads(p.read_text()) for p in sorted(self.state_dir.glob("*.json"))]
        for state in records:
            if self.is_legacy(state):
                state["remote_state"] = "legacy-gitea"
            elif state.get("pr"):
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
    verification = verify.add_mutually_exclusive_group(required=True)
    verification.add_argument("--checks-file")
    verification.add_argument("--remote-steps-file", help="remote_runner exact-argv plan; uses the remote build window")
    finish = sub.add_parser("finish")
    finish.add_argument("task")
    finish.add_argument("--body-file", required=True)
    finish.add_argument("--pr-only", action="store_true")
    abandon = sub.add_parser("abandon")
    abandon.add_argument("task")
    abandon.add_argument("--reason", required=True)
    sub.add_parser("status").add_argument("task", nargs="?")
    protection = sub.add_parser("protection")
    protection.add_argument("--configure", action="store_true")
    protection.add_argument("--batch-gate", action="store_true", help="add the published CI Validation gate without removing existing requirements")
    sub.add_parser("preview", help="optionally submit a detached combined preview job").add_argument(
        "arguments", nargs=argparse.REMAINDER)
    sub.add_parser("batch", help="admit focused fixes and validate an explicit combined batch").add_argument(
        "arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    try:
        workflow = Workflow(args.repo)
        if args.command == "batch":
            sys.modules.setdefault("agent_workflow", sys.modules[__name__])
            from validation_batch import main as batch_main
            print(json.dumps(batch_main(workflow, args.arguments), indent=2))
            return 0
        if args.command == "preview":
            # Keep one exception/Git helper identity when this file is __main__.
            sys.modules.setdefault("agent_workflow", sys.modules[__name__])
            from preview_workflow import main as preview_main
            print(json.dumps(preview_main(workflow, args.arguments), indent=2))
            return 0
        if args.command == "verify":
            # verify takes the metadata lock itself, only around record changes.
            result = workflow.verify(args.task, json.loads(Path(args.remote_steps_file or args.checks_file).read_text()),
                                     remote=bool(args.remote_steps_file))
            print(json.dumps(result, indent=2))
            return 0
        with workflow.lock():
            if args.command == "start":
                result = workflow.start(args.task, args.title, args.worktree, args.branch, args.adopt)
            elif args.command == "checkpoint":
                result = workflow.checkpoint(args.task, args.message, args.paths)
            elif args.command == "finish":
                result = workflow.finish(args.task, Path(args.body_file).read_text(), args.pr_only)
            elif args.command == "abandon":
                result = workflow.abandon(args.task, args.reason)
            elif args.command == "protection":
                if args.batch_gate and not args.configure:
                    raise WorkflowError("--batch-gate requires --configure after publishing its workflow")
                result = workflow.protection(args.configure, args.batch_gate)
            else:
                result = getattr(workflow, args.command)(args.task)
        print(json.dumps(result, indent=2))
        return 0
    except (WorkflowError, OSError, ValueError) as error:
        print("agent-workflow: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
