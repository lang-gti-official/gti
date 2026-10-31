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

    def test_worker_builds_actual_dirty_snapshot_with_original_head(self):
        (self.repo / "tracked.txt").write_text("changed\n")
        (self.repo / "deleted.txt").unlink()
        (self.repo / "new.txt").write_text("new\n")
        head = runner.git(self.repo, "rev-parse", "HEAD").decode().strip()
        code = ("from pathlib import Path;import subprocess;"
                "assert Path('tracked.txt').read_text() == 'changed\\n';"
                "assert not Path('deleted.txt').exists();"
                "assert Path('new.txt').read_text() == 'new\\n';"
                f"assert subprocess.check_output(['git','rev-parse','HEAD']).decode().strip() == '{head}';"
                "print(subprocess.check_output(['git','status','--porcelain']).decode())")
        run_id = self.submit(code)
        self.assertEqual(self.wait(run_id)["exit_code"], 0)
        self.assert_clean(run_id)

    def test_invalid_step_plans_and_optimized_defaults(self):
        for steps in ([], {}, [{"name": None, "kind": "suite", "cwd": "source", "argv": ["true"]}],
                      [{"name": "bad", "kind": "unknown", "cwd": "source", "argv": ["true"]}],
                      [{"name": "bad", "kind": "suite", "cwd": "../", "argv": ["true"]}]):
            with self.assertRaises(runner.RunnerError):
                runner.validate_steps(steps)
        steps = runner.default_steps({"build_type": "Release", "cmake_args": [], "targets": ["gti"],
                                      "full_suite": True, "ctest_regex": None})
        self.assertIn("-DCMAKE_BUILD_TYPE=Release", steps[0]["argv"])
        self.assertIn("--no-tests=error", steps[-1]["argv"])
        self.assertIn("--test-load", steps[-1]["argv"])
        with mock.patch.object(runner, "local_directory", return_value=self.directory):
            with self.assertRaises(runner.RunnerError):
                runner.main(["--host", "example", "run", "--cmake-arg=-DCMAKE_BUILD_TYPE=Debug"])

    def test_successful_step_reports_source_mutation_before_cleanup(self):
        head = runner.git(self.repo, "rev-parse", "HEAD").decode().strip()
        run_id = self.submit("from pathlib import Path;Path('tracked.txt').write_text('modified by test')")
        report = self.wait(run_id)
        self.assertEqual(report["exit_code"], 0)
        self.assertEqual(report["source"]["status"], "")
        self.assertEqual(report["final_source"]["head"], head)
        self.assertIn(" M tracked.txt", report["final_source"]["status"])
        self.assert_clean(run_id)

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
        outputs = [process.communicate(timeout=10) for process in processes]
        for process, (out, err) in zip(processes, outputs):
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

    def test_terminal_publication_allows_immediate_result_removal(self):
        run_id = self.prepare_inline_worker()
        run = self.root / "runs" / run_id
        path = run / "input/request.json"
        request = runner.read_json(path)
        request["steps"][0]["argv"] = [sys.executable, "-c", "pass"]
        runner.write_json(path, request)
        write_json = runner.write_json
        removed = []

        def publish(path, value):
            write_json(path, value)
            if Path(path) == run / "state.json" and value["state"] in runner.TERMINAL:
                # A client may fetch/remove immediately after observing completion.
                self.assert_clean(run_id)
                runner.remote_action("remove", str(self.root), run_id)
                removed.append(run_id)

        with mock.patch.object(runner, "write_json", side_effect=publish):
            self.assertEqual(runner.worker(str(self.root), run_id), 0)
        self.assertEqual(removed, [run_id])
        self.assertFalse(run.exists())

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

    def test_wrong_remote_platform_fails_before_commands_and_still_cleans_up(self):
        run_id = self.prepare_inline_worker()
        path = self.root / "runs" / run_id / "input/request.json"
        request = runner.read_json(path)
        request["required_platform"] = "Linux" if runner.platform.system() == "Darwin" else "Darwin"
        runner.write_json(path, request)
        self.assertEqual(runner.worker(str(self.root), run_id), 1)
        report = runner.read_json(self.root / "runs" / run_id / "state.json")
        self.assertIn("run requires", report["error"])
        self.assertEqual(report["steps"], [])
        self.assert_clean(run_id)

    def test_detached_submission_records_host_and_resource_defaults_before_upload(self):
        local = self.directory / "client"
        submitted = []

        def submit(config, action, run_id=None, input_path=None, output_path=None):
            self.assertEqual(action, "submit")
            saved = runner.read_json(local / "runs" / (run_id + ".json"))
            self.assertEqual(saved["runner"], "ubuntu-8")
            self.assertEqual(saved["connection"], config)
            with tarfile.open(input_path) as archive:
                submitted.append(json.load(archive.extractfile("request.json")))
            return {"id": run_id}

        with mock.patch.object(runner, "local_directory", return_value=local), \
                mock.patch.object(runner, "connection", side_effect=submit), \
                mock.patch.dict(os.environ, {"GTI_REMOTE_HOST": "", "GTI_REMOTE_ROOT": ""}), \
                mock.patch("sys.stdout", new_callable=io.StringIO):
            self.assertEqual(runner.main(["--repo", str(self.repo), "--runner", "ubuntu-8",
                                         "run", "--detach", "--target", "gti"]), 0)
        self.assertEqual(submitted[0]["runner"], {"name": "ubuntu-8", "host": "root@100.81.111.76"})
        self.assertEqual(submitted[0]["jobs"], 3)
        self.assertEqual(submitted[0]["test_jobs"], 6)
        self.assertEqual(submitted[0]["required_platform"], "Linux")
        self.assertEqual(submitted[0]["cc"], "/usr/bin/gcc-14")
        self.assertEqual(submitted[0]["cxx"], "/usr/bin/g++-14")


