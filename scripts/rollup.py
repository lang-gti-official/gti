#!/usr/bin/env python3
"""Rollups: compose green, rollup-labelled PRs into one merge-queue entry and reconcile the queue's verdict.

One complete merge-queue run then covers several pull requests. `compose`
merges the exact heads of eligible PRs onto current main as a `rollup/<date>-<n>`
branch with a membership manifest and opens its PR; `enqueue` queues a rollup
whose `CI gate` passed; `reconcile` closes members a merged rollup integrated,
and turns a failed rollup back into individual queue entries. The bot never
resolves conflicts, never checks out or runs PR content (trial merges are Git
plumbing), and never forces anything. `--dry-run` prints planned writes only.
"""

from __future__ import annotations

import argparse
import base64
from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import urllib.parse

import agent_workflow as workflow
import validation_checks as checks

MAYBE, ALWAYS, NEVER = workflow.ROLLUP_MAYBE, workflow.ROLLUP_ALWAYS, workflow.ROLLUP_NEVER
FAILED, REBASE = workflow.ROLLUP_FAILED, workflow.ROLLUP_REBASE
ELIGIBLE = {MAYBE, ALWAYS}
GATE = workflow.GATE_CHECK
OWNER, NAME = workflow.REPOSITORY.split("/")
PRIORITY = re.compile(r"p=(-?[0-9]{1,6})\Z")
MAX_MEMBERS = 6
# validation_checks.manifests() accepts at most eight members.
MANIFEST_LIMIT = 8
DECLARED_LIMIT = 60
DECLARATION_ITEM = re.compile(r"-\s+([A-Za-z0-9_-]+):\s*(.*)")
QUEUE_BRANCH = re.compile(r"gh-readonly-queue/main/pr-([0-9]+)-[0-9a-f]{40}\Z")
SHA = re.compile(r"[0-9a-f]{40}\Z")
REQUEUE_MARKER = "<!-- gti-rollup-requeue -->"
PARTIAL_MARKER = "<!-- gti-rollup-partial -->"
SECRETS = ("GTI_ROLLUP_APP_ID", "GTI_ROLLUP_APP_PRIVATE_KEY")
SETUP = '.github/README.md, section "Rollup bot"'
MISSING_TOKEN = ("GH_TOKEN is not set. The rollup bot writes only with a GitHub App installation token minted "
                 "from the " + " and ".join(SECRETS) + " repository secrets, never with GITHUB_TOKEN; see "
                 + SETUP + ".")
ENQUEUE = workflow.Workflow.ENQUEUE
QUEUE = ("query($owner: String!, $name: String!) { repository(owner: $owner, name: $name) { "
         "mergeQueue(branch: \"main\") { entries(first: 100) { nodes { pullRequest { number } } } } } }")


class RollupError(RuntimeError):
    pass


# Pure selection, ordering and record construction.

def labels(pr):
    return workflow.label_names(pr)


def priority(pr):
    values = [int(match.group(1)) for name in labels(pr) if (match := PRIORITY.fullmatch(name or ""))]
    return max(values, default=0)


def is_rollup(pr):
    head, base = pr.get("head") or {}, pr.get("base") or {}
    return bool(workflow.ROLLUP_BRANCH.fullmatch(head.get("ref") or "") and base.get("ref") == "main"
                and (head.get("repo") or {}).get("full_name") == workflow.REPOSITORY)


def manifest_path(branch):
    return checks.BATCH_DIRECTORY + "rollup-" + branch.split("/", 1)[1] + ".json"


def exclusion(pr, queued, busy):
    """Why an open PR is not a rollup candidate before its checks are read, or None."""
    names = labels(pr)
    if not names & ELIGIBLE:
        return "not labelled rollup=maybe or rollup=always"
    if pr.get("draft"):
        return "draft"
    if (pr.get("base") or {}).get("ref") != "main":
        return "not based on main"
    for name in (NEVER, FAILED):
        if name in names:
            return "labelled " + name
    if pr["number"] in queued:
        return "already in the merge queue"
    if pr["number"] in busy:
        return "already in an open rollup"
    return None


