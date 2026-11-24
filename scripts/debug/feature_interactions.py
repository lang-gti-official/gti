#!/usr/bin/env python3
"""Generate, execute, replay and shrink bounded GTI feature interactions.

Every case is compiled by GTI's LLVM backend, the only tested backend, at the
LLVM levels in CAMPAIGN_LEVELS, for both metamorphic variants, and its native
executable is compared with the model's oracle. `--cxx` only names the native
link driver forwarded to `gti --cxx`; no C++ is generated, compiled or used
as an oracle. Triage measures the LLVM-era stage boundaries: verified MIR,
LLVM IR emission, native object emission and linking, then execution.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass, replace
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
from typing import Any

# The same compiler invocation/resource contract as the existing typed audit.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
# The repository's single definition of LLVM execution modes and their flags.
sys.path.append(str(Path(__file__).resolve().parents[2] / "tests"))
from backend_modes import llvm_modes, optimization_level
from local_language_audit import INTERNAL_FAILURE_MARKERS, resolve_tool
from debug.feature_interaction_model import (
    FACTORS, InteractionCase, admissible, coverage, covering_array, make_case,
    negative_controls,
)


# The campaign names its levels rather than following GTI_TEST_LLVM_LEVELS:
# optimisation is part of its subject. O0 is the unoptimised pipeline; O3 runs
# every GTI-registered LLVM pass that O2 does (checked offsets and ranges,
# private atomics, SLP vectorisation) plus LLVM's most aggressive inlining and
# loop transforms. That is the slow tier's O0+O3 pair; adding O2 would cost
# half as many outcomes again for little distinct coverage.
CAMPAIGN_LEVELS = ("O0", "O3")
MODES = {(optimization_level(flags), "llvm"): flags
         for _, flags, _ in llvm_modes(levels=CAMPAIGN_LEVELS)}
# (optimization, backend), as failure records spell it.
CONFIGURATIONS = tuple(MODES)
VARIANTS = ("original", "renamed")
# Failure records written before the LLVM port named a C++ standard. The case
# and its oracle are backend-independent, so such a record replays at the same
# level on LLVM; the migration is recorded and announced, never implicit.
LEGACY_CPP_CONFIGURATIONS = {(level, standard): (level, "llvm")
                             for level in CAMPAIGN_LEVELS for standard in ("c++20", "c++23")}


def write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


@dataclass(frozen=True)
class Observation:
    stage: str
    command: list[str]
    status: int | None
    stdout: str
    stderr: str
    timed_out: bool = False

    def snapshot(self) -> tuple[int | None, str, str]:
        return self.status, self.stdout, self.stderr


def execute(command: list[str], stage: str, timeout: int, root: Path,
            records: list[Observation]) -> Observation:
    try:
        result = run_process(command, timeout)
        observed = Observation(stage, command, result.returncode, result.stdout, result.stderr)
    except subprocess.TimeoutExpired as error:
        decode = lambda value: value.decode("utf-8", errors="replace") if isinstance(value, bytes) else value or ""
        observed = Observation(stage, command, None, decode(error.stdout), decode(error.stderr), True)
    records.append(observed)
    prefix = root / f"{len(records):03d}-{stage}"
    prefix.with_suffix(".stdout").write_text(observed.stdout, encoding="utf-8")
    prefix.with_suffix(".stderr").write_text(observed.stderr, encoding="utf-8")
    write_json(root / "observations.json", [asdict(record) for record in records])
    return observed


def run_process(command: list[str], timeout: int) -> subprocess.CompletedProcess[str]:
    """A timed-out compiler must not leave its native compiler/linker running."""
    with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, start_new_session=os.name != "nt") as process:
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            if os.name == "nt":
                # taskkill is the Windows process-tree counterpart to killpg.
                subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                               capture_output=True, timeout=5, check=False)
            else:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            process.kill()
            stdout, stderr = process.communicate()
            raise subprocess.TimeoutExpired(command, timeout, stdout, stderr)
    return subprocess.CompletedProcess(command, process.returncode, stdout, stderr)


def compiler_failure(observed: Observation) -> dict[str, Any] | None:
    text = observed.stdout + observed.stderr
    if observed.timed_out:
        return {"kind": "timeout", "stage": observed.stage}
    if observed.status is not None and observed.status < 0:
        return {"kind": "signal", "stage": observed.stage, "signal": -observed.status}
    code = re.search(r"error\[(GTI-[A-Z][0-9]{4})\]: ([^\n]+)", text)
    if any(marker.casefold() in text.casefold() for marker in INTERNAL_FAILURE_MARKERS):
        # IDs and literal values can change while shrinking the same compiler
        # invariant. Preserve the diagnostic reason, not incidental numbers.
        detail = re.search(r"^Detail:[ \t]*(.+)$", text, re.MULTILINE)
        cause = code[2] if code else "internal failure"
        if code and code[1] == "GTI-B0001" and detail:
            cause = detail[1]
        reason = normalize_reason(cause)
        return {"kind": "internal", "stage": observed.stage,
                "code": code[1] if code else None, "reason": reason}
    if observed.status != 0:
        reason = normalize_reason(code[2]) if code else text.strip()[:300]
        if not code and "gti: backend diagnostics:\n" in text:
            output = text.split("gti: backend diagnostics:\n", 1)[1]
            output = output.split("gti: backend failed while compiling and linking", 1)[0]
            error = re.search(r"\berror:[ \t]*([^\n]+)", output)
            # Do not let progress/argv paths replace the native tool's cause.
            reason = normalize_reason(error[1] if error else output.strip()[:300])
        return {"kind": "rejected-valid" if code else "build-failed",
                "stage": observed.stage, "code": code[1] if code else None,
                "reason": reason}
    return None


def normalize_reason(reason: str) -> str:
    reason = re.sub(r"\b[0-9]+\b", "#", reason)
    return re.sub(r"(__gti_(?:[a-z]+_)+)[0-9]+", r"\1#", reason)


def disagreement(expected: tuple[int, str, str], observed: Observation) -> dict[str, Any] | None:
    if observed.timed_out:
        return {"kind": "timeout", "stage": "run"}
    if observed.status is not None and observed.status < 0:
        return {"kind": "signal", "stage": "run", "signal": -observed.status}
    channels = [name for name, wanted, actual in zip(
        ("status", "stdout", "stderr"), expected, observed.snapshot()) if wanted != actual]
    return {"kind": "oracle-mismatch", "stage": "run", "channels": channels} if channels else None


def build_command(gti: str, cxx: str | None, source: Path, output: Path,
                  configuration: tuple[str, str], *emit: str) -> list[str]:
    """The direct LLVM compile; `emit` selects an emission stage instead of a link.

    The driver rejects `--backend` beside an explicit emission mode: verified
    MIR is backend-independent and `--emit-llvm` is the LLVM backend's own
    output, so an emission keeps only the level.
    """
    if emit:
        return [gti, str(source), f"-{configuration[0]}", *emit, "--verbose", "-o", str(output)]
    command = [gti, str(source), *MODES[configuration]]
    if cxx:
        command += ["--cxx", cxx]  # the native link driver, nothing else
    # Developer evidence must retain the real backend reason/tool output,
    # rather than classifying every neutral backend summary as the same bug.
    return command + ["--verbose", "-o", str(output)]


def probe(case: InteractionCase, gti: str, cxx: str | None, root: Path, *,
          variant: str, configuration: tuple[str, str], timeout: int,
          repeat: int = 2) -> dict[str, Any] | None:
    root.mkdir(parents=True)
    write_json(root / "case.json", case.record())
    source = root / f"{variant}.gti"
    source.write_text(case.emit(variant), encoding="utf-8")
    executable = root / ("program.exe" if sys.platform == "win32" else "program")
    command = build_command(gti, cxx, source, executable, configuration)
    records: list[Observation] = []
    built = execute(command, "build", timeout, root, records)
    signature = compiler_failure(built)
    runs: list[Observation] = []
    if signature is None and not executable.is_file():
        signature = {"kind": "missing-artifact", "stage": "build"}
    if signature is None:
        for _ in range(repeat):
            runs.append(execute([str(executable)], "run", timeout, root, records))
        if len({run.snapshot() for run in runs}) > 1:
            signature = {"kind": "nondeterministic", "stage": "run"}
        else:
            signature = disagreement(case.oracle(), runs[0])
    if signature is None:
        return None
    failure = {
        "schema": "gti-feature-interaction-failure-v1",
        "signature": signature, "case": case.record(), "variant": variant,
        "configuration": list(configuration), "expected": list(case.oracle()),
        "resource_environment": {name: os.environ[name] for name in (
            "GTI_STDLIB_PATH", "GTI_RUNTIME_INCLUDE", "GTI_RUNTIME_LIBRARY")
            if name in os.environ},
        "observations": [asdict(record) for record in records],
    }
    write_json(root / "failure.json", failure)
    return failure


def validate_negative_controls(gti: str, timeout: int, root: Path) -> int:
    root.mkdir()
    records: list[Observation] = []
    for name, (source, code) in negative_controls().items():
        path = root / f"{name}.gti"
        path.write_text(source, encoding="utf-8")
        observed = execute([gti, str(path), "--emit-mir", "--verbose", "-o", str(root / f"{name}.mir")],
                           "negative", timeout, root, records)
        failure = compiler_failure(observed)
        if (failure is None or failure.get("kind") != "rejected-valid"
                or failure.get("code") != code):
            write_json(root / "failure.json", {"control": name, "expected_code": code,
                                               "observation": asdict(observed)})
            raise RuntimeError(f"negative control {name} expected {code}: {asdict(observed)}")
    return len(negative_controls())


def smoke_selections() -> list[tuple[bool, ...]]:
    # Owning/error/propagation transport, its copyable/manual-check counterpart,
    # and a minimal control. The smoke is not claimed as full pairwise coverage.
    second = {"lambda", "expected", "payload_enum", "vector", "borrow", "cleanup"}
    return [tuple(True for _ in FACTORS), tuple(name in second for name in FACTORS),
            tuple(False for _ in FACTORS)]


def suite_cases(seed: int, strength: int, smoke: bool, extra: int) -> list[InteractionCase]:
    selections = smoke_selections() if smoke else covering_array(seed, strength)
    # Extra cases revisit covered shapes with fresh data, not a claim of newly
    # covered interactions. Each index participates in the stable seed recipe.
    selections += [selections[index % len(selections)] for index in range(extra)]
    return [make_case(seed, index, row) for index, row in enumerate(selections)]


def fresh_output(requested: str | None) -> Path:
    if requested:
        root = Path(requested).expanduser().absolute()
        root.mkdir(parents=True, exist_ok=False)
        return root
    return Path(tempfile.mkdtemp(prefix="gti-feature-interactions-"))


def tool_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--gti", help="optimized GTI compiler (Release/RelWithDebInfo/MinSizeRel)")
    parser.add_argument("--cxx", help="native link driver forwarded to `gti --cxx` "
                        "(default: the compiler's own discovery); no C++ is compiled")
    parser.add_argument("--timeout", type=int, default=60, help="per build/run timeout in seconds")
    parser.add_argument("--out", help="new output directory; never overwrite an existing run")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    sub = result.add_subparsers(dest="command", required=True)
    for name in ("generate", "run"):
        command = sub.add_parser(name)
        command.add_argument("--seed", type=int, default=20261003)
        command.add_argument("--strength", type=int, choices=(2, 3), default=2)
        command.add_argument("--smoke", action="store_true")
        command.add_argument("--extra-cases", type=int, default=0)
        if name == "run":
            tool_arguments(command)
            command.add_argument("--keep-going", action="store_true")
            command.add_argument("--repeat", type=int, default=2)
        else:
            command.add_argument("--out", required=True)
    for name in ("replay", "reduce", "triage"):
        command = sub.add_parser(name)
        tool_arguments(command)
        command.add_argument("--failure", required=True, type=Path, help="retained failure.json")
        if name == "reduce":
            command.add_argument("--max-probes", type=int, default=40)
    return result


def read_failure(path: Path) -> tuple[InteractionCase, dict[str, Any]]:
    record = json.loads(path.read_text(encoding="utf-8"))
    if (record.get("schema") != "gti-feature-interaction-failure-v1"
            or record.get("variant") not in VARIANTS):
        raise ValueError("unsupported interaction failure")
    configuration = tuple(record.get("configuration", ()))
    if configuration in LEGACY_CPP_CONFIGURATIONS:
        record["migrated_configuration"] = {
            "from": list(configuration),
            "to": list(LEGACY_CPP_CONFIGURATIONS[configuration]),
            "reason": "the C++ backend is frozen and untested; replayed on LLVM at the same level"}
        record["configuration"] = list(LEGACY_CPP_CONFIGURATIONS[configuration])
    elif configuration not in CONFIGURATIONS:
        raise ValueError(f"unsupported interaction configuration {list(configuration)}; "
                         f"this runner replays {[list(item) for item in CONFIGURATIONS]}")
    case = InteractionCase.from_record(record["case"])
    if record["expected"] != list(case.oracle()):
        raise ValueError("failure oracle disagrees with case")
    if "reason" in record["signature"]:
        record["signature"]["reason"] = normalize_reason(record["signature"]["reason"])
    return case, record


def grouped_findings(failures: list[tuple[str, dict[str, Any]]]) -> list[dict[str, Any]]:
    """Group equal diagnostic predicates, not presumed compiler root causes."""
    groups: dict[str, dict[str, Any]] = {}
    for path, failure in failures:
        key = json.dumps(failure["signature"], sort_keys=True)
        group = groups.setdefault(key, {"signature": failure["signature"],
                                        "occurrences": 0, "cases": [], "evidence": []})
        group["occurrences"] += 1
        index = failure["case"]["index"]
        if index not in group["cases"]:
            group["cases"].append(index)
        group["evidence"].append(path)
    return list(groups.values())


# The direct driver's LLVM-era stage boundaries, in pipeline order. Semantic
# analysis has no direct-file emission of its own (`gti check` is project-only),
# so it is read from the verified-MIR probe's diagnostic family instead.
TRIAGE_STAGES = (("mir", "--emit-mir", ".mir"), ("llvm-ir", "--emit-llvm", ".ll"))
FRONTEND_CODE = re.compile(r"GTI-[ILPS][0-9]{4}")


def triage_failure(case: InteractionCase, original: dict[str, Any], root: Path,
                   gti: str, cxx: str | None, timeout: int) -> bool:
    """Reproduce first, then measure each stage without guessing from an ICE."""
    variant = original["variant"]
    configuration = tuple(original["configuration"])
    repeated = probe(case, gti, cxx, root / "replay", variant=variant,
                     configuration=configuration, timeout=timeout)
    status = ("disappeared" if repeated is None else "reproduced"
              if repeated["signature"] == original["signature"] else "replaced")
    source = root / "program.gti"
    source.write_text(case.emit(variant), encoding="utf-8")
    write_json(root / "case.json", case.record())
    records: list[Observation] = []
    execute([gti, "--version"], "version", timeout, root, records)
    phases: dict[str, Any] = {}
    for stage, option, suffix in TRIAGE_STAGES:
        artifact = root / ("program" + suffix)
        observed = execute(build_command(gti, cxx, source, artifact, configuration, option),
                           stage, timeout, root, records)
        signature = compiler_failure(observed)
        phases[stage] = {"accepted": signature is None and artifact.is_file(),
                         "artifact": str(artifact) if artifact.is_file() else None,
                         "signature": signature}
    # Object emission and linking have no separate driver output; the replay's
    # full build is that probe, and its executable the run probe.
    current = repeated["signature"] if repeated else None
    linked = current is None or current["stage"] == "run"
    phases["native-link"] = {"accepted": linked, "evidence": str(root / "replay"),
                             "signature": None if linked else current}
    phases["run"] = {"accepted": current is None, "evidence": str(root / "replay"),
                     "signature": current if linked else None}
    mir = phases["mir"]
    if status != "reproduced":
        boundary = "not-reproduced"
    elif not mir["accepted"]:
        frontend = (mir["signature"] and mir["signature"]["kind"] == "rejected-valid"
                    and FRONTEND_CODE.fullmatch(mir["signature"].get("code") or ""))
        boundary = "semantic-admission" if frontend else "before-verified-mir"
    elif not phases["llvm-ir"]["accepted"]:
        boundary = "llvm-ir-emission"
    elif original["signature"]["stage"] == "build":
        boundary = "native-object-or-link"
    else:
        boundary = "native-execution"
    result = {"schema": "gti-feature-interaction-triage-v2", "status": status,
              "observed_boundary": boundary, "original_signature": original["signature"],
              "current_signature": current, "configuration": list(configuration),
              "phases": phases,
              "resource_environment": {name: os.environ[name] for name in (
                  "GTI_STDLIB_PATH", "GTI_RUNTIME_INCLUDE", "GTI_RUNTIME_LIBRARY")
                  if name in os.environ},
              "scope": "measured stage boundary; not proof of the root cause"}
    if "migrated_configuration" in original:
        result["migrated_configuration"] = original["migrated_configuration"]
    write_json(root / "triage.json", result)
    print(f"{status}: {boundary}; evidence: {root / 'triage.json'}", flush=True)
    return status == "reproduced"


def reduction_candidates(case: InteractionCase) -> list[InteractionCase]:
    candidates: list[InteractionCase] = []
    for index, enabled in enumerate(case.selection):
        if enabled:
            row = list(case.selection)
            row[index] = False
            if FACTORS[index] == "expected":
                row[FACTORS.index("propagate")] = False
            if admissible(tuple(row)):
                candidates.append(replace(case, selection=tuple(row)))
    for name, smallest in (("base", 1), ("delta", 1), ("width", 2)):
        if getattr(case, name) != smallest:
            candidates.append(replace(case, **{name: smallest}))
    return candidates


def reduce_failure(case: InteractionCase, original: dict[str, Any], root: Path,
                   gti: str, cxx: str | None, timeout: int, max_probes: int) -> bool:
    options = {"variant": original["variant"],
               "configuration": tuple(original["configuration"]), "timeout": timeout}
    initial = probe(case, gti, cxx, root / "probe-000", **options)
    if initial is None or initial["signature"] != original["signature"]:
        write_json(root / "reduction.json", {"status": "not-reproduced"})
        return False
    best = case
    best_failure = initial
    probes = 1
    seen = {json.dumps(best.record(), sort_keys=True)}
    exhausted = False
    while True:
        changed = False
        for candidate in reduction_candidates(best):
            identity = json.dumps(candidate.record(), sort_keys=True)
            if identity in seen:
                continue
            if probes >= max_probes:
                exhausted = True
                break
            seen.add(identity)
            observed = probe(candidate, gti, cxx, root / f"probe-{probes:03d}", **options)
            probes += 1
            if observed is not None and observed["signature"] == original["signature"]:
                best, best_failure = candidate, observed
                changed = True
                break
        if exhausted or not changed:
            break
    (root / "reduced.gti").write_text(best.emit(original["variant"]), encoding="utf-8")
    write_json(root / "case.json", best.record())
    write_json(root / "failure.json", best_failure)
    write_json(root / "reduction.json", {
        "schema": "gti-feature-interaction-reduction-v1",
        "status": "probe-budget" if exhausted else "bounded-fixed-point",
        "probes": probes, "original_bytes": len(case.emit(original["variant"]).encode()),
        "reduced_bytes": len(best.emit(original["variant"]).encode()),
        "predicate": original["signature"],
        "scope": "feature removal and bounded data shrinking; not token minimality",
        "configuration": list(options["configuration"]),
        **({"migrated_configuration": original["migrated_configuration"]}
           if "migrated_configuration" in original else {}),
    })
    print(f"reduced in {probes} probes; replayable failure: {root / 'failure.json'}", flush=True)
    return True


def main(arguments: list[str] | None = None) -> int:
    cli = parser()
    args = cli.parse_args(arguments)
    if getattr(args, "extra_cases", 0) < 0 or getattr(args, "repeat", 1) < 1:
        cli.error("extra-cases must be nonnegative and repeat must be positive")
    if getattr(args, "timeout", 1) < 1 or getattr(args, "max_probes", 1) < 1:
        cli.error("timeout and max-probes must be positive")
    # Validate supplied evidence before creating outputs or executing tools.
    prior = read_failure(args.failure) if args.command in ("replay", "reduce", "triage") else None
    if args.command != "generate":
        gti = resolve_tool(args.gti, "gti", "GTI compiler")
        cxx = resolve_tool(args.cxx, "c++", "native link driver") if args.cxx else None
    root = fresh_output(args.out)
    print(f"evidence: {root}", flush=True)
    if prior:
        case, record = prior
        if "migrated_configuration" in record:
            migration = record["migrated_configuration"]
            print(f"note: C++-era configuration {migration['from']} replays as {migration['to']}",
                  flush=True)
        if args.command == "reduce":
            return 0 if reduce_failure(case, record, root, gti, cxx, args.timeout, args.max_probes) else 1
        if args.command == "triage":
            return 0 if triage_failure(case, record, root, gti, cxx, args.timeout) else 1
        observed = probe(case, gti, cxx, root / "replay", variant=record["variant"],
                         configuration=tuple(record["configuration"]), timeout=args.timeout)
        status = ("disappeared" if observed is None else "reproduced"
                  if observed["signature"] == record["signature"] else "replaced")
        write_json(root / "replay.json", {
            "status": status, "configuration": record["configuration"],
            **({"migrated_configuration": record["migrated_configuration"]}
               if "migrated_configuration" in record else {})})
        print(status, flush=True)
        return 0 if status == "reproduced" else 1

    cases = suite_cases(args.seed, args.strength, args.smoke, args.extra_cases)
    plan = {
        "schema": "gti-feature-interaction-suite-v1", "seed": args.seed,
        "configurations": [list(item) for item in CONFIGURATIONS], "variants": list(VARIANTS),
        "cases": [case.record() for case in cases],
        "planned_coverage": coverage([case.selection for case in cases], args.strength),
    }
    write_json(root / "suite.json", plan)
    if args.command == "generate":
        for case in cases:
            directory = root / f"case-{case.index:03d}"
            directory.mkdir()
            write_json(directory / "case.json", case.record())
            for variant in VARIANTS:
                (directory / f"{variant}.gti").write_text(case.emit(variant), encoding="utf-8")
        print(f"generated {len(cases)} cases; coverage {plan['planned_coverage']['covered']}/"
              f"{plan['planned_coverage']['required']}", flush=True)
        return 0

    version_records: list[Observation] = []
    execute([gti, "--version"], "version", args.timeout, root, version_records)
    verified = []
    failures = []
    failure_records = []
    outcomes = 0
    controls = validate_negative_controls(gti, args.timeout, root / "negative-controls")
    for case in cases:
        case_failed = False
        for variant in VARIANTS:
            for configuration in CONFIGURATIONS:
                directory = root / f"case-{case.index:03d}" / f"{variant}-{configuration[0]}-{configuration[1]}"
                failure = probe(case, gti, cxx, directory, variant=variant,
                                configuration=configuration, timeout=args.timeout, repeat=args.repeat)
                outcomes += 1
                if failure:
                    case_failed = True
                    failures.append(str(directory / "failure.json"))
                    failure_records.append((str(directory / "failure.json"), failure))
                    print(f"FAIL case {case.index} {variant} {configuration}: {failure['signature']}\n"
                          f"  {directory / 'failure.json'}", flush=True)
                    if not args.keep_going:
                        break
            if case_failed and not args.keep_going:
                break
        if not case_failed:
            verified.append(case.selection)
            print(f"PASS case {case.index} ({', '.join(name for name in FACTORS if case.enabled(name)) or 'control'})", flush=True)
        report = {
            "schema": "gti-feature-interaction-report-v1", "seed": args.seed,
            "configurations": [list(item) for item in CONFIGURATIONS],
            "planned_cases": len(cases), "verified_cases": len(verified),
            "native_outcomes": outcomes, "negative_controls": controls,
            "failures": failures,
            "findings": grouped_findings(failure_records),
            "verified_coverage": coverage(verified, args.strength),
        }
        write_json(root / "report.json", report)
        if case_failed and not args.keep_going:
            break
    measured = report["verified_coverage"]
    print(f"{len(verified)}/{len(cases)} cases passed; {outcomes} native outcomes; "
          f"verified interactions {measured['covered']}/{measured['required']}", flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, KeyError, TypeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
