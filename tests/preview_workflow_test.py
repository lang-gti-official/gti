#!/usr/bin/env python3
"""Preview admission with real disposable Git repositories and fake builds/GitHub."""

from contextlib import nullcontext
import json
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import agent_workflow_test as lifecycle
from agent_workflow import git, WorkflowError
from preview_workflow import Preview, tests as focused_tests
import preview_workflow
import preview_examples
from preview_examples import NESTED, required_tests, validate_inventory, validate_results, validate_sources
import install_neovim_local as installer


class PreviewTests(unittest.TestCase):
    setUp = lifecycle.Lifecycle.setUp
    tearDown = lifecycle.Lifecycle.tearDown

    def feature(self, name="task"):
        tree = self.root / name
        self.flow.start(name, name, tree, "codex/" + name)
        (tree / (name + ".txt")).write_text(name)
        self.flow.checkpoint(name, name, [name + ".txt"])
        return tree

    def prepare(self, name="task", suffix="candidate"):
        return Preview(self.flow).prepare(name, self.root / suffix, [name + "_focused"])

    def verify(self, candidate, failure=False):
        original = subprocess.run
        executed = []

        def run(argv, *args, **kwargs):
            if argv[0] == "git":
                return original(argv, *args, **kwargs)
            executed.append(argv)
            return subprocess.CompletedProcess(argv, 1 if failure else 0)

        with patch("preview_workflow.subprocess.run", side_effect=run), patch.object(
                self.flow.window, "acquire", return_value=nullcontext()):
            result = Preview(self.flow).verify(candidate["id"], [])
        return result, executed

    def test_combines_features_retests_each_and_preserves_main(self):
        original_main = git(self.remote, "rev-parse", "main")
        self.feature()
        first = self.prepare()
        with self.assertRaises(WorkflowError):
            Preview(self.flow).publish(first["id"])
        self.verify(first)
        Preview(self.flow).publish(first["id"])
        self.feature("second")
        second = self.prepare("second", "candidate2")
        _, executed = self.verify(second)
        self.assertEqual(executed[-1], ["python3", "scripts/preview_examples.py", "out/preview-build",
                                      "--focused=second_focused", "--focused=task_focused"])
        Preview(self.flow).publish(second["id"])
        self.assertEqual(git(self.remote, "rev-parse", "main"), original_main)
        self.assertEqual(git(self.remote, "rev-parse", "preview"), second["head"])
        self.assertEqual([row["pr"] for row in second["manifest"]["features"]], [1, 2])
        self.assertEqual(git(second["worktree"], "show", "HEAD:task.txt"), "task")

    def test_failed_validation_does_not_publish(self):
        self.feature()
        candidate = self.prepare()
        with self.assertRaises(WorkflowError):
            self.verify(candidate, failure=True)
        with self.assertRaises(WorkflowError):
            Preview(self.flow).publish(candidate["id"])
        self.assertIsNone(Preview(self.flow).remote())

    def test_remote_results_are_new_per_attempt_and_retained(self):
        candidate = Preview(self.flow).prepare(None, self.root / "seed", [])
        destinations = []

        def remote_run(argv):
            destination = Path(argv[argv.index("--results") + 1])
            self.assertFalse(destination.exists())
            destination.mkdir()
            destinations.append(destination)
            (destination / "report.json").write_text(json.dumps({
                "state": "passed", "exit_code": 0,
                "source": {"head": candidate["head"], "status": ""},
                "final_source": {"head": candidate["head"], "status": ""}}))
            return 0

        with patch("preview_workflow.load_remote_runner") as runner:
            runner.return_value.main.side_effect = remote_run
            for _ in range(2):
                result = Preview(self.flow).verify(candidate["id"], [], remote=True)
                self.assertEqual(result["verified"]["logs"], str(destinations[-1]))
        self.assertNotEqual(*destinations)
        self.assertTrue(all((path / "report.json").is_file() for path in destinations))

    def test_seed_builds_only_example_dependencies_but_features_build_all(self):
        seed = Preview(self.flow).prepare(None, self.root / "seed", [])
        _, executed = self.verify(seed)
        self.assertEqual(executed[1], ["cmake", "--build", "out/preview-build", "--parallel", "4",
                                      "--target", "gti", "gti_runtime", "gti_lsp"])
        self.feature()
        feature = self.prepare()
        _, executed = self.verify(feature)
        self.assertNotIn("--target", executed[1])

    def test_example_gate_accepts_real_cmake_cache_and_rejects_debug_or_wrong_source(self):
        source = self.root / "cache-source"
        source.mkdir()
        (source / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.20)\nproject(cache NONE)\n")
        build = self.root / "cache-build"
        subprocess.run(["cmake", "-S", str(source), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release"],
                       check=True, capture_output=True)
        cache = build / "CMakeCache.txt"
        original_cache = cache.read_text()

        def run(argv, **kwargs):
            if argv[0] == "git":
                return subprocess.CompletedProcess(argv, 0, stdout="\0".join(
                    f"examples/{directory}/main.gti" for directory in NESTED))
            required = required_tests(sys.platform)
            if "--show-only=json-v1" in argv:
                return subprocess.CompletedProcess(argv, 0, stdout=json.dumps({
                    "tests": [{"name": name} for name in required]}))
            result = Path(argv[argv.index("--output-junit") + 1])
            result.write_text("<testsuite>" + "".join(f'<testcase name="{name}"/>' for name in required)
                              + "</testsuite>")
            return subprocess.CompletedProcess(argv, 0)

        with patch.object(preview_examples, "__file__", str(source / "scripts/preview_examples.py")), \
                patch.object(sys, "argv", ["preview_examples.py", str(build)]), \
                patch("preview_examples.subprocess.run", side_effect=run):
            preview_examples.main()
            cache.write_text(original_cache.replace("CMAKE_BUILD_TYPE:UNINITIALIZED=Release",
                                                    "CMAKE_BUILD_TYPE:UNINITIALIZED=Debug"))
            with self.assertRaisesRegex(ValueError, "optimized compiler"):
                preview_examples.main()
            cache.write_text(original_cache.replace(str(source), str(self.root / "other")))
            with self.assertRaisesRegex(ValueError, "optimized compiler"):
                preview_examples.main()

    def test_preview_cannot_finish_main_or_resolve_a_release(self):
        tree = self.feature()
        (tree / ".gti-preview.json").write_text("{}")
        self.flow.checkpoint("task", "preview marker", [".gti-preview.json"])
        with self.assertRaisesRegex(Exception, "cannot merge wholesale"):
            self.flow.finish("task", "Never merge preview")
        head = git(tree, "rev-parse", "HEAD")
        result = subprocess.run([sys.executable, str(ROOT / "scripts/resolve_release_source.py"),
            "source", "--sha", head, "--event", "workflow_dispatch", "--output", str(self.root / "release")],
            cwd=tree, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("preview candidates cannot publish", result.stderr)
        self.assertFalse((self.root / "release").exists())
        admission = subprocess.run([sys.executable, str(ROOT / "scripts/check_release_version.py"),
            "origin/main", head], cwd=tree, capture_output=True, text=True)
        self.assertNotEqual(admission.returncode, 0)
        self.assertIn("preview candidates cannot enter main", admission.stderr)
        git(tree, "rm", ".gti-preview.json")
        git(tree, "commit", "-m", "Removing marker does not erase preview history")
        self.flow.checkpoint("task")
        with self.assertRaisesRegex(Exception, "Preview history cannot"):
            self.flow.finish("task", "Never merge preview")

    def test_dirty_candidate_and_racing_publishers_refused(self):
        self.feature()
        first = self.prepare()
        second = self.prepare(suffix="candidate2")
        self.verify(first)
        self.verify(second)
        dirty = Path(first["worktree"]) / "dirty.txt"
        dirty.write_text("preserve")
        with self.assertRaises(Exception):
            Preview(self.flow).publish(first["id"])
        self.assertEqual(dirty.read_text(), "preserve")
        Preview(self.flow).publish(second["id"])
        with self.assertRaises(Exception):
            Preview(self.flow).publish(first["id"])
        self.assertEqual(Preview(self.flow).remote(), second["head"])

    def test_publish_lease_closes_race_after_remote_check(self):
        self.feature()
        first = self.prepare()
        second = self.prepare(suffix="candidate2")
        self.verify(first)
        original = preview_workflow.git

        def racing_git(root, *args, **kwargs):
            if args and args[0] == "push":
                git(self.remote, "update-ref", "refs/heads/preview", second["head"])
            return original(root, *args, **kwargs)

        with patch.object(preview_workflow, "git", side_effect=racing_git), self.assertRaises(WorkflowError):
            Preview(self.flow).publish(first["id"])
        self.assertEqual(Preview(self.flow).remote(), second["head"])

    def test_lost_publish_response_can_be_retried(self):
        self.feature()
        candidate = self.prepare()
        self.verify(candidate)
        original = preview_workflow.git

        def lost_response(root, *args, **kwargs):
            result = original(root, *args, **kwargs)
            if args and args[0] == "push":
                raise WorkflowError("simulated lost response")
            return result

        with patch.object(preview_workflow, "git", side_effect=lost_response), self.assertRaises(WorkflowError):
            Preview(self.flow).publish(candidate["id"])
        self.assertEqual(Preview(self.flow).publish(candidate["id"])["phase"], "published")

    def test_candidate_checkpoint_can_resume_after_push_failure(self):
        self.feature()
        original = preview_workflow.git

        def unavailable(root, *args, **kwargs):
            if args and args[0] == "push":
                raise WorkflowError("simulated push failure")
            return original(root, *args, **kwargs)

        with patch.object(preview_workflow, "git", side_effect=unavailable), self.assertRaises(WorkflowError):
            self.prepare()
        preview = Preview(self.flow)
        state = json.loads(next(preview.directory.glob("*.json")).read_text())
        self.assertEqual(state["phase"], "prepared")
        with self.assertRaises(WorkflowError):
            preview.verify(state["id"], [])
        recovered = preview.resume(state["id"])
        self.assertEqual(recovered["head"], state["head"])
        self.assertTrue(recovered["checkpointed"])

    def test_revert_preserves_history_and_original_pr(self):
        self.feature()
        first = self.prepare()
        self.verify(first)
        Preview(self.flow).publish(first["id"])
        removed = Preview(self.flow).prepare("task", self.root / "removed", remove=True)
        self.assertEqual(removed["manifest"]["features"], [])
        self.assertFalse((Path(removed["worktree"]) / "task.txt").exists())
        self.verify(removed)
        Preview(self.flow).publish(removed["id"])
        self.assertTrue(self.flow.ancestor(first["head"], removed["head"]))
        self.assertEqual(self.api.prs[0]["state"], "open")
        (self.root / "task" / "new.txt").write_text("updated feature")
        self.flow.checkpoint("task", "Update removed feature", ["new.txt"])
        restored = self.prepare(suffix="restored")
        self.assertEqual((Path(restored["worktree"]) / "task.txt").read_text(), "task")
        self.assertEqual((Path(restored["worktree"]) / "new.txt").read_text(), "updated feature")
        self.verify(restored)
        Preview(self.flow).publish(restored["id"])
        removed_again = Preview(self.flow).prepare("task", self.root / "removed-again", remove=True)
        self.assertFalse((Path(removed_again["worktree"]) / "task.txt").exists())
        self.assertFalse((Path(removed_again["worktree"]) / "new.txt").exists())

    def test_already_reachable_head_and_reverting_main_are_refused(self):
        tree = self.feature()
        first = self.prepare()
        self.verify(first)
        Preview(self.flow).publish(first["id"])
        duplicate = self.root / "duplicate"
        self.flow.start("duplicate", "Duplicate", duplicate, "codex/duplicate")
        git(duplicate, "merge", "--ff-only", "codex/task")
        self.flow.checkpoint("duplicate")
        with self.assertRaisesRegex(WorkflowError, "already reachable"):
            self.prepare("duplicate", "duplicate-candidate")
        git(self.remote, "update-ref", "refs/heads/main", git(tree, "rev-parse", "HEAD"))
        with self.assertRaisesRegex(WorkflowError, "already integrated"):
            Preview(self.flow).prepare("task", self.root / "remove-main", remove=True)
        self.assertFalse((self.root / "remove-main").exists())

    def refresh_removed_feature_after_main_acceptance(self, add_feature):
        tree = self.feature()
        first = self.prepare()
        self.verify(first)
        preview = Preview(self.flow)
        preview.publish(first["id"])
        removed = preview.prepare("task", self.root / "removed", remove=True)
        self.verify(removed)
        preview.publish(removed["id"])
        # The original PR is accepted independently of its preview admission.
        accepted = git(tree, "rev-parse", "HEAD")
        git(self.remote, "update-ref", "refs/heads/main", accepted)
        if add_feature:
            self.feature("second")
            refreshed = self.prepare("second", "refreshed")
        else:
            refreshed = preview.prepare(None, self.root / "refreshed")
        root = Path(refreshed["worktree"])
        self.assertEqual((root / "task.txt").read_text(), "task")
        record = refreshed["manifest"]["removed"][0]
        self.assertTrue(record["restored"])
        self.assertEqual(record["accepted_in_main"], accepted)
        self.assertEqual([row["task"] for row in refreshed["manifest"]["features"]],
                         ["second"] if add_feature else [])
        self.verify(refreshed)
        preview.publish(refreshed["id"])
        self.assertTrue(self.flow.ancestor(removed["head"], refreshed["head"]))

    def test_seed_restores_removed_feature_accepted_into_main(self):
        self.refresh_removed_feature_after_main_acceptance(False)

    def test_admission_restores_removed_dependency_accepted_into_main(self):
        self.refresh_removed_feature_after_main_acceptance(True)

    def test_seed_and_conflict_resume(self):
        seeded = Preview(self.flow).prepare(None, self.root / "seed")
        self.assertEqual(seeded["manifest"]["features"], [])
        self.verify(seeded)
        Preview(self.flow).publish(seeded["id"])
        self.feature()
        first = self.prepare()
        self.verify(first)
        Preview(self.flow).publish(first["id"])
        second = self.feature("second")
        (second / "task.txt").write_text("conflicting task")
        self.flow.checkpoint("second", "conflict", ["task.txt"])
        preview = Preview(self.flow)
        with self.assertRaises(WorkflowError):
            preview.prepare("second", self.root / "conflict", ["second_focused"])
        states = [json.loads(path.read_text()) for path in preview.directory.glob("*.json")]
        pending = next(state for state in states if state["phase"] == "preparing")
        root = Path(pending["worktree"])
        (root / "task.txt").write_text("resolved")
        git(root, "add", "task.txt")
        git(root, "commit", "-m", "Resolve preview interaction")
        result = preview.resume(pending["id"])
        self.assertEqual(result["phase"], "prepared")
        self.assertEqual(len(result["manifest"]["features"]), 2)
        self.verify(result)
        preview.publish(result["id"])

    def test_example_inventory_never_passes_empty_disabled_or_skipped(self):
        for invalid in ([], [""], [".*"], [["ctest"]]):
            with self.assertRaises(WorkflowError):
                focused_tests(invalid)
        paths = [f"examples/{directory}/main.gti" for directory in NESTED]
        validate_sources(paths)
        with self.assertRaises(ValueError):
            validate_sources(paths + ["examples/new-project/main.gti"])
        with self.assertRaises(ValueError):
            validate_inventory({"tests": []}, {"example"})
        with self.assertRaises(ValueError):
            validate_inventory({"tests": [{"name": "example", "properties": [
                {"name": "DISABLED", "value": True}]}]}, {"example"})
        result = self.root / "results.xml"
        result.write_text('<testsuite><testcase name="example"><skipped/></testcase></testsuite>')
        with self.assertRaises(ValueError):
            validate_results(result, {"example"})
        self.assertIn("objective_cpp_bridge", required_tests("darwin"))
        self.assertNotIn("objective_cpp_bridge", required_tests("linux"))

    def test_preview_installer_pins_source_and_preserves_dirty_primary(self):
        self.feature()
        candidate = self.prepare()
        self.verify(candidate)
        Preview(self.flow).publish(candidate["id"])
        dirty = self.primary / "base.txt"
        dirty.write_text("user changes")
        primary_head = git(self.primary, "rev-parse", "HEAD")
        git(self.primary, "remote", "set-url", "origin", "https://example.invalid/lookalike/gti.git")
        with patch.object(installer, "CANONICAL", str(self.remote)):
            root, head = installer.preview_source(self.primary)
            self.assertEqual(head, candidate["head"])
            self.assertEqual(git(root, "rev-parse", "HEAD"), head)
            self.assertEqual(installer.preview_source(self.primary), (root, head))
            self.assertEqual(git(self.primary, "rev-parse", "HEAD"), primary_head)
            self.assertEqual(dirty.read_text(), "user changes")
            (root / "task.txt").write_text("preview edits")
            with self.assertRaises(ValueError):
                installer.preview_source(self.primary)
            self.assertEqual((root / "task.txt").read_text(), "preview edits")
            git(root, "add", "task.txt")
            git(root, "commit", "-m", "Unique local preview work")
            unique = git(root, "rev-parse", "HEAD")
            with self.assertRaises(ValueError):
                installer.preview_source(self.primary)
            self.assertEqual(git(root, "rev-parse", "HEAD"), unique)

    def test_preview_dry_run_only_resolves_and_missing_preview_fails(self):
        with patch.object(installer, "CANONICAL", str(self.remote)):
            with self.assertRaises(subprocess.CalledProcessError):
                installer.preview_source(self.primary, dry_run=True)
        self.feature()
        candidate = self.prepare()
        self.verify(candidate)
        Preview(self.flow).publish(candidate["id"])
        with patch.object(installer, "CANONICAL", str(self.remote)):
            root, head = installer.preview_source(self.primary, dry_run=True)
        self.assertIsNone(root)
        self.assertEqual(head, candidate["head"])
        self.assertFalse((self.root / "gti-preview-installs").exists())

    def test_preview_activate_only_rejected_before_source_or_install(self):
        with patch.object(sys, "argv", ["installer", "--preview", "--activate-only"]), patch.object(
                installer.platform, "system", return_value="Darwin"), patch.object(
                installer, "preview_source") as source, patch.object(installer, "finish_activation") as activate:
            with self.assertRaises(SystemExit):
                installer.main()
            source.assert_not_called()
            activate.assert_not_called()

    def test_preview_build_failure_never_activates(self):
        source = self.root / "selected-source"
        source.mkdir()
        (source / "VERSION").write_text("1.2.3\n")
        executed = []

        def run(argv, **kwargs):
            executed.append([str(arg) for arg in argv])
            if "--build" in argv:
                raise subprocess.CalledProcessError(1, argv)
            return subprocess.CompletedProcess(argv, 0, json.dumps(str(self.root / "nvim")), "")

        with patch.object(sys, "argv", ["installer", "--preview", "--no-terminal"]), patch.object(
                installer, "ROOT", self.primary), patch.object(installer.platform, "system", return_value="Darwin"), patch.object(
                installer, "preview_source", return_value=(source, "a" * 40)), patch.object(
                installer.shutil, "which", return_value="/usr/bin/tool"), patch.object(
                installer, "discover_llvm", return_value=self.root / "llvm"), patch.object(
                installer, "run", side_effect=run), patch.object(installer, "finish_activation") as activate:
            with self.assertRaises(subprocess.CalledProcessError):
                installer.main()
            activate.assert_not_called()
        self.assertFalse(any("--install" in argv for argv in executed))
        self.assertFalse(any(str(source / "scripts/install_neovim_local.lua") in argv and "probe" not in argv
                             for argv in executed))


if __name__ == "__main__":
    unittest.main()
