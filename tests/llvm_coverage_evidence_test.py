#!/usr/bin/env python3
"""Audit LLVM ledger claims against registered, runnable evidence profiles."""

from copy import deepcopy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from llvm_evidence_manifest import build_manifest, validate_manifest, program_request_arguments


BASELINE_SCHEMA = "gti.llvm.readiness-baseline/v1"
USE_SCHEMA = "gti.llvm.required-uses/v1"
RUN_SCHEMA = "gti.llvm.readiness-run/v1"
SUMMARY_SCHEMA = "gti.llvm.coverage-readiness/v2"

REQUIRED_PROVIDER_IDS = {
    "registered-llvm-evidence",
    "shipped-examples",
    "stdlib-surface",
}
REQUIRED_PROVIDER_KINDS = {
    "registered-llvm-evidence": "registered-evidence-manifests",
    "shipped-examples": "source-census",
    "stdlib-surface": "source-backed-surface-manifest",
}
REQUIRED_FAMILY_OBLIGATIONS = {
    "conditional-required-uses",
    "general-language-coverage",
    "implemented-ledger-rows",
    "verified-nonexecuting-producer-absence",
}
REQUIRED_FAMILY_KINDS = {
    "conditional-required-uses": "admission-plan-implemented-shape-witness",
    "general-language-coverage": "reviewed-campaign-signoff",
    "implemented-ledger-rows": "registered-positive-and-boundary-evidence",
    "verified-nonexecuting-producer-absence":
        "producer-and-executable-absence-evidence",
}
REQUIRED_TARGET_IDS = {
    "linux-x86_64-concurrent",
    "linux-x86_64-single-threaded",
    "macos-arm64-concurrent",
    "macos-arm64-single-threaded",
}
REQUIRED_GATE_IDS = {
    "compiler-behavioral-regressions",
    "cross-family-interactions",
    "general-family-coverage",
    "historical-bug-probes",
    "installed-package",
    "native-embedding-oracles",
    "optimized-workloads",
    "runtime-sanitizers",
    "shipped-examples",
    "stdlib-surface",
}
ALL_TARGETS = tuple(sorted(REQUIRED_TARGET_IDS))
REQUIRED_GATE_TARGETS = {
    identity: ("linux-x86_64-single-threaded",
               "macos-arm64-single-threaded")
    if identity == "installed-package" else ALL_TARGETS
    for identity in REQUIRED_GATE_IDS
}


def run(command: list[str], *, timeout: int = 120) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, capture_output=True, text=True, timeout=timeout)


