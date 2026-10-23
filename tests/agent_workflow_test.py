#!/usr/bin/env python3
"""Exercise task lifecycle with real disposable Git repositories, a fake GitHub API and a Gitea mirror."""

import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import types
import unittest
import unittest.mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import agent_workflow as module
import validation_checks as validation
from validation_batch import Batches
git = module.git
Error = module.WorkflowError


class GitHub:
    """The subset of GitHub's REST/GraphQL surface the helper uses, with GitHub's response shapes."""

    def __init__(self, remote):
        self.remote = remote
        self.prs = []
        self.rule = None
        self.merge_payload = None
        self.reject_merge = False
        self.lose_create_response = False
        self.lose_close_response = False
        self.before_merge = None
        self.ready_calls = []

    def request(self, method, path, payload=None, url=None):
        if path == "/branches/main/protection":
            if method == "GET":
                return self.rule
            self.rule = {
                "required_status_checks": payload["required_status_checks"],
                "enforce_admins": {"enabled": payload["enforce_admins"]},
                "required_pull_request_reviews": payload["required_pull_request_reviews"],
                "allow_force_pushes": {"enabled": payload["allow_force_pushes"]},
                "allow_deletions": {"enabled": payload["allow_deletions"]},
                "required_linear_history": {"enabled": payload["required_linear_history"]},
                "required_conversation_resolution": {"enabled": payload["required_conversation_resolution"]}}
            return self.rule
        if path.startswith("/pulls?"):
            # The list view carries merged_at, never `merged`.
            return [{k: v for k, v in pr.items() if k != "merged"} for pr in self.prs]
        if path == "/pulls":
            head = git(self.remote, "rev-parse", "refs/heads/" + payload["head"])
            if git(self.remote, "rev-list", "--count", "main.." + head) == "0":
                # GitHub answers 422 Validation Failed for a branch without
                # commits beyond the base.
                raise Error("Validation Failed: No commits between main and " + payload["head"])
            number = len(self.prs) + 1
            pr = {**payload, "number": number, "node_id": f"PR_{number}",
                  "html_url": f"https://example.invalid/pull/{number}", "state": "open",
                  "draft": bool(payload.get("draft")), "merged": False, "merge_commit_sha": None,
                  "head": {"ref": payload["head"], "sha": head,
                           "repo": {"full_name": module.REPOSITORY}},
                  "base": {"ref": "main", "repo": {"full_name": module.REPOSITORY}}}
            self.prs.append(pr)
            if self.lose_create_response:
                self.lose_create_response = False
                raise Error("simulated lost create response")
            return pr
        pr = self.prs[int(path.split("/")[2]) - 1]
        if not pr["merged"]:
            pr["head"]["sha"] = git(self.remote, "rev-parse", "refs/heads/" + pr["head"]["ref"])
        if method == "GET":
            return pr.copy()
        if method == "PATCH":
            pr.update(payload)
            if self.lose_close_response and payload.get("state") == "closed":
                self.lose_close_response = False
                raise Error("simulated lost close response")
            return pr
        if method == "PUT" and path.endswith("/merge"):
            self.merge_payload = payload
            if self.before_merge:
                self.before_merge()
            base = git(self.remote, "rev-parse", "main")
            head = pr["head"]["sha"]
            if (self.reject_merge or payload["sha"] != head or payload["merge_method"] != "merge"
                    or git(self.remote, "merge-base", "--is-ancestor", base, head, check=False).returncode):
                raise Error("server rejected pending checks or outdated branch")
            tree = git(self.remote, "rev-parse", head + "^{tree}")
            commit = git(self.remote, "commit-tree", tree, "-p", base, "-p", head, "-m", "merge task")
            git(self.remote, "update-ref", "refs/heads/main", commit)
            pr.update(merged=True, state="closed", merge_commit_sha=commit)
            return {"merged": True, "sha": commit}
        raise AssertionError((method, path, payload))

    def ready(self, pr):
        self.ready_calls.append(pr["number"])
        self.prs[pr["number"] - 1]["draft"] = False