def green(run):
    return bool(run) and run.get("status") == "completed" and run.get("conclusion") == "success"


def ready_time(run):
    try:
        return datetime.fromisoformat(str(run.get("completed_at")).replace("Z", "+00:00"))
    except ValueError:
        return datetime.max.replace(tzinfo=timezone.utc)


def order(candidates):
    """Priority descending, then the time each became green, then PR number."""
    return sorted(candidates, key=lambda c: (-c["priority"], c["ready"], c["pr"]["number"]))


def parse_validation(body):
    """The T0/T1 CTest names and tier a PR body's `Validation:` block declares.

    Same grammar as PR CI: one `Validation:` line, then `- key: value` items
    (continued on indented lines) up to a blank line or fence. T0/T1 values are
    comma-separated CTest names; anything else there is dropped, never used.
    """
    result = {"T0": [], "T1": [], "tier": None}
    lines = (body or "").splitlines()
    starts = [index for index, line in enumerate(lines) if line.strip() == "Validation:"]
    if len(starts) != 1:
        return result
    key = None
    for line in lines[starts[0] + 1:]:
        stripped = line.strip()
        if not stripped or stripped.startswith("```"):
            break
        item = DECLARATION_ITEM.fullmatch(stripped)
        if item:
            key, value = item.groups()
        elif line[:1].isspace() and key is not None:
            value = stripped
        else:
            break
        if key in ("T0", "T1"):
            for name in (token.strip() for token in value.split(",")):
                if name and checks.NAME.fullmatch(name) and name not in result["T0"] and name not in result["T1"]:
                    result[key].append(name)
        elif key == "tier" and item and result["tier"] is None:
            tier = re.match(r"(T[0-3])\b", value)
            result["tier"] = tier.group(1) if tier else None
    return result


def validation_block(declarations, limit=DECLARED_LIMIT):
    """The rollup's `Validation:` block: the members' T0 then T1 names, deduplicated, at most `limit`."""
    chosen, seen, dropped = {"T0": [], "T1": []}, set(), 0
    for key in ("T0", "T1"):
        for declared in declarations:
            for name in declared[key]:
                if name in seen:
                    continue
                if len(seen) >= limit:
                    dropped += 1
                    continue
                seen.add(name)
                chosen[key].append(name)
    if not seen:
        # Every full-mode PR run includes the compiler smoke anyway.
        chosen["T1"] = [checks.SMOKE]
    lines = ["Validation:", "- scope: union of the members' declared T0/T1 tests; "
             "the merge queue runs the complete suite on this candidate"]
    lines += [f"- {key}: " + ", ".join(names) for key, names in chosen.items() if names]
    if dropped:
        lines.append(f"- omitted: {dropped} further declared tests beyond the {limit}-name limit")
    return "\n".join(lines)


def member_plan(number, declared):
    """A validate_plan()-valid plan that claims no evidence the member did not declare.

    Members are ordinary queue-validated PRs, not batch admissions, so no
    admission plan exists. The plan names exactly the member's declared T0/T1
    tests (the compiler smoke when it declared none), records an undeclared
    tier as T2, the highest tier a rollup admits, and says where reproduction
    and cross-phase reasoning live. A degraded-mode full run then requires
    those named tests to pass on the combined candidate.
    """
    t0, t1 = declared["T0"], [name for name in declared["T1"] if name not in declared["T0"]]
    tier = declared.get("tier") if declared.get("tier") in {"T1", "T2"} else "T2"
    regressions, owning = t0 or t1 or [checks.SMOKE], t1 or t0 or [checks.SMOKE]
    notes = []
    if not (t0 or t1):
        notes.append("it declared no T0/T1 tests, so only the compiler smoke is named")
    if declared.get("tier") not in {"T1", "T2"}:
        notes.append("its tier was not declared, so it is recorded as T2")
    plan = {
        "tier": tier,
        "rationale": (f"Rollup member #{number}: its declared Validation T0/T1 tests; the merge queue runs the "
                      "complete suite on the combined candidate" + ("; " + "; ".join(notes) if notes else "")),
        "regressions": regressions,
        "owning": owning,
        "cross_phase": [],
        "unaffected": "Not assessed per member by the rollup bot; the merge queue runs every registered test",
        "targets": ["gti"],
        "platforms": ["darwin", "linux"],
        "reproduction": {"exception": f"Recorded in the description of #{number}; a rollup reproduces no member failure"},
    }
    if tier != "T1":
        plan["broader"] = list(owning)
    return checks.validate_plan(plan)