def canonical_bytes(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")


def digest(value: object) -> str:
    return hashlib.sha256(canonical_bytes(value)).hexdigest()


def file_digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def repository_path(root: Path, value: object, label: str) -> Path:
    require(isinstance(value, str) and value != "", f"{label} must be a path")
    candidate = (root / value).resolve()
    try:
        candidate.relative_to(root)
    except ValueError as error:
        raise AssertionError(f"{label} escapes the repository: {value}") from error
    require(candidate.is_file(), f"{label} does not name a file: {value}")
    return candidate


def keyed_rows(values: object, required: set[str], label: str) -> dict[str, dict]:
    require(isinstance(values, list) and values, f"{label} must be nonempty")
    rows: dict[str, dict] = {}
    for value in values:
        require(isinstance(value, dict), f"{label} entries must be objects")
        identity = value.get("id")
        require(isinstance(identity, str) and identity, f"{label} id must be nonempty")
        require(identity not in rows, f"duplicate {label} id: {identity}")
        rows[identity] = value
    missing = sorted(required - set(rows))
    extra = sorted(set(rows) - required)
    require(not missing and not extra,
            f"{label} inventory differs: missing={missing}, extra={extra}")
    return rows


def load_baseline(path: Path, root: Path) -> tuple[dict, dict[str, dict],
                                                    dict[str, dict], dict[str, dict]]:
    try:
        baseline = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise AssertionError(f"cannot read LLVM readiness baseline: {error}") from error
    require(isinstance(baseline, dict), "LLVM readiness baseline must be an object")
    require(baseline.get("schema") == BASELINE_SCHEMA,
            f"LLVM readiness baseline schema must be {BASELINE_SCHEMA}")
    provenance = baseline.get("provenance")
    require(isinstance(provenance, dict), "baseline provenance must be an object")
    frozen = provenance.get("frozen_commit")
    require(isinstance(frozen, str) and len(frozen) == 40,
            "baseline frozen_commit must be an exact commit")
    reconciled = provenance.get("reconciled_through")
    require(isinstance(reconciled, str) and len(reconciled) == 40,
            "baseline reconciled_through must be an exact reviewed commit")
    require(isinstance(provenance.get("version"), str) and provenance["version"],
            "baseline version must be nonempty")
    providers = keyed_rows(baseline.get("providers"), REQUIRED_PROVIDER_IDS,
                           "provider")
    obligations = keyed_rows(baseline.get("family_obligations"),
                             REQUIRED_FAMILY_OBLIGATIONS, "family obligation")
    targets = keyed_rows(baseline.get("targets"), REQUIRED_TARGET_IDS, "target")
    gates = keyed_rows(baseline.get("gates"), REQUIRED_GATE_IDS, "gate")
    for gate_id, gate in gates.items():
        required_targets = gate.get("targets")
        require(isinstance(required_targets, list) and required_targets,
                f"gate {gate_id} must name required targets")
        require(len(required_targets) == len(set(required_targets)),
                f"gate {gate_id} repeats a target")
        require(set(required_targets) <= set(targets),
                f"gate {gate_id} names an unknown target")
        require(tuple(sorted(required_targets)) ==
                tuple(sorted(REQUIRED_GATE_TARGETS[gate_id])),
                f"gate {gate_id} target contract was weakened")
    for identity, obligation in obligations.items():
        require(obligation.get("kind") == REQUIRED_FAMILY_KINDS[identity],
                f"family obligation {identity} kind changed")
    for identity, target in targets.items():
        expected_profile = ("concurrent" if identity.endswith("-concurrent")
                            else "single-threaded")
        expected = {
            "os": "macos" if identity.startswith("macos-") else "linux",
            "vendor": "apple" if identity.startswith("macos-") else "unknown",
            "arch": "arm64" if "-arm64-" in identity else "x86_64",
            "endianness": "little",
            "pointer_width_bits": 64,
            "execution_profile": expected_profile,
            "threads": expected_profile == "concurrent",
        }
        for field, value in expected.items():
            require(target.get(field) == value,
                    f"target {identity} has wrong {field}: {target.get(field)}")
    for provider_id, provider in providers.items():
        require(provider.get("kind") == REQUIRED_PROVIDER_KINDS[provider_id],
                f"provider {provider_id} kind changed")
        if provider_id != "registered-llvm-evidence":
            repository_path(root, provider.get("manifest"),
                            f"provider {provider_id} manifest")
        require(isinstance(provider.get("member_count"), int) and
                provider["member_count"] > 0,
                f"provider {provider_id} member_count must be positive")
        fingerprint = provider.get("inventory_sha256")
        require(isinstance(fingerprint, str) and len(fingerprint) == 64,
                f"provider {provider_id} inventory_sha256 must be SHA-256")
    return baseline, providers, targets, gates


def baseline_mutation_controls(baseline: dict, root: Path) -> None:
    def rejected(mutation) -> None:
        changed = deepcopy(baseline)
        mutation(changed)
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8",
                                         suffix=".json") as output:
            json.dump(changed, output)
            output.flush()
            try:
                load_baseline(Path(output.name), root)
            except AssertionError:
                return
        raise AssertionError("LLVM readiness baseline accepted a mutated contract")

    rejected(lambda value: value["providers"].clear())
    rejected(lambda value: value["providers"].pop())
    rejected(lambda value: value["providers"].append(
        {"id": "extra", "kind": "forged", "member_count": 1,
         "inventory_sha256": "0" * 64}))
    rejected(lambda value: value["family_obligations"].pop())
    rejected(lambda value: value["targets"].pop())
    rejected(lambda value: value["targets"].append({"id": "extra"}))
    rejected(lambda value: value["gates"].pop())
    rejected(lambda value: value["gates"][0].__setitem__("targets", []))


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


def example_provider_inventory(root: Path, provider: dict) -> dict:
    manifest_path = repository_path(root, provider["manifest"],
                                    "shipped examples manifest")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    examples = manifest.get("examples")
    require(isinstance(examples, dict) and examples,
            "shipped examples census must be nonempty")
    members = []
    for name in sorted(examples):
        source = repository_path(root, f"examples/{name}",
                                 f"shipped example {name}")
        members.append({"path": source.relative_to(root).as_posix(),
                        "sha256": file_digest(source)})
    return {
        "manifest": manifest_path.relative_to(root).as_posix(),
        "manifest_sha256": file_digest(manifest_path),
        "members": members,
    }


def stdlib_provider_inventory(root: Path, provider: dict) -> dict:
    manifest_path = repository_path(root, provider["manifest"],
                                    "stdlib surface manifest")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    fingerprints = manifest.get("source_fingerprints")
    cases = manifest.get("cases")
    require(isinstance(fingerprints, dict) and fingerprints,
            "stdlib surface source inventory must be nonempty")
    require(isinstance(cases, list) and cases,
            "stdlib surface case inventory must be nonempty")
    for relative, expected in fingerprints.items():
        source = repository_path(root, relative, f"stdlib source {relative}")
        require(file_digest(source) == expected,
                f"stdlib surface source identity drifted: {relative}")
    case_inventory = []
    for case in cases:
        case_id = case.get("id")
        source = repository_path(root, case.get("source"),
                                 f"stdlib surface case {case_id}")
        native_sources = []
        for relative in case.get("native_sources", []):
            native = repository_path(
                root, relative, f"stdlib surface native case {case_id}")
            native_sources.append({
                "path": native.relative_to(root).as_posix(),
                "sha256": file_digest(native),
            })
        case_inventory.append({
            "id": case_id,
            "source": source.relative_to(root).as_posix(),
            "sha256": file_digest(source),
            "native_sources": native_sources,
        })
    return {
        "manifest": manifest_path.relative_to(root).as_posix(),
        "manifest_sha256": file_digest(manifest_path),
        "source_fingerprints": fingerprints,
        "cases": case_inventory,
    }


def registered_provider_inventory(root: Path, modules: dict) -> dict:
    manifests = {
        test: module.evidence_manifest(root)
        for test, module in sorted(modules.items())
    }
    require(manifests, "registered LLVM evidence provider must be nonempty")
    return {"manifests": manifests}


