#!/usr/bin/env python3
"""GTI task branches: published checkpoints, merge-queue integration on GitHub and safe cleanup."""

from __future__ import annotations

import argparse
from contextlib import contextmanager, nullcontext
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
OWNER = REPOSITORY.split("/")[0]
API = "https://api.github.com/repos/" + REPOSITORY
GRAPHQL = "https://api.github.com/graphql"
HOST = "github"
SCHEMA = 4
REHOST_SCHEMAS = {("github", 2), ("gitea", 3)}
# The one required status check. CI reports it on pull_request (fast checks)
# and on merge_group (the complete integration suite of the queued candidate).
GATE_CHECK = "CI gate"
NAME = re.compile(r"[a-z0-9][a-z0-9-]{0,63}\Z")
# Rollups (scripts/rollup.py) compose several green PRs into one queue entry.
# `finish` labels an eligible PR for them instead of queueing it alone.
ROLLUP_MAYBE, ROLLUP_ALWAYS, ROLLUP_NEVER = "rollup=maybe", "rollup=always", "rollup=never"
ROLLUP_CHOICES = (ROLLUP_MAYBE, ROLLUP_ALWAYS, ROLLUP_NEVER)
ROLLUP_FAILED, ROLLUP_REBASE = "rollup-failed", "needs-rebase"
ROLLUP_BRANCH = re.compile(r"rollup/[0-9]{8}-[0-9]{1,4}\Z")
# The bot's comment when it closes a member whose head a merged rollup carried.
ROLLUP_MARKER = re.compile(r"<!-- gti-rollup-integrated: (\{[^\n]*?\}) -->")


def rollup_marker(record):
    return "<!-- gti-rollup-integrated: " + json.dumps(record, sort_keys=True) + " -->"


def label_names(pr):
    return {label.get("name") for label in pr.get("labels") or [] if isinstance(label, dict)}
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
    """GitHub REST/GraphQL for the canonical repository; credentials never follow redirects."""

    def __init__(self, primary=None, token=None):
        # An explicit token (the rollup bot's App token) is used as given.
        token = token or os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
        if not token:
            # The authenticated GitHub CLI is the normal credential source; the
            # token is read from it, never stored by this helper.
            try:
                result = subprocess.run(["gh", "auth", "token", "--hostname", "github.com"],
                                        capture_output=True, text=True, check=False, timeout=30)
            except (OSError, subprocess.TimeoutExpired):
                result = None
            token = result.stdout.strip() if result and result.returncode == 0 else ""
        if not token:
            raise WorkflowError("Authenticate the GitHub CLI (gh auth login) or set GH_TOKEN")
        self.token = token

    def _open(self, url, method, payload):
        data = None if payload is None else json.dumps(payload).encode()
        req = urllib.request.Request(url, data=data, method=method,
                                     headers={"Authorization": "Bearer " + self.token,
                                              "Accept": "application/vnd.github+json",
                                              "X-GitHub-Api-Version": "2022-11-28",
                                              "Content-Type": "application/json"})
        with urllib.request.build_opener(NoRedirect).open(req, timeout=30) as result:
            body = result.read()
        return json.loads(body) if body else None

    def request(self, method, path, payload=None):
        try:
            return self._open(API + path, method, payload)
        except urllib.error.HTTPError as error:
            error.close()
            # Do not print response bodies, credentials, or unknown redirects.
            raise WorkflowError(f"GitHub {method} {path} failed ({error.code}); use status and retry") from error
        except urllib.error.URLError as error:
            raise WorkflowError(f"GitHub {method} {path} failed (unavailable); use status and retry") from error

    def graphql(self, query, variables):
        try:
            result = self._open(GRAPHQL, "POST", {"query": query, "variables": variables})
        except (urllib.error.HTTPError, urllib.error.URLError) as error:
            if isinstance(error, urllib.error.HTTPError):
                error.close()
            raise WorkflowError("GitHub GraphQL request failed; use status and retry") from error
        if not isinstance(result, dict) or result.get("errors"):
            messages = "; ".join(str(item.get("message", "error"))[:200]
                                 for item in (result or {}).get("errors", []) if isinstance(item, dict))
            raise WorkflowError("GitHub GraphQL request was refused: " + (messages or "unknown error"))
        return result["data"]

