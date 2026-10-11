#!/usr/bin/env python3
"""Exercise task lifecycle with real disposable Git repositories and a fake Gitea."""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import unittest.mock
from harness_timeouts import scaled

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("agent_workflow", ROOT / "scripts/agent_workflow.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
git = module.git
Error = module.WorkflowError


class Gitea:
    def __init__(self, remote):
        self.remote = remote
        self.prs = []
        self.rules = []
        self.merge_payload = None
        self.reject_merge = False
        self.lose_create_response = False
        self.lose_close_response = False
        self.before_merge = None

    def request(self, method, path, payload=None):
        if path.startswith("/branch_protections"):
            if method == "GET":
                return self.rules
            self.rules = [{**(self.rules[0] if self.rules else {}), **payload}]
            return self.rules[0]
        if path.startswith("/pulls?"):
            return self.prs
        if path == "/pulls":
            head = git(self.remote, "rev-parse", "refs/heads/" + payload["head"])
            pr = {**payload, "number": len(self.prs) + 1,
                  "html_url": "https://example.invalid/pulls/1", "state": "open",
                  "draft": True, "merged": False,
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
            if "title" in payload:
                pr["draft"] = payload["title"].startswith("WIP:")
            if self.lose_close_response and payload.get("state") == "closed":
                self.lose_close_response = False
                raise Error("simulated lost close response")
            return pr
        if path.endswith("/merge"):
            self.merge_payload = payload
            if self.before_merge:
                self.before_merge()
            base = git(self.remote, "rev-parse", "main")
            head = pr["head"]["sha"]
            if self.reject_merge or payload["head_commit_id"] != head or git(self.remote, "merge-base", "--is-ancestor", base, head, check=False).returncode:
                raise Error("server rejected pending checks or outdated branch")
            tree = git(self.remote, "rev-parse", head + "^{tree}")
            commit = git(self.remote, "commit-tree", tree, "-p", base, "-p", head, "-m", "merge task")
            git(self.remote, "update-ref", "refs/heads/main", commit)
            pr.update(merged=True, state="closed", merge_commit_sha=commit)
            return None
        raise AssertionError((method, path, payload))


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
        self.api = Gitea(self.remote)
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
        self.assertFalse(self.api.merge_payload["force_merge"])
        self.assertEqual(self.api.merge_payload["head_commit_id"], state["verified"]["head"])
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

    def test_protection_preserves_reviews_and_statuses(self):
        self.api.rules = [{"rule_name": "main", "required_approvals": 2,
                           "enable_status_check": True, "status_check_contexts": ["Native toolchain"]}]
        result = self.flow.protection(True)
        self.assertEqual(result["required_approvals"], 2)
        self.assertEqual(result["status_check_contexts"], ["Native toolchain"])
        self.assertTrue(result["enable_status_check"])
        self.assertFalse(result["enable_push"])
        self.assertTrue(result["block_on_outdated_branch"])

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
            thread.join(timeout=scaled(20))
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


if __name__ == "__main__":
    unittest.main()