def validate_provider_inventory(provider_id: str, provider: dict,
                                inventory: dict) -> None:
    if provider_id == "shipped-examples":
        count = len(inventory["members"])
    elif provider_id == "stdlib-surface":
        count = len(inventory["cases"])
    else:
        count = sum(len(manifest["sources"])
                    for manifest in inventory["manifests"].values())
    require(count == provider["member_count"],
            f"provider {provider_id} member count drifted: expected "
            f"{provider['member_count']}, found {count}")
    actual = digest(inventory)
    require(actual == provider["inventory_sha256"],
            f"provider {provider_id} inventory drifted: expected "
            f"{provider['inventory_sha256']}, found {actual}")


def provider_mutation_controls(providers: dict[str, dict],
                               inventories: dict[str, dict]) -> None:
    identity = "shipped-examples"
    stale = deepcopy(providers[identity])
    stale["inventory_sha256"] = "0" * 64
    try:
        validate_provider_inventory(identity, stale, inventories[identity])
    except AssertionError:
        pass
    else:
        raise AssertionError("stale canonical source-provider identity passed")

    truncated = deepcopy(inventories[identity])
    truncated["members"].pop()
    try:
        validate_provider_inventory(identity, providers[identity], truncated)
    except AssertionError:
        pass
    else:
        raise AssertionError("truncated canonical source provider passed")


def profile_sources(root: Path, modules: dict) -> dict:
    sources = {}
    for test, module in modules.items():
        cases = module.evidence_cases(root)
        profiles = module.evidence_profiles(root)
        declared = module.evidence_manifest(root)
        requests = getattr(module, "evidence_program_requests", lambda root: {})(root)
        validate_manifest(declared, test, cases, profiles, program_requests=requests)
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
        sources[test] = (cases, profiles, declared["profiles"], requests)
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


def execution_profile_controls(evidence: Path, root: Path) -> None:
    cases = {"basic": "int main() { return 0; }\n"}
    profiles = {"basic": ("basic",)}
    manifest = build_manifest("profile-control", cases, profiles)
    for mode in ("sequential", "unknown", 0, None):
        changed = deepcopy(manifest)
        changed["profiles"]["basic"]["execution-profile"] = mode
        try:
            validate_manifest(changed, "profile-control", cases, profiles)
        except ValueError:
            pass
        else:
            raise AssertionError("invalid execution profile was accepted")
    for remove in (True, False):
        changed = deepcopy(manifest)
        if remove:
            changed["profiles"]["basic"].pop("execution-profile")
        else:
            changed["schema"] = "gti.llvm.execution-evidence/v1"
        try:
            validate_manifest(changed, "profile-control", cases, profiles)
        except ValueError:
            pass
        else:
            raise AssertionError("missing or legacy profile metadata was accepted")
    try:
        build_manifest("profile-control", cases, profiles,
                       execution_profiles={"unknown": "concurrent"})
    except ValueError:
        pass
    else:
        raise AssertionError("unknown execution profile override was accepted")
    with tempfile.TemporaryDirectory(prefix="gti-llvm-profile-controls-") as raw:
        source = Path(raw) / "basic.gti"
        source.write_text(cases["basic"], encoding="utf-8")
        for mode, key in (("single-threaded", "ExecutionProfile.SingleThreaded"),
                          ("concurrent", "ExecutionProfile.Concurrent")):
            result = run([str(evidence), "--probe", str(root), str(source), mode])
            require(result.returncode == 0 and key in result.stdout.splitlines(),
                    f"explicit profile probe failed: {mode}: {result.stderr}")
        default = run([str(evidence), "--probe", str(root), str(source)])
        require(default.returncode == 0 and
                "ExecutionProfile.SingleThreaded" in default.stdout.splitlines(),
                "default probe changed its execution profile")
        source.write_text("#include <std/atomic>\nint main() { "
                          "std::atomic<int> value{0}; return value.load(); }\n",
                          encoding="utf-8")
        for arguments in ([], ["single-threaded"]):
            rejected = run([str(evidence), "--probe", str(root), str(source), *arguments])
            require(rejected.returncode == 1 and "GTI-S2084" in rejected.stderr,
                    "single-threaded atomic probe lost its source rejection")
        concurrent = run([str(evidence), "--probe", str(root), str(source), "concurrent"])
        require(concurrent.returncode == 0 and {
                    "ExecutionProfile.Concurrent", "Shape.NativeAtomicOperation"
                }.issubset(set(concurrent.stdout.splitlines())),
                "explicit concurrent atomic probe lost its exact witness")
        downgraded = build_manifest("atomic-profile-control", {"basic": source.read_text()},
                                   profiles, execution_profiles={"basic": "concurrent"})
        downgraded["profiles"]["basic"]["execution-profile"] = "single-threaded"
        validate_manifest(downgraded, "atomic-profile-control", {"basic": source.read_text()}, profiles)
        rejected = run([str(evidence), "--probe", str(root), str(source),
                        downgraded["profiles"]["basic"]["execution-profile"]])
        require(rejected.returncode == 1 and "GTI-S2084" in rejected.stderr,
                "atomic evidence inferred concurrency after metadata downgrade")
        invalid = run([str(evidence), "--probe", str(root), str(source), "unknown"])
        require(invalid.returncode == 2 and "GTI-" not in invalid.stderr,
                "invalid profile reached source analysis")


