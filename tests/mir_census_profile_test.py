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

import census_budget as budget
import lowered_program_census_test as lowered_census
import mir_census_baseline as reviewed
import mir_census_test as census
import mir_cutover_corpus_oracle_test as oracle

ROOT = Path(__file__).resolve().parents[1]
DOCUMENT = json.loads((ROOT / "tests/mir_census_baseline.json").read_text())


class ProfileControls(unittest.TestCase):
    def test_profiles_keep_complete_inventory(self):
        mac, linux = (reviewed.select(DOCUMENT, target) for target in reviewed.TARGETS)
        # Recorded examples may lag the corpus (added ones are reported until
        # the next --refresh-all), but both profiles record the same examples.
        self.assertEqual(set(mac), set(linux))
        names = set(mac)
        # Reviewed macOS/Linux body differences, as offsets, applied once the
        # example is recorded. Offsets (not absolute counts) so a PR that
        # changes one of these examples does not have to edit this control.
        profile_offsets = {
            "087-associative-containers.gti": 1,
            "089-library-workflow.gti": 1,
            "186-consuming-ranges.gti": 1,
        }
        self.assertEqual({name: mac[name] - linux[name] for name in names
                          if mac[name] != linux[name]},
                         {name: offset for name, offset in profile_offsets.items()
                          if name in names})
        lowered = json.loads((ROOT / "tests/lowered_program_census_baseline.json").read_text())
        for name, row in lowered["variants"][reviewed.TARGETS[1]]["examples"].items():
            if name not in names:
                continue  # not yet recorded in the MIR census
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
        # Totals are derived; a stored one is tolerated only when consistent.
        rejected(lambda d: d.update(total=sum(d["examples"].values()) + 1))
        rejected(lambda d: d.update(target="unknown"))
        rejected(lambda d: d["variants"].update(unknown={}))
        rejected(lambda d: d["variants"].clear())
        variant = lambda d: d["variants"][reviewed.TARGETS[1]]
        rejected(lambda d: variant(d).update(total=0))
        rejected(lambda d: variant(d).update(examples={"missing.gti": 1}))
        for value in [None, True, 0, -1, 447.0, "447"]:
            rejected(lambda d: variant(d)["examples"].update({"087-associative-containers.gti": value}))

    def test_totals_are_derived_never_stored(self):
        # A stored total made every pull request rewrite the same lines and
        # any two of them conflict; the baseline carries counts only.
        self.assertNotIn("total", DOCUMENT)
        for variant in DOCUMENT["variants"].values():
            self.assertNotIn("total", variant)
        consistent = deepcopy(DOCUMENT)
        consistent["total"] = sum(consistent["examples"].values())
        for target in reviewed.TARGETS:
            self.assertEqual(reviewed.select(consistent, target), reviewed.select(DOCUMENT, target))
        refreshed = reviewed.updated(consistent, reviewed.TARGETS[0], reviewed.select(DOCUMENT, reviewed.TARGETS[0]))
        self.assertNotIn("total", refreshed)
        self.assertEqual(set(refreshed["variants"][reviewed.TARGETS[1]]), {"examples"})

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
                # An example the baseline has not recorded still runs, and the
                # summary must not read its missing count.
                unrecorded = deepcopy(DOCUMENT)
                del unrecorded["examples"]["087-associative-containers.gti"]
                unrecorded["variants"][reviewed.TARGETS[1]]["examples"].pop("087-associative-containers.gti", None)
                baseline.write_text(json.dumps(unrecorded))
                with patch.object(sys, "argv", argv), patch.object(oracle, "build_and_run", side_effect=emitted):
                    self.assertEqual(oracle.main(), 0)
                baseline.write_text(json.dumps(DOCUMENT))
                # Even a selected-case run validates the entire corpus first.
                bad = deepcopy(DOCUMENT)
                bad["examples"]["001-basics.gti"] = 0
                baseline.write_text(json.dumps(bad))
                with patch.object(sys, "argv", argv), patch.object(oracle, "build_and_run") as run:
                    self.assertEqual(oracle.main(), 1)
                    run.assert_not_called()