class Workflow:
    def __init__(self, repo, api=None, enforce_origin=True):
        self.repo = Path(git(repo, "rev-parse", "--show-toplevel")).resolve()
        self.common = Path(git(repo, "rev-parse", "--path-format=absolute", "--git-common-dir")).resolve()
        self.primary = self.common.parent
        if self.common.name != ".git":
            raise WorkflowError("Use a regular checkout with a primary .git directory")
        if enforce_origin and (git(self.repo, "remote", "get-url", "origin") != CANONICAL or
                               git(self.repo, "remote", "get-url", "--push", "--all", "origin") != CANONICAL):
            raise WorkflowError("origin must be canonical GTI on GitHub; credentials are never sent to other hosts")
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
            raise WorkflowError("Task record predates the GitHub repository; use rehost for a migrated historical "
                                "PR, or start --adopt for preserved work. Historical tracker numbers are not interchangeable")
        return state

    @staticmethod
    def is_legacy(state):
        return state.get("host") != HOST or state.get("schema") != SCHEMA

    def rehost(self, task, number):
        """Rebind a published historical task to its migrated PR without rewriting its evidence.

        GitHub history carries rewritten commit identities, so commit IDs
        differ while every tree is byte-identical. The migrated PR head must
        have exactly the tree of the task's last published historical head, and a
        clean worktree at that head moves to the identical rewritten commit.
        """
        state = self.load(task, legacy=True)
        if state.get("phase") == "verifying":
            raise WorkflowError("Validation is still running; wait for its owner to finish before rehosting")
        if not self.is_legacy(state):
            if state.get("pr") != number:
                raise WorkflowError("Task is already bound to another GitHub PR")
            self.pull(state)
            return state
        if (state.get("host"), state.get("schema")) not in REHOST_SCHEMAS or not state.get("published_head"):
            raise WorkflowError("Only a published schema-2 GitHub or schema-3 Gitea task can be rehosted; "
                                "use start --adopt for other preserved work")
        pr = self.api.request("GET", f"/pulls/{number}")
        if not self.matches(pr, state["branch"]) or pr.get("state") != "open":
            raise WorkflowError("Migrated PR repository, branch or state differs; preserve and investigate")
        head = pr["head"]["sha"]
        git(self.primary, "fetch", "origin", "main", "refs/heads/" + state["branch"])
        old_tree = git(self.primary, "rev-parse", "--verify", state["published_head"] + "^{tree}", check=False)
        if old_tree.returncode or old_tree.stdout.strip() != git(self.primary, "rev-parse", head + "^{tree}"):
            raise WorkflowError("Migrated PR head does not carry the task's published tree; preserve and investigate")
        recovery_name = "gitea-hosting-record.json" if state["host"] == "gitea" else "pre-cutover-hosting-record.json"
        recovery = self.state_dir / "evidence" / task / recovery_name
        recovery.parent.mkdir(parents=True, exist_ok=True)
        if recovery.exists():
            if json.loads(recovery.read_text()) != state:
                raise WorkflowError("Original hosting recovery record already exists with different contents")
        else:
            recovery.write_text(json.dumps(state, indent=2) + "\n")
        root = Path(state["worktree"])
        if root.exists():
            self.tree(state)
            clean(root)
            if git(root, "rev-parse", "HEAD") != state["published_head"]:
                raise WorkflowError("Worktree holds unpublished commits; preserve them before rehosting")
            # Identical trees: moving the branch changes no file.
            git(root, "reset", "--keep", head)
        state.pop("verified", None)
        state.update(schema=SCHEMA, host=HOST, pr=number, pr_url=pr["html_url"], published_head=head,
                     base=git(self.primary, "merge-base", head, "origin/main"),
                     hosting_migration={"from_host": state["host"], "from_pr": state.get("pr_url"),
                                        "from_head": state["published_head"], "at": self.now(),
                                        "recovery": str(recovery)})
        self.save(state, "active")
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
        query = urllib.parse.urlencode({"state": "all", "head": OWNER + ":" + branch, "per_page": 100})
        found = [p for p in self.api.request("GET", "/pulls?" + query) if self.matches(p, branch)]
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
                "base": "main", "head": state["branch"], "title": state["title"], "draft": True,
                "body": f"Workflow task `{state['task']}`. Implementation and validation are in progress."})
        if not self.matches(pr, state["branch"]):
            raise WorkflowError("Created PR has unexpected repository/branch identity")
        state.update(pr=pr["number"], pr_url=pr["html_url"])
        self.save(state)
        if pr["state"] != "open" or pr.get("merged"):
            raise WorkflowError("This task's PR is closed; do not reuse its branch")
        if created and not pr.get("draft"):
            raise WorkflowError("GitHub did not create the PR as a draft")
        return pr

    def start(self, task, title, worktree, branch=None, adopt=False):
        path = self.file(task)
        if path.exists() and self.is_legacy(self.load(task, legacy=True)):
            # Never bind a historical tracker number to the replacement repo.
            if not adopt:
                raise WorkflowError("Task record predates the GitHub repository; use rehost or start --adopt")
            old = self.load(task, legacy=True)
            if old.get("pr") and (old.get("host") == "gitea" or
                                  (old.get("host"), old.get("schema")) in REHOST_SCHEMAS):
                raise WorkflowError("Use rehost to preserve the migrated PR and its verification evidence")
            recovery = self.state_dir / "evidence" / task / "legacy-record.json"
            recovery.parent.mkdir(parents=True, exist_ok=True)
            if recovery.exists():
                raise WorkflowError("Historical recovery record exists; inspect it before adoption")
            path.replace(recovery)
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
                continue  # historical records cannot identify PRs on this host
            if other.get("branch") == branch or other.get("worktree") == str(root):
                raise WorkflowError("Another task already owns this branch or worktree")
        state = {"schema": SCHEMA, "host": HOST, "task": task, "title": title, "branch": branch,
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
        if existing and existing["head"]["sha"] != git(root, "rev-parse", "HEAD"):
            self.cancel_handoff(state, existing)
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
        if state.get("pr"):
            self.cancel_handoff(state, self.pull(state))
        state.pop("verified", None)
        self.save(state, "reconciling")
        result = git(root, "merge", "--no-edit", "origin/main", check=False)
        if result.returncode:
            self.save(state, "blocked")
            raise WorkflowError("Upstream merge needs resolution in the task worktree; resolve, commit and checkpoint it")
        state["base"] = base
        self.save(state, "active")
        return self.checkpoint(task)

    def verify(self, task, checks, remote=False, focused=False):
        """Record the attempt under the lock, run the checks in a build-window suite slot, then record the result.

        This is the task's own focused evidence. The merge queue validates the
        combined candidate against whatever main is then, so the evidence binds
        to the task head only and an advancing main never voids it.

        The metadata lock is held only while task records change, so a peer's
        `start`, `status`, `checkpoint` or `cleanup` never waits on validation.
        Local broad checks hold one `suite` slot; checked automation/source-library
        owning checks may run directly. Remote plans acquire their build/suite
        slots on the configured host through remote_runner, not here.
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
            if state.get("published_head") != head:
                raise WorkflowError("Checkpoint the current head before verification")
            if focused:
                from validation_checks import focused_scope, source_focused_checks
                paths = git(root, "diff", "--name-only", base, head).splitlines()
                scope = focused_scope(paths)
                if remote or scope is None:
                    raise WorkflowError("Focused helper verification is only for local automation-only or source-stdlib changes; compiler components use batch enroll")
                if scope == "source-stdlib":
                    try:
                        source_focused_checks(checks)
                    except ValueError as error:
                        raise WorkflowError(str(error)) from error
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
                window = (nullcontext() if focused else self.window.acquire(
                    "suite", [sys.executable, __file__, "verify", task], worktree=str(root), checks=checks))
                with window:
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
                    git(root, "rev-parse", "HEAD") != head):
                self.save(state, "active")
                raise WorkflowError("Head changed during validation; checkpoint and verify again")
            state["verified"] = {"head": head, "base": base, "checks": checks, "at": self.now()}
            if focused:
                state["verified"]["scope"] = scope
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

    RULES = {"deletion", "non_fast_forward", "pull_request", "required_status_checks", "merge_queue"}

    @staticmethod
    def ruleset():
        """The main ruleset: PRs only, through the merge queue, gated by CI, no bypass."""
        return {
            "name": "main", "target": "branch", "enforcement": "active", "bypass_actors": [],
            "conditions": {"ref_name": {"include": ["refs/heads/main"], "exclude": []}},
            "rules": [
                {"type": "deletion"},
                {"type": "non_fast_forward"},
                {"type": "pull_request", "parameters": {
                    "required_approving_review_count": 0, "dismiss_stale_reviews_on_push": False,
                    "require_code_owner_review": False, "require_last_push_approval": False,
                    "required_review_thread_resolution": False, "allowed_merge_methods": ["merge"]}},
                {"type": "required_status_checks", "parameters": {
                    "strict_required_status_checks_policy": False, "do_not_enforce_on_create": False,
                    "required_status_checks": [{"context": GATE_CHECK}]}},
                # Group size 1: rollups, not the queue, combine compatible work.
                {"type": "merge_queue", "parameters": {
                    "merge_method": "MERGE", "grouping_strategy": "ALLGREEN",
                    "max_entries_to_build": 2, "min_entries_to_merge": 1, "max_entries_to_merge": 1,
                    "min_entries_to_merge_wait_minutes": 0, "check_response_timeout_minutes": 360}},
            ]}

    def protection(self, configure=False):
        rulesets = self.api.request("GET", "/rulesets?includes_parents=false&per_page=100") or []
        named = [rule for rule in rulesets if rule.get("name") == "main" and rule.get("target") == "branch"]
        if len(named) > 1:
            raise WorkflowError("More than one main ruleset exists; inspect them before configuring")
        if configure:
            payload = self.ruleset()
            if named:
                self.api.request("PUT", f"/rulesets/{named[0]['id']}", payload)
            else:
                named = [self.api.request("POST", "/rulesets", payload)]
        detail = self.api.request("GET", f"/rulesets/{named[0]['id']}") if named else None
        active = self.api.request("GET", "/rules/branches/main") or []
        types = {rule.get("type") for rule in active}
        contexts = {check.get("context") for rule in active if rule.get("type") == "required_status_checks"
                    for check in rule.get("parameters", {}).get("required_status_checks", [])}
        if (not detail or detail.get("enforcement") != "active" or detail.get("bypass_actors") or
                not self.RULES <= types or GATE_CHECK not in contexts):
            raise WorkflowError("main must require pull requests, the merge queue and the CI gate check, and deny "
                                "force pushes, deletion and bypass; use protection --configure")
        return detail

    ENQUEUE = ("mutation($id: ID!, $head: GitObjectID!) { enqueuePullRequest(input: "
               "{pullRequestId: $id, expectedHeadOid: $head}) { mergeQueueEntry { position } } }")
    READY = "mutation($id: ID!) { markPullRequestReadyForReview(input: {pullRequestId: $id}) { clientMutationId } }"

    MERGE_WHEN_READY = ("mutation($id: ID!, $head: GitObjectID!) { enablePullRequestAutoMerge(input: "
                        "{pullRequestId: $id, expectedHeadOid: $head}) { clientMutationId } }")
    INTEGRATION = ("query($id: ID!) { node(id: $id) { ... on PullRequest { "
                   "headRefOid merged isInMergeQueue autoMergeRequest { enabledAt } } } }")
    CANCEL_AUTO = ("mutation($id: ID!) { disablePullRequestAutoMerge(input: "
                   "{pullRequestId: $id}) { clientMutationId } }")
    DEQUEUE = ("mutation($id: ID!) { dequeuePullRequest(input: "
               "{pullRequestId: $id}) { clientMutationId } }")

    def integration(self, pr):
        result = self.api.graphql(self.INTEGRATION, {"id": pr["node_id"]}).get("node")
        if (not isinstance(result, dict) or not result.get("headRefOid") or
                not isinstance(result.get("isInMergeQueue"), bool) or
                not isinstance(result.get("merged"), bool) or "autoMergeRequest" not in result):
            raise WorkflowError("GitHub did not return the PR integration state; use status and retry")
        return result

    @staticmethod
    def integration_phase(remote):
        if remote["merged"]:
            return "merged"
        if remote["isInMergeQueue"]:
            return "queued"
        return "awaiting-ci" if remote["autoMergeRequest"] else "not-requested"

    def cancel_handoff(self, state, pr):
        # A new checkpoint must not inherit an older head's merge intent.
        # Fail before pushing if either cancellation cannot be confirmed.
        remote = self.integration(pr)
        if remote["merged"]:
            raise WorkflowError("PR merged during handoff cancellation; preserve new work in a new task")
        had_handoff = bool(remote["autoMergeRequest"] or remote["isInMergeQueue"])
        if remote["autoMergeRequest"]:
            self.api.graphql(self.CANCEL_AUTO, {"id": pr["node_id"]})
            remote = self.integration(pr)
        if remote["isInMergeQueue"]:
            self.api.graphql(self.DEQUEUE, {"id": pr["node_id"]})
        if had_handoff:
            if self.integration_phase(self.integration(pr)) != "not-requested":
                raise WorkflowError("PR handoff cancellation was not confirmed; do not publish a new head")
        state.pop("handoff", None)
        state.pop("queued", None)
        self.save(state, "active")

    def handoff(self, state, pr, head):
        remote = self.integration(pr)
        if remote["headRefOid"] != head:
            raise WorkflowError("PR head changed before integration handoff; checkpoint and verify again")
        if self.integration_phase(remote) == "not-requested":
            # GitHub owns the durable wait for required checks/reviews, then
            # enters the required merge queue. No polling worker or bypass.
            self.api.graphql(self.MERGE_WHEN_READY, {"id": pr["node_id"], "head": head})
            remote = self.integration(pr)
        if remote["headRefOid"] != head:
            self.cancel_handoff(state, pr)
            raise WorkflowError("PR head changed during integration handoff; checkpoint and verify again")
        phase = self.integration_phase(remote)
        if phase == "not-requested":
            raise WorkflowError("GitHub did not retain merge-when-ready; use status and retry")
        state["handoff"] = {"head": head, "at": self.now()}
        if phase == "queued":
            state["queued"] = {"head": head, "at": self.now()}
        else:
            state.pop("queued", None)
        self.save(state, phase)

    def rollup_label(self, state, pr, root, base, head):
        """`rollup=always`/`rollup=maybe` for a PR a rollup may carry, else None (validate alone)."""
        if state.get("batch") or label_names(pr) & {ROLLUP_NEVER, ROLLUP_FAILED}:
            return None
        import validation_checks as checks
        paths = git(root, "diff", "--name-only", base + "..." + head).splitlines()
        # Every path any commit touched, so a reverted system change still counts.
        touched = git(root, "log", "--format=", "--name-only", base + ".." + head).splitlines()
        if checks.system_risk(set(paths) | set(filter(None, touched))):
            return None
        return ROLLUP_ALWAYS if checks.ci_mode({}, paths) == "automation" else ROLLUP_MAYBE

    def set_rollup_label(self, pr, label):
        names = label_names(pr)
        for other in ROLLUP_CHOICES:
            if other != label and other in names:
                self.api.request("DELETE", f"/issues/{pr['number']}/labels/" + urllib.parse.quote(other, safe=""))
        if label not in names:
            self.api.request("POST", f"/issues/{pr['number']}/labels", {"labels": [label]})

    def finish(self, task, body, pr_only=False, rollup=True):
        """Hand a focused-validated task to integration, which the merge queue owns.

        An eligible PR is labelled for a rollup (phase `rollup`): the rollup bot
        composes it with other green PRs into one queue entry. Otherwise, or
        with `rollup=False`, GitHub waits for CI then queues it alone
        (phase `awaiting-ci` or `queued`). The
        queue validates the exact candidate against current main, so the task's
        own evidence binds to its head. `sync` advances local main once merged.
        """
        state = self.load(task)
        pr = self.pull(state)
        if not pr.get("merged") and pr["state"] != "open":
            # A rollup bot closure is integration only with its proof; sync checks it.
            return self.sync(task)
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
            if verified.get("head") != head or pr["head"]["sha"] != head:
                raise WorkflowError("Current head or PR differs from the recorded validation; checkpoint and verify again")
            if not body.strip():
                raise WorkflowError("Provide the final reviewable PR description")
            # PR CI re-runs the declared T0/T1 tests; claim only what verify ran.
            from validation_checks import ci_mode, declared_tests, uncovered_declarations
            if ci_mode({}, git(root, "diff", "--name-only", base, head).splitlines()) == "full":
                try:
                    unrun = uncovered_declarations(declared_tests(body), verified.get("checks", []), root)
                except (ValueError, OSError) as error:
                    raise WorkflowError(str(error)) from error
                if unrun:
                    raise WorkflowError("Declared tests were not part of the recorded validation: " + ", ".join(unrun))
            # Body edits are CI routing events. Retrying a merge while checks
            # run must not cancel and restart that same validation campaign.
            if pr.get("title") != state["title"] or pr.get("body") != body:
                self.api.request("PATCH", f"/pulls/{state['pr']}",
                                 {"title": state["title"], "body": body})
            if pr.get("draft"):
                self.api.graphql(self.READY, {"id": pr["node_id"]})
                pr = self.pull(state)
                if pr.get("draft"):
                    raise WorkflowError("GitHub still reports the PR as a draft")
            if pr_only:
                self.cancel_handoff(state, pr)
                self.save(state, "ready")
                return state
            self.protection()
            label = self.rollup_label(state, pr, root, base, head) if rollup else None
            if label:
                self.cancel_handoff(state, pr)
                self.set_rollup_label(pr, label)
                if state.get("rollup", {}).get("head") != head or state["rollup"].get("label") != label:
                    state["rollup"] = {"head": head, "label": label, "at": self.now()}
                self.save(state, "rollup")
                return state
            self.set_rollup_label(pr, ROLLUP_NEVER)
            self.handoff(state, pr, head)
            pr = self.pull(state)
            if not pr.get("merged"):
                return state
        state["merged_head"] = pr["head"]["sha"]
        state["merge_commit"] = pr["merge_commit_sha"]
        self.save(state, "merged")
        return self.sync(task)

    def integration_commit(self, state, pr, main):
        """Prove direct integration or exact inclusion in an accepted umbrella PR.

        GitHub need not mark component PRs merged. Never enable heuristic
        detection or manual-merge overrides just to make their UI say merged.
        """
        if pr.get("merged") and pr.get("merge_commit_sha") and self.ancestor(pr["merge_commit_sha"], main):
            commit = pr["merge_commit_sha"]
        elif not pr.get("merged") and state.get("integration_batch"):
            batch = self.load(state["integration_batch"])
            combined = self.pull(batch)
            members = batch.get("batch", {}).get("members", [])
            member = next((m for m in members if m.get("task") == state["task"]), {})
            proof = batch.get("verified", {})
            identity = {key: proof.get(key) for key in ("head", "base")}
            platforms = {p for m in members for p in m["plan"]["platforms"]}
            if (not combined.get("merged") or member.get("pr") != state.get("pr") or
                    member.get("head") != pr["head"]["sha"] or
                    state.get("published_head") != member.get("head") or
                    state.get("admission", {}).get("head") != member.get("head") or
                    proof.get("head") != combined["head"]["sha"] or not platforms or
                    any(batch.get("batch_verified", {}).get(p) != identity for p in platforms) or
                    not self.ancestor(member["head"], combined["head"]["sha"])):
                raise WorkflowError("Component lacks exact source and full-platform proof in its merged batch")
            manifest = json.loads(git(self.primary, "show", combined["head"]["sha"] + ":" + batch["batch"]["manifest"]))
            if manifest != {"schema": 1, "base": batch["batch"]["base"], "members": members}:
                raise WorkflowError("Merged batch manifest differs from component integration records")
            commit = combined["merge_commit_sha"]
        elif pr["state"] == "closed" and (proof := self.rollup_proof(pr, main)):
            commit = proof["merge_commit"]
            state["integrated_via_rollup"] = proof
        elif pr.get("merged"):
            commit = pr["merge_commit_sha"]
        else:
            raise WorkflowError("PR has not merged; do not advance local main")
        if not self.ancestor(pr["head"]["sha"], main) or not self.ancestor(commit, main):
            raise WorkflowError("Integrated source/merge commits are not reachable from current remote main")
        return commit

    def rollup_proof(self, pr, main):
        """A merged rollup that carried this PR's exact head into main, named by the bot's comment.

        The comment only says where to look: the rollup must be a merged
        canonical rollup PR whose head contains this PR's head, and both its
        merge commit and this head must be reachable from main.
        """
        head = pr["head"]["sha"]
        comments = self.api.request("GET", f"/issues/{pr['number']}/comments?per_page=100") or []
        for comment in reversed(comments):
            found = ROLLUP_MARKER.search(comment.get("body") or "")
            if not found:
                continue
            try:
                record = json.loads(found.group(1))
                number = int(record["rollup"])
            except (ValueError, KeyError, TypeError):
                continue
            if record.get("head") != head:
                continue
            rollup = self.api.request("GET", f"/pulls/{number}")
            commit = rollup.get("merge_commit_sha")
            if (rollup.get("merged") and commit and ROLLUP_BRANCH.fullmatch(rollup.get("head", {}).get("ref") or "")
                    and self.matches(rollup, rollup["head"]["ref"]) and self.ancestor(commit, main)
                    and self.ancestor(head, rollup["head"]["sha"]) and self.ancestor(head, main)):
                return {"rollup": number, "pr_url": rollup.get("html_url"), "head": head, "merge_commit": commit}
        return None

    def sync(self, task):
        state = self.load(task)
        pr = self.pull(state)
        base = self.fetch()
        commit = self.integration_commit(state, pr, base)
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
        if not pr.get("merged") and not state.get("integrated_via_rollup"):
            batch = self.load(state["integration_batch"])
            if pr["state"] == "open":
                self.api.request("PATCH", f"/pulls/{state['pr']}", {
                    "state": "closed", "body": (pr.get("body") or "").rstrip() +
                    f"\n\nIntegrated with exact-source validation via {batch['pr_url']} at `{commit}`."})
            if self.pull(state)["state"] != "closed":
                raise WorkflowError("Component integration is proven but PR closure is not confirmed; retry sync")
            state["integrated_via_batch"] = {"task": batch["task"], "pr_url": batch["pr_url"],
                                              "head": pr["head"]["sha"], "merge_commit": commit}
        self.save(state, "synced")
        return state

    def cleanup(self, task):
        state = self.load(task)
        pr = self.pull(state)
        main = self.fetch()
        if state["phase"] not in {"synced", "completed"}:
            raise WorkflowError("Only a merged and locally synced task may be cleaned up")
        self.integration_commit(state, pr, main)
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

        Abandonment keeps unmerged work behind a closed PR, but the host refuses
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
                state["remote_state"] = "legacy-" + str(state.get("host") or "unknown")
            elif state.get("pr"):
                pr = self.pull(state)
                state["remote_state"] = "merged" if pr.get("merged") else pr["state"]
                if pr["state"] == "open" and (state.get("handoff") or state.get("queued")):
                    remote = self.integration(pr)
                    state["remote_integration"] = self.integration_phase(remote)
                    if remote["headRefOid"] != state.get("verified", {}).get("head"):
                        state["remote_integration"] = "head-changed"
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
    rehost = sub.add_parser("rehost", help="bind a historical published task to its migrated GitHub PR by tree identity")
    rehost.add_argument("task")
    rehost.add_argument("--pr", type=int, required=True)
    checkpoint = sub.add_parser("checkpoint")
    checkpoint.add_argument("task")
    checkpoint.add_argument("--message")
    checkpoint.add_argument("--paths", nargs="+", default=[])
    for name in ("reconcile", "sync", "cleanup"):
        sub.add_parser(name).add_argument("task")
    verify = sub.add_parser("verify")
    verify.add_argument("task")
    verify.add_argument("--focused", action="store_true", help="run checked automation/source-stdlib owning checks without a broad-suite reservation")
    verification = verify.add_mutually_exclusive_group(required=True)
    verification.add_argument("--checks-file")
    verification.add_argument("--remote-steps-file", help="remote_runner exact-argv plan; uses the remote build window")
    finish = sub.add_parser("finish")
    finish.add_argument("task")
    finish.add_argument("--body-file", required=True)
    finish.add_argument("--pr-only", action="store_true")
    finish.add_argument("--no-rollup", action="store_true", help="queue this PR alone instead of labelling it for a rollup")
    abandon = sub.add_parser("abandon")
    abandon.add_argument("task")
    abandon.add_argument("--reason", required=True)
    sub.add_parser("status").add_argument("task", nargs="?")
    protection = sub.add_parser("protection")
    protection.add_argument("--configure", action="store_true", help="create or update the main ruleset")
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
                                     remote=bool(args.remote_steps_file), focused=args.focused)
            print(json.dumps(result, indent=2))
            return 0
        with workflow.lock():
            if args.command == "start":
                result = workflow.start(args.task, args.title, args.worktree, args.branch, args.adopt)
            elif args.command == "rehost":
                result = workflow.rehost(args.task, args.pr)
            elif args.command == "checkpoint":
                result = workflow.checkpoint(args.task, args.message, args.paths)
            elif args.command == "finish":
                result = workflow.finish(args.task, Path(args.body_file).read_text(), args.pr_only,
                                         rollup=not args.no_rollup)
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
