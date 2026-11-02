#!/usr/bin/env python3
"""Risk-based validation plans, complete CTest evidence, and CI admission."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

SMOKE = "compiler_pipeline"
MARKER = re.compile(r"<!-- gti-validation-component: ([^\n]+) -->")
BATCH_DIRECTORY = ".gti-validation/batches/"
TIERS = {"T1", "T2", "T3"}
OPTIMIZED = {"Release", "RelWithDebInfo", "MinSizeRel"}
NAME = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.-]*\Z")
SHA = re.compile(r"[0-9a-f]{40}\Z")
# This is a conservative floor, not a substitute for reviewing the invariant.
SHARED = ("include/gti/semantic", "include/gti/hir", "include/gti/mir",
          "src/compiler/semantic_analyzer_impl.h", "src/compiler/semantic_model.cpp",
          "src/compiler/semantic_flow_analysis.cpp", "src/compiler/semantic_constexpr.cpp",
          "src/compiler/semantic_ownership", "src/compiler/semantic_type",
          "src/compiler/semantic_generic", "src/compiler/mir_", "src/compiler/hir_",
          "src/compiler/cpp_", "src/compiler/optimization/", "runtime/")
AUTOMATION = ("docs/", ".codex/", ".claude/", ".agents/")
# Only orchestration workflows with workflow-regression coverage can skip native CI.
# Build, packaging and integration workflows (including unknown additions) stay full.
AUTOMATION_WORKFLOWS = {".github/workflows/rollup.yml", ".github/workflows/train.yml",
                        ".github/workflows/backport.yml", ".github/workflows/rollback.yml"}
FULL_VALIDATION_FILES = {"scripts/validation_checks.py", "scripts/validation_batch.py",
                         "scripts/named_test_shards.py", "tests/ci_release_test.py",
                         "tests/gtiup_test.py", "tests/preview_release_test.py"}
AUTOMATION_FILES = {"AGENTS.md", "CLAUDE.md", "README.md"}
AUTOMATION_SCRIPTS = {"agent_workflow.py", "validation_batch.py", "validation_checks.py",
                      "preview_workflow.py", "preview_submit.py", "build_window.py", "bounded_log.py",
                      "cut_release.py", "check_release_version.py", "rollup.py", "release_train.py"}
# Exactly the existing harnesses that CI's workflow-regression job runs. The
# batch, validation and preview-submit scripts are covered by agent_workflow_test.
AUTOMATION_TESTS = {"agent_workflow_test.py", "preview_workflow_test.py", "build_window_test.py",
                    "bounded_log_test.py", "release_version_test.py", "ci_resources_test.py",
                    "repository_hosting_test.py", "cut_release_test.py",
                    "ownership_map_test.py", "plan_checks_test.py", "rollup_test.py",
                    "release_train_test.py", "named_test_shards_test.py", "ci_release_test.py",
                    "gtiup_test.py", "preview_release_test.py"}
# A rollup's membership record lists queue-validated members; unlike a degraded-
# mode batch manifest it adds no admission obligation, so it leaves the
# classification of the members' own paths unchanged.
ROLLUP_MANIFEST = re.compile(re.escape(BATCH_DIRECTORY) + r"rollup-[0-9]{8}-[0-9]{1,4}\.json\Z")
# Changes that must validate alone in the merge queue and never inside a rollup:
# build-system and toolchain files, CI workflows, test registration and
# throughput, CI configuration scripts, and the runtime ABI/layout surface that
# generated code links against. A deliberately small, conservative path rule.
# TODO: read the `risk = "system"` areas of tests/ownership.toml once it lands.
SYSTEM_RISK_FILES = {"tests/CMakeLists.txt", "tests/TestThroughput.cmake", "CMakePresets.json",
                     "include/gti/object_layout.h", "runtime/src/atomic_lane.h"}
SYSTEM_RISK_PREFIXES = ("cmake/", ".github/workflows/", "scripts/configure_ci_", "runtime/include/gti/",
                        "src/compiler/llvm/abi.", "src/compiler/llvm/layout.",
                        "src/compiler/llvm/native_c_abi.", "src/compiler/llvm/runtime_abi.")


def system_risk(paths):
    """The paths that make a change Risk-system: it must validate alone, never in a rollup."""
    return sorted({path for path in paths if path in SYSTEM_RISK_FILES or path.startswith(SYSTEM_RISK_PREFIXES)
                   or Path(path).name == "CMakeLists.txt" or path.endswith((".cmake", ".cmake.in"))})


def focused_scope(paths):
    """Classify local focused eligibility, independently of advisory CI policy."""
    if ci_mode({}, paths) == "automation":
        return "automation"
    metadata = {"VERSION", "CHANGELOG.md", "README.md", "stdlib/README.md", "examples/README.md",
                "tests/stdlib_surface_manifest.json", "tests/mir_census_baseline.json",
                "tests/lowered_program_census_baseline.json"}
    def library(path):
        return path.startswith("stdlib/std/") and path.endswith(".gti")
    def allowed(path):
        return (library(path) or path in metadata or path.startswith("docs/") and path.endswith(".md") or
                path.startswith(("examples/", "tests/fixtures/")) and path.endswith(".gti") or
                re.fullmatch(r"tests/[A-Za-z0-9_]+_test\.py", path) is not None)
    if paths and any(library(path) for path in paths) and all(allowed(path) for path in paths):
        return "source-stdlib"
    return None


def source_focused_checks(checks):
    """Reject obvious broad work; harness scope/coverage still needs human review."""
    commands(checks)
    owning = False
    for command in checks:
        program = Path(command[0]).name
        if program in {"cmake", "ninja", "make", "gmake", "sh", "bash", "zsh"}:
            raise ValueError("Focused source-stdlib checks cannot run builds or shell wrappers")
        if program == "ctest":
            if any(arg.startswith(("--rerun-failed", "--script", "--build-and-test")) or arg == "-S"
                   for arg in command[1:]):
                raise ValueError("Focused source-stdlib CTest requires one exact test name")
            expressions = []
            for index, arg in enumerate(command[1:], 1):
                if arg in {"-R", "--tests-regex"} and index + 1 < len(command):
                    expressions.append(command[index + 1])
                elif arg.startswith("--tests-regex="):
                    expressions.append(arg.split("=", 1)[1])
            if len(expressions) != 1 or not re.fullmatch(r"\^[A-Za-z0-9_][A-Za-z0-9_-]*\$", expressions[0]):
                raise ValueError("Focused source-stdlib CTest requires one exact test name")
            owning = True
        elif program.startswith("python") and len(command) > 1:
            script = command[1]
            if Path(script).name in {"build_window.py", "agent_workflow.py", "remote_runner.py",
                                     "mir_census_test.py", "lowered_program_census_test.py"}:
                raise ValueError("Broad orchestration/corpus checks require the shared window")
            if Path(script).name == "stdlib_surface_test.py" and "--validate-only" not in command:
                raise ValueError("Full stdlib surface execution requires the shared window")
            owning |= re.fullmatch(r"tests/[A-Za-z0-9_]+_test\.py", script) is not None
    if not owning:
        raise ValueError("Focused source-stdlib validation needs a named owning test harness or exact CTest")


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args], text=True).strip()


def names(value, label, required=True):
    if not isinstance(value, list) or (required and not value) or len(value) > 256 or any(
            not isinstance(item, str) or not NAME.fullmatch(item) for item in value):
        raise ValueError(f"{label} must contain exact test/target names")
    if len(set(value)) != len(value):
        raise ValueError(f"Duplicate {label}")
    return value


def validate_plan(plan, paths=()):
    fields = {"tier", "rationale", "regressions", "owning", "cross_phase", "broader",
              "unaffected", "targets", "reproduction", "platforms", "additional_checks",
              "platform_tests", "platform_checks"}
    if not isinstance(plan, dict) or set(plan) - fields or plan.get("tier") not in TIERS:
        raise ValueError("Use a T1/T2/T3 validation plan with known fields")
    if not isinstance(plan.get("rationale"), str) or not plan["rationale"].strip():
        raise ValueError("Explain the changed invariant and risk tier")
    for key in ("regressions", "owning", "targets", "platforms"):
        names(plan.get(key), key)
    for key in ("cross_phase", "broader"):
        names(plan.get(key, []), key, required=False)
    if not plan.get("cross_phase") and not str(plan.get("unaffected", "")).strip():
        raise ValueError("Select cross-phase checks or explain why other phases are unaffected")
    if plan["tier"] != "T1" and not plan.get("broader"):
        raise ValueError("T2/T3 admission needs relevant broader subsystem checks")
    if plan["tier"] == "T1" and any(path.startswith(SHARED) for path in paths):
        raise ValueError("Shared semantic/IR/representation/runtime paths require at least T2")
    reproduction = plan.get("reproduction")
    if not isinstance(reproduction, dict):
        raise ValueError("Record the original failure or a justified reproduction exception")
    if "exception" in reproduction:
        if set(reproduction) != {"exception"} or not isinstance(reproduction["exception"], str) or not reproduction["exception"].strip():
            raise ValueError("Explain why a before-fix reproduction is not practical")
    else:
        if set(reproduction) != {"revision", "command", "failure", "log"} or not SHA.fullmatch(str(reproduction.get("revision", ""))):
            raise ValueError("Original failure needs revision, argv command, failure reason and log")
        commands([reproduction.get("command")])
        if not all(isinstance(reproduction.get(key), str) and reproduction[key].strip() for key in ("failure", "log")):
            raise ValueError("Record the observed original failure and retained log")
    commands(plan.get("additional_checks", []), allow_empty=True)
    if set(plan["platforms"]) - {"darwin", "linux", "win32"}:
        raise ValueError("Platform obligations use darwin, linux or win32")
    for field in ("platform_tests", "platform_checks"):
        rows = plan.get(field, {})
        if not isinstance(rows, dict) or set(rows) - set(plan["platforms"]):
            raise ValueError("Platform gates must belong to a declared platform")
        for value in rows.values():
            if field == "platform_tests":
                names(value, "platform_tests")
            else:
                commands(value)
    return plan


def commands(value, allow_empty=False):
    if not isinstance(value, list) or (not value and not allow_empty) or any(
            not isinstance(row, list) or not row or any(not isinstance(arg, str) or not arg for arg in row)
            for row in value):
        raise ValueError("Checks must be argv arrays, never shell text")
    return value


def plan_tests(plan, platform=None):
    scoped = plan.get("platform_tests", {})
    scoped_names = {name for rows in scoped.values() for name in rows}
    common = {name for key in ("regressions", "owning", "cross_phase", "broader") for name in plan.get(key, [])}
    return sorted({SMOKE, *(common - scoped_names), *scoped.get(platform or sys.platform, [])})


def extra_checks(plan, platform=None):
    return [*plan.get("additional_checks", []), *plan.get("platform_checks", {}).get(platform or sys.platform, [])]


def component_body(body, record=None):
    body = MARKER.sub("", body or "").rstrip()
    return body + ("\n\n<!-- gti-validation-component: " + json.dumps(record, sort_keys=True) + " -->" if record else "")


def ci_mode(event, paths):
    pr = event.get("pull_request")
    if pr:
        # CI does not subscribe to `edited`; a caller replaying one gets the
        # conservative rule: only an identified body-only edit is metadata,
        # and unknown edit payloads or title changes reclassify.
        changes = event.get("changes")
        if event.get("action") == "edited" and isinstance(changes, dict) and set(changes) == {"body"}:
            return "metadata"
        if MARKER.search(pr.get("body") or ""):
            return "component"
        if pr.get("draft"):
            return "draft"
    # Compiler tests, CMake and packaged scripts are deliberately not inferred
    # safe just because their filename includes 'test' or 'workflow'.
    own = [path for path in paths if not ROLLUP_MANIFEST.fullmatch(path)]
    if any(path in FULL_VALIDATION_FILES for path in own):
        return "full"
    if own and all(path in AUTOMATION_FILES or path in AUTOMATION_WORKFLOWS or path.startswith(AUTOMATION) or
                   path in {"scripts/" + name for name in AUTOMATION_SCRIPTS} or
                   path in {"tests/" + name for name in AUTOMATION_TESTS} for path in own):
        return "automation"
    return "full"


LINT_JOBS = {"plan", "formatting", "release-version", "tree-sitter", "automation-tests"}
EVENTS = {"pull_request", "merge_group"}


def admission(mode, results, event="pull_request"):
    """The one required check. A PR proves its fast checks before it may queue;
    the merge queue proves the complete suite on the exact candidate."""
    if event not in EVENTS:
        raise ValueError("Unknown CI event")
    if mode == "component":
        raise ValueError("Awaiting integration: a batch component cannot merge directly; finish its validated batch or withdraw admission")
    required = set(LINT_JOBS)
    if mode == "full":
        # System-LLVM checks run in fast; production package and full shards gate integration.
        required |= {"fast"} if event == "pull_request" else {"fast", "package", "integration", "sanitizers"}
    elif mode == "draft":
        if event != "pull_request":
            raise ValueError("A draft cannot enter the merge queue")
    elif mode != "automation":
        raise ValueError("Unknown CI validation mode")
    missing = sorted(key for key in required if results.get(key, {}).get("result") != "success")
    if missing:
        raise ValueError("Required validation did not pass: " + ", ".join(missing))
    return required


def build_cache(root, build):
    cache = dict(re.findall(r"^([^/#\r\n][^:\r\n]*):[^=\r\n]+=([^\r\n]*)$",
                            (build / "CMakeCache.txt").read_text(), re.M))
    if Path(cache.get("CMAKE_HOME_DIRECTORY", "")).resolve() != root or cache.get("CMAKE_BUILD_TYPE") not in OPTIMIZED:
        raise ValueError("Validation requires this worktree's optimized compiler build")
    return cache


def inventory(document, required):
    rows = document.get("tests", [])
    found = {row["name"]: row for row in rows}
    if len(found) != len(rows) or not found or set(required) - found.keys():
        raise ValueError("CTest inventory is empty, duplicated, or missing required tests")
    for name in required:
        if any(prop["name"] == "DISABLED" and prop["value"] for prop in found[name].get("properties", [])):
            raise ValueError("Required test is disabled: " + name)
    return set(found)


def results(path, expected, required):
    cases = ET.parse(path).getroot().findall(".//testcase")
    if len(cases) != len(expected) or {case.get("name") for case in cases} != set(expected):
        raise ValueError("CTest results do not match the selected inventory")
    skipped = []
    for case in cases:
        if case.find("failure") is not None or case.find("error") is not None:
            raise ValueError("CTest failure: " + str(case.get("name")))
        if case.find("skipped") is not None or case.get("status") == "notrun":
            skipped.append(case.get("name"))
    if set(required) & set(skipped):
        raise ValueError("A required member regression/owning/smoke test was skipped")
    return skipped


QUARANTINE = "tests/ci_quarantine.json"
QUARANTINE_ISSUE = re.compile(r"https://github\.com/lang-gti-official/gti/issues/[1-9][0-9]*\Z")
QUARANTINE_PLATFORMS = {"darwin", "linux"}


def load_quarantine(root):
    """Known failures already on main that the queue's suite excludes until fixed.

    Each entry names one registered test, the platforms it fails on, the issue
    tracking its fix, and why. Adding or widening an entry edits this file in
    review and runs the complete suite; the gate prints every exclusion it
    applies, and the smoke can never be quarantined.
    """
    path = Path(root) / QUARANTINE
    document = json.loads(path.read_text()) if path.exists() else {"schema": 1, "tests": []}
    if (not isinstance(document, dict) or document.get("schema") != 1
            or set(document) - {"schema", "description", "tests"} or not isinstance(document.get("tests"), list)):
        raise ValueError(QUARANTINE + " must be a schema 1 object with a tests list")
    table = {}
    for row in document["tests"]:
        if not isinstance(row, dict) or set(row) != {"name", "platforms", "issue", "reason", "since"}:
            raise ValueError("Each quarantine entry needs exactly name, platforms, issue, reason and since")
        name, platforms = row["name"], row["platforms"]
        if not isinstance(name, str) or not NAME.fullmatch(name) or name == SMOKE or name in table:
            raise ValueError(f"Invalid, duplicate or unquarantinable test: {name!r}")
        if (not isinstance(platforms, list) or not platforms or len(set(platforms)) != len(platforms)
                or set(platforms) - QUARANTINE_PLATFORMS):
            raise ValueError(f"Quarantine of {name} must list darwin and/or linux once each")
        if not isinstance(row["issue"], str) or not QUARANTINE_ISSUE.fullmatch(row["issue"]):
            raise ValueError(f"Quarantine of {name} must link its tracking issue in lang-gti-official/gti")
        if not isinstance(row["reason"], str) or not row["reason"].strip():
            raise ValueError(f"Quarantine of {name} must say why it fails")
        if not isinstance(row["since"], str) or not re.fullmatch(r"\d{4}-\d{2}-\d{2}", row["since"]):
            raise ValueError(f"Quarantine of {name} must record the date it began")
        table[name] = set(platforms)
    return table


def quarantined_on(table, platform):
    return sorted(name for name, platforms in table.items() if platform in platforms)


def shard_assignment(names, durations, shards, default=30.0):
    """Deterministic cost-balanced partition: longest first onto the lightest shard."""
    if shards < 1:
        raise ValueError("shards must be positive")
    loads, assigned = [0.0] * shards, [[] for _ in range(shards)]
    for name in sorted(names, key=lambda item: (-float(durations.get(item, default)), item)):
        index = min(range(shards), key=lambda candidate: (loads[candidate], candidate))
        assigned[index].append(name)
        loads[index] += float(durations.get(name, default))
    return [sorted(rows) for rows in assigned]


def merge_shards(reports, platforms):
    """Prove that each platform's shards ran exactly its whole inventory once."""
    summary = {}
    for platform in sorted(platforms):
        rows = [report for report in reports if report.get("platform") == platform]
        if not rows:
            raise ValueError("No shard results for required platform " + platform)
        first = rows[0]
        count = first.get("shards")
        if (not isinstance(count, int) or count < 1 or len(rows) != count or
                sorted(report.get("index") for report in rows) != list(range(count))):
            raise ValueError("Incomplete or duplicated shards for " + platform)
        for report in rows:
            if (report.get("scope") != "shard" or report.get("shards") != count or
                    report.get("head") != first.get("head") or report.get("tree") != first.get("tree") or
                    report.get("inventory") != first.get("inventory") or report.get("build_type") not in OPTIMIZED
                    or report.get("quarantined", []) != first.get("quarantined", [])):
                raise ValueError("Shards for " + platform + " disagree on source, inventory, quarantine or build")
        excluded = set(first.get("quarantined", []))
        if SMOKE in excluded or excluded - set(first["inventory"]):
            raise ValueError("Shards for " + platform + " quarantined the smoke or an unregistered test")
        executed = [name for report in rows for name in report.get("tests", [])]
        if len(executed) != len(set(executed)) or set(executed) != set(first["inventory"]) - excluded:
            raise ValueError("Shards for " + platform + " did not run their inventory exactly once")
        if not any(report.get("smoke") for report in rows):
            raise ValueError("No compiler smoke result for " + platform)
        summary[platform] = {"head": first["head"], "tests": len(executed), "shards": count,
                             "quarantined": sorted(excluded),
                             "skipped": sorted(name for report in rows for name in report.get("skipped", []))}
    heads = {row["head"] for row in summary.values()}
    if len(heads) != 1:
        raise ValueError("Platforms validated different sources")
    return summary


