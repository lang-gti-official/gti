#!/usr/bin/env python3
"""Exercise the rollup bot against a fake GitHub and real disposable Git repositories."""

import contextlib
import copy
from datetime import datetime, timezone
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
import unittest.mock
import urllib.parse

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import agent_workflow as workflow
import rollup
import validation_checks as checks

git = workflow.git
NOW = datetime(2026, 10, 29, 12, 0, tzinfo=timezone.utc)
REPO = {"full_name": workflow.REPOSITORY}


class GitHub:
    """The REST/GraphQL surface the bot uses: PR lists, check runs, labels, comments, the queue and runs."""

    def __init__(self, remote):
        self.remote = remote
        self.prs = {}
        self.checks = {}
        self.queue = set()
        self.runs = []
        self.permissions = {}
        self.writes = []
        self.reject_enqueue = False

    # Fixture helpers.

    def open_pr(self, branch, title, labels=(), body="", draft=False):
        number = len(self.prs) + 1
        head = git(self.remote, "rev-parse", "refs/heads/" + branch)
        git(self.remote, "update-ref", f"refs/pull/{number}/head", head)
        self.prs[number] = {
            "number": number, "node_id": f"PR_{number}", "title": title, "body": body, "draft": draft,
            "state": "open", "merged": False, "merged_at": None, "merge_commit_sha": None,
            "html_url": f"https://example.invalid/pull/{number}",
            "labels": [{"name": name} for name in labels], "comments": [],
            "head": {"ref": branch, "sha": head, "repo": dict(REPO)},
            "base": {"ref": "main", "repo": dict(REPO)}}
        return self.prs[number]

    def set_gate(self, sha, conclusion="success", completed="2026-10-29T01:00:00Z", status="completed"):
        runs = self.checks.setdefault(sha, [])
        runs.append({"id": len(runs) + 1, "name": workflow.GATE_CHECK, "status": status,
                     "conclusion": conclusion if status == "completed" else None, "completed_at": completed,
                     "html_url": f"https://example.invalid/checks/{sha[:7]}/{len(runs) + 1}"})

    def refresh(self, number):
        pr = self.prs[number]
        if pr["state"] == "open":
            current = git(self.remote, "rev-parse", "--verify", "refs/heads/" + pr["head"]["ref"], check=False)
            if current.returncode == 0:
                pr["head"]["sha"] = current.stdout.strip()
                git(self.remote, "update-ref", f"refs/pull/{number}/head", pr["head"]["sha"])
        return pr

    def labels(self, number):
        return [label["name"] for label in self.prs[number]["labels"]]

    # API.

    def request(self, method, path, payload=None):
        if method != "GET":
            self.writes.append((method, path))
        url = urllib.parse.urlsplit(path)
        query = urllib.parse.parse_qs(url.query)
        parts = url.path.split("/")
        if url.path == "/pulls" and method == "GET":
            state = query["state"][0]
            rows = [self.refresh(n) for n in sorted(self.prs)]
            rows = [pr for pr in rows if state == "all" or pr["state"] == state]
            if "head" in query:
                owner, _, branch = query["head"][0].partition(":")
                rows = [pr for pr in rows if pr["head"]["ref"] == branch and owner == workflow.OWNER]
            page = int(query.get("page", ["1"])[0])
            size = int(query["per_page"][0])
            # The list view carries merged_at, never `merged`.
            return [{k: copy.deepcopy(v) for k, v in pr.items() if k not in {"merged", "comments"}}
                    for pr in rows[(page - 1) * size:page * size]]
        if url.path == "/pulls" and method == "POST":
            if git(self.remote, "rev-list", "--count", "main..refs/heads/" + payload["head"]) == "0":
                raise workflow.WorkflowError("No commits between main and " + payload["head"])
            pr = self.open_pr(payload["head"], payload["title"], body=payload["body"], draft=payload["draft"])
            return copy.deepcopy(pr)
        if parts[1] == "pulls":
            pr = self.refresh(int(parts[2]))
            if method == "GET":
                return {k: copy.deepcopy(v) for k, v in pr.items() if k != "comments"}
            if method == "PATCH":
                pr.update(payload)
                return copy.deepcopy(pr)
        if parts[1] == "issues":
            pr = self.prs[int(parts[2])]
            if parts[3] == "labels" and method == "POST":
                for name in payload["labels"]:
                    if name not in self.labels(pr["number"]):
                        pr["labels"].append({"name": name})
                return pr["labels"]
            if parts[3] == "labels" and method == "DELETE":
                name = urllib.parse.unquote(parts[4])
                pr["labels"] = [label for label in pr["labels"] if label["name"] != name]
                return pr["labels"]
            if parts[3] == "comments" and method == "GET":
                return copy.deepcopy(pr["comments"])
            if parts[3] == "comments" and method == "POST":
                pr["comments"].append({"body": payload["body"]})
                return pr["comments"][-1]
        if parts[1] == "commits" and parts[3] == "check-runs":
            return {"check_runs": copy.deepcopy(self.checks.get(parts[2], []))}
        if parts[1] == "collaborators":
            if parts[2] not in self.permissions:
                raise workflow.WorkflowError("GitHub GET failed (404)")
            return dict(self.permissions[parts[2]])
        if url.path == "/actions/workflows/ci.yml/runs":
            assert query["event"] == ["merge_group"]
            return {"workflow_runs": copy.deepcopy(self.runs)}
        raise AssertionError((method, path, payload))

    def graphql(self, query, variables):
        if "enqueuePullRequest" not in query and "mergeQueue(" in query:
            return {"repository": {"mergeQueue": {"entries": {"nodes": [
                {"pullRequest": {"number": number}} for number in sorted(self.queue)]}}}}
        if "enqueuePullRequest" in query:
            self.writes.append(("GRAPHQL", "enqueue"))
            pr = self.refresh(int(variables["id"].split("_")[1]))
            if self.reject_enqueue or pr["draft"] or variables["head"] != pr["head"]["sha"]:
                raise workflow.WorkflowError("server rejected the merge queue entry")
            self.queue.add(pr["number"])
            return {"enqueuePullRequest": {"mergeQueueEntry": {"position": 1}}}
        raise AssertionError(query)

    # The merge queue's outcomes.

    def merge(self, number):
        """Merge a queued PR's head onto main as the queue does after a passing CI gate."""
        pr = self.refresh(number)
        base = git(self.remote, "rev-parse", "main")
        tree = git(self.remote, "merge-tree", "--write-tree", base, pr["head"]["sha"]).splitlines()[0]
        commit = git(self.remote, "commit-tree", tree, "-p", base, "-p", pr["head"]["sha"], "-m", "Merge queue")
        git(self.remote, "update-ref", "refs/heads/main", commit)
        self.queue.discard(number)
        pr.update(merged=True, state="closed", merge_commit_sha=commit, merged_at="2026-10-29T11:00:00Z")
        return commit

    def drop(self, number, conclusion="failure", status="completed"):
        """Remove a queued PR after its merge-group run, reporting `conclusion` (None: no gate at all)."""
        self.queue.discard(number)
        sha = format(len(self.runs) + 1, "040x")
        self.runs.append({"id": len(self.runs) + 1, "head_sha": sha, "status": status,
                          "head_branch": f"gh-readonly-queue/main/pr-{number}-{'a' * 40}",
                          "html_url": f"https://example.invalid/runs/{len(self.runs) + 1}"})
        if conclusion:
            self.set_gate(sha, conclusion)
        return sha