class BudgetControls(unittest.TestCase):
    def test_budget_is_the_larger_of_five_counts_or_five_percent(self):
        self.assertEqual(budget.allowance(12), 5)
        self.assertEqual(budget.allowance(256), 13)
        self.assertTrue(budget.within(256, 265))    # a nine-instance stdlib ripple
        self.assertTrue(budget.within(256, 243))    # loss is budgeted symmetrically
        self.assertFalse(budget.within(435, 611))   # a real body increase
        self.assertFalse(budget.within(12, 18))
        self.assertEqual(list(budget.ordered({"b": 1, "a": 2}, {"a": 3, "c": 4, "b": 5})),
                         ["b", "a", "c"])

    def run_census(self, observed):
        with tempfile.TemporaryDirectory() as raw:
            path = Path(raw) / "baseline.json"
            path.write_text(json.dumps(DOCUMENT))
            argv = ["census", "/unused/gti", str(ROOT), str(path)]
            out, err = io.StringIO(), io.StringIO()
            with patch.object(sys, "argv", argv), patch.object(census, "census", return_value=observed), \
                    patch.object(reviewed, "host_target", return_value=reviewed.TARGETS[0]), \
                    redirect_stdout(out), redirect_stderr(err):
                code = census.main()
            return code, out.getvalue() + err.getvalue()

    def test_census_fails_only_outside_the_budget(self):
        mac = reviewed.select(DOCUMENT, reviewed.TARGETS[0])
        name = next(n for n, c in mac.items() if c >= 200)
        broken = dict(mac, **{name: None})
        self.assertEqual(self.run_census(dict(mac, **{name: mac[name] + 9}))[0], 0)
        code, text = self.run_census(dict(mac, **{name: mac[name] + 9 + budget.allowance(mac[name])}))
        self.assertEqual(code, 1)
        self.assertIn("left its budget", text)
        code, text = self.run_census(broken)
        self.assertEqual(code, 1)
        self.assertIn("no longer compile", text)

    def test_unrecorded_and_removed_examples_are_reported_not_failed(self):
        mac = reviewed.select(DOCUMENT, reviewed.TARGETS[0])
        observed = {**{k: v for k, v in mac.items() if k != "001-basics.gti"}, "999-new.gti": 40}
        code, text = self.run_census(observed)
        self.assertEqual(code, 0)
        self.assertIn("unrecorded: 999-new.gti", text)
        self.assertIn("removed: 001-basics.gti", text)

    def test_partial_refresh_touches_only_named_rows_and_keeps_offsets(self):
        mac = reviewed.select(DOCUMENT, reviewed.TARGETS[0])
        linux = reviewed.select(DOCUMENT, reviewed.TARGETS[1])
        counts = {name: count + 1 for name, count in mac.items()}
        counts["087-associative-containers.gti"] = mac["087-associative-containers.gti"] + 40
        result = reviewed.refreshed(DOCUMENT, reviewed.TARGETS[0], counts,
                                    {"087-associative-containers.gti"})
        after_mac = reviewed.select(result, reviewed.TARGETS[0])
        after_linux = reviewed.select(result, reviewed.TARGETS[1])
        self.assertEqual({n for n in mac if after_mac[n] != mac[n]}, {"087-associative-containers.gti"})
        self.assertEqual(after_linux["087-associative-containers.gti"] - after_mac["087-associative-containers.gti"],
                         linux["087-associative-containers.gti"] - mac["087-associative-containers.gti"])
        self.assertEqual(list(result["examples"]), list(DOCUMENT["examples"]))
        with self.assertRaises(ValueError):
            reviewed.refreshed(DOCUMENT, reviewed.TARGETS[0], counts, {"999-new.gti"})

    def test_full_refresh_records_added_and_drops_removed_examples(self):
        mac = reviewed.select(DOCUMENT, reviewed.TARGETS[0])
        counts = {**{k: v for k, v in mac.items() if k != "001-basics.gti"}, "999-new.gti": 40}
        result = reviewed.refreshed(DOCUMENT, reviewed.TARGETS[0], counts)
        for target in reviewed.TARGETS:
            profile = reviewed.select(result, target)
            self.assertEqual(profile["999-new.gti"], 40)
            self.assertNotIn("001-basics.gti", profile)
        self.assertNotIn("999-new.gti", result["variants"][reviewed.TARGETS[1]]["examples"])


class LoweredBudgetControls(unittest.TestCase):
    ROW = {"bodies": 257, "generated_items": 143,
           "generated_kinds": {kind: 0 for kind in lowered_census.GENERATED_KINDS}}

    def test_declarations_are_not_recorded_or_compared(self):
        document = json.loads((ROOT / "tests/lowered_program_census_baseline.json").read_text())
        for rows in [document["examples"], *(v["examples"] for v in document["variants"].values())]:
            self.assertFalse(any("declarations" in row for row in rows.values()))
        legacy = dict(self.ROW, declarations=1000)
        self.assertEqual(lowered_census.outside_budget(legacy, self.ROW), [])

    def test_rows_fail_only_outside_the_budget(self):
        ripple = dict(self.ROW, bodies=266, generated_items=147)
        self.assertEqual(lowered_census.outside_budget(self.ROW, ripple), [])
        grown = dict(self.ROW, bodies=320)
        self.assertEqual(lowered_census.outside_budget(self.ROW, grown), ["bodies 257 -> 320"])
        kinds = dict(self.ROW, generated_kinds=dict(self.ROW["generated_kinds"], callable_adapter=9))
        self.assertEqual(lowered_census.outside_budget(self.ROW, kinds),
                         ["generated_kinds.callable_adapter 0 -> 9"])

    def test_partial_refresh_rewrites_only_named_rows(self):
        base = {"a.gti": self.ROW, "b.gti": self.ROW}
        document = {"examples": dict(base), "target": lowered_census.TARGETS[0],
                    "variants": {lowered_census.TARGETS[1]: {"examples": {}}}}
        changed = dict(self.ROW, bodies=400)
        observed = {target: {"a.gti": changed, "b.gti": dict(self.ROW, bodies=258)}
                    for target in lowered_census.TARGETS}
        result = lowered_census.refreshed(document, observed, {"a.gti"})
        self.assertEqual(result["examples"]["a.gti"]["bodies"], 400)
        self.assertEqual(result["examples"]["b.gti"]["bodies"], 257)
        self.assertEqual(result["variants"][lowered_census.TARGETS[1]]["examples"], {})


if __name__ == "__main__":
    unittest.main()
