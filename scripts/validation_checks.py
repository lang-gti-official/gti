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
NAME = re.compile(r"[A-Za-z0-9_.-]+\Z")
SHA = re.compile(r"[0-9a-f]{40}\Z")
# This is a conservative floor, not a substitute for reviewing the invariant.
SHARED = ("include/gti/semantic", "include/gti/hir", "include/gti/mir",
          "src/compiler/semantic_ownership", "src/compiler/semantic_type",
          "src/compiler/semantic_generic", "src/compiler/mir_", "src/compiler/hir_",
          "src/compiler/cpp_", "src/compiler/optimization/", "runtime/")
AUTOMATION = ("docs/", ".codex/", ".claude/", ".agents/")
AUTOMATION_FILES = {"AGENTS.md", "CLAUDE.md", "README.md", ".github/workflows/ci.yml"}


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
              "unaffected", "targets", "reproduction", "platforms", "additional_checks"}
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
    return plan


def commands(value, allow_empty=False):
    if not isinstance(value, list) or (not value and not allow_empty) or any(
            not isinstance(row, list) or not row or any(not isinstance(arg, str) or not arg for arg in row)
            for row in value):
        raise ValueError("Checks must be argv arrays, never shell text")
    return value


def plan_tests(plan):
    return sorted({SMOKE, *(name for key in ("regressions", "owning", "cross_phase", "broader")
                           for name in plan.get(key, []))})


def component_body(body, record=None):
    body = MARKER.sub("", body or "").rstrip()
    return body + ("\n\n<!-- gti-validation-component: " + json.dumps(record, sort_keys=True) + " -->" if record else "")


def ci_mode(event, paths):
    pr = event.get("pull_request")
    if pr:
        if MARKER.search(pr.get("body") or ""):
            return "component"
        if pr.get("draft"):
            return "draft"
    # Compiler tests, CMake and packaged scripts are deliberately not inferred
    # safe just because their filename includes 'test' or 'workflow'.
    automation_scripts = {"agent_workflow.py", "validation_batch.py", "validation_checks.py",
                          "preview_workflow.py", "preview_submit.py", "build_window.py", "bounded_log.py"}
    automation_tests = {name.removesuffix(".py") + "_test.py" for name in automation_scripts}
    automation_tests |= {"validation_ci_test.py", "release_version_test.py", "ci_resources_test.py"}
    if paths and all(path in AUTOMATION_FILES or path.startswith(AUTOMATION) or
                     path in {"scripts/" + name for name in automation_scripts} or
                     path in {"tests/" + name for name in automation_tests} for path in paths):
        return "automation"
    return "full"


def admission(mode, results):
    if mode in {"draft", "component"}:
        raise ValueError("Awaiting integration: draft/component PRs cannot merge directly; prepare a validated batch or withdraw admission")
    required = {"validation-plan", "formatting", "release-version", "tree-sitter", "automation-tests"}
    if mode == "full":
        required |= {"build-and-test", "compiler-sanitizers"}
    elif mode != "automation":
        raise ValueError("Unknown CI validation mode")
    missing = sorted(key for key in required if results.get(key, {}).get("result") != "success")
    if missing:
        raise ValueError("Required validation did not pass: " + ", ".join(missing))


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
        "owners": """#include <std/string>
#include <std/vector>
int main() {
  mut std::vector<std::string> values{};
  values.push_back(std::string("smoke"));
  std::string copy = values[0].clone();
  return int(copy.size()) == 5 ? 0 : 1;
}
""",
    }
    env = dict(os.environ, GTI_STDLIB_PATH=str(root / "stdlib"))
    with tempfile.TemporaryDirectory(prefix="gti-compiler-smoke-") as directory:
        for name, source in programs.items():
            path = Path(directory) / (name + ".gti")
            path.write_text(source)
            for level in ("0", "3"):
                output = Path(directory) / (name + "-O" + level)
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
        try:
            git(root, "cat-file", "-e", base + ":" + path)
        except subprocess.CalledProcessError:
            pass
        else:
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
                   check=True, stdout=log, stderr=subprocess.STDOUT)
    selected = [] if full else ["-R", "^(?:" + "|".join(re.escape(name) for name in sorted(required)) + ")$"]
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
        admission(args.mode, json.loads(args.results))
        return
    if args.jobs < 1:
        raise ValueError("jobs must be positive")
    log = Path(args.log).open("w") if args.log else None
    try:
        junit = str(Path(args.report).with_suffix(".xml")) if args.report else None
        if args.command == "focused":
            plan = validate_plan(json.loads(Path(args.plan).read_text()))
            required = set(plan_tests(plan))
            report = run(root, Path(args.build).resolve(), required, False, plan["targets"], plan.get("additional_checks", []), args.jobs, log, junit)
        else:
            members = manifests(root, args.base, "HEAD")
            required = {SMOKE, *(name for member in members for name in plan_tests(member["plan"]))}
            report = run(root, Path(args.build).resolve(), required, True,
                         extra=[command for member in members for command in member["plan"].get("additional_checks", [])],
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