def smoke(root, build, log=None):
    """Small native execution checks, independent of the patch's own tests."""
    programs = {
        "scalar": """int twice(int value) { return value * 2; }
int main() {
  mut int sum = 0;
  for (mut int value = 0; value < 10; value++) { sum += value; }
  return twice(sum) == 90 ? 0 : 1;
}
""",
        "owners": """import std.string;
import std.vector;
int main() {
  mut std::vector<std::string> values{};
  values.push_back(std::string("smoke"));
  std::string copy = values[std::size_t(0)].clone();
  return int(copy.size()) == 5 ? 0 : 1;
}
""",
    }
    env = dict(os.environ, GTI_STDLIB_PATH=str(root / "stdlib"),
               GTI_RUNTIME_INCLUDE=str(root / "runtime/include"),
               GTI_RUNTIME_LIBRARY=str(build / ("gti_runtime.lib" if os.name == "nt" else "libgti_runtime.a")))
    with tempfile.TemporaryDirectory(prefix="gti-compiler-smoke-") as directory:
        for name, source in programs.items():
            path = Path(directory) / (name + ".gti")
            path.write_text(source)
            for level in ("0", "3"):
                output = Path(directory) / (name + "-O" + level + (".exe" if os.name == "nt" else ""))
                compiler = build / ("gti.exe" if os.name == "nt" else "gti")
                subprocess.run([str(compiler), str(path), "-O" + level, "-o", str(output)],
                               env=env, check=True, timeout=120, stdout=log, stderr=subprocess.STDOUT)
                subprocess.run([str(output)], check=True, timeout=10,
                               stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
    return [name + "-O" + level for name in programs for level in ("0", "3")]


def manifests(root, base, head):
    paths = git(root, "diff", "--name-only", base, head, "--", BATCH_DIRECTORY).splitlines()
    members = []
    for path in paths:
        if not path.endswith(".json") or not path.startswith(BATCH_DIRECTORY):
            raise ValueError("Unexpected batch record path")
        existed = subprocess.run(["git", "-C", str(root), "cat-file", "-e", base + ":" + path],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
        if existed:
            raise ValueError("Integrated batch records are immutable; use a new batch identity")
        record = json.loads(git(root, "show", head + ":" + path))
        if record.get("schema") != 1 or not isinstance(record.get("members"), list) or not 1 <= len(record["members"]) <= 8:
            raise ValueError("Invalid or oversized integration batch")
        if not SHA.fullmatch(str(record.get("base", ""))):
            raise ValueError("Batch base must be a full source identity")
        subprocess.run(["git", "-C", str(root), "merge-base", "--is-ancestor", record["base"], base], check=True)
        seen = set()
        for member in record["members"]:
            if not SHA.fullmatch(str(member.get("head", ""))) or member.get("pr") in seen or not isinstance(member.get("pr"), int):
                raise ValueError("Duplicate or invalid batch member")
            seen.add(member["pr"])
            validate_plan(member.get("plan"))
            subprocess.run(["git", "-C", str(root), "merge-base", "--is-ancestor", member["head"], head], check=True)
            members.append(member)
    return members


def run(root, build, required, full, targets=(), extra=(), jobs=4, log=None, junit=None):
    cache = build_cache(root, build)
    command = ["cmake", "--build", str(build), "--parallel", str(jobs)]
    if not full:
        command += ["--target", *sorted({"gti", "gti_runtime", *targets})]
    subprocess.run([sys.executable, str(root / "scripts/build_window.py"), "--kind", "build", "--", *command],
                   check=True, cwd=root, stdout=log, stderr=subprocess.STDOUT)
    # CTest's regular expressions have no non-capturing groups; `(?:` makes
    # it print a compile error ahead of its JSON listing.
    selected = [] if full else ["-R", "^(" + "|".join(re.escape(name) for name in sorted(required)) + ")$"]
    document = json.loads(subprocess.check_output(["ctest", "--test-dir", str(build), "--show-only=json-v1", *selected], text=True))
    expected = inventory(document, required)
    with tempfile.TemporaryDirectory(prefix="gti-validation-results-") as directory:
        report = Path(junit) if junit else Path(directory) / "ctest.xml"
        subprocess.run(["ctest", "--test-dir", str(build), "--no-tests=error", "--output-on-failure",
                        "--parallel", str(jobs), "--output-junit", str(report), *selected],
                       check=True, stdout=log, stderr=subprocess.STDOUT)
        skipped = results(report, expected, required)
    for command in dict.fromkeys(tuple(command) for command in extra):
        subprocess.run(command, cwd=root, check=True, stdout=log, stderr=subprocess.STDOUT)
    smoke_results = smoke(root, build, log)
    return {"head": git(root, "rev-parse", "HEAD"), "tree": git(root, "rev-parse", "HEAD^{tree}"),
            "platform": sys.platform, "build_type": cache["CMAKE_BUILD_TYPE"],
            "scope": "full" if full else "focused", "tests": sorted(expected), "skipped": skipped,
            "compiler": str(build / "gti"), "required": sorted(required), "smoke": smoke_results}


def run_shard(root, build, shards, index, durations, jobs=4, log=None, junit=None):
    cache = build_cache(root, build)
    if not 0 <= index < shards:
        raise ValueError("shard index out of range")
    subprocess.run(["cmake", "--build", str(build), "--parallel", str(jobs)],
                   check=True, cwd=root, stdout=log, stderr=subprocess.STDOUT)
    document = json.loads(subprocess.check_output(["ctest", "--test-dir", str(build), "--show-only=json-v1"], text=True))
    expected = inventory(document, {SMOKE})
    excluded = set(quarantined_on(load_quarantine(root), sys.platform))
    if excluded - expected:
        raise ValueError("Quarantined tests are not registered; remove them: " + ", ".join(sorted(excluded - expected)))
    table = json.loads(Path(durations).read_text())
    selected = shard_assignment(expected - excluded, table["seconds"], shards, table.get("default_seconds", 30))[index]
    skipped = []
    if selected:
        with tempfile.TemporaryDirectory(prefix="gti-validation-shard-") as directory:
            report = Path(junit) if junit else Path(directory) / "ctest.xml"
            subprocess.run(["ctest", "--test-dir", str(build), "--no-tests=error", "--output-on-failure",
                            "--parallel", str(jobs), "--output-junit", str(report),
                            "-R", "^(" + "|".join(re.escape(name) for name in selected) + ")$"],
                           check=True, stdout=log, stderr=subprocess.STDOUT)
            skipped = results(report, set(selected), set(selected) & {SMOKE})
    smoke_results = smoke(root, build, log) if index == 0 else []
    return {"head": git(root, "rev-parse", "HEAD"), "tree": git(root, "rev-parse", "HEAD^{tree}"),
            "platform": sys.platform, "build_type": cache["CMAKE_BUILD_TYPE"], "scope": "shard",
            "shards": shards, "index": index, "inventory": sorted(expected), "tests": selected,
            "quarantined": sorted(excluded), "skipped": skipped, "smoke": smoke_results}


DECLARED_KEYS = ("T0", "T1")
DECLARED_LIMIT = 60
DECLARATION_ITEM = re.compile(r"-\s+([A-Za-z0-9_-]+):\s*(.*)")


def declared_tests(body):
    """The CTest names a pull request declares as its T0 and T1 evidence.

    The PR body's `Validation:` block lists `- <key>: <value>` items. `T0` and
    `T1` items hold comma-separated CTest names only (continued on more-indented
    lines); every other key is free prose for reviewers. PR CI re-runs exactly
    these names on both platforms, so the body is untrusted input: anything that
    is not a plain test name is rejected rather than interpreted.
    """
    lines = (body or "").splitlines()
    starts = [index for index, line in enumerate(lines) if line.strip() == "Validation:"]
    if len(starts) != 1:
        raise ValueError("Add exactly one `Validation:` block naming the T0 and T1 CTest names the change ran")
    declared, key = {}, None
    for line in lines[starts[0] + 1:]:
        stripped = line.strip()
        if not stripped or stripped.startswith("```"):
            break
        item = DECLARATION_ITEM.fullmatch(stripped)
        if item:
            key, value = item.groups()
        elif line[:1].isspace() and key is not None:
            value = stripped
        else:
            break
        if key in DECLARED_KEYS:
            for name in (token.strip() for token in value.split(",")):
                if not name:
                    continue
                if not NAME.fullmatch(name):
                    raise ValueError(f"Validation {key} lists CTest names only; move notes such as {name!r} to another key")
                declared.setdefault(name, key)
    if not declared:
        raise ValueError("The `Validation:` block must name at least one T0 or T1 CTest test")
    if len(declared) > DECLARED_LIMIT:
        raise ValueError(f"Declare at most {DECLARED_LIMIT} T0/T1 tests; the merge queue runs the rest")
    return sorted(declared)


def uncovered_declarations(names, checks, root):
    """Declared names that no recorded check ran: a PR may only claim what it ran.

    A recorded check covers a name through a `ctest -R/--tests-regex` pattern
    or a `validation_checks.py focused --plan` plan. Labels and wrapper scripts
    are not expanded, so declare tests that a name-selecting check ran.
    """
    patterns, planned = [], set()
    for check in checks:
        argv = check.get("argv", []) if isinstance(check, dict) else check
        ctest = next((index for index, argument in enumerate(argv) if Path(argument).name == "ctest"), None)
        focused = "focused" in argv and any(Path(argument).name == "validation_checks.py" for argument in argv)
        for index, argument in enumerate(argv[:-1]):
            if ctest is not None and index > ctest and argument in ("-R", "--tests-regex"):
                patterns.append(argv[index + 1])
            elif focused and argument == "--plan":
                planned.update(plan_tests(validate_plan(json.loads((Path(root) / argv[index + 1]).read_text()))))
    def covered(name):
        return name in planned or any(re.search(pattern, name) for pattern in patterns)
    return [name for name in names if not covered(name)]


def run_declared(build, names, jobs=4, log=None):
    """Re-run declared tests by exact name; every one must be registered and enabled."""
    selected = sorted(set(names) - {SMOKE})
    if not selected:
        return []
    pattern = "^(" + "|".join(re.escape(name) for name in selected) + ")$"
    document = json.loads(subprocess.check_output(["ctest", "--test-dir", str(build), "--show-only=json-v1"], text=True))
    registered = {row["name"] for row in document.get("tests", [])}
    missing = [name for name in selected if name not in registered]
    if missing:
        raise ValueError("Declared tests are not registered CTest tests: " + ", ".join(missing))
    inventory(document, selected)
    with tempfile.TemporaryDirectory(prefix="gti-declared-") as directory:
        report = Path(directory) / "ctest.xml"
        subprocess.run(["ctest", "--test-dir", str(build), "--no-tests=error", "--output-on-failure",
                        "--parallel", str(jobs), "--output-junit", str(report), "-R", pattern],
                       check=True, stdout=log, stderr=subprocess.STDOUT)
        results(report, set(selected), set(selected))
    return selected


# CMake writes each argument either quoted ("...") or as a bracket argument
# ([=[...]=]), depending on its version; accept both.
TEST_LINE = re.compile(r'^add_test\((?P<name>"(?:[^"\\]|\\.)*"|\[(?P<level>=*)\[.*?\](?P=level)\]) '
                       r'(?P<arguments>.*)\)$', re.M)
QUOTED = re.compile(r'"((?:[^"\\]|\\.)*)"|\[(=*)\[(.*?)\]\2\]')


def cmake_argument(text):
    match = QUOTED.fullmatch(text)
    return match.group(1) if match.group(1) is not None else match.group(3)


def test_targets(build, pattern):
    """The build targets the tests matching `pattern` run, read from a configured tree.

    CTest learns a test's command only once it is built, so this reads the
    generated CTestTestfile.cmake files instead: every argument (or `key=path`
    argument) naming a file in the build tree is a target's output. The
    compiler and runtime are always included. A target this misses makes its
    test fail as not found, never pass.
    """
    build = Path(build).resolve()
    selected = re.compile(pattern)
    targets, matched = {"gti", "gti_runtime"}, 0
    for path in sorted(build.rglob("CTestTestfile.cmake")):
        for match in TEST_LINE.finditer(path.read_text(encoding="utf-8")):
            if not selected.search(cmake_argument(match.group("name"))):
                continue
            matched += 1
            for token in QUOTED.finditer(match.group("arguments")):
                argument = token.group(1) if token.group(1) is not None else token.group(3)
                for part in argument.split("="):
                    if part.startswith(str(build) + "/"):
                        name = Path(part).name
                        name = name.removeprefix("lib").rsplit(".", 1)[0] if name.startswith("lib") else name
                        if NAME.fullmatch(name):
                            targets.add(name)
    if not matched:
        raise ValueError("No configured test matches " + pattern)
    return sorted(targets)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    ci = sub.add_parser("ci-mode")
    ci.add_argument("--event", required=True)
    ci.add_argument("--base", required=True)
    ci.add_argument("--head", default="HEAD")
    gate = sub.add_parser("ci-gate")
    gate.add_argument("--mode", required=True)
    gate.add_argument("--results", required=True)
    gate.add_argument("--event", default="pull_request")
    gate.add_argument("--shard-reports", help="directory of shard reports, required for merge_group full")
    gate.add_argument("--platforms", default="darwin,linux")
    release = sub.add_parser("merge-shards", help="prove a release suite ran every test once per platform")
    release.add_argument("--shard-reports", required=True)
    release.add_argument("--platforms", default="darwin,linux")
    release.add_argument("--head", required=True)
    shard = sub.add_parser("shard")
    shard.add_argument("--build", required=True)
    shard.add_argument("--shards", type=int, required=True)
    shard.add_argument("--index", type=int, required=True)
    shard.add_argument("--durations", default="tests/ci_test_durations.json")
    shard.add_argument("--jobs", type=int, default=4)
    shard.add_argument("--report", required=True)
    shard.add_argument("--log")
    declared = sub.add_parser("declared")
    declared.add_argument("--event", required=True)
    declared.add_argument("--build", help="re-run the declared tests in this build tree")
    declared.add_argument("--jobs", type=int, default=4)
    smoke_parser = sub.add_parser("smoke", help="run only the native O0/O3 compiler smoke")
    smoke_parser.add_argument("--build", required=True)
    smoke_parser.add_argument("--log")
    targets = sub.add_parser("test-targets", help="print the build targets the matching tests run")
    targets.add_argument("--build", required=True)
    targets.add_argument("--regex", required=True)
    for name in ("focused", "full"):
        run_parser = sub.add_parser(name)
        run_parser.add_argument("--build", required=True)
        run_parser.add_argument("--jobs", type=int, default=4)
        run_parser.add_argument("--report")
        run_parser.add_argument("--base", default="origin/main")
        run_parser.add_argument("--plan")
        run_parser.add_argument("--log")
    args = parser.parse_args(argv)
    root = Path(__file__).resolve().parents[1]
    if args.command == "ci-mode":
        event = json.loads(Path(args.event).read_text())
        paths = git(root, "diff", "--name-only", args.base, args.head).splitlines()
        print("mode=" + ci_mode(event, paths))
        return
    if args.command == "ci-gate":
        if args.mode == "metadata":
            print("Metadata-only PR edit: no compiler validation or integration acceptance is claimed.")
            return
        admission(args.mode, json.loads(args.results), args.event)
        if args.mode == "full" and args.event == "merge_group":
            if not args.shard_reports:
                raise ValueError("The merge queue gate needs the shard reports")
            reports = [json.loads(path.read_text()) for path in sorted(Path(args.shard_reports).rglob("*.json"))]
            summary = merge_shards(reports, [p for p in args.platforms.split(",") if p])
            print("GTI_INTEGRATION_RESULT=" + json.dumps(summary, sort_keys=True))
        return
    if args.command == "declared":
        pr = json.loads(Path(args.event).read_text()).get("pull_request") or {}
        names = declared_tests(pr.get("body"))
        if args.build:
            names = run_declared(Path(args.build).resolve(), names, args.jobs)
        print("GTI_DECLARED_TESTS=" + json.dumps(names))
        return
    if args.command == "merge-shards":
        reports = [json.loads(path.read_text()) for path in sorted(Path(args.shard_reports).rglob("*.json"))]
        summary = merge_shards(reports, [p for p in args.platforms.split(",") if p])
        if {row["head"] for row in summary.values()} != {args.head}:
            raise ValueError("Release suite shards validated a different source")
        print("GTI_RELEASE_SUITE_RESULT=" + json.dumps(summary, sort_keys=True))
        return
    if args.command == "test-targets":
        print(" ".join(test_targets(args.build, args.regex)))
        return
    if args.command == "shard":
        for path in (args.report, args.log):
            if path:
                Path(path).parent.mkdir(parents=True, exist_ok=True)
        log = Path(args.log).open("w") if args.log else None
        try:
            report = run_shard(root, Path(args.build).resolve(), args.shards, args.index,
                               root / args.durations, args.jobs, log,
                               str(Path(args.report).with_suffix(".xml")))
        finally:
            if log:
                log.close()
        Path(args.report).write_text(json.dumps(report, indent=2) + "\n")
        print("GTI_SHARD_RESULT=" + json.dumps({k: report[k] for k in ("platform", "index", "shards")}))
        return
    if args.command == "smoke":
        log = Path(args.log).open("w") if args.log else None
        try:
            executed = smoke(root, Path(args.build).resolve(), log)
        finally:
            if log:
                log.close()
        print("GTI_SMOKE_RESULT=" + json.dumps(executed))
        return
    if args.jobs < 1:
        raise ValueError("jobs must be positive")
    log = Path(args.log).open("w") if args.log else None
    try:
        junit = str(Path(args.report).with_suffix(".xml")) if args.report else None
        if args.command == "focused":
            plan = validate_plan(json.loads(Path(args.plan).read_text()))
            required = set(plan_tests(plan))
            report = run(root, Path(args.build).resolve(), required, False, plan["targets"], extra_checks(plan), args.jobs, log, junit)
        else:
            members = manifests(root, args.base, "HEAD")
            required = {SMOKE, *(name for member in members for name in plan_tests(member["plan"]))}
            report = run(root, Path(args.build).resolve(), required, True,
                         extra=[command for member in members for command in extra_checks(member["plan"])],
                         jobs=args.jobs, log=log, junit=junit)
    finally:
        if log:
            log.close()
    if args.report:
        Path(args.report).write_text(json.dumps(report, indent=2) + "\n")
    print("GTI_VALIDATION_RESULT=" + json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print("validation: " + str(error), file=sys.stderr)
        raise SystemExit(1)
