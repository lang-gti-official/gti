#!/usr/bin/env python3

from __future__ import annotations

import copy
from contextlib import contextmanager
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
from typing import Any, Iterator


EXPECTED_RECORD = "GTI-BENCH-1 digest:0000000014b81952 work-units:200000000"
THREAD_EXPECTED_RECORD = (
    "GTI-BENCH-1 digest:000000097bfbb42c work-units:2800000"
)
SCHEMA_1_BASE_SHAPE = Path(__file__).with_name(
    "benchmark_schema1_base_shape.json"
)
SCHEMA_1_NULLABLE_SCALAR_TYPES = {
    "$.environment.cpu_model": "string|null",
    "$.environment.gti_toolchain_overrides.GTI_RUNTIME_INCLUDE": "string|null",
    "$.environment.gti_toolchain_overrides.GTI_RUNTIME_LIBRARY": "string|null",
    "$.environment.gti_toolchain_overrides.GTI_STDLIB_PATH": "string|null",
    "$.environment.gti_toolchain_overrides.GTI_VENDOR_INCLUDE": "string|null",
    "$.environment.logical_cpu_count": "number|null",
    "$.repository.commit": "string|null",
    "$.repository.dirty": "boolean|null",
    "$.repository.reason": "string|null",
    "$.repository.root": "string|null",
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run(
    command: list[str],
    cwd: Path,
    expected: int = 0,
    environment: dict[str, str] | None = None,
    timeout: float = 300.0,
) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        command,
        cwd=cwd,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
        timeout=timeout,
    )
    if completed.returncode != expected:
        raise AssertionError(
            f"expected exit {expected}, got {completed.returncode}: "
            f"{' '.join(command)}\nstdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    return completed


def load_runner(path: Path) -> Any:
    specification = importlib.util.spec_from_file_location(
        "gti_benchmark_compare_under_test", path
    )
    require(
        specification is not None and specification.loader is not None,
        f"failed to load benchmark runner at {path}",
    )
    module = importlib.util.module_from_spec(specification)
    sys.modules[specification.name] = module
    specification.loader.exec_module(module)
    return module


def descriptor_document() -> dict[str, Any]:
    return {
        "schema": 1,
        "name": "contract-test",
        "description": "Small benchmark descriptor used by the harness tests",
        "work_units": 1,
        "minimum_sample_seconds": 0,
        "variants": [
            {"name": "gti", "kind": "gti", "source": "main.gti"},
            {"name": "cpp", "kind": "cpp", "source": "main.cpp"},
        ],
    }


def write_descriptor(
    directory: Path, document: dict[str, Any] | None = None
) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "main.gti").write_text("int main() { return 0; }\n", encoding="utf-8")
    (directory / "main.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
    descriptor = directory / "benchmark.json"
    descriptor.write_text(
        json.dumps(descriptor_document() if document is None else document),
        encoding="utf-8",
    )
    return descriptor


def expect_benchmark_error(module: Any, action: Any, fragment: str) -> None:
    try:
        action()
    except module.BenchmarkError as error:
        require(fragment in str(error), f"expected {fragment!r} in {str(error)!r}")
        return
    raise AssertionError(f"expected BenchmarkError containing {fragment!r}")


def expect_cli_error(
    runner: Path,
    gti: Path,
    cxx: Path,
    descriptor: Path,
    output: Path,
    fragment: str,
    extra: list[str] | None = None,
) -> None:
    command = [
        sys.executable,
        str(runner),
        "--gti",
        str(gti),
        "--cxx",
        str(cxx),
        "--workload",
        str(descriptor),
        "--output",
        str(output),
        "--smoke",
    ]
    if extra:
        command.extend(extra)
    completed = run(command, runner.parent.parent, expected=2)
    require(
        fragment in completed.stderr,
        f"expected {fragment!r} in CLI diagnostic {completed.stderr!r}",
    )


def test_strict_descriptors(
    module: Any, runner: Path, gti: Path, cxx: Path, root: Path
) -> None:
    workload = root / "strict"

    unknown = descriptor_document()
    unknown["unexpected"] = True
    descriptor = write_descriptor(workload, unknown)
    expect_benchmark_error(
        module,
        lambda: module.load_workload(str(descriptor)),
        "contains unknown fields: unexpected",
    )
    expect_cli_error(
        runner,
        gti,
        cxx,
        descriptor,
        root / "unknown-output",
        "contains unknown fields: unexpected",
    )

    missing = descriptor_document()
    del missing["description"]
    descriptor = write_descriptor(workload, missing)
    expect_benchmark_error(
        module,
        lambda: module.load_workload(str(descriptor)),
        "is missing fields: description",
    )
    expect_cli_error(
        runner,
        gti,
        cxx,
        descriptor,
        root / "missing-output",
        "is missing fields: description",
    )

    variant_unknown = descriptor_document()
    variant_unknown["variants"][0]["command"] = "not permitted"
    descriptor = write_descriptor(workload, variant_unknown)
    expect_benchmark_error(
        module,
        lambda: module.load_workload(str(descriptor)),
        "contains unknown fields: command",
    )

    variant_missing = descriptor_document()
    del variant_missing["variants"][0]["kind"]
    descriptor = write_descriptor(workload, variant_missing)
    expect_benchmark_error(
        module,
        lambda: module.load_workload(str(descriptor)),
        "is missing fields: kind",
    )

    duplicate = write_descriptor(workload)
    duplicate.write_text(
        '{"schema":1,"schema":1,"name":"contract-test",'
        '"description":"duplicate","work_units":1,'
        '"minimum_sample_seconds":0,"variants":[]}',
        encoding="utf-8",
    )
    expect_benchmark_error(
        module,
        lambda: module.load_workload(str(duplicate)),
        "repeats field 'schema'",
    )

    nonfinite = descriptor_document()
    nonfinite["minimum_sample_seconds"] = 10**400
    descriptor = write_descriptor(workload, nonfinite)
    expect_benchmark_error(
        module,
        lambda: module.load_workload(str(descriptor)),
        "must be finite and nonnegative",
    )

    invalid_schema = descriptor_document()
    invalid_schema["schema"] = []
    descriptor = write_descriptor(workload, invalid_schema)
    expect_benchmark_error(
        module,
        lambda: module.load_workload(str(descriptor)),
        "requires schema 1 or 2",
    )

    invalid_kind = descriptor_document()
    invalid_kind["variants"][0]["kind"] = []
    descriptor = write_descriptor(workload, invalid_kind)
    expect_benchmark_error(
        module,
        lambda: module.load_workload(str(descriptor)),
        "kind must be 'gti' or 'cpp'",
    )

    multi_gti = descriptor_document()
    multi_gti["schema"] = 2
    multi_gti["variants"] = [
        {
            "name": "gti-sequential",
            "kind": "gti",
            "source": "main.gti",
            "execution_profile": "concurrent",
        },
        {
            "name": "gti-threaded",
            "kind": "gti",
            "source": "main.gti",
            "execution_profile": "single-threaded",
        },
    ]
    descriptor = write_descriptor(workload, multi_gti)
    loaded = module.load_workload(str(descriptor))
    require(
        [variant.execution_profile for variant in loaded.variants]
        == ["concurrent", "single-threaded"],
        "schema 2 did not retain explicit GTI execution profiles",
    )

    legacy_multi_gti = descriptor_document()
    legacy_multi_gti["variants"][1] = {
        "name": "gti-second",
        "kind": "gti",
        "source": "main.gti",
    }
    descriptor = write_descriptor(workload, legacy_multi_gti)
    expect_benchmark_error(
        module,
        lambda: module.load_workload(str(descriptor)),
        "must declare exactly one GTI variant",
    )

    missing_profile = json.loads(json.dumps(multi_gti))
    del missing_profile["variants"][0]["execution_profile"]
    descriptor = write_descriptor(workload, missing_profile)
    expect_benchmark_error(
        module,
        lambda: module.load_workload(str(descriptor)),
        "is missing fields: execution_profile",
    )

    invalid_profile = json.loads(json.dumps(multi_gti))
    invalid_profile["variants"][0]["execution_profile"] = "parallel"
    descriptor = write_descriptor(workload, invalid_profile)
    expect_benchmark_error(
        module,
        lambda: module.load_workload(str(descriptor)),
        "must be 'single-threaded' or 'concurrent'",
    )

    native_profile = json.loads(json.dumps(multi_gti))
    native_profile["variants"][1] = {
        "name": "cpp",
        "kind": "cpp",
        "source": "main.cpp",
        "execution_profile": "concurrent",
    }
    descriptor = write_descriptor(workload, native_profile)
    expect_benchmark_error(
        module,
        lambda: module.load_workload(str(descriptor)),
        "must be null for a C++ variant",
    )

    mixed_profiles = json.loads(json.dumps(native_profile))
    mixed_profiles["variants"][1]["execution_profile"] = None
    descriptor = write_descriptor(workload, mixed_profiles)
    loaded = module.load_workload(str(descriptor))
    require(
        [variant.execution_profile for variant in loaded.variants]
        == ["concurrent", None],
        "schema 2 did not retain a GTI profile and null native profile",
    )

    no_gti = json.loads(json.dumps(mixed_profiles))
    no_gti["variants"][0] = {
        "name": "cpp-semantic",
        "kind": "cpp",
        "source": "main.cpp",
        "execution_profile": None,
    }
    descriptor = write_descriptor(workload, no_gti)
    expect_benchmark_error(
        module,
        lambda: module.load_workload(str(descriptor)),
        "must declare at least one GTI variant",
    )


def test_source_containment(module: Any, root: Path) -> None:
    root.mkdir(parents=True)
    outside = root / "outside.gti"
    outside.write_text("int main() { return 0; }\n", encoding="utf-8")

    traversal = descriptor_document()
    traversal["variants"][0]["source"] = "../outside.gti"
    descriptor = write_descriptor(root / "traversal", traversal)
    expect_benchmark_error(
        module,
        lambda: module.load_workload(str(descriptor)),
        "must remain beneath the workload directory",
    )

    symlink_directory = root / "symlink"
    descriptor = write_descriptor(symlink_directory)
    source_link = symlink_directory / "linked.gti"
    descriptor_link = root / "linked-descriptor.json"
    try:
        source_link.symlink_to(outside)
        descriptor_link.symlink_to(descriptor)
    except (NotImplementedError, OSError):
        return

    linked = descriptor_document()
    linked["variants"][0]["source"] = source_link.name
    descriptor.write_text(json.dumps(linked), encoding="utf-8")
    expect_benchmark_error(
        module,
        lambda: module.load_workload(str(descriptor)),
        "may not traverse a symbolic link",
    )
    expect_benchmark_error(
        module,
        lambda: module.load_workload(str(descriptor_link)),
        "descriptor may not be a symbolic link",
    )

    named_root = root / "named-workloads"
    named_root.mkdir()
    named_outside = root / "named-outside"
    write_descriptor(named_outside)
    named_link = named_root / "linked"
    try:
        named_link.symlink_to(named_outside, target_is_directory=True)
    except (NotImplementedError, OSError):
        return
    with patched(module, WORKLOAD_ROOT=named_root):
        expect_benchmark_error(
            module,
            lambda: module.load_workload("linked"),
            "may not traverse a symbolic link",
        )


def test_output_containment(
    module: Any, runner: Path, gti: Path, cxx: Path, root: Path
) -> None:
    descriptor = write_descriptor(root / "containment-workload")
    workload = module.load_workload(str(descriptor))
    inside_workload = descriptor.parent / "generated"
    expect_benchmark_error(
        module,
        lambda: module.canonical_output_root(str(inside_workload), [workload]),
        "inside workload 'contract-test'",
    )
    expect_cli_error(
        runner,
        gti,
        cxx,
        descriptor,
        descriptor.parent / "cli-generated",
        "inside workload 'contract-test'",
    )

    output = root / "contained-output"
    output.mkdir()
    default_report = output / "report.json"
    outside_report = root / "escaped-report.json"
    expect_benchmark_error(
        module,
        lambda: module.report_path(str(outside_report), default_report, output),
        "must remain beneath",
    )
    expect_cli_error(
        runner,
        gti,
        cxx,
        descriptor,
        root / "cli-contained-output",
        "must remain beneath",
        ["--json", str(outside_report)],
    )

    expect_benchmark_error(
        module,
        lambda: module.canonical_output_root(str(root), [workload]),
        "inside workload 'contract-test'",
    )
    expect_benchmark_error(
        module,
        lambda: module.canonical_output_root(
            str(module.REPOSITORY_ROOT), [workload]
        ),
        "may not be inside source assets",
    )

    outside_report.write_text("outside\n", encoding="utf-8")
    linked_report = output / "linked-report.json"
    try:
        linked_report.symlink_to(outside_report)
    except (NotImplementedError, OSError):
        return
    expect_benchmark_error(
        module,
        lambda: module.report_path(str(linked_report), default_report, output),
        "must remain beneath",
    )


def synthetic_workload(module: Any, root: Path) -> Any:
    descriptor = write_descriptor(root / "synthetic")
    third_source = descriptor.parent / "third.cpp"
    third_source.write_text("int main() { return 0; }\n", encoding="utf-8")
    variants = (
        module.Variant("gti", "gti", descriptor.parent / "main.gti", None),
        module.Variant(
            "cpp-semantic", "cpp", descriptor.parent / "main.cpp", None
        ),
        module.Variant("cpp-idiomatic", "cpp", third_source, None),
    )
    return module.Workload(
        name="synthetic",
        description="Synthetic module-level workload",
        work_units=1,
        minimum_sample_seconds=0.0,
        descriptor_path=descriptor,
        descriptor=descriptor_document(),
        descriptor_digest="0" * 64,
        variants=variants,
    )


def fake_build_variant(
    workload: Any,
    variant: Any,
    directory: Path,
    gti: Any,
    cxx: Any,
    optimization: str,
    cpp_standard: str,
    native_flags: list[str],
    timeout: float,
) -> dict[str, Any]:
    del workload, gti, cxx, optimization, cpp_standard, native_flags, timeout
    return {
        "name": variant.name,
        "kind": variant.kind,
        "executable": str(directory / variant.name),
    }


@contextmanager
def patched(module: Any, **replacements: Any) -> Iterator[None]:
    originals = {name: getattr(module, name) for name in replacements}
    try:
        for name, value in replacements.items():
            setattr(module, name, value)
        yield
    finally:
        for name, value in originals.items():
            setattr(module, name, value)


def measure(
    module: Any,
    workload: Any,
    directory: Path,
    seed: int,
    runs: int,
    smoke: bool,
) -> dict[str, Any]:
    fake_tool = module.Tool(
        "fake", directory / "fake", (str(directory / "fake"), "--version"), "fake 1"
    )
    return module.measure_workload(
        workload,
        directory,
        fake_tool,
        fake_tool,
        "3",
        "c++23",
        [],
        0,
        runs,
        seed,
        1.0,
        smoke,
    )


def test_correctness_precedes_timing(module: Any, root: Path) -> None:
    workload = synthetic_workload(module, root)
    invocations: list[list[str]] = []

    def malformed_invoke(
        command: list[str] | tuple[str, ...], cwd: Path, timeout: float
    ) -> tuple[subprocess.CompletedProcess[str], int]:
        del cwd, timeout
        invocations.append(list(command))
        return subprocess.CompletedProcess(command, 0, "not-a-record\n", ""), 1

    with patched(
        module, build_variant=fake_build_variant, invoke=malformed_invoke
    ):
        expect_benchmark_error(
            module,
            lambda: measure(module, workload, root / "malformed-run", 7, 4, False),
            "validation produced a malformed result record",
        )
    require(
        len(invocations) == 1,
        "a malformed validation record must abort before warmup or measurement",
    )

    phases: list[tuple[str, str]] = []

    def mismatched_execute(
        selected_workload: Any, build: dict[str, Any], phase: str, timeout: float
    ) -> dict[str, Any]:
        del selected_workload, timeout
        phases.append((build["name"], phase))
        digest = "0000000000000001" if build["name"] == "gti" else "0000000000000002"
        record = f"GTI-BENCH-1 digest:{digest} work-units:1"
        return {
            "command": [build["executable"]],
            "duration_ns": 1,
            "record": record,
            "digest": digest,
            "work_units": 1,
        }

    with patched(
        module,
        build_variant=fake_build_variant,
        execute_variant=mismatched_execute,
    ):
        expect_benchmark_error(
            module,
            lambda: measure(module, workload, root / "mismatch-run", 7, 4, False),
            "does not match 'gti'",
        )
    require(
        phases
        == [
            ("gti", "validation"),
            ("cpp-semantic", "validation"),
            ("cpp-idiomatic", "validation"),
        ],
        f"mismatched records reached timing phases: {phases!r}",
    )


def test_deterministic_sample_order(module: Any, root: Path) -> None:
    workload = synthetic_workload(module, root)
    duration = 0

    def identical_execute(
        selected_workload: Any, build: dict[str, Any], phase: str, timeout: float
    ) -> dict[str, Any]:
        nonlocal duration
        del selected_workload, phase, timeout
        duration += 1
        digest = "0000000000000001"
        return {
            "command": [build["executable"]],
            "duration_ns": duration,
            "record": f"GTI-BENCH-1 digest:{digest} work-units:1",
            "digest": digest,
            "work_units": 1,
        }

    seed = 193
    rounds = 8
    with patched(
        module,
        build_variant=fake_build_variant,
        execute_variant=identical_execute,
    ):
        first = measure(module, workload, root / "order-one", seed, rounds, False)
        second = measure(module, workload, root / "order-two", seed, rounds, False)

    generator = random.Random(seed)
    names = ["gti", "cpp-semantic", "cpp-idiomatic"]
    expected = []
    for round_index in range(rounds):
        order = list(names)
        generator.shuffle(order)
        expected.append({"round": round_index, "variants": order})
    require(first["sample_order"] == expected, "sample order does not follow the seed")
    require(
        second["sample_order"] == expected,
        "the same seed did not reproduce the same sample order",
    )


def test_exact_head_truth_table(module: Any, root: Path) -> None:
    compiler = root / "gti"
    compiler.parent.mkdir(parents=True)
    compiler.write_bytes(b"compiler")
    compiler.chmod(0o755)
    tool = module.Tool("gti", compiler, (str(compiler), "--version"), "gti test")
    compiler_sha256 = sha256_file(compiler)
    clean = {"commit": "1" * 40, "dirty": False}

    def workloads(
        state: str = "clean",
        commit: str | None = "1" * 40,
        producer_sha256: str = compiler_sha256,
    ) -> list[dict[str, Any]]:
        return [{
            "variants": [{
                "kind": "gti",
                "provenance": {
                    "build": {
                        "producer": {
                            "compiler_build": {"state": state, "commit": commit},
                            "executable": {"sha256": producer_sha256},
                        }
                    }
                },
            }]
        }]

    require(
        module.exact_head_evidence(clean, clean, tool, workloads())["exact"] is True,
        "matching clean repository/compiler/producer identities were not exact",
    )
    cases = (
        ({"commit": None, "dirty": False}, clean, workloads(),
         "benchmarked_head_unavailable"),
        (clean, {"commit": None, "dirty": False}, workloads(),
         "recorded_head_unavailable"),
        ({"commit": "1" * 40, "dirty": True}, clean, workloads(),
         "repository_not_clean_before_benchmark"),
        (clean, {"commit": "1" * 40, "dirty": True}, workloads(),
         "repository_not_clean_after_benchmark"),
        (clean, {"commit": "2" * 40, "dirty": False}, workloads(),
         "recorded_head_differs_from_benchmarked_head"),
        (clean, clean, workloads("unavailable", None),
         "compiler_build_identity_unavailable"),
        (clean, clean, workloads("clean", None),
         "compiler_build_identity_unavailable"),
        (clean, clean, workloads("dirty"), "compiler_build_identity_dirty"),
        (clean, clean, workloads("clean", "2" * 40),
         "compiler_commit_differs_from_benchmarked_head"),
        (clean, clean, workloads(producer_sha256="2" * 64),
         "sidecar_producer_differs_from_selected_compiler"),
    )
    for before, after, reports, reason in cases:
        evidence = module.exact_head_evidence(before, after, tool, reports)
        require(
            evidence["exact"] is False and reason in evidence["reasons"],
            f"exact-head evidence missed {reason}: {evidence!r}",
        )
    inconsistent = workloads()
    inconsistent[0]["variants"].append(
        workloads(commit="2" * 40)[0]["variants"][0]
    )
    evidence = module.exact_head_evidence(clean, clean, tool, inconsistent)
    require(
        evidence["exact"] is False
        and "compiler_build_identity_inconsistent" in evidence["reasons"],
        f"exact-head evidence accepted inconsistent compiler commits: {evidence!r}",
    )
    mixed_schema = workloads()
    mixed_schema[0]["descriptor"] = {"schema": 2}
    mixed_schema.append({
        "descriptor": {"schema": 1},
        "variants": [{"kind": "cpp"}],
    })
    require(
        module.exact_head_evidence(clean, clean, tool, mixed_schema)["exact"] is True,
        "a schema-1 C++ variant incorrectly invalidated GTI exact-head evidence",
    )
    mixed_schema[-1]["variants"].append({"kind": "gti"})
    evidence = module.exact_head_evidence(clean, clean, tool, mixed_schema)
    require(
        evidence["exact"] is False
        and "gti_build_provenance_unavailable" in evidence["reasons"],
        f"exact-head evidence skipped a schema-1 GTI variant: {evidence!r}",
    )


def structural_paths(
    value: Any,
    nullable_scalar_types: dict[str, str],
    path: str = "$",
) -> list[str]:
    paths: set[str] = set()
    seen_nullable_paths: set[str] = set()

    def visit(current: Any, current_path: str) -> None:
        if isinstance(current, dict):
            paths.add(f"{current_path}:object")
            for key, child in current.items():
                visit(child, f"{current_path}.{key}")
            return
        if isinstance(current, list):
            paths.add(f"{current_path}:array")
            for child in current:
                visit(child, f"{current_path}[*]")
            return
        if current is None:
            scalar_type = "null"
        elif isinstance(current, bool):
            scalar_type = "boolean"
        elif isinstance(current, (int, float)):
            scalar_type = "number"
        elif isinstance(current, str):
            scalar_type = "string"
        else:
            raise AssertionError(f"unexpected report value at {current_path}: {current!r}")
        declared_type = nullable_scalar_types.get(current_path)
        if declared_type is not None:
            allowed_types = declared_type.split("|")
            if scalar_type not in allowed_types:
                raise AssertionError(
                    f"{current_path} has {scalar_type}, outside declared "
                    f"schema-1 union {declared_type}"
                )
            scalar_type = declared_type
            seen_nullable_paths.add(current_path)
        paths.add(f"{current_path}:{scalar_type}")

    visit(value, path)
    missing_nullable_paths = set(nullable_scalar_types) - seen_nullable_paths
    if missing_nullable_paths:
        raise AssertionError(
            "schema-1 report omitted declared nullable scalar paths: "
            f"{sorted(missing_nullable_paths)!r}"
        )
    return sorted(paths)


def require_schema_1_base_shape(
    report: dict[str, Any], reference: dict[str, Any], context: str
) -> None:
    actual = structural_paths(report, SCHEMA_1_NULLABLE_SCALAR_TYPES)
    actual_digest = hashlib.sha256(
        ("\n".join(actual) + "\n").encode("utf-8")
    ).hexdigest()
    require(
        len(actual) == reference["structural_path_count"]
        and actual_digest == reference["structural_paths_sha256"],
        f"{context} schema-1 report shape differs from the frozen base implementation\n"
        f"expected count/digest={reference['structural_path_count']}/"
        f"{reference['structural_paths_sha256']}\n"
        f"actual count/digest={len(actual)}/{actual_digest}\n"
        f"actual paths={actual!r}",
    )


def test_schema_1_base_shape(report: dict[str, Any]) -> None:
    reference = json.loads(SCHEMA_1_BASE_SHAPE.read_text(encoding="utf-8"))
    require(
        reference["base_commit"] == "90b32f0ffec221e7e357ac1748592e2fe598e807",
        "schema-1 base-shape fixture lost its immutable base commit",
    )
    require(
        reference["base_runner_sha256"]
        == "55c11eeb1f89beec5160233f6050313262b30730a709e6c8a3dfe72d946ef6aa",
        "schema-1 base-shape fixture lost its base runner identity",
    )
    require(
        reference["generation"]["method"] == "executed-local-git-object"
        and reference["generation"]["workload"] == "vector-checked-loop",
        "schema-1 base-shape fixture lost transparent generation metadata",
    )
    require(
        "GTI_STDLIB_PATH" in reference["generation"]["environment"],
        "schema-1 base-shape fixture lost its generation environment",
    )
    require(
        "Git-unavailable source archive"
        in reference["generation"]["normalization_validation"],
        "schema-1 base-shape fixture lost its cross-environment validation",
    )
    require(
        reference["generation"]["normalization"]["nullable_scalar_types"]
        == SCHEMA_1_NULLABLE_SCALAR_TYPES,
        "schema-1 base-shape fixture lost its declared nullable scalar unions",
    )
    require(
        report["schema"] == 1
        and len(report["workloads"]) == 1
        and report["workloads"][0]["descriptor"]["schema"] == 1,
        "shape regression requires a real schema-1-only branch report",
    )
    require_schema_1_base_shape(report, reference, "actual")

    git_available = copy.deepcopy(report)
    git_available["repository"] = {
        "root": "/repository",
        "commit": "1" * 40,
        "dirty": False,
        "reason": None,
    }
    git_available["environment"]["cpu_model"] = "Test CPU"
    git_available["environment"]["logical_cpu_count"] = 8
    git_available["environment"]["gti_toolchain_overrides"] = {
        name: "/toolchain" for name in (
            "GTI_STDLIB_PATH",
            "GTI_RUNTIME_INCLUDE",
            "GTI_RUNTIME_LIBRARY",
            "GTI_VENDOR_INCLUDE",
        )
    }
    require_schema_1_base_shape(git_available, reference, "Git-available")

    git_unavailable = copy.deepcopy(report)
    git_unavailable["repository"] = {
        "root": None,
        "commit": None,
        "dirty": None,
        "reason": "Git repository unavailable",
    }
    git_unavailable["environment"]["cpu_model"] = None
    git_unavailable["environment"]["logical_cpu_count"] = None
    git_unavailable["environment"]["gti_toolchain_overrides"] = {
        name: None for name in (
            "GTI_STDLIB_PATH",
            "GTI_RUNTIME_INCLUDE",
            "GTI_RUNTIME_LIBRARY",
            "GTI_VENDOR_INCLUDE",
        )
    }
    require_schema_1_base_shape(git_unavailable, reference, "Git-unavailable")

    invalid_nullable_type = copy.deepcopy(git_available)
    invalid_nullable_type["repository"]["dirty"] = "false"
    try:
        structural_paths(invalid_nullable_type, SCHEMA_1_NULLABLE_SCALAR_TYPES)
    except AssertionError as error:
        require(
            "outside declared schema-1 union" in str(error),
            f"nullable scalar rejection was unclear: {error}",
        )
    else:
        raise AssertionError("nullable scalar normalization accepted a wrong type")


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def test_real_smoke(module: Any, runner: Path, gti: Path, cxx: Path, root: Path) -> None:
    repository = runner.parent.parent
    output = root / "real-smoke-output"
    json_report = output / "report.json"
    markdown_report = output / "report.md"
    seed = 29
    command = [
        sys.executable,
        str(runner),
        "--gti",
        str(gti),
        "--cxx",
        str(cxx),
        "--optimization",
        "3",
        "--cpp-standard",
        "c++23",
        "--warmup",
        "7",
        "--runs",
        "5",
        "--seed",
        str(seed),
        "--timeout",
        "120",
        "--workload",
        "vector-checked-loop",
        "--output",
        str(output),
        "--json",
        str(json_report),
        "--markdown",
        str(markdown_report),
        "--smoke",
    ]
    environment = os.environ.copy()
    environment["GTI_STDLIB_PATH"] = str(repository / "stdlib")
    environment.setdefault("GTI_RUNTIME_INCLUDE", str(repository / "runtime/include"))
    environment.setdefault(
        "GTI_VENDOR_INCLUDE", str(repository / "vendor/expected_lite/include")
    )
    runtime_library = gti.parent / "libgti_runtime.a"
    if runtime_library.is_file():
        environment.setdefault("GTI_RUNTIME_LIBRARY", str(runtime_library))

    completed = run(command, repository, environment=environment)
    require(json_report.is_file(), "smoke run did not publish the JSON report")
    require(markdown_report.is_file(), "smoke run did not publish the Markdown report")
    require(
        f"Benchmark JSON: {json_report}" in completed.stdout,
        "runner did not identify the JSON report",
    )
    require(
        f"Benchmark Markdown: {markdown_report}" in completed.stdout,
        "runner did not identify the Markdown report",
    )

    report = json.loads(json_report.read_text(encoding="utf-8"))
    test_schema_1_base_shape(report)
    require(report["schema"] == 1, "unexpected benchmark report schema")
    require(report["runner"]["path"] == str(runner), "runner identity path changed")
    require(
        report["runner"]["sha256"] == sha256_file(runner),
        "runner identity digest is incorrect",
    )
    configuration = report["configuration"]
    require(configuration["smoke"] is True, "report did not retain smoke mode")
    require(configuration["requested_warmups"] == 7, "requested warmups were lost")
    require(configuration["requested_runs"] == 5, "requested runs were lost")
    require(configuration["effective_warmups"] == 0, "smoke warmups were not disabled")
    require(configuration["effective_runs"] == 1, "smoke runs were not reduced to one")
    require(configuration["seed"] == seed, "sample-order seed was not recorded")
    require(configuration["native_flags"] == [], "unexpected native flags in report")
    require(
        configuration["strict_native_flags"]
        == [
            "-fno-fast-math",
            "-ffp-contract=off",
            "-D__gti_strict_ieee754=1",
        ],
        "strict native flags were not recorded",
    )

    identity_environment = environment.copy()
    identity_environment["LC_ALL"] = "C"
    identity_environment["LANG"] = "C"
    for name, executable in (("gti", gti), ("cxx", cxx)):
        identity = report["tools"][name]
        expected_command = [str(executable), "--version"]
        require(identity["requested"] == str(executable), f"{name} request changed")
        require(identity["resolved"] == str(executable), f"{name} path was not resolved")
        require(
            identity["version_command"] == expected_command,
            f"{name} version command was not recorded exactly",
        )
        version = run(
            expected_command, repository, environment=identity_environment, timeout=30
        )
        require(
            identity["version_output"] == (version.stdout + version.stderr).strip(),
            f"{name} version output does not match the resolved compiler",
        )

    require(
        "exact_head_evidence" not in report
        and "source_identity" not in report["tools"]["gti"],
        "schema-1 report gained schema-2 compiler evidence",
    )

    require(len(report["workloads"]) == 1, "schema-1 smoke report lost its workload")
    workloads = {entry["name"]: entry for entry in report["workloads"]}
    require(
        set(workloads) == {"vector-checked-loop"},
        "smoke report contains the wrong workloads",
    )
    workload = workloads["vector-checked-loop"]
    require(workload["name"] == "vector-checked-loop", "wrong smoke workload")
    require(workload["correctness_record"] == EXPECTED_RECORD, "wrong correctness record")
    require(workload["quality"] == "smoke", "smoke run applied timing thresholds")
    require(
        workload["inconclusive_reasons"] == [],
        "smoke run reported timing-quality failures",
    )
    require(len(workload["variants"]) == 3, "smoke workload must retain three variants")

    run_directory = Path(report["run_directory"])
    require(run_directory.is_relative_to(output), "run directory escaped the output root")
    descriptor_by_name = {
        entry["name"]: entry for entry in workload["descriptor"]["variants"]
    }
    variants = {entry["name"]: entry for entry in workload["variants"]}
    strict_native_flags = [
        "-fno-fast-math",
        "-ffp-contract=off",
        "-D__gti_strict_ieee754=1",
    ]
    require(
        list(module.STRICT_NATIVE_FLAGS) == strict_native_flags,
        "benchmark strict native policy changed",
    )
    require(
        configuration["strict_native_flags"] == strict_native_flags,
        "report did not record the strict native policy exactly",
    )
    require(
        set(variants) == {"gti", "cpp-semantic", "cpp-idiomatic"},
        "smoke report variant set changed",
    )
    require(
        {entry["validation"]["record"] for entry in variants.values()}
        == {EXPECTED_RECORD},
        "validated variants did not publish one identical record",
    )

    expected_order = list(workload["descriptor"]["variants"])
    expected_names = [entry["name"] for entry in expected_order]
    random.Random(seed).shuffle(expected_names)
    require(
        workload["sample_order"] == [{"round": 0, "variants": expected_names}],
        "smoke sample order does not match its recorded seed",
    )

    for name, variant in variants.items():
        require(
            "execution_profile" not in variant and "provenance" not in variant,
            f"schema-1 variant {name} gained schema-2 report fields",
        )
        executable = variant["executable"]
        source = str(
            (Path(workload["descriptor_path"]).parent / descriptor_by_name[name]["source"])
            .resolve()
        )
        if variant["kind"] == "gti":
            expected_command = [
                str(gti),
                source,
                "-O3",
                "--std",
                "c++23",
                "--cxx",
                str(cxx),
                "--keep-cpp",
                "-o",
                executable,
            ]
        else:
            expected_command = [
                str(cxx),
                "-O3",
                "-std=c++23",
                *strict_native_flags,
                source,
                "-o",
                executable,
            ]
        require(
            variant["command"] == expected_command,
            f"build command for {name} was not preserved exactly",
        )
        require(
            variant["validation"]["command"] == [executable],
            f"validation command for {name} was not preserved exactly",
        )
        require(variant["warmup_samples_ns"] == [], f"{name} ran smoke warmups")
        samples = variant["raw_samples_ns"]
        require(
            len(samples) == 1 and isinstance(samples[0], int) and samples[0] > 0,
            f"{name} did not retain its one raw sample",
        )
        summary = variant["summary"]
        require(summary["count"] == 1, f"{name} summary count is not one")
        require(summary["minimum_ns"] == samples[0], f"{name} minimum lost raw sample")
        require(summary["maximum_ns"] == samples[0], f"{name} maximum lost raw sample")
        require(summary["median_ns"] == samples[0], f"{name} median lost raw sample")

    emitted = variants["gti"]["artifacts"]["emitted_cpp"]
    emitted_path = Path(emitted["path"])
    require(emitted_path.is_relative_to(run_directory), "emitted GTI C++ escaped the run")
    require(emitted_path.is_file(), "reported emitted GTI C++ does not exist")
    require(emitted["bytes"] == emitted_path.stat().st_size, "emitted C++ size is wrong")
    require(
        emitted["sha256"] == sha256_file(emitted_path),
        "emitted GTI C++ digest is wrong",
    )

    thread_output = root / "provenance-smoke-output"
    thread_json_report = thread_output / "report.json"
    thread_markdown_report = thread_output / "report.md"
    thread_command = [
        sys.executable,
        str(runner),
        "--gti",
        str(gti),
        "--cxx",
        str(cxx),
        "--optimization",
        "3",
        "--cpp-standard",
        "c++23",
        "--warmup",
        "7",
        "--runs",
        "5",
        "--seed",
        str(seed),
        "--timeout",
        "120",
        "--workload",
        "managed-thread-parallelism",
        "--output",
        str(thread_output),
        "--json",
        str(thread_json_report),
        "--markdown",
        str(thread_markdown_report),
        "--smoke",
    ]
    run(thread_command, repository, environment=environment)
    thread_report = json.loads(thread_json_report.read_text(encoding="utf-8"))
    require(thread_report["schema"] == 2, "provenance run did not use report schema 2")
    require(
        "exact_head_evidence" in thread_report,
        "schema-2 report omitted exact-head evidence",
    )
    thread_run_directory = Path(thread_report["run_directory"])
    require(
        thread_run_directory.is_relative_to(thread_output),
        "schema-2 run directory escaped its output root",
    )
    require(len(thread_report["workloads"]) == 1, "schema-2 smoke lost its workload")
    thread_workload = thread_report["workloads"][0]
    require(
        thread_workload["correctness_record"] == THREAD_EXPECTED_RECORD,
        "wrong managed-thread correctness record",
    )
    require(
        thread_workload["quality"] == "smoke"
        and thread_workload["inconclusive_reasons"] == [],
        "managed-thread smoke applied timing-quality thresholds",
    )
    require(
        thread_workload["descriptor"]["schema"] == 2,
        "managed-thread workload did not retain descriptor schema 2",
    )
    thread_descriptors = {
        entry["name"]: entry
        for entry in thread_workload["descriptor"]["variants"]
    }
    thread_variants = {
        entry["name"]: entry for entry in thread_workload["variants"]
    }
    require(
        set(thread_variants) == {"gti-sequential", "gti-four-threads"},
        "managed-thread smoke variant set changed",
    )
    require(
        {entry["validation"]["record"] for entry in thread_variants.values()}
        == {THREAD_EXPECTED_RECORD},
        "managed-thread variants did not publish one identical record",
    )

    thread_names = [
        entry["name"] for entry in thread_workload["descriptor"]["variants"]
    ]
    random.Random(seed).shuffle(thread_names)
    require(
        thread_workload["sample_order"]
        == [{"round": 0, "variants": thread_names}],
        "managed-thread smoke order does not match its recorded seed",
    )

    for name, variant in thread_variants.items():
        executable = variant["executable"]
        descriptor_entry = thread_descriptors[name]
        source = str(
            (
                Path(thread_workload["descriptor_path"]).parent
                / descriptor_entry["source"]
            ).resolve()
        )
        expected_command = [
            str(gti),
            source,
            "-O3",
            "--std",
            "c++23",
            "--cxx",
            str(cxx),
            "--keep-cpp",
            "--execution-profile",
            "concurrent",
            "--build-report",
            variant["provenance"]["sidecar"]["path"],
            "-o",
            executable,
        ]
        require(
            variant["kind"] == "gti"
            and variant["execution_profile"] == "concurrent",
            f"{name} lost its explicit concurrent GTI profile",
        )
        require(
            variant["command"] == expected_command,
            f"build command for {name} did not retain its execution profile",
        )
        require(
            variant["validation"]["command"] == [executable],
            f"validation command for {name} was not preserved exactly",
        )
        require(variant["warmup_samples_ns"] == [], f"{name} ran smoke warmups")
        samples = variant["raw_samples_ns"]
        require(
            len(samples) == 1 and isinstance(samples[0], int) and samples[0] > 0,
            f"{name} did not retain its one raw sample",
        )
        emitted = variant["artifacts"]["emitted_cpp"]
        emitted_path = Path(emitted["path"])
        require(
            emitted_path.is_relative_to(thread_run_directory)
            and emitted_path.is_file(),
            f"{name} emitted C++ escaped or is missing",
        )
        require(
            emitted["sha256"] == sha256_file(emitted_path),
            f"{name} emitted C++ digest is wrong",
        )
        provenance = variant["provenance"]
        build_provenance = provenance["build"]
        require(
            provenance["sidecar"]["sha256"]
            == sha256_file(Path(provenance["sidecar"]["path"])),
            f"{name} sidecar identity is wrong",
        )
        producer = build_provenance["producer"]
        require(
            producer["executable"]["canonical_path"] == str(gti)
            and producer["executable"]["sha256"] == sha256_file(gti),
            f"{name} sidecar is not bound to the selected GTI compiler",
        )
        source_units = build_provenance["sources"]["units"]
        source_paths = {unit["logical_identity"] for unit in source_units}
        require(
            "package:kernel.gti" in source_paths
            and "prelude" in source_paths
            and "stdlib:std/format" in source_paths,
            f"{name} source provenance omitted recursive application/stdlib inputs",
        )
        if name == "gti-four-threads":
            require(
                "package:threaded.gti" in source_paths
                and "stdlib:std/thread" in source_paths,
                "threaded provenance omitted its entry or thread library closure",
            )
        else:
            require(
                "package:sequential.gti" in source_paths,
                "sequential provenance omitted its entry source",
            )
        runtime = build_provenance["native"]["runtime"]
        require(
            any(header["kind"] == "runtime" for header in runtime["headers"])
            and runtime["archive"]["canonical_path"]
            in build_provenance["native"]["argv"],
            f"{name} provenance omitted used runtime material",
        )
        materials = [
            *(unit["material"] for unit in source_units),
            *(header["material"] for header in runtime["headers"]),
            runtime["archive"],
        ]
        for record in materials:
            path = Path(record["canonical_path"])
            require(path.is_file(), f"provenance material is missing: {path}")
            require(
                record["sha256"] == sha256_file(path)
                and record["bytes"] == path.stat().st_size,
                f"provenance material identity is wrong: {path}",
            )

    source_hashes = {
        variant["provenance"]["build"]["sources"]["materials_sha256"]
        for variant in thread_variants.values()
    }
    require(
        len(source_hashes) == 2,
        "different loaded source graphs did not alter source provenance",
    )
    tampered_path = root / "tampered-build-report.json"
    tampered = json.loads(
        Path(thread_variants["gti-sequential"]["provenance"]["sidecar"]["path"])
        .read_text(encoding="utf-8")
    )
    tampered["sources"]["units"][0]["material"]["sha256"] = "0" * 64
    tampered_path.write_text(json.dumps(tampered), encoding="utf-8")
    expect_benchmark_error(
        module,
        lambda: module.load_build_provenance(
            tampered_path,
            module.Tool(str(gti), gti, (str(gti), "--version"), "gti"),
            module.Tool(str(cxx), cxx, (str(cxx), "--version"), "cxx"),
            (
                Path(thread_workload["descriptor_path"]).parent
                / thread_descriptors["gti-sequential"]["source"]
            ).resolve(),
        ),
        "no longer matches its recorded material",
    )

    sequential_sidecar = Path(
        thread_variants["gti-sequential"]["provenance"]["sidecar"]["path"]
    )
    sequential_report = json.loads(sequential_sidecar.read_text(encoding="utf-8"))
    archive_path = Path(
        sequential_report["native"]["runtime"]["archive"]["canonical_path"]
    )
    archive_index = sequential_report["native"]["argv"].index(str(archive_path))
    selected_gti = module.Tool(str(gti), gti, (str(gti), "--version"), "gti")
    selected_cxx = module.Tool(str(cxx), cxx, (str(cxx), "--version"), "cxx")
    sequential_entry = (
        Path(thread_workload["descriptor_path"]).parent
        / thread_descriptors["gti-sequential"]["source"]
    ).resolve()

    archive_link = root / "runtime-archive-link"
    try:
        archive_link.symlink_to(archive_path)
    except OSError:
        pass
    else:
        linked_archive_report = json.loads(json.dumps(sequential_report))
        linked_archive_report["native"]["argv"][archive_index] = str(archive_link)
        linked_archive_path = root / "linked-archive-build-report.json"
        linked_archive_path.write_text(
            json.dumps(linked_archive_report), encoding="utf-8"
        )
        module.load_build_provenance(
            linked_archive_path, selected_gti, selected_cxx, sequential_entry
        )

    missing_archive_report = json.loads(json.dumps(sequential_report))
    missing_archive_report["native"]["argv"][archive_index] = str(
        root / "missing-runtime-archive.a"
    )
    missing_archive_path = root / "missing-archive-build-report.json"
    missing_archive_path.write_text(
        json.dumps(missing_archive_report), encoding="utf-8"
    )
    expect_benchmark_error(
        module,
        lambda: module.load_build_provenance(
            missing_archive_path, selected_gti, selected_cxx, sequential_entry
        ),
        "runtime archive is not present",
    )

    different_archive = root / "different-runtime-archive.a"
    different_archive.write_bytes(archive_path.read_bytes())
    different_archive_report = json.loads(json.dumps(sequential_report))
    different_archive_report["native"]["argv"][archive_index] = str(
        different_archive
    )
    different_archive_path = root / "different-archive-build-report.json"
    different_archive_path.write_text(
        json.dumps(different_archive_report), encoding="utf-8"
    )
    expect_benchmark_error(
        module,
        lambda: module.load_build_provenance(
            different_archive_path, selected_gti, selected_cxx, sequential_entry
        ),
        "runtime archive is not present",
    )

    markdown = markdown_report.read_text(encoding="utf-8")
    require("# GTI benchmark report" in markdown, "Markdown report has no heading")
    require(EXPECTED_RECORD in markdown, "Markdown report lost correctness evidence")
    require("Measurement quality: **smoke**" in markdown, "Markdown smoke quality is wrong")
    for name in variants:
        require(f"| {name} |" in markdown, f"Markdown report omitted {name}")
    thread_markdown = thread_markdown_report.read_text(encoding="utf-8")
    require(
        THREAD_EXPECTED_RECORD in thread_markdown,
        "Markdown report lost managed-thread correctness evidence",
    )
    for name in thread_variants:
        require(
            f"| {name} |" in thread_markdown,
            f"Markdown report omitted {name}",
        )


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit("usage: benchmark_compare_test.py runner.py gti cxx")

    runner = Path(sys.argv[1]).resolve(strict=True)
    gti = Path(sys.argv[2]).resolve(strict=True)
    cxx = Path(sys.argv[3]).resolve(strict=True)
    module = load_runner(runner)
    with tempfile.TemporaryDirectory(prefix="gti-benchmark-harness-") as directory:
        root = Path(directory).resolve()
        test_strict_descriptors(module, runner, gti, cxx, root / "descriptors")
        test_source_containment(module, root / "sources")
        test_output_containment(module, runner, gti, cxx, root / "outputs")
        test_correctness_precedes_timing(module, root / "correctness")
        test_deterministic_sample_order(module, root / "ordering")
        test_exact_head_truth_table(module, root / "exact-head")
        test_real_smoke(module, runner, gti, cxx, root / "smoke")


if __name__ == "__main__":
    main()
