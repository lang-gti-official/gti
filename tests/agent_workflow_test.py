#!/usr/bin/env python3
"""Exercise the GitHub task lifecycle with real disposable Git repositories and no automatic backup pushes."""

import contextlib
import copy
import io
import json
import os
from pathlib import Path
import re
import shutil
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
    """The GitHub REST/GraphQL surface: real drafts, head-filtered lookup, rulesets and a merge queue."""

    def __init__(self, remote):
        self.remote = remote
        self.prs = []
        self.rulesets = []
        self.enqueued = []
        self.reject_enqueue = False
        self.queue_async = False
        self.lose_create_response = False
        self.lose_close_response = False
        self.before_enqueue = None
        self.ready_calls = []
        self.pending_checks = False
        self.auto_calls = []
        self.lose_auto_response = False
        self.reject_disable = False

    def effective_rules(self):
        return [rule for ruleset in self.rulesets if ruleset["enforcement"] == "active"
                and "refs/heads/main" in ruleset["conditions"]["ref_name"]["include"]
                for rule in ruleset["rules"]]

    def request(self, method, path, payload=None):
        if path.startswith("/rulesets"):
            if path.startswith("/rulesets?") and method == "GET":
                return [{k: v for k, v in r.items() if k not in {"rules", "bypass_actors"}} for r in self.rulesets]
            if path == "/rulesets" and method == "POST":
                ruleset = {**copy.deepcopy(payload), "id": len(self.rulesets) + 1}
                self.rulesets.append(ruleset)
                return copy.deepcopy(ruleset)
            ruleset = next(r for r in self.rulesets if r["id"] == int(path.split("/")[2]))
            if method == "GET":
                return copy.deepcopy(ruleset)
            if method == "PUT":
                ruleset.clear()
                ruleset.update({**copy.deepcopy(payload), "id": int(path.split("/")[2])})
                return copy.deepcopy(ruleset)
            raise AssertionError((method, path))
        if path == "/rules/branches/main":
            return copy.deepcopy(self.effective_rules())
        if path.startswith("/pulls?"):
            query = module.urllib.parse.parse_qs(module.urllib.parse.urlsplit(path).query)
            if query.get("state") != ["all"] or query.get("per_page") != ["100"] or "page" in query:
                raise AssertionError(path)
            owner, _, branch = query["head"][0].partition(":")
            # The list view carries merged_at, never `merged`; GitHub filters by owner:branch.
            return [{k: v for k, v in pr.items() if k != "merged"} for pr in self.prs
                    if pr["head"]["ref"] == branch and pr["head"]["repo"]["full_name"].split("/")[0] == owner]
        if path == "/pulls":
            head = git(self.remote, "rev-parse", "refs/heads/" + payload["head"])
            if git(self.remote, "rev-list", "--count", "main.." + head) == "0":
                # A PR needs commits beyond the base.
                raise Error("Validation Failed: No commits between main and " + payload["head"])
            number = len(self.prs) + 1
            pr = {**payload, "number": number, "node_id": f"PR_{number}",
                  "html_url": f"https://example.invalid/pull/{number}", "state": "open",
                  "draft": bool(payload.get("draft")), "merged": False, "merge_commit_sha": None,
                  "labels": [], "comments": [],
                  "head": {"ref": payload["head"], "sha": head,
                           "repo": {"full_name": module.REPOSITORY}},
                  "base": {"ref": "main", "repo": {"full_name": module.REPOSITORY}}}
            self.prs.append(pr)
            if self.lose_create_response:
                self.lose_create_response = False
                raise Error("simulated lost create response")
            return pr
        pr = self.pr(int(path.split("/")[2]))
        if path.startswith("/issues/"):
            return self.issue(pr, method, path, payload)
        if method == "GET":
            return pr.copy()
        if method == "PATCH":
            pr.update(payload)
            if self.lose_close_response and payload.get("state") == "closed":
                self.lose_close_response = False
                raise Error("simulated lost close response")
            return pr
        raise AssertionError((method, path, payload))

    def issue(self, pr, method, path, payload):
        """Labels and comments go through the issues API for PRs."""
        rest = path.split("/", 3)[3]
        if rest == "labels" and method == "POST":
            for name in payload["labels"]:
                if name not in {label["name"] for label in pr["labels"]}:
                    pr["labels"].append({"name": name})
            return pr["labels"]
        if rest.startswith("labels/") and method == "DELETE":
            name = module.urllib.parse.unquote(rest.split("/", 1)[1])
            pr["labels"] = [label for label in pr["labels"] if label["name"] != name]
            return pr["labels"]
        if rest.startswith("comments") and method == "GET":
            return copy.deepcopy(pr["comments"])
        if rest == "comments" and method == "POST":
            pr["comments"].append({"body": payload["body"]})
            return pr["comments"][-1]
        raise AssertionError((method, path, payload))

    def pr(self, number):
        pr = next(pr for pr in self.prs if pr["number"] == number)
        if not pr["merged"]:
            current = git(self.remote, "rev-parse", "--verify", "refs/heads/" + pr["head"]["ref"], check=False)
            if current.returncode == 0:
                pr["head"]["sha"] = current.stdout.strip()
        return pr

    def graphql(self, query, variables):
        pr = self.pr(int(variables["id"].split("_")[1]))
        if "node(id:" in query:
            return {"node": {"headRefOid": pr["head"]["sha"], "merged": pr["merged"],
                             "isInMergeQueue": bool(pr.get("in_queue")),
                             "autoMergeRequest": pr.get("auto_merge")}}
        if "disablePullRequestAutoMerge" in query:
            if self.reject_disable:
                raise Error("disable refused")
            pr["auto_merge"] = None
            return {"disablePullRequestAutoMerge": {"clientMutationId": None}}
        if "dequeuePullRequest" in query:
            pr["in_queue"] = False
            return {"dequeuePullRequest": {"clientMutationId": None}}
        if "markPullRequestReadyForReview" in query:
            self.ready_calls.append(pr["number"])
            pr["draft"] = False
            return {"markPullRequestReadyForReview": {"clientMutationId": None}}
        if "enablePullRequestAutoMerge" in query:
            if self.reject_enqueue or variables["head"] != pr["head"]["sha"]:
                raise Error("server rejected the merge queue entry")
            self.auto_calls.append(variables["head"])
            pr["auto_merge"] = {"enabledAt": "now"}
            if self.lose_auto_response:
                self.lose_auto_response = False
                raise Error("lost auto-merge response")
            if self.pending_checks:
                return {"enablePullRequestAutoMerge": {"clientMutationId": None}}
        elif "enqueuePullRequest" not in query:
            raise AssertionError(query)
        elif self.pending_checks:
            raise Error('Required status check "CI gate" is expected')
        if (self.reject_enqueue or pr["draft"] or variables["head"] != pr["head"]["sha"]
                or not {"merge_queue", "required_status_checks"} <= {r["type"] for r in self.effective_rules()}):
            raise Error("server rejected the merge queue entry")
        self.enqueued.append((pr["number"], variables["head"]))
        pr["in_queue"] = True
        if self.before_enqueue:
            self.before_enqueue()
        if not self.queue_async:
            self.merge_queue_head()
        return {"enqueuePullRequest": {"mergeQueueEntry": {"position": 1}}}

    def merge_queue_head(self):
        """Merge the queued candidate built on current main, as the queue does after CI."""
        number, head = self.enqueued[-1]
        pr = self.pr(number)
        base = git(self.remote, "rev-parse", "main")
        tree = git(self.remote, "merge-tree", "--write-tree", base, head).splitlines()[0]
        commit = git(self.remote, "commit-tree", tree, "-p", base, "-p", head, "-m", "Merge queue candidate")
        git(self.remote, "update-ref", "refs/heads/main", commit)
        pr.update(merged=True, state="closed", merge_commit_sha=commit, in_queue=False, auto_merge=None)


class DeclaredTests(unittest.TestCase):
    def test_validation_block_names_only_tests(self):
        body = ("Fixes it.\n\nValidation:\n- tier: T2 (MIR cleanup)\n- T0: mir_regression\n"
                "- T1: mir_full_expression, optimizer_conformance,\n      compiler_pipeline\n"
                "- reproduction: fails on parent (GTI-B0003), passes on patch\n- T3: deferred\n\nProse.")
        self.assertEqual(validation.declared_tests(body),
                         ["compiler_pipeline", "mir_full_expression", "mir_regression", "optimizer_conformance"])
        for bad in ("", "Validation:\n- tier: T1", "Validation:\n- T0: a\nValidation:\n- T1: b",
                    "Validation:\n- T0: name with spaces", "Validation:\n- T1: a;b", "Validation:\n- T0: $(id)"):
            with self.subTest(bad), self.assertRaises(ValueError):
                validation.declared_tests(bad)
        many = "Validation:\n- T1: " + ", ".join(f"t{i}" for i in range(validation.DECLARED_LIMIT + 1))
        with self.assertRaisesRegex(ValueError, "at most"):
            validation.declared_tests(many)

    def test_coverage_reads_wrapped_ctest_patterns_and_focused_plans(self):
        with tempfile.TemporaryDirectory() as directory:
            plan = {"tier": "T1", "rationale": "local", "regressions": ["planned_regression"],
                    "owning": ["planned_owner"], "targets": ["gti"], "platforms": ["darwin"],
                    "unaffected": "frontend only", "reproduction": {"exception": "new feature"}}
            Path(directory, "plan.json").write_text(json.dumps(plan))
            checks = [["python3", "scripts/build_window.py", "--kind", "suite", "--",
                       "ctest", "--test-dir", "build", "-R", "^llvm_(scalar|class)_execution$"],
                      {"name": "remote", "kind": "suite", "cwd": "build", "argv": ["ctest", "--tests-regex", "^lsp_protocol$"]},
                      ["python3", "scripts/validation_checks.py", "focused", "--build", "build", "--plan", "plan.json"],
                      ["python3", "-c", "print('-R not_a_ctest_pattern')"]]
            names = ["llvm_scalar_execution", "llvm_class_execution", "lsp_protocol", "planned_owner",
                     "compiler_pipeline", "llvm_place_execution", "not_a_ctest_pattern"]
            self.assertEqual(validation.uncovered_declarations(names, checks, directory),
                             ["llvm_place_execution", "not_a_ctest_pattern"])


