#!/usr/bin/env python3
"""Detached optional preview jobs, isolated from normal task/PR acceptance."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
from subprocess import Popen
import sys
import uuid

from agent_workflow import Workflow, WorkflowError, build_window, git
from preview_workflow import Preview, tests


ACTIVE = {"queued", "running"}
SCRIPTS = ("preview_submit.py", "preview_workflow.py", "agent_workflow.py",
           "build_window.py", "remote_runner.py")


class Submissions:
    def __init__(self, flow):
        self.flow = flow
        self.preview = Preview(flow)
        self.directory = self.preview.directory / "submissions"
        self.directory.mkdir(exist_ok=True)

    def path(self, identity):
        if len(identity) != 32 or any(c not in "0123456789abcdef" for c in identity):
            raise WorkflowError("Use the submission ID returned by preview submit")
        return self.directory / identity

    def exists(self, identity):
        return (self.path(identity) / "state.json").is_file()

    def load(self, identity):
        return json.loads((self.path(identity) / "state.json").read_text())

    def save(self, state):
        root = self.path(state["id"])
        temporary = root / ("state-" + uuid.uuid4().hex + ".tmp")
        temporary.write_text(json.dumps(state, indent=2) + "\n")
        temporary.replace(root / "state.json")
        return state

    def status(self, identity=None):
        if identity is None:
            return [self.status(path.parent.name) for path in sorted(self.directory.glob("*/state.json"))]
        state = self.load(identity)
        if state["phase"] in ACTIVE and not build_window.pid_alive(state.get("pid")):
            # Observation only: don't race the worker's atomic state updates or
            # restart an uncertain remote run merely because the submitter died.
            state.update(phase="interrupted", note="Inspect the retained worker log/remote run before resubmitting")
        return state

    def submit(self, task, checks):
        with self.flow.lock():
            record = self.flow.load(task)
            if not record.get("pr"):
                raise WorkflowError("Checkpoint the completed task and open its PR before optional preview submission")
            pr = self.flow.pull(record)
            if pr["state"] != "open":
                raise WorkflowError("Only an open task PR can be submitted to preview")
            history = sorted((self.load(path.parent.name) for path in self.directory.glob("*/state.json")
                              if json.loads(path.read_text())["task"] == task), key=lambda row: row["created"])
            checks = tests(checks if checks is not None else (history[-1]["tests"] if history else None))
            if "preview_admission" in checks:
                raise WorkflowError("Choose owning focused tests, not the aggregate preview_admission gate")
            for prior in history:
                observed = self.status(prior["id"])
                if observed["phase"] in ACTIVE:
                    if prior["head"] == pr["head"]["sha"] and prior["tests"] == checks:
                        return observed
                    raise WorkflowError("This task already has a preview submission running; its normal PR can continue")
            identity = uuid.uuid4().hex
            root = self.path(identity)
            root.mkdir()
            scripts = root / "tools"
            scripts.mkdir()
            # Detached workers must survive normal task worktree cleanup.
            for name in SCRIPTS:
                shutil.copyfile(Path(__file__).with_name(name), scripts / name)
            state = {"id": identity, "task": task, "task_record": record,
                     "head": pr["head"]["sha"], "tests": checks, "created": self.flow.now(),
                     "phase": "queued", "attempts": [], "log": str(root / "worker.log"),
                     "note": "Optional preview only; continue the normal PR workflow without waiting"}
            self.save(state)
            try:
                with (root / "worker.log").open("a") as output:
                    worker = Popen([sys.executable, str(scripts / "preview_submit.py"),
                                               str(self.flow.primary), identity],
                                              cwd=self.flow.primary, stdin=subprocess.DEVNULL,
                                              stdout=output, stderr=subprocess.STDOUT,
                                              start_new_session=True, close_fds=True)
                state["pid"] = worker.pid
            except OSError as error:
                state.update(phase="failed", error=f"Could not launch optional preview worker: {error}")
            # Worker takes this same short lock before reading the launch record.
            return self.save(state)

    def cleanup(self, state):
        """Remove only our clean, published candidate checkout; retain Git refs/logs."""
        candidate = self.preview.load(state["candidate"])
        root = Path(candidate["worktree"])
        if candidate["phase"] != "published" or not root.exists():
            return
        with self.flow.lock():
            if git(root, "rev-parse", "HEAD") != candidate["head"] or git(
                    root, "status", "--porcelain", "--untracked-files=all", "--ignored"):
                state["cleanup"] = "Preserved candidate with changed or ignored files"
                return
            remote = self.preview.remote()
            if not remote or not self.flow.ancestor(candidate["head"], remote):
                raise WorkflowError("Cannot clean a candidate without confirmed preview publication")
            git(self.flow.repo, "worktree", "remove", str(root))
            state["cleanup"] = "Removed clean candidate checkout; checkpoint refs and evidence retained"

    def run(self, identity):
        with self.flow.lock():
            state = self.load(identity)
            if state["phase"] != "queued" or state.get("pid") != os.getpid():
                raise WorkflowError("Only the registered detached worker may consume this submission")
        # Separate semaphore: waiting for preview never owns a normal build or
        # lifecycle slot. The existing remote runner owns heavy build windows.
        queue = build_window.Window(self.preview.directory / "queue", slots={"suite": 1})
        try:
            with queue.acquire("suite", ["preview", "submit", state["task"]], worktree=str(self.flow.primary)):
                state.update(phase="running")
                self.save(state)
                for attempt in range(2):
                    pr = self.flow.pull(state["task_record"])
                    if pr["state"] != "open" or pr["head"]["sha"] != state["head"]:
                        state.update(phase="superseded", note="Original PR closed or advanced; no different head was admitted")
                        break
                    # Sibling of the primary checkout, never inside its source
                    # graph. Unique paths, no user supplied cleanup targets.
                    root = self.flow.primary.parent / ("gti-preview-" + identity + "-" + str(attempt))
                    state["attempts"].append(str(root))
                    self.save(state)
                    candidate = self.preview.prepare(state["task"], root, state["tests"], selection=state)
                    state["candidate"] = candidate["id"]
                    self.save(state)
                    try:
                        self.preview.verify(candidate["id"], [], remote=True, automatic=True)
                        self.preview.publish(candidate["id"])
                    except WorkflowError:
                        # A lost successful push response must not trigger an
                        # unnecessary rebuild or replacement admission.
                        candidate = self.preview.load(candidate["id"])
                        if self.preview.remote() == candidate["head"] and candidate.get("verified"):
                            self.preview.publish(candidate["id"])
                        elif self.preview.remote() != candidate["previous"] or self.flow.fetch() != candidate["main"]:
                            if attempt == 0:
                                continue
                            state.update(phase="stale", note="Inputs advanced twice; resubmit if preview is still useful")
                            break
                        else:
                            raise
                    state.update(phase="published", note="Preview published; original PR requirements remain unchanged")
                    self.save(state)
                    try:
                        self.cleanup(state)
                    except Exception as error:
                        state["cleanup"] = f"Published; retained checkout: {error}"
                    break
        except Exception as error:
            state.update(phase="failed", error=str(error),
                         note="Preview only: normal task verification, merge and cleanup can continue")
        return self.save(state)


if __name__ == "__main__":
    result = Submissions(Workflow(sys.argv[1])).run(sys.argv[2])
    print(json.dumps(result, indent=2), flush=True)
