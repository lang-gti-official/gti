#!/usr/bin/env python3
"""Audit LLVM ledger claims against registered, runnable evidence profiles."""

from copy import deepcopy
import json
from pathlib import Path
import subprocess
import sys
import tempfile

import llvm_class_execution_test as class_execution
from llvm_evidence_manifest import validate_manifest
import llvm_place_execution_test as place_execution
import llvm_scalar_execution_test as scalar_execution


def run(command: list[str], *, timeout: int = 120) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, capture_output=True, text=True, timeout=timeout)


def ctest_names(ctest: str, build: Path) -> set[str]:
    result = run([ctest, "--test-dir", str(build), "--show-only=json-v1"])
    if result.returncode != 0:
        raise AssertionError(result.stderr)
    inventory = json.loads(result.stdout)
    return {test["name"] for test in inventory["tests"]}


def ledger(evidence: Path) -> dict:
    result = run([str(evidence), "--ledger"])
    if result.returncode != 0:
        raise AssertionError(result.stderr)
    return json.loads(result.stdout)


def profile_sources(root: Path) -> dict[str, tuple[dict[str, str], dict[str, tuple[str, ...]]]]:
    modules = {
        "llvm_scalar_execution": scalar_execution,
        "llvm_place_execution": place_execution,
        "llvm_class_execution": class_execution,
    }
    sources = {}
    for test, module in modules.items():
        cases = module.evidence_cases(root)
        profiles = module.evidence_profiles(root)
        declared = module.evidence_manifest(root)
        validate_manifest(declared, test, cases, profiles)
        for name, record in declared["sources"].items():
            origin = record["origin"]
            if origin == "inline":
                continue
            path = (root / origin).resolve()
            if root not in path.parents or path.read_text(encoding="utf-8") != cases[name]:
                raise AssertionError(f"{test}:{name}: source origin is not exact")
        listed = run([sys.executable, module.__file__, "--list-evidence", str(root)])
        if listed.returncode != 0:
            raise AssertionError(listed.stderr)
        if json.loads(listed.stdout) != declared:
            raise AssertionError(f"{test}: --list-evidence diverges from runnable cases")
        sources[test] = (cases, profiles)
    return sources


def manifest_mutation_controls(root: Path) -> None:
    test = "llvm_scalar_execution"
    cases = scalar_execution.evidence_cases(root)
    profiles = scalar_execution.evidence_profiles(root)
    manifest = scalar_execution.evidence_manifest(root)

    unknown = deepcopy(manifest)
    unknown["unexpected"] = True
    try:
        validate_manifest(unknown, test, cases, profiles)
        raise AssertionError("unknown manifest field was accepted")
    except ValueError:
        pass

    stale = deepcopy(manifest)
    stale["sources"]["scalars"]["sha256"] = "0" * 64
    try:
        validate_manifest(stale, test, cases, profiles)
        raise AssertionError("stale manifest source hash was accepted")
    except ValueError:
        pass

    dangling = deepcopy(manifest)
    dangling["profiles"]["scalars"]["sources"] = ["missing-source"]
    try:
        validate_manifest(dangling, test, cases, profiles)
        raise AssertionError("dangling manifest source was accepted")
    except ValueError:
        pass


def observed_profiles(report: dict, evidence: Path, root: Path) -> dict[tuple[str, str], set[str]]:
    sources = profile_sources(root)
    requested = {
        (row["acceptance"]["test"], row["acceptance"]["assertion"])
        for row in report["rows"]
        if row["state"] == "implemented" and row["acceptance"]["test"]
    }
    observations: dict[tuple[str, str], set[str]] = {}
    with tempfile.TemporaryDirectory(prefix="gti-llvm-evidence-") as raw:
        directory = Path(raw)
        for test, profile in sorted(requested):
            if test not in sources:
                continue
            cases, profiles = sources[test]
            if profile not in profiles:
                continue
            keys: set[str] = set()
            for case in profiles[profile]:
                source = directory / f"{test}-{profile}-{case}.gti"
                source.write_text(cases[case], encoding="utf-8")
                result = run([str(evidence), "--probe", str(root), str(source)])
                if result.returncode != 0:
                    raise AssertionError(
                        f"coverage probe failed for {test}:{profile}:{case}:\n"
                        f"{result.stderr}")
                keys.update(line for line in result.stdout.splitlines() if line)
            observations[(test, profile)] = keys
    return observations