class GitHubClient(unittest.TestCase):
    def test_credentials_come_from_the_environment_or_the_github_cli(self):
        completed = types.SimpleNamespace(returncode=0, stdout="cli-token\n")
        with unittest.mock.patch.dict(os.environ, {}, clear=True), \
                unittest.mock.patch.object(module.subprocess, "run", return_value=completed) as run:
            self.assertEqual(module.GitHub().token, "cli-token")
            self.assertEqual(run.call_args.args[0][:3], ["gh", "auth", "token"])
        failed = types.SimpleNamespace(returncode=1, stdout="")
        with unittest.mock.patch.dict(os.environ, {}, clear=True), \
                unittest.mock.patch.object(module.subprocess, "run", return_value=failed):
            with self.assertRaisesRegex(Error, "gh auth login"):
                module.GitHub()
        with unittest.mock.patch.dict(os.environ, {"GH_TOKEN": "preferred"}, clear=True), \
                unittest.mock.patch.object(module.subprocess, "run") as run:
            self.assertEqual(module.GitHub().token, "preferred")
            run.assert_not_called()

    def test_requests_pin_github_auth_host_and_refuse_redirects(self):
        with unittest.mock.patch.dict(os.environ, {"GH_TOKEN": "secret"}, clear=True):
            client = module.GitHub()
        opener = unittest.mock.MagicMock()
        opener.open.return_value.__enter__.return_value.read.return_value = b'{"number": 1}'
        with unittest.mock.patch.object(module.urllib.request, "build_opener", return_value=opener) as build:
            self.assertEqual(client.request("GET", "/pulls/1"), {"number": 1})
        operation = opener.open.call_args.args[0]
        self.assertEqual(operation.full_url, module.API + "/pulls/1")
        self.assertEqual(operation.get_header("Authorization"), "Bearer secret")
        self.assertIs(build.call_args.args[0], module.NoRedirect)
        self.assertIsNone(module.NoRedirect().redirect_request(None, None, 302, "", {}, "https://example.invalid"))
        with self.assertRaises(TypeError):
            client.request("GET", "/pulls/1", url="https://example.invalid")

    def test_errors_redact_server_details_and_graphql_refusals_fail(self):
        with unittest.mock.patch.dict(os.environ, {"GH_TOKEN": "never-print-me"}, clear=True):
            client = module.GitHub()
        opener = unittest.mock.MagicMock()
        opener.open.side_effect = module.urllib.error.HTTPError(module.API, 404, "never-print-me", {}, None)
        with unittest.mock.patch.object(module.urllib.request, "build_opener", return_value=opener):
            with self.assertRaises(Error) as caught:
                client.request("GET", "/pulls/1")
            self.assertNotIn("never-print-me", str(caught.exception))
        opener = unittest.mock.MagicMock()
        opener.open.return_value.__enter__.return_value.read.return_value = b'{"errors": [{"message": "queue off"}]}'
        with unittest.mock.patch.object(module.urllib.request, "build_opener", return_value=opener):
            with self.assertRaisesRegex(Error, "queue off"):
                client.graphql("query", {})
        self.assertEqual(opener.open.call_args.args[0].full_url, module.GRAPHQL)


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
        git(self.primary, "remote", "add", "github-backup", str(self.mirror))
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
        # Lifecycle tests use placeholder bodies; the declaration rule has its own tests.
        self.declarations = unittest.mock.patch.object(validation, "declared_tests", return_value=[])
        self.declarations.start()

    def tearDown(self):
        unittest.mock.patch.stopall()
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
        state = self.flow.finish("task", "Final implementation and passing tests", rollup=False)
        self.assertEqual(state["phase"], "synced")
        self.assertEqual((self.primary / "base.txt").read_text(), "user edit\n")
        self.assertTrue((self.primary / "feature.txt").is_file())
        # The merge queue, pinned to the validated head, performs the merge.
        self.assertEqual(self.api.enqueued, [(1, state["verified"]["head"])])
        self.assertEqual(self.api.ready_calls, [1])
        self.assertFalse(self.api.prs[0]["draft"])
        # A separately configured backup is never a hidden publication target.
        self.assertEqual(git(self.primary, "ls-remote", str(self.mirror)), "")
        self.assertNotIn("mirror", state)
        self.assertEqual(git(self.primary, "ls-remote", str(self.mirror), "refs/heads/codex/*"), "")
        self.flow.cleanup("task")
        self.assertFalse(self.worktree.exists())
        self.assertEqual(git(self.primary, "ls-remote", "origin", "refs/heads/" + self.branch), "")
        self.assertEqual(self.flow.load("task")["phase"], "completed")
        self.flow.cleanup("task")  # retry a completed cleanup

    def test_finish_claims_only_declared_tests_that_verify_ran(self):
        self.declarations.stop()
        ctest = self.root / "bin" / "ctest"
        ctest.parent.mkdir()
        ctest.write_text("#!/bin/sh\nexit 0\n")
        ctest.chmod(0o755)
        self.start()
        self.checkpoint()
        self.flow.protection(True)
        self.flow.verify("task", [[str(ctest), "--test-dir", "build", "-R", "^(alpha|beta)$"]])
        with self.assertRaisesRegex(Error, "Validation:"):
            self.flow.finish("task", "No declaration")
        with self.assertRaisesRegex(Error, "not part of the recorded validation: gamma"):
            self.flow.finish("task", "Fix.\n\nValidation:\n- T0: alpha\n- T1: beta, gamma\n")
        self.assertEqual(self.api.enqueued, [])
        state = self.flow.finish("task", "Fix.\n\nValidation:\n- T0: alpha\n- T1: beta\n- T2: deferred\n", rollup=False)
        self.assertEqual(state["phase"], "synced")

    def test_automation_only_changes_need_no_declaration(self):
        self.declarations.stop()
        self.start()
        (self.worktree / "docs").mkdir()
        self.checkpoint("notes\n", "docs/notes.md")
        self.flow.protection(True)
        self.flow.verify("task", [[sys.executable, "-c", "pass"]])
        self.assertEqual(self.flow.finish("task", "Documentation only", rollup=False)["phase"], "synced")

    def test_recover_lost_pr_create_response(self):
        self.start()
        self.api.lose_create_response = True
        with self.assertRaises(Error):
            self.checkpoint()
        self.flow.checkpoint("task")
        self.assertEqual(len(self.api.prs), 1)
        self.assertEqual(self.flow.load("task")["pr"], 1)

    def test_main_advance_does_not_void_focused_evidence(self):
        # The queue validates the combined candidate against current main, so
        # another task landing first neither voids nor requires reconciling this one.
        self.ready()
        self.advance_remote()
        advanced = git(self.remote, "rev-parse", "main")
        state = self.flow.finish("task", "description", rollup=False)
        self.assertEqual(state["phase"], "synced")
        self.assertTrue(self.flow.ancestor(advanced, git(self.primary, "rev-parse", "main")))

    def test_queue_builds_on_main_that_advances_while_queued(self):
        self.ready()
        self.api.before_enqueue = self.advance_remote
        state = self.flow.finish("task", "description", rollup=False)
        merge = state["merge_commit"]
        # The candidate's first parent is the main that advanced while queued.
        self.assertEqual(git(self.primary, "log", "-1", "--format=%s", merge + "^1"), "another task")
        self.assertEqual(git(self.primary, "rev-parse", merge + "^2"), state["verified"]["head"])

    def test_queued_task_waits_for_the_merge_then_syncs(self):
        self.ready()
        self.api.queue_async = True
        state = self.flow.finish("task", "description", rollup=False)
        self.assertEqual(state["phase"], "queued")
        self.assertFalse(self.api.prs[0]["merged"])
        with self.assertRaisesRegex(Error, "has not merged"):
            self.flow.sync("task")
        self.flow.finish("task", "description", rollup=False)  # retry does not enqueue twice
        self.assertEqual(len(self.api.enqueued), 1)
        self.api.merge_queue_head()
        self.assertEqual(self.flow.finish("task", "description", rollup=False)["phase"], "synced")

    def test_pending_ci_is_a_durable_handoff_not_an_enqueue_error(self):
        self.ready()
        self.api.pending_checks = True
        state = self.flow.finish("task", "description", rollup=False)
        self.assertEqual(state["phase"], "awaiting-ci")
        self.assertEqual(state["handoff"]["head"], state["verified"]["head"])
        self.assertEqual(self.api.enqueued, [])
        self.flow.finish("task", "description", rollup=False)
        self.assertEqual(len(self.api.auto_calls), 1)
        self.assertEqual(self.flow.status("task")[0]["remote_integration"], "awaiting-ci")
        # GitHub observes a passing check and queues it without another helper call.
        self.api.pending_checks = False
        self.api.queue_async = True
        self.api.graphql(module.Workflow.ENQUEUE, {"id": "PR_1", "head": state["verified"]["head"]})
        self.assertEqual(self.flow.status("task")[0]["remote_integration"], "queued")
        self.assertEqual(self.flow.finish("task", "description", rollup=False)["phase"], "queued")
        self.assertEqual(len(self.api.auto_calls), 1)

    def test_lost_auto_response_recovers_without_rearming(self):
        self.ready()
        self.api.pending_checks = True
        self.api.lose_auto_response = True
        with self.assertRaisesRegex(Error, "lost auto-merge"):
            self.flow.finish("task", "description", rollup=False)
        self.assertEqual(self.flow.finish("task", "description", rollup=False)["phase"], "awaiting-ci")
        self.assertEqual(len(self.api.auto_calls), 1)

    def test_checkpoint_cancels_handoff_before_publishing_a_new_head(self):
        self.ready()
        self.api.pending_checks = True
        self.flow.finish("task", "description", rollup=False)
        self.checkpoint("changed after handoff\n")
        self.assertIsNone(self.api.prs[0]["auto_merge"])
        self.assertNotIn("handoff", self.flow.load("task"))
        self.assertNotIn("verified", self.flow.load("task"))
        with self.assertRaisesRegex(Error, "recorded validation"):
            self.flow.finish("task", "description", rollup=False)

    def test_cancellation_failure_prevents_push(self):
        self.ready()
        self.api.pending_checks = True
        state = self.flow.finish("task", "description", rollup=False)
        self.api.reject_disable = True
        with self.assertRaisesRegex(Error, "disable refused"):
            self.checkpoint("changed after handoff\n")
        self.assertEqual(self.api.pr(1)["head"]["sha"], state["verified"]["head"])

    def test_reconcile_and_pr_only_withdraw_pending_intent(self):
        self.ready()
        self.api.pending_checks = True
        self.flow.finish("task", "description", rollup=False)
        self.flow.finish("task", "description", pr_only=True)
        self.assertIsNone(self.api.prs[0]["auto_merge"])
        self.flow.finish("task", "description", rollup=False)
        self.advance_remote()
        self.flow.reconcile("task")
        self.assertIsNone(self.api.prs[0]["auto_merge"])
        self.assertNotIn("verified", self.flow.load("task"))

    def test_new_checkpoint_withdraws_existing_queue_entry(self):
        self.ready()
        self.api.queue_async = True
        self.flow.finish("task", "description", rollup=False)
        self.checkpoint("new head after queue entry\n")
        self.assertFalse(self.api.prs[0]["in_queue"])
        self.assertIsNone(self.api.prs[0]["auto_merge"])
        self.assertNotIn("queued", self.flow.load("task"))

    def test_changed_head_cannot_arm_auto_merge(self):
        self.ready()
        self.api.pending_checks = True
        graphql = self.api.graphql
        def raced(query, variables):
            if "enablePullRequestAutoMerge" in query:
                variables = {**variables, "head": "0" * 40}
            return graphql(query, variables)
        self.api.graphql = raced
        with self.assertRaisesRegex(Error, "server rejected"):
            self.flow.finish("task", "description", rollup=False)
        self.assertEqual(self.api.auto_calls, [])

    def test_new_commit_and_dirty_files_refuse_merge(self):
        self.ready()
        (self.worktree / "feature.txt").write_text("unfinished\n")
        with self.assertRaisesRegex(Error, "Uncommitted"):
            self.flow.finish("task", "description", rollup=False)
        self.flow.checkpoint("task", "new checkpoint", ["feature.txt"])
        with self.assertRaisesRegex(Error, "differs from the recorded validation"):
            self.flow.finish("task", "description", rollup=False)

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
        self.assertEqual(self.api.enqueued, [])
        self.assertFalse(self.api.prs[0]["draft"])
        self.api.reject_enqueue = True
        with self.assertRaisesRegex(Error, "server rejected"):
            self.flow.finish("task", "description", rollup=False)
        self.assertFalse(self.api.prs[0]["merged"])

    def test_sync_overlap_and_divergent_main_preserved(self):
        self.start()
        self.checkpoint("feature changed base\n", "base.txt")
        self.flow.protection(True)
        self.flow.verify("task", [[sys.executable, "-c", "pass"]])
        (self.primary / "base.txt").write_text("local edit\n")
        with self.assertRaisesRegex(Error, "local edits"):
            self.flow.finish("task", "description", rollup=False)
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
        self.flow.finish("task", "description", rollup=False)
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

    def test_protection_requires_the_queue_gate_and_no_bypass(self):
        with self.assertRaisesRegex(Error, "protection --configure"):
            self.flow.protection()
        result = self.flow.protection(True)
        self.assertEqual(result["enforcement"], "active")
        self.assertEqual(result["bypass_actors"], [])
        self.assertEqual({rule["type"] for rule in result["rules"]}, module.Workflow.RULES)
        self.flow.protection()
        self.assertEqual(len(self.api.rulesets), 1)
        self.flow.protection(True)  # configure updates, never duplicates
        self.assertEqual(len(self.api.rulesets), 1)
        for forge in ("bypass", "disabled", "no-queue", "other-check"):
            ruleset = self.api.rulesets[0]
            saved = copy.deepcopy(ruleset)
            if forge == "bypass":
                ruleset["bypass_actors"] = [{"actor_type": "RepositoryRole", "actor_id": 5}]
            elif forge == "disabled":
                ruleset["enforcement"] = "evaluate"
            elif forge == "no-queue":
                ruleset["rules"] = [rule for rule in ruleset["rules"] if rule["type"] != "merge_queue"]
            else:
                for rule in ruleset["rules"]:
                    if rule["type"] == "required_status_checks":
                        rule["parameters"]["required_status_checks"] = [{"context": "something else"}]
            with self.assertRaisesRegex(Error, "protection --configure"):
                self.flow.protection()
            self.api.rulesets[0] = saved

    def test_finish_retry_does_not_rewrite_unchanged_ci_routing_body(self):
        self.ready()
        with unittest.mock.patch.object(self.api, "request", wraps=self.api.request) as request:
            self.flow.finish("task", "Stable review description", pr_only=True)
            self.flow.finish("task", "Stable review description", pr_only=True)
        edits = [call for call in request.call_args_list if call.args[:2] == ("PATCH", "/pulls/1")]
        self.assertEqual(len(edits), 1)

    def test_sync_needs_no_backup_remote_and_never_pushes_one(self):
        git(self.primary, "remote", "remove", "github-backup")
        self.ready()
        state = self.flow.finish("task", "description", rollup=False)
        self.assertEqual(state["phase"], "synced")
        self.assertNotIn("mirror", state)

    def test_legacy_record_steps_aside_for_adoption(self):
        old = self.root / "old"
        git(self.primary, "worktree", "add", "-b", "codex/old", str(old), "main")
        self.flow.file("old").write_text(json.dumps({
            "schema": 1, "task": "old", "title": "Old task", "branch": "codex/old",
            "worktree": str(old), "base": git(self.primary, "rev-parse", "main"),
            "phase": "active", "pr": 7, "pr_url": "https://git.example/pulls/7"}))
        with self.assertRaisesRegex(Error, "predates the GitHub repository"):
            self.flow.load("old")
        records = {s["task"]: s for s in self.flow.status()}
        self.assertEqual(records["old"]["remote_state"], "legacy-unknown")
        self.assertEqual(self.api.prs, [])
        with self.assertRaisesRegex(Error, "start --adopt"):
            self.flow.start("old", "Old task", old, "codex/old")
        state = self.flow.start("old", "Old task", old, "codex/old", adopt=True)
        self.assertEqual(state["host"], "github")
        self.assertEqual(state["schema"], 4)
        self.assertEqual(state["phase"], "active")
        self.assertTrue((self.flow.state_dir / "evidence/old/legacy-record.json").is_file())
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
        git(self.primary, "remote", "set-url", "--push", "origin", "https://example.invalid/gti.git")
        with self.assertRaisesRegex(Error, "canonical"):
            module.Workflow(self.primary, self.api)
        git(self.primary, "remote", "set-url", "--push", "origin", module.CANONICAL)
        module.Workflow(self.primary, self.api)

    def legacy_gitea(self):
        self.ready()
        state = self.flow.load("task")
        state.update(schema=3, host="gitea", pr_url="https://git.example/gti-lang/gti/pulls/41")
        self.flow.save(state)
        return state

    def test_rehost_binds_a_gitea_task_by_identical_tree(self):
        original = self.legacy_gitea()
        with self.assertRaisesRegex(Error, "predates the GitHub repository"):
            self.flow.load("task")
        self.assertEqual(self.flow.status("task")[0]["remote_state"], "legacy-gitea")
        state = self.flow.rehost("task", 1)
        self.assertEqual(state["host"], "github")
        self.assertEqual(state["schema"], 4)
        self.assertNotIn("verified", state)
        self.assertEqual(state["published_head"], self.api.prs[0]["head"]["sha"])
        self.assertEqual(state["hosting_migration"]["from_pr"], original["pr_url"])
        self.assertEqual(state["hosting_migration"]["from_head"], original["published_head"])
        recovery = Path(state["hosting_migration"]["recovery"])
        self.assertEqual(json.loads(recovery.read_text()), original)
        self.assertEqual(self.flow.rehost("task", 1), state)
        with self.assertRaisesRegex(Error, "another GitHub PR"):
            self.flow.rehost("task", 2)

    def test_rehost_moves_a_clean_worktree_to_the_rewritten_commit(self):
        original = self.legacy_gitea()
        # A rewritten identity: same tree and parent, different commit.
        old = original["published_head"]
        rewritten = git(self.worktree, "commit-tree", old + "^{tree}", "-p", old + "^",
                        "-m", "implement (rewritten identity)")
        git(self.primary, "push", "--force", "origin", rewritten + ":refs/heads/" + self.branch)
        state = self.flow.rehost("task", 1)
        self.assertEqual(state["published_head"], rewritten)
        self.assertEqual(git(self.worktree, "rev-parse", "HEAD"), rewritten)
        self.assertEqual(git(self.worktree, "status", "--porcelain"), "")

    def test_rehost_moves_a_pre_gitea_github_record_by_tree_identity(self):
        original = self.legacy_gitea()
        original.update(schema=2, host="github", pr=320,
                        pr_url="https://github.com/lang-gti-official/gti/pull/320")
        self.flow.save(original)
        old = original["published_head"]
        rewritten = git(self.worktree, "commit-tree", old + "^{tree}", "-p", old + "^",
                        "-m", "implement (rewritten GitHub identity)")
        git(self.primary, "push", "--force", "origin", rewritten + ":refs/heads/" + self.branch)
        state = self.flow.rehost("task", 1)
        self.assertEqual(state["schema"], 4)
        self.assertEqual(state["pr"], 1)
        self.assertEqual(state["published_head"], rewritten)
        self.assertEqual(state["hosting_migration"]["from_host"], "github")
        self.assertEqual(state["hosting_migration"]["from_pr"], original["pr_url"])
        self.assertEqual(json.loads(Path(state["hosting_migration"]["recovery"]).read_text()), original)
        self.assertEqual(git(self.worktree, "rev-parse", "HEAD"), rewritten)
        self.assertEqual(git(self.worktree, "status", "--porcelain"), "")
        self.assertNotIn("verified", state)

    def test_pre_gitea_github_record_requires_explicit_rehost(self):
        original = self.legacy_gitea()
        original.update(schema=2, host="github", pr=320)
        self.flow.save(original)
        with self.assertRaisesRegex(Error, "Use rehost"):
            self.flow.start("task", "Task outcome", self.worktree, self.branch, adopt=True)
        self.assertEqual(self.flow.load("task", legacy=True), original)

    def test_rehost_rejects_live_verification_changed_tree_and_implicit_adoption(self):
        original = self.legacy_gitea()
        with self.assertRaisesRegex(Error, "Use rehost"):
            self.flow.start("task", "Task outcome", self.worktree, self.branch, adopt=True)
        self.flow.save(original, "verifying")
        with self.assertRaisesRegex(Error, "still running"):
            self.flow.rehost("task", 1)
        self.flow.save(original, "ready")
        (self.worktree / "feature.txt").write_text("changed after migration\n")
        git(self.worktree, "commit", "-am", "different tree")
        git(self.worktree, "push", "--force", "origin", "HEAD:refs/heads/" + self.branch)
        with self.assertRaisesRegex(Error, "published tree"):
            self.flow.rehost("task", 1)
        self.assertEqual(self.flow.load("task", legacy=True)["host"], "gitea")

    def test_rehost_rejects_unidentified_records_even_if_numbers_collide(self):
        original = self.legacy_gitea()
        original.update(schema=1)
        self.flow.save(original)
        with self.assertRaisesRegex(Error, "Only a published schema-2 GitHub or schema-3 Gitea task"):
            self.flow.rehost("task", 1)

    def test_pr_lookup_filters_by_owner_branch_and_checks_repository_identity(self):
        self.start()
        self.checkpoint()
        actual = self.api.prs[0]
        fork = {**copy.deepcopy(actual), "number": 2,
                "head": {**actual["head"], "repo": {"full_name": "someone/gti"}}}
        self.api.prs.append(fork)
        with unittest.mock.patch.object(self.api, "request", wraps=self.api.request) as requests:
            self.assertEqual(self.flow.find_pr(self.branch)["number"], 1)
        lookups = [call.args[1] for call in requests.call_args_list if call.args[1].startswith("/pulls?")]
        self.assertEqual(lookups, ["/pulls?state=all&head=lang-gti-official%3Acodex%2Ftask&per_page=100"])

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
            self.flow.finish("task", "description", rollup=False)
        self.assertEqual(secret.read_text(), "preserve user secret")
        self.assertEqual(self.flow.load("task")["phase"], "sync-blocked")

    def test_reconcile_never_mutates_integrated_branch(self):
        self.ready()
        self.flow.finish("task", "description", rollup=False)
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
        self.flow.finish("task", "description", rollup=False)

    def test_recover_merged_pr_without_recreating_branch(self):
        self.ready()
        self.flow.finish("task", "description", rollup=False)
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

    def test_focused_automation_checks_do_not_take_a_broad_suite_slot(self):
        self.start()
        self.checkpoint("workflow instructions", "AGENTS.md")
        state = self.flow.verify("task", [self.probe("len(holders) == 0")], focused=True)
        self.assertEqual(state["verified"]["scope"], "automation")
        self.assertEqual(self.flow.window.holders("suite"), [])

    def test_focused_helper_refuses_to_downgrade_compiler_changes(self):
        self.start()
        (self.worktree / "src/compiler").mkdir(parents=True)
        self.checkpoint("compiler source", "src/compiler/change.cpp")
        with self.assertRaisesRegex(Error, "only for local automation-only"):
            self.flow.verify("task", [[sys.executable, "-c", "raise SystemExit(99)"]], focused=True)

    def test_focused_source_library_checks_do_not_take_a_suite_slot(self):
        self.start()
        (self.worktree / "stdlib/std").mkdir(parents=True)
        (self.worktree / "tests").mkdir()
        self.checkpoint("namespace library {}", "stdlib/std/library.gti")
        self.checkpoint(self.probe("len(holders) == 0")[2], "tests/library_runtime_test.py")
        state = self.flow.verify("task", [[sys.executable, "tests/library_runtime_test.py"]], focused=True)
        self.assertEqual(state["phase"], "ready")
        self.assertEqual(state["verified"]["scope"], "source-stdlib")
        self.assertEqual(self.flow.window.holders("suite"), [])

    def test_focused_source_library_refuses_broad_checks_before_execution(self):
        self.start()
        (self.worktree / "stdlib/std").mkdir(parents=True)
        self.checkpoint("namespace library {}", "stdlib/std/library.gti")
        with self.assertRaisesRegex(Error, "one exact test name"):
            self.flow.verify("task", [["ctest", "--test-dir", "build"]], focused=True)
        self.assertNotEqual(self.flow.load("task")["phase"], "verifying")

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
            with self.assertRaisesRegex(Error, "already running"):
                self.flow.verify("task", [[sys.executable, "-c", "raise SystemExit(99)"]])
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
        self.flow.finish("task", "description", rollup=False)
        old = git(self.remote, "rev-parse", self.branch)
        git(self.primary, "worktree", "remove", str(self.worktree))
        git(self.primary, "branch", "-d", self.branch)
        self.flow.file("task").unlink()
        with self.assertRaisesRegex(Error, "already uses"):
            self.flow.start("new-task", "new task", self.worktree, self.branch)
        self.assertEqual(git(self.remote, "rev-parse", self.branch), old)
        self.assertFalse(self.worktree.exists())

    def labels(self, number=1):
        return [label["name"] for label in self.api.prs[number - 1]["labels"]]

    def test_finish_labels_an_eligible_pr_for_a_rollup_instead_of_queueing(self):
        self.ready()
        state = self.flow.finish("task", "description")
        self.assertEqual(state["phase"], "rollup")
        self.assertEqual(state["rollup"]["head"], state["verified"]["head"])
        self.assertEqual(self.labels(), [module.ROLLUP_MAYBE])
        self.assertEqual(self.api.enqueued, [])
        self.assertFalse(self.api.prs[0]["draft"])
        # A retry is a normal completion point: no second label, no queue entry.
        self.assertEqual(self.flow.finish("task", "description")["phase"], "rollup")
        self.assertEqual(self.labels(), [module.ROLLUP_MAYBE])
        with self.assertRaisesRegex(Error, "has not merged"):
            self.flow.sync("task")

    def test_automation_only_changes_are_always_rollup_eligible(self):
        self.start()
        (self.worktree / "docs").mkdir()
        self.checkpoint("notes\n", "docs/notes.md")
        self.flow.protection(True)
        self.flow.verify("task", [[sys.executable, "-c", "pass"]])
        self.assertEqual(self.flow.finish("task", "description")["phase"], "rollup")
        self.assertEqual(self.labels(), [module.ROLLUP_ALWAYS])

    def test_system_risk_changes_validate_alone(self):
        for name in ("CMakeLists.txt", "cmake/Toolchain.cmake", "runtime/include/gti/c_abi.h", "reverted"):
            with self.subTest(name=name):
                self.tearDown()
                self.setUp()
                self.start()
                if name == "reverted":
                    # A system path touched by any commit counts, even if a later one reverts it.
                    (self.worktree / "CMakeLists.txt").write_text("project(x)\n")
                    self.flow.checkpoint("task", "touch build", ["CMakeLists.txt"])
                    (self.worktree / "CMakeLists.txt").unlink()
                    self.flow.checkpoint("task", "revert build", ["CMakeLists.txt"])
                    self.checkpoint()
                else:
                    (self.worktree / name).parent.mkdir(parents=True, exist_ok=True)
                    self.checkpoint("system\n", name)
                self.flow.protection(True)
                self.flow.verify("task", [[sys.executable, "-c", "pass"]])
                state = self.flow.finish("task", "description")
                self.assertEqual(state["phase"], "synced")
                self.assertEqual(self.labels(), [module.ROLLUP_NEVER])
                self.assertEqual(len(self.api.enqueued), 1)

    def test_no_rollup_and_failed_rollups_queue_alone(self):
        self.ready()
        self.api.queue_async = True
        self.assertEqual(self.flow.finish("task", "description")["phase"], "rollup")
        state = self.flow.finish("task", "description", rollup=False)
        self.assertEqual(state["phase"], "queued")
        self.assertEqual(self.labels(), [module.ROLLUP_NEVER])
        self.assertEqual(self.api.enqueued, [(1, state["verified"]["head"])])
        # A PR the bot marked rollup-failed validates alone without --no-rollup.
        self.tearDown()
        self.setUp()
        self.ready()
        self.api.prs[0]["labels"].append({"name": module.ROLLUP_FAILED})
        self.assertEqual(self.flow.finish("task", "description")["phase"], "synced")
        self.assertIn(module.ROLLUP_NEVER, self.labels())

    def test_cli_no_rollup_requests_a_direct_queue_entry(self):
        body = self.root / "body.md"
        body.write_text("description")
        calls = []
        fake = types.SimpleNamespace(lock=contextlib_null, finish=lambda *args, **kwargs: calls.append((args, kwargs)) or {})
        for argv, expected in ((["finish", "task", "--body-file", str(body), "--no-rollup"], False),
                               (["finish", "task", "--body-file", str(body)], True)):
            with unittest.mock.patch.object(module, "Workflow", return_value=fake), \
                    unittest.mock.patch.object(sys, "argv", ["agent_workflow.py", *argv]), \
                    unittest.mock.patch("builtins.print"):
                self.assertEqual(module.main(), 0)
            self.assertEqual(calls[-1][1], {"rollup": expected})

    def merge_rollup(self, member_head):
        """What the bot and the queue do: a rollup branch carrying the member's head merges into main."""
        base = git(self.remote, "rev-parse", "main")
        tree = git(self.remote, "merge-tree", "--write-tree", base, member_head).splitlines()[0]
        head = git(self.remote, "commit-tree", tree, "-p", base, "-p", member_head, "-m", "Merge #1")
        git(self.remote, "update-ref", "refs/heads/rollup/20261029-1", head)
        rollup = self.api.request("POST", "/pulls", {"base": "main", "head": "rollup/20261029-1",
                                                     "title": "Rollup of 2 pull requests", "draft": False})
        merge = git(self.remote, "commit-tree", tree, "-p", base, "-p", head, "-m", "Merge queue candidate")
        git(self.remote, "update-ref", "refs/heads/main", merge)
        rollup.update(merged=True, state="closed", merge_commit_sha=merge)
        return rollup

    def test_sync_after_a_rollup_merge_requires_the_bots_proof(self):
        self.ready()
        head = self.flow.finish("task", "description")["rollup"]["head"]
        rollup = self.merge_rollup(head)
        member = self.api.prs[0]
        with self.assertRaisesRegex(Error, "has not merged"):
            self.flow.sync("task")
        # A closed PR without a marker, or with one naming an unmerged or wrong rollup, is not integration.
        member["state"] = "closed"
        with self.assertRaisesRegex(Error, "has not merged"):
            self.flow.sync("task")
        member["comments"].append({"body": module.rollup_marker({"rollup": rollup["number"], "head": "0" * 40,
                                                                 "merge_commit": rollup["merge_commit_sha"]})})
        rollup["merged"] = False
        member["comments"].append({"body": module.rollup_marker({"rollup": rollup["number"], "head": head,
                                                                 "merge_commit": rollup["merge_commit_sha"]})})
        with self.assertRaisesRegex(Error, "has not merged"):
            self.flow.finish("task", "description")
        rollup["merged"] = True
        state = self.flow.finish("task", "description")
        self.assertEqual(state["phase"], "synced")
        self.assertEqual(state["integrated_via_rollup"]["rollup"], rollup["number"])
        self.assertEqual(state["integrated_via_rollup"]["merge_commit"], rollup["merge_commit_sha"])
        self.assertTrue((self.primary / "feature.txt").is_file())
        self.assertEqual(member["state"], "closed")
        self.flow.cleanup("task")
        self.assertEqual(self.flow.load("task")["phase"], "completed")

    def test_sync_accepts_a_member_github_marked_merged_by_the_rollup(self):
        self.ready()
        head = self.flow.finish("task", "description")["rollup"]["head"]
        rollup = self.merge_rollup(head)
        self.api.prs[0].update(merged=True, state="closed", merge_commit_sha=rollup["merge_commit_sha"])
        self.assertEqual(self.flow.sync("task")["phase"], "synced")


