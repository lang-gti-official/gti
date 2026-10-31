#!/usr/bin/env python3
"""Frozen pre-extraction census plans, without compiling the example corpus."""

from dataclasses import replace
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

import example_case_context as examples
from llvm_case_context import CaseContext
import llvm_coverage_evidence_test as audit
import mir_census_test as census


# Frozen from census() at 798e67db, solely an independent regression oracle.
# Production discovery/profile selection lives only in example_case_context.
PRE_EXTRACTION_CONCURRENT = {
    "066-managed-threads.gti", "076-scalar-atomics.gti", "175-thread-local-storage.gti",
}


def frozen_plan(source: Path, gti: Path, root: Path, output: Path,
                inherited: dict[str, str]) -> tuple[list[str], dict]:
    env = dict(inherited)
    env.setdefault("GTI_STDLIB_PATH", str(root / "stdlib"))
    return ([str(gti), "--emit-cpp",
             *(["--execution-profile", "concurrent"]
               if source.name in PRE_EXTRACTION_CONCURRENT else []),
             str(source), "-o", str(output)],
            {"cwd": root, "capture_output": True, "timeout": 180,
             "check": False, "env": env})


def equivalent(cases: tuple[CaseContext, ...], root: Path) -> None:
    selected = sorted((root / "examples").glob("*.gti"))
    assert [root / case.source for case in cases] == selected, "source selection/order changed"
    for case, source in zip(cases, selected, strict=True):
        assert case.provider == "shipped-examples" and case.identity == case.source
        assert case.source_sha256 == hashlib.sha256(source.read_bytes()).hexdigest(), "source digest changed"
        for inherited in ({"OTHER": "preserved"}, {"GTI_STDLIB_PATH": "custom stdlib", "OTHER": "preserved"}):
            gti = Path("/controlled/compiler")
            output = Path("/controlled/census.cpp")
            expected_command, expected_options = frozen_plan(source, gti, root, output, inherited)
            assert examples.census_command(case, root, gti, output) == expected_command, "compiler arguments changed"
            assert examples.environment(root, inherited) == expected_options["env"], "environment policy changed"
            declared = examples.observation_context(case)
            assert declared["case"]["loading"] == "driver-default-prelude-with-stdlib-path-setdefault"
            native_only = source.name in examples.LLVM_ONLY_EXAMPLES
            assert declared["case"]["observation"] == (
                "sealed-lowered-census-with-cpp-rejection" if native_only else "mir-census-emit-cpp")
            assert declared["stdlib_environment"] == {
                "name": "GTI_STDLIB_PATH", "operation": "setdefault",
                "repository_default": "stdlib"}
            assert declared["timeout_seconds"] == expected_options["timeout"], "timeout changed"
            assert declared["cwd"] == "repository-root"
            expected_observation = {
                "mode": "sealed-lowered-program-inventory" if native_only else "--emit-cpp", "returncode": 0,
                "output_required": True, "oracle": "reviewed MIR body census"}
            if native_only:
                expected_observation["cpp_rejection"] = (
                    "GTI-B0003: generic fixed-array reference signatures" if source.name == "182-bounded-array-borrows.gti"
                    else "GTI-B0003: general sizeof/alignof" if source.name == "181-general-object-layout.gti"
                    else "GTI-B0003: wrapping atomic read-modify-write")
            assert declared["compile_observation"] == expected_observation
            assert declared["case"]["execution_profile"] == (
                "concurrent" if source.name in PRE_EXTRACTION_CONCURRENT else "single-threaded")
            assert set(declared["case"]["unresolved"]) == {
                "llvm-target-applicability", "native-execution-outcome",
                "native-dependencies", "native-optimization-endpoints"}


def rejected(action) -> None:
    try:
        action()
    except (AssertionError, ValueError):
        return
    raise AssertionError("context mutation escaped its equivalence control")


