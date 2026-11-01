#!/usr/bin/env python3
"""Target-profile controls; these do not replace native corpus execution."""
from copy import deepcopy
from contextlib import redirect_stderr, redirect_stdout
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import mir_census_baseline as reviewed
import mir_census_test as census
import mir_cutover_corpus_oracle_test as oracle

ROOT = Path(__file__).resolve().parents[1]
DOCUMENT = json.loads((ROOT / "tests/mir_census_baseline.json").read_text())


class ProfileControls(unittest.TestCase):
    def test_profiles_keep_complete_inventory(self):
        mac, linux = (reviewed.select(DOCUMENT, target) for target in reviewed.TARGETS)
        names = {p.name for p in (ROOT / "examples").glob("*.gti")}
        self.assertEqual(set(mac), names)
        self.assertEqual(set(linux), names)
        self.assertEqual({name: (mac[name], linux[name]) for name in names
                          if mac[name] != linux[name]}, {
            "087-associative-containers.gti": (448, 447),
            "089-library-workflow.gti": (913, 912),
        })
        lowered = json.loads((ROOT / "tests/lowered_program_census_baseline.json").read_text())
        for name, row in lowered["variants"][reviewed.TARGETS[1]]["examples"].items():
            self.assertEqual(linux[name] - mac[name],
                             row["bodies"] - lowered["examples"][name]["bodies"])

    def test_host_mapping_and_unknown_host(self):
        for system, machine, expected in [("darwin", "arm64", reviewed.TARGETS[0]),
                                           ("darwin", "aarch64", reviewed.TARGETS[0]),
                                           ("linux", "x86_64", reviewed.TARGETS[1]),
                                           ("linux", "AMD64", reviewed.TARGETS[1])]:
            with patch.object(reviewed.sys, "platform", system), patch.object(reviewed.platform, "machine", return_value=machine):
                self.assertEqual(reviewed.host_target(), expected)
        with patch.object(reviewed.sys, "platform", "linux"), patch.object(reviewed.platform, "machine", return_value="riscv64"):
            with self.assertRaises(ValueError):
                reviewed.host_target()
        with self.assertRaises(ValueError):
            reviewed.select(DOCUMENT, "not-a-target")

    def test_reject_malformed_and_inactive_overrides(self):
        def rejected(change):
            bad = deepcopy(DOCUMENT)
            change(bad)
            for target in reviewed.TARGETS:
                with self.assertRaises(ValueError):
                    reviewed.select(bad, target)
        rejected(lambda d: d.update(total=d["total"] + 1))
        rejected(lambda d: d.update(target="unknown"))
        rejected(lambda d: d["variants"].update(unknown={}))
        rejected(lambda d: d["variants"].clear())
        variant = lambda d: d["variants"][reviewed.TARGETS[1]]
        rejected(lambda d: variant(d).update(total=0))
        rejected(lambda d: variant(d).update(examples={"missing.gti": 1}))
        for value in [None, True, 0, -1, 447.0, "447"]:
            rejected(lambda d: variant(d)["examples"].update({"087-associative-containers.gti": value}))

    def test_update_preserves_other_profile_and_rejects_partial_results(self):
        for target in reviewed.TARGETS:
            counts = reviewed.select(DOCUMENT, target)
            counts["001-basics.gti"] += 1
            updated = reviewed.updated(DOCUMENT, target, counts)
            self.assertEqual(reviewed.select(updated, target), counts)
            other = next(t for t in reviewed.TARGETS if t != target)
            self.assertEqual(reviewed.select(updated, other), reviewed.select(DOCUMENT, other))
            for invalid in [{**counts, "001-basics.gti": None}, {k: v for k, v in counts.items() if k != "001-basics.gti"}]:
                with self.assertRaises(ValueError):
                    reviewed.updated(DOCUMENT, target, invalid)

    def test_llvm_only_inventory_uses_the_native_host(self):
        case = next(case for case in census.examples.cases(ROOT)
                    if Path(case.source).name in census.examples.LLVM_ONLY_EXAMPLES)
        name = Path(case.source).name
        observed = []
        def inventory(command, **options):
            observed.append(command)
            self.assertEqual(command, ["/unused/gti_lowered_program_census", str(ROOT),
                                       "--target", reviewed.TARGETS[1], name])
            return census.subprocess.CompletedProcess(command, 0,
                "generated_kinds\n" + name + "\t10\t7\t0\n", "")
        with patch.object(reviewed, "host_target", return_value=reviewed.TARGETS[1]), patch.object(census.examples, "cases", return_value=[case]), patch.object(census.examples, "require_cpp_limitation") as rejection, patch.object(census.subprocess, "run", side_effect=inventory):
            self.assertEqual(census.census(Path("/unused/gti"), ROOT), {name: 7})
            rejection.assert_called_once()
            self.assertEqual(len(observed), 1)

    def test_cli_rejects_invalid_or_nonhost_targets_before_observation(self):
        for module, extra in [(census, []), (oracle, ["/unused/runtime"])]:
            for target in ["invalid-triple", reviewed.TARGETS[1]]:
                argv = ["test", "/unused/gti", str(ROOT),
                        str(ROOT / "tests/mir_census_baseline.json"),
                        *extra, "--target", target]
                with patch.object(sys, "argv", argv), patch.object(reviewed, "host_target", return_value=reviewed.TARGETS[0]), redirect_stderr(io.StringIO()), patch.object(census, "census") as observe, patch.object(oracle, "build_and_run") as execute:
                    with self.assertRaises(SystemExit) as failure:
                        module.main()
                    self.assertEqual(failure.exception.code, 2)
                    observe.assert_not_called()
                    execute.assert_not_called()

    def test_both_consumers_select_linux_and_fail_closed(self):
        linux = reviewed.select(DOCUMENT, reviewed.TARGETS[1])
        with tempfile.TemporaryDirectory() as raw:
            baseline = Path(raw) / "baseline.json"
            baseline.write_text(json.dumps(DOCUMENT))
            def emitted(gti, root, scratch, source, optimization, standard, env, backend):
                markers = frozenset(("function-instance", i) for i in range(linux[source.name]))
                return oracle.CaseResult(source.name, optimization, standard, markers,
                                         frozenset(("scalar-cfg-v1", k, i) for k, i in markers), 0, "ok", "")
            with patch.object(reviewed, "host_target", return_value=reviewed.TARGETS[1]), redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                with patch.object(sys, "argv", ["census", "/unused/gti", str(ROOT), str(baseline)]), patch.object(census, "census", return_value=linux):
                    self.assertEqual(census.main(), 0)
                argv = ["oracle", "/unused/gti", str(ROOT), str(baseline), "/unused/runtime", "--case", "087-associative-containers.gti", "--case", "089-library-workflow.gti"]
                with patch.object(sys, "argv", argv), patch.object(oracle, "build_and_run", side_effect=emitted) as run:
                    self.assertEqual(oracle.main(), 0)
                    self.assertEqual(run.call_count, 4)  # Both endpoints for each selected example.
                # Even a selected-case run validates the entire corpus first.
                bad = deepcopy(DOCUMENT)
                del bad["examples"]["001-basics.gti"]
                bad["total"] -= 8
                bad["variants"][reviewed.TARGETS[1]]["total"] -= 8
                baseline.write_text(json.dumps(bad))
                with patch.object(sys, "argv", argv), patch.object(oracle, "build_and_run") as run:
                    self.assertEqual(oracle.main(), 1)
                    run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
