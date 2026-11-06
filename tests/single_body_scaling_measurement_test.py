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


# Linear MIR-only growth (4x), for cases that model only the native profile.
LINEAR_MIR = {"base": [1], "small": [2], "large": [5]}


class ScalingMeasurement(unittest.TestCase):
    def check_model(self, samples, mir_samples=LINEAR_MIR):
        profiles = {False: samples, True: mir_samples}
        calls = {(mir, name): 0 for mir in profiles for name in profiles[mir]}
        def measure(compiler, source, timeout, flags=None, *, mir=False):
            name = source.stem
            index = calls[mir, name]
            calls[mir, name] += 1
            values = profiles[mir][name]
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

    def test_mir_profile_sixfold_boundary(self):
        native = {"base": [5], "small": [6], "large": [9]}
        self.assertEqual(self.check_model(native, {"base": [1], "small": [2], "large": [7]}), 0)
        self.assertEqual(self.check_model(native, {"base": [1], "small": [2], "large": [7.01]}), 1)

    def test_mir_regression_fails_when_native_passes(self):
        self.assertEqual(self.check_model({"base": [5], "small": [6], "large": [9]},
                                          {"base": [1], "small": [2], "large": [17]}), 1)


if __name__ == "__main__":
    unittest.main()