def main() -> None:
    helper, root, build = (Path(value).resolve() for value in sys.argv[1:4])
    contexts = examples.cases(root)
    equivalent(contexts, root)
    baseline = json.loads((root / "tests/mir_census_baseline.json").read_text())["examples"]
    assert {Path(case.source).name for case in contexts} == set(baseline)
    assert contexts and len(contexts) == len(baseline)
    active = next(index for index, case in enumerate(contexts)
                  if case.execution_profile == "concurrent")
    for changed in (
        contexts[:-1], contexts + (contexts[0],), tuple(reversed(contexts)),
        contexts[:active] + (replace(contexts[active], execution_profile="single-threaded"),) + contexts[active + 1:],
        (replace(contexts[0], execution_profile="concurrent"),) + contexts[1:],
        (replace(contexts[0], source_sha256="0" * 64),) + contexts[1:],
        (replace(contexts[0], loading="wrong-prelude"),) + contexts[1:],
    ):
        rejected(lambda: equivalent(changed, root))
    rejected(lambda: replace(contexts[0], source="../outside.gti"))
    rejected(lambda: replace(contexts[0], source="/outside.gti"))
    rejected(lambda: replace(contexts[0], execution_profile="unknown"))
    rejected(lambda: replace(contexts[0], source_sha256="bad"))
    timeout = examples.CENSUS_TIMEOUT
    examples.CENSUS_TIMEOUT = 179
    try:
        rejected(lambda: equivalent(contexts, root))
    finally:
        examples.CENSUS_TIMEOUT = timeout
    environment = examples.environment
    examples.environment = lambda root, inherited: {"GTI_STDLIB_PATH": str(root / "stdlib")}
    try:
        rejected(lambda: equivalent(contexts, root))
    finally:
        examples.environment = environment

    # Exercise the real census loop with a controlled compiler observation.
    # Every planned call is compared to the old code; no corpus compilation runs.
    selected = sorted((root / "examples").glob("*.gti"))
    calls = []
    previous_run = census.subprocess.run
    inherited = dict(os.environ)
    old_failures = dict(census.FAILURES)
    census.FAILURES.clear()

    def fake_compiler(command, **options):
        source = selected[len(calls)]
        if source.name in examples.LLVM_ONLY_EXAMPLES:
            env = examples.environment(root, inherited)
            assert options == {"capture_output": True, "text": True, "env": env, "timeout": 180}
            if command[0] == "/controlled/gti_lowered_program_census":
                assert command == ["/controlled/gti_lowered_program_census", str(root), source.name]
                calls.append(command)
                return subprocess.CompletedProcess(command, 0,
                    "generated_kinds\n" + source.name + "\t9\t2\t0\n", "")
            output = Path(command[-1])
            assert command == ["/controlled/compiler", str(source), "--std", "c++23", "-O1",
                               *(["--execution-profile", "concurrent"]
                                 if source.name in PRE_EXTRACTION_CONCURRENT else []), "-o", str(output)]
            assert output.read_bytes() == b"preserved unsupported output"
            rejection = ("GTI-B0003: generic fixed-array reference signatures"
                         if source.name == "182-bounded-array-borrows.gti"
                         else "GTI-B0003: general sizeof/alignof"
                         if source.name == "181-general-object-layout.gti"
                         else "GTI-B0003: wrapping atomic read-modify-write")
            return subprocess.CompletedProcess(command, 65, "", rejection)
        output = Path(command[-1])
        expected_command, expected_options = frozen_plan(
            source, Path(command[0]), root, output, inherited)
        assert command == expected_command and options == expected_options
        assert not output.exists(), "census did not remove the preceding observation"
        calls.append(command)
        if source == selected[1]:
            return subprocess.CompletedProcess(command, 0, b"missing artifact", b"")
        if source == selected[3]:
            return subprocess.CompletedProcess(command, 1, b"", "original diagnostic é".encode())
        output.write_text("// GTI verified-MIR body: function-instance 7\n"
                          "// GTI verified-MIR body: function-instance 7\n"
                          "// GTI verified-MIR body: lambda-instance 8\n")
        return subprocess.CompletedProcess(command, 0, b"", b"")

    census.subprocess.run = fake_compiler
    try:
        counts = census.census(Path("/controlled/compiler"), root)
        assert len(calls) == len(contexts) and len(counts) == len(contexts)
        assert counts[selected[1].name] is None and counts[selected[3].name] is None
        assert census.FAILURES[selected[1].name] == "missing artifact"
        assert census.FAILURES[selected[3].name] == "original diagnostic é"
        assert all(count == 2 for name, count in counts.items()
                   if name not in {selected[1].name, selected[3].name})
    finally:
        census.subprocess.run = previous_run
        census.FAILURES.clear()
        census.FAILURES.update(old_failures)

    # One independent real admission probe with the optimized helper. The
    # census oracle above cannot establish native execution or target support.
    report = audit.ledger(helper)
    assert report["build"]["configuration"] in {"Release", "RelWithDebInfo", "MinSizeRel"}
    observed = audit.probe_uses(helper, root, root / contexts[0].source,
                                "focused-context:example", contexts[0].execution_profile, report)
    assert observed.get("schema") == audit.USE_SCHEMA, "representative source analysis failed"
    unsupported = (observed["uses"][observed["first_unsupported"]]["base"]["key"]
                   if observed["first_unsupported"] is not None else None)
    probe_path = build / "gti_llvm_case_context_probe.json"
    probe_path.write_text(json.dumps({
        "scope": "focused-context-admission-observation", "artifact": observed,
        "native_execution": "unrun", "campaign_promotion_ready": False,
    }, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"equivalent_census_plans": len(contexts),
                      "membership_unchanged": True,
                      "representative_admission": observed["admitted"],
                      "representative_unsupported": unsupported,
                      "probe_artifact": str(probe_path),
                      "native_execution": "unrun", "campaign_promotion_ready": False}))


if __name__ == "__main__":
    main()