class contextlib_null:
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def batch_plan(**changes):
    return {"tier": "T1", "rationale": "A confined source diagnostic recovery fix",
            "regressions": ["regression"], "owning": ["owning"],
            "cross_phase": ["editor"], "targets": ["owning_tests"],
            "reproduction": {"exception": "The historical failing compiler is unavailable; kernel and original CTest evidence are retained"},
            "platforms": [sys.platform], **changes}


class ValidationChecks(unittest.TestCase):
    def test_source_library_focused_scope_is_narrow_and_does_not_change_ci(self):
        source = ["stdlib/std/encoding.gti"]
        allowed = ["stdlib/std/encoding.gti", "tests/fixtures/encoding.gti",
                   "tests/encoding_runtime_test.py", "examples/094-encoding.gti",
                   "examples/README.md", "stdlib/README.md", "docs/language/encoding.md",
                   "VERSION", "CHANGELOG.md", "tests/stdlib_surface_manifest.json",
                   "tests/mir_census_baseline.json", "tests/lowered_program_census_baseline.json"]
        self.assertEqual(validation.focused_scope(source + allowed), "source-stdlib")
        self.assertEqual(validation.ci_mode({}, source + allowed), "full")
        self.assertEqual(validation.focused_scope(["docs/index.md"]), "automation")
        self.assertIsNone(validation.focused_scope(["tests/encoding_runtime_test.py"]))
        for path in ("src/compiler/foo.cpp", "include/gti/type.h", "runtime/host.cpp",
                     "stdlib/std/foo.cpp", "stdlib/native/foo.gti", "tests/CMakeLists.txt",
                     "CMakeLists.txt", "tests/compiler_tests.cpp", "scripts/install.py",
                     ".github/workflows/ci.yml"):
            with self.subTest(path=path):
                self.assertIsNone(validation.focused_scope(source + [path]))

    def test_source_library_focused_checks_reject_obvious_broad_work(self):
        owning = [sys.executable, "tests/encoding_runtime_test.py", "/tmp/gti"]
        validation.source_focused_checks([owning, ["git", "diff", "--check"]])
        validation.source_focused_checks([["ctest", "-R", "^encoding_runtime$"]])
        validation.source_focused_checks([["ctest", "--tests-regex=^encoding_runtime$"]])
        for command in (["ctest"], ["ctest", "-R", ".*"], ["ctest", "-R"],
                        ["ctest", "-R", "^encoding_runtime$", "--rerun-failed"],
                        ["ctest", "-R", "^encoding_runtime$", "-R", ".*"],
                        ["cmake", "--build", "build"], ["ninja"], ["bash", "-c", "ctest"],
                        [sys.executable, "scripts/build_window.py"],
                        [sys.executable, "tests/mir_census_test.py"],
                        [sys.executable, "tests/stdlib_surface_test.py"]):
            with self.subTest(command=command):
                with self.assertRaises(ValueError):
                    validation.source_focused_checks([owning, command])
        with self.assertRaisesRegex(ValueError, "named owning"):
            validation.source_focused_checks([["git", "diff", "--check"]])

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

    def test_ci_gate_requires_fast_checks_on_prs_and_the_suite_in_the_queue(self):
        lint = {name: {"result": "success"} for name in validation.LINT_JOBS}
        pr = {**lint, "fast": {"result": "success"}}
        queue = {**lint, "fast": {"result": "success"}, "package": {"result": "success"}, "integration": {"result": "success"}, "sanitizers": {"result": "success"}}
        validation.admission("full", pr, "pull_request")
        validation.admission("full", queue, "merge_group")
        with self.assertRaises(ValueError):
            validation.admission("full", pr, "merge_group")
        with self.assertRaises(ValueError):
            validation.admission("full", lint, "pull_request")
        for results, event in ((pr, "pull_request"), (queue, "merge_group")):
            for name in results:
                failed = copy.deepcopy(results)
                failed[name]["result"] = "skipped"
                with self.assertRaises(ValueError):
                    validation.admission("full", failed, event)
        with self.assertRaises(ValueError):
            validation.admission("full", queue, "push")

    def test_ci_draft_and_component_report_incomplete_integration(self):
        lint = {name: {"result": "success"} for name in validation.LINT_JOBS}
        for draft in (True, False):
            body = validation.component_body("Reviewable feature description", {"head": "a" * 40})
            event = {"pull_request": {"draft": draft, "body": body}}
            self.assertEqual(validation.ci_mode(event, ["src/compiler/parser.cpp"]), "component")
            with self.assertRaises(ValueError):
                validation.admission("component", lint)
            self.assertEqual(validation.component_body(body), "Reviewable feature description")
        self.assertEqual(validation.ci_mode({"pull_request": {"draft": True}}, []), "draft")
        # A draft PR reports its lint checks; it can never enter the queue.
        validation.admission("draft", lint, "pull_request")
        with self.assertRaises(ValueError):
            validation.admission("draft", lint, "merge_group")

    def test_shards_partition_deterministically_and_merge_only_complete_coverage(self):
        names = ["a", "b", "c", "d", "e"]
        durations = {"a": 100, "b": 90, "c": 10, "d": 5}
        first = validation.shard_assignment(names, durations, 2, default=1)
        self.assertEqual(first, validation.shard_assignment(list(reversed(names)), durations, 2, default=1))
        self.assertEqual(sorted(name for shard in first for name in shard), names)
        self.assertEqual(first, [["a", "d"], ["b", "c", "e"]])
        self.assertEqual(validation.shard_assignment(names, durations, 9, default=1)[8], [])
        def report(platform, index, tests, head="h", inventory=names):
            return {"platform": platform, "index": index, "shards": 2, "scope": "shard", "head": head,
                    "tree": "t", "inventory": inventory, "tests": tests, "skipped": [],
                    "build_type": "Release", "smoke": ["scalar-O0"] if index == 0 else []}
        good = [report(p, i, first[i]) for p in ("darwin", "linux") for i in (0, 1)]
        summary = validation.merge_shards(good, ["darwin", "linux"])
        self.assertEqual(summary["linux"]["tests"], 5)
        forged = {
            "missing shard": good[:3],
            "missing test": [*good[:3], report("linux", 1, ["b"])],
            "duplicate test": [*good[:3], report("linux", 1, ["a", "b", "c"])],
            "other source": [*good[:3], report("linux", 1, first[1], head="other")],
            "other inventory": [*good[:3], report("linux", 1, first[1], inventory=names[:4])],
            "missing platform": good[2:],
        }
        for label, reports in forged.items():
            with self.subTest(label), self.assertRaises(ValueError):
                validation.merge_shards(reports, ["darwin", "linux"])
        no_smoke = [dict(row, smoke=[]) for row in good]
        with self.assertRaises(ValueError):
            validation.merge_shards(no_smoke, ["darwin", "linux"])

    def test_quarantine_excludes_only_listed_platform_tests_and_the_gate_accounts_for_it(self):
        issue = "https://github.com/lang-gti-official/gti/issues/12"
        entry = {"name": "slow_case", "platforms": ["linux"], "issue": issue,
                 "reason": "fails on main", "since": "2026-10-30"}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertEqual(validation.load_quarantine(root), {})
            (root / "tests").mkdir()
            path = root / validation.QUARANTINE
            path.write_text(json.dumps({"schema": 1, "tests": [entry]}))
            table = validation.load_quarantine(root)
            self.assertEqual(validation.quarantined_on(table, "linux"), ["slow_case"])
            self.assertEqual(validation.quarantined_on(table, "darwin"), [])
            for label, bad in {
                "smoke": dict(entry, name=validation.SMOKE),
                "unknown platform": dict(entry, platforms=["win32"]),
                "no platforms": dict(entry, platforms=[]),
                "other repository issue": dict(entry, issue="https://github.com/other/gti/issues/1"),
                "no reason": dict(entry, reason=" "),
                "no date": dict(entry, since="soon"),
                "extra field": dict(entry, expires="never"),
            }.items():
                path.write_text(json.dumps({"schema": 1, "tests": [bad]}))
                with self.subTest(label), self.assertRaises(ValueError):
                    validation.load_quarantine(root)
            path.write_text(json.dumps({"schema": 1, "tests": [entry, entry]}))
            with self.assertRaises(ValueError):
                validation.load_quarantine(root)
        names = ["a", "b", "slow_case"]
        def report(platform, index, tests, quarantined):
            return {"platform": platform, "index": index, "shards": 2, "scope": "shard", "head": "h",
                    "tree": "t", "inventory": names, "tests": tests, "quarantined": quarantined,
                    "skipped": [], "build_type": "Release", "smoke": ["scalar-O0"] if index == 0 else []}
        good = [report("linux", 0, ["a"], ["slow_case"]), report("linux", 1, ["b"], ["slow_case"]),
                report("darwin", 0, ["a", "slow_case"], []), report("darwin", 1, ["b"], [])]
        summary = validation.merge_shards(good, ["darwin", "linux"])
        self.assertEqual((summary["linux"]["tests"], summary["linux"]["quarantined"]), (2, ["slow_case"]))
        self.assertEqual(summary["darwin"]["quarantined"], [])
        for label, reports in {
            "shards disagree": [good[0], report("linux", 1, ["b"], []), *good[2:]],
            "smoke quarantined": [report("linux", 0, ["a"], [validation.SMOKE]),
                                  report("linux", 1, ["b", "slow_case"], [validation.SMOKE]), *good[2:]],
            "unregistered quarantine": [report("linux", 0, ["a"], ["ghost"]),
                                        report("linux", 1, ["b", "slow_case"], ["ghost"]), *good[2:]],
        }.items():
            with self.subTest(label), self.assertRaises(ValueError):
                validation.merge_shards(reports, ["darwin", "linux"])

    def test_test_targets_reads_configured_commands_without_a_build(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory).resolve()
            (build / "tests").mkdir()
            (build / "tests" / "CTestTestfile.cmake").write_text(
                f'add_test("unit_a" "{build}/gti_unit_a_tests" "--flag")\n'
                f'add_test("script_b" "/usr/bin/python3" "/src/b.py" "frontend={build}/gti_frontend_tests" "{build}/libgti_runtime.a")\n'
                f'add_test("other" "{build}/gti_other_tests")\n'
                f'add_test([=[bracket_c]=] "{build}/gti_bracket_tests" [==[{build}/gti_bracket_arg]==])\n')
            self.assertEqual(validation.test_targets(build, "unit_a|script_b"),
                             ["gti", "gti_frontend_tests", "gti_runtime", "gti_unit_a_tests"])
            # Older CMake writes bracket arguments for names and arguments.
            self.assertEqual(validation.test_targets(build, "^bracket_c$"),
                             ["gti", "gti_bracket_arg", "gti_bracket_tests", "gti_runtime"])
            with self.assertRaises(ValueError):
                validation.test_targets(build, "^missing$")

    def test_committed_quarantine_is_valid(self):
        validation.load_quarantine(Path(__file__).resolve().parents[1])

    def test_ci_automation_paths_name_real_files_that_ci_runs(self):
        # A name for a missing harness would let a future file of that name
        # skip native CI without review, so the inventory names only real
        # scripts and exactly the harnesses the workflow-regression job runs.
        root = Path(__file__).resolve().parents[1]
        for name in validation.AUTOMATION_SCRIPTS:
            self.assertTrue((root / "scripts" / name).is_file(), name)
        workflow = (root / ".github/workflows/ci.yml").read_text()
        job = workflow.split("  automation-tests:", 1)[1].split("\n  formatting:", 1)[0]
        executed = set(re.findall(r"python3 tests/([A-Za-z0-9_]+_test\.py)", job))
        self.assertEqual(validation.AUTOMATION_TESTS, executed)
        for name in validation.AUTOMATION_TESTS:
            self.assertTrue((root / "tests" / name).is_file(), name)

    def test_system_risk_paths_are_conservative(self):
        for path in ("CMakeLists.txt", "src/compiler/CMakeLists.txt", "tests/CMakeLists.txt",
                     "tests/TestThroughput.cmake", "cmake/GTIConfig.cmake.in", ".github/workflows/rollup.yml",
                     "scripts/configure_ci_toolchain.sh", "runtime/include/gti/c_abi.h",
                     "include/gti/object_layout.h", "src/compiler/llvm/runtime_abi.cpp"):
            self.assertEqual(validation.system_risk([path, "docs/x.md"]), [path])
        self.assertEqual(validation.system_risk(["src/compiler/parser.cpp", "docs/index.md", "scripts/rollup.py",
                                                 "runtime/src/io.cpp", "tests/parser_tests.cpp"]), [])

    def test_rollup_manifest_keeps_its_members_classification(self):
        event = {"pull_request": {"draft": False}}
        manifest = ".gti-validation/batches/rollup-20261029-1.json"
        self.assertEqual(validation.ci_mode(event, ["docs/index.md", manifest]), "automation")
        self.assertEqual(validation.ci_mode(event, ["src/compiler/parser.cpp", manifest]), "full")
        for paths in ([manifest], [".gti-validation/batches/rollup-x.json", "docs/index.md"]):
            self.assertEqual(validation.ci_mode(event, paths), "full")

    def test_orchestration_workflows_skip_native_ci_but_mixed_candidates_do_not(self):
        for event in ({"pull_request": {"draft": False}}, {"merge_group": {}}):
            for workflow in validation.AUTOMATION_WORKFLOWS:
                with self.subTest(event=event, workflow=workflow):
                    self.assertEqual(validation.ci_mode(event, [workflow, "tests/rollup_test.py"]), "automation")
                    for sensitive in ("src/compiler/parser.cpp", "runtime/src/memory.cpp",
                                      "stdlib/std.gti", "tests/CMakeLists.txt", "CMakeLists.txt",
                                      ".github/workflows/ci.yml", ".github/workflows/cache.yml",
                                      ".github/workflows/nightly.yml", ".github/workflows/beta.yml",
                                      ".github/workflows/new.yml", "scripts/validation_checks.py",
                                      "scripts/validation_batch.py", "scripts/named_test_shards.py",
                                      "scripts/configure_ci_resources.sh"):
                        self.assertEqual(validation.ci_mode(event, [workflow, sensitive]), "full")
            results = {name: {"result": "success"} for name in validation.LINT_JOBS}
            results.update({name: {"result": "skipped"} for name in ("fast", "integration", "sanitizers")})
            validation.admission("automation", results, event="merge_group" if "merge_group" in event else "pull_request")
            results["automation-tests"]["result"] = "failure"
            with self.assertRaises(ValueError):
                validation.admission("automation", results)

    def test_ci_automation_scope_is_conservative_and_has_its_own_gates(self):
        event = {"pull_request": {"draft": False}}
        self.assertEqual(validation.ci_mode(event, ["scripts/agent_workflow.py", "tests/agent_workflow_test.py",
                                                  "tests/repository_hosting_test.py", "docs/architecture/verification.md"]), "automation")
        for paths in ([], ["src/compiler/parser.cpp"], ["tests/parser_tests.cpp"],
                      ["runtime/include/gti/runtime.h"], ["CMakeLists.txt"],
                      [".gitea/workflows/release.yml"], [".github/workflows/ci.yml"],
                      [".gti-validation/batches/batch.json"]):
            self.assertEqual(validation.ci_mode(event, paths), "full")
        results = {name: {"result": "success"} for name in validation.LINT_JOBS}
        validation.admission("automation", results)
        results["automation-tests"]["result"] = "failure"
        with self.assertRaises(ValueError):
            validation.admission("automation", results)

    def test_edited_title_routes_validation_but_body_only_is_not_admission(self):
        event = {"pull_request": {"draft": False}, "action": "edited",
                 "changes": {"title": {"from": "WIP: Task"}}}
        self.assertEqual(validation.ci_mode(event, ["src/compiler/parser.cpp"]), "full")
        event["pull_request"]["draft"] = True
        self.assertEqual(validation.ci_mode(event, ["src/compiler/parser.cpp"]), "draft")
        event["changes"] = {"body": {"from": "old description"}}
        self.assertEqual(validation.ci_mode(event, ["src/compiler/parser.cpp"]), "metadata")
        with self.assertRaises(ValueError):
            validation.admission("metadata", {})
        event["changes"] = {}
        event["pull_request"]["draft"] = False
        self.assertEqual(validation.ci_mode(event, ["src/compiler/parser.cpp"]), "full")

    @unittest.skipIf(os.name == "nt", "The fake compiler and its outputs are POSIX scripts")
    def test_smoke_command_runs_each_program_at_o0_and_o3(self):
        with tempfile.TemporaryDirectory(prefix="gti-smoke-command-") as temporary:
            build = Path(temporary)
            calls = build / "calls.txt"
            compiler = build / "gti"
            # Each "compiled" program records its own run, so the test proves
            # that the subcommand executes the outputs it asked for.
            compiler.write_text(f"#!{sys.executable}\nimport pathlib, sys\n"
                                f"calls = pathlib.Path({str(calls)!r})\n"
                                "output = pathlib.Path(sys.argv[sys.argv.index('-o') + 1])\n"
                                "with calls.open('a') as log: log.write('compile ' + output.name + '\\n')\n"
                                "output.write_text('#!/bin/sh\\necho run ' + output.name + "
                                f"' >> {calls}\\n')\n"
                                "output.chmod(0o755)\n")
            compiler.chmod(0o755)
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                validation.main(["smoke", "--build", str(build)])
            expected = ["scalar-O0", "scalar-O3", "owners-O0", "owners-O3"]
            self.assertEqual(stdout.getvalue(), "GTI_SMOKE_RESULT=" + json.dumps(expected) + "\n")
            log = calls.read_text().splitlines()
            self.assertEqual([line for line in log if line.startswith("run ")],
                             ["run " + name for name in expected])

    def test_automation_job_harnesses_are_classified_as_automation(self):
        _, script = CIEnvironment.step("automation-tests", "Verify workflow, batch, preservation and resource rules")
        harnesses = re.findall(r"^python3 tests/([A-Za-z0-9_]+_test\.py)\b", script, re.M)
        self.assertIn("ownership_map_test.py", harnesses)
        self.assertIn("plan_checks_test.py", harnesses)
        for harness in harnesses:
            with self.subTest(harness=harness):
                self.assertTrue((ROOT / "tests" / harness).is_file())
                self.assertIn(harness, validation.AUTOMATION_TESTS)
                expected = "full" if "tests/" + harness in validation.FULL_VALIDATION_FILES else "automation"
                self.assertEqual(validation.ci_mode({}, ["tests/" + harness]), expected)

    def test_platform_scoped_gates_keep_other_hosts_obligations(self):
        plan = batch_plan(platforms=["darwin", "linux"],
                          platform_tests={"darwin": ["native_mac"], "linux": ["native_linux"]},
                          additional_checks=[["common-check"]],
                          platform_checks={"darwin": [["mac-check"]], "linux": [["linux-check"]]})
        validation.validate_plan(plan)
        self.assertIn("native_mac", validation.plan_tests(plan, "darwin"))
        self.assertNotIn("native_mac", validation.plan_tests(plan, "linux"))
        self.assertIn("native_linux", validation.plan_tests(plan, "linux"))
        self.assertIn("regression", validation.plan_tests(plan, "linux"))
        self.assertEqual(validation.extra_checks(plan, "darwin"), [["common-check"], ["mac-check"]])
        self.assertEqual(validation.extra_checks(plan, "linux"), [["common-check"], ["linux-check"]])


