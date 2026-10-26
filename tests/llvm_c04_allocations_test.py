#!/usr/bin/env python3
"""Mutation controls for bounded C04 allocation observations."""

import unittest

from llvm_c04_allocations import validate_counts


class AllocationControls(unittest.TestCase):
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