class Repositories(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gti-rollup-test-")
        self.root = Path(self.temp.name).resolve()
        self.remote = self.root / "remote.git"
        subprocess.run(["git", "init", "--bare", "-b", "main", str(self.remote)], check=True, capture_output=True)
        self.dev = self.clone("dev")
        (self.dev / "base.txt").write_text("base\n")
        (self.dev / "shared.txt").write_text("one\ntwo\nthree\n")
        git(self.dev, "add", ".")
        git(self.dev, "commit", "-m", "initial")
        git(self.dev, "push", "origin", "HEAD:refs/heads/main")
        self.bot_root = self.clone("bot")
        self.api = GitHub(self.remote)
        self.out = io.StringIO()

    def tearDown(self):
        self.temp.cleanup()

    def clone(self, name):
        path = self.root / name
        subprocess.run(["git", "clone", "-q", str(self.remote), str(path)], check=True, capture_output=True)
        git(path, "config", "user.name", "Rollup Test")
        git(path, "config", "user.email", "test@example.invalid")
        return path

    def bot(self, **options):
        return rollup.Rollup(self.api, self.bot_root, enforce_origin=False, now=lambda: NOW, out=self.out, **options)

    def main(self):
        return git(self.remote, "rev-parse", "main")

    def member(self, name, files=None, labels=(rollup.MAYBE,), body="", gate="success",
               ready="2026-10-29T01:00:00Z", draft=False, title=None):
        git(self.dev, "fetch", "-q", "origin", "main")
        git(self.dev, "switch", "-q", "-C", "codex/" + name, "origin/main")
        for path, content in (files or {name + ".txt": name + "\n"}).items():
            (self.dev / path).parent.mkdir(parents=True, exist_ok=True)
            (self.dev / path).write_text(content)
            git(self.dev, "add", path)
        git(self.dev, "commit", "-q", "-m", "change " + name)
        git(self.dev, "push", "-q", "origin", "HEAD:refs/heads/codex/" + name)
        pr = self.api.open_pr("codex/" + name, title or ("Change " + name), labels, body, draft)
        if gate:
            self.api.set_gate(pr["head"]["sha"], gate, ready)
        return pr

    def push_more(self, pr, name="more.txt"):
        git(self.dev, "switch", "-q", pr["head"]["ref"])
        (self.dev / name).write_text("more\n")
        git(self.dev, "add", name)
        git(self.dev, "commit", "-q", "-m", "more")
        git(self.dev, "push", "-q", "origin", "HEAD:refs/heads/" + pr["head"]["ref"])
        return self.api.refresh(pr["number"])["head"]["sha"]

    def composed(self, *members, **options):
        report = self.bot(**options).compose()
        self.assertIsNotNone(report["rollup"], report)
        return report, self.api.prs[report["rollup"]["pr"]]


class Selection(unittest.TestCase):
    def test_validation_block_parses_the_pr_grammar_and_drops_anything_else(self):
        body = ("Fixes the thing.\n\nValidation:\n- tier: T1 (confined diagnostic)\n- T0: first_regression\n"
                "- T1: owning_suite, compiler_pipeline,\n  continued_suite, not a name, first_regression\n"
                "- reproduction: fails on the parent\n\n- T1: after_the_block\n")
        self.assertEqual(rollup.parse_validation(body), {
            "T0": ["first_regression"], "T1": ["owning_suite", "compiler_pipeline", "continued_suite"], "tier": "T1"})
        twice = "Validation:\n- T0: a\n\nValidation:\n- T0: b\n"
        self.assertEqual(rollup.parse_validation(twice), {"T0": [], "T1": [], "tier": None})
        self.assertEqual(rollup.parse_validation(None)["T0"], [])

    def test_validation_block_is_the_deduplicated_capped_union(self):
        declared = [{"T0": ["a_regression"], "T1": ["shared", "compiler_pipeline"]},
                    {"T0": ["b_regression", "shared"], "T1": ["shared", "b_owning"]}]
        block = rollup.validation_block(declared)
        self.assertEqual(rollup.parse_validation(block), {
            "T0": ["a_regression", "b_regression", "shared"], "T1": ["compiler_pipeline", "b_owning"],
            "tier": None})
        self.assertEqual(block.count("Validation:"), 1)
        many = [{"T0": [f"t0_{i}" for i in range(40)], "T1": [f"t1_{i}" for i in range(40)]}]
        block = rollup.validation_block(many)
        parsed = rollup.parse_validation(block)
        self.assertEqual(len(parsed["T0"]) + len(parsed["T1"]), rollup.DECLARED_LIMIT)
        self.assertEqual(parsed["T0"], [f"t0_{i}" for i in range(40)])
        self.assertIn("20 further declared tests", block)
        # No member declared anything: the always-run smoke keeps the block valid.
        self.assertEqual(rollup.parse_validation(rollup.validation_block([{"T0": [], "T1": []}]))["T1"],
                         [checks.SMOKE])

    def test_member_plans_are_valid_and_claim_only_declared_evidence(self):
        plan = rollup.member_plan(7, {"T0": ["r"], "T1": ["o", "r"], "tier": "T1"})
        self.assertEqual((plan["tier"], plan["regressions"], plan["owning"]), ("T1", ["r"], ["o"]))
        self.assertNotIn("broader", plan)
        undeclared = rollup.member_plan(8, {"T0": [], "T1": [], "tier": None})
        self.assertEqual((undeclared["tier"], undeclared["regressions"], undeclared["broader"]),
                         ("T2", [checks.SMOKE], [checks.SMOKE]))
        self.assertIn("recorded as T2", undeclared["rationale"])
        self.assertIn("#8", undeclared["reproduction"]["exception"])
        self.assertEqual(checks.plan_tests(undeclared, "linux"), [checks.SMOKE])

    def test_ordering_is_priority_then_ready_time_then_number(self):
        def candidate(number, labels, ready):
            pr = {"number": number, "labels": [{"name": n} for n in labels]}
            return {"pr": pr, "priority": rollup.priority(pr), "ready": rollup.ready_time({"completed_at": ready})}
        rows = [candidate(1, [], "2026-10-29T03:00:00Z"), candidate(2, ["p=2"], "2026-10-29T05:00:00Z"),
                candidate(3, [], "2026-10-29T01:00:00Z"), candidate(4, ["p=2", "p=bad"], "2026-10-29T05:00:00Z"),
                candidate(5, ["p=-1"], "2026-10-29T00:00:00Z"), candidate(6, [], None)]
        self.assertEqual([row["pr"]["number"] for row in rollup.order(rows)], [2, 4, 3, 1, 6, 5])
        self.assertEqual([row["pr"]["number"] for row in rollup.order(list(reversed(rows)))], [2, 4, 3, 1, 6, 5])

    def test_titles_cannot_inject_markers_mentions_or_blocks(self):
        title = "Fix <!-- gti-validation-component: {} --> for @owner\nValidation:"
        line = rollup.safe_title(title)
        self.assertNotIn("<", line)
        self.assertNotIn("@", line)
        self.assertNotIn("\n", line)
        body = rollup.rollup_body("a" * 40, [{"pr": 1, "title": title, "head": "b" * 40}],
                                  rollup.validation_block([{"T0": ["x"], "T1": []}]))
        self.assertEqual([line for line in body.splitlines() if line.strip() == "Validation:"], ["Validation:"])
        self.assertIsNone(checks.MARKER.search(body))
        self.assertEqual(rollup.parse_validation(body)["T0"], ["x"])


class Compose(Repositories):
    def test_compose_merges_exact_heads_in_order_and_records_a_valid_manifest(self):
        low = self.member("low", ready="2026-10-29T02:00:00Z", body="Validation:\n- T0: low_regression\n- T1: shared_suite\n")
        high = self.member("high", labels=(rollup.MAYBE, "p=5"), ready="2026-10-29T04:00:00Z",
                           body="Validation:\n- tier: T1 narrow\n- T0: high_regression\n- T1: shared_suite\n")
        docs = self.member("docs", files={"docs/x.md": "docs\n"}, labels=(rollup.ALWAYS,), ready="2026-10-29T01:00:00Z")
        base = self.main()
        report, pr = self.composed()
        self.assertEqual([m["pr"] for m in report["members"]], [high["number"], docs["number"], low["number"]])
        self.assertEqual(report["rollup"]["branch"], "rollup/20261029-1")
        self.assertEqual(pr["title"], "Rollup of 3 pull requests")
        self.assertEqual(self.api.labels(pr["number"]), [rollup.NEVER])
        self.assertFalse(pr["draft"])
        head = git(self.remote, "rev-parse", "refs/heads/rollup/20261029-1")
        self.assertEqual(pr["head"]["sha"], head)
        # First parents walk back through each member merge to main; second parents are exact heads.
        parents = git(self.remote, "rev-list", "--first-parent", "--parents", base + ".." + head).splitlines()
        self.assertEqual([row.split()[2] for row in reversed(parents) if len(row.split()) == 3],
                         [high["head"]["sha"], docs["head"]["sha"], low["head"]["sha"]])
        self.assertIn(f"#{high['number']}", git(self.remote, "log", "-1", "--format=%B", parents[-1].split()[0]))
        for member in (high, docs, low):
            self.assertIn(f"- #{member['number']} {member['title']} @`{member['head']['sha']}`", pr["body"])
        self.assertEqual(rollup.parse_validation(pr["body"]), {
            "T0": ["high_regression", "low_regression"], "T1": ["shared_suite"], "tier": None})
        # The degraded-mode full runner's manifest check accepts the record.
        git(self.bot_root, "fetch", "-q", "origin", "main", "rollup/20261029-1")
        members = checks.manifests(self.bot_root, base, head)
        self.assertEqual([(m["pr"], m["head"]) for m in members],
                         [(m["pr"], m["head"]) for m in report["members"]])
        self.assertEqual(members[0]["plan"]["tier"], "T1")
        self.assertEqual(members[2]["plan"]["regressions"], ["low_regression"])
        record = json.loads(git(self.remote, "show", head + ":.gti-validation/batches/rollup-20261029-1.json"))
        self.assertEqual((record["schema"], record["base"]), (1, base))
        # The manifest leaves the members' own path classification alone.
        paths = git(self.remote, "diff", "--name-only", base, head).splitlines()
        self.assertEqual(checks.ci_mode({"pull_request": {}}, paths), "full")
        # An open rollup holds its members: a second compose finds too few others.
        self.member("late")
        self.assertIsNone(self.bot().compose()["rollup"])

    def test_exclusions_keep_only_green_ready_unqueued_labelled_prs(self):
        good = [self.member("good-one"), self.member("good-two")]
        self.member("unlabelled", labels=())
        self.member("draft", draft=True)
        self.member("red", gate="failure")
        self.member("pending", gate=None)
        pending = self.member("running", gate=None)
        self.api.set_gate(pending["head"]["sha"], status="in_progress")
        queued = self.member("queued")
        self.api.queue.add(queued["number"])
        self.member("failed", labels=(rollup.MAYBE, rollup.FAILED))
        self.member("never", labels=(rollup.MAYBE, rollup.NEVER))
        self.member("system", files={"cmake/Extra.cmake": "x\n"})
        stale = self.member("stale")
        self.push_more(stale)
        report, _ = self.composed()
        self.assertEqual([m["pr"] for m in report["members"]], [pr["number"] for pr in good])
        reasons = {row["pr"]: row["reason"] for row in report["skipped"]}
        self.assertEqual(sorted(reasons.values()), sorted([
            "draft", "CI gate has not passed on the head", "CI gate has not passed on the head",
            "CI gate has not passed on the head", "CI gate has not passed on the head", "already in the merge queue",
            "labelled rollup-failed", "labelled rollup=never",
            "touches system-risk paths; it must validate alone"]))

    def test_conflicts_are_skipped_never_resolved(self):
        first = self.member("first", files={"shared.txt": "one\nFIRST\nthree\n"})
        second = self.member("second", files={"shared.txt": "one\nSECOND\nthree\n"}, ready="2026-10-29T02:00:00Z")
        third = self.member("third", ready="2026-10-29T03:00:00Z")
        # Main moves under a fourth PR, so that one conflicts with main itself.
        rebase = self.member("rebase", files={"base.txt": "rebase\n"}, ready="2026-10-29T04:00:00Z")
        git(self.dev, "switch", "-q", "-C", "main", "origin/main")
        (self.dev / "base.txt").write_text("main moved\n")
        git(self.dev, "commit", "-q", "-am", "main moves")
        git(self.dev, "push", "-q", "origin", "HEAD:refs/heads/main")
        report, _ = self.composed()
        self.assertEqual([m["pr"] for m in report["members"]], [first["number"], third["number"]])
        reasons = {row["pr"]: row["reason"] for row in report["skipped"]}
        # Conflicting only with an earlier member is not the PR's fault: no label.
        self.assertEqual(reasons[second["number"]], "conflicts with an earlier member")
        self.assertEqual(self.api.labels(second["number"]), [rollup.MAYBE])
        self.assertEqual(reasons[rebase["number"]], "conflicts with main")
        self.assertEqual(self.api.labels(rebase["number"]), [rollup.MAYBE, rollup.REBASE])
        self.assertIn("never resolves conflicts", self.api.prs[rebase["number"]]["comments"][0]["body"])
        self.assertEqual(git(self.remote, "show", "main:base.txt"), "main moved")

    def test_needs_rebase_clears_once_the_pr_merges_cleanly(self):
        self.member("one")
        healed = self.member("two", labels=(rollup.MAYBE, rollup.REBASE))
        report, _ = self.composed()
        self.assertIn(healed["number"], [m["pr"] for m in report["members"]])
        self.assertEqual(self.api.labels(healed["number"]), [rollup.MAYBE])

    def test_rollups_hold_at_most_six_members_by_default(self):
        prs = [self.member(f"m{i}", ready=f"2026-10-29T0{i}:00:00Z") for i in range(8)]
        report, pr = self.composed()
        self.assertEqual([m["pr"] for m in report["members"]], [p["number"] for p in prs[:6]])
        self.assertEqual(pr["title"], "Rollup of 6 pull requests")
        self.assertEqual([row["reason"] for row in report["skipped"]], ["rollup is full"] * 2)
        with self.assertRaises(rollup.RollupError):
            self.bot(max_members=9)

    def test_fewer_than_two_members_is_a_no_op(self):
        self.member("alone")
        self.member("red", gate="failure")
        report = self.bot().compose()
        self.assertIsNone(report["rollup"])
        self.assertEqual(report["reason"], "fewer than two members")
        self.assertEqual(self.api.writes, [])
        self.assertEqual(git(self.remote, "for-each-ref", "refs/heads/rollup"), "")

    def test_identities_skip_existing_branches_and_prs(self):
        self.member("a")
        self.member("b")
        git(self.remote, "update-ref", "refs/heads/rollup/20261029-1", self.main())
        self.assertEqual(self.bot().identity(self.main())[0], "rollup/20261029-2")

    def test_dry_run_makes_no_writes(self):
        self.member("one")
        self.member("two")
        self.member("rebase", files={"base.txt": "x\n"})
        git(self.dev, "switch", "-q", "-C", "main", "origin/main")
        (self.dev / "base.txt").write_text("moved\n")
        git(self.dev, "commit", "-q", "-am", "move")
        git(self.dev, "push", "-q", "origin", "HEAD:refs/heads/main")
        refs = git(self.remote, "for-each-ref")
        bot = self.bot(dry_run=True)
        report = bot.compose()
        self.assertEqual(self.api.writes, [])
        self.assertEqual(git(self.remote, "for-each-ref"), refs)
        self.assertEqual(len(self.api.prs), 3)
        self.assertTrue(any(action.startswith("push ") for action in bot.actions))
        self.assertTrue(any("needs-rebase" in action for action in bot.actions))
        self.assertIn("dry-run: open PR", self.out.getvalue())
        self.assertEqual(report["rollup"]["pr"], None)


class Queue(Repositories):
    def rollup(self, count=2):
        members = [self.member(f"m{i}", ready=f"2026-10-29T0{i}:00:00Z") for i in range(count)]
        report, pr = self.composed()
        self.api.set_gate(pr["head"]["sha"])
        return members, pr

    def test_enqueue_pins_the_green_rollup_head_once(self):
        members, pr = self.rollup()
        pending = self.member("p1"), self.member("p2")
        report, other = self.composed()
        self.assertEqual(self.bot().enqueue(), {"enqueued": [pr["number"]], "waiting": [other["number"]]})
        self.assertEqual(self.api.queue, {pr["number"]})
        self.assertEqual(self.bot().enqueue()["enqueued"], [])
        # A rollup the queue already ran is reconcile's, never re-queued here.
        self.api.drop(pr["number"], "failure")
        self.assertEqual(self.bot().enqueue()["enqueued"], [])

    def test_reconcile_closes_members_a_merged_rollup_integrated(self):
        (first, second, third), pr = self.rollup(3)
        self.bot().enqueue()
        commit = self.api.merge(pr["number"])
        # GitHub marked one member merged itself; another gained commits after composition.
        self.api.prs[first["number"]].update(merged=True, state="closed", merge_commit_sha=commit,
                                             merged_at="2026-10-29T11:00:00Z")
        older = third["head"]["sha"]
        newer = self.push_more(third)
        report = self.bot().reconcile()
        self.assertEqual(report["integrated"], [second["number"]])
        closed = self.api.prs[second["number"]]
        self.assertEqual(closed["state"], "closed")
        self.assertIn(f"via rollup #{pr['number']}", closed["comments"][-1]["body"])
        self.assertEqual(self.api.prs[first["number"]]["comments"], [])
        self.assertEqual(self.api.prs[third["number"]]["state"], "open")
        self.assertIn(rollup.PARTIAL_MARKER, self.api.prs[third["number"]]["comments"][0]["body"])
        self.assertIn(older, self.api.prs[third["number"]]["comments"][0]["body"])
        self.assertNotEqual(newer, older)
        # The closing comment is the proof agent_workflow's sync accepts.
        git(self.bot_root, "fetch", "-q", "origin", "main")
        flow = workflow.Workflow(self.bot_root, self.api, enforce_origin=False)
        proof = flow.rollup_proof(self.api.request("GET", f"/pulls/{second['number']}"), self.main())
        self.assertEqual(proof, {"rollup": pr["number"], "pr_url": pr["html_url"],
                                 "head": second["head"]["sha"], "merge_commit": commit})
        # Idempotent: nothing more to say or close.
        writes = len(self.api.writes)
        self.bot().reconcile()
        self.assertEqual(len(self.api.writes), writes)

    def test_a_failed_rollup_is_closed_and_members_queue_alone(self):
        (first, second), pr = self.rollup()
        self.bot().enqueue()
        self.api.drop(pr["number"], "failure")
        report = self.bot().reconcile()
        self.assertEqual(report["failed"], [pr["number"]])
        self.assertEqual(self.api.prs[pr["number"]]["state"], "closed")
        self.assertEqual(git(self.remote, "for-each-ref", "refs/heads/rollup"), "")
        self.assertIn("Closed: `CI gate` failed", self.api.prs[pr["number"]]["comments"][-1]["body"])
        self.assertEqual(self.api.queue, {first["number"], second["number"]})
        for member in (first, second):
            self.assertIn(rollup.FAILED, self.api.labels(member["number"]))
            comment = self.api.prs[member["number"]]["comments"][-1]["body"]
            self.assertIn("https://example.invalid/checks/", comment)
            self.assertIn("queued this PR alone", comment)
        # Failed members are never rolled up again.
        self.member("x")
        self.member("y")
        report, _ = self.composed()
        self.assertNotIn(first["number"], [m["pr"] for m in report["members"]])

    def test_a_red_pull_request_gate_also_fails_the_rollup(self):
        (first, second), pr = self.rollup()
        self.api.set_gate(pr["head"]["sha"], "failure")
        self.api.set_gate(second["head"]["sha"], "failure")
        self.assertEqual(self.bot().reconcile()["failed"], [pr["number"]])
        self.assertEqual(self.api.queue, {first["number"]})
        self.assertIn("--no-rollup", self.api.prs[second["number"]]["comments"][-1]["body"])

    def test_infrastructure_failures_are_requeued_once(self):
        _, pr = self.rollup()
        self.bot().enqueue()
        self.api.drop(pr["number"], None)
        report = self.bot().reconcile()
        self.assertEqual(report["requeued"], [pr["number"]])
        self.assertIn(pr["number"], self.api.queue)
        self.assertEqual(self.api.prs[pr["number"]]["state"], "open")
        self.api.drop(pr["number"], "cancelled")
        report = self.bot().reconcile()
        self.assertEqual(report["failed"], [pr["number"]])
        self.assertIn("dropped it again", self.api.prs[pr["number"]]["comments"][-1]["body"])

    def test_waiting_rollups_are_left_alone(self):
        _, pr = self.rollup()
        self.bot().enqueue()
        self.assertEqual(self.bot().reconcile()["waiting"], [pr["number"]])
        self.api.queue.discard(pr["number"])
        self.api.drop(pr["number"], "success")
        self.assertEqual(self.bot().reconcile()["waiting"], [pr["number"]])
        self.assertEqual(self.api.prs[pr["number"]]["state"], "open")

    def test_a_member_that_moves_before_the_queue_makes_the_rollup_stale(self):
        (first, second), pr = self.rollup()
        self.push_more(first)
        report = self.bot().reconcile()
        self.assertEqual(report["stale"], [pr["number"]])
        self.assertEqual(self.api.prs[pr["number"]]["state"], "closed")
        self.assertEqual(self.api.labels(first["number"]), [rollup.MAYBE])
        self.assertEqual(self.api.labels(second["number"]), [rollup.MAYBE])

    def test_reconcile_dry_run_makes_no_writes(self):
        _, pr = self.rollup()
        self.bot().enqueue()
        self.api.drop(pr["number"], "failure")
        writes, refs = list(self.api.writes), git(self.remote, "for-each-ref")
        bot = self.bot(dry_run=True)
        self.assertEqual(bot.reconcile()["failed"], [pr["number"]])
        self.assertEqual((self.api.writes, git(self.remote, "for-each-ref")), (writes, refs))
        self.assertTrue(any(action.startswith("close ") for action in bot.actions))


class Trigger(unittest.TestCase):
    def test_only_an_exact_rollup_comment_from_a_writer_runs_the_bot(self):
        api = GitHub(None)
        api.permissions = {"writer": {"permission": "write", "role_name": "write"},
                           "maintainer": {"permission": "write", "role_name": "maintain"},
                           "admin": {"permission": "admin", "role_name": "admin"},
                           "triager": {"permission": "read", "role_name": "triage"},
                           "reader": {"permission": "read", "role_name": "read"}}
        def event(body="/rollup", login="writer", action="created"):
            return {"action": action, "comment": {"body": body, "user": {"login": login}}}
        self.assertTrue(rollup.authorize("schedule", {}, None))
        self.assertTrue(rollup.authorize("workflow_dispatch", {}, None))
        for login in ("writer", "maintainer", "admin"):
            self.assertTrue(rollup.authorize("issue_comment", event(login=login), api), login)
        for login in ("triager", "reader", "stranger", "rollup[bot]", "../writer"):
            self.assertFalse(rollup.authorize("issue_comment", event(login=login), api), login)
        for body in ("/rollup please", " /rollup", "/ROLLUP", "", None):
            self.assertFalse(rollup.authorize("issue_comment", event(body=body), api), body)
        self.assertFalse(rollup.authorize("issue_comment", event(action="edited"), api))
        self.assertFalse(rollup.authorize("pull_request_target", event(), api))
        self.assertEqual(api.writes, [])

    def test_authorize_command_prints_a_step_output(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "event.json"
            path.write_text(json.dumps({"action": "created", "comment": {"body": "hello", "user": {"login": "x"}}}))
            for name, expected in (("schedule", "authorized=true"), ("issue_comment", "authorized=false")):
                output = io.StringIO()
                with contextlib.redirect_stdout(output), \
                        unittest.mock.patch.dict(os.environ, {"GH_TOKEN": "read-only"}):
                    self.assertEqual(rollup.main(["authorize", "--event-name", name, "--event", str(path)]), 0)
                self.assertEqual(output.getvalue().strip(), expected)

    def test_missing_app_token_fails_clearly_and_never_uses_github_token(self):
        for command in ("compose", "enqueue", "reconcile"):
            error = io.StringIO()
            with unittest.mock.patch.dict(os.environ, {"GITHUB_TOKEN": "workflow-token"}, clear=True), \
                    contextlib.redirect_stderr(error), \
                    unittest.mock.patch.object(workflow.subprocess, "run") as run:
                self.assertEqual(rollup.main([command]), 1)
                run.assert_not_called()
            for needle in ("GH_TOKEN", "GTI_ROLLUP_APP_ID", "GTI_ROLLUP_APP_PRIVATE_KEY", "never with GITHUB_TOKEN",
                           ".github/README.md", "Rollup bot"):
                self.assertIn(needle, error.getvalue())

    def test_workflow_fails_naming_both_missing_secrets(self):
        text = (ROOT / ".github/workflows/rollup.yml").read_text()
        step = text.split("- name: Require the rollup GitHub App secrets\n", 1)[1].split("\n      - name:", 1)[0]
        lines = step.split("        run: |\n", 1)[1].splitlines()
        script = "\n".join(line[10:] for line in lines if line.startswith("          ") or not line)
        self.assertIn("secrets.GTI_ROLLUP_APP_ID", step)
        self.assertIn("secrets.GTI_ROLLUP_APP_PRIVATE_KEY", step)
        cases = (({}, ["GTI_ROLLUP_APP_ID GTI_ROLLUP_APP_PRIVATE_KEY"]),
                 ({"GTI_ROLLUP_APP_ID": "1"}, ["secret(s): GTI_ROLLUP_APP_PRIVATE_KEY."]),
                 ({"GTI_ROLLUP_APP_ID": "1", "GTI_ROLLUP_APP_PRIVATE_KEY": "k"}, None))
        for env, expected in cases:
            result = subprocess.run(["/bin/bash", "-e", "-c", script], capture_output=True, text=True,
                                    env={"PATH": os.environ["PATH"], "GTI_ROLLUP_APP_ID": "",
                                         "GTI_ROLLUP_APP_PRIVATE_KEY": "", **env})
            if expected is None:
                self.assertEqual(result.returncode, 0)
                continue
            self.assertEqual(result.returncode, 1)
            for needle in (*expected, ".github/README.md", "Rollup bot", "never writes with GITHUB_TOKEN"):
                self.assertIn(needle, result.stdout)

    def test_git_runs_without_hooks_and_with_credentials_only_in_the_environment(self):
        with tempfile.TemporaryDirectory() as directory:
            subprocess.run(["git", "init", "-q", directory], check=True)
            bot = rollup.Rollup(GitHub(None), directory, token="app-token", enforce_origin=False)
            with unittest.mock.patch.object(rollup.subprocess, "run") as run:
                run.return_value = subprocess.CompletedProcess([], 0, "", "")
                bot.git("status")
            argv, env = run.call_args.args[0], run.call_args.kwargs["env"]
            self.assertEqual(argv[:3], ["git", "-c", "core.hooksPath=/dev/null"])
            self.assertNotIn("app-token", " ".join(argv))
            self.assertEqual(env["GIT_CONFIG_KEY_0"], "http.https://github.com/.extraheader")
            self.assertNotIn("app-token", env["GIT_CONFIG_VALUE_0"])  # base64, never plain
            subprocess.run(["git", "-C", directory, "remote", "add", "origin", "https://example.invalid/gti.git"], check=True)
            with self.assertRaisesRegex(rollup.RollupError, "origin must be canonical"):
                rollup.Rollup(GitHub(None), directory)


if __name__ == "__main__":
    unittest.main()
