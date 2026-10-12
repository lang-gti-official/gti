#!/usr/bin/env python3
"""Opt-in combined preview candidates; never substitutes for main PR validation."""

from __future__ import annotations

import argparse
import json
import platform
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import uuid

from agent_workflow import WorkflowError, clean, git, load_remote_runner

BRANCH = "preview"
MANIFEST = ".gti-preview.json"


def tests(value):
    if not isinstance(value, list) or not value or any(
            not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9_.-]+", name) for name in value):
        raise WorkflowError("Select at least one exact registered CTest name")
    return value


class Preview:
    def __init__(self, workflow):
        self.flow = workflow
        self.directory = workflow.state_dir / "preview"
        self.directory.mkdir(exist_ok=True)

    def remote(self):
        fields = git(self.flow.repo, "ls-remote", "origin", "refs/heads/preview").split()
        return fields[0] if fields else None

    def save(self, state):
        destination = self.directory / (state["id"] + ".json")
        temporary = destination.with_suffix(".tmp")
        temporary.write_text(json.dumps(state, indent=2) + "\n")
        temporary.replace(destination)
        return state

    def load(self, identity):
        if len(identity) != 32 or any(c not in "0123456789abcdef" for c in identity):
            raise WorkflowError("Use the candidate ID returned by preview prepare")
        return json.loads((self.directory / (identity + ".json")).read_text())

    def prepare(self, task, worktree, checks=None, remove=False):
        with self.flow.lock():
            main = self.flow.fetch()
            previous = self.remote()
            if previous:
                git(self.flow.repo, "fetch", "origin", "refs/heads/preview")
            root = Path(worktree).resolve()
            if root.exists() or root.is_relative_to(self.flow.primary):
                raise WorkflowError("Preview needs a new worktree outside the primary checkout")
            manifest = (json.loads(git(self.flow.repo, "show", previous + ":" + MANIFEST))
                        if previous else {"schema": 1, "features": [], "removed": []})
            if manifest.get("schema") != 1 or not isinstance(manifest.get("features"), list):
                raise WorkflowError("Published preview has an unsupported inclusion manifest")
            operations = []
            # Git remembers the original head even after a preview revert, so
            # merging main alone would retain that revert. Restore accepted
            # contributions first; they now belong to main, not pending PRs.
            for removed in reversed(manifest.get("removed", [])):
                if not removed.get("restored") and self.flow.ancestor(removed["head"], main):
                    for commit in reversed(removed["reverts"]):
                        operations.append({"kind": "restore", "sha": commit})
                    removed.update(restored=True, accepted_in_main=main)
            if remove:
                rows = [row for row in manifest["features"] if row["task"] == task]
                if not rows:
                    raise WorkflowError("Task is not included in preview")
                if any(self.flow.ancestor(row["head"], main) for row in rows):
                    raise WorkflowError("Task is already integrated into main; preview cannot revert accepted main behavior")
                for row in reversed(rows):
                    removed = manifest.setdefault("removed", [])
                    removal = len(removed)
                    removed.append({**row, "reverts": []})
                    for commit in reversed(row["contributions"]):
                        operations.append({"kind": "revert", "sha": commit, "removal": removal})
                manifest["features"] = [row for row in manifest["features"] if row["task"] != task]
            elif task is not None:
                checks = tests(checks)
                task_state = self.flow.load(task)
                if not task_state.get("pr"):
                    raise WorkflowError("Checkpoint the completed feature and open its task PR before preview admission")
                pr = self.flow.pull(task_state)
                if pr["state"] != "open":
                    raise WorkflowError("Only an open task PR can enter preview")
                head = pr["head"]["sha"]
                if self.flow.ancestor(head, main):
                    raise WorkflowError("PR head is already integrated into main")
                if any(row["task"] == task for row in manifest["features"]):
                    raise WorkflowError("Task already included; revert it before admitting a replacement")
                restoring = any(row["task"] == task and not row.get("restored")
                                for row in manifest.get("removed", []))
                if previous and self.flow.ancestor(head, previous) and not restoring:
                    raise WorkflowError("PR head is already reachable from preview; there is no new feature merge to record")
                git(self.flow.repo, "fetch", "origin", task_state["branch"])
                if git(self.flow.repo, "rev-parse", "FETCH_HEAD") != head:
                    raise WorkflowError("PR moved while preparing; prepare a new candidate")
                feature = {"task": task, "pr": pr["number"], "head": head, "tests": checks,
                           "contributions": []}
                manifest["features"].append(feature)
                for removed in reversed(manifest.get("removed", [])):
                    if removed["task"] == task and not removed.get("restored"):
                        for commit in reversed(removed["reverts"]):
                            operations.append({"kind": "restore", "sha": commit, "feature": feature})
                        removed["restored"] = True
                operations.append({"kind": "merge", "sha": head, "feature": feature})
            operations.append({"kind": "merge", "sha": main})
            state = {"id": uuid.uuid4().hex, "main": main, "previous": previous,
                     "worktree": str(root), "phase": "preparing", "manifest": manifest,
                     "operations": operations}
            state["branch"] = "codex/preview-" + state["id"]
            git(self.flow.repo, "worktree", "add", "-b", state["branch"], str(root), previous or main)
            self.save(state)
            return self.resume(state["id"], locked=True)

    def resume(self, identity, locked=False):
        if not locked:
            with self.flow.lock():
                return self.resume(identity, locked=True)
        state = self.load(identity)
        if state["phase"] == "prepared" and not state.get("checkpointed"):
            root = Path(state["worktree"])
            clean(root)
            if git(root, "rev-parse", "HEAD") != state["head"]:
                raise WorkflowError("Candidate changed before checkpoint publication")
            git(root, "push", "-u", "origin", "HEAD:refs/heads/" + state["branch"])
            state["checkpointed"] = True
            return self.save(state)
        if state["phase"] != "preparing":
            return state
        root = Path(state["worktree"])
        clean(root)
        manifest = state["manifest"]
        while state["operations"]:
            operation = state["operations"][0]
            head = git(root, "rev-parse", "HEAD")
            if "before" in operation and head != operation["before"]:
                # A user resolved and committed the pending conflict. Preserve
                # that exact result, then validate it as part of the candidate.
                if not self.flow.ancestor(operation["before"], head) or (
                        operation["kind"] == "merge" and not self.flow.ancestor(operation["sha"], head)):
                    raise WorkflowError("Resolved candidate no longer contains the pending operation")
            else:
                operation["before"] = head
                self.save(state)
                if operation["kind"] == "merge":
                    argv = ["merge", "--no-ff", "--no-edit", operation["sha"]]
                else:
                    argv = ["revert", "--no-edit", *(["-m", "1"] if operation["kind"] == "revert" else []), operation["sha"]]
                try:
                    git(root, *argv)
                except Exception as error:
                    raise WorkflowError(f"Candidate {identity} needs resolution in {root}; commit it, then preview resume {identity}") from error
            after = git(root, "rev-parse", "HEAD")
            if operation.get("feature"):
                feature = operation["feature"]
                # JSON persistence does not retain object aliases.
                row = next(row for row in manifest["features"] if row["task"] == feature["task"])
                if after != operation["before"]:
                    row["contributions"].append(after)
                if operation["kind"] == "merge":
                    row["merge"] = after
            if "removal" in operation:
                manifest["removed"][operation["removal"]]["reverts"].append(after)
            state["operations"].pop(0)
            self.save(state)
        if state["operations"] == []:
            main, previous = state["main"], state["previous"]
            manifest.update(main=main, previous=previous, candidate=state["id"])
            path = root / MANIFEST
            path.write_text(json.dumps(manifest, indent=2) + "\n")
            git(root, "add", "--", MANIFEST)
            if git(root, "diff", "--cached", "--name-only"):
                git(root, "commit", "-m", "Record combined preview candidate identities")
            state.update(head=git(root, "rev-parse", "HEAD"), manifest=manifest, phase="prepared")
            self.save(state)
            return self.resume(identity, locked=True)

    def current(self, state):
        if not state.get("head"):
            raise WorkflowError("Finish candidate preparation with preview resume before validation/publication")
        if not state.get("checkpointed"):
            raise WorkflowError("Publish the candidate checkpoint with preview resume before validation")
        root = Path(state["worktree"])
        clean(root)
        if git(root, "rev-parse", "HEAD") != state["head"]:
            raise WorkflowError("Candidate changed; prepare and validate a new candidate")
        if self.remote() != state["previous"] or self.flow.fetch() != state["main"]:
            raise WorkflowError("Preview or main advanced; prepare and validate a new combined candidate")
        return root

    def verify(self, identity, cmake_args, remote=False, jobs=4):
        with self.flow.lock():
            state = self.load(identity)
            root = self.current(state)
            state.pop("verified", None)
            self.save(state)
        # A fresh optimized build and the fixed example gate are mandatory.
        build = "out/preview-build"
        steps = [
            {"kind": "build", "argv": ["cmake", "-S", ".", "-B", build, "-G", "Ninja", *cmake_args,
                "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=ON", "-DGTI_BUILD_LSP=ON"]},
            {"kind": "build", "argv": ["cmake", "--build", build, "--parallel", str(jobs)]},
        ]
        focused = sorted({name for feature in state["manifest"]["features"] for name in tests(feature["tests"])})
        steps.append({"kind": "suite", "argv": ["python3", "scripts/preview_examples.py", build,
                                                 *["--focused=" + name for name in focused]]})
        for index, step in enumerate(steps):
            step.update(name=f"preview-{index:02d}", cwd="source")
        logs = self.directory / (identity + "-results")
        logs.mkdir(exist_ok=True)
        execution = {"kind": "local", "platform": platform.system(), "machine": platform.machine()}
        if remote:
            runner = load_remote_runner()
            with tempfile.TemporaryDirectory(prefix="gti-preview-plan-") as temporary:
                plan = Path(temporary) / "steps.json"
                plan.write_text(json.dumps(steps))
                result = runner.main(["--repo", str(root), "run", "--steps-file", str(plan), "--results", str(logs)])
            if result:
                raise WorkflowError(f"Preview validation failed; see {logs}")
            report = json.loads((logs / "report.json").read_text())
            if report.get("state") != "passed" or report.get("exit_code") != 0 or any(
                    report.get(key, {}).get("head") != state["head"] or report.get(key, {}).get("status") != ""
                    for key in ("source", "final_source")):
                raise WorkflowError("Remote preview evidence does not match the candidate")
            execution = {"kind": "remote", "runner": report.get("runner"), "run": report.get("id")}
        else:
            for index, step in enumerate(steps):
                log = logs / f"{index:02d}.log"
                with self.flow.window.acquire(step["kind"], step["argv"], worktree=str(root)):
                    with log.open("w") as output:
                        result = subprocess.run(step["argv"], cwd=root, stdout=output, stderr=subprocess.STDOUT)
                if result.returncode:
                    raise WorkflowError(f"Preview validation failed; see {log}")
        with self.flow.lock():
            self.current(state)
            state.update(verified={"head": state["head"], "steps": steps, "logs": str(logs),
                                   "at": self.flow.now(), "execution": execution}, phase="verified")
            return self.save(state)

    def publish(self, identity):
        with self.flow.lock():
            state = self.load(identity)
            if not state.get("head") or state.get("verified", {}).get("head") != state["head"]:
                raise WorkflowError("Validate the combined candidate before publishing preview")
            # An uncertain push is recoverable without changing or retesting source.
            if self.remote() == state["head"]:
                state["phase"] = "published"
                return self.save(state)
            root = self.current(state)
            if state["previous"] and not self.flow.ancestor(state["previous"], state["head"]):
                raise WorkflowError("Preview publication must preserve existing history")
            # The ancestry check prohibits rewrites; the exact lease only closes
            # concurrent publishers' race (including compare-and-create).
            git(root, "push", "--force-with-lease=refs/heads/preview:" + (state["previous"] or ""),
                "origin", state["head"] + ":refs/heads/preview")
            state["phase"] = "published"
            return self.save(state)


