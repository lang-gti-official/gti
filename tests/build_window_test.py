#!/usr/bin/env python3
"""Exercise the shared build window: slot files, waiting, dead-owner reclamation and counts."""

import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock
from harness_timeouts import scaled

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/build_window.py"
spec = importlib.util.spec_from_file_location("build_window", SCRIPT)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
Window = module.Window
Error = module.WindowError


def exited_pid():
    """A pid that belonged to a process which has already exited and been reaped."""
    child = subprocess.Popen([sys.executable, "-c", "pass"])
    child.wait()
    return child.pid


class BuildWindow(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gti-build-window-test-")
        self.dir = Path(self.temp.name).resolve()
        self.messages = []
        # The test must see neither the developer's nor CI's window settings.
        self.env = mock.patch.dict(os.environ, {}, clear=False)
        self.env.start()
        for name in list(os.environ):
            if name.startswith(module.ENV_PREFIX):
                del os.environ[name]

    def tearDown(self):
        self.env.stop()
        self.temp.cleanup()

    def window(self, **kwargs):
        kwargs.setdefault("poll", 0.05)
        kwargs.setdefault("log", self.messages.append)
        return Window(self.dir, **kwargs)

    def files(self, kind):
        return sorted((self.dir / kind).glob("*.json")) if (self.dir / kind).is_dir() else []

    def test_slot_records_owner_and_is_released(self):
        window = self.window()
        command = ["cmake", "--build", "build-release", "-j", "8"]
        with window.acquire("build", command, worktree="/tmp/task") as path:
            self.assertEqual(self.files("build"), [path])
            record = json.loads(path.read_text())
            self.assertEqual(record["pid"], os.getpid())
            self.assertEqual(record["kind"], "build")
            self.assertEqual(record["command"], command)
            self.assertEqual(record["worktree"], "/tmp/task")
            self.assertIn("T", record["time"])
            self.assertEqual([r["file"] for r in window.holders("build")], [path.name])
        self.assertEqual(self.files("build"), [])
        self.assertFalse((self.dir / "lock").exists())
        self.assertEqual(self.messages, [])

    def test_waits_until_a_full_window_frees_a_slot(self):
        holder = self.window()
        waiter = self.window()
        acquired = []
        with holder.acquire("suite", ["ctest", "--test-dir", "build-release", "-j", "6"]):
            thread = threading.Thread(target=lambda: acquired.append(
                (waiter.take("suite", ["ctest", "-R", "later"]), time.monotonic())))
            thread.start()
            time.sleep(0.3)
            self.assertEqual(acquired, [])
            released = time.monotonic()
        thread.join(timeout=scaled(5))
        self.assertEqual(len(acquired), 1)
        self.assertGreaterEqual(acquired[0][1], released)
        waiter.release(acquired[0][0])
        self.assertEqual(self.files("suite"), [])
        self.assertTrue(any("waiting for a suite slot (1/1 held)" in m and f"pid {os.getpid()}" in m
                            and "ctest --test-dir build-release -j 6" in m for m in self.messages))
        self.assertTrue(any(m.startswith("acquired a suite slot") for m in self.messages))

    def test_timeout_reports_who_holds_the_slots(self):
        with self.window().acquire("suite", ["ctest", "--test-dir", "build"]):
            with self.assertRaisesRegex(Error, r"timed out .* suite slot \(1/1 held\): pid \d+ .*ctest --test-dir build"):
                self.window(timeout=scaled(0.2)).take("suite", ["ctest"])
        self.assertEqual(self.files("suite"), [])

    def test_reclaims_slot_of_exited_owner(self):
        (self.dir / "suite").mkdir(parents=True)
        stale = self.dir / "suite" / "stale.json"
        stale.write_text(json.dumps({"pid": exited_pid(), "time": "earlier", "kind": "suite",
                                     "command": ["ctest", "--old"]}))
        window = self.window(timeout=scaled(2))
        self.assertEqual(window.holders("suite"), [])  # listing never reclaims
        self.assertTrue(stale.exists())
        with window.acquire("suite", ["ctest", "--new"]) as path:
            self.assertEqual(self.files("suite"), [path])
        self.assertFalse(stale.exists())
        self.assertTrue(any(m.startswith("reclaimed suite slot of exited pid") for m in self.messages))

    def test_live_owner_is_never_reclaimed(self):
        (self.dir / "suite").mkdir(parents=True)
        live = self.dir / "suite" / "live.json"
        live.write_text(json.dumps({"pid": os.getpid(), "time": "now", "kind": "suite", "command": ["ctest"]}))
        with self.assertRaisesRegex(Error, "timed out"):
            self.window(timeout=scaled(0.2)).take("suite", ["ctest"])
        self.assertTrue(live.exists())

    def test_counts_from_defaults_environment_and_flags(self):
        window = self.window(timeout=scaled(0.1))
        self.assertEqual(window.slots, {"suite": 1, "build": 2})
        with window.acquire("build", ["a"]), window.acquire("build", ["b"]):
            with self.assertRaisesRegex(Error, r"\(2/2 held\)"):
                window.take("build", ["c"])
        os.environ[module.ENV_PREFIX + "BUILD_SLOTS"] = "3"
        os.environ[module.ENV_PREFIX + "SUITE_SLOTS"] = "2"
        window = self.window(timeout=scaled(0.1))
        self.assertEqual(window.slots, {"suite": 2, "build": 3})
        with window.acquire("build", ["a"]), window.acquire("build", ["b"]), window.acquire("build", ["c"]):
            self.assertEqual(len(self.files("build")), 3)
        with window.acquire("suite", ["a"]), window.acquire("suite", ["b"]):
            self.assertEqual(len(self.files("suite")), 2)
        explicit = self.window(timeout=scaled(0.1), slots={"build": 1})
        self.assertEqual(explicit.slots, {"suite": 2, "build": 1})
        with explicit.acquire("build", ["a"]):
            with self.assertRaisesRegex(Error, r"\(1/1 held\)"):
                explicit.take("build", ["b"])
        os.environ[module.ENV_PREFIX + "BUILD_SLOTS"] = "zero"
        with self.assertRaisesRegex(Error, "BUILD_SLOTS"):
            self.window()
        os.environ[module.ENV_PREFIX + "BUILD_SLOTS"] = "0"
        with self.assertRaisesRegex(Error, "at least 1"):
            self.window()
        del os.environ[module.ENV_PREFIX + "BUILD_SLOTS"]
        with self.assertRaisesRegex(Error, "unknown slot kind"):
            self.window().take("deploy", ["x"])

    def test_stale_mutex_is_reclaimed(self):
        mutex = self.dir / "lock"
        mutex.mkdir(parents=True)
        (mutex / "owner.json").write_text(json.dumps({"pid": exited_pid(), "time": "earlier"}))
        with self.window(timeout=scaled(2)).acquire("build", ["cmake"]):
            pass
        self.assertFalse(mutex.exists())
        self.assertTrue(any("reclaimed the window mutex" in m for m in self.messages))

    def run_cli(self, *options, command=(), env=None, **kwargs):
        argv = [sys.executable, str(SCRIPT), "--dir", str(self.dir), "--poll-seconds", "0.05", *options]
        if command:
            argv += ["--", *command]
        return subprocess.run(argv, capture_output=True, text=True, check=False,
                              env={**os.environ, **(env or {})}, **kwargs)

    def test_cli_holds_slot_during_command_and_propagates_exit_status(self):
        probe = ("import glob, sys; names = glob.glob(sys.argv[1] + '/build/*.json'); "
                 "print(len(names)); sys.exit(7)")
        result = self.run_cli("--kind", "build", command=[sys.executable, "-c", probe, str(self.dir)])
        self.assertEqual(result.returncode, 7, result.stderr)
        self.assertEqual(result.stdout.strip(), "1")
        self.assertEqual(self.files("build"), [])
        result = self.run_cli("--kind", "build", command=["gti-no-such-command-for-the-window"])
        self.assertEqual(result.returncode, 2)
        self.assertIn("command not found", result.stderr)
        result = self.run_cli(command=["true"])
        self.assertEqual(result.returncode, 2)
        self.assertIn("--kind", result.stderr)
        result = self.run_cli("--kind", "suite")
        self.assertEqual(result.returncode, 2)
        self.assertIn("after `--`", result.stderr)

    def test_cli_flags_override_environment_and_status_lists_holders(self):
        with self.window().acquire("suite", ["ctest", "--test-dir", "build-release"]):
            result = self.run_cli("--kind", "suite", "--timeout-seconds", "0.2", command=["true"],
                                  env={module.ENV_PREFIX + "SUITE_SLOTS": "2"})
            self.assertEqual(result.returncode, 0, result.stderr)
            result = self.run_cli("--kind", "suite", "--suite-slots", "1", "--timeout-seconds", "0.2",
                                  command=["true"], env={module.ENV_PREFIX + "SUITE_SLOTS": "2"})
            self.assertEqual(result.returncode, 2)
            self.assertIn("timed out", result.stderr)
            self.assertIn("ctest --test-dir build-release", result.stderr)
            status = json.loads(self.run_cli("--status").stdout)
        self.assertEqual(status["slots"], {"suite": 1, "build": 2})
        self.assertEqual([h["pid"] for h in status["holders"]["suite"]], [os.getpid()])
        self.assertEqual(status["holders"]["build"], [])

    @unittest.skipUnless(os.name == "posix", "process-group interrupt is POSIX only")
    def test_interrupt_releases_the_slot(self):
        process = subprocess.Popen(
            [sys.executable, str(SCRIPT), "--dir", str(self.dir), "--kind", "suite", "--",
             sys.executable, "-c", "import time; time.sleep(60)"],
            start_new_session=True, stderr=subprocess.PIPE, text=True)
        deadline = time.monotonic() + 20
        while not self.files("suite") and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertEqual(len(self.files("suite")), 1)
        time.sleep(0.2)
        os.killpg(process.pid, signal.SIGINT)
        _, stderr = process.communicate(timeout=scaled(20))
        self.assertNotEqual(process.returncode, 0, stderr)
        self.assertEqual(self.files("suite"), [])
        self.assertFalse((self.dir / "lock").exists())

    def test_pid_liveness(self):
        self.assertTrue(module.pid_alive(os.getpid()))
        self.assertFalse(module.pid_alive(0))
        self.assertFalse(module.pid_alive("1"))
        self.assertFalse(module.pid_alive(True))
        if os.name == "posix":
            self.assertFalse(module.pid_alive(exited_pid()))


if __name__ == "__main__":
    unittest.main()
