#!/usr/bin/env python3
"""Explicit, bounded integration batches over published GTI task PRs."""

from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
from pathlib import Path
import subprocess
import shutil
import sys
from datetime import datetime, timezone

from agent_workflow import WorkflowError, clean, git
import validation_checks as checks

COLLECTION_SECONDS = 600
MAX_MEMBERS = 8


class Batches:
    def __init__(self, workflow, execute=None):
        self.flow = workflow
        self.execute = execute or subprocess.run

    def status(self):
        ready, active = [], []
        for path in self.flow.state_dir.glob("*.json"):
            state = json.loads(path.read_text())
            if self.flow.is_legacy(state) or state.get("phase") in {"merged", "synced", "completed", "abandoned"}:
                continue
            if state.get("batch"):
                active.append({"task": state["task"], "phase": state["phase"], "pr": state.get("pr")})
            if state.get("admission") and state["phase"] == "ready-for-integration":
                admission = state["admission"]
                age = (datetime.now(timezone.utc) - datetime.fromisoformat(admission["at"])).total_seconds()
                ready.append({"task": state["task"], "head": admission["head"], "pr": state.get("pr"),
                              "batch": state.get("integration_batch"), "age_seconds": round(age),
                              "collection_due": age >= COLLECTION_SECONDS})
        return {"ready": ready, "active": active, "max_members": MAX_MEMBERS,
                "collection_seconds": COLLECTION_SECONDS}

    def enroll(self, task, plan, build, jobs=4):
        with self.flow.lock():
            state = self.flow.load(task)
            root = self.flow.tree(state)
            clean(root)
            pr = self.flow.pull(state)
            if state.get("batch") or state.get("integration_batch") or pr["state"] != "open" or not pr.get("draft"):
                raise WorkflowError("Only an unassigned draft task PR can enter a batch")
            head, base = git(root, "rev-parse", "HEAD"), self.flow.fetch()
            if state.get("published_head") != head or pr["head"]["sha"] != head:
                raise WorkflowError("Checkpoint this exact source before batch admission")
            if not self.flow.ancestor(base, head):
                raise WorkflowError("Reconcile current main before focused admission")
            paths = git(root, "diff", "--name-only", base, head).splitlines()
            try:
                checks.validate_plan(plan, paths)
            except ValueError as error:
                raise WorkflowError(str(error)) from error
            if sys.platform not in plan["platforms"] or not set(plan["regressions"]) & set(checks.plan_tests(plan)):
                raise WorkflowError("Admission must run on a declared platform with its regression enabled")
            if "revision" in plan["reproduction"] and not self.flow.ancestor(plan["reproduction"]["revision"], head):
                raise WorkflowError("The original-failure revision is not an ancestor of this fix")
            state.pop("verified", None)
            self.flow.save(state, "focused-validation")
        directory = self.flow.state_dir / "batch-evidence" / task / head
        directory.mkdir(parents=True, exist_ok=True)
        plan_file = directory / "plan.json"
        report_file = directory / "focused.json"
        plan_file.write_text(json.dumps(plan, indent=2) + "\n")
        command = [sys.executable, str(root / "scripts/validation_checks.py"), "focused", "--build", str(Path(build).resolve()),
                   "--plan", str(plan_file), "--report", str(report_file), "--jobs", str(jobs)]
        try:
            # T1 exact owning selections need no broad-suite reservation. T2/3
            # plans include subsystem coverage and use the normal shared slot.
            window = (self.flow.window.acquire("suite", command, worktree=str(root))
                      if plan["tier"] != "T1" or plan.get("broader") else nullcontext())
            with window, (directory / "focused.log").open("w") as log:
                self.execute(command, cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True)
            report = json.loads(report_file.read_text())
            if (report.get("head") != head or report.get("scope") != "focused" or
                    report.get("build_type") not in checks.OPTIMIZED or report.get("skipped") or
                    report.get("platform") != sys.platform or
                    report.get("smoke") != ["scalar-O0", "scalar-O3", "owners-O0", "owners-O3"] or
                    not set(checks.plan_tests(plan)).issubset(report.get("tests", []))):
                raise WorkflowError("Focused evidence is incomplete or belongs to another source")
        except (OSError, ValueError, subprocess.CalledProcessError) as error:
            self.flow.record(task, "focused-validation", "blocked")
            raise WorkflowError(f"Focused admission failed; inspect {directory}") from error
        with self.flow.lock():
            state = self.flow.load(task)
            clean(root)
            pr = self.flow.pull(state)
            if (state["phase"] != "focused-validation" or git(root, "rev-parse", "HEAD") != head or
                    self.flow.fetch() != base or pr["head"]["sha"] != head or not pr.get("draft")):
                self.flow.save(state, "active")
                raise WorkflowError("Task/main/PR changed during admission; retain evidence and reconcile")
            portable = json.loads(json.dumps(plan))
            reproduction = portable["reproduction"]
            if "log" in reproduction:
                log = Path(reproduction["log"])
                if not log.is_file():
                    raise WorkflowError("Retain the original failure log before admission")
                digest = hashlib.sha256()
                with log.open("rb") as stream:
                    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                        digest.update(chunk)
                retained = directory / "original-failure.log"
                if log.resolve() != retained.resolve():
                    shutil.copyfile(log, retained)
                reproduction["log"] = "sha256:" + digest.hexdigest()
            state["admission"] = {"head": head, "base": base, "plan": portable,
                                  "report": str(report_file), "at": self.flow.now()}
            self.flow.api.request("PATCH", f"/pulls/{state['pr']}", {
                "body": checks.component_body(pr.get("body"), {"head": head, "task": task, "tier": plan["tier"]})})
            self.flow.save(state, "ready-for-integration")
            return state

    def withdraw(self, task, reason):
        if not reason.strip():
            raise WorkflowError("Record why the task is leaving batch admission")
        with self.flow.lock():
            state = self.flow.load(task)
            if state.get("integration_batch"):
                raise WorkflowError("Release the selected batch before withdrawing its member")
            pr = self.flow.pull(state)
            if pr["state"] != "open" or not pr.get("draft"):
                raise WorkflowError("Return the component PR to draft before withdrawal so independent CI starts on ready_for_review")
            self.flow.api.request("PATCH", f"/pulls/{state['pr']}", {"body": checks.component_body(pr.get("body"))})
            state.pop("admission", None)
            state["batch_withdrawal"] = {"reason": reason, "at": self.flow.now()}
            self.flow.save(state, "active")
            return state

    def prepare(self, task, selected, worktree):
        if not 1 <= len(selected) <= MAX_MEMBERS or len(selected) != len(set(selected)) or task in selected:
            raise WorkflowError("Select one to eight distinct admitted tasks explicitly")
        # Reserve components before creating/merging source. A failed prepare
        # retains the reservation and checkout, so a second agent cannot start
        # a duplicate full campaign while conflicts await resolution.
        with self.flow.lock():
            snapshots = [self.flow.load(name) for name in selected]
        members = []
        main = self.flow.fetch()
        # Network reads/fetches for up to eight PRs must not hold the shared
        # metadata lock and block unrelated checkpoints for their duration.
        for member in snapshots:
            name = member["task"]
            admission = member.get("admission", {})
            pr = self.flow.pull(member)
            head = pr["head"]["sha"]
            if (member.get("integration_batch") not in {None, task} or not admission or
                    member["phase"] != "ready-for-integration" or member.get("published_head") != head or
                    admission.get("head") != head or pr["state"] != "open" or not pr.get("draft") or
                    self.flow.ancestor(head, main)):
                raise WorkflowError(f"{name} is no longer an eligible exact published component")
            member_root = self.flow.tree(member)
            clean(member_root)
            if git(member_root, "rev-parse", "HEAD") != head:
                raise WorkflowError("Component acquired unpublished commits; checkpoint and re-enroll it")
            git(self.flow.repo, "fetch", "origin", member["branch"])
            if git(self.flow.repo, "rev-parse", "FETCH_HEAD") != head:
                raise WorkflowError("Component branch changed during selection")
            members.append({"task": name, "pr": member["pr"], "head": head, "plan": admission["plan"]})
        with self.flow.lock():
            if any(row["task"] != task for row in self.status()["active"]):
                raise WorkflowError("Another integration batch is active; keep components ready and inspect batch status")
            for before in snapshots:
                if self.flow.load(before["task"]) != before:
                    raise WorkflowError("Component admission changed during preparation; retry explicit selection")
            state = self.flow.load(task) if self.flow.file(task).exists() else None
            if state and (state.get("batch", {}).get("tasks") != selected or state["worktree"] != str(Path(worktree).resolve())):
                raise WorkflowError("Resume a batch with its original member list and worktree")
            if state is None:
                state = self.flow.start(task, "Integrate validated compiler fixes: " + ", ".join(selected),
                                        worktree, "codex/" + task)
                state["batch"] = {"tasks": selected, "members": members, "base": state["base"],
                                  "manifest": checks.BATCH_DIRECTORY + task + ".json"}
                self.flow.save(state, "preparing-batch")
            elif state["batch"]["members"] != members:
                raise WorkflowError("Selected source changed; release this batch and prepare a new identity")
            for name in selected:
                member = self.flow.load(name)
                member["integration_batch"] = task
                self.flow.save(member)
        return self.resume(task)

    def resume(self, task):
        state = self.flow.load(task)
        root = self.flow.tree(state)
        clean(root)
        batch = state.get("batch")
        if not batch:
            raise WorkflowError("Not an integration batch")
        for member in batch["members"]:
            pr = self.flow.pull(self.flow.load(member["task"]))
            if pr["state"] != "open" or pr["head"]["sha"] != member["head"]:
                raise WorkflowError("Pinned component changed; preserve this checkout and release the batch")
            if self.flow.ancestor(member["head"], git(root, "rev-parse", "HEAD")):
                continue
            result = git(root, "merge", "--no-edit", member["head"], check=False)
            if result.returncode:
                self.flow.save(state, "batch-conflict")
                raise WorkflowError(f"Resolve and commit {member['task']}'s conflict in {root}, then batch resume {task}; no work was discarded")
        destination = root / batch["manifest"]
        if (not destination.resolve().is_relative_to(root) or destination.is_symlink() or
                any(parent.is_symlink() for parent in destination.parents if parent != root and root in parent.parents)):
            raise WorkflowError("Batch records must be ordinary paths inside their candidate")
        destination.parent.mkdir(parents=True, exist_ok=True)
        manifest = {"schema": 1, "base": batch["base"], "members": batch["members"]}
        contents = json.dumps(manifest, indent=2) + "\n"
        if destination.exists() and destination.read_text() != contents:
            raise WorkflowError("Do not replace an existing batch manifest")
        if not destination.exists():
            destination.write_text(contents)
            return self.flow.checkpoint(task, "Record exact integration batch membership", [batch["manifest"]])
        return self.flow.checkpoint(task)

    def verify(self, task, build=None, jobs=4, remote_steps=None):
        state = self.flow.load(task)
        if not state.get("batch"):
            raise WorkflowError("Only a prepared batch can use full batch verification")
        if state["phase"] == "verifying":
            raise WorkflowError("This batch already has a verification operation; resume its recorded session/run instead of submitting another")
        self.validate_members(state)
        if remote_steps is not None:
            # The worker owns its host's suite window. Require the fixed full
            # runner (no regex/test-subset switches) in the supplied plan.
            rows = remote_steps.get("steps", remote_steps) if isinstance(remote_steps, dict) else remote_steps
            candidates = [row.get("argv") for row in rows if isinstance(row, dict) and
                          row.get("kind") == "suite" and row.get("cwd") == "source"] if isinstance(rows, list) else []
            base = self.flow.fetch()
            def full_command(command):
                if not isinstance(command, list) or command[1:3] != ["scripts/validation_checks.py", "full"]:
                    return False
                arguments = command[3:]
                keys = arguments[::2]
                if len(arguments) % 2 or len(set(keys)) != len(keys) or set(keys) - {"--build", "--jobs", "--base"}:
                    return False
                values = dict(zip(keys, arguments[1::2]))
                return values.get("--base") == base and bool(values.get("--build"))
            if not any(full_command(command) for command in candidates):
                raise WorkflowError("Remote batch plans must execute scripts/validation_checks.py full --base <current-main-sha>")
            verified = self.flow.verify(task, remote_steps, remote=True)
            # Remote run logs contain the fixed runner's structured summary.
            directory = Path(verified["verified"]["remote"]["directory"])
            summaries = []
            for log in directory.rglob("*.log"):
                with log.open(errors="replace") as stream:
                    for line in stream:
                        if line.startswith("GTI_VALIDATION_RESULT="):
                            summaries.append(json.loads(line.partition("=")[2]))
            if len(summaries) != 1:
                raise WorkflowError("Remote full-suite summary is missing or ambiguous")
            report = summaries[0]
        else:
            if build is None:
                raise WorkflowError("Provide this batch's optimized build directory")
            root = self.flow.tree(state)
            directory = self.flow.state_dir / "batch-evidence" / task / git(root, "rev-parse", "HEAD")
            directory.mkdir(parents=True, exist_ok=True)
            report_path = directory / (sys.platform + "-full.json")
            command = [sys.executable, str(root / "scripts/validation_checks.py"), "full", "--build", str(Path(build).resolve()),
                       "--report", str(report_path), "--jobs", str(jobs), "--log", str(directory / (sys.platform + "-full.log"))]
            verified = self.flow.verify(task, [command])
            report = json.loads(report_path.read_text())
        self.accept_report(task, verified, report)
        return self.flow.load(task)

    def accept_report(self, task, state, report):
        proof = state["verified"]
        required = {name for member in state["batch"]["members"] for name in checks.plan_tests(member["plan"], report.get("platform"))}
        if (report.get("head") != proof["head"] or report.get("scope") != "full" or
                report.get("build_type") not in checks.OPTIMIZED or not report.get("tests") or
                not required.issubset(report["tests"]) or required.intersection(report.get("skipped", [])) or
                report.get("smoke") != ["scalar-O0", "scalar-O3", "owners-O0", "owners-O3"]):
            raise WorkflowError("Full batch evidence does not cover this source and every component")
        with self.flow.lock():
            current = self.flow.load(task)
            if current.get("verified") != proof:
                raise WorkflowError("Batch validation changed while recording platform evidence")
            current.setdefault("batch_verified", {})[report["platform"]] = {key: proof[key] for key in ("head", "base")}
            self.flow.save(current)

    def validate_members(self, state):
        root = self.flow.tree(state)
        manifest = json.loads((root / state["batch"]["manifest"]).read_text())
        if manifest != {"schema": 1, "base": state["batch"]["base"], "members": state["batch"]["members"]}:
            raise WorkflowError("Committed batch membership differs from the selected records")
        for member in state["batch"]["members"]:
            selected = self.flow.load(member["task"])
            pr = self.flow.pull(selected)
            if pr["head"]["sha"] != member["head"] or pr["state"] != "open":
                raise WorkflowError("Component PR changed or closed after batch preparation")
            member_root = self.flow.tree(selected)
            clean(member_root)
            if git(member_root, "rev-parse", "HEAD") != member["head"]:
                raise WorkflowError("Component acquired unpublished commits after batch preparation")
            if not self.flow.ancestor(member["head"], git(root, "rev-parse", "HEAD")):
                raise WorkflowError("Batch no longer contains a selected source commit")

    def before_finish(self, state):
        self.validate_members(state)
        required = {platform for member in state["batch"]["members"] for platform in member["plan"]["platforms"]}
        proof = state.get("verified")
        identity = {key: proof[key] for key in ("head", "base")} if proof else None
        if not proof or any(state.get("batch_verified", {}).get(platform) != identity for platform in required):
            raise WorkflowError("Full combined validation is missing for a required platform or current head/base")
        # Full exact-source/platform proof above is the acceptance gate.
        # Hosted CI is additional feedback here, never a runner-queue dependency.
        self.flow.protection()

    def release(self, task, reason):
        with self.flow.lock():
            state = self.flow.load(task)
            if not state.get("batch"):
                raise WorkflowError("Not a selected batch")
            # Normal abandonment preserves the published checkout and commits.
            state = self.flow.abandon(task, reason)
            for name in state["batch"]["tasks"]:
                member = self.flow.load(name)
                if member.get("integration_batch") == task:
                    member.pop("integration_batch")
                    self.flow.save(member)
            return state


