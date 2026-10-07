#!/usr/bin/env python3
"""Fail-closed worker/census tests and exact serial/parallel runtime parity."""

import contextlib
import io
import os
import pathlib
import subprocess
import sys
import threading
import time
import unittest
from unittest.mock import patch

import case_jobs
import lambda_pack_invocation_runtime_test as matrix
import stdlib_surface_test as surface


class CaseJobsTests(unittest.TestCase):
    def run_cases(self, *args, **kwargs):
        with contextlib.redirect_stdout(io.StringIO()):
            return case_jobs.run_cases(*args, **kwargs)

    def test_invalid_and_empty_plans(self):
        for value in (0, -1, "2.0", " 2", True, "1\n", "+1"):
            with self.assertRaises(ValueError):
                case_jobs.positive_jobs(value)
        for cases in ([], [("", 1)], [("x", 1), ("x", 2)]):
            with self.assertRaises(ValueError):
                self.run_cases(cases, lambda value: value, 2)
        with patch.dict(os.environ, {"GTI_TEST_CASE_JOBS": "bad"}):
            with self.assertRaises(ValueError):
                case_jobs.configured_jobs()

    def test_bounded_workers_and_ordered_observations(self):
        lock = threading.Lock()
        barrier = threading.Barrier(2)
        active = peak = 0
        caller = threading.get_ident()
        observed = []

        def work(value):
            nonlocal active, peak
            with lock:
                active += 1
                peak = max(peak, active)
            barrier.wait(timeout=5)
            if value % 2 == 0:
                time.sleep(0.01)
            with lock:
                active -= 1
            return value

        def observe(value):
            self.assertEqual(threading.get_ident(), caller)
            observed.append(value)

        results = self.run_cases([(str(i), i) for i in range(6)], work, 2, observe)
        self.assertEqual(peak, 2)
        self.assertEqual(observed, list(range(6)))
        self.assertEqual(results, [(str(i), i) for i in range(6)])

    def test_failures_cannot_be_lost(self):
        visited = []

        def work(value):
            if value == 1:
                raise subprocess.TimeoutExpired("fake-compiler", 120)
            return value

        def observe(value):
            visited.append(value)
            if value == 2:
                raise AssertionError("wrong native observation")

        with self.assertRaises(RuntimeError) as failure:
            self.run_cases([(str(i), i) for i in range(4)], work, 2, observe)
        self.assertIn("1: TimeoutExpired", str(failure.exception))
        self.assertIn("2: AssertionError", str(failure.exception))
        self.assertEqual(visited, [0, 2, 3])

    def test_missing_and_duplicate_completions_fail(self):
        for repeat in (False, True):
            def corrupted(futures):
                values = list(futures)
                return iter([values[0], *values] if repeat else values[1:])

            with patch.object(case_jobs, "as_completed", corrupted):
                with self.assertRaisesRegex(RuntimeError, "case census"):
                    self.run_cases([("a", 1), ("b", 2)], lambda value: value, 2)

    def test_serial_preserves_work_observation_interleaving(self):
        visits = []
        self.run_cases([("a", 0), ("b", 1)],
                       lambda value: visits.append(("work", value)), 1,
                       lambda value: visits.append(("observe", value)))
        self.assertEqual(visits, [("work", 0), ("observe", None),
                                  ("work", 1), ("observe", None)])

    def test_compile_and_execution_share_the_worker_budget(self):
        lock = threading.Lock()
        active = peak = 0

        def busy(value):
            nonlocal active, peak
            with lock:
                active += 1
                peak = max(peak, active)
            time.sleep(0.005)
            with lock:
                active -= 1
            return value

        self.run_cases([(str(i), i) for i in range(20)], busy, 2, busy)
        self.assertLessEqual(peak, 2)

    def matrix_calls(self, jobs, corrupt=None):
        builds, executions = [], []
        lock = threading.Lock()

        def process(command, **kwargs):
            if "-o" in command:
                binary = pathlib.Path(command[command.index("-o") + 1])
                normalized = [pathlib.Path(arg).name if arg == str(binary) else arg
                              for arg in command]
                with lock:
                    builds.append((tuple(normalized), kwargs["env"]["ASAN_OPTIONS"],
                                   kwargs["env"]["UBSAN_OPTIONS"], kwargs["timeout"]))
                return subprocess.CompletedProcess(command, 1 if corrupt == "build" else 0,
                                                   "", "compile rejection")
            binary = pathlib.Path(command[0]).name
            executions.append((binary, tuple(command[1:]), kwargs.get("input"),
                               kwargs["timeout"], threading.get_ident()))
            return subprocess.CompletedProcess(command, 0, "bad" if corrupt == "run" else "ok", "")

        arguments = ["matrix", "fake-gti", "--jobs", str(jobs)]
        cases = {name: [((), (0, "ok", "")), (("fail",), (0, "ok", ""))]
                 for name in ("fixture", "other")}
        with patch.object(sys, "argv", arguments), patch.object(subprocess, "run", process), \
                patch.dict(os.environ, {"GTI_TEST_CASE_JOBS": "1"}), \
                contextlib.redirect_stdout(io.StringIO()):
            matrix.main(names=("fixture", "other"), cases=cases, execution_profile="concurrent",
                        stdin_by_name={"fixture": "bytes", "other": "bytes"})
        return sorted(builds), executions

    def test_exact_matrix_and_native_observations_preserved(self):
        serial, serial_runs = self.matrix_calls(1)
        parallel, parallel_runs = self.matrix_calls(2)
        self.assertEqual(serial, parallel)
        self.assertEqual(serial_runs, parallel_runs)
        self.assertEqual(len(serial), 16)
        self.assertEqual(len(serial_runs), 32)
        self.assertTrue(any("-fno-elide-constructors" in row[0] for row in serial))
        self.assertTrue(any("-fsanitize=address,undefined" in row[0] for row in serial))
        self.assertTrue(all(row[2] == "bytes" for row in serial_runs))
        self.assertTrue(all(row[-1] == threading.get_ident() for row in serial_runs))

    def test_matrix_compilation_and_wrong_output_fail(self):
        for corrupt in ("build", "run"):
            with self.assertRaises(RuntimeError):
                self.matrix_calls(2, corrupt)

    def surface_cases(self, jobs, corrupt=False):
        repository = pathlib.Path(__file__).resolve().parent.parent
        manifest = surface.load_manifest(repository / "tests/stdlib_surface_manifest.json")
        records = []
        lock = threading.Lock()

        def execute(case, *args):
            with lock:
                records.append(case)
            if corrupt and case["id"] == manifest["cases"][-1]["id"]:
                raise AssertionError("late surface failure")

        arguments = ["surface", "fake-gti", "clang++", "runtime.a", str(repository),
                     "--jobs", str(jobs)]
        with patch.object(sys, "argv", arguments), patch.object(surface, "run_case", execute), \
                patch.dict(os.environ, {"GTI_TEST_CASE_JOBS": "1"}), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(surface.main(), 0)
        return sorted(records, key=lambda case: case["id"])

    def test_surface_full_manifest_preserved(self):
        self.assertEqual(self.surface_cases(1), self.surface_cases(2))
        with self.assertRaises(RuntimeError):
            self.surface_cases(2, corrupt=True)


if __name__ == "__main__":
    unittest.main()
