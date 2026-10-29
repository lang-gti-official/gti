#!/usr/bin/env python3
"""Path-to-selection cases for the map-driven verification planner."""

from __future__ import annotations

import json
from pathlib import Path
import re
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / ".codex/skills/verify-change/scripts"))
import plan_checks  # noqa: E402

PLANNER = ROOT / ".codex/skills/verify-change/scripts/plan_checks.py"


def fixture_map(**overrides):
    document = {
        "schema": 1,
        "configuration_dependent": {"tests": ["mac_only"], "reason": "macOS only"},
        "area": [
            {"area": "parser", "paths": ["src/compiler/parser.cpp", "include/gti/parser.h"],
             "owners": ["parser_tests"], "consumers": ["lsp_protocol", "corpus"]},
            {"area": "mir", "risk": "shared", "paths": ["src/compiler/mir_*"],
             "owners": ["^mir_"], "consumers": ["corpus", "parser_tests"]},
            {"area": "abi", "risk": "system", "paths": ["src/compiler/llvm/abi.*"],
             "owners": ["llvm_abi"], "consumers": []},
            {"area": "llvm", "paths": ["src/compiler/llvm/"],
             "owners": ["^llvm_"], "consumers": ["corpus"]},
            {"area": "deep", "paths": ["docs/**/index.md"], "owners": [], "consumers": []},
        ],
        "unowned": [{"paths": ["scripts/tool.py"], "reason": "manual tool"}],
    }
    document.update(overrides)
    return document


def tests_of(selection, tier):
    return {row["test"]: row.get("areas") or row.get("paths") for row in selection[tier]}


class Patterns(unittest.TestCase):
    def test_prefix_exact_and_globs(self):
        self.assertTrue(plan_checks.matches("src/compiler/llvm/", "src/compiler/llvm/body.cpp"))
        self.assertTrue(plan_checks.matches("src/compiler/llvm/", "src/compiler/llvm/x/y.cpp"))
        self.assertFalse(plan_checks.matches("src/compiler/llvm/", "src/compiler/llvm_backend.cpp"))
        self.assertTrue(plan_checks.matches("src/compiler/mir_*", "src/compiler/mir_effects.cpp"))
        self.assertFalse(plan_checks.matches("src/compiler/mir_*", "src/compiler/mir.cpp"))
        self.assertFalse(plan_checks.matches("src/*", "src/compiler/mir.cpp"))
        self.assertTrue(plan_checks.matches("docs/**/index.md", "docs/a/b/index.md"))
        self.assertTrue(plan_checks.matches("VERSION", "VERSION"))
        self.assertFalse(plan_checks.matches("VERSION", "VERSION.txt"))

    def test_ctest_regex_is_anchored_and_selects_exactly(self):
        regex = plan_checks.ctest_regex(["b.c", "a", "^mir_", "a"])
        self.assertEqual(regex, r"^(a|b\.c)$|^mir_")
        compiled = re.compile(regex)
        for name, expected in (("a", True), ("b.c", True), ("bxc", False), ("aa", False),
                               ("mir_census", True), ("not_mir_x", False)):
            self.assertEqual(bool(compiled.search(name)), expected, name)
        self.assertEqual(plan_checks.ctest_command(["compiler_pipeline"]),
                         "ctest --test-dir build --output-on-failure -R '^(compiler_pipeline)$'")


class Schema(unittest.TestCase):
    def test_fixture_is_valid(self):
        self.assertEqual(plan_checks.schema_errors(fixture_map()), [])

    def test_rejects_malformed_entries(self):
        bad = fixture_map()
        bad["area"].append(dict(bad["area"][0]))
        bad["area"].append({"area": "Bad Label", "paths": [], "owners": ["mir.*"],
                            "consumers": ["^(unclosed"], "risk": "high", "extra": 1})
        bad["area"].append({"area": "escape", "paths": ["../outside", "/abs"],
                            "owners": ["x", "x"], "consumers": []})
        errors = "\n".join(plan_checks.schema_errors(bad))
        for fragment in ("'parser' is duplicated", "short lowercase label", "unknown key 'extra'",
                         "paths must be a non-empty list", "neither an exact test name",
                         "not a valid regex", "risk must be one of", "must be repository-relative",
                         "listed twice"):
            self.assertIn(fragment, errors)
        self.assertIn("unknown top-level key", "\n".join(
            plan_checks.schema_errors(fixture_map(extra=1))))
        self.assertIn("needs exactly paths and reason", "\n".join(
            plan_checks.schema_errors(fixture_map(unowned=[{"paths": ["x"]}]))))
        with self.assertRaises(ValueError):
            plan_checks.parse_map(bad)


