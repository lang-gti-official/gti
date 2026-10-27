#!/usr/bin/env python3
"""Automation-only controls for the optional C04 cost reporter."""

import tempfile
import json
import platform
import sys
import unittest
from pathlib import Path

from llvm_c04_cost import checksum, digest, observation, sections, verify_artifacts
from llvm_c04_mir_evaluation_test import run


class CostTests(unittest.TestCase):
    @unittest.skipUnless(platform.system() in ("Darwin", "Linux"), "system time metrics")
    def test_build_metrics_include_process_memory(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            result = run([sys.executable, "-c", "print(7)"], root, "measure",
                         measure_build=True)
            self.assertEqual(result.stdout, "7\n")
            record = json.loads((root / "measure.json").read_text())
            self.assertGreater(record["seconds"], 0)
            self.assertGreater(record["peak_rss_kib"], 0)
            self.assertEqual(record["returncode"], 0)

    def test_checksum_crosses_input_wrap(self):
        for count in (2, 63, 1024, 1025, 2051):
            for outcome in ("success", "failure", "mixed"):
                expected = sum(1 if outcome == "failure" or
                               (outcome == "mixed" and i % 2) else i % 1024 + 1
                               for i in range(count))
                self.assertEqual(checksum(outcome, count), expected)

    def test_timing_requires_result_oracle(self):
        self.assertEqual(observation("2 100 3\n", "success", 2), 100)
        for text in ("2 0 3\n", "3 100 3\n", "2 100 4\n", "2 100 3\njunk"):
            with self.assertRaises(ValueError):
                observation(text, "success", 2)

    def test_section_formats(self):
        self.assertEqual(sections("section size addr\n.text 17 0\n.eh_frame 48 0x20\nTotal 65\n"),
                         {".text": 17, ".eh_frame": 48})
        self.assertEqual(sections("__text 17 0\n__compact_unwind 32 17\n"),
                         {"__text": 17, "__compact_unwind": 32})
        self.assertEqual(sections("__const 1 0\n__const 2 8\n"), {"__const": 3})
        with self.assertRaises(ValueError):
            sections("no sections")

    def test_validation_is_bound_to_exact_artifacts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            evidence = {"sanitized_native_carrier": False, "receipts": []}
            for mode in ("status", "unwind"):
                case = root / f"scalar-{mode}-O3"
                case.mkdir()
                row = {"fixture": "scalar", "mode": mode, "level": "O3"}
                for filename, key in (("generated.lowered", "input_sha256"),
                                      ("generated.o", "object_sha256"),
                                      ("probe", "executable_sha256")):
                    path = case / filename
                    path.write_bytes(filename.encode())
                    row[key] = digest(path)
                evidence["receipts"].append(row)
            self.assertEqual(len(verify_artifacts(evidence, root, ("scalar",))), 2)
            evidence["profile"] = "native-boundary"
            with self.assertRaisesRegex(ValueError, "exact evaluation profile"):
                verify_artifacts(evidence, root, ("scalar",))
            evidence.pop("profile")
            evidence["receipts"][0]["profile"] = "native-boundary"
            with self.assertRaisesRegex(ValueError, "exact evaluation profile"):
                verify_artifacts(evidence, root, ("scalar",))
            evidence["receipts"][0].pop("profile")
            evidence["call_boundaries"] = "optimizable"
            with self.assertRaisesRegex(ValueError, "different call-boundary"):
                verify_artifacts(evidence, root, ("scalar",))
            for row in evidence["receipts"]:
                row["call_boundaries"] = "optimizable"
                path = root / f"scalar-{row['mode']}-O3/generated.codegen.ll"
                path.write_text("optimized IR")
                row["codegen_ir_sha256"] = digest(path)
            self.assertEqual(len(verify_artifacts(evidence, root, ("scalar",))), 2)
            path.write_text("changed IR")
            with self.assertRaisesRegex(ValueError, "optimized IR changed"):
                verify_artifacts(evidence, root, ("scalar",))
            path.write_text("optimized IR")
            evidence["call_boundaries"] = "unknown"
            with self.assertRaisesRegex(ValueError, "unknown call-boundary"):
                verify_artifacts(evidence, root, ("scalar",))
            evidence["call_boundaries"] = "optimizable"
            evidence["sanitized_native_carrier"] = True
            with self.assertRaisesRegex(ValueError, "unsanitized"):
                verify_artifacts(evidence, root, ("scalar",))
            evidence["sanitized_native_carrier"] = False
            evidence["receipts"].append(evidence["receipts"][0])
            with self.assertRaisesRegex(ValueError, "duplicate"):
                verify_artifacts(evidence, root, ("scalar",))
            evidence["receipts"].pop()
            path = root / "scalar-unwind-O3/generated.lowered"
            path.write_bytes(b"different MIR")
            with self.assertRaisesRegex(ValueError, "artifact changed"):
                verify_artifacts(evidence, root, ("scalar",))
            evidence["receipts"][1]["input_sha256"] = digest(path)
            with self.assertRaisesRegex(ValueError, "different MIR"):
                verify_artifacts(evidence, root, ("scalar",))

    def test_native_profile_cannot_be_omitted(self):
        for fixture, profile in (("native_boundary", "native-boundary"),
                                 ("dynamic_pointer", "dynamic-calls"),
                                 ("scoped_worker", "scoped-workers")):
            with self.subTest(fixture=fixture):
                self.check_explicit_profile(fixture, profile)

    def check_explicit_profile(self, fixture, profile):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            evidence = {"sanitized_native_carrier": False, "receipts": []}
            for mode in ("status", "unwind"):
                case = root / f"{fixture}-{mode}-O3"
                case.mkdir()
                row = {"fixture": fixture, "mode": mode, "level": "O3"}
                for filename, key in (("generated.lowered", "input_sha256"),
                                      ("generated.o", "object_sha256"),
                                      ("probe", "executable_sha256")):
                    path = case / filename
                    path.write_text(filename)
                    row[key] = digest(path)
                evidence["receipts"].append(row)
            with self.assertRaisesRegex(ValueError, "exact evaluation profile"):
                verify_artifacts(evidence, root, (fixture,))
            evidence["profile"] = profile
            with self.assertRaisesRegex(ValueError, "exact evaluation profile"):
                verify_artifacts(evidence, root, (fixture,))
            for row in evidence["receipts"]:
                row["profile"] = profile
            self.assertEqual(len(verify_artifacts(evidence, root, (fixture,))), 2)
            evidence["profile"] = "wrong-profile"
            with self.assertRaisesRegex(ValueError, "exact evaluation profile"):
                verify_artifacts(evidence, root, (fixture,))


if __name__ == "__main__":
    unittest.main()
