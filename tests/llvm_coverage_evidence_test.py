#!/usr/bin/env python3
"""Audit LLVM ledger claims against registered, runnable evidence profiles."""

from copy import deepcopy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from llvm_evidence_manifest import EXECUTION_PROFILES, build_manifest, validate_manifest


def run(command: list[str], *, timeout: int = 120) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, capture_output=True, text=True, timeout=timeout)


def ctest_inventory(ctest: str, build: Path) -> dict[str, dict]:
    result = run([ctest, "--test-dir", str(build), "--show-only=json-v1"])
    if result.returncode != 0:
        raise AssertionError(result.stderr)
    inventory = json.loads(result.stdout)
    tests = {test["name"]: test for test in inventory["tests"]}
    if len(tests) != len(inventory["tests"]):
        raise AssertionError("CTest inventory contains duplicate test names")
    return tests


def ledger(evidence: Path) -> dict:
    result = run([str(evidence), "--ledger"])
    if result.returncode != 0:
        raise AssertionError(result.stderr)
    return json.loads(result.stdout)


def repository_source(root: Path) -> dict:
    head = run(["git", "-C", str(root), "rev-parse", "HEAD"])
    status = run(["git", "-C", str(root), "status", "--porcelain=v1",
                  "--untracked-files=all"])
    if head.returncode != 0 or status.returncode != 0:
        raise AssertionError("cannot establish LLVM evidence repository identity")
    commit = head.stdout.strip()
    state = "dirty" if status.stdout else "clean"
    return {"commit": commit, "state": state,
            "exact": state == "clean" and len(commit) == 40}


def repository_source_errors(report: dict, observed: dict) -> list[str]:
    declared = report.get("source", {})
    return ([] if declared == observed else [
        "compiled LLVM evidence source identity does not match the repository"
    ])


def load_registry(registry: Path, root: Path,
                  registered: dict[str, dict]) -> dict:
    modules = {}
    for index, line in enumerate(registry.read_text(encoding="utf-8").splitlines(), 1):
        fields = line.split("\t")
        if len(fields) != 2 or not all(fields):
            raise AssertionError(f"malformed LLVM evidence registry line {index}")
        test, raw_script = fields
        if test in modules:
            raise AssertionError(f"duplicate LLVM evidence registry test {test}")
        if test not in registered:
            raise AssertionError(f"unregistered LLVM evidence test {test}")
        script = Path(raw_script).resolve()
        if root not in script.parents or script.suffix != ".py" or not script.is_file():
            raise AssertionError(f"invalid LLVM evidence script for {test}: {script}")
        command_scripts = {
            Path(argument).resolve()
            for argument in registered[test].get("command", [])
            if isinstance(argument, str) and argument.endswith(".py")
        }
        if script not in command_scripts:
            raise AssertionError(
                f"registered LLVM evidence test {test} does not execute {script}")
        module_name = f"gti_llvm_evidence_{index}"
        spec = importlib.util.spec_from_file_location(module_name, script)
        if spec is None or spec.loader is None:
            raise AssertionError(f"cannot load LLVM evidence script for {test}")
        module = importlib.util.module_from_spec(spec)
        sys.modules[module_name] = module
        try:
            spec.loader.exec_module(module)
        except Exception:
            sys.modules.pop(module_name, None)
            raise
        for function in ("evidence_cases", "evidence_profiles", "evidence_manifest"):
            if not callable(getattr(module, function, None)):
                raise AssertionError(f"{test}: missing {function}")
        modules[test] = module
    if not modules:
        raise AssertionError("LLVM evidence registry is empty")
    return modules


def profile_sources(root: Path, modules: dict) -> dict:
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
        sources[test] = (cases, profiles, declared["profiles"])
    return sources


