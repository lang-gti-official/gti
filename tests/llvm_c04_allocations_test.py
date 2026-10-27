#!/usr/bin/env python3
"""Mutation controls for bounded C04 allocation observations."""

import unittest
import tempfile
from pathlib import Path

from llvm_c04_allocations import validate_counts, verify_relink_sources
from llvm_c04_mir_evaluation_test import digest


class AllocationControls(unittest.TestCase):
    def test_relink_preserves_validated_driver(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sources = {}
            for name in ("driver.cpp", "carrier.h", "worker_driver.h"):
                path = root / "tests/fixtures/llvm_c04_mir" / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(name)
                sources[str(path.relative_to(root))] = digest(path)
            evidence = {"profile": "scoped-workers", "source_sha256": sources}
            verify_relink_sources(evidence, root)
            for key in list(sources):
                saved = sources.pop(key)
                with self.assertRaisesRegex(ValueError, "lacks validated"):
                    verify_relink_sources(evidence, root)
                sources[key] = saved
                sources[key] = "forged"
                with self.assertRaisesRegex(ValueError, "differs"):
                    verify_relink_sources(evidence, root)
                sources[key] = saved

    def test_worker_runtime_cost_is_not_zero(self):
        for mode in ("status", "unwind"):
            for outcome, failures in (("success", 0), ("failure", 64), ("mixed", 32)):
                count = failures * (5 if mode == "unwind" else 2)
                valid = {"controls": True, "new_calls": 128, "new_bytes": 1024,
                         "delete_calls": 128, "exception_calls": count,
                         "exception_bytes": count, "runtime_new_calls": 128,
                         "runtime_new_bytes": 1024, "runtime_delete_calls": 128,
                         "runtime_exception_calls": 0, "runtime_exception_bytes": 0}
                validate_counts(valid, mode, outcome, 64, "scoped_worker")
                for key in valid:
                    altered = dict(valid)
                    altered[key] = False if key == "controls" else valid[key] + 1
                    if key in ("new_bytes", "exception_bytes"):
                        altered[key] = -1
                    with self.subTest(mode=mode, outcome=outcome, key=key):
                        with self.assertRaises(ValueError):
                            validate_counts(altered, mode, outcome, 64, "scoped_worker")

    def test_exact_counts(self):
        for mode in ("status", "unwind"):
            for outcome, failures in (("success", 0), ("failure", 64), ("mixed", 32)):
                count = failures if mode == "unwind" else 0
                valid = {"controls": True, "new_calls": 0, "new_bytes": 0,
                         "delete_calls": 0, "exception_calls": count,
                         "exception_bytes": count}
                validate_counts(valid, mode, outcome, 64)
                for key in valid:
                    altered = dict(valid)
                    altered[key] = False if key == "controls" else valid[key] + 1
                    if key == "exception_bytes" and count:
                        altered[key] = count - 1
                    with self.subTest(mode=mode, outcome=outcome, key=key):
                        with self.assertRaises(ValueError):
                            validate_counts(altered, mode, outcome, 64)


if __name__ == "__main__":
    unittest.main()
