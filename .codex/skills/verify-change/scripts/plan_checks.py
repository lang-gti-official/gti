#!/usr/bin/env python3
"""Select GTI verification checks for a change from tests/ownership.toml.

The map names, for each source area, the CTest tests that own it (T1) and the
downstream phases and tooling that consume it (T2). This planner reports the
areas a change touches, turns their tests into runnable CTest commands, adds
the policy baseline and tooling checks, reports the highest risk, and lists
changed paths that no area covers. Its selection is a lower bound.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import json
from pathlib import Path
import re
import subprocess
import sys
import tomllib
from typing import Iterable

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "scripts"))
from validation_checks import SHARED, SMOKE, ci_mode  # noqa: E402

MAP_PATH = "tests/ownership.toml"
RISKS = ("shared", "system")
RISK_ORDER = {None: 0, "shared": 1, "system": 2}
TEST_NAME = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.-]*\Z")
# CTest -R uses CMake's regular expressions; families stay inside that subset.
FAMILY = re.compile(r"\^[A-Za-z0-9_.()|*+?$\[\]-]+\Z")
GLOB = re.compile(r"[*?\[]")
SYSTEM_ADVICE = ("set rollup=never / validate alone; do not run a near-full suite "
                 "locally: the merge queue runs the complete suite")
ADVISORY = (
    "The selection is a lower bound. Extend it from your own analysis of the "
    "changed invariant: name its affected consumers and add their tests. "
    "Classify the invariant, not the symptom; the complete suite is deferred to "
    "the merge queue."
)


@dataclass(frozen=True)
class Check:
    name: str
    command: str
    reason: str


@dataclass
class Area:
    area: str
    paths: list[str]
    owners: list[str]
    consumers: list[str]
    risk: str | None = None


@dataclass
class OwnershipMap:
    areas: list[Area]
    unowned: list[dict] = field(default_factory=list)
    configuration_dependent: list[str] = field(default_factory=list)


# ---------------------------------------------------------------------------
# The map
# ---------------------------------------------------------------------------

def schema_errors(document: dict) -> list[str]:
    """Return every schema violation in a parsed ownership map."""
    errors = []
    if document.get("schema") != 1:
        errors.append("schema must be 1")
    known = {"schema", "area", "unowned", "configuration_dependent"}
    errors += [f"unknown top-level key {key!r}" for key in sorted(set(document) - known)]
    areas = document.get("area")
    if not isinstance(areas, list) or not areas:
        errors.append("the map needs at least one [[area]]")
        areas = []
    seen = set()
    for index, entry in enumerate(areas):
        where = f"area #{index + 1}"
        if not isinstance(entry, dict):
            errors.append(f"{where} is not a table")
            continue
        name = entry.get("area")
        if not isinstance(name, str) or not re.fullmatch(r"[a-z0-9][a-z0-9-]*", name or ""):
            errors.append(f"{where}: area must be a short lowercase label")
        else:
            where = f"area {name!r}"
            if name in seen:
                errors.append(f"{where} is duplicated")
            seen.add(name)
        extra = set(entry) - {"area", "paths", "owners", "consumers", "risk"}
        errors += [f"{where}: unknown key {key!r}" for key in sorted(extra)]
        paths = entry.get("paths")
        if not isinstance(paths, list) or not paths or not all(
                isinstance(path, str) and path for path in paths):
            errors.append(f"{where}: paths must be a non-empty list of strings")
        else:
            errors += [f"{where}: {message}" for message in pattern_errors(paths)]
        for key in ("owners", "consumers"):
            tests = entry.get(key)
            if not isinstance(tests, list) or not all(isinstance(test, str) for test in tests):
                errors.append(f"{where}: {key} must be a list of strings")
                continue
            errors += [f"{where}: {key}: {message}" for message in test_errors(tests)]
        if "risk" in entry and entry["risk"] not in RISKS:
            errors.append(f"{where}: risk must be one of {', '.join(RISKS)}")
    for index, entry in enumerate(document.get("unowned", [])):
        where = f"unowned #{index + 1}"
        if not isinstance(entry, dict) or set(entry) != {"paths", "reason"}:
            errors.append(f"{where} needs exactly paths and reason")
            continue
        if not isinstance(entry["reason"], str) or not entry["reason"].strip():
            errors.append(f"{where}: reason must be non-empty")
        if not isinstance(entry["paths"], list) or not entry["paths"] or not all(
                isinstance(path, str) and path for path in entry["paths"]):
            errors.append(f"{where}: paths must be a non-empty list of strings")
        else:
            errors += [f"{where}: {message}" for message in pattern_errors(entry["paths"])]
    dependent = document.get("configuration_dependent", {"tests": [], "reason": "-"})
    if not isinstance(dependent, dict) or set(dependent) != {"tests", "reason"}:
        errors.append("configuration_dependent needs exactly tests and reason")
    elif not isinstance(dependent["tests"], list) or not all(
            isinstance(test, str) and TEST_NAME.match(test) for test in dependent["tests"]):
        errors.append("configuration_dependent.tests must be exact test names")
    return errors


def pattern_errors(paths: Iterable[str]) -> list[str]:
    errors = []
    for path in paths:
        if path.startswith("/") or path.startswith("./") or "\\" in path or ".." in path.split("/"):
            errors.append(f"path {path!r} must be repository-relative")
    duplicates = sorted({path for path in paths if list(paths).count(path) > 1})
    errors += [f"path {path!r} is listed twice" for path in duplicates]
    return errors


def test_errors(tests: list[str]) -> list[str]:
    errors = []
    for test in tests:
        if test.startswith("^"):
            if not FAMILY.match(test):
                errors.append(f"{test!r} uses characters outside the CTest regex subset")
                continue
            try:
                re.compile(test)
            except re.error as error:
                errors.append(f"{test!r} is not a valid regex: {error}")
        elif not TEST_NAME.match(test):
            errors.append(f"{test!r} is neither an exact test name nor an anchored regex")
    errors += [f"{test!r} is listed twice" for test in sorted({t for t in tests if tests.count(t) > 1})]
    return errors


def parse_map(document: dict) -> OwnershipMap:
    errors = schema_errors(document)
    if errors:
        raise ValueError("invalid ownership map:\n  " + "\n  ".join(errors))
    areas = [Area(entry["area"], list(entry["paths"]), list(entry["owners"]),
                  list(entry["consumers"]), entry.get("risk")) for entry in document["area"]]
    dependent = document.get("configuration_dependent", {}).get("tests", [])
    return OwnershipMap(areas, list(document.get("unowned", [])), list(dependent))


def load_map(path: Path) -> OwnershipMap:
    with path.open("rb") as handle:
        return parse_map(tomllib.load(handle))


def pattern_regex(pattern: str) -> re.Pattern[str]:
    """`dir/` is a prefix, globs use `*` within and `**` across segments."""
    if pattern.endswith("/"):
        return re.compile(re.escape(pattern) + ".*")
    if not GLOB.search(pattern):
        return re.compile(re.escape(pattern))
    out, index = "", 0
    while index < len(pattern):
        char = pattern[index]
        if pattern.startswith("**", index):
            out += ".*"
            index += 2
            continue
        if char == "*":
            out += "[^/]*"
        elif char == "?":
            out += "[^/]"
        elif char == "[":
            end = pattern.find("]", index)
            if end < 0:
                out += re.escape(char)
            else:
                out += pattern[index:end + 1]
                index = end
        else:
            out += re.escape(char)
        index += 1
    return re.compile(out)


def matches(pattern: str, path: str) -> bool:
    return pattern_regex(pattern).fullmatch(path) is not None


def areas_for(path: str, ownership: OwnershipMap) -> list[Area]:
    return [area for area in ownership.areas if any(matches(p, path) for p in area.paths)]


def unowned_for(path: str, ownership: OwnershipMap) -> dict | None:
    for entry in ownership.unowned:
        if any(matches(pattern, path) for pattern in entry["paths"]):
            return entry
    return None


def test_matches(entry: str, name: str) -> bool:
    if entry.startswith("^"):
        return re.match(entry, name) is not None
    return entry == name


def ctest_regex(entries: Iterable[str]) -> str:
    """One anchored CTest -R expression selecting every entry."""
    exact = sorted({entry for entry in entries if not entry.startswith("^")})
    families = sorted({entry for entry in entries if entry.startswith("^")})
    parts = []
    if exact:
        parts.append("^(" + "|".join(name.replace(".", "\\.") for name in exact) + ")$")
    parts.extend(families)
    return "|".join(parts)


def ctest_command(entries: Iterable[str], build: str = "build") -> str:
    return f"ctest --test-dir {build} --output-on-failure -R '{ctest_regex(entries)}'"


# ---------------------------------------------------------------------------
# Selection
# ---------------------------------------------------------------------------

def is_shared_floor(path: str) -> bool:
    return path.startswith(SHARED)


def select(paths: Iterable[str], ownership: OwnershipMap) -> dict:
    """Pure selection: paths and a parsed map in, areas/tests/risk out."""
    paths = sorted(set(paths))
    surfaces: dict[str, list[str]] = {}
    uncovered, unowned = [], {}
    tiers: dict[str, dict[str, list[str]]] = {"T1": {}, "T2": {}}
    risk = None
    risk_areas: list[str] = []
    for path in paths:
        matched = areas_for(path, ownership)
        if not matched:
            entry = unowned_for(path, ownership)
            if entry is None:
                uncovered.append(path)
            else:
                unowned[path] = entry["reason"]
        for area in matched:
            surfaces.setdefault(area.area, []).append(path)
            for test in area.owners:
                tiers["T1"].setdefault(test, [])
                if area.area not in tiers["T1"][test]:
                    tiers["T1"][test].append(area.area)
            for test in area.consumers:
                tiers["T2"].setdefault(test, [])
                if area.area not in tiers["T2"][test]:
                    tiers["T2"][test].append(area.area)
        path_risk = max((area.risk for area in matched), key=RISK_ORDER.__getitem__, default=None)
        if is_shared_floor(path) and RISK_ORDER[path_risk] < RISK_ORDER["shared"]:
            path_risk = "shared"
        if RISK_ORDER[path_risk] > RISK_ORDER[risk]:
            risk = path_risk
    for area in ownership.areas:
        if area.area in surfaces and area.risk == risk and risk is not None:
            risk_areas.append(area.area)
    # A test selected as an owner is not repeated as a consumer, and the
    # baseline smoke test is reported once as the baseline.
    for test in list(tiers["T2"]):
        if test in tiers["T1"]:
            del tiers["T2"][test]
    for tier in tiers.values():
        tier.pop(SMOKE, None)
    return {
        "surfaces": surfaces,
        "selection": {tier: [{"test": test, "areas": sorted(areas)} for test, areas in sorted(values.items())]
                      for tier, values in tiers.items()},
        "risk": risk,
        "risk_areas": risk_areas,
        "uncovered": uncovered,
        "unowned": unowned,
    }


def registered_tests(repo: Path) -> tuple[dict[str, str], dict[str, str]]:
    """Textual add_executable and add_test blocks from the test CMake files."""
    executables: dict[str, str] = {}
    tests: dict[str, str] = {}
    for relative in ("tests/CMakeLists.txt", "tests/TestThroughput.cmake"):
        path = repo / relative
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8")
        for match in re.finditer(r'add_executable\(\s*"?([A-Za-z0-9_]+)"?([^)]*)\)', text):
            executables[match.group(1)] = executables.get(match.group(1), "") + match.group(2)
        for match in re.finditer(r'add_test\(\s*NAME\s+"?([A-Za-z0-9_.-]+)"?([^)]*)\)', text):
            tests[match.group(1)] = tests.get(match.group(1), "") + match.group(2)
    return executables, tests


def regression_tests(paths: Iterable[str], executables: dict[str, str],
                     tests: dict[str, str]) -> dict[str, list[str]]:
    """T0: registered tests whose CMake registration names a changed test file."""
    selected: dict[str, list[str]] = {}
    for path in paths:
        if not path.startswith("tests/") or path in {"tests/CMakeLists.txt", "tests/TestThroughput.cmake"}:
            continue
        name = Path(path).name
        token = re.compile(r"(?<![A-Za-z0-9_.-])" + re.escape(name) + r"(?![A-Za-z0-9_.-])")
        targets = [target for target, body in executables.items() if token.search(body)]
        for test, body in tests.items():
            if token.search(body) or any(
                    re.search(r"(?<![A-Za-z0-9_])" + re.escape(target) + r"(?![A-Za-z0-9_])", body)
                    for target in targets):
                selected.setdefault(test, []).append(path)
    return dict(sorted(selected.items()))


def under(path: str, *prefixes: str) -> bool:
    return any(path == prefix.rstrip("/") or path.startswith(prefix) for prefix in prefixes)


def candidate_checks(paths: list[str], selection: dict, regressions: dict[str, list[str]],
                     build: str = "build") -> list[Check]:
    checks = [Check("diff", "git diff --check", "reject whitespace and patch-format errors")]
    automation_only = ci_mode({}, paths) == "automation"
    implementation = bool(paths) and not automation_only
    first_party_cpp = any(
        Path(path).suffix in {".c", ".cc", ".cpp", ".h", ".hpp", ".mm"}
        and not under(path, "tree-sitter-gti/src/parser.c", "build/", "build-")
        for path in paths
    )
    if first_party_cpp:
        checks.append(Check("format", "python3 scripts/clang_format.py --check",
                            "first-party C or C++ changed"))
    if implementation:
        checks.append(Check(
            "build", f"python3 scripts/build_window.py --kind build -- cmake --build {build} -j4",
            "the selected tests need a current optimized build; a full build holds a build-window slot"))
        checks.append(Check(
            "baseline", ctest_command([SMOKE], build),
            "every implementation change runs compiler_pipeline"))
        checks.append(Check(
            "native-smoke", f"python3 scripts/validation_checks.py smoke --build {build}",
            "every implementation change runs the native O0/O3 smoke"))
    if regressions:
        checks.append(Check(
            "T0", ctest_command(regressions, build),
            "registered tests for the changed regression or fixture files: "
            + ", ".join(sorted({p for values in regressions.values() for p in values}))))
    for tier, label in (("T1", "owning"), ("T2", "consumer")):
        rows = selection["selection"][tier]
        if rows:
            areas = sorted({area for row in rows for area in row["areas"]})
            checks.append(Check(tier, ctest_command([row["test"] for row in rows], build),
                                f"{label} tests from map areas: {', '.join(areas)}"))
    if any(under(path, "tree-sitter-gti/") for path in paths):
        checks.extend([
            Check("tree-sitter-generate", "npm --prefix tree-sitter-gti run generate",
                  "Tree-sitter grammar or generated source changed"),
            Check("tree-sitter-tests", "npm --prefix tree-sitter-gti test",
                  "Tree-sitter corpus changed"),
            Check("tree-sitter-highlights", "npm --prefix tree-sitter-gti run test:highlights",
                  "Tree-sitter highlighting may be affected"),
            Check("tree-sitter-shipped", "npm --prefix tree-sitter-gti run test:shipped",
                  "shipped GTI sources must continue to parse"),
            Check("tree-sitter-generated-diff", "git diff --exit-code -- tree-sitter-gti/src",
                  "generated parser must match the grammar"),
        ])
    if any(path in {
        "scripts/agent_workflow.py", "scripts/validation_batch.py", "scripts/validation_checks.py",
        "scripts/build_window.py", "tests/agent_workflow_test.py", "tests/build_window_test.py",
    } for path in paths):
        checks.append(Check("agent-workflow", "python3 tests/agent_workflow_test.py",
                            "workflow/batch lifecycle, CI classification and evidence integrity changed"))
    if any(path == MAP_PATH or under(path, ".codex/skills/verify-change/") or path in {
        "tests/ownership_map_test.py", "tests/plan_checks_test.py"} for path in paths):
        checks.append(Check("ownership-map", "python3 tests/ownership_map_test.py",
                            "the ownership map, its validator or the planner changed"))
        checks.append(Check("planner", "python3 tests/plan_checks_test.py",
                            "the ownership map, its validator or the planner changed"))
    seen: set[str] = set()
    unique = []
    for check in checks:
        if check.command not in seen:
            seen.add(check.command)
            unique.append(check)
    return unique


def plan(paths: list[str], ownership: OwnershipMap, repo: Path, build: str = "build") -> dict:
    selection = select(paths, ownership)
    executables, tests = registered_tests(repo)
    regressions = regression_tests(paths, executables, tests)
    checks = candidate_checks(paths, selection, regressions, build)
    risk = selection["risk"]
    return {
        "changed_files": paths,
        "surfaces": selection["surfaces"],
        "selection": dict(selection["selection"],
                          T0=[{"test": test, "paths": values} for test, values in regressions.items()]),
        "risk": risk,
        "risk_areas": selection["risk_areas"],
        "risk_advice": SYSTEM_ADVICE if risk == "system" else (
            "T2 floor: include the subsystem-wide consumer sweeps" if risk == "shared" else
            "no shared or system area; review can still raise the tier"),
        "risk_floor": "T2" if risk else "review required",
        "uncovered": selection["uncovered"],
        "unowned": selection["unowned"],
        "candidate_checks": [check.__dict__ for check in checks],
        "ci_mode": ci_mode({}, paths),
        "advisory": ADVISORY,
    }


# ---------------------------------------------------------------------------
# Git and the command line
# ---------------------------------------------------------------------------

def git(repo: Path, *args: str) -> bytes:
    result = subprocess.run(["git", *args], cwd=repo, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, check=False)
    if result.returncode != 0:
        message = result.stderr.decode(errors="replace").strip()
        raise RuntimeError(message or f"git {' '.join(args)} failed")
    return result.stdout


def repository_root(start: Path) -> Path:
    return Path(git(start, "rev-parse", "--show-toplevel").decode().strip()).resolve()


def nul_paths(output: bytes) -> set[str]:
    return {item.decode(errors="surrogateescape") for item in output.split(b"\0") if item}


def changed_paths(repo: Path, base: str | None, staged: bool,
                  include_untracked: bool) -> tuple[list[str], str]:
    if staged:
        tracked = nul_paths(git(repo, "diff", "--cached", "--name-only",
                                "--diff-filter=ACMRTUXB", "-z", "HEAD"))
        scope = "staged index against HEAD"
    elif base:
        merge_base = git(repo, "merge-base", base, "HEAD").decode().strip()
        tracked = nul_paths(git(repo, "diff", "--name-only", "--diff-filter=ACMRTUXB", "-z", merge_base))
        scope = f"merge base {merge_base} of {base} and HEAD through the working tree"
    else:
        tracked = nul_paths(git(repo, "diff", "--name-only", "--diff-filter=ACMRTUXB", "-z", "HEAD"))
        scope = "HEAD through the working tree"
    if include_untracked and not staged:
        tracked.update(nul_paths(git(repo, "ls-files", "--others", "--exclude-standard", "-z")))
    return sorted(tracked), scope


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Select GTI verification checks for a change from tests/ownership.toml.")
    scope = parser.add_mutually_exclusive_group()
    scope.add_argument("--base",
                       help="Compare from the merge base of this ref through the current working tree.")
    scope.add_argument("--staged", action="store_true",
                       help="Inspect only changes staged in the index against HEAD.")
    scope.add_argument("--paths", nargs="+", metavar="PATH",
                       help="Plan for these repository-relative paths instead of a Git diff.")
    parser.add_argument("--no-untracked", action="store_true",
                        help="Exclude untracked files from the change surface.")
    parser.add_argument("--format", choices=("text", "json"), default="text",
                        help="Output format (default: text).")
    return parser.parse_args(argv)


def print_text(repo: Path, scope: str, payload: dict) -> None:
    paths = payload["changed_files"]
    print(f"Repository: {repo}")
    print(f"Scope: {scope}")
    print(f"Changed files: {len(paths)}")
    if not paths:
        print("  (none)")
    for name, values in payload["surfaces"].items():
        print(f"\n{name} ({len(values)}):")
        for path in values:
            print(f"  - {path}")
    if payload["unowned"]:
        print("\nUnowned by policy:")
        for path, reason in payload["unowned"].items():
            print(f"  - {path}: {reason}")
    if payload["uncovered"]:
        print("\nUncovered (no ownership-map area; extend tests/ownership.toml):")
        for path in payload["uncovered"]:
            print(f"  - {path}")
    risk = payload["risk"] or "none"
    areas = f" ({', '.join(payload['risk_areas'])})" if payload["risk_areas"] else ""
    print(f"\nRisk: {risk}{areas} - {payload['risk_advice']}")
    print("\nCandidate checks:")
    for check in payload["candidate_checks"]:
        print(f"  [{check['name']}] {check['command']}")
        print(f"    {check['reason']}")
    for tier in ("T0", "T1", "T2"):
        rows = payload["selection"][tier]
        if not rows:
            continue
        print(f"\n{tier} selection:")
        for row in rows:
            source = row.get("areas") or row.get("paths")
            print(f"  {row['test']}  <- {', '.join(source)}")
    print(f"\nAdvisory: {payload['advisory']}")


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        repo = repository_root(Path.cwd())
        if args.paths:
            paths, scope = sorted(set(args.paths)), "explicit paths"
        else:
            paths, scope = changed_paths(repo, args.base, args.staged, not args.no_untracked)
        ownership = load_map(repo / MAP_PATH)
    except (RuntimeError, OSError, ValueError, tomllib.TOMLDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    payload = dict(plan(paths, ownership, repo), repository=str(repo), scope=scope)
    if args.format == "json":
        json.dump(payload, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
    else:
        print_text(repo, scope, payload)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