def safe_title(title):
    """A PR title as one inert line: no HTML comments or markers, mentions or code spans."""
    text = " ".join(str(title or "").split())
    return re.sub(r"[<>`]", "", text).replace("@", "(at)")[:200]


def rollup_body(base, members, block):
    lines = [f"Rollup of {len(members)} pull requests, merged in this order onto `main` at `{base}`:", ""]
    lines += [f"- #{member['pr']} {safe_title(member['title'])} @`{member['head']}`" for member in members]
    lines += ["", "Composed by `scripts/rollup.py`. Each member keeps its own focused evidence; the merge queue "
              "runs the complete suite once for all of them. If the queue rejects this rollup, the bot closes it, "
              "labels every member `rollup-failed` and queues each one alone. Do not push to this branch; close the "
              "PR to stop it.", "", block]
    return "\n".join(lines) + "\n"


def authorize(event_name, event, api):
    """Whether this event may run the bot: schedule/dispatch, or an exact `/rollup` from a writer."""
    if event_name in {"schedule", "workflow_dispatch"}:
        return True
    if event_name != "issue_comment" or event.get("action") != "created":
        return False
    comment = event.get("comment") or {}
    login = (comment.get("user") or {}).get("login") or ""
    if comment.get("body") != "/rollup" or not re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,38})", login):
        return False
    try:
        permission = api.request("GET", "/collaborators/" + urllib.parse.quote(login) + "/permission") or {}
    except workflow.WorkflowError:
        return False
    return permission.get("permission") in {"admin", "write"} or permission.get("role_name") in {"admin", "maintain", "write"}


