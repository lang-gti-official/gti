"""Native test providers preserve link order, mode and failure evidence."""

from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

from native_fixture_build import build_fixture


class NativeFixtureBuildTests(unittest.TestCase):
    def test_ordinary_direct_build_is_unchanged(self):
        result = subprocess.CompletedProcess([], 0, "built", "")
        with patch("native_fixture_build.subprocess.run", return_value=result) as run:
            self.assertIs(build_fixture(
                ["gti", "main.gti", "--keep-cpp", "-O3"], "program",
                native_arguments=["-fno-elide-constructors"], text=True), result)
        run.assert_called_once_with(
            ["gti", "main.gti", "--keep-cpp", "-O3", "-o", "program",
             "--", "-fno-elide-constructors"], text=True)

    def test_provider_precedes_archive_and_preserves_driver_and_flags(self):
        environment = {"GTI_CXX": "/native/clang++",
                       "GTI_RUNTIME_LIBRARY": "/runtime/libgti_runtime.a",
                       "GTI_RUNTIME_INCLUDE": "/runtime/include",
                       "GTI_VENDOR_INCLUDE": "/compat/include"}
        emitted = subprocess.CompletedProcess([], 0, "emit\n", "warning\n")
        compiled = subprocess.CompletedProcess([], 9, "native\n", "link failure\n")
        with patch("native_fixture_build.subprocess.run",
                   side_effect=[emitted, compiled]) as run:
            result = build_fixture(
                ["gti", "fixture.gti", "--std", "c++20", "-O1"],
                Path("output/program"), native_sources=["provider one.cpp", "two.o"],
                native_arguments=["-fsanitize=address,undefined", "-ffast-math"],
                standard="c++20", optimization="O1", env=environment,
                capture_output=True, text=True, timeout=120)
        self.assertEqual(run.call_args_list[0].args[0],
                         ["gti", "fixture.gti", "--std", "c++20", "-O1",
                          "--emit-cpp", "-o", "output/program.gti.cpp"])
        native = run.call_args_list[1].args[0]
        self.assertEqual(native[0], "/native/clang++")
        self.assertEqual(native[1:3], ["-std=c++20", "-O1"])
        self.assertIn("-I/compat/include", native)
        self.assertLess(native.index("provider one.cpp"), native.index("two.o"))
        self.assertLess(native.index("two.o"), native.index("/runtime/libgti_runtime.a"))
        self.assertLess(native.index("-ffast-math"), native.index("-fno-fast-math"))
        self.assertIn("-D__gti_strict_ieee754=1", native)
        self.assertEqual(run.call_args_list[1].kwargs, run.call_args_list[0].kwargs)
        self.assertEqual((result.returncode, result.stdout, result.stderr),
                         (9, "emit\nnative\n", "warning\nlink failure\n"))

    def test_frontend_failure_never_invokes_native_compiler(self):
        failed = subprocess.CompletedProcess([], 65, "", "source error")
        with patch("native_fixture_build.subprocess.run", return_value=failed) as run:
            self.assertIs(build_fixture(["gti", "invalid.gti"], "program",
                                        native_sources=["provider.cpp"]), failed)
        self.assertEqual(run.call_count, 1)


if __name__ == "__main__":
    unittest.main()