def embedding_request_controls(evidence: Path, root: Path, modules: dict) -> None:
    module = modules["llvm_embedding_execution"]
    cases, profiles = module.evidence_cases(root), module.evidence_profiles(root)
    requests = module.evidence_program_requests(root)
    manifest = module.evidence_manifest(root)
    for mutation in (lambda record: record.pop("program-request"),
                     lambda record: record["program-request"].update(kind="executable"),
                     lambda record: record["program-request"].update(exports=[]),
                     lambda record: record["program-request"]["exports"][0].update(function="unused"),
                     lambda record: record["program-request"]["exports"][0].update(symbol="wrong_symbol")):
        changed = deepcopy(manifest)
        mutation(changed["sources"]["infallible"])
        try:
            validate_manifest(changed, manifest["test"], cases, profiles,
                              program_requests=requests)
        except ValueError:
            pass
        else:
            raise AssertionError("wrong/omitted embedding request manufactured evidence")
    with tempfile.TemporaryDirectory(prefix="gti-llvm-embedding-controls-") as raw:
        source = Path(raw) / "embedding.gti"
        source.write_text(cases["infallible"], encoding="utf-8")
        prefix = [str(evidence), "--probe", str(root), str(source), "single-threaded"]
        for arguments in ([], ["--library"], ["--library", "missing=embed_missing"]):
            result = run([*prefix, *arguments])
            require((not arguments or result.returncode != 0) and
                    "Shape.InfallibleEmbedding" not in result.stdout,
                    "invalid embedding probe manufactured a witness")
        result = run([*prefix, *program_request_arguments(requests["infallible"])])
        require(result.returncode == 0 and "Shape.InfallibleEmbedding" in result.stdout,
                f"exact embedding request did not produce its witness: {result.stderr}")


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
                      modules: dict) -> dict[tuple[str, str], set[str]]:
    sources = profile_sources(root, modules)
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
            cases, profiles, metadata, requests = sources[test]
            if profile not in profiles:
                continue
            keys: set[str] = set()
            for case in profiles[profile]:
                source = directory / f"{test}-{profile}-{case}.gti"
                source.write_text(cases[case], encoding="utf-8")
                result = run([str(evidence), "--probe", str(root), str(source),
                              metadata[profile]["execution-profile"],
                              *program_request_arguments(requests.get(case))])
                if result.returncode != 0:
                    raise AssertionError(
                        f"coverage probe failed for {test}:{profile}:{case}:\n"
                        f"{result.stderr}")
                keys.update(line for line in result.stdout.splitlines() if line)
            observations[(test, profile)] = keys
    return observations


def probe_uses(evidence: Path, root: Path, source: Path, source_id: str,
               profile: str, report: dict) -> dict:
    result = run([str(evidence), "--probe-uses", str(root), str(source), profile])
    if result.returncode != 0:
        return {
            "schema": RUN_SCHEMA,
            "status": "failed",
            "source": {"id": source_id, "sha256": file_digest(source)},
            "returncode": result.returncode,
            "stdout": result.stdout,
            "stderr": result.stderr,
        }
    try:
        artifact = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise AssertionError(f"{source_id}: malformed use artifact: {error}") from error
    artifact["input"] = {"id": source_id, "sha256": file_digest(source)}
    errors = validate_use_artifact(artifact, report)
    if errors:
        raise AssertionError(f"{source_id}: invalid use artifact:\n" + "\n".join(errors))
    artifact["artifact_sha256"] = digest(artifact)
    return artifact


def coverage_identity(entry: dict) -> tuple[object, ...]:
    return (entry.get("key"), entry.get("state"), entry.get("nonexecuting"),
            entry.get("predicate"))


def use_is_unsupported(use: dict) -> bool:
    base = use["base"]
    witness = use.get("witness")
    return ((base["state"] == "pending" and
             (not witness or witness["state"] != "implemented")) or
            (use["executable"] and
             base["nonexecuting"] == "must-be-absent"))