def main(workflow, arguments):
    parser = argparse.ArgumentParser(prog="agent_workflow.py preview")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("seed", help="start or refresh preview from main without adding a PR").add_argument("--worktree", required=True)
    for name in ("prepare", "revert"):
        prepare = sub.add_parser(name)
        prepare.add_argument("task")
        prepare.add_argument("--worktree", required=True)
        if name == "prepare":
            prepare.add_argument("--tests", nargs="+", required=True, help="exact registered focused CTest names")
    verify = sub.add_parser("verify")
    verify.add_argument("candidate")
    verify.add_argument("--cmake-arg", action="append", default=[])
    verify.add_argument("--remote", action="store_true")
    verify.add_argument("--jobs", type=int, default=4)
    sub.add_parser("publish").add_argument("candidate")
    sub.add_parser("resume").add_argument("candidate")
    sub.add_parser("status").add_argument("candidate")
    args = parser.parse_args(arguments)
    preview = Preview(workflow)
    if args.command == "seed":
        return preview.prepare(None, args.worktree)
    if args.command in {"prepare", "revert"}:
        checks = args.tests if args.command == "prepare" else None
        return preview.prepare(args.task, args.worktree, checks, args.command == "revert")
    if args.command == "verify":
        if args.jobs < 1:
            raise WorkflowError("--jobs must be positive")
        return preview.verify(args.candidate, args.cmake_arg, args.remote, args.jobs)
    return getattr(preview, "load" if args.command == "status" else args.command)(args.candidate)