class Rollup:
    def __init__(self, api, repo, dry_run=False, runs_api=None, token=None, enforce_origin=True,
                 now=None, out=None, max_members=MAX_MEMBERS, max_open=2, merged_days=3):
        if not 2 <= max_members <= MANIFEST_LIMIT:
            raise RollupError(f"A rollup holds between 2 and {MANIFEST_LIMIT} members")
        self.api, self.runs_api = api, runs_api or api
        self.dry_run, self.token = dry_run, token
        self.repo = Path(repo)
        self.now = now or (lambda: datetime.now(timezone.utc))
        self.out = out or sys.stdout
        self.max_members, self.max_open, self.merged_days = max_members, max_open, merged_days
        self.actions = []
        self.repo = Path(self.git("rev-parse", "--show-toplevel"))
        if enforce_origin and (self.git("remote", "get-url", "origin") != workflow.CANONICAL or
                               self.git("remote", "get-url", "--push", "--all", "origin") != workflow.CANONICAL):
            raise RollupError("origin must be canonical GTI on GitHub; the token is never sent to other hosts")

    # Git: plumbing only, with hooks disabled and no custom merge drivers.

    def git(self, *args, check=True, stdin=None, env=None):
        environment = dict(os.environ, **(env or {}))
        if self.token:
            # Credentials reach Git through the environment, never argv or .git/config.
            basic = base64.b64encode(("x-access-token:" + self.token).encode()).decode()
            environment.update(GIT_CONFIG_COUNT="1", GIT_CONFIG_KEY_0="http.https://github.com/.extraheader",
                               GIT_CONFIG_VALUE_0="AUTHORIZATION: basic " + basic)
        environment["GIT_TERMINAL_PROMPT"] = "0"
        result = subprocess.run(["git", "-c", "core.hooksPath=/dev/null", "-C", str(self.repo), *args],
                                capture_output=True, text=True, input=stdin, env=environment, check=False)
        if check and result.returncode:
            raise RollupError("git " + args[0] + " failed: " + (result.stderr.strip() or result.stdout.strip())[:400])
        return result.stdout.strip() if check else result

    def ancestor(self, older, newer):
        return self.git("merge-base", "--is-ancestor", older, newer, check=False).returncode == 0

    def fetch_main(self):
        self.git("fetch", "--no-tags", "--quiet", "origin", "+refs/heads/main:refs/gti-rollup/main")
        return self.git("rev-parse", "refs/gti-rollup/main")

    def fetch_pull(self, number):
        """The PR's head as GitHub publishes it (forks included), or None."""
        ref = f"refs/gti-rollup/pull/{number}"
        if self.git("fetch", "--no-tags", "--quiet", "origin", f"+refs/pull/{number}/head:{ref}", check=False).returncode:
            return None
        return self.git("rev-parse", ref)

    def merge_tree(self, first, second):
        """The clean automatic merge tree of two commits, or None on conflict."""
        result = self.git("merge-tree", "--write-tree", "--no-messages", first, second, check=False)
        if result.returncode == 1:
            return None
        if result.returncode:
            raise RollupError("git merge-tree failed: " + result.stderr.strip()[:400])
        return result.stdout.split()[0]

    def merge(self, tip, pr, head):
        tree = self.merge_tree(tip, head)
        if tree is None:
            return None
        message = f"Merge #{pr['number']}: {safe_title(pr.get('title'))}\n\nRollup member at {head}.\n"
        return self.git("commit-tree", tree, "-p", tip, "-p", head, "-m", message)

    def commit_file(self, tip, path, contents, message):
        blob = self.git("hash-object", "-w", "--stdin", stdin=contents)
        with tempfile.TemporaryDirectory(prefix="gti-rollup-index-") as directory:
            env = {"GIT_INDEX_FILE": str(Path(directory) / "index")}
            self.git("read-tree", tip, env=env)
            self.git("update-index", "--add", "--cacheinfo", f"100644,{blob},{path}", env=env)
            tree = self.git("write-tree", env=env)
        return self.git("commit-tree", tree, "-p", tip, "-m", message)

    # GitHub.

    def write(self, description, operation):
        self.actions.append(description)
        if self.dry_run:
            print("dry-run: " + description, file=self.out)
            return None
        return operation()

    def pulls(self, state, pages=None):
        rows, page = [], 1
        while True:
            query = {"state": state, "base": "main", "per_page": 100, "page": page}
            if state == "closed":
                query.update(sort="updated", direction="desc")
            batch = self.api.request("GET", "/pulls?" + urllib.parse.urlencode(query)) or []
            rows += batch
            if len(batch) < 100 or (pages and page >= pages):
                return rows
            page += 1

    def pull(self, number):
        return self.api.request("GET", f"/pulls/{number}")

    def gate(self, sha):
        query = urllib.parse.urlencode({"check_name": GATE, "per_page": 100})
        runs = [run for run in (self.api.request("GET", f"/commits/{sha}/check-runs?{query}") or {}).get("check_runs", [])
                if run.get("name") == GATE]
        return max(runs, key=lambda run: run.get("id", 0)) if runs else None

    def queued(self):
        data = self.api.graphql(QUEUE, {"owner": OWNER, "name": NAME}) or {}
        queue = (data.get("repository") or {}).get("mergeQueue") or {}
        return {node["pullRequest"]["number"] for node in (queue.get("entries") or {}).get("nodes") or []
                if node and node.get("pullRequest")}

    def queue_runs(self):
        """The latest merge-queue CI run for each PR number (read-only, so GITHUB_TOKEN may list it)."""
        query = urllib.parse.urlencode({"event": "merge_group", "per_page": 100})
        rows = (self.runs_api.request("GET", "/actions/workflows/ci.yml/runs?" + query) or {}).get("workflow_runs", [])
        latest = {}
        for run in rows:
            match = QUEUE_BRANCH.fullmatch(run.get("head_branch") or "")
            if match and run.get("id", 0) > latest.get(int(match.group(1)), {}).get("id", -1):
                latest[int(match.group(1))] = run
        return latest

    def comments(self, number):
        return self.api.request("GET", f"/issues/{number}/comments?per_page=100") or []

    def has_comment(self, number, marker):
        return any(marker in (comment.get("body") or "") for comment in self.comments(number))

    def label(self, number, name):
        self.write(f"label #{number} {name}",
                   lambda: self.api.request("POST", f"/issues/{number}/labels", {"labels": [name]}))

    def unlabel(self, number, name):
        self.write(f"remove label {name} from #{number}",
                   lambda: self.api.request("DELETE", f"/issues/{number}/labels/" + urllib.parse.quote(name, safe="")))

    def comment(self, number, body):
        self.write(f"comment on #{number}: {body.splitlines()[0][:120]}",
                   lambda: self.api.request("POST", f"/issues/{number}/comments", {"body": body}))

    def close(self, number):
        self.write(f"close #{number}", lambda: self.api.request("PATCH", f"/pulls/{number}", {"state": "closed"}))

    def enqueue_pr(self, pr, head):
        self.write(f"enqueue #{pr['number']} at {head}",
                   lambda: self.api.graphql(ENQUEUE, {"id": pr["node_id"], "head": head}))

    def delete_branch(self, branch, head):
        ref = "refs/heads/" + branch
        remote = self.git("ls-remote", "origin", ref)
        if remote and remote.split()[0] == head:
            # Conditional delete: a commit pushed meanwhile keeps the branch.
            self.write(f"delete branch {branch}",
                       lambda: self.git("push", "--force-with-lease=" + ref + ":" + head, "origin", ":" + ref))

    def manifest(self, pr):
        """The membership record committed at a rollup PR's head, or None if absent or malformed."""
        head = pr["head"]["sha"]
        if self.fetch_pull(pr["number"]) is None and self.git("cat-file", "-e", head, check=False).returncode:
            return None
        result = self.git("show", head + ":" + manifest_path(pr["head"]["ref"]), check=False)
        try:
            record = json.loads(result.stdout) if result.returncode == 0 else None
        except ValueError:
            return None
        if (not isinstance(record, dict) or record.get("schema") != 1 or not isinstance(record.get("members"), list)
                or not all(isinstance(m, dict) and isinstance(m.get("pr"), int) and SHA.fullmatch(str(m.get("head", "")))
                           for m in record["members"])):
            return None
        return record

    # Commands.

    def compose(self):
        base = self.fetch_main()
        open_prs = self.pulls("open")
        rollups = [pr for pr in open_prs if is_rollup(pr)]
        report = {"base": base, "rollup": None, "members": [], "skipped": []}
        if len(rollups) >= self.max_open:
            report["reason"] = f"{len(rollups)} rollups are already open"
            return report
        busy = set()
        for pr in rollups:
            record = self.manifest(pr)
            busy |= {member["pr"] for member in (record or {}).get("members", [])}
        queued = self.queued()
        candidates = []
        for pr in open_prs:
            if is_rollup(pr):
                continue
            reason = exclusion(pr, queued, busy)
            if reason is None:
                run = self.gate(pr["head"]["sha"])
                if not green(run):
                    reason = "CI gate has not passed on the head"
            if reason:
                if labels(pr) & ELIGIBLE:
                    report["skipped"].append({"pr": pr["number"], "reason": reason})
                continue
            candidates.append({"pr": pr, "ready": ready_time(run), "priority": priority(pr),
                               "declared": parse_validation(pr.get("body"))})
        tip, members = base, []
        for candidate in order(candidates):
            pr = candidate["pr"]
            number, head = pr["number"], pr["head"]["sha"]
            if len(members) >= self.max_members:
                report["skipped"].append({"pr": number, "reason": "rollup is full"})
                continue
            reason = None
            if self.fetch_pull(number) != head:
                reason = "head moved since its checks"
            elif self.ancestor(head, base):
                reason = "head is already in main"
            elif checks.system_risk(filter(None, self.git("log", "--format=", "--name-only", base + ".." + head).splitlines())):
                reason = "touches system-risk paths; it must validate alone"
            elif self.merge_tree(base, head) is None:
                reason = "conflicts with main"
                if REBASE not in labels(pr):
                    self.label(number, REBASE)
                    self.comment(number, f"This PR conflicts with `main` at `{base}`, so the rollup bot skipped it. "
                                         "Reconcile it with main; the bot removes `needs-rebase` once it merges cleanly. "
                                         "The bot never resolves conflicts.")
            if reason:
                report["skipped"].append({"pr": number, "reason": reason})
                continue
            if REBASE in labels(pr):
                self.unlabel(number, REBASE)
            merged = self.merge(tip, pr, head)
            if merged is None:
                # Not this PR's fault against main: it joins a later rollup.
                report["skipped"].append({"pr": number, "reason": "conflicts with an earlier member"})
                continue
            tip = merged
            members.append({"pr": number, "head": head, "title": pr.get("title"), "declared": candidate["declared"]})
        report["members"] = [{"pr": m["pr"], "head": m["head"]} for m in members]
        if len(members) < 2:
            report["reason"] = "fewer than two members"
            return report
        branch, path = self.identity(base)
        record = {"schema": 1, "base": base, "members": [
            {"pr": m["pr"], "head": m["head"], "plan": member_plan(m["pr"], m["declared"])} for m in members]}
        commit = self.commit_file(tip, path, json.dumps(record, indent=2) + "\n",
                                  f"Record rollup membership: " + ", ".join(f"#{m['pr']}" for m in members) + "\n")
        title = f"Rollup of {len(members)} pull requests"
        body = rollup_body(base, members, validation_block([m["declared"] for m in members]))
        ref = "refs/heads/" + branch
        self.write(f"push {commit} to {branch}",
                   lambda: self.git("push", "--force-with-lease=" + ref + ":", "origin", commit + ":" + ref))
        created = self.write(f"open PR {title!r} from {branch}", lambda: self.api.request(
            "POST", "/pulls", {"title": title, "head": branch, "base": "main", "body": body, "draft": False}))
        if created is not None:
            self.label(created["number"], NEVER)
        report.update(rollup={"branch": branch, "manifest": path, "head": commit, "title": title,
                              "pr": created and created["number"]}, body=body)
        return report

    def identity(self, base):
        day = self.now().astimezone(timezone.utc).strftime("%Y%m%d")
        existing = {line.split()[1] for line in self.git("ls-remote", "origin", "refs/heads/rollup/*").splitlines()}
        for index in range(1, 10000):
            branch = f"rollup/{day}-{index}"
            path = manifest_path(branch)
            if "refs/heads/" + branch in existing or not self.git("cat-file", "-e", base + ":" + path, check=False).returncode:
                continue
            query = urllib.parse.urlencode({"state": "all", "head": OWNER + ":" + branch, "per_page": 100})
            if self.api.request("GET", "/pulls?" + query):
                continue
            return branch, path
        raise RollupError("No unused rollup identity today")

    def enqueue(self, number=None):
        queued, history = self.queued(), self.queue_runs()
        report = {"enqueued": [], "waiting": []}
        for pr in self.pulls("open"):
            if not is_rollup(pr) or (number is not None and pr["number"] != number):
                continue
            head = pr["head"]["sha"]
            if pr.get("draft") or pr["number"] in queued or pr["number"] in history or not green(self.gate(head)):
                # A rollup the queue already ran belongs to reconcile.
                report["waiting"].append(pr["number"])
                continue
            self.enqueue_pr(pr, head)
            report["enqueued"].append(pr["number"])
        return report

    def reconcile(self):
        main = self.fetch_main()
        queued, history = self.queued(), self.queue_runs()
        report = {"waiting": [], "failed": [], "requeued": [], "stale": [], "integrated": [], "invalid": []}
        for pr in self.pulls("open"):
            if is_rollup(pr):
                self.reconcile_open(pr, queued, history, report)
        horizon = self.now() - timedelta(days=self.merged_days)
        for pr in self.pulls("closed", pages=1):
            merged = pr.get("merged_at")
            if is_rollup(pr) and merged and datetime.fromisoformat(merged.replace("Z", "+00:00")) >= horizon:
                self.reconcile_merged(pr, main, report)
        return report

    def reconcile_open(self, pr, queued, history, report):
        number, head = pr["number"], pr["head"]["sha"]
        record = self.manifest(pr)
        if record is None:
            report["invalid"].append(number)
            return
        if number in queued:
            report["waiting"].append(number)
            return
        run = history.get(number)
        if run is None:
            gate = self.gate(head)
            if gate and gate.get("status") == "completed" and gate.get("conclusion") == "failure":
                return self.fail(pr, record, gate.get("html_url") or pr.get("html_url"),
                                 "its pull-request `CI gate` failed", queued, report)
            members = [self.pull(m["pr"]) for m in record["members"]]
            if any(m["state"] != "open" or m["head"]["sha"] != r["head"] for m, r in zip(members, record["members"])):
                # A member moved or closed before the queue ran: rebuild from current heads.
                self.comment(number, "A member changed or closed before this rollup entered the merge queue, so the "
                                     "bot closed it; its open members return to the rollup pool.")
                self.close(number)
                self.delete_branch(pr["head"]["ref"], head)
                report["stale"].append(number)
                return
            report["waiting"].append(number)
            return
        if run.get("status") != "completed":
            report["waiting"].append(number)
            return
        gate = self.gate(run["head_sha"]) if run.get("head_sha") else None
        if green(gate):
            report["waiting"].append(number)  # the queue is merging it
            return
        url = (gate or {}).get("html_url") or run.get("html_url")
        if gate and gate.get("status") == "completed" and gate.get("conclusion") == "failure":
            return self.fail(pr, record, url, "`CI gate` failed on its merge-queue candidate", queued, report)
        # Cancelled, timed out or never reported: infrastructure, retried once.
        if not self.has_comment(number, REQUEUE_MARKER) and green(self.gate(head)):
            self.comment(number, f"{REQUEUE_MARKER}\nThe merge queue dropped this rollup without a `CI gate` failure "
                                 f"({url}); the bot re-queued it once.")
            self.enqueue_pr(pr, head)
            report["requeued"].append(number)
            return
        self.fail(pr, record, url, "the merge queue dropped it again without a `CI gate` verdict", queued, report)

    def fail(self, pr, record, url, reason, queued, report):
        """Close a failed rollup and validate each member alone, so the queue isolates the culprit.

        Members are handled first and each step is idempotent, so an interrupted
        run is repaired by the next one while the rollup is still open.
        TODO(rollup bisection): split the members in halves before falling
        back to single entries once queue capacity makes that cheaper.
        """
        number = pr["number"]
        for member in record["members"]:
            current = self.pull(member["pr"])
            if current["state"] != "open":
                continue
            head = current["head"]["sha"]
            alone = (not current.get("draft") and current["number"] not in queued and green(self.gate(head)))
            if FAILED not in labels(current):
                self.label(current["number"], FAILED)
                self.comment(current["number"], f"Rollup #{number} failed: {reason} ({url}). " + (
                    "The bot queued this PR alone, so the merge queue validates it by itself." if alone else
                    "Run `finish <task> --no-rollup` once this PR's `CI gate` passes to queue it alone."))
            if alone:
                self.enqueue_pr(current, head)
        self.comment(number, f"Closed: {reason} ({url}). Every member is labelled `rollup-failed` and queued alone.")
        self.close(number)
        self.delete_branch(pr["head"]["ref"], pr["head"]["sha"])
        report["failed"].append(number)

    def reconcile_merged(self, pr, main, report):
        number = pr["number"]
        detail = self.pull(number)
        commit = detail.get("merge_commit_sha")
        record = self.manifest(detail)
        if record is None or not commit or not self.ancestor(commit, main):
            return
        for member in record["members"]:
            current = self.pull(member["pr"])
            head = current["head"]["sha"]
            marker = workflow.rollup_marker({"rollup": number, "head": head, "merge_commit": commit})
            if current.get("merged"):
                merge = current.get("merge_commit_sha")
                if not (merge and self.ancestor(merge, main)) and self.ancestor(head, main) \
                        and not self.has_comment(current["number"], marker):
                    self.comment(current["number"], f"Merged into `main` via rollup #{number} (`{commit}`).\n{marker}")
                continue
            if current["state"] != "open":
                continue
            self.fetch_pull(current["number"])
            if self.ancestor(head, main) and self.ancestor(head, detail["head"]["sha"]):
                self.comment(current["number"], f"Merged into `main` via rollup #{number} (`{commit}`); "
                                                f"closing.\n{marker}")
                self.close(current["number"])
                report["integrated"].append(current["number"])
            elif not self.has_comment(current["number"], PARTIAL_MARKER):
                self.comment(current["number"], f"{PARTIAL_MARKER}\nRollup #{number} merged this PR's earlier head "
                                                f"`{member['head']}`; the newer commits remain open for review.")