def validate_use_artifact(artifact: dict, report: dict) -> list[str]:
    if not isinstance(artifact, dict):
        return ["use artifact must be an object"]
    errors: list[str] = []
    if artifact.get("schema") != USE_SCHEMA:
        errors.append(f"use artifact schema must be {USE_SCHEMA}")
    if artifact.get("compiler") != {
            "commit": report["source"]["commit"],
            "state": report["source"]["state"]}:
        errors.append("use artifact compiler identity does not match ledger build")
    if artifact.get("build") != report.get("build"):
        errors.append("use artifact build identity does not match ledger build")
    target = artifact.get("target")
    if not isinstance(target, dict) or set(target) != {
            "arch", "endianness", "execution_profile", "os",
            "pointer_width_bits", "threads", "vendor"}:
        errors.append("use artifact target identity is malformed")
    uses = artifact.get("uses")
    if not isinstance(uses, list) or artifact.get("use_count") != len(uses):
        errors.append("use artifact count does not match its complete inventory")
        uses = []
    rows = {row["key"]: row for row in report.get("rows", [])}
    fallback_unsupported = None
    located_unsupported = None
    for index, use in enumerate(uses):
        if not isinstance(use, dict) or set(use) != {
                "base", "witness", "executable", "body", "block",
                "instruction", "occurrences", "source"}:
            errors.append(f"use {index} has malformed fields")
            continue
        base = use.get("base")
        witness = use.get("witness")
        if (not isinstance(base, dict) or
                not isinstance(base.get("key"), str) or
                base["key"] not in rows):
            errors.append(f"use {index} has an unknown base row")
            continue
        expected = rows[base["key"]]
        if coverage_identity(base) != (
                expected["key"], expected["state"], expected["nonexecuting"],
                expected["predicate"]):
            errors.append(f"use {index} base row identity/state drifted")
            continue
        if witness is not None:
            if (not isinstance(witness, dict) or
                    not isinstance(witness.get("key"), str) or
                    witness["key"] not in rows):
                errors.append(f"use {index} has an unknown witness row")
                continue
            else:
                expected_witness = rows[witness["key"]]
                if coverage_identity(witness) != (
                        expected_witness["key"], expected_witness["state"],
                        expected_witness["nonexecuting"],
                        expected_witness["predicate"]):
                    errors.append(f"use {index} witness identity/state drifted")
                    continue
        if not isinstance(use.get("executable"), bool):
            errors.append(f"use {index} executable flag is malformed")
            continue
        if type(use.get("occurrences")) is not int or use["occurrences"] <= 0:
            errors.append(f"use {index} occurrence count is malformed")
            continue
        body = use.get("body")
        if body is not None and (
                not isinstance(body, dict) or set(body) != {"kind", "owner"} or
                any(type(body[field]) is not int or body[field] < 0
                    for field in ("kind", "owner"))):
            errors.append(f"use {index} body identity is malformed")
            continue
        if any(type(use.get(field)) is not int or use[field] < 0
               for field in ("block", "instruction")):
            errors.append(f"use {index} instruction identity is malformed")
            continue
        source = use.get("source")
        if not isinstance(source, dict) or set(source) != {
                "path", "start", "end", "line"}:
            errors.append(f"use {index} source identity is malformed")
            continue
        if (not isinstance(source["path"], str) or
                any(type(source[field]) is not int or source[field] < 0
                    for field in ("start", "end", "line"))):
            errors.append(f"use {index} source identity is malformed")
            continue
        if use_is_unsupported(use):
            if fallback_unsupported is None:
                fallback_unsupported = index
            # Match LlvmAdmissionPlan::firstUnsupported(): prefer the first
            # source-located instruction, retaining the earliest fallback.
            if (located_unsupported is None and body is not None and
                    use["instruction"] != 0 and source["path"]):
                located_unsupported = index
    computed_unsupported = (located_unsupported if located_unsupported is not None
                            else fallback_unsupported)
    if artifact.get("first_unsupported") != computed_unsupported:
        errors.append("use artifact firstUnsupported disagrees with its plan records")
    if artifact.get("admitted") != (computed_unsupported is None):
        errors.append("use artifact admission result disagrees with firstUnsupported")
    if artifact.get("closure") not in {
            "all-executable-bodies", "sealed-entry-dependencies"}:
        errors.append("use artifact closure is malformed")
    return errors


def use_artifact_validation_controls(report: dict,
                                    probes: dict[str, dict]) -> None:
    original = probes["unsupported"]
    unsupported = original["uses"][original["first_unsupported"]]
    located = deepcopy(unsupported)
    located.update(body={"kind": 0, "owner": 1}, instruction=1,
                   source={"path": "probe.gti", "start": 1, "end": 2, "line": 1})
    no_body = deepcopy(located)
    no_body["body"] = None
    no_instruction = deepcopy(located)
    no_instruction["instruction"] = 0
    no_source = deepcopy(located)
    no_source["source"]["path"] = ""

    def selection(uses: list[dict], selected: int) -> dict:
        artifact = deepcopy(original)
        artifact.update(uses=deepcopy(uses), use_count=len(uses),
                        first_unsupported=selected, admitted=False)
        return artifact

    # A fallback may precede the first instruction, and a later instruction
    # must not displace the first source-located unsupported instruction.
    preferred = selection([no_body, located, located], 1)
    require(not validate_use_artifact(preferred, report),
            "source-located unsupported instruction did not outrank fallback")
    for incorrect in (0, 2):
        changed = deepcopy(preferred)
        changed["first_unsupported"] = incorrect
        require(any("firstUnsupported" in error
                    for error in validate_use_artifact(changed, report)),
                "incorrect unsupported instruction selection was accepted")

    fallback = selection([no_body, no_instruction, no_source], 0)
    require(not validate_use_artifact(fallback, report),
            "earliest unsupported fallback was not retained")
    fallback["first_unsupported"] = 1
    require(any("firstUnsupported" in error
                for error in validate_use_artifact(fallback, report)),
            "later fallback displaced the first unsupported record")

    callable_artifact = probes["callable"]
    conditional_index = next(
        index for index, use in enumerate(callable_artifact["uses"])
        if use["base"]["state"] == "pending" and use.get("witness"))
    known_key = callable_artifact["uses"][conditional_index]["witness"]["key"]
    malformed_witnesses = (
        {}, {"key": known_key}, {"key": "unknown"}, {"key": []},
        {"key": known_key, "state": "implemented"}, "bad", ["bad"], 1,
    )
    for witness in malformed_witnesses:
        changed = deepcopy(callable_artifact)
        changed["uses"][conditional_index]["witness"] = witness
        require(any("witness" in error
                    for error in validate_use_artifact(changed, report)),
                f"malformed witness was not rejected: {witness!r}")