class Selection(unittest.TestCase):
    def setUp(self):
        self.map = plan_checks.parse_map(fixture_map())

    def test_owners_consumers_and_selecting_areas(self):
        result = plan_checks.select(["src/compiler/parser.cpp"], self.map)
        self.assertEqual(result["surfaces"], {"parser": ["src/compiler/parser.cpp"]})
        self.assertEqual(tests_of(result["selection"], "T1"), {"parser_tests": ["parser"]})
        self.assertEqual(tests_of(result["selection"], "T2"),
                         {"lsp_protocol": ["parser"], "corpus": ["parser"]})
        self.assertIsNone(result["risk"])
        self.assertEqual(result["uncovered"], [])

    def test_owner_is_not_repeated_as_consumer_and_areas_merge(self):
        result = plan_checks.select(["src/compiler/parser.cpp", "src/compiler/mir_effects.cpp"],
                                    self.map)
        t1, t2 = tests_of(result["selection"], "T1"), tests_of(result["selection"], "T2")
        self.assertEqual(t1, {"parser_tests": ["parser"], "^mir_": ["mir"]})
        self.assertNotIn("parser_tests", t2)
        self.assertEqual(t2["corpus"], ["mir", "parser"])
        self.assertEqual(result["risk"], "shared")

    def test_highest_risk_wins_across_nested_areas(self):
        result = plan_checks.select(["src/compiler/llvm/abi.cpp", "src/compiler/mir_x.cpp"], self.map)
        self.assertEqual(result["surfaces"]["abi"], ["src/compiler/llvm/abi.cpp"])
        self.assertEqual(result["surfaces"]["llvm"], ["src/compiler/llvm/abi.cpp"])
        self.assertEqual(result["risk"], "system")
        self.assertEqual(result["risk_areas"], ["abi"])
        self.assertEqual(set(tests_of(result["selection"], "T1")), {"llvm_abi", "^llvm_", "^mir_"})

    def test_shared_floor_applies_without_a_shared_area(self):
        result = plan_checks.select(["runtime/src/thread.cpp"], self.map)
        self.assertEqual(result["uncovered"], ["runtime/src/thread.cpp"])
        self.assertEqual(result["risk"], "shared")

    def test_uncovered_and_unowned_paths_are_reported(self):
        result = plan_checks.select(["scripts/tool.py", "src/new_thing.cpp"], self.map)
        self.assertEqual(result["uncovered"], ["src/new_thing.cpp"])
        self.assertEqual(result["unowned"], {"scripts/tool.py": "manual tool"})
        self.assertEqual(result["selection"], {"T1": [], "T2": []})


