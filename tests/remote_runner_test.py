#!/usr/bin/env python3
"""Disposable real Git/process tests for the SSH runner (no network required)."""
import importlib.util
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tarfile
import tempfile
import time
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
SCRIPT = REPO / "scripts/remote_runner.py"
sys.path.insert(0, str(SCRIPT.parent))
spec = importlib.util.spec_from_file_location("remote_runner", SCRIPT)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
from build_window import Window


class RemoteRunner(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gti-remote-runner-test-")
        self.directory = Path(self.temporary.name).resolve()
        self.root = runner.root_directory(str(self.directory / "gti-remote-runner"), create=True)
        self.repo = self.directory / "repo"
        self.repo.mkdir()
        runner.git(self.repo, "init", "--template=")
        runner.git(self.repo, "config", "user.name", "Runner Test")
        runner.git(self.repo, "config", "user.email", "runner@example.invalid")
        (self.repo / ".gitignore").write_text("ignored/\n.env*\n")
        (self.repo / "tracked.txt").write_text("original\n")
        (self.repo / "deleted.txt").write_text("delete me\n")
        runner.git(self.repo, "add", ".")
        runner.git(self.repo, "commit", "-m", "fixture")
        self.ids = []

    def tearDown(self):
        # Tests must not leave detached workers behind even on assertion failure.
        for run_id in self.ids:
            run = self.root / "runs" / run_id
            if run.exists() and (run / "state.json").exists():
                state = runner.read_json(run / "state.json")
                if state["state"] not in runner.TERMINAL:
                    (run / "cancel").touch()
                    try:
                        self.wait(run_id)
                    except AssertionError:
                        pass
        self.temporary.cleanup()

    def packet(self, steps, timeout=30):
        directory = self.directory / ("packet-" + str(len(self.ids)))
        directory.mkdir()
        identity = runner.snapshot(self.repo, directory)
        run_id = "run-" + __import__("uuid").uuid4().hex
        self.ids.append(run_id)
        request = {"schema": runner.SCHEMA, "id": run_id, "source": identity,
                   "steps": steps, "timeout": timeout}
        runner.write_json(directory / "request.json", request)
        packet = directory / "upload.tar"
        with tarfile.open(packet, "w") as archive:
            for name in ("head.bundle", "source.tar", "request.json"):
                archive.add(directory / name, arcname=name, recursive=False)
            runner.add_bytes(archive, "remote_runner.py", SCRIPT.read_bytes())
            runner.add_bytes(archive, "build_window.py", (SCRIPT.parent / "build_window.py").read_bytes())
        return run_id, packet

    def submit(self, code="print('result')", kind="suite", timeout=30):
        steps = [{"name": "fixture", "kind": kind, "cwd": "source",
                  "argv": [sys.executable, "-u", "-c", code]}]
        run_id, packet = self.packet(steps, timeout)
        with packet.open("rb") as incoming:
            result = subprocess.run([sys.executable, str(SCRIPT), "_remote", "submit", str(self.root), run_id],
                                    stdin=incoming, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(json.loads(result.stdout)["id"], run_id)
        return run_id

    def wait(self, run_id, terminal=True):
        deadline = time.monotonic() + 15
        run = self.root / "runs" / run_id
        while time.monotonic() < deadline:
            state = runner.read_json(run / "state.json")
            if (state["state"] in runner.TERMINAL if terminal else state.get("phase") == "fixture"):
                return state
            time.sleep(0.02)
        self.fail(f"worker did not progress: {runner.read_json(run / 'state.json')}")

    def assert_clean(self, run_id):
        run = self.root / "runs" / run_id
        for name in ("source", "build", "input", "logs", "results", "upload.tar"):
            self.assertFalse((run / name).exists(), name)
        self.assertTrue((run / "results.tar.gz").is_file())
        self.assertEqual(Window(self.root / "window").status(), {"build": [], "suite": []})

    def transport(self, config, action, run_id=None, input_path=None, output_path=None):
        if action == "fetch":
            __import__("shutil").copyfile(self.root / "runs" / run_id / "results.tar.gz", output_path)
            return None
        return runner.remote_action(action, str(self.root), run_id)

    def test_snapshot_dirty_deleted_new_ignored_and_identity(self):
        (self.repo / "tracked.txt").write_text("changed\n")
        (self.repo / "deleted.txt").unlink()
        (self.repo / "new.txt").write_text("new\n")
        (self.repo / "ignored").mkdir()
        (self.repo / "ignored" / "large").write_text("not sent")
        (self.repo / ".env").write_text("not sent either")
        directory = self.directory / "snapshot"
        directory.mkdir()
        identity = runner.snapshot(self.repo, directory)
        with tarfile.open(directory / "source.tar") as archive:
            self.assertEqual(archive.getnames(), [".gitignore", "new.txt", "tracked.txt"])
            self.assertEqual(archive.extractfile("tracked.txt").read(), b"changed\n")
        self.assertEqual(identity["head"], runner.git(self.repo, "rev-parse", "HEAD").decode().strip())
        other = self.directory / "snapshot-other"
        other.mkdir()
        self.assertEqual(identity, runner.snapshot(self.repo, other))
        self.assertIn(" D deleted.txt", identity["status"])

    def test_reject_source_symlink_and_archive_traversal_links_duplicates(self):
        (self.repo / "bad-link").symlink_to(self.directory / "outside")
        directory = self.directory / "snapshot"
        directory.mkdir()
        with self.assertRaises(runner.RunnerError):
            runner.snapshot(self.repo, directory)
        for name, kind in (("../outside", tarfile.REGTYPE), ("/absolute", tarfile.REGTYPE),
                           ("symlink", tarfile.SYMTYPE), ("hardlink", tarfile.LNKTYPE)):
            with tarfile.open(directory / "bad.tar", "w") as archive:
                info = tarfile.TarInfo(name)
                info.type = kind
                archive.addfile(info)
            with self.assertRaises(runner.RunnerError):
                runner.extract(directory / "bad.tar", directory / "out", 100)
        with tarfile.open(directory / "bad.tar", "w") as archive:
            runner.add_bytes(archive, "twice", b"a")
            runner.add_bytes(archive, "twice", b"b")
        with self.assertRaises(runner.RunnerError):
            runner.extract(directory / "bad.tar", directory / "out", 100)

    def test_pass_and_failure_retrieve_results_and_remove(self):
        for code, expected in (("print('success')", 0), ("print('failed output');raise SystemExit(7)", 7)):
            run_id = self.submit(code)
            state = self.wait(run_id)
            self.assertEqual(state["exit_code"], expected)
            self.assert_clean(run_id)
            destination = self.directory / run_id
            with mock.patch.object(runner, "connection", side_effect=self.transport):
                report = runner.fetch({}, run_id, destination)
            self.assertEqual(report, state)
            self.assertIn("output" if expected else "success", (destination / "logs/fixture.log").read_text())
            self.assertFalse((self.root / "runs" / run_id).exists())

    def test_fetch_failure_retains_remote_evidence(self):
        run_id = self.submit()
        self.wait(run_id)
        destination = self.directory / "results"
        destination.mkdir()
        with self.assertRaises(runner.RunnerError):
            runner.fetch({}, run_id, destination)
        with mock.patch.object(runner, "connection", side_effect=runner.RunnerError("network lost")):
            with self.assertRaises(runner.RunnerError):
                runner.fetch({}, run_id, self.directory / "other")
        self.assert_clean(run_id)

    def test_cancel_timeout_and_active_cleanup_refusal(self):
        for timeout, expected in ((30, 130), (1, 124)):
            run_id = self.submit("import time;print('started');time.sleep(60)", timeout=timeout)
            self.wait(run_id, terminal=False)
            with self.assertRaises(runner.RunnerError):
                runner.remote_action("remove", str(self.root), run_id)
            if expected == 130:
                runner.remote_action("cancel", str(self.root), run_id)
            self.assertEqual(self.wait(run_id)["exit_code"], expected)
            self.assert_clean(run_id)

    def test_shared_window_serializes_workers_and_cancels_waiter(self):
        window = Window(self.root / "window", slots={"suite": 1})
        with window.acquire("suite", ["test holds suite"]):
            first = self.submit("raise SystemExit('must not run')")
            second = self.submit("print('after holder exits')")
            for run_id in (first, second):
                self.wait(run_id, terminal=False)
                self.assertIsNone(runner.read_json(self.root / "runs" / run_id / "state.json")["child_pid"])
            runner.remote_action("cancel", str(self.root), first)
            self.assertEqual(self.wait(first)["exit_code"], 130)
            self.assertEqual(len(window.holders("suite")), 1)
        self.assertEqual(self.wait(second)["exit_code"], 0)
        self.assert_clean(second)

    def abandoned(self, state):
        run_id = "run-" + __import__("uuid").uuid4().hex
        run = self.root / "runs" / run_id
        run.mkdir()
        runner.write_json(run / "owner.json", {"schema": runner.SCHEMA, "id": run_id})
        runner.write_json(run / "state.json", {"id": run_id, **state})
        (run / "source").mkdir()
        (run / "upload.tar").write_bytes(b"incomplete upload")
        return run_id

    def test_prune_crash_partial_upload_and_only_owned_dead_runs(self):
        for key in ("created", "started"):
            run_id = self.abandoned({"state": "running", key: time.time() - 120, "pid": None})
            runner.prune(self.root)
            state = runner.read_json(self.root / "runs" / run_id / "state.json")
            self.assertEqual(state["state"], "interrupted")
            self.assert_clean(run_id)
        old = self.abandoned({"state": "failed", "finished": time.time() - runner.RETENTION - 1})
        live = self.abandoned({"state": "running", "started": 0, "pid": os.getpid()})
        foreign = self.root / "runs" / ("run-" + "f" * 32)
        foreign.mkdir()
        runner.write_json(foreign / "owner.json", {"not": "ours"})
        self.assertIn(old, runner.prune(self.root))
        self.assertTrue((self.root / "runs" / live / "source").exists())
        self.assertTrue(foreign.exists())

    def test_concurrent_prune_serializes_recovery(self):
        run_id = self.abandoned({"state": "running", "started": time.time() - 120, "pid": None})
        commands = [[sys.executable, str(SCRIPT), "_remote", "clean", str(self.root)]] * 3
        processes = [subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE) for argv in commands]
        for process in processes:
            out, err = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 0, err.decode())
            json.loads(out)
        self.assert_clean(run_id)

    def test_refuse_unsafe_root_and_unmarked_existing_root(self):
        for value in ("/", str(self.directory), "relative/gti-remote-runner", str(self.root / ".." / "gti-remote-runner")):
            with self.assertRaises(runner.RunnerError):
                runner.root_directory(value, create=True)
        other = self.directory / "other" / "gti-remote-runner"
        other.mkdir(parents=True)
        with self.assertRaises(runner.RunnerError):
            runner.root_directory(str(other), create=True)
        (self.directory / "alias").symlink_to(self.directory / "other", target_is_directory=True)
        with self.assertRaises(runner.RunnerError):
            runner.root_directory(str(self.directory / "alias" / "gti-remote-runner"), create=True)
        with self.assertRaises(runner.RunnerError):
            runner.run_directory(self.root, "../../")

    def prepare_inline_worker(self):
        run_id, packet = self.packet([{"name": "fixture", "kind": "suite", "cwd": "source",
                                      "argv": [sys.executable, "-c", "import time;time.sleep(60)"]}])
        run = self.root / "runs" / run_id
        run.mkdir()
        runner.write_json(run / "owner.json", {"schema": runner.SCHEMA, "id": run_id})
        (run / "input").mkdir()
        runner.extract(packet, run / "input", runner.MAX_INPUT)
        return run_id

    def test_cancel_during_spawn_tracks_and_stops_child_before_slot_release(self):
        run_id = self.prepare_inline_worker()
        real_popen = subprocess.Popen
        children = []

        def spawn(argv, **options):
            process = real_popen(argv, **options)
            children.append(process)
            if argv[0] == sys.executable:
                signal.getsignal(signal.SIGTERM)(signal.SIGTERM, None)
            return process

        with mock.patch.object(runner.subprocess, "Popen", side_effect=spawn):
            self.assertEqual(runner.worker(str(self.root), run_id), 130)
        self.assertTrue(all(process.poll() is not None for process in children))
        self.assert_clean(run_id)

    def test_cleanup_error_cannot_be_reported_as_passed(self):
        run_id = self.prepare_inline_worker()
        path = self.root / "runs" / run_id / "input/request.json"
        request = runner.read_json(path)
        request["steps"][0]["argv"] = [sys.executable, "-c", "pass"]
        runner.write_json(path, request)
        with mock.patch.object(runner, "cleanup_heavy", side_effect=runner.RunnerError("cannot remove build")):
            self.assertEqual(runner.worker(str(self.root), run_id), 1)
        run = self.root / "runs" / run_id
        with tarfile.open(run / "results.tar.gz") as archive:
            report = json.load(archive.extractfile("report.json"))
        self.assertEqual(report["state"], "failed")
        self.assertEqual(report["cleanup_error"], "cannot remove build")
        with self.assertRaises(runner.RunnerError):
            runner.remote_action("remove", str(self.root), run_id)


if __name__ == "__main__":
    unittest.main()