def client(write):
    """The bot's API client. Writes use only GH_TOKEN (the App token), never GITHUB_TOKEN or the CLI."""
    if write:
        token = os.environ.get("GH_TOKEN", "").strip()
        if not token:
            raise RollupError(MISSING_TOKEN)
        return workflow.GitHub(token=token), token
    return workflow.GitHub(), None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", default=str(Path(__file__).resolve().parents[1]))
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--dry-run", action="store_true", help="print planned writes; make none")
    sub = parser.add_subparsers(dest="command", required=True)
    compose = sub.add_parser("compose", parents=[common])
    compose.add_argument("--max-members", type=int, default=MAX_MEMBERS)
    compose.add_argument("--max-open", type=int, default=2)
    sub.add_parser("enqueue", parents=[common]).add_argument("--pr", type=int)
    sub.add_parser("reconcile", parents=[common]).add_argument("--merged-days", type=int, default=3)
    auth = sub.add_parser("authorize", help="print authorized=true|false for a workflow event")
    auth.add_argument("--event-name", required=True)
    auth.add_argument("--event", required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "authorize":
            event = json.loads(Path(args.event).read_text()) if args.event_name == "issue_comment" else {}
            api = workflow.GitHub() if args.event_name == "issue_comment" else None
            print("authorized=" + ("true" if authorize(args.event_name, event, api) else "false"))
            return 0
        api, token = client(write=not args.dry_run)
        read_token = os.environ.get("GTI_READ_TOKEN", "").strip()
        runs_api = workflow.GitHub(token=read_token) if read_token else None
        bot = Rollup(api, args.repo, dry_run=args.dry_run, runs_api=runs_api, token=token,
                     max_members=getattr(args, "max_members", MAX_MEMBERS), max_open=getattr(args, "max_open", 2),
                     merged_days=getattr(args, "merged_days", 3))
        if args.command == "compose":
            result = bot.compose()
        elif args.command == "enqueue":
            result = bot.enqueue(args.pr)
        else:
            result = bot.reconcile()
        result["actions"] = bot.actions
        print(json.dumps(result, indent=2, default=str))
        return 0
    except (RollupError, workflow.WorkflowError, OSError, ValueError) as error:
        print("rollup: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
