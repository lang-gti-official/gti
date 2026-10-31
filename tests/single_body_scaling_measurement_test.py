#!/usr/bin/env python3
"""Deterministic timing inputs for the single-body scaling acceptance rule."""
import contextlib
import importlib.util
import io
from pathlib import Path
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location(
    "scaling", Path(__file__).with_name("single_body_compile_scaling_test.py"))
scaling = importlib.util.module_from_spec(spec)
spec.loader.exec_module(scaling)


class ScalingMeasurement(unittest.TestCase):
    def check_model(self, samples):
        calls = {name: 0 for name in samples}
        def measure(compiler, source, timeout):
            name = source.stem
            index = calls[name]
            calls[name] += 1
            values = samples[name]
            return values[min(index, len(values) - 1)]
        with mock.patch.object(scaling, "compile_seconds", side_effect=measure), \
             mock.patch.object(scaling.sys, "argv", ["scaling", "compiler"]), \
             contextlib.redirect_stdout(io.StringIO()), \
             contextlib.redirect_stderr(io.StringIO()):
            return scaling.main()

    def test_transient_baseline_cost_does_not_fake_superlinear_growth(self):
        self.assertEqual(self.check_model({"base": [5.28, 5.30, 4.20],
                                          "small": [5.58], "large": [10.19]}), 0)

    def test_linear_body_with_fixed_import_cost_passes(self):
        self.assertEqual(self.check_model({"base": [5], "small": [6], "large": [9]}), 0)

    def test_quadratic_body_fails(self):
        self.assertEqual(self.check_model({"base": [5], "small": [6], "large": [21]}), 1)

    def test_cubic_body_fails(self):
        self.assertEqual(self.check_model({"base": [5], "small": [6], "large": [69]}), 1)

    def test_original_eightfold_boundary_is_preserved(self):
        self.assertEqual(self.check_model({"base": [5], "small": [6], "large": [13]}), 0)
        self.assertEqual(self.check_model({"base": [5], "small": [6], "large": [13.01]}), 1)

    def test_persistent_excess_growth_is_not_discarded(self):
        self.assertEqual(self.check_model({"base": [5.28], "small": [5.58],
                                          "large": [10.19]}), 1)


if __name__ == "__main__":
    unittest.main()