def target_id(target: dict) -> str | None:
    profile = target.get("execution_profile")
    if target.get("os") == "macos" and target.get("vendor") == "apple" and \
            target.get("arch") == "arm64":
        return f"macos-arm64-{profile}"
    if target.get("os") == "linux" and target.get("vendor") == "unknown" and \
            target.get("arch") == "x86_64":
        return f"linux-x86_64-{profile}"
    return None


def focused_use_probes(evidence: Path, root: Path, report: dict) -> dict[str, dict]:
    with tempfile.TemporaryDirectory(prefix="gti-llvm-readiness-probes-") as raw:
        directory = Path(raw)
        scalar = directory / "scalar.gti"
        scalar.write_text("int main() { return 0; }\n", encoding="utf-8")
        native = directory / "native.gti"
        native.write_text(
            'extern "C" { int foreign_call(int value); }\n'
            "int main() { return foreign_call(0); }\n", encoding="utf-8")
        unsupported = directory / "unsupported.gti"
        # Keep the real unsupported probe aligned with an existing admission
        # control, not a native import that now has an implemented ABI witness.
        # Base-view replacement stays refused; move this control to another
        # refused construct as LLVM coverage grows.
        unsupported.write_text(
            "class Owner { public: virtual int read() { return 0; }  public: Owner() = default; };\n"
            "class Derived : public Owner { public: Derived() = default; };\n"
            "unsafe int create(mut Owner& value, Owner& other) { "
            "value = other; return 0; }\n"
            "int main() { mut Derived first{}; Derived second{}; "
            "unsafe { return create(first, second); } }\n", encoding="utf-8")
        malformed = directory / "malformed.gti"
        malformed.write_text("int main( { return 0; }\n", encoding="utf-8")
        probes = {
            "scalar": probe_uses(evidence, root, scalar, "focused:scalar",
                                 "single-threaded", report),
            "callable": probe_uses(
                evidence, root, root / "tests/fixtures/llvm_callable_pointer.gti",
                "focused:callable", "single-threaded", report),
            "native": probe_uses(evidence, root, native, "focused:native",
                                 "single-threaded", report),
            "unsupported": probe_uses(evidence, root, unsupported,
                                      "focused:unsupported", "single-threaded",
                                      report),
            "frontend-failure": probe_uses(
                evidence, root, malformed, "focused:frontend-failure",
                "single-threaded", report),
        }
    require(probes["scalar"].get("admitted") is True,
            "real scalar readiness probe was not admitted")
    require(probes["callable"].get("admitted") is True,
            "real callable readiness probe was not admitted")
    require(probes["native"].get("admitted") is True,
            "real native import readiness probe was not admitted")
    conditional = [
        use for use in probes["callable"]["uses"]
        if use["base"]["state"] == "pending" and use.get("witness") and
        use["witness"]["state"] == "implemented"
    ]
    require(conditional,
            "real callable readiness probe has no conditional Pending witness")
    require(probes["unsupported"].get("admitted") is False and
            probes["unsupported"].get("first_unsupported") is not None,
            "real unsupported readiness probe did not fail closed")
    require(probes["frontend-failure"].get("status") == "failed" and
            probes["frontend-failure"].get("returncode") != 0 and
            probes["frontend-failure"].get("stderr"),
            "failed readiness probe did not preserve its original report")
    return probes


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


def legacy_readiness_summary(report: dict) -> dict:
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


def payload_digest(artifact: dict) -> str:
    return digest({key: value for key, value in artifact.items()
                   if key != "artifact_sha256"})


def use_receipt(artifact: dict, baseline_sha256: str) -> dict:
    return {
        "schema": "gti.llvm.readiness-use-receipt/v1",
        "scope": "full-baseline",
        "status": "passed",
        "baseline_sha256": baseline_sha256,
        "artifact_sha256": artifact["artifact_sha256"],
        "compiler": artifact["compiler"],
        "build": artifact["build"],
        "target": artifact["target"],
        "input": artifact["input"],
    }


def validate_use_receipt(artifact: dict, receipt: dict,
                         baseline_sha256: str) -> list[str]:
    errors = []
    expected = use_receipt(artifact, baseline_sha256)
    if artifact.get("artifact_sha256") != payload_digest(artifact):
        errors.append("use artifact digest changed or was truncated")
    for field, value in expected.items():
        if receipt.get(field) != value:
            errors.append(f"use receipt {field} does not match its exact artifact")
    if set(receipt) != set(expected):
        errors.append("use receipt has missing or unknown fields")
    return errors


def synthetic_complete_ready(fixture: dict, report: dict) -> bool:
    if fixture.get("schema") != "gti.llvm.synthetic-readiness/v1":
        return False
    if set(fixture.get("providers", {})) != {"synthetic-complete"}:
        return False
    if set(fixture.get("targets", {})) != {"synthetic-target"}:
        return False
    if set(fixture.get("gates", {})) != {"synthetic-general-gate"}:
        return False
    if fixture["gates"]["synthetic-general-gate"] != "passed":
        return False
    artifacts = fixture["providers"]["synthetic-complete"]
    if not isinstance(artifacts, list) or not artifacts:
        return False
    receipts = fixture.get("receipts")
    if not isinstance(receipts, list) or len(receipts) != len(artifacts):
        return False
    baseline_sha256 = fixture.get("baseline_sha256")
    if not isinstance(baseline_sha256, str) or len(baseline_sha256) != 64:
        return False
    for artifact, receipt in zip(artifacts, receipts, strict=True):
        if validate_use_artifact(artifact, report) or not artifact.get("admitted"):
            return False
        if validate_use_receipt(artifact, receipt, baseline_sha256):
            return False
        if target_id(artifact["target"]) not in REQUIRED_TARGET_IDS:
            return False
    return True


