#!/usr/bin/env python3

"""Validate and execute the source-backed GTI stdlib surface manifest."""

from __future__ import annotations

import hashlib
import argparse
import json
import os
import pathlib
import subprocess
import sys
import tempfile
from typing import Any

from case_jobs import configured_jobs, positive_jobs, run_cases
from native_fixture_build import build_fixture
from llvm_case_context import CaseContext, source_digest
from internal_module_fixture import private_library, private_source


SCHEMA = "gti-stdlib-surface-v1"
STATUSES = {"executable", "scaffold", "unsupported"}
CASE_MODES = {"run", "diagnostic"}
# Each native build of a case has this limit unless the case records a longer,
# justified one. The ceiling keeps a raised limit from hiding a hang.
DEFAULT_BUILD_TIMEOUT_SECONDS = 300
MAX_BUILD_TIMEOUT_SECONDS = 900


class ManifestError(ValueError):
    """Raised when the checked-in surface census is incomplete or incoherent."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ManifestError(message)


def repository_path(repository: pathlib.Path, value: object, label: str) -> pathlib.Path:
    require(isinstance(value, str) and value != "", f"{label} must be a path")
    candidate = (repository / value).resolve()
    try:
        candidate.relative_to(repository)
    except ValueError as error:
        raise ManifestError(f"{label} escapes the repository: {value}") from error
    require(candidate.is_file(), f"{label} does not name a file: {value}")
    return candidate


def stdlib_sources(repository: pathlib.Path) -> list[pathlib.Path]:
    return [repository / "stdlib" / "prelude.gti", repository / "stdlib/std.gti", *sorted(
        (repository / "stdlib" / "std").rglob("*.gti")
    )]


def source_digest(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_manifest(path: pathlib.Path) -> dict[str, Any]:
    try:
        loaded = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ManifestError(f"cannot read manifest {path}: {error}") from error
    require(isinstance(loaded, dict), "manifest root must be an object")
    return loaded


def validate_manifest(manifest: dict[str, Any], repository: pathlib.Path) -> None:
    repository = repository.resolve()
    require(manifest.get("schema") == SCHEMA, f"schema must be {SCHEMA}")

    fingerprints = manifest.get("source_fingerprints")
    require(isinstance(fingerprints, dict), "source_fingerprints must be an object")
    expected_sources = {
        path.relative_to(repository).as_posix() for path in stdlib_sources(repository)
    }
    actual_sources = set(fingerprints)
    missing_sources = sorted(expected_sources - actual_sources)
    extra_sources = sorted(actual_sources - expected_sources)
    require(
        not missing_sources and not extra_sources,
        "stdlib source census differs: "
        f"missing={missing_sources}, extra={extra_sources}",
    )
    require(
        list(fingerprints) == sorted(fingerprints),
        "source_fingerprints keys must be sorted",
    )
    for relative, expected in fingerprints.items():
        require(
            isinstance(expected, str) and len(expected) == 64,
            f"invalid SHA-256 for {relative}",
        )
        actual = source_digest(repository / relative)
        require(
            actual == expected,
            f"stdlib source changed without a surface review: {relative} "
            f"(expected {expected}, found {actual})",
        )

    cases = manifest.get("cases")
    require(isinstance(cases, list) and cases, "cases must be a nonempty array")
    case_ids: set[str] = set()
    case_by_id: dict[str, dict[str, Any]] = {}
    for case in cases:
        require(isinstance(case, dict), "each case must be an object")
        case_id = case.get("id")
        require(isinstance(case_id, str) and case_id, "case id must be nonempty")
        require(case_id not in case_ids, f"duplicate case id: {case_id}")
        case_ids.add(case_id)
        case_by_id[case_id] = case
        require(case.get("backend") in {None, "cpp", "llvm"}, f"invalid backend for {case_id}")
        mode = case.get("mode")
        require(mode in CASE_MODES, f"invalid case mode for {case_id}: {mode}")
        require(
            case.get("execution_profile") in {None, "single-threaded", "concurrent"},
            f"invalid execution profile for {case_id}",
        )
        repository_path(repository, case.get("source"), f"case {case_id} source")
        private_modules = case.get("private_modules", [])
        require(isinstance(private_modules, list) and
                all(module in {"detail_iterator", "detail.map_entry", "detail.memory_storage"}
                    for module in private_modules),
                f"{case_id} must name an explicit supported white-box module setup")
        if "build_timeout_seconds" in case or "build_timeout_reason" in case:
            timeout = case.get("build_timeout_seconds")
            require(
                isinstance(timeout, int)
                and not isinstance(timeout, bool)
                and DEFAULT_BUILD_TIMEOUT_SECONDS
                < timeout
                <= MAX_BUILD_TIMEOUT_SECONDS,
                f"{case_id} build_timeout_seconds must raise the "
                f"{DEFAULT_BUILD_TIMEOUT_SECONDS}-second limit to at most "
                f"{MAX_BUILD_TIMEOUT_SECONDS}",
            )
            reason = case.get("build_timeout_reason")
            require(
                isinstance(reason, str) and reason.strip() != "",
                f"{case_id} build_timeout_seconds needs a build_timeout_reason",
            )
        native_sources = case.get("native_sources", [])
        require(isinstance(native_sources, list), f"{case_id} native_sources must be a list")
        for index, source in enumerate(native_sources):
            repository_path(
                repository, source, f"case {case_id} native_sources[{index}]"
            )
        files = case.get("files", {})
        require(isinstance(files, dict), f"{case_id} files must be an object")
        for name, contents in files.items():
            require(
                isinstance(name, str)
                and name
                and pathlib.PurePosixPath(name).name == name,
                f"{case_id} file names must be single safe path components",
            )
            require(
                isinstance(contents, dict)
                and contents.get("encoding") == "hex"
                and isinstance(contents.get("contents"), str),
                f"{case_id} file {name} must contain hexadecimal bytes",
            )
            try:
                bytes.fromhex(contents["contents"])
            except ValueError as error:
                raise ManifestError(f"{case_id} file {name} has invalid hex") from error

        expected = case.get("expected")
        require(isinstance(expected, dict), f"{case_id} expected must be an object")
        require(
            isinstance(expected.get("exit_code"), int),
            f"{case_id} expected exit_code must be an integer",
        )
        if mode == "run":
            require(
                case.get("optimizations") == ["O0", "O3"],
                f"run case {case_id} must cover O0 and O3",
            )
            require(
                set(expected) == {"exit_code", "stdout", "stderr"}
                and isinstance(expected["stdout"], str)
                and isinstance(expected["stderr"], str),
                f"run case {case_id} needs exact exit/stdout/stderr",
            )
            require(expected["exit_code"] == 0, f"run case {case_id} must succeed")
        else:
            require(
                case.get("optimization") in {"O0", "O3"},
                f"diagnostic case {case_id} needs one bounded optimization",
            )
            fragments = expected.get("stderr_contains")
            require(
                expected["exit_code"] != 0
                and isinstance(fragments, list)
                and fragments
                and all(isinstance(item, str) and item for item in fragments),
                f"diagnostic case {case_id} needs failure fragments",
            )

    require(
        [case["id"] for case in cases] == sorted(case_ids),
        "cases must be sorted by id",
    )

    entries = manifest.get("entries")
    require(isinstance(entries, list) and entries, "entries must be a nonempty array")
    entry_ids: set[str] = set()
    operations: set[str] = set()
    represented_sources: set[str] = set()
    referenced_cases: set[str] = set()
    for entry in entries:
        require(isinstance(entry, dict), "each entry must be an object")
        entry_id = entry.get("id")
        require(isinstance(entry_id, str) and entry_id, "entry id must be nonempty")
        require(entry_id not in entry_ids, f"duplicate entry id: {entry_id}")
        entry_ids.add(entry_id)
        status = entry.get("status")
        require(status in STATUSES, f"invalid status for {entry_id}: {status}")
        source = entry.get("source")
        require(source in fingerprints, f"{entry_id} has unknown stdlib source: {source}")
        represented_sources.add(source)

        entry_operations = entry.get("operations")
        require(
            isinstance(entry_operations, list)
            and entry_operations
            and all(isinstance(item, str) and item for item in entry_operations),
            f"{entry_id} must list operations",
        )
        require(
            entry_operations == sorted(entry_operations),
            f"{entry_id} operations must be sorted",
        )
        duplicates = operations.intersection(entry_operations)
        require(not duplicates, f"operations appear twice: {sorted(duplicates)}")
        operations.update(entry_operations)

        references = entry.get("case_ids", [])
        require(
            isinstance(references, list)
            and all(isinstance(item, str) and item in case_by_id for item in references),
            f"{entry_id} references an unknown case",
        )
        referenced_cases.update(references)
        evidence = entry.get("evidence", [])
        require(
            isinstance(evidence, list)
            and evidence
            and all(isinstance(item, str) and item for item in evidence),
            f"{entry_id} must cite evidence",
        )
        instantiations = entry.get("required_instantiations", [])
        require(
            isinstance(instantiations, list)
            and all(isinstance(item, str) and item for item in instantiations),
            f"{entry_id} required_instantiations must be a list",
        )
        if status == "executable":
            require(references, f"executable entry {entry_id} has no run case")
            require(
                all(case_by_id[case_id]["mode"] == "run" for case_id in references),
                f"executable entry {entry_id} references a non-run case",
            )
            require("reason" not in entry, f"executable entry {entry_id} has a reason")
        else:
            require(
                isinstance(entry.get("reason"), str) and entry["reason"],
                f"{status} entry {entry_id} needs a reason",
            )
            if status == "scaffold":
                require(not references, f"scaffold entry {entry_id} must not run")
            else:
                require(
                    all(
                        case_by_id[case_id]["mode"] == "diagnostic"
                        for case_id in references
                    ),
                    f"unsupported entry {entry_id} references a run case",
                )

    require(
        [entry["id"] for entry in entries] == sorted(entry_ids),
        "entries must be sorted by id",
    )
    require(
        represented_sources == expected_sources,
        "every stdlib source must have at least one classified entry; "
        f"missing={sorted(expected_sources - represented_sources)}",
    )
    require(
        referenced_cases == case_ids,
        f"every case must prove an entry; unreferenced={sorted(case_ids - referenced_cases)}",
    )


def compiler_environment_overrides(
    repository: pathlib.Path, runtime_library: pathlib.Path
) -> dict[str, str]:
    return {
        "GTI_STDLIB_PATH": str(repository / "stdlib"),
        "GTI_RUNTIME_INCLUDE": str(repository / "runtime" / "include"),
        "GTI_RUNTIME_LIBRARY": str(runtime_library),
    }


def compiler_environment(
    repository: pathlib.Path, runtime_library: pathlib.Path
) -> dict[str, str]:
    environment = os.environ.copy()
    environment.update(compiler_environment_overrides(repository, runtime_library))
    return environment


def case_plan(
    case: dict[str, Any],
    compiler: pathlib.Path,
    cxx: pathlib.Path,
    runtime_library: pathlib.Path,
    repository: pathlib.Path,
    scratch: pathlib.Path,
) -> dict[str, Any]:
    case_root = scratch / case["id"]
    source = repository / case["source"]
    native_sources = [repository / item for item in case.get("native_sources", [])]
    environment = compiler_environment(repository, runtime_library)
    if case.get("private_modules"):
        source = case_root / source.name
        environment["GTI_STDLIB_PATH"] = str(case_root / "stdlib")
    optimizations = (
        case["optimizations"] if case["mode"] == "run" else [case["optimization"]]
    )
    steps = []
    for optimization in optimizations:
        executable = case_root / f"program-{optimization}"
        command = [
            str(compiler),
            str(source),
            "--cxx",
            str(cxx),
            f"-{optimization}",
        ]
        if case.get("backend") is not None:
            command.extend(["--backend", case["backend"]])
        if case.get("execution_profile") is not None:
            command.extend(
                ["--execution-profile", str(case["execution_profile"])]
            )
        steps.append({
            "optimization": optimization, "command": command, "executable": executable,
            "build_options": {
                "native_sources": native_sources, "optimization": optimization, "cxx": cxx,
                "cwd": case_root, "env": environment, "text": True, "capture_output": True,
                "timeout": case.get("build_timeout_seconds", DEFAULT_BUILD_TIMEOUT_SECONDS),
                "check": False,
            },
            "run_command": [str(executable)],
            "run_options": {"cwd": case_root, "input": case.get("stdin", ""),
                            "text": True, "capture_output": True, "timeout": 120, "check": False},
        })
    return {
        "case_root": case_root, "mode": case["mode"], "expected": case["expected"],
        "files": {name: bytes.fromhex(record["contents"])
                  for name, record in case.get("files", {}).items()},
        "steps": steps,
    }


def case_role(case: dict[str, Any]) -> str:
    return "positive-admission" if case["mode"] == "run" else "expected-rejection-control"


def case_context(case: dict[str, Any], repository: pathlib.Path) -> dict:
    # Tokens preserve the invocation shape without leaking ambient environment
    # values or pretending that this native fixture contract is an LLVM recipe.
    root, runtime = pathlib.Path("@REPOSITORY"), pathlib.Path("@RUNTIME_LIBRARY")
    planned = case_plan(case, pathlib.Path("@COMPILER"), pathlib.Path("@CXX"),
                        runtime, root, pathlib.Path("@SCRATCH"))
    context = CaseContext(
        provider="stdlib-surface", identity=case["id"], source=case["source"],
        source_sha256=source_digest(repository / case["source"]),
        execution_profile=case.get("execution_profile") or "single-threaded",
        loading="stdlib-surface-native-fixture-with-compiler-overrides",
        observation=f"stdlib-surface-{case['mode']}",
        unresolved=("llvm-target-applicability", "native-fixture-build-contract",
                    *(("implicit-shipped-backend",) if not case.get("backend") else ())),
    )
    return {
        "context": context.record(), "role": case_role(case), "observed": "unrun",
        "case": case,
        "compiler_environment": {"policy": "copy-inherited-and-override",
                                 "overrides": {**compiler_environment_overrides(root, runtime),
                                     **({"GTI_STDLIB_PATH": str(planned["case_root"] / "stdlib")}
                                        if case.get("private_modules") else {})}},
        "runtime_environment": "inherited-separately",
        "steps": [{
            "optimization": step["optimization"], "command": step["command"],
            "executable": str(step["executable"]),
            "native_sources": [str(path) for path in step["build_options"]["native_sources"]],
            "build_options": {
                key: ([str(path) for path in value] if key == "native_sources" else
                      str(value) if isinstance(value, pathlib.Path) else value)
                for key, value in step["build_options"].items() if key != "env"
            },
            "cwd": str(step["build_options"]["cwd"]),
            "build_timeout": step["build_options"]["timeout"],
            "native_builder": "native_fixture_build.build_fixture",
            "runtime": ({"command": step["run_command"],
                         "options": {key: str(value) if isinstance(value, pathlib.Path) else value
                                     for key, value in step["run_options"].items()}}
                        if case["mode"] == "run" else None),
        } for step in planned["steps"]],
    }


def run_case(
    case: dict[str, Any], compiler: pathlib.Path, cxx: pathlib.Path,
    runtime_library: pathlib.Path, repository: pathlib.Path, scratch: pathlib.Path,
) -> None:
    plan = case_plan(case, compiler, cxx, runtime_library, repository, scratch)
    plan["case_root"].mkdir()
    if case.get("private_modules"):
        private_library(repository, plan["case_root"] / "stdlib", case["private_modules"])
        source = repository / case["source"]
        private_source(source, plan["case_root"] / source.name, case["private_modules"])
    for name, contents in plan["files"].items():
        (plan["case_root"] / name).write_bytes(contents)
    for step in plan["steps"]:
        optimization = step["optimization"]
        built = build_fixture(step["command"], step["executable"], **step["build_options"])
        expected = plan["expected"]
        if plan["mode"] == "diagnostic":
            if built.returncode != expected["exit_code"] or any(
                fragment not in built.stderr
                for fragment in expected["stderr_contains"]
            ):
                raise AssertionError(
                    f"diagnostic case {case['id']} {optimization} drifted: "
                    f"exit={built.returncode}\nstdout:\n{built.stdout}\nstderr:\n{built.stderr}"
                )
            continue
        if built.returncode != 0:
            raise AssertionError(
                f"run case {case['id']} {optimization} did not build: "
                f"exit={built.returncode}\nstdout:\n{built.stdout}\nstderr:\n{built.stderr}"
            )
        executed = subprocess.run(step["run_command"], **step["run_options"])
        actual = (executed.returncode, executed.stdout, executed.stderr)
        wanted = (
            expected["exit_code"],
            expected["stdout"],
            expected["stderr"],
        )
        if actual != wanted:
            raise AssertionError(
                f"run case {case['id']} {optimization} drifted: "
                f"wanted={wanted!r}, actual={actual!r}"
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("cxx", type=pathlib.Path)
    parser.add_argument("runtime_library", type=pathlib.Path)
    parser.add_argument("repository", type=pathlib.Path)
    parser.add_argument("--validate-only", action="store_true")
    parser.add_argument("--jobs", type=positive_jobs, default=configured_jobs(),
                        help="Concurrent isolated cases; default: serial outside CTest.")
    arguments = parser.parse_args()
    compiler = arguments.compiler.resolve()
    # Preserve the driver spelling: resolving a clang++ symlink to `clang`
    # silently switches the native link step from C++ to C mode.
    cxx = arguments.cxx.absolute()
    runtime_library = arguments.runtime_library.resolve()
    repository = arguments.repository.resolve()
    manifest_path = repository / "tests" / "stdlib_surface_manifest.json"
    manifest = load_manifest(manifest_path)
    validate_manifest(manifest, repository)
    if arguments.validate_only:
        print(
            f"stdlib surface manifest valid: {len(manifest['entries'])} entries, "
            f"{len(manifest['cases'])} cases"
        )
        return 0

    with tempfile.TemporaryDirectory(prefix="gti-stdlib-surface-") as temporary:
        scratch = pathlib.Path(temporary)
        def execute(case):
            run_case(
                case,
                compiler,
                cxx,
                runtime_library,
                repository,
                scratch,
            )
        run_cases([(case["id"], case) for case in manifest["cases"]],
                  execute, arguments.jobs)
    executable = sum(
        entry["status"] == "executable" for entry in manifest["entries"]
    )
    print(
        f"stdlib surface passed: {executable} executable entries, "
        f"{len(manifest['cases'])} O0/O3 or diagnostic cases"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
