#!/usr/bin/env python3
"""Installed provider evidence must not silently use a source/build checkout."""

import pathlib
import subprocess
import tempfile
import unittest
from unittest import mock

import process_resource_policy_test as policy

from process_resource_policy_test import installed_resources


class InstalledResourcesTests(unittest.TestCase):
    def test_storage_oracle_requires_the_selected_backend_contract(self):
        class CapturedStorageCompile(Exception):
            pass

        for backend, expected in (("llvm", "1"), ("cpp", "0")):
            def capture(command, environment, expected_status=0):
                if len(command) > 1 and command[1].endswith("/process_storage_policy.gti"):
                    self.assertIn(f"-DGTI_TEST_PROCESS_OWNERS={expected}", command)
                    raise CapturedStorageCompile
                return subprocess.CompletedProcess(command, 0, "", "")

            with self.subTest(backend=backend), \
                    mock.patch("sys.argv", ["process_resource_policy_test.py", "/selected/build/gti"]), \
                    mock.patch.dict("os.environ", {"GTI_TEST_BACKENDS": backend}), \
                    mock.patch.object(policy, "run", side_effect=capture):
                with self.assertRaises(CapturedStorageCompile):
                    policy.main()

    def test_build_tree_keeps_explicit_runtime_archive(self):
        class CapturedCompile(Exception):
            pass

        def capture(command, environment, expected_status=0):
            self.assertEqual(environment["GTI_RUNTIME_LIBRARY"], "/selected/build/libgti_runtime.a")
            self.assertEqual(environment["GTI_STDLIB_PATH"],
                             str(pathlib.Path(policy.__file__).resolve().parent.parent / "stdlib"))
            self.assertIn("process_resource_policy.gti", command[1])
            raise CapturedCompile

        with mock.patch("sys.argv", ["process_resource_policy_test.py", "/selected/build/gti"]), \
                mock.patch.dict("os.environ", {"GTI_RUNTIME_LIBRARY": "/selected/build/libgti_runtime.a"}), \
                mock.patch.object(policy, "run", side_effect=capture):
            with self.assertRaises(CapturedCompile):
                policy.main()

    def test_exact_stage_and_missing_resources(self):
        for library in ("libgti_runtime.a", "gti_runtime.lib"):
            with self.subTest(library=library), tempfile.TemporaryDirectory() as directory:
                prefix = pathlib.Path(directory).resolve()
                required = ["bin/gti", f"lib/{library}",
                            "share/gti/stdlib/prelude.gti",
                            "share/gti/stdlib/std/memory.gti",
                            "include/gti/runtime.hpp", "include/gti/memory.h"]
                for name in required:
                    path = prefix / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.touch()
                resources = installed_resources(prefix / "bin/gti")
                self.assertEqual(resources, {
                    "GTI_STDLIB_PATH": str(prefix / "share/gti/stdlib"),
                    "GTI_RUNTIME_INCLUDE": str(prefix / "include"),
                    "GTI_RUNTIME_LIBRARY": str(prefix / f"lib/{library}"),
                })
                for name in required[1:]:
                    path = prefix / name
                    path.unlink()
                    with self.subTest(missing=name), self.assertRaises(RuntimeError):
                        installed_resources(prefix / "bin/gti")
                    path.touch()
                other = "gti_runtime.lib" if library.endswith(".a") else "libgti_runtime.a"
                (prefix / "lib" / other).touch()
                with self.assertRaisesRegex(RuntimeError, "expected one installed runtime"):
                    installed_resources(prefix / "bin/gti")


if __name__ == "__main__":
    unittest.main()