def manifest_mutation_controls(root: Path, modules: dict) -> None:
    test = "llvm_scalar_execution"
    scalar_execution = modules[test]
    cases = scalar_execution.evidence_cases(root)
    profiles = scalar_execution.evidence_profiles(root)
    manifest = scalar_execution.evidence_manifest(root)

    unsupported = deepcopy(manifest)
    unsupported["schema"] = "gti.llvm.execution-evidence/unsupported"
    try:
        validate_manifest(unsupported, test, cases, profiles)
        raise AssertionError("unsupported manifest schema was accepted")
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

    unknown = deepcopy(manifest)
    unknown["unexpected"] = True
    try:
        validate_manifest(unknown, test, cases, profiles)
        raise AssertionError("unknown manifest field was accepted")
    except ValueError:
        pass


    legacy = deepcopy(manifest)
    legacy["schema"] = "gti.llvm.execution-evidence/v1"
    for record in legacy["profiles"].values():
        record.pop("execution-profile")
    try:
        validate_manifest(legacy, test, cases, profiles)
        raise AssertionError("v1 execution metadata was silently reinterpreted")
    except ValueError:
        pass

    for mode in ("sequential", "unknown", 0):
        invalid = deepcopy(manifest)
        invalid["profiles"]["scalars"]["execution-profile"] = mode
        try:
            validate_manifest(invalid, test, cases, profiles)
            raise AssertionError("invalid provider execution profile was accepted")
        except ValueError:
            pass
        try:
            build_manifest(test, cases, profiles,
                           execution_profiles={"scalars": mode})
            raise AssertionError("invalid execution-profile override was accepted")
        except ValueError:
            pass
    missing = deepcopy(manifest)
    missing["profiles"]["scalars"].pop("execution-profile")
    try:
        validate_manifest(missing, test, cases, profiles)
        raise AssertionError("missing execution-profile metadata was accepted")
    except ValueError:
        pass
    try:
        build_manifest(test, cases, profiles,
                       execution_profiles={"unknown-profile": "concurrent"})
        raise AssertionError("unknown execution-profile override was accepted")
    except ValueError:
        pass


def registry_mutation_controls(registry: Path, root: Path,
                               registered: dict[str, dict]) -> None:
    lines = registry.read_text(encoding="utf-8").splitlines()
    if not lines:
        raise AssertionError("cannot mutate an empty LLVM evidence registry")
    test, script = lines[0].split("\t")

    def expect_failure(contents: str, expected: str,
                       available: dict[str, dict] = registered) -> None:
        with tempfile.NamedTemporaryFile(
                mode="w", encoding="utf-8", suffix=".tsv") as mutated:
            mutated.write(contents)
            mutated.flush()
            try:
                load_registry(Path(mutated.name), root, available)
            except AssertionError as error:
                if expected not in str(error):
                    raise AssertionError(
                        f"unexpected registry rejection: {error}") from error
            else:
                raise AssertionError(
                    f"LLVM evidence registry accepted mutation: {expected}")

    expect_failure("malformed\n", "malformed LLVM evidence registry")
    expect_failure(f"{lines[0]}\n{lines[0]}\n",
                   "duplicate LLVM evidence registry test")
    expect_failure(f"llvm_unregistered_execution\t{script}\n",
                   "unregistered LLVM evidence test")
    expect_failure(f"{test}\t{root / 'tests' / 'missing_evidence.py'}\n",
                   "invalid LLVM evidence script")
    unrelated = root / "tests" / "backend_execution.py"
    expect_failure(f"{test}\t{unrelated}\n", "does not execute")
    matching = deepcopy(registered)
    matching[test]["command"] = [
        str(unrelated) if Path(argument).resolve() == Path(script).resolve()
        else argument
        for argument in matching[test].get("command", [])
    ]
    expect_failure(f"{test}\t{unrelated}\n",
                   f"{test}: missing evidence_cases", matching)


def observed_profiles(report: dict, evidence: Path, root: Path,
                      modules: dict) -> dict[tuple[str, str], dict]:
    sources = profile_sources(root, modules)
    requested = {
        (row["acceptance"]["test"], row["acceptance"]["assertion"])
        for row in report["rows"]
        if row["state"] == "implemented" and row["acceptance"]["test"]
    }
    observations: dict[tuple[str, str], dict] = {}
    with tempfile.TemporaryDirectory(prefix="gti-llvm-evidence-") as raw:
        directory = Path(raw)
        for test, profile in sorted(requested):
            if test not in sources:
                continue
            cases, profiles, metadata = sources[test]
            if profile not in profiles:
                continue
            mode = metadata[profile]["execution-profile"]
            keys: set[str] = set()
            for case in profiles[profile]:
                source = directory / f"{test}-{profile}-{case}.gti"
                source.write_text(cases[case], encoding="utf-8")
                result = run([str(evidence), "--probe", str(root), str(source),
                              "--execution-profile", mode])
                if result.returncode != 0:
                    raise AssertionError(
                        f"coverage probe failed for {test}:{profile}:{case}:\n"
                        f"{result.stderr}")
                keys.update(line for line in result.stdout.splitlines() if line)
            observations[(test, profile)] = {
                "keys": keys, "execution-profile": mode,
            }
    return observations