class CIEnvironment(unittest.TestCase):
    def test_current_guidance_does_not_restore_gitea_source_or_advisory_ci(self):
        retired = [re.compile(pattern, re.I) for pattern in (
            r"\bGitea\b[^.\n]{0,80}\bis canonical for (?:code|source|branches)",
            r"\bcanonical Gitea (?:repository|`main`|PRs?\b|workflow|origin)",
            r"\bGitea CI is advisory\b",
            r"protection --configure --advisory-ci",
            r"\bmerge (?:only )?through Gitea\b",
            r"\bnever publish (?:branches|PRs)[^.\n]{0,40}to GitHub\b",
        )]
        paths = [ROOT / name for name in ("AGENTS.md", "CLAUDE.md", "README.md")]
        for directory in ("docs", ".codex/skills", ".claude/skills"):
            paths.extend((ROOT / directory).rglob("*.md"))
        violations = []
        for path in paths:
            relative = path.relative_to(ROOT).as_posix()
            # The implementation sequence is a dated campaign record.
            if relative.startswith(("docs/plans/archive/", "docs/third-party-audit/")) or \
                    relative == "docs/plans/implementation-sequence.md":
                continue
            text = path.read_text()
            for pattern in retired:
                if pattern.search(text):
                    violations.append(f"{relative}: {pattern.pattern}")
        self.assertEqual(violations, [], "Active guidance must not restore retired hosting")

    @staticmethod
    def step(job, name):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text()
        body = re.search(r"^  " + re.escape(job) + r":\n(.*?)(?=^  [\w-]+:\n|\Z)",
                         workflow, re.M | re.S).group(1)
        step = re.search(r"^      - name: " + re.escape(name) +
                         r"\n(.*?)(?=^      - |\Z)", body, re.M | re.S)
        if step is None:
            raise AssertionError(f"Missing {job} step: {name}")
        lines = step.group(1).split("        run: |\n", 1)[1].splitlines()
        script = []
        for line in lines:
            if line and not line.startswith("          "):
                break
            script.append(line[10:] if line else "")
        return step.group(1), "\n".join(script)

    def test_classification_uses_context_event_without_prefixed_aliases(self):
        step, script = self.step("plan", "Classify candidate without weakening its merge gate")
        with tempfile.TemporaryDirectory(prefix="gti-ci-context-") as temporary:
            root = Path(temporary) / "runner workspace"
            (root / "scripts").mkdir(parents=True)
            shutil.copy2(ROOT / "scripts/validation_checks.py", root / "scripts/validation_checks.py")
            git(root, "init", "-b", "main")
            git(root, "config", "user.name", "CI Test")
            git(root, "config", "user.email", "test@example.invalid")
            git(root, "add", ".")
            git(root, "commit", "-m", "baseline")
            base = git(root, "rev-parse", "HEAD")
            git(root, "update-ref", "refs/remotes/origin/main", base)
            (root / "README.md").write_text("automation-only change\n")
            git(root, "add", ".")
            git(root, "commit", "-m", "documentation")
            payload = root / "event payload.json"
            output = root / "runner output"
            env = {key: value for key, value in os.environ.items()
                   if key not in {"GITEA_EVENT_PATH", "GITHUB_EVENT_PATH"}}
            env.update(GTI_EVENT_PATH=str(payload), GTI_BASE_SHA=base, GITHUB_OUTPUT=str(output),
                       GITHUB_EVENT_NAME="pull_request", GITHUB_REF="refs/pull/1/merge")

            def execute(success):
                output.write_text("")
                result = subprocess.run(["bash", "-e", "-o", "pipefail", "-c", script],
                                        cwd=root, env=env, capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode == 0, success, result.stderr)
                return output.read_text()

            for event, expected, before in (({}, "automation", "0" * 40),
                                           ({"pull_request": {"draft": True}}, "draft", base)):
                with self.subTest(event=event):
                    payload.write_text(json.dumps(event))
                    env["GTI_BASE_SHA"] = before
                    self.assertEqual(execute(True), f"mode={expected}\n")
            for path in ("", str(root), str(root / "missing.json")):
                with self.subTest(invalid_event_path=path):
                    env["GTI_EVENT_PATH"] = path
                    self.assertEqual(execute(False), "")
            env["GTI_EVENT_PATH"] = str(payload)
            payload.write_text("invalid JSON")
            self.assertEqual(execute(False), "")
            # Full commissioning ignores diff classification, but only on main.
            # Simulate this explicitly rather than inheriting the host event.
            payload.write_text("{}")
            env.update(GITHUB_EVENT_NAME="workflow_dispatch", GITHUB_REF="refs/heads/main")
            self.assertEqual(execute(True), "mode=full\n")
            env["GITHUB_REF"] = "refs/heads/feature"
            self.assertEqual(execute(False), "")
        self.assertIn("GTI_EVENT_PATH: ${{ github.event_path }}", step)

    @unittest.skipIf(os.name == "nt", "The ubuntu-24.04 bootstrap uses POSIX tools")
    def test_workflow_bootstrap_supplies_real_cmake_ctest_and_make(self):
        _, script = self.step("automation-tests", "Install workflow test prerequisites")
        binaries = {name: shutil.which(name) for name in ("cmake", "ctest", "make")}
        self.assertTrue(all(binaries.values()), "Local verification requires CMake, CTest and Make")
        with tempfile.TemporaryDirectory(prefix="gti-ci-bootstrap-") as temporary:
            root = Path(temporary)
            tools = root / "tools"
            tools.mkdir()
            calls = root / "apt-calls.json"
            sudo = tools / "sudo"
            # Intercept package installation locally; expose the real tools only
            # when the workflow requests their actual Debian package names.
            sudo.write_text(f"#!{sys.executable}\nimport json, pathlib, sys\n"
                            f"tools=pathlib.Path({str(tools)!r})\nlog=pathlib.Path({str(calls)!r})\n"
                            f"binaries={binaries!r}\n"
                            "args=sys.argv[1:]\nassert args[0]=='apt-get'\n"
                            "calls=json.loads(log.read_text()) if log.exists() else []\n"
                            "calls.append(args)\nlog.write_text(json.dumps(calls))\n"
                            "if args[1]=='install':\n"
                            " for package, names in {'cmake':['cmake','ctest'],'make':['make']}.items():\n"
                            "  if package in args:\n"
                            "   for name in names: (tools/name).symlink_to(binaries[name])\n")
            sudo.chmod(0o755)
            env = dict(os.environ, PATH=str(tools))
            with self.assertRaises(FileNotFoundError):
                subprocess.run(["cmake", "--version"], env=env, check=True)
            subprocess.run(["/bin/bash", "-e", "-o", "pipefail", "-c", script],
                           cwd=root, env=env, check=True, capture_output=True, timeout=30)
            installed = json.loads(calls.read_text())
            self.assertEqual(installed[0], ["apt-get", "update"])
            self.assertTrue(any(row[:2] == ["apt-get", "install"] and
                                {"cmake", "make"} <= set(row) for row in installed))
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.20)\nproject(CIBootstrap NONE)\n'
                'enable_testing()\nadd_custom_target(workflow_fixture)\n'
                f'add_test(NAME ci_bootstrap COMMAND "{sys.executable}" -c "pass")\n')
            for argv in (["cmake", "-S", str(root), "-B", str(root / "build"), "-G", "Unix Makefiles"],
                         ["cmake", "--build", str(root / "build"), "--target", "workflow_fixture"],
                         ["ctest", "--test-dir", str(root / "build"), "--no-tests=error", "--output-on-failure"]):
                subprocess.run(argv, cwd=root, env=env, check=True, capture_output=True, timeout=30)
        job = (ROOT / ".github/workflows/ci.yml").read_text().split("  automation-tests:\n", 1)[1].split("  formatting:\n", 1)[0]
        self.assertLess(job.index("Install workflow test prerequisites"), job.index("python3 tests/agent_workflow_test.py"))


