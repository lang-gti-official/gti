#!/usr/bin/env python3
"""Check coverage, model bounds and bug-finder decisions independently of GTI."""

from __future__ import annotations

from dataclasses import replace
import itertools
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

REPOSITORY = Path(sys.argv.pop(1)).resolve()
sys.path.insert(0, str(REPOSITORY / "scripts"))
from debug.feature_interaction_model import (
    FACTORS, V1_FACTORS, SELECTED_TRIPLES, InteractionCase, coverage, covering_array, make_case,
)
from debug import feature_interactions as runner
from debug import capture_compiler_failure as capture
from harness_timeouts import scaled


class ModelTests(unittest.TestCase):
    def test_complete_constrained_coverage(self):
        for seed, strength in ((20261003, 2), (7, 2), (19, 3)):
            rows = covering_array(seed, strength)
            self.assertEqual(rows, covering_array(seed, strength))
            self.assertEqual(len(rows), len(set(rows)))
            self.assertLess(len(rows), 60)
            named = [dict(zip(FACTORS, row)) for row in rows]
            self.assertTrue(all(not row["propagate"] or row["expected"] for row in named))
            groups = list(itertools.combinations(FACTORS, strength))
            if strength == 2:
                groups.extend(SELECTED_TRIPLES)
            # An independent demand census: every on/off tuple compatible with
            # the documented implication must be witnessed, not just all-on pairs.
            for group in groups:
                seen = {tuple(row[name] for name in group) for row in named}
                for values in itertools.product((False, True), repeat=len(group)):
                    required = dict(zip(group, values))
                    impossible = (required.get("propagate") is True
                                  and required.get("expected") is False)
                    if not impossible:
                        self.assertIn(values, seen, (seed, group, values))
            measured = coverage(rows, strength)
            self.assertEqual(measured["missing"], [])
            self.assertEqual(measured["covered"], measured["required"])
            self.assertGreater(len(coverage(rows[:1], strength)["missing"]), 0)

    def test_seeded_serialization_and_transform(self):
        for index, row in enumerate(covering_array(20261003)):
            case = make_case(20261003, index, row)
            self.assertEqual(case.record(), make_case(20261003, index, row).record())
            self.assertEqual(case, InteractionCase.from_record(case.record()))
            self.assertNotEqual(case.emit(), case.emit("renamed"))
            self.assertNotIn("audit_answer;", case.emit("renamed"))
            if case.enabled("expected"):
                self.assertIn("exercise(true)", case.emit())
        case = make_case(1, 0, tuple(True for _ in FACTORS))
        record = case.record()
        record["expected"][1] = "forged oracle"
        with self.assertRaises(ValueError):
            InteractionCase.from_record(record)

    def test_independent_value_effect_and_cleanup_oracles(self):
        all_on = InteractionCase(1, 0, tuple(True for _ in FACTORS), 3, 2, 3)
        self.assertEqual(all_on.oracle(), (0, "2\n80\n2\n80\n2\n80\n2\n80\n6\n100\n101\n102\n"
                                               "5\n4\n3\n90\n18\n90\n53\n53\n8\n2\n4\n2\n2\n1\n1\n", ""))
        plain = replace(all_on, selection=tuple(False for _ in FACTORS))
        self.assertEqual(plain.oracle(), (0, "3\n0\n1\n0\n", ""))
        row = tuple(name in {"expected", "propagate", "cleanup"} for name in FACTORS)
        scalar_error = replace(all_on, selection=row)
        self.assertEqual(scalar_error.oracle(), (0, "3\n90\n3\n90\n53\n0\n2\n0\n", ""))
        self.assertIn("read_payload(finish(make_payload(", all_on.emit())
        self.assertIn("[owned = std::move(audit_capture)]", all_on.emit())
        self.assertIn("reserve(std::size_t(1))", all_on.emit())
        self.assertIn("continue;", all_on.emit())
        self.assertIn("break;", all_on.emit())
        self.assertIn("Ticket audit_route_guard", all_on.emit())

    def test_v1_cases_keep_exact_source_hashes_and_oracle(self):
        # This record was produced by the ten-factor generator, before any
        # control-flow source existed. Do not silently invalidate retained bugs.
        record = {
            "schema": "gti-feature-interaction-case-v1", "seed": 1, "index": 0,
            "factors": dict.fromkeys(V1_FACTORS, True), "base": 3, "delta": 2, "width": 3,
            "expected": [0, "2\n2\n2\n2\n6\n5\n4\n3\n90\n18\n90\n53\n53\n8\n2\n4\n", ""],
            "source_sha256": {
                "original": "24b48f552da3ba6a6054d281592853c07eea21e4ed4f849245f323374b812596",
                "renamed": "219148caa8fdf972f0fb92441fec00ec6674fe8b0ee46a36ece16e329cf570c7",
            },
        }
        restored = InteractionCase.from_record(record)
        self.assertFalse(restored.enabled("control_flow"))
        self.assertEqual(restored.record()["source_sha256"], record["source_sha256"])
        self.assertEqual(list(restored.oracle()), record["expected"])
        upgraded = restored.record()
        self.assertEqual(upgraded["schema"], "gti-feature-interaction-case-v2")
        self.assertEqual(restored, InteractionCase.from_record(upgraded))
        record["factors"]["control_flow"] = True
        with self.assertRaises(ValueError):
            InteractionCase.from_record(record)

    def test_bounds_and_prerequisites(self):
        case = make_case(1, 0, tuple(True for _ in FACTORS))
        for changes in ({"base": 0}, {"delta": 6}, {"width": 1}, {"selection": ()}):
            with self.assertRaises(ValueError):
                replace(case, **changes)
        row = list(case.selection)
        row[FACTORS.index("expected")] = False
        with self.assertRaises(ValueError):
            replace(case, selection=tuple(row))
        with self.assertRaises(ValueError):
            covering_array(1, 4)
        with self.assertRaises(ValueError):
            case.emit("unknown")


class DecisionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gti-interaction-decisions-")
        self.root = Path(self.temporary.name)
        self.case = InteractionCase(1, 0, tuple(True for _ in FACTORS), 7, 5, 4)

    def tearDown(self):
        self.temporary.cleanup()

    def fake_commands(self, snapshots, build_status=0, build_stderr="", artifact=True):
        seen = iter(snapshots)
        def run(command, timeout):
            if "-o" in command:
                if artifact:
                    Path(command[command.index("-o") + 1]).touch()
                return subprocess.CompletedProcess(command, build_status, "", build_stderr)
            status, stdout, stderr = next(seen)
            return subprocess.CompletedProcess(command, status, stdout, stderr)
        return run

    def probe(self, name="probe"):
        return runner.probe(self.case, "gti", "c++", self.root / name,
                            variant="original", configuration=("O0", "c++23"), timeout=scaled(1))

    def test_success_and_retained_oracle_disagreement(self):
        with patch.object(runner, "run_process", self.fake_commands([self.case.oracle()] * 2)):
            self.assertIsNone(self.probe("good"))
        with patch.object(runner, "run_process", self.fake_commands([(0, "wrong\n", "")] * 2)):
            failure = self.probe("bad")
        self.assertEqual(failure["signature"], {"kind": "oracle-mismatch", "stage": "run", "channels": ["stdout"]})
        parsed, retained = runner.read_failure(self.root / "bad/failure.json")
        self.assertEqual(parsed, self.case)
        self.assertEqual(retained, failure)
        self.assertEqual(len(failure["observations"]), 3)
        self.assertIn("--keep-cpp", failure["observations"][0]["command"])
        self.assertIn("--verbose", failure["observations"][0]["command"])
        self.assertTrue((self.root / "bad/002-run.stdout").is_file())

    def test_nondeterminism_and_missing_artifact(self):
        with patch.object(runner, "run_process", self.fake_commands([self.case.oracle(), (7, "", "")])):
            self.assertEqual(self.probe()["signature"]["kind"], "nondeterministic")
        with patch.object(runner, "run_process", self.fake_commands([], artifact=False)):
            self.assertEqual(self.probe("missing")["signature"]["kind"], "missing-artifact")

    def test_compile_failure_classification(self):
        for status, stderr, kind in (
            (65, "error[GTI-S2003]: invalid generated binding", "rejected-valid"),
            (65, "error[GTI-B0001]: Internal compiler error: body 7", "internal"),
            (-11, "", "signal"),
            (1, "native linker failed", "build-failed"),
        ):
            observed = runner.Observation("build", ["gti"], status, "", stderr)
            self.assertEqual(runner.compiler_failure(observed)["kind"], kind)
        self.assertIsNone(runner.compiler_failure(runner.Observation("build", [], 0, "", "")))

    def test_timeout_retains_partial_raw_output(self):
        with patch.object(runner, "run_process", side_effect=subprocess.TimeoutExpired(["gti"], 1, b"partial", b"detail")):
            failure = self.probe()
        self.assertEqual(failure["signature"], {"kind": "timeout", "stage": "build"})
        self.assertEqual(failure["observations"][0]["stdout"], "partial")
        self.assertEqual(failure["observations"][0]["stderr"], "detail")

    def test_backend_neutral_output_keeps_distinct_developer_causes(self):
        heading = "error[GTI-B0001]: Internal compiler error: backend failed while generating a representation.\n"
        failures = []
        for reason in ("invalid call input 17", "invalid cleanup 23"):
            text = heading + "Backend: cpp\nDetail: " + reason + "\n"
            observed = runner.Observation("build", ["gti"], 65, "", text)
            failures.append(runner.compiler_failure(observed))
            record = {"stage": "cpp-emission", "timed_out": False, "signal": None,
                      "exit": 65, "_stdout": b"", "_stderr": text.encode()}
            self.assertEqual(capture.signature_from(record)["stem"], reason)
        self.assertNotEqual(failures[0]["reason"], failures[1]["reason"])
        self.assertEqual(failures[0]["reason"], "invalid call input #")
        text = ("Emitting backend representation\n+ compiler /tmp/random-17.cpp\n"
                "gti: backend diagnostics:\n/tmp/random-17.cpp:42: error: invalid native call 19\n"
                "gti: backend failed while compiling and linking the executable (exit code 1)\n")
        native = runner.Observation("build", ["gti"], 1, "", text)
        self.assertEqual(runner.compiler_failure(native)["reason"], "invalid native call #")
        for heading in ("gti: backend failed while compiling and linking the executable (exit code 9)",
                        "gti: native C++ compiler failed with exit code 9"):
            record = {"stage": "native-build", "timed_out": False, "signal": None,
                      "exit": 9, "_stdout": b"", "_stderr": heading.encode()}
            self.assertEqual(capture.signature_from(record)["kind"], "native-toolchain")
        for probe in capture.direct_ladder("main.gti"):
            self.assertEqual("--verbose" in probe.argv, probe.stage != "run")
        record = {"stage": "native-build", "timed_out": False, "signal": None,
                  "exit": 1, "_stdout": b"",
                  "_stderr": b"/tmp/random-17.cpp:42:9: error: invalid call\ngti: backend failed while compiling and linking"}
        current = capture.signature_from(record)
        self.assertEqual(current["stem"], "invalid call")
        legacy = dict(current, stem="random-99.cpp: error: invalid call")
        self.assertTrue(capture.signatures_match(legacy, current))

    def test_actual_process_timeout_and_capture(self):
        with self.assertRaises(subprocess.TimeoutExpired) as caught:
            runner.run_process([sys.executable, "-c", "import time; print('started', flush=True); time.sleep(30)"], 1)
        self.assertEqual(caught.exception.stdout, "started\n")

    def test_reducer_preserves_kind_and_regenerates_oracle(self):
        signature = {"kind": "oracle-mismatch", "stage": "run", "channels": ["stdout"]}
        record = {"schema": "gti-feature-interaction-failure-v1", "signature": signature,
                  "case": self.case.record(), "variant": "original", "configuration": ["O0", "c++23"],
                  "expected": list(self.case.oracle()), "observations": []}
        def injected(case, *args, **kwargs):
            observed = dict(record, case=case.record(), expected=list(case.oracle()))
            if case.enabled("lambda") and case.enabled("move_only"):
                return observed
            # A replacement semantic rejection must not count as persistence of
            # the original wrong-code failure, even though both fail the suite.
            return dict(observed, signature={"kind": "rejected-valid", "stage": "build", "code": "GTI-S2003"})
        output = self.root / "reduced"
        output.mkdir()
        with patch.object(runner, "probe", injected):
            self.assertTrue(runner.reduce_failure(self.case, record, output, "gti", "c++", 1, 40))
        reduced, failure = runner.read_failure(output / "failure.json")
        self.assertEqual({name for name in FACTORS if reduced.enabled(name)}, {"lambda", "move_only"})
        self.assertEqual(failure["signature"], signature)
        self.assertEqual((reduced.base, reduced.delta, reduced.width), (1, 1, 2))
        report = json.loads((output / "reduction.json").read_text())
        self.assertLess(report["reduced_bytes"], report["original_bytes"])
        self.assertLessEqual(report["probes"], 40)

    def test_reducer_budget_and_disappeared_failure(self):
        record = {"signature": {"kind": "internal", "stage": "build"},
                  "variant": "original", "configuration": ["O0", "c++23"]}
        output = self.root / "limited"
        output.mkdir()
        with patch.object(runner, "probe", return_value=dict(record, case=self.case.record())):
            self.assertTrue(runner.reduce_failure(self.case, record, output, "gti", "c++", 1, 1))
        self.assertEqual(json.loads((output / "reduction.json").read_text())["status"], "probe-budget")
        vanished = self.root / "vanished"
        vanished.mkdir()
        with patch.object(runner, "probe", return_value=None):
            self.assertFalse(runner.reduce_failure(self.case, record, vanished, "gti", "c++", 1, 40))
        self.assertFalse((vanished / "reduced.gti").exists())

    def test_existing_output_is_not_overwritten(self):
        marker = self.root / "user-file"
        marker.write_text("keep")
        with self.assertRaises(FileExistsError):
            runner.fresh_output(str(self.root))
        self.assertEqual(marker.read_text(), "keep")

    def test_findings_group_predicates_without_discarding_evidence(self):
        first = {"signature": {"kind": "internal", "stage": "build", "reason": "first"},
                 "case": {"index": 4}}
        second = dict(first, case={"index": 7})
        unrelated = {"signature": {"kind": "oracle-mismatch", "stage": "run", "channels": ["stdout"]},
                     "case": {"index": 4}}
        groups = runner.grouped_findings([("a.json", first), ("b.json", second),
                                          ("c.json", first), ("d.json", unrelated)])
        self.assertEqual(len(groups), 2)
        self.assertEqual(groups[0]["occurrences"], 3)
        self.assertEqual(groups[0]["cases"], [4, 7])
        self.assertEqual(groups[0]["evidence"], ["a.json", "b.json", "c.json"])
        self.assertEqual(groups[1]["evidence"], ["d.json"])

    def test_triage_measures_each_admission_boundary(self):
        build = {"kind": "internal", "stage": "build", "code": "GTI-B0001", "reason": "invariant"}
        run = {"kind": "oracle-mismatch", "stage": "run", "channels": ["stdout"]}
        for index, (mir_ok, cpp_ok, signature, boundary) in enumerate((
            (False, False, build, "before-verified-mir"),
            (True, False, build, "cpp-emission"),
            (True, True, build, "native-build-or-driver"),
            (True, True, run, "native-execution"),
        )):
            prior = {"signature": signature, "variant": "original", "configuration": ["O0", "c++23"]}
            def admission(command, timeout):
                if "--version" in command:
                    return subprocess.CompletedProcess(command, 0, "gti test\n", "")
                accepted = mir_ok if "--emit-mir" in command else cpp_ok
                if accepted:
                    Path(command[command.index("-o") + 1]).touch()
                return subprocess.CompletedProcess(command, 0 if accepted else 65, "",
                    "" if accepted else "error[GTI-B0001]: Internal compiler error: invariant")
            output = self.root / f"triage-{index}"
            output.mkdir()
            with patch.object(runner, "probe", return_value=prior), patch.object(runner, "run_process", admission):
                self.assertTrue(runner.triage_failure(self.case, prior, output, "gti", "c++", 1))
            report = json.loads((output / "triage.json").read_text())
            self.assertEqual(report["observed_boundary"], boundary)
            self.assertEqual(report["phases"]["mir"]["accepted"], mir_ok)
            self.assertEqual(report["phases"]["cpp"]["accepted"], cpp_ok)
            self.assertTrue((output / "002-mir.stderr").exists())
            self.assertTrue((output / "003-cpp.stdout").exists())

    def test_triage_replacement_and_missing_artifact_are_not_admission(self):
        prior = {"signature": {"kind": "internal", "stage": "build"},
                 "variant": "renamed", "configuration": ["O3", "c++20"]}
        replacement = dict(prior, signature={"kind": "rejected-valid", "stage": "build"})
        output = self.root / "triage-replaced"
        output.mkdir()
        def no_artifacts(command, timeout):
            return subprocess.CompletedProcess(command, 0, "", "")
        with patch.object(runner, "probe", return_value=replacement), patch.object(runner, "run_process", no_artifacts):
            self.assertFalse(runner.triage_failure(self.case, prior, output, "gti", "c++", 1))
        report = json.loads((output / "triage.json").read_text())
        self.assertEqual(report["status"], "replaced")
        self.assertEqual(report["observed_boundary"], "not-reproduced")
        self.assertFalse(report["phases"]["mir"]["accepted"])
        self.assertFalse(report["phases"]["cpp"]["accepted"])


if __name__ == "__main__":
    unittest.main()
