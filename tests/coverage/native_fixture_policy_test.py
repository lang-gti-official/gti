#!/usr/bin/env python3
"""Coverage C fixtures stay contained and cannot masquerade as GTI findings."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("coverage_gate", Path(__file__).with_name("gate.py"))
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class NativeFixturePolicy(unittest.TestCase):
    def test_default_keeps_existing_command_path(self):
        with patch.object(gate.subprocess, "run") as called:
            self.assertEqual(gate.native_objects(Path("unused"), {}), [])
            called.assert_not_called()
        self.assertIsNotNone(gate.check("fixture", {"finding": "X", "outcome": "test-error"},
                                        "test-error", "C compilation failed"))

    def test_paths_and_compiler_failures(self):
        with tempfile.TemporaryDirectory(prefix="gti-native-policy-") as directory:
            root = Path(directory)
            (root / "native").mkdir()
            (root / "native/fixture.c").write_text("int fixture(void) { return 0; }")
            (root / "native/link.c").symlink_to(root / "native/fixture.c")
            with patch.object(gate, "ROOT", root):
                for entry in ("../fixture.c", "/tmp/fixture.c", "fixture.c", "native/../fixture.c",
                              "native/link.c", "native/missing.c", "native/fixture.cpp"):
                    with self.subTest(entry=entry), self.assertRaises(gate.CoverageInfrastructureError):
                        gate.native_objects(root / "out", {"native_sources": [entry]})
                with self.assertRaises(gate.CoverageInfrastructureError):
                    gate.native_objects(root / "out", {"native_sources": "native/fixture.c"})
                failure = subprocess.CompletedProcess([], 1, "", "C source error")
                with patch.object(gate.subprocess, "run", return_value=failure):
                    with self.assertRaisesRegex(gate.CoverageInfrastructureError, "C source error"):
                        gate.native_objects(root / "out", {"native_sources": ["native/fixture.c"]})
                with patch.object(gate.subprocess, "run", side_effect=OSError("missing C compiler")):
                    with self.assertRaisesRegex(gate.CoverageInfrastructureError, "missing C compiler"):
                        gate.native_objects(root / "out", {"native_sources": ["native/fixture.c"]})
                commands = []

                def compile_fixture(command, **kwargs):
                    commands.append(command)
                    self.assertEqual(kwargs.get("shell", False), False)
                    Path(command[-1]).write_bytes(b"test object")
                    return subprocess.CompletedProcess(command, 0, "", "")

                with patch.object(gate.subprocess, "run", side_effect=compile_fixture):
                    objects = gate.native_objects(root / "out", {"native_sources": ["native/fixture.c"]})
                self.assertEqual(objects, [str(root / "out.native-0.o")])
                self.assertEqual(commands[0][1:], ["-std=c17", "-c", str(root / "native/fixture.c"),
                                                 "-o", objects[0]])


if __name__ == "__main__":
    unittest.main()