def validate(report: dict, registered: set[str],
             observations: dict[tuple[str, str], set[str]]) -> list[str]:
    errors: list[str] = []
    keys: set[str] = set()
    if report.get("schema") != 1:
        errors.append("unsupported report schema")
    source = report.get("source", {})
    exact_expected = source.get("state") == "clean" and len(source.get("commit", "")) == 40
    if source.get("exact") != exact_expected:
        errors.append("source exactness does not match commit/state")
    build = report.get("build", {})
    for field in ("configuration", "system", "processor", "compiler", "llvm"):
        if not build.get(field) or build[field] == "unknown":
            errors.append(f"build identity is missing {field}")
    for row in report.get("rows", []):
        key = row["key"]
        if key in keys:
            errors.append(f"duplicate ledger key {key}")
        keys.add(key)
        acceptance = row["acceptance"]
        boundary = row["boundary"]
        if acceptance.get("observed") not in {"passed", "failed", "unrun", "unavailable"}:
            errors.append(f"{key}: invalid acceptance observation")
        if boundary.get("observed") not in {"passed", "failed", "unrun", "unavailable"}:
            errors.append(f"{key}: invalid boundary observation")
        if boundary.get("test") not in registered:
            errors.append(f"{key}: boundary test is unavailable: {boundary.get('test')}")
        state = row["state"]
        if state == "implemented":
            has_evidence = bool(acceptance.get("test") and acceptance.get("assertion"))
            has_gap = bool(acceptance.get("gap"))
            if has_evidence == has_gap:
                errors.append(f"{key}: implementation must have evidence or one explicit gap")
            if has_evidence:
                test = acceptance["test"]
                assertion = acceptance["assertion"]
                if test not in registered:
                    errors.append(f"{key}: acceptance test is unavailable: {test}")
                profile = observations.get((test, assertion))
                if profile is None:
                    errors.append(f"{key}: acceptance assertion is unavailable: {test}:{assertion}")
                elif key not in profile:
                    errors.append(f"{key}: {test}:{assertion} does not exercise the ledger row")
        elif state == "pending":
            if acceptance.get("test") or acceptance.get("assertion") or acceptance.get("gap"):
                errors.append(f"{key}: pending row carries synthetic acceptance")
            if acceptance.get("observed") == "passed":
                errors.append(f"{key}: pending row cannot be observed passed")
        elif state == "verified-nonexecuting":
            if (acceptance.get("test"), acceptance.get("assertion")) != (
                    "llvm_admission", "ledger.nonexecuting"):
                errors.append(f"{key}: nonexecuting structural evidence is missing")
            if acceptance.get("test") not in registered:
                errors.append(f"{key}: nonexecuting evidence test is unavailable")
        else:
            errors.append(f"{key}: unknown coverage state {state}")
    return errors


def mutation_controls(report: dict, registered: set[str],
                      observations: dict[tuple[str, str], set[str]]) -> None:
    implemented = next(row for row in report["rows"]
                       if row["state"] == "implemented" and row["acceptance"]["test"])
    stale_test = deepcopy(report)
    target = next(row for row in stale_test["rows"] if row["key"] == implemented["key"])
    target["acceptance"]["test"] = "llvm_missing_execution_gate"
    assert any("acceptance test is unavailable" in error
               for error in validate(stale_test, registered, observations))

    stale_assertion = deepcopy(report)
    target = next(row for row in stale_assertion["rows"] if row["key"] == implemented["key"])
    target["acceptance"]["assertion"] = "missing-profile"
    assert any("acceptance assertion is unavailable" in error
               for error in validate(stale_assertion, registered, observations))

    pending_pass = deepcopy(report)
    target = next(row for row in pending_pass["rows"] if row["state"] == "pending")
    target["acceptance"]["observed"] = "passed"
    assert any("pending row cannot be observed passed" in error
               for error in validate(pending_pass, registered, observations))

    unavailable = set(registered)
    unavailable.remove(implemented["acceptance"]["test"])
    assert any("acceptance test is unavailable" in error
               for error in validate(report, unavailable, observations))


def main() -> None:
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: llvm_coverage_evidence_test.py <evidence-helper> <ctest> "
            "<build-dir> <repo-root>")
    evidence = Path(sys.argv[1]).resolve()
    ctest = sys.argv[2]
    build = Path(sys.argv[3]).resolve()
    root = Path(sys.argv[4]).resolve()
    report = ledger(evidence)
    registered = ctest_names(ctest, build)
    manifest_mutation_controls(root)
    observations = observed_profiles(report, evidence, root)
    errors = validate(report, registered, observations)
    if errors:
        raise AssertionError("LLVM coverage evidence audit failed:\n" + "\n".join(errors))
    mutation_controls(report, registered, observations)
    counts = {state: sum(row["state"] == state for row in report["rows"])
              for state in ("implemented", "pending", "verified-nonexecuting")}
    gaps = sum(bool(row["acceptance"]["gap"]) for row in report["rows"])
    print(json.dumps({"source": report["source"], "build": report["build"],
                      "rows": counts, "implemented_evidence_gaps": gaps,
                      "promotion_ready": counts["pending"] == 0 and gaps == 0},
                     sort_keys=True))


if __name__ == "__main__":
    main()