def main(workflow, argv):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("status")
    enroll = sub.add_parser("enroll")
    enroll.add_argument("task")
    enroll.add_argument("--plan", required=True)
    enroll.add_argument("--build", required=True)
    enroll.add_argument("--jobs", type=int, default=4)
    prepare = sub.add_parser("prepare")
    prepare.add_argument("task")
    prepare.add_argument("--tasks", nargs="+", required=True)
    prepare.add_argument("--worktree", required=True)
    for name in ("resume", "withdraw", "release"):
        command = sub.add_parser(name)
        command.add_argument("task")
        if name != "resume":
            command.add_argument("--reason", required=True)
    verify = sub.add_parser("verify")
    verify.add_argument("task")
    sources = verify.add_mutually_exclusive_group(required=True)
    sources.add_argument("--build")
    sources.add_argument("--remote-steps-file")
    verify.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args(argv)
    batches = Batches(workflow)
    if args.command == "status":
        return batches.status()
    if args.command == "enroll":
        return batches.enroll(args.task, json.loads(Path(args.plan).read_text()), args.build, args.jobs)
    if args.command == "prepare":
        return batches.prepare(args.task, args.tasks, args.worktree)
    if args.command == "verify":
        return batches.verify(args.task, args.build, args.jobs,
                              json.loads(Path(args.remote_steps_file).read_text()) if args.remote_steps_file else None)
    if args.command in {"withdraw", "release"}:
        return getattr(batches, args.command)(args.task, args.reason)
    return batches.resume(args.task)