def readiness_mutation_controls(report: dict, probes: dict[str, dict]) -> None:
    if legacy_readiness_summary(report)["promotion_ready"]:
        raise AssertionError("an unobserved raw ledger was promotion-ready")

    callable_artifact = probes["callable"]
    conditional_index = next(
        index for index, use in enumerate(callable_artifact["uses"])
        if use["base"]["state"] == "pending" and use.get("witness") and
        use["witness"]["state"] == "implemented")
    baseline_sha256 = "2" * 64
    fixture = {
        "schema": "gti.llvm.synthetic-readiness/v1",
        "baseline_sha256": baseline_sha256,
        "providers": {"synthetic-complete": [callable_artifact]},
        "targets": {"synthetic-target": callable_artifact["target"]},
        "gates": {"synthetic-general-gate": "passed"},
        "receipts": [use_receipt(callable_artifact, baseline_sha256)],
    }
    require(synthetic_complete_ready(fixture, report),
            "synthetic complete baseline with a real conditional witness failed")

    def rejected(mutation) -> None:
        changed = deepcopy(fixture)
        mutation(changed)
        require(not synthetic_complete_ready(changed, report),
                "synthetic readiness accepted a mutated exact artifact")

    rejected(lambda value: value["providers"]["synthetic-complete"][0]
             ["uses"][conditional_index].__setitem__("witness", None))

    other_witness = next(
        row for row in report["rows"]
        if row["state"] == "implemented" and
        row["key"] != callable_artifact["uses"][conditional_index]
        ["witness"]["key"])

    def wrong_witness(value: dict) -> None:
        value["providers"]["synthetic-complete"][0]["uses"][conditional_index][
            "witness"] = {
                "key": other_witness["key"],
                "state": other_witness["state"],
                "nonexecuting": other_witness["nonexecuting"],
                "predicate": other_witness["predicate"],
            }

    rejected(wrong_witness)
    rejected(lambda value: value["providers"]["synthetic-complete"][0]
             ["uses"].pop())
    rejected(lambda value: value["providers"]["synthetic-complete"][0]
             ["uses"].append(deepcopy(
                 value["providers"]["synthetic-complete"][0]["uses"][0])))
    rejected(lambda value: value["providers"].clear())
    rejected(lambda value: value["providers"].__setitem__("extra", []))
    rejected(lambda value: value["targets"].clear())
    rejected(lambda value: value["targets"].__setitem__("extra", {}))
    rejected(lambda value: value["gates"].__setitem__(
        "synthetic-general-gate", "unrun"))
    rejected(lambda value: value["gates"].__setitem__(
        "synthetic-general-gate", "failed"))
    rejected(lambda value: value["receipts"][0]["compiler"].__setitem__(
        "commit", "0" * 40))
    rejected(lambda value: value["receipts"][0]["target"].__setitem__(
        "arch", "arm64" if callable_artifact["target"]["arch"] == "x86_64" else "x86_64"))
    rejected(lambda value: value["receipts"][0]["build"].__setitem__(
        "llvm", "stale"))
    rejected(lambda value: value["receipts"][0]["input"].__setitem__(
        "sha256", "0" * 64))
    rejected(lambda value: value["receipts"][0].__setitem__(
        "baseline_sha256", "0" * 64))

    must_be_absent = next(
        row for row in report["rows"]
        if row["nonexecuting"] == "must-be-absent")

    def forged_nonexecuting(value: dict) -> None:
        artifact = value["providers"]["synthetic-complete"][0]
        use = artifact["uses"][0]
        use["base"] = {
            "key": must_be_absent["key"],
            "state": must_be_absent["state"],
            "nonexecuting": must_be_absent["nonexecuting"],
            "predicate": must_be_absent["predicate"],
        }
        use["executable"] = True
        artifact["artifact_sha256"] = payload_digest(artifact)
        value["receipts"][0] = use_receipt(artifact, baseline_sha256)

    rejected(forged_nonexecuting)

    stale = deepcopy(report)
    stale["source"]["commit"] = "0" * 40
    if not repository_source_errors(stale, report["source"]):
        raise AssertionError("stale compiled source identity was accepted")