def validate(report: dict, registered: set[str],
             observations: dict[tuple[str, str], dict]) -> list[str]:
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
                elif (set(profile) != {"keys", "execution-profile"}
                      or not isinstance(profile["execution-profile"], str)
                      or profile["execution-profile"] not in EXECUTION_PROFILES):
                    errors.append(f"{key}: invalid execution-profile observation")
                elif ("ExecutionProfile." +
                      ("Concurrent" if profile["execution-profile"] == "concurrent"
                       else "SingleThreaded")) not in profile["keys"]:
                    errors.append(f"{key}: observed execution profile differs from its target")
                elif key not in profile["keys"]:
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
                      observations: dict[tuple[str, str], dict]) -> None:
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


    forged = deepcopy(observations)
    selected = (implemented["acceptance"]["test"],
                implemented["acceptance"]["assertion"])
    forged[selected]["execution-profile"] = "unknown"
    assert any("invalid execution-profile observation" in error
               for error in validate(report, registered, forged))
    mismatched = deepcopy(observations)
    mode = mismatched[selected]["execution-profile"]
    mismatched[selected]["execution-profile"] = (
        "single-threaded" if mode == "concurrent" else "concurrent")
    assert any("observed execution profile differs from its target" in error
               for error in validate(report, registered, mismatched))


def readiness_summary(report: dict) -> dict:
    states = ("implemented", "pending", "verified-nonexecuting")
    statuses = ("passed", "failed", "unrun", "unavailable")
    counts = {state: sum(row["state"] == state for row in report["rows"])
              for state in states}
    gaps = sum(bool(row["acceptance"]["gap"]) for row in report["rows"])
    required_acceptance = [
        row["acceptance"] for row in report["rows"]
        if row["state"] != "pending" and row["acceptance"]["test"]
    ]
    boundaries = [row["boundary"] for row in report["rows"]]
    observed = {
        "acceptance": {
            status: sum(item["observed"] == status
                        for item in required_acceptance)
            for status in statuses
        },
        "boundary": {
            status: sum(item["observed"] == status for item in boundaries)
            for status in statuses
        },
    }
    structural_ready = counts["pending"] == 0 and gaps == 0
    execution_ready = (bool(required_acceptance) and bool(boundaries) and
                       all(item["observed"] == "passed"
                           for item in required_acceptance) and
                       all(item["observed"] == "passed"
                           for item in boundaries))
    return {
        "source": report["source"],
        "build": report["build"],
        "rows": counts,
        "implemented_evidence_gaps": gaps,
        "observations": observed,
        "structural_ready": structural_ready,
        "execution_ready": execution_ready,
        "promotion_ready": (bool(report["source"]["exact"]) and
                            structural_ready and execution_ready),
    }


def readiness_mutation_controls(report: dict) -> None:
    if readiness_summary(report)["promotion_ready"]:
        raise AssertionError("an unobserved raw ledger was promotion-ready")

    complete = deepcopy(report)
    complete["source"] = {"commit": "1" * 40, "state": "clean", "exact": True}
    for row in complete["rows"]:
        if row["state"] == "pending":
            row["state"] = "verified-nonexecuting"
            row["acceptance"]["test"] = "llvm_admission"
            row["acceptance"]["assertion"] = "ledger.nonexecuting"
        elif not row["acceptance"]["test"]:
            row["acceptance"]["test"] = "synthetic_execution"
            row["acceptance"]["assertion"] = "complete"
        row["acceptance"]["gap"] = ""
        row["acceptance"]["observed"] = "passed"
        row["boundary"]["observed"] = "passed"
    ready = readiness_summary(complete)
    if not (ready["structural_ready"] and ready["execution_ready"] and
            ready["promotion_ready"]):
        raise AssertionError("complete exact observations were not promotion-ready")

    complete["rows"][0]["acceptance"]["observed"] = "unrun"
    if readiness_summary(complete)["promotion_ready"]:
        raise AssertionError("one unrun acceptance remained promotion-ready")

    complete["rows"][0]["acceptance"]["observed"] = "passed"
    complete["source"]["state"] = "dirty"
    complete["source"]["exact"] = False
    if readiness_summary(complete)["promotion_ready"]:
        raise AssertionError("dirty source remained promotion-ready")

    stale = deepcopy(report)
    stale["source"]["commit"] = "0" * 40
    if not repository_source_errors(stale, report["source"]):
        raise AssertionError("stale compiled source identity was accepted")