class Lifecycle(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gti-workflow-test-")
        self.root = Path(self.temp.name).resolve()
        self.remote = self.root / "remote.git"
        subprocess.run(["git", "init", "--bare", str(self.remote)], check=True, capture_output=True)
        git(self.remote, "config", "user.name", "Workflow Test")
        git(self.remote, "config", "user.email", "test@example.invalid")
        self.primary = self.root / "primary"
        subprocess.run(["git", "clone", str(self.remote), str(self.primary)], check=True, capture_output=True)
        git(self.primary, "config", "user.name", "Workflow Test")
        git(self.primary, "config", "user.email", "test@example.invalid")
        git(self.primary, "switch", "-c", "main")
        (self.primary / "base.txt").write_text("base\n")
        (self.primary / ".gitignore").write_text(".env\nbuild/\n")
        git(self.primary, "add", ".")
        git(self.primary, "commit", "-m", "initial")
        git(self.primary, "push", "-u", "origin", "main")
        self.mirror = self.root / "mirror.git"
        subprocess.run(["git", "init", "--bare", str(self.mirror)], check=True, capture_output=True)
        git(self.primary, "remote", "add", module.MIRROR_REMOTE, str(self.mirror))
        self.api = GitHub(self.remote)
        self.env = unittest.mock.patch.dict(os.environ)
        self.env.start()
        for name in list(os.environ):
            if name.startswith(module.build_window.ENV_PREFIX):
                del os.environ[name]
        self.flow = module.Workflow(self.primary, self.api, enforce_origin=False)
        self.flow.lock_wait = 0
        self.flow.window.poll = 0.05
        self.window_messages = []
        self.flow.window.log = self.window_messages.append
        self.worktree = self.root / "task"
        self.branch = "codex/task"

    def tearDown(self):
        self.env.stop()
        self.temp.cleanup()

    def start(self):
        return self.flow.start("task", "Task outcome", self.worktree, self.branch)

    def checkpoint(self, content="feature\n", name="feature.txt"):
        (self.worktree / name).write_text(content)
        return self.flow.checkpoint("task", "implement", [name])

    def ready(self):
        self.start()
        self.checkpoint()
        self.flow.protection(True)
        self.flow.verify("task", [[sys.executable, "-c", "pass"]])

    def advance_remote(self):
        base = git(self.remote, "rev-parse", "main")
        tree = git(self.remote, "rev-parse", "main^{tree}")
        commit = git(self.remote, "commit-tree", tree, "-p", base, "-m", "another task")
        git(self.remote, "update-ref", "refs/heads/main", commit)

    def test_full_lifecycle_preserves_primary_edits(self):
        self.start()
        self.assertEqual(git(self.remote, "rev-parse", self.branch), git(self.primary, "rev-parse", "main"))
        self.assertEqual(self.api.prs, [])
        self.checkpoint()
        self.assertTrue(self.api.prs[0]["draft"])
        self.flow.protection(True)
        (self.primary / "base.txt").write_text("user edit\n")
        self.flow.verify("task", [[sys.executable, "-c", "pass"]])
        state = self.flow.finish("task", "Final implementation and passing tests")
        self.assertEqual(state["phase"], "synced")
        self.assertEqual((self.primary / "base.txt").read_text(), "user edit\n")
        self.assertTrue((self.primary / "feature.txt").is_file())
        self.assertEqual(self.api.merge_payload["merge_method"], "merge")
        self.assertEqual(self.api.merge_payload["sha"], state["verified"]["head"])
        self.assertEqual(self.api.ready_calls, [1])
        self.assertFalse(self.api.prs[0]["draft"])
        # The Gitea backup follows every integration.
        self.assertEqual(git(self.mirror, "rev-parse", "main"), git(self.remote, "rev-parse", "main"))
        self.assertEqual(state["mirror"]["remote"], module.MIRROR_REMOTE)
        self.assertTrue(state["mirror"]["pushed"])
        self.assertIsNone(state["mirror"]["error"])
        self.assertEqual(git(self.primary, "ls-remote", str(self.mirror), "refs/heads/codex/*"), "")
        self.flow.cleanup("task")
        self.assertFalse(self.worktree.exists())
        self.assertEqual(git(self.primary, "ls-remote", "origin", "refs/heads/" + self.branch), "")
        self.assertEqual(self.flow.load("task")["phase"], "completed")
        self.flow.cleanup("task")  # retry a completed cleanup

    def test_recover_lost_pr_create_response(self):
        self.start()
        self.api.lose_create_response = True
        with self.assertRaises(Error):
            self.checkpoint()
        self.flow.checkpoint("task")
        self.assertEqual(len(self.api.prs), 1)
        self.assertEqual(self.flow.load("task")["pr"], 1)

    def test_upstream_invalidates_validation_then_reconciles(self):
        self.ready()
        self.advance_remote()
        with self.assertRaisesRegex(Error, "differ from validation"):
            self.flow.finish("task", "description")
        self.flow.reconcile("task")
        self.assertNotIn("verified", self.flow.load("task"))
        self.flow.verify("task", [[sys.executable, "-c", "pass"]])
        self.flow.finish("task", "description")

    def test_server_closes_remote_advance_race(self):
        self.ready()
        self.api.before_merge = self.advance_remote
        with self.assertRaisesRegex(Error, "server rejected"):
            self.flow.finish("task", "description")
        self.assertFalse(self.api.prs[0]["merged"])

    def test_new_commit_and_dirty_files_refuse_merge(self):
        self.ready()
        (self.worktree / "feature.txt").write_text("unfinished\n")
        with self.assertRaisesRegex(Error, "Uncommitted"):
            self.flow.finish("task", "description")
        self.flow.checkpoint("task", "new checkpoint", ["feature.txt"])
        with self.assertRaisesRegex(Error, "differ from validation"):
            self.flow.finish("task", "description")

    def test_failed_checks_never_ready(self):
        self.start()
        self.checkpoint()
        with self.assertRaisesRegex(Error, "Validation failed"):
            self.flow.verify("task", [[sys.executable, "-c", "raise SystemExit(1)"]])
        self.assertNotIn("verified", self.flow.load("task"))
        self.assertEqual(self.flow.load("task")["phase"], "blocked")
        with self.assertRaises(Error):
            self.flow.verify("task", [])

    def test_pending_server_checks_and_pr_only(self):
        self.ready()
        self.flow.finish("task", "description", pr_only=True)
        self.assertIsNone(self.api.merge_payload)
        self.assertFalse(self.api.prs[0]["draft"])
        self.api.reject_merge = True
        with self.assertRaisesRegex(Error, "server rejected"):
            self.flow.finish("task", "description")
        self.assertFalse(self.api.prs[0]["merged"])

    def test_sync_overlap_and_divergent_main_preserved(self):
        self.start()
        self.checkpoint("feature changed base\n", "base.txt")
        self.flow.protection(True)
        self.flow.verify("task", [[sys.executable, "-c", "pass"]])
        (self.primary / "base.txt").write_text("local edit\n")
        with self.assertRaisesRegex(Error, "local edits"):
            self.flow.finish("task", "description")
        self.assertEqual((self.primary / "base.txt").read_text(), "local edit\n")
        git(self.primary, "add", "base.txt")
        git(self.primary, "commit", "-m", "preserve unique local work")
        local = git(self.primary, "rev-parse", "HEAD")
        with self.assertRaisesRegex(Error, "unique commits"):
            self.flow.sync("task")
        self.assertEqual(git(self.primary, "rev-parse", "HEAD"), local)
        self.assertEqual(git(self.primary, "rev-parse", "refs/gti-agent-workflow/recovery/task-main"), local)

    def test_cleanup_rejects_dirty_ignored_and_new_commits(self):
        self.ready()
        self.flow.finish("task", "description")
        secret = self.worktree / ".env"
        secret.write_text("local secret\n")
        with self.assertRaisesRegex(Error, "ignored files"):
            self.flow.cleanup("task")
        self.assertTrue(secret.is_file())
        secret.unlink()
        (self.worktree / "new.txt").write_text("preserve\n")
        with self.assertRaisesRegex(Error, "Uncommitted"):
            self.flow.cleanup("task")
        git(self.worktree, "add", "new.txt")
        git(self.worktree, "commit", "-m", "unmerged")
        with self.assertRaisesRegex(Error, "new commits"):
            self.flow.cleanup("task")
        self.assertTrue(self.worktree.exists())

    def test_abandon_preserves_history_and_refuses_cleanup(self):
        self.start()
        self.checkpoint()
        state = self.flow.abandon("task", "superseded")
        self.assertEqual(state["phase"], "abandoned")
        self.assertEqual(self.api.prs[0]["state"], "closed")
        self.assertTrue(git(self.primary, "ls-remote", "origin", "refs/heads/" + self.branch))
        self.assertTrue(self.worktree.exists())
        with self.assertRaises(Error):
            self.flow.cleanup("task")

    def test_abandon_retires_a_task_without_commits(self):
        self.start()
        self.advance_remote()
        state = self.flow.abandon("task", "superseded before any commit")
        self.assertEqual(state["phase"], "abandoned")
        self.assertEqual(state["reason"], "superseded before any commit")
        self.assertEqual(self.api.prs, [])
        self.assertFalse(self.worktree.exists())
        self.assertFalse(git(self.primary, "ls-remote", "origin", "refs/heads/" + self.branch))
        local = git(self.primary, "rev-parse", "--verify", "refs/heads/" + self.branch, check=False)
        self.assertNotEqual(local.returncode, 0)
        repeated = self.flow.abandon("task", "repeated")
        self.assertEqual(repeated["reason"], "superseded before any commit")

    def test_abandon_without_commits_refuses_unpreserved_files(self):
        self.start()
        (self.worktree / "notes.txt").write_text("draft\n")
        with self.assertRaisesRegex(Error, "Uncommitted changes"):
            self.flow.abandon("task", "superseded")
        (self.worktree / "notes.txt").unlink()
        (self.worktree / "build").mkdir()
        (self.worktree / "build" / "cache").write_text("generated\n")
        with self.assertRaisesRegex(Error, "ignored files"):
            self.flow.abandon("task", "superseded")
        self.assertEqual(self.flow.load("task")["phase"], "active")
        self.assertTrue(self.worktree.exists())
        self.assertTrue(git(self.primary, "ls-remote", "origin", "refs/heads/" + self.branch))
        self.assertEqual(self.api.prs, [])

    def test_abandon_publishes_unpublished_commits_behind_a_closed_pr(self):
        self.start()
        (self.worktree / "feature.txt").write_text("feature\n")
        git(self.worktree, "add", "feature.txt")
        git(self.worktree, "commit", "-m", "local only")
        state = self.flow.abandon("task", "superseded")
        self.assertEqual(state["phase"], "abandoned")
        self.assertEqual(len(self.api.prs), 1)
        self.assertEqual(self.api.prs[0]["state"], "closed")
        self.assertEqual(git(self.primary, "ls-remote", "origin", "refs/heads/" + self.branch).split()[0],
                         git(self.worktree, "rev-parse", "HEAD"))
        self.assertTrue(self.worktree.exists())

    def test_protection_preserves_reviews_and_statuses(self):
        self.api.rule = {"required_status_checks": {"strict": False, "contexts": ["Native toolchain"]},
                         "enforce_admins": {"enabled": False},
                         "required_pull_request_reviews": {"required_approving_review_count": 2,
                                                           "dismiss_stale_reviews": True},
                         "allow_force_pushes": {"enabled": True}, "allow_deletions": {"enabled": False}}
        result = self.flow.protection(True)
        self.assertEqual(result["required_pull_request_reviews"]["required_approving_review_count"], 2)
        self.assertTrue(result["required_pull_request_reviews"]["dismiss_stale_reviews"])
        self.assertEqual(result["required_status_checks"], {"strict": True, "contexts": ["Native toolchain"]})
        self.assertTrue(result["enforce_admins"]["enabled"])
        self.assertFalse(result["allow_force_pushes"]["enabled"])
        self.assertFalse(result["allow_deletions"]["enabled"])
        self.api.rule = None
        with self.assertRaisesRegex(Error, "protection --configure"):
            self.flow.protection()
        self.api.rule = {**self.flow.protection(True), "enforce_admins": {"enabled": False}}
        with self.assertRaisesRegex(Error, "protection --configure"):
            self.flow.protection()

    def test_sync_reports_a_missing_or_failing_mirror_without_blocking(self):
        git(self.primary, "remote", "remove", module.MIRROR_REMOTE)
        self.ready()
        state = self.flow.finish("task", "description")
        self.assertEqual(state["phase"], "synced")
        self.assertIsNone(state["mirror"]["remote"])
        self.assertFalse(state["mirror"]["pushed"])
        self.assertIn(module.MIRROR_REMOTE, state["mirror"]["error"])

    def test_legacy_gitea_record_steps_aside_for_adoption(self):
        old = self.root / "old"
        git(self.primary, "worktree", "add", "-b", "codex/old", str(old), "main")
        self.flow.file("old").write_text(json.dumps({
            "schema": 1, "task": "old", "title": "Old task", "branch": "codex/old",
            "worktree": str(old), "base": git(self.primary, "rev-parse", "main"),
            "phase": "active", "pr": 7, "pr_url": "https://git.example/pulls/7"}))
        with self.assertRaisesRegex(Error, "predates the GitHub move"):
            self.flow.load("old")
        records = {s["task"]: s for s in self.flow.status()}
        self.assertEqual(records["old"]["remote_state"], "legacy-gitea")
        self.assertEqual(self.api.prs, [])
        with self.assertRaisesRegex(Error, "start --adopt"):
            self.flow.start("old", "Old task", old, "codex/old")
        state = self.flow.start("old", "Old task", old, "codex/old", adopt=True)
        self.assertEqual(state["host"], "github")
        self.assertEqual(state["phase"], "active")
        self.assertTrue((self.flow.state_dir / "old.legacy-gitea.json").is_file())
        self.assertTrue(git(self.primary, "ls-remote", "origin", "refs/heads/codex/old"))

    def test_lock_ownership_and_scope_guards(self):
        with self.flow.lock():
            with self.assertRaisesRegex(Error, "holds"):
                with self.flow.lock():
                    self.fail("must not acquire a second lock")
        self.assertFalse((self.flow.state_dir / "lock").exists())
        with self.assertRaises(Error):
            self.flow.start("bad/name", "title", self.worktree)
        with self.assertRaises(Error):
            self.flow.start("task", "title", self.primary)
        self.start()
        with self.assertRaisesRegex(Error, "explicit"):
            self.flow.checkpoint("task", "broad", ["."])
        with self.assertRaisesRegex(Error, "canonical"):
            module.Workflow(self.primary, self.api)
        git(self.primary, "remote", "set-url", "origin", module.CANONICAL)
        with self.assertRaisesRegex(Error, "Gitea mirror"):
            module.Workflow(self.primary, self.api)
        git(self.primary, "remote", "set-url", module.MIRROR_REMOTE, module.MIRROR)
        module.Workflow(self.primary, self.api)

    def test_literal_pathspecs_do_not_commit_unrelated_files(self):
        self.start()
        (self.worktree / "wanted.txt").write_text("wanted")
        (self.worktree / "unrelated.txt").write_text("other task")
        with self.assertRaises(Error):
            self.flow.checkpoint("task", "unsafe broad path", [":/"])
        self.assertEqual(git(self.worktree, "diff", "--cached", "--name-only"), "")
        self.assertEqual(git(self.worktree, "rev-list", "--count", "origin/main..HEAD"), "0")

    def test_sync_never_overwrites_ignored_untracked_files(self):
        self.start()
        secret = self.primary / ".env"
        secret.write_text("preserve user secret")
        (self.worktree / ".env").write_text("incoming tracked file")
        git(self.worktree, "add", "-f", ".env")
        git(self.worktree, "commit", "-m", "incoming path")
        self.flow.checkpoint("task")
        self.flow.protection(True)
        self.flow.verify("task", [[sys.executable, "-c", "pass"]])
        with self.assertRaisesRegex(Error, "would be overwritten"):
            self.flow.finish("task", "description")
        self.assertEqual(secret.read_text(), "preserve user secret")
        self.assertEqual(self.flow.load("task")["phase"], "sync-blocked")

    def test_reconcile_never_mutates_integrated_branch(self):
        self.ready()
        self.flow.finish("task", "description")
        head = git(self.worktree, "rev-parse", "HEAD")
        with self.assertRaisesRegex(Error, "integrated"):
            self.flow.reconcile("task")
        self.assertEqual(git(self.worktree, "rev-parse", "HEAD"), head)
        self.assertEqual(self.flow.load("task")["phase"], "synced")

    def test_fork_pr_cannot_be_adopted_by_name(self):
        self.start()
        head = git(self.worktree, "rev-parse", "HEAD")
        self.api.prs.append({"number": 1, "state": "open", "draft": True,
                             "head": {"ref": self.branch, "sha": head,
                                      "repo": {"full_name": "someone/fork"}},
                             "base": {"ref": "main", "repo": {"full_name": module.REPOSITORY}}})
        self.checkpoint()
        self.assertEqual(self.flow.load("task")["pr"], 2)
        self.assertEqual(len(self.api.prs), 2)

    def test_recover_ready_pr_after_main_advancement(self):
        self.ready()
        self.flow.finish("task", "description", pr_only=True)
        self.flow.file("task").unlink()
        self.advance_remote()
        state = self.flow.start("task", "Task outcome", self.worktree, self.branch, adopt=True)
        self.assertEqual(state["pr"], 1)
        self.assertEqual(len(self.api.prs), 1)
        self.flow.reconcile("task")
        self.flow.verify("task", [[sys.executable, "-c", "pass"]])
        self.flow.finish("task", "description")

    def test_recover_merged_pr_without_recreating_branch(self):
        self.ready()
        self.flow.finish("task", "description")
        self.flow.file("task").unlink()
        state = self.flow.start("task", "Task outcome", self.worktree, self.branch, adopt=True)
        self.assertEqual(state["phase"], "synced")
        self.assertEqual(len(self.api.prs), 1)
        self.flow.cleanup("task")

    def test_recover_lost_abandon_response(self):
        self.start()
        self.checkpoint()
        self.api.lose_close_response = True
        with self.assertRaisesRegex(Error, "lost close"):
            self.flow.abandon("task", "superseded")
        self.assertEqual(self.flow.load("task")["phase"], "abandoning")
        state = self.flow.abandon("task", "superseded")
        self.assertEqual(state["phase"], "abandoned")
        self.assertTrue(self.worktree.exists())
        self.assertEqual(len(self.api.prs), 1)

    def test_deleted_directory_never_stages_other_task_deletions(self):
        self.start()
        old = self.worktree / "old"
        old.mkdir()
        (old / "ours.txt").write_text("ours")
        (old / "other.txt").write_text("other task")
        git(self.worktree, "add", "old/ours.txt", "old/other.txt")
        git(self.worktree, "commit", "-m", "initial files")
        self.flow.checkpoint("task")
        (old / "ours.txt").unlink()
        (old / "other.txt").unlink()
        old.rmdir()
        head = git(self.worktree, "rev-parse", "HEAD")
        with self.assertRaisesRegex(Error, "one exact tracked file"):
            self.flow.checkpoint("task", "delete", ["old"])
        self.assertEqual(git(self.worktree, "rev-parse", "HEAD"), head)
        self.assertEqual(git(self.worktree, "diff", "--cached", "--name-only"), "")

    def probe(self, expression):
        """A check command that exits 0 only when `expression` holds while the checks run."""
        lock = str(self.flow.state_dir / "lock")
        suite = str(self.flow.window.directory / "suite")
        return [sys.executable, "-c",
                f"import glob, json, os, sys\nlock = {lock!r}\nsuite = {suite!r}\n"
                f"holders = [json.load(open(p)) for p in glob.glob(suite + '/*.json')]\n"
                f"sys.exit(0 if ({expression}) else 3)"]

    def test_verify_releases_lock_and_holds_one_suite_slot(self):
        self.start()
        self.checkpoint()
        state = self.flow.verify("task", [self.probe(
            "not os.path.exists(lock) and len(holders) == 1 and holders[0]['pid'] == os.getppid()"
            " and holders[0]['kind'] == 'suite' and holders[0]['checks'][0][0] == sys.executable")])
        self.assertEqual(state["phase"], "ready")
        self.assertEqual(self.flow.window.holders("suite"), [])
        self.assertFalse((self.flow.state_dir / "lock").exists())
        self.assertEqual(self.window_messages, [])
        with self.assertRaisesRegex(Error, "Validation failed"):
            self.flow.verify("task", [self.probe("os.path.exists(lock)")])
        self.assertEqual(self.flow.load("task")["phase"], "blocked")
        self.assertEqual(self.flow.window.holders("suite"), [])

    def test_verify_waits_for_a_live_suite_holder_and_reclaims_an_exited_one(self):
        self.start()
        self.checkpoint()
        exited = subprocess.Popen([sys.executable, "-c", "pass"])
        exited.wait()
        suite = self.flow.window.directory / "suite"
        suite.mkdir(parents=True)
        (suite / "stale.json").write_text(json.dumps({"pid": exited.pid, "time": "earlier",
                                                      "kind": "suite", "command": ["ctest"]}))
        self.flow.verify("task", [self.probe("len(holders) == 1")])
        self.assertFalse((suite / "stale.json").exists())
        self.assertTrue(any(m.startswith("reclaimed suite slot") for m in self.window_messages))
        self.flow.checkpoint("task")  # drops the verification and returns to active
        self.assertNotIn("verified", self.flow.load("task"))
        with self.flow.window.acquire("suite", ["ctest", "--test-dir", "build-release"]):
            self.flow.window.timeout = 0.2
            with self.assertRaisesRegex(Error, "timed out .*ctest --test-dir build-release"):
                self.flow.verify("task", [[sys.executable, "-c", "pass"]])
            self.assertEqual(self.flow.load("task")["phase"], "active")
            self.assertFalse((self.flow.state_dir / "lock").exists())
        self.flow.window.timeout = None
        self.flow.verify("task", [[sys.executable, "-c", "pass"]])
        self.assertEqual(self.flow.load("task")["phase"], "ready")

    def test_remote_verify_uses_remote_window_and_binds_retrieved_evidence(self):
        self.start()
        self.checkpoint()
        real_runner = module.load_remote_runner()
        steps = [{"name": "fixture", "kind": "suite", "cwd": "source", "argv": ["python3", "fixture.py"]}]
        head = git(self.worktree, "rev-parse", "HEAD")
        scenario = {"head": head, "code": 0, "final_status": ""}

        def run(arguments):
            self.assertFalse((self.flow.state_dir / "lock").exists())
            self.assertEqual(len(self.flow.window.holders("suite")), 1)  # no second laptop slot
            self.assertEqual(arguments[arguments.index("--repo") + 1], str(self.worktree))
            plan = Path(arguments[arguments.index("--steps-file") + 1])
            self.assertEqual(json.loads(plan.read_text()), steps)
            destination = Path(arguments[arguments.index("--results") + 1])
            destination.mkdir(parents=True)
            (destination / "report.json").write_text(json.dumps({
                "id": "run-" + "a" * 32, "state": "passed", "exit_code": 0,
                "final_source": {"head": head, "status": scenario["final_status"]},
                "source": {"head": scenario["head"], "status": "", "snapshot_sha256": "digest"}}))
            return scenario["code"]

        fake = types.SimpleNamespace(validate_steps=real_runner.validate_steps,
                                     RunnerError=real_runner.RunnerError, main=run)
        with self.flow.window.acquire("suite", ["unrelated local suite"]):
            self.flow.window.timeout = 0
            with unittest.mock.patch.object(module, "load_remote_runner", return_value=fake):
                state = self.flow.verify("task", steps, remote=True)
                self.assertEqual(state["phase"], "ready")
                self.assertEqual(state["verified"]["head"], head)
                self.assertEqual(state["verified"]["remote"]["snapshot_sha256"], "digest")
                scenario["head"] = "wrong head"
                with self.assertRaisesRegex(Error, "clean published head"):
                    self.flow.verify("task", steps, remote=True)
                self.assertEqual(self.flow.load("task")["phase"], "blocked")
                self.assertNotIn("verified", self.flow.load("task"))
                scenario["head"] = head
                scenario["final_status"] = " M base.txt\n"
                with self.assertRaisesRegex(Error, "clean published head"):
                    self.flow.verify("task", steps, remote=True)
                scenario["final_status"] = ""
                scenario["code"] = 7
                with self.assertRaisesRegex(Error, "Remote validation failed"):
                    self.flow.verify("task", steps, remote=True)
                self.assertEqual(self.flow.load("task")["phase"], "blocked")

    def test_metadata_commands_run_while_verify_executes_checks(self):
        self.start()
        self.checkpoint()
        other = self.root / "other"
        self.flow.start("other", "Other task", other, "codex/other")
        wait = (self.flow.state_dir / "wait-here")
        checks = [[sys.executable, "-c",
                   f"import os, time\nwhile not os.path.exists({str(wait)!r}): time.sleep(0.02)"]]
        result = {}
        thread = threading.Thread(target=lambda: result.update(state=self.flow.verify("task", checks)))
        thread.start()
        try:
            deadline = time.monotonic() + 10
            while not self.flow.window.holders("suite") and time.monotonic() < deadline:
                time.sleep(0.02)
            self.assertEqual(len(self.flow.window.holders("suite")), 1)
            self.assertEqual(self.flow.load("task")["phase"], "verifying")
            records = {s["task"]: s for s in self.flow.status()}
            self.assertEqual(records["task"]["phase"], "verifying")
            (other / "other.txt").write_text("peer\n")
            self.flow.checkpoint("other", "peer checkpoint", ["other.txt"])
            self.assertTrue(self.api.prs[-1]["head"]["ref"] == "codex/other")
        finally:
            wait.write_text("go\n")
            thread.join(timeout=20)
        self.assertEqual(result["state"]["phase"], "ready")
        self.assertEqual(self.flow.window.holders("suite"), [])

    def test_lock_waits_briefly_for_a_short_holder(self):
        self.flow.lock_wait = 5
        released = []

        def hold():
            with self.flow.lock():
                time.sleep(0.4)
                released.append(time.monotonic())

        thread = threading.Thread(target=hold)
        thread.start()
        time.sleep(0.1)
        with self.flow.lock():
            self.assertGreaterEqual(time.monotonic(), released[0])
        thread.join()

    def test_closed_remote_branch_never_reused(self):
        self.ready()
        self.flow.finish("task", "description")
        old = git(self.remote, "rev-parse", self.branch)
        git(self.primary, "worktree", "remove", str(self.worktree))
        git(self.primary, "branch", "-d", self.branch)
        self.flow.file("task").unlink()
        with self.assertRaisesRegex(Error, "already uses"):
            self.flow.start("new-task", "new task", self.worktree, self.branch)
        self.assertEqual(git(self.remote, "rev-parse", self.branch), old)
        self.assertFalse(self.worktree.exists())


def batch_plan(**changes):
    return {"tier": "T1", "rationale": "A confined source diagnostic recovery fix",
            "regressions": ["regression"], "owning": ["owning"],
            "cross_phase": ["editor"], "targets": ["owning_tests"],
            "reproduction": {"exception": "The historical failing compiler is unavailable; kernel and original CTest evidence are retained"},
            "platforms": [sys.platform], **changes}


class ValidationChecks(unittest.TestCase):
    def test_risk_requires_actual_scope_and_evidence(self):
        validation.validate_plan(batch_plan())
        for plan in (batch_plan(tier="ICE"), batch_plan(rationale=""),
                     batch_plan(regressions=[]), batch_plan(owning=[]), batch_plan(targets=[]),
                     batch_plan(reproduction={}), batch_plan(tier="T2"),
                     batch_plan(cross_phase=[], unaffected=""), batch_plan(platforms=["any"]),
                     batch_plan(additional_checks=["ctest -R foo"])):
            with self.assertRaises(ValueError):
                validation.validate_plan(plan)
        with self.assertRaisesRegex(ValueError, "at least T2"):
            validation.validate_plan(batch_plan(), ["src/compiler/mir_program_verifier.cpp"])
        validation.validate_plan(batch_plan(tier="T2", broader=["lifecycle"]),
                                 ["src/compiler/mir_program_verifier.cpp"])

    def test_exact_required_inventory_and_disabled_tests(self):
        document = {"tests": [{"name": "owning"}, {"name": "regression"}]}
        self.assertEqual(validation.inventory(document, {"regression"}), {"owning", "regression"})
        for bad in ({"tests": []}, {"tests": [{"name": "owning"}]},
                    {"tests": [{"name": "regression"}, {"name": "regression"}]},
                    {"tests": [{"name": "regression", "properties": [{"name": "DISABLED", "value": True}]}]}):
            with self.assertRaises(ValueError):
                validation.inventory(bad, {"regression"})

    def test_complete_result_inventory_never_silently_skips_required_checks(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "results.xml"
            path.write_text('<testsuite><testcase name="regression"/><testcase name="platform"><skipped/></testcase></testsuite>')
            self.assertEqual(validation.results(path, {"regression", "platform"}, {"regression"}), ["platform"])
            with self.assertRaises(ValueError):
                validation.results(path, {"regression", "platform"}, {"platform"})
            for cases in ('<testcase name="regression"><failure/></testcase>',
                          '<testcase name="other"/>', '',
                          '<testcase name="regression"/><testcase name="regression"/>'):
                path.write_text('<testsuite>' + cases + '</testsuite>')
                with self.assertRaises(ValueError):
                    validation.results(path, {"regression"}, {"regression"})

    def test_only_same_source_optimized_builds_are_admissible(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            build = root / "build"
            build.mkdir()
            cache = build / "CMakeCache.txt"
            for mode in validation.OPTIMIZED:
                cache.write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={root}\nCMAKE_BUILD_TYPE:STRING={mode}\n")
                self.assertEqual(validation.build_cache(root, build)["CMAKE_BUILD_TYPE"], mode)
            cache.write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={root}\nCMAKE_BUILD_TYPE:STRING=Debug\n")
            with self.assertRaises(ValueError):
                validation.build_cache(root, build)
            cache.write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={root / 'other'}\nCMAKE_BUILD_TYPE:STRING=Release\n")
            with self.assertRaises(ValueError):
                validation.build_cache(root, build)

    def test_ci_draft_and_component_can_never_be_merge_gates(self):
        successes = {name: {"result": "success"} for name in
                     ("validation-plan", "automation-tests", "formatting", "release-version", "tree-sitter",
                      "build-and-test", "compiler-sanitizers")}
        for draft in (True, False):
            body = validation.component_body("Reviewable feature description", {"head": "a" * 40})
            event = {"pull_request": {"draft": draft, "body": body}}
            self.assertEqual(validation.ci_mode(event, ["src/compiler/parser.cpp"]), "component")
            with self.assertRaises(ValueError):
                validation.admission("component", successes)
            self.assertEqual(validation.component_body(body), "Reviewable feature description")
        self.assertEqual(validation.ci_mode({"pull_request": {"draft": True}}, []), "draft")
        with self.assertRaises(ValueError):
            validation.admission("draft", successes)
        validation.admission("full", successes)
        for name in successes:
            failed = copy.deepcopy(successes)
            failed[name]["result"] = "skipped"
            with self.assertRaises(ValueError):
                validation.admission("full", failed)

    def test_ci_automation_scope_is_conservative_and_has_its_own_gates(self):
        event = {"pull_request": {"draft": False}}
        self.assertEqual(validation.ci_mode(event, ["scripts/validation_batch.py", "tests/agent_workflow_test.py", "docs/architecture/verification.md"]), "automation")
        for paths in ([], ["src/compiler/parser.cpp"], ["tests/parser_tests.cpp"],
                      ["runtime/include/gti/runtime.h"], ["CMakeLists.txt"],
                      [".github/workflows/release.yml"], [".gti-validation/batches/batch.json"]):
            self.assertEqual(validation.ci_mode(event, paths), "full")
        results = {name: {"result": "success"} for name in
                   ("validation-plan", "automation-tests", "formatting", "release-version", "tree-sitter")}
        validation.admission("automation", results)
        results["automation-tests"]["result"] = "failure"
        with self.assertRaises(ValueError):
            validation.admission("automation", results)


class BatchLifecycle(Lifecycle):
    # Inherit the existing real-Git fixture but do not duplicate all lifecycle
    # tests under a second class in the default suite.
    def fake_focused(self, command, **kwargs):
        root = Path(kwargs["cwd"])
        report_path = Path(command[command.index("--report") + 1])
        plan = json.loads(Path(command[command.index("--plan") + 1]).read_text())
        report_path.write_text(json.dumps({"head": git(root, "rev-parse", "HEAD"),
            "scope": "focused", "build_type": "Release", "tests": validation.plan_tests(plan), "skipped": []}))

    def component(self, name, file=None, content=None, plan=None):
        tree = self.root / name
        self.flow.start(name, name, tree, "codex/" + name)
        (tree / (file or (name + ".txt"))).write_text(content or name)
        self.flow.checkpoint(name, "fix " + name, [file or (name + ".txt")])
        batches = Batches(self.flow, self.fake_focused)
        batches.enroll(name, plan or batch_plan(), tree / "build")
        return tree

    def combined(self):
        self.component("one")
        self.component("two")
        return Batches(self.flow).prepare("combined", ["one", "two"], self.root / "combined")

    def bind_full(self, state, platform=None):
        self.flow.verify(state["task"], [[sys.executable, "-c", "pass"]])
        state = self.flow.load(state["task"])
        Batches(self.flow).accept_report(state["task"], state, {
            "head": state["verified"]["head"], "scope": "full", "build_type": "Release",
            "tests": validation.plan_tests(batch_plan()), "skipped": [], "platform": platform or sys.platform,
            "smoke": ["scalar-O0", "scalar-O3", "owners-O0", "owners-O3"]})
        return self.flow.load(state["task"])

    def test_batch_components_cannot_finish_directly(self):
        self.component("one")
        state = self.flow.load("one")
        self.assertEqual(state["phase"], "ready-for-integration")
        self.assertNotIn("verified", state)
        self.assertIn("gti-validation-component", self.flow.pull(state)["body"])
        self.flow.verify("one", [[sys.executable, "-c", "pass"]])
        with self.assertRaisesRegex(Error, "not directly mergeable"):
            self.flow.finish("one", "Do not bypass batch acceptance")
        self.assertFalse(self.flow.pull(state)["merged"])

    def test_batch_preserves_exact_heads_and_full_evidence_before_merge(self):
        state = self.combined()
        root = self.root / "combined"
        heads = [self.flow.load(name)["admission"]["head"] for name in ("one", "two")]
        self.assertTrue(all(self.flow.ancestor(head, git(root, "rev-parse", "HEAD")) for head in heads))
        with self.assertRaisesRegex(Error, "Full combined"):
            self.flow.finish("combined", "Must not merge focused-only batch")
        state = self.bind_full(state)
        self.flow.protection(True)
        self.flow.finish("combined", "Full combined tests passed")
        self.assertTrue(all(self.flow.ancestor(head, git(self.primary, "rev-parse", "HEAD")) for head in heads))
        self.assertEqual(self.flow.load("combined")["phase"], "synced")

    def test_batch_rejects_advanced_member_after_validation(self):
        self.bind_full(self.combined())
        tree = self.root / "one"
        (tree / "one.txt").write_text("new implementation")
        self.flow.checkpoint("one", "advance", ["one.txt"])
        with self.assertRaisesRegex(Error, "changed or closed"):
            self.flow.finish("combined", "old membership cannot merge")

    def test_batch_rejects_forged_manifest_and_missing_platform(self):
        state = self.combined()
        root = self.root / "combined"
        manifest_path = root / state["batch"]["manifest"]
        original = manifest_path.read_text()
        manifest_path.write_text(original.replace('"T1"', '"T2"'))
        with self.assertRaisesRegex(Error, "membership differs"):
            Batches(self.flow).validate_members(state)
        manifest_path.write_text(original)
        state = self.bind_full(state, platform="different-platform")
        with self.assertRaisesRegex(Error, "required platform"):
            Batches(self.flow).before_finish(state)

    def test_batch_rejects_partial_or_stale_full_reports(self):
        state = self.combined()
        self.flow.verify("combined", [[sys.executable, "-c", "pass"]])
        state = self.flow.load("combined")
        report = {"head": state["verified"]["head"], "scope": "full", "build_type": "Release",
                  "tests": validation.plan_tests(batch_plan()), "skipped": [], "platform": sys.platform,
                  "smoke": ["scalar-O0", "scalar-O3", "owners-O0", "owners-O3"]}
        for changes in ({"scope": "focused"}, {"head": "a" * 40}, {"tests": []},
                        {"skipped": ["regression"]}, {"build_type": "Debug"}, {"smoke": []}):
            with self.assertRaisesRegex(Error, "Full batch evidence"):
                Batches(self.flow).accept_report("combined", state, {**report, **changes})

    def test_batch_selection_is_bounded_and_serialized(self):
        self.component("one")
        self.component("two")
        batches = Batches(self.flow)
        for names in ([], ["one", "one"], ["x" + str(number) for number in range(9)]):
            with self.assertRaisesRegex(Error, "one to eight"):
                batches.prepare("combined", names, self.root / "combined")
        batches.prepare("combined", ["one"], self.root / "combined")
        with self.assertRaisesRegex(Error, "Another integration batch"):
            batches.prepare("second", ["two"], self.root / "second")
        with self.assertRaisesRegex(Error, "Release"):
            batches.withdraw("one", "try to escape a selected batch")

    def test_batch_conflict_preserves_source_and_can_resume(self):
        self.component("one", file="shared.txt", content="first")
        self.component("two", file="shared.txt", content="second")
        batches = Batches(self.flow)
        with self.assertRaisesRegex(Error, "Resolve and commit two"):
            batches.prepare("combined", ["one", "two"], self.root / "combined")
        root = self.root / "combined"
        self.assertTrue((root / "shared.txt").exists())
        self.assertEqual(self.flow.load("combined")["phase"], "batch-conflict")
        (root / "shared.txt").write_text("reviewed combination")
        git(root, "add", "shared.txt")
        git(root, "commit", "-m", "resolve owning interaction")
        state = batches.resume("combined")
        self.assertTrue((root / state["batch"]["manifest"]).exists())

    def test_batch_withdrawal_and_release_preserve_published_work(self):
        self.component("one")
        batches = Batches(self.flow)
        batches.withdraw("one", "standalone validation selected")
        state = self.flow.load("one")
        self.assertNotIn("admission", state)
        self.assertNotIn("gti-validation-component", self.flow.pull(state)["body"])
        Batches(self.flow, self.fake_focused).enroll("one", batch_plan(), self.root / "one/build")
        batches.prepare("combined", ["one"], self.root / "combined")
        batches.release("combined", "investigate a failed interaction in its owning patch")
        self.assertTrue((self.root / "combined").exists())
        self.assertNotIn("integration_batch", self.flow.load("one"))
        self.assertEqual(self.flow.load("one")["phase"], "ready-for-integration")

    def test_batch_gate_activation_preserves_existing_review_and_app_pins(self):
        self.api.rule = {"required_status_checks": {"strict": True, "contexts": ["existing"],
                         "checks": [{"context": "existing", "app_id": 42}]},
                         "required_pull_request_reviews": {"required_approving_review_count": 2}}
        result = self.flow.protection(True, batch_gate=True)
        self.assertEqual(result["required_status_checks"]["contexts"], ["existing", "Validation gate"])
        self.assertEqual(result["required_status_checks"]["checks"], [{"context": "existing", "app_id": 42}])
        self.assertEqual(result["required_pull_request_reviews"]["required_approving_review_count"], 2)


if __name__ == "__main__":
    # BatchLifecycle reuses fixture helpers, not the parent's full test list.
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(Lifecycle)
    suite.addTests(unittest.defaultTestLoader.loadTestsFromTestCase(ValidationChecks))
    suite.addTests(BatchLifecycle(name) for name in sorted(BatchLifecycle.__dict__) if name.startswith("test_batch_"))
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    raise SystemExit(not result.wasSuccessful())