def readiness_summary(report: dict, baseline_path: Path, baseline: dict,
                      providers: dict[str, dict],
                      targets: dict[str, dict], gates: dict[str, dict],
                      provider_inventories: dict[str, dict],
                      observations: dict[tuple[str, str], set[str]],
                      probes: dict[str, dict], run_artifact: Path) -> dict:
    legacy = legacy_readiness_summary(report)
    represented = set().union(*observations.values()) if observations else set()
    implemented = {
        row["key"] for row in report["rows"] if row["state"] == "implemented"
    }
    unrepresented = sorted(implemented - represented)
    unresolved = [
        *(f"provider:{identity}:full-use-artifact-unrun"
          for identity in sorted(providers)),
        *(f"target:{identity}:full-use-artifact-unrun"
          for identity in sorted(targets)),
        *(f"gate:{identity}:receipt-unrun" for identity in sorted(gates)),
    ]
    if unrepresented:
        unresolved.append("family:implemented-row-unrepresented")
    if legacy["implemented_evidence_gaps"]:
        unresolved.append("family:implemented-evidence-gap")
    focused = {
        name: {
            "status": artifact.get("status", "observed"),
            "admitted": artifact.get("admitted"),
            "artifact_sha256": artifact.get("artifact_sha256"),
            "first_unsupported": artifact.get("first_unsupported"),
            "target": target_id(artifact.get("target", {})),
            "use_count": artifact.get("use_count"),
        }
        for name, artifact in probes.items()
    }
    return {
        "schema": SUMMARY_SCHEMA,
        # Backward-compatible v1 fields retain their old global-row meaning.
        **legacy,
        "legacy_global_row_readiness": {
            "structural_ready": legacy["structural_ready"],
            "execution_ready": legacy["execution_ready"],
            "promotion_ready": legacy["promotion_ready"],
        },
        "row_universe_classified": True,
        "baseline": {
            "schema": baseline["schema"],
            "sha256": file_digest(baseline_path),
            "provenance": baseline["provenance"],
            "providers": {
                identity: {
                    "member_count": provider["member_count"],
                    "inventory_sha256": digest(provider_inventories[identity]),
                    "status": "full-use-artifact-unrun",
                }
                for identity, provider in sorted(providers.items())
            },
            "required_targets": sorted(targets),
            "required_gates": sorted(gates),
        },
        "focused_probes": focused,
        "unrepresented_supported_families": unrepresented,
        "baseline_unresolved": unresolved,
        "baseline_structural_ready": False,
        "execution_boundary_evidence_ready": False,
        "general_language_ready": False,
        "platform_performance_integration_ready": False,
        "campaign_promotion_ready": False,
        "focused_run_artifact": {
            "path": str(run_artifact),
            "sha256": file_digest(run_artifact),
        },
    }


def focused_run_document(report: dict, baseline_path: Path, baseline: dict,
                         provider_inventories: dict[str, dict],
                         targets: dict[str, dict], gates: dict[str, dict],
                         probes: dict[str, dict]) -> dict:
    return {
        "schema": RUN_SCHEMA,
        "scope": "focused-schema-control",
        "baseline": {
            "path": baseline_path.name,
            "sha256": file_digest(baseline_path),
            "provenance": baseline["provenance"],
        },
        "compiler": report["source"],
        "build": report["build"],
        "provider_inventories": provider_inventories,
        "required_targets": {
            identity: {"contract": target, "status": "full-use-artifact-unrun"}
            for identity, target in sorted(targets.items())
        },
        "required_gates": {
            identity: {"contract": gate, "status": "receipt-unrun"}
            for identity, gate in sorted(gates.items())
        },
        "focused_probes": probes,
        "baseline_structural_ready": False,
        "execution_boundary_evidence_ready": False,
        "general_language_ready": False,
        "platform_performance_integration_ready": False,
        "campaign_promotion_ready": False,
    }


def main() -> None:
    if len(sys.argv) not in {6, 7}:
        raise SystemExit(
            "usage: llvm_coverage_evidence_test.py <evidence-helper> <ctest> "
            "<build-dir> <repo-root> <manifest-registry> [readiness-baseline]")
    evidence = Path(sys.argv[1]).resolve()
    ctest = sys.argv[2]
    build = Path(sys.argv[3]).resolve()
    root = Path(sys.argv[4]).resolve()
    registry = Path(sys.argv[5]).resolve()
    baseline_path = (Path(sys.argv[6]).resolve() if len(sys.argv) == 7 else
                     root / "tests/llvm_readiness_baseline.json")
    report = ledger(evidence)
    observed_source = repository_source(root)
    inventory = ctest_inventory(ctest, build)
    registered = set(inventory)
    modules = load_registry(registry, root, inventory)
    baseline, providers, targets, gates = load_baseline(baseline_path, root)
    baseline_mutation_controls(baseline, root)
    provider_inventories = {
        "shipped-examples": example_provider_inventory(
            root, providers["shipped-examples"]),
        "stdlib-surface": stdlib_provider_inventory(
            root, providers["stdlib-surface"]),
        "registered-llvm-evidence": registered_provider_inventory(root, modules),
    }
    for identity, provider in providers.items():
        validate_provider_inventory(identity, provider,
                                    provider_inventories[identity])
    provider_mutation_controls(providers, provider_inventories)
    registry_mutation_controls(registry, root, inventory)
    manifest_mutation_controls(root, modules)
    execution_profile_controls(evidence, root)
    embedding_request_controls(evidence, root, modules)
    observations = observed_profiles(report, evidence, root, modules)
    errors = (repository_source_errors(report, observed_source) +
              validate(report, registered, observations))
    if errors:
        raise AssertionError("LLVM coverage evidence audit failed:\n" + "\n".join(errors))
    mutation_controls(report, registered, observations)
    probes = focused_use_probes(evidence, root, report)
    use_artifact_validation_controls(report, probes)
    readiness_mutation_controls(report, probes)
    run_artifact = build / "gti_llvm_readiness_focused.json"
    run_artifact.write_text(
        json.dumps(focused_run_document(
            report, baseline_path, baseline, provider_inventories, targets,
            gates, probes), sort_keys=True, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(readiness_summary(
        report, baseline_path, baseline, providers, targets, gates,
        provider_inventories, observations, probes, run_artifact),
        sort_keys=True))


if __name__ == "__main__":
    main()
