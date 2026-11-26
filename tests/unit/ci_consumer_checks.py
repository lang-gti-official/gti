"""CI consumers preserve package context and configured subprocess deadlines."""
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests"))


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


sweep = load("sweep", ROOT / "scripts/debug/stdlib_include_sweep.py")
runtime = load("runtime", ROOT / "tests/macos_installed_runtime_test.py")


class Consumers(unittest.TestCase):
    def test_package_import_has_real_manifest(self):
        def check(command, **kwargs):
            self.assertEqual(command[1:], ["check", "--offline"])
            root = kwargs["cwd"]
            self.assertIn('version = "1.0.0"', (root / "gti.toml").read_text())
            self.assertIn("import std.package;", (root / "main.gti").read_text())
            return mock.Mock(returncode=0)
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(sweep.subprocess, "run", side_effect=check):
            self.assertEqual(sweep.classify(Path("/gti"), Path(directory),
                                           "package", "std.package"), ("ok", ""))

    def test_runtime_honors_configured_scale(self):
        with mock.patch.dict(os.environ, {"GTI_TEST_TIMEOUT_SCALE": "3"}), \
             mock.patch.object(runtime.subprocess, "run",
                               return_value=mock.Mock(returncode=0, stdout="", stderr="")) as run:
            # The child environment intentionally has no GTI_ overrides.
            runtime.run(["cmake", "--install", "build"], {})
            self.assertEqual(run.call_args.kwargs["timeout"], 540)
            runtime.run_program("program", "", {}, Path.cwd())
            self.assertEqual(run.call_args.kwargs["timeout"], 90)


if __name__ == "__main__":
    unittest.main()