class Checks(unittest.TestCase):
    def setUp(self):
        self.map = plan_checks.parse_map(fixture_map())

    def names(self, paths, regressions=None):
        selection = plan_checks.select(paths, self.map)
        return {check.name: check for check in plan_checks.candidate_checks(
            paths, selection, regressions or {})}

    def test_implementation_change_gets_baseline_and_tiers(self):
        checks = self.names(["src/compiler/parser.cpp"])
        self.assertEqual(checks["baseline"].command,
                         "ctest --test-dir build --output-on-failure -R '^(compiler_pipeline)$'")
        self.assertEqual(checks["native-smoke"].command,
                         "python3 scripts/validation_checks.py smoke --build build")
        self.assertIn("format", checks)
        self.assertIn("build", checks)
        self.assertEqual(checks["T1"].command,
                         "ctest --test-dir build --output-on-failure -R '^(parser_tests)$'")
        self.assertIn("parser", checks["T1"].reason)
        self.assertEqual(checks["T2"].command,
                         "ctest --test-dir build --output-on-failure -R '^(corpus|lsp_protocol)$'")

    def test_automation_change_skips_build_and_baseline(self):
        checks = self.names(["scripts/agent_workflow.py"])
        for name in ("build", "baseline", "native-smoke"):
            self.assertNotIn(name, checks)
        self.assertEqual(checks["agent-workflow"].command, "python3 tests/agent_workflow_test.py")
        checks = self.names(["tests/ownership.toml"])
        self.assertIn("baseline", checks)
        self.assertEqual(checks["ownership-map"].command, "python3 tests/ownership_map_test.py")

    def test_tree_sitter_checks_are_kept(self):
        checks = self.names(["tree-sitter-gti/grammar.js"])
        self.assertIn("tree-sitter-generate", checks)
        self.assertIn("tree-sitter-generated-diff", checks)

    def test_regression_files_select_their_registered_tests(self):
        cmake = ('add_executable(gti_widget_tests widget_tests.cpp)\n'
                 'add_test(NAME widget COMMAND gti_widget_tests)\n'
                 'add_test(NAME widget_runtime COMMAND "${Python3_EXECUTABLE}"\n'
                 '  "${PROJECT_SOURCE_DIR}/tests/widget_runtime_test.py" $<TARGET_FILE:gti>\n'
                 '  "${PROJECT_SOURCE_DIR}/tests/fixtures/widget.gti")\n'
                 'add_test(NAME other COMMAND gti_widget_tests_extra)\n')
        executables = {"gti_widget_tests": " widget_tests.cpp"}
        tests = {match.group(1): match.group(2) for match in re.finditer(
            r'add_test\(\s*NAME\s+"?([A-Za-z0-9_.-]+)"?([^)]*)\)', cmake)}
        self.assertEqual(plan_checks.regression_tests(
            ["tests/widget_tests.cpp", "tests/fixtures/widget.gti", "tests/CMakeLists.txt"],
            executables, tests),
            {"widget": ["tests/widget_tests.cpp"], "widget_runtime": ["tests/fixtures/widget.gti"]})
        checks = self.names(["tests/fixtures/widget.gti"], {"widget_runtime": ["tests/fixtures/widget.gti"]})
        self.assertEqual(checks["T0"].command,
                         "ctest --test-dir build --output-on-failure -R '^(widget_runtime)$'")


class RepositoryMap(unittest.TestCase):
    """The committed map, read through the planner's own command line."""

    def run_planner(self, *paths):
        result = subprocess.run([sys.executable, str(PLANNER), "--format", "json", "--paths", *paths],
                                cwd=ROOT, check=True, capture_output=True, text=True)
        return json.loads(result.stdout)

    def test_committed_map_selects_owning_and_consumer_suites(self):
        payload = self.run_planner("src/compiler/llvm/body.cpp")
        self.assertIn("llvm-backend", payload["surfaces"])
        self.assertIn("^llvm_", tests_of(payload["selection"], "T1"))
        self.assertEqual(payload["uncovered"], [])
        payload = self.run_planner("src/compiler/mir_lowering.cpp")
        self.assertEqual(payload["risk"], "shared")
        t2 = tests_of(payload["selection"], "T2")
        self.assertIn("lowered_program_corpus_census", t2)
        self.assertIn("language_coverage_corpus", t2)
        payload = self.run_planner("runtime/src/thread.cpp")
        self.assertEqual(payload["risk"], "system")
        self.assertIn("rollup=never", payload["risk_advice"])
        payload = self.run_planner("tests/CMakeLists.txt")
        self.assertEqual(payload["risk"], "system")

    def test_text_output_names_areas_and_commands(self):
        result = subprocess.run([sys.executable, str(PLANNER), "--paths", "src/lsp/main.cpp",
                                 "no/such/path.cpp"], cwd=ROOT, check=True, capture_output=True,
                                text=True)
        self.assertIn("lsp (1):", result.stdout)
        self.assertIn("[T1] ctest --test-dir build --output-on-failure -R ", result.stdout)
        self.assertIn("Uncovered (no ownership-map area", result.stdout)
        self.assertIn("  - no/such/path.cpp", result.stdout)
        self.assertIn("[native-smoke]", result.stdout)


if __name__ == "__main__":
    unittest.main()