class RunnerConnections(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gti-runner-connections-")
        self.local = Path(self.temporary.name).resolve()
        self.path = self.local / "config.json"
        self.environment = mock.patch.dict(os.environ, {
            "GTI_REMOTE_RUNNER": "", "GTI_REMOTE_HOST": "", "GTI_REMOTE_ROOT": ""})
        self.environment.start()

    def tearDown(self):
        self.environment.stop()
        self.temporary.cleanup()

    def args(self, *argv):
        return runner.parser().parse_args(argv or ["status"])

    def test_three_presets_and_legacy_single_host_migration(self):
        settings = runner.load_settings(self.path)
        self.assertEqual(set(settings["hosts"]), {"mini", "ubuntu-6", "ubuntu-8"})
        self.assertEqual(settings["default"], "mini")
        old = {"host": "custom-mini", "python": "/usr/local/bin/python3",
               "root": "/custom/gti-remote-runner"}
        runner.write_json(self.path, old)
        settings = runner.load_settings(self.path)
        name, config = runner.select_connection(self.args(), settings, self.local)
        self.assertEqual(name, "mini")
        for key, value in old.items():
            self.assertEqual(config[key], value)
        self.assertNotIn("platform", config)
        self.assertEqual(runner.read_json(self.path), old)  # reads do not migrate on disk

    def test_named_cli_environment_and_default_precedence(self):
        settings = runner.load_settings(self.path)
        settings["default"] = "ubuntu-6"
        self.assertEqual(runner.select_connection(self.args(), settings, self.local)[0], "ubuntu-6")
        with mock.patch.dict(os.environ, {"GTI_REMOTE_RUNNER": "ubuntu-8"}):
            self.assertEqual(runner.select_connection(self.args(), settings, self.local)[0], "ubuntu-8")
            self.assertEqual(runner.select_connection(self.args("--runner", "mini", "status"),
                                                     settings, self.local)[0], "mini")
        with mock.patch.dict(os.environ, {"GTI_REMOTE_HOST": "environment-alias"}):
            config = runner.select_connection(self.args("--runner", "ubuntu-6", "--host", "explicit-alias",
                                                        "status"), settings, self.local)[1]
            self.assertEqual(config["host"], "explicit-alias")
        self.assertEqual(settings["hosts"]["ubuntu-6"]["host"], "root@100.77.66.96")

    def test_configure_preserves_other_hosts_and_only_changes_default_on_request(self):
        runner.save_connection(self.path, "ubuntu-6", dict(runner.HOSTS["ubuntu-6"], jobs=2))
        runner.save_connection(self.path, "ubuntu-8", dict(runner.HOSTS["ubuntu-8"], jobs=1))
        settings = runner.load_settings(self.path)
        self.assertEqual(settings["default"], "mini")
        self.assertEqual(settings["hosts"]["ubuntu-6"]["jobs"], 2)
        self.assertEqual(settings["hosts"]["ubuntu-8"]["jobs"], 1)
        # Old worktrees still use these flat fields during rolling updates.
        saved = runner.read_json(self.path)
        for key in ("host", "root", "python"):
            self.assertEqual(saved[key], runner.HOSTS["mini"][key])
        runner.save_connection(self.path, "ubuntu-8", runner.HOSTS["ubuntu-8"], make_default=True)
        self.assertEqual(runner.load_settings(self.path)["default"], "ubuntu-8")
        self.assertEqual(runner.read_json(self.path)["host"], runner.HOSTS["ubuntu-8"]["host"])

    def test_cli_configure_validates_os_and_preserves_default(self):
        with mock.patch.object(runner, "local_directory", return_value=self.local), \
                mock.patch.object(runner, "connection", return_value={"platform": "Linux"}) as connection, \
                mock.patch("sys.stdout", new_callable=io.StringIO):
            self.assertEqual(runner.main(["--runner", "ubuntu-8", "configure"]), 0)
            self.assertEqual(connection.call_args.args[0]["host"], "root@100.81.111.76")
            self.assertEqual(runner.load_settings(self.path)["default"], "mini")
            self.assertEqual(runner.main(["--runner", "ubuntu-8", "configure", "--default"]), 0)
            self.assertEqual(runner.load_settings(self.path)["default"], "ubuntu-8")

    def test_cli_configure_rejects_wrong_os_without_saving(self):
        with mock.patch.object(runner, "local_directory", return_value=self.local), \
                mock.patch.object(runner, "connection", return_value={"platform": "Darwin"}):
            with self.assertRaisesRegex(runner.RunnerError, "platform differs"):
                runner.main(["--runner", "ubuntu-6", "configure"])
        self.assertFalse(self.path.exists())

    def test_concurrent_configuration_updates_preserve_every_named_host(self):
        commands = []
        for index in range(3):
            code = ("import sys;sys.path.insert(0," + repr(str(SCRIPT.parent)) + ");"
                    "import remote_runner as r;"
                    f"r.save_connection(r.Path({str(self.path)!r}), 'custom-{index}',"
                    f"dict(r.HOSTS['ubuntu-6'],jobs={index + 1}))")
            commands.append([sys.executable, "-c", code])
        processes = [subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                     for argv in commands]
        for process in processes:
            out, err = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 0, err.decode())
        settings = runner.load_settings(self.path)
        for index in range(3):
            self.assertEqual(settings["hosts"][f"custom-{index}"]["jobs"], index + 1)

    def test_detached_run_uses_origin_after_default_environment_and_profile_change(self):
        run_id = "run-" + "a" * 32
        original = dict(runner.HOSTS["ubuntu-6"])
        runner.record_connection(self.local, run_id, "ubuntu-6", original)
        settings = runner.load_settings(self.path)
        settings["default"] = "ubuntu-8"
        settings["hosts"]["ubuntu-6"]["host"] = "new-address"
        with mock.patch.dict(os.environ, {"GTI_REMOTE_RUNNER": "mini", "GTI_REMOTE_HOST": "wrong-address",
                                         "GTI_REMOTE_ROOT": "/wrong/gti-remote-runner"}):
            for command in ("status", "fetch", "cancel", "clean"):
                name, config = runner.select_connection(self.args(command, run_id), settings,
                                                        self.local, run_id)
                self.assertEqual((name, config), ("ubuntu-6", original))
        with self.assertRaisesRegex(runner.RunnerError, "belongs to ubuntu-6"):
            runner.select_connection(self.args("--runner", "ubuntu-8", "fetch", run_id),
                                     settings, self.local, run_id)
        with self.assertRaisesRegex(runner.RunnerError, "redirect"):
            runner.select_connection(self.args("--host", "wrong", "cancel", run_id),
                                     settings, self.local, run_id)
        with self.assertRaisesRegex(runner.RunnerError, "overwrite"):
            runner.record_connection(self.local, run_id, "ubuntu-8", runner.HOSTS["ubuntu-8"])

    def test_legacy_unrecorded_runs_use_the_selected_connection(self):
        settings = runner.load_settings(self.path)
        run_id = "run-" + "b" * 32
        name, config = runner.select_connection(self.args("--runner", "mini", "fetch", run_id),
                                                settings, self.local, run_id)
        self.assertEqual(name, "mini")
        self.assertEqual(config, runner.HOSTS["mini"])

    def test_reject_malformed_settings_connections_names_and_identity(self):
        for value in ([], {"schema": 99}, {"schema": 2, "hosts": {}, "default": []},
                      dict(runner.HOSTS["mini"], token="must not be printed"),
                      {"schema": 2, "hosts": {"bad name": {}}, "default": "mini"}):
            runner.write_json(self.path, value)
            with self.assertRaises(runner.RunnerError):
                runner.load_settings(self.path)
        for key, value in (("host", "-option"), ("host", "host with space"), ("root", "/"),
                           ("root", "/tmp/../gti-remote-runner"), ("transport", "unverified"),
                           ("python", ""), ("jobs", 0), ("test_jobs", True), ("token", "secret")):
            with self.assertRaises(runner.RunnerError):
                runner.validate_connection(dict(runner.HOSTS["ubuntu-6"], **{key: value}))
        self.path.unlink()
        settings = runner.load_settings(self.path)
        for name in ("unknown", "bad/name", ""):
            with self.assertRaises(runner.RunnerError):
                runner.select_connection(self.args("--runner", name, "status"), settings, self.local)
        run_id = "run-" + "c" * 32
        with self.assertRaisesRegex(runner.RunnerError, "invalid run"):
            runner.record_connection(self.local, run_id, "request.json", runner.HOSTS["mini"])
        runner.record_connection(self.local, run_id, "mini", runner.HOSTS["mini"])
        path = self.local / "runs" / (run_id + ".json")
        record = runner.read_json(path)
        record["id"] = "run-" + "d" * 32
        runner.write_json(path, record)
        with self.assertRaisesRegex(runner.RunnerError, "identity mismatch"):
            runner.select_connection(self.args("fetch", run_id), settings, self.local, run_id)

    def test_ssh_and_tailscale_transports_keep_noninteractive_host_verification(self):
        for name in ("mini", "ubuntu-6"):
            config = runner.HOSTS[name]
            with mock.patch.object(runner.subprocess, "run",
                                   return_value=subprocess.CompletedProcess([], 0, b"{}")) as run:
                runner.connection(config, "status")
            argv = run.call_args.args[0]
            if name == "mini":
                self.assertEqual(argv[0], "ssh")
                self.assertIn("--", argv)
            else:
                self.assertEqual(argv[:3], ["tailscale", "ssh", config["host"]])
            self.assertIn("BatchMode=yes", argv)
            self.assertIn("ConnectTimeout=10", argv)
            self.assertNotIn("StrictHostKeyChecking=no", argv)
        with mock.patch.object(runner.subprocess, "run",
                               return_value=subprocess.CompletedProcess([], 255, b"")) as run:
            with self.assertRaisesRegex(runner.RunnerError, "SSH runner action"):
                runner.connection(runner.HOSTS["ubuntu-6"], "status")
            self.assertEqual(run.call_count, 1)  # no unverified fallback

    def test_linux_and_macos_window_policy_and_llvm_discovery(self):
        for system, slots in (("Darwin", {"build": 2, "suite": 1}), ("Linux", {"build": 1, "suite": 1})):
            with mock.patch.object(runner.platform, "system", return_value=system):
                self.assertEqual(runner.window_slots(), slots)
        llvm = self.local / "llvm"
        llvm.mkdir()
        (llvm / "LLVMConfig.cmake").write_text("fixture")
        with mock.patch.object(runner.platform, "system", return_value="Linux"), \
                mock.patch.object(runner.shutil, "which", return_value="/usr/bin/llvm-config"), \
                mock.patch.object(runner.subprocess, "run",
                                  return_value=subprocess.CompletedProcess([], 0, str(llvm) + "\n")) as run:
            self.assertEqual(runner.llvm_directory(), str(llvm))
            self.assertEqual(run.call_args.args[0], ["/usr/bin/llvm-config", "--cmakedir"])
        request = {"build_type": "Release", "cmake_args": [], "targets": ["gti"],
                   "full_suite": True, "ctest_regex": None, "jobs": 3, "test_jobs": 6}
        with mock.patch.object(runner.platform, "system", return_value="Linux"), \
                mock.patch.object(runner, "llvm_directory", return_value=str(llvm)):
            steps = runner.default_steps(request)
        self.assertIn("-DLLVM_DIR=" + str(llvm), steps[0]["argv"])
        self.assertFalse(any("CMAKE_CXX_COMPILER=" in arg for arg in steps[0]["argv"]))
        self.assertEqual(steps[1]["argv"][4], "3")
        self.assertIn("6", steps[2]["argv"])
        request["cmake_args"] = ["-DCMAKE_CXX_COMPILER=/custom/cxx"]
        request.update(cc="/usr/bin/gcc-14", cxx="/usr/bin/g++-14")
        with mock.patch.object(runner, "llvm_directory", return_value=None), \
                mock.patch.dict(os.environ, {"CC": "", "CXX": ""}):
            configure = runner.default_steps(request)[0]["argv"]
            self.assertIn("-DCMAKE_C_COMPILER=/usr/bin/gcc-14", configure)
            self.assertIn("-DCMAKE_CXX_COMPILER=/usr/bin/g++-14", configure)
            self.assertEqual(configure[-1], "-DCMAKE_CXX_COMPILER=/custom/cxx")
        with mock.patch.object(runner, "llvm_directory", return_value=None), \
                mock.patch.dict(os.environ, {"CC": "/env/cc", "CXX": "/env/cxx"}):
            configure = runner.default_steps(request)[0]["argv"]
            self.assertFalse(any(arg.endswith("=/usr/bin/gcc-14") or arg.endswith("=/usr/bin/g++-14")
                                 for arg in configure))
        config = runner.select_connection(self.args("--runner", "ubuntu-6", "--cc", "/custom/cc",
                                                     "--cxx", "/custom/cxx", "status"),
                                          runner.load_settings(self.path), self.local)[1]
        self.assertEqual((config["cc"], config["cxx"]), ("/custom/cc", "/custom/cxx"))

    def test_platform_selection_rejects_wrong_host_before_snapshot(self):
        with mock.patch.object(runner, "local_directory", return_value=self.local), \
                mock.patch.object(runner, "snapshot") as snapshot:
            with self.assertRaisesRegex(runner.RunnerError, "requires Darwin"):
                runner.main(["--runner", "ubuntu-6", "run", "--platform", "Darwin"])
            snapshot.assert_not_called()

    def test_hosts_is_offline_and_status_all_preserves_partial_failures(self):
        with mock.patch.object(runner, "local_directory", return_value=self.local), \
                mock.patch.object(runner, "connection") as connection, \
                mock.patch("sys.stdout", new_callable=io.StringIO) as output:
            self.assertEqual(runner.main(["hosts"]), 0)
            self.assertEqual(len(json.loads(output.getvalue())["hosts"]), 3)
            connection.assert_not_called()

        def status(config, action):
            if config["host"] == runner.HOSTS["mini"]["host"]:
                raise runner.RunnerError("offline")
            return {"platform": "Linux", "window": {"build": [], "suite": []}, "runs": [
                {"id": "active", "state": "running", "phase": "build", "source": {"large": "metadata"}},
                {"id": "done", "state": "passed", "phase": "finished", "steps": ["long history"]}]}

        with mock.patch.object(runner, "local_directory", return_value=self.local), \
                mock.patch.object(runner, "connection", side_effect=status), \
                mock.patch("sys.stdout", new_callable=io.StringIO) as output:
            self.assertEqual(runner.main(["status", "--all"]), 1)
            result = json.loads(output.getvalue())
        self.assertEqual(result["mini"], {"error": "offline"})
        self.assertIn("window", result["ubuntu-6"])
        self.assertIn("window", result["ubuntu-8"])
        self.assertEqual(result["ubuntu-8"]["active_runs"],
                         [{"id": "active", "state": "running", "phase": "build"}])
        self.assertEqual(result["ubuntu-8"]["retained_results"], 1)
        self.assertNotIn("runs", result["ubuntu-8"])


if __name__ == "__main__":
    unittest.main()