def probe_option_controls(evidence: Path, root: Path, modules: dict) -> None:
    with tempfile.TemporaryDirectory(prefix="gti-llvm-profile-options-") as raw:
        work = Path(raw)
        basic = work / "basic.gti"
        basic.write_text("int main() { return 0; }\n", encoding="utf-8")
        result = run([str(evidence), "--probe", str(root), str(basic)])
        if result.returncode != 0 or "ExecutionProfile.SingleThreaded" not in result.stdout.splitlines():
            raise AssertionError(f"default single-threaded probe failed: {result.stderr}")

        atomic = modules["llvm_atomic_execution"]
        cases = atomic.evidence_cases(root)
        profiles = atomic.evidence_profiles(root)
        manifest = atomic.evidence_manifest(root)
        source = work / "atomic.gti"
        source.write_text(cases["scalar"], encoding="utf-8")
        for options in ([], ["--execution-profile", "single-threaded"]):
            result = run([str(evidence), "--probe", str(root), str(source), *options])
            if result.returncode != 1 or "GTI-S2084" not in result.stderr:
                raise AssertionError("single-threaded atomic probe lost its source rejection")
        result = run([str(evidence), "--probe", str(root), str(source),
                      "--execution-profile", "concurrent"])
        if result.returncode != 0 or not {
                "ExecutionProfile.Concurrent", "Shape.NativeAtomicOperation"
        }.issubset(set(result.stdout.splitlines())):
            raise AssertionError(f"explicit concurrent atomic probe failed: {result.stderr}")

        changed = deepcopy(manifest)
        changed["profiles"]["atomic.scalar-core"]["execution-profile"] = "single-threaded"
        validate_manifest(changed, "llvm_atomic_execution", cases, profiles)
        mode = changed["profiles"]["atomic.scalar-core"]["execution-profile"]
        result = run([str(evidence), "--probe", str(root), str(source),
                      "--execution-profile", mode])
        if result.returncode != 1 or "GTI-S2084" not in result.stderr:
            raise AssertionError("atomic evidence inferred concurrency after metadata downgrade")

        missing_source = work / "must-not-analyze.gti"
        for options in (["--execution-profile"],
                        ["--execution-profile", "unknown"],
                        ["--execution-profile", "sequential"]):
            result = run([str(evidence), "--probe", str(root), str(missing_source), *options])
            if result.returncode != 2 or "GTI-" in result.stderr:
                raise AssertionError("invalid profile option reached frontend analysis")


def main() -> None:
    if len(sys.argv) != 6:
        raise SystemExit(
            "usage: llvm_coverage_evidence_test.py <evidence-helper> <ctest> "
            "<build-dir> <repo-root> <manifest-registry>")
    evidence = Path(sys.argv[1]).resolve()
    ctest = sys.argv[2]
    build = Path(sys.argv[3]).resolve()
    root = Path(sys.argv[4]).resolve()
    registry = Path(sys.argv[5]).resolve()
    report = ledger(evidence)
    observed_source = repository_source(root)
    inventory = ctest_inventory(ctest, build)
    registered = set(inventory)
    modules = load_registry(registry, root, inventory)
    registry_mutation_controls(registry, root, inventory)
    manifest_mutation_controls(root, modules)
    probe_option_controls(evidence, root, modules)
    observations = observed_profiles(report, evidence, root, modules)
    errors = (repository_source_errors(report, observed_source) +
              validate(report, registered, observations))
    if errors:
        raise AssertionError("LLVM coverage evidence audit failed:\n" + "\n".join(errors))
    mutation_controls(report, registered, observations)
    readiness_mutation_controls(report)
    print(json.dumps(readiness_summary(report), sort_keys=True))


if __name__ == "__main__":
    main()