class BatchLifecycle(unittest.TestCase):
    setUp = Lifecycle.setUp
    tearDown = Lifecycle.tearDown

    def fake_focused(self, command, **kwargs):
        root = Path(kwargs["cwd"])
        report_path = Path(command[command.index("--report") + 1])
        plan = json.loads(Path(command[command.index("--plan") + 1]).read_text())
        report_path.write_text(json.dumps({"head": git(root, "rev-parse", "HEAD"),
            "scope": "focused", "build_type": "Release", "tests": validation.plan_tests(plan), "skipped": [],
            "platform": sys.platform, "smoke": ["scalar-O0", "scalar-O3", "owners-O0", "owners-O3"]}))

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
        with unittest.mock.patch.object(self.api, "request", wraps=self.api.request) as requests:
            self.flow.finish("combined", "Full combined tests passed; the merge queue integrates it")
        self.assertFalse(any("/actions/" in call.args[1] or "/check-runs" in call.args[1]
                             for call in requests.call_args_list))
        self.assertTrue(all(self.flow.ancestor(head, git(self.primary, "rev-parse", "HEAD")) for head in heads))
        self.assertEqual(self.flow.load("combined")["phase"], "synced")

    def test_batch_components_close_with_explicit_proof_and_cleanup_safely(self):
        state = self.bind_full(self.combined())
        original = copy.deepcopy(self.flow.load("one")["admission"])
        self.flow.protection(True)
        self.flow.finish("combined", "Full combined platform evidence")
        self.assertFalse(self.flow.pull(self.flow.load("one"))["merged"])
        synced = self.flow.sync("one")
        self.assertEqual(synced["admission"], original)
        self.assertEqual(synced["integrated_via_batch"]["task"], "combined")
        pr = self.flow.pull(synced)
        self.assertEqual(pr["state"], "closed")
        self.assertFalse(pr["merged"], "do not invent a merged status")
        self.assertIn(self.flow.load("combined")["pr_url"], pr["body"])
        self.flow.cleanup("one")
        self.assertFalse((self.root / "one").exists())
        self.assertEqual(self.flow.load("one")["phase"], "completed")
        self.assertTrue(self.flow.ancestor(original["head"], git(self.primary, "rev-parse", "main")))
        self.flow.cleanup("one")  # interrupted/repeated cleanup is safe

    def test_batch_component_sync_requires_merged_full_platform_exact_source_proof(self):
        state = self.combined()
        with self.assertRaisesRegex(Error, "full-platform proof"):
            self.flow.sync("one")
        state = self.bind_full(state)
        self.flow.protection(True)
        self.flow.finish("combined", "Full combined platform evidence")
        accepted = copy.deepcopy(self.flow.load("combined"))
        broken = copy.deepcopy(accepted)
        broken["batch_verified"] = {}
        self.flow.save(broken)
        with self.assertRaisesRegex(Error, "full-platform proof"):
            self.flow.sync("one")
        self.assertEqual(self.flow.pull(self.flow.load("one"))["state"], "open")
        self.flow.save(accepted)
        member = self.flow.load("one")
        member["published_head"] = "0" * 40
        self.flow.save(member)
        with self.assertRaisesRegex(Error, "full-platform proof"):
            self.flow.sync("one")
        self.assertEqual(self.flow.pull(member)["state"], "open")

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
        self.api.prs[0]["draft"] = False
        with self.assertRaisesRegex(Error, "Return the component PR to draft"):
            batches.withdraw("one", "independent validation")
        self.api.prs[0]["draft"] = True
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

    def test_batch_manifest_requires_ancestry_and_becomes_immutable(self):
        state = self.combined()
        root = self.root / "combined"
        head = git(root, "rev-parse", "HEAD")
        rows = validation.manifests(root, state["batch"]["base"], head)
        self.assertEqual([row["task"] for row in rows], ["one", "two"])
        path = root / state["batch"]["manifest"]
        document = json.loads(path.read_text())
        document["members"][0]["head"] = "0" * 40
        path.write_text(json.dumps(document))
        git(root, "add", state["batch"]["manifest"])
        git(root, "commit", "-m", "forge unrelated source identity")
        with self.assertRaises(ValueError):
            validation.manifests(root, head, "HEAD")
        with self.assertRaises(subprocess.CalledProcessError):
            validation.manifests(root, state["batch"]["base"], "HEAD")

    def test_batch_unpublished_member_source_cannot_be_silently_left_behind(self):
        self.component("one")
        root = self.root / "one"
        (root / "one.txt").write_text("unpublished improvement")
        git(root, "add", "one.txt")
        git(root, "commit", "-m", "local source")
        with self.assertRaisesRegex(Error, "unpublished commits"):
            Batches(self.flow).prepare("combined", ["one"], self.root / "combined")
        self.assertTrue((root / "one.txt").exists())

    def test_batch_full_runner_executes_unrelated_canary_as_well_as_required_checks(self):
        root = self.primary
        (root / "scripts").mkdir()
        (root / "scripts/build_window.py").write_bytes((ROOT / "scripts/build_window.py").read_bytes())
        canary = root / "unrelated-ran"
        cmake = ('cmake_minimum_required(VERSION 3.20)\nproject(FakeCompiler NONE)\n'
                 'enable_testing()\nadd_custom_target(gti)\nadd_custom_target(gti_runtime)\n'
                 'add_test(NAME compiler_pipeline COMMAND "' + sys.executable + '" -c "pass")\n'
                 'add_test(NAME regression COMMAND "' + sys.executable + '" -c "pass")\n'
                 'add_test(NAME unrelated COMMAND "' + sys.executable + '" -c '
                 '"import pathlib; pathlib.Path(r\'' + str(canary) + '\').touch()")\n')
        (root / "CMakeLists.txt").write_text(cmake)
        build = root / "build"
        subprocess.run(["cmake", "-S", str(root), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release"],
                       check=True, capture_output=True)
        with (root / "full.log").open("w") as log, unittest.mock.patch.object(validation, "smoke", return_value=["scalar-O0", "scalar-O3", "owners-O0", "owners-O3"]):
            report = validation.run(root, build, {"compiler_pipeline", "regression"}, True, log=log, junit=root / "full.xml")
        self.assertTrue(canary.exists())
        self.assertEqual(report["tests"], ["compiler_pipeline", "regression", "unrelated"])
        self.assertTrue((root / "full.xml").is_file())

    def test_batch_focused_runner_selects_exact_names_with_a_ctest_regex(self):
        # Real CTest has no `(?:` groups; the selector must be one it compiles,
        # and its escaped names must match only themselves.
        root = self.primary
        (root / "scripts").mkdir()
        (root / "scripts/build_window.py").write_bytes((ROOT / "scripts/build_window.py").read_bytes())
        canary = root / "unselected-ran"
        touch = ('"' + sys.executable + '" -c "import pathlib; pathlib.Path(r\'' +
                 str(canary) + '\').touch()"')
        cmake = ('cmake_minimum_required(VERSION 3.20)\nproject(FakeCompiler NONE)\n'
                 'enable_testing()\nadd_custom_target(gti)\nadd_custom_target(gti_runtime)\n'
                 'add_test(NAME compiler_pipeline COMMAND "' + sys.executable + '" -c "pass")\n'
                 'add_test(NAME regression_2 COMMAND "' + sys.executable + '" -c "pass")\n'
                 'add_test(NAME smoke.gate-1 COMMAND "' + sys.executable + '" -c "pass")\n'
                 'add_test(NAME regression_20 COMMAND ' + touch + ')\n'
                 'add_test(NAME smokeXgate-1 COMMAND ' + touch + ')\n')
        (root / "CMakeLists.txt").write_text(cmake)
        build = root / "build"
        subprocess.run(["cmake", "-S", str(root), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release"],
                       check=True, capture_output=True)
        required = {"compiler_pipeline", "regression_2", "smoke.gate-1"}
        with (root / "focused.log").open("w") as log, unittest.mock.patch.object(validation, "smoke", return_value=["scalar-O0", "scalar-O3", "owners-O0", "owners-O3"]):
            report = validation.run(root, build, required, False, log=log, junit=root / "focused.xml")
        self.assertFalse(canary.exists())
        self.assertEqual(report["scope"], "focused")
        self.assertEqual(report["tests"], sorted(required))

    def test_batch_remote_acceptance_requires_fixed_full_runner_and_real_summary(self):
        state = self.combined()
        base = git(self.primary, "rev-parse", "origin/main")
        head = git(self.root / "combined", "rev-parse", "HEAD")
        report = {"head": head, "scope": "full", "build_type": "Release", "platform": sys.platform,
                  "tests": validation.plan_tests(batch_plan()), "skipped": [],
                  "smoke": ["scalar-O0", "scalar-O3", "owners-O0", "owners-O3"]}
        directory = self.root / "remote-results"
        directory.mkdir()

        def verified(task, steps, remote=False):
            self.assertTrue(remote)
            state = self.flow.load(task)
            state["verified"] = {"head": head, "base": base, "checks": steps,
                                 "remote": {"directory": str(directory)}}
            self.flow.save(state, "ready")
            (directory / "full.log").write_text("GTI_VALIDATION_RESULT=" + json.dumps(report) + "\n")
            return state

        def plan(arguments):
            return [{"name": "combined", "kind": "suite", "cwd": "source", "argv":
                     ["python3", "scripts/validation_checks.py", "full", "--build", "{build}", *arguments]}]

        batches = Batches(self.flow)
        with unittest.mock.patch.object(self.flow, "verify", side_effect=verified) as execute:
            for arguments in ([], ["--base", base, "--base", head], ["--base", head], ["--base", base, "-R", "only-one"]):
                with self.assertRaisesRegex(Error, "Remote batch plans"):
                    batches.verify("combined", remote_steps=plan(arguments))
            wrong_window = plan(["--base", base])
            wrong_window[0]["kind"] = "build"
            with self.assertRaisesRegex(Error, "Remote batch plans"):
                batches.verify("combined", remote_steps=wrong_window)
            execute.assert_not_called()
            batches.verify("combined", remote_steps=plan(["--base", base]))
        self.assertEqual(self.flow.load("combined")["batch_verified"][sys.platform], {"head": head, "base": base})


if __name__ == "__main__":
    unittest.main()
