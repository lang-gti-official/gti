#!/usr/bin/env python3
"""Focused build-resource controls; no GTI compilation or workload timings."""

import importlib.util
from pathlib import Path
import platform
import subprocess
import sys
import unittest


if len(sys.argv) == 3 and sys.argv[1] == "--harness":
    harness_path = Path(sys.argv[2])
    del sys.argv[1:]
else:
    harness_path = Path(__file__).with_name("llvm_runtime_cost_test.py")
spec = importlib.util.spec_from_file_location("llvm_runtime_cost", harness_path)
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)


class BuildResourceControls(unittest.TestCase):
    def test_darwin_units_and_diagnostics(self):
        result = harness.parse_build_resources(
            "compiler diagnostic\n  5242880  maximum resident set size\n  0 page reclaims\n", "Darwin")
        self.assertEqual(result["peak_resident_bytes"], 5242880)
        self.assertEqual(result["reported_unit"], "bytes")
        self.assertIn("not simultaneous process-tree RSS", result["scope"])

    def test_linux_units_and_diagnostics(self):
        result = harness.parse_build_resources(
            "compiler diagnostic\n\tMaximum resident set size (kbytes):\t5120\n", "Linux")
        self.assertEqual(result["peak_resident_bytes"], 5242880)
        self.assertEqual(result["reported_value"], 5120)
        self.assertEqual(result["reported_unit"], "KiB")

    def test_missing_ambiguous_or_invalid_metrics_reject(self):
        for system, valid in (
            ("Darwin", " 10 maximum resident set size\n"),
            ("Linux", "Maximum resident set size (kbytes): 10\n"),
        ):
            for value in ("", valid + valid, valid.replace("10", "0"),
                          valid.replace("10", "-1"), valid.replace("10", "no-value")):
                with self.subTest(system=system, value=value):
                    with self.assertRaisesRegex(ValueError, "one positive peak RSS"):
                        harness.parse_build_resources(value, system)

    def test_unsupported_platform_rejects(self):
        with self.assertRaisesRegex(ValueError, "unsupported build resource platform"):
            harness.parse_build_resources("", "unknown")
        with self.assertRaisesRegex(ValueError, "unsupported build resource platform"):
            harness.build_resource_command(["compiler"], "unknown")

    def test_wrapping_preserves_arguments_without_a_shell(self):
        original = [Path("/a compiler"), "source path.gti", "--flag=$(literal)"]
        for system, flag in (("Darwin", "-l"), ("Linux", "-v")):
            self.assertEqual(harness.build_resource_command(original, system),
                             ["/usr/bin/time", flag, *map(str, original)])

    def test_native_child_resources_are_per_command(self):
        system = platform.system()
        self.assertIn(system, ("Darwin", "Linux"))
        self.assertTrue(Path("/usr/bin/time").is_file())
        peaks = []
        # A large command first catches accidental cumulative RUSAGE_CHILDREN
        # high-water reuse when measuring the smaller command that follows it.
        for size in (64 * 1024 * 1024, 4 * 1024 * 1024):
            source = (f"memory = bytearray({size})\n"
                      "for offset in range(0, len(memory), 4096): memory[offset] = 1\n"
                      "print('resource-control')\n")
            command = harness.build_resource_command([sys.executable, "-c", source], system)
            result = subprocess.run(command, capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, "resource-control\n")
            try:
                peaks.append(harness.parse_build_resources(result.stderr, system)["peak_resident_bytes"])
            except ValueError as error:
                self.fail(f"{error}: {result.stderr}")
        self.assertGreater(peaks[0] - peaks[1], 16 * 1024 * 1024, peaks)


if __name__ == "__main__":
    unittest.main()
