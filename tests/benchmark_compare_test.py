#!/usr/bin/env python3

from __future__ import annotations

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
        module.Variant("gti", "gti", descriptor.parent / "main.gti"),
        module.Variant("cpp-semantic", "cpp", descriptor.parent / "main.cpp"),
        module.Variant("cpp-idiomatic", "cpp", third_source),
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

    require(len(report["workloads"]) == 1, "smoke report contains extra workloads")
    workload = report["workloads"][0]
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

    markdown = markdown_report.read_text(encoding="utf-8")
    require("# GTI benchmark report" in markdown, "Markdown report has no heading")
    require(EXPECTED_RECORD in markdown, "Markdown report lost correctness evidence")
    require("Measurement quality: **smoke**" in markdown, "Markdown smoke quality is wrong")
    for name in variants:
        require(f"| {name} |" in markdown, f"Markdown report omitted {name}")


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
        test_real_smoke(module, runner, gti, cxx, root / "smoke")


if __name__ == "__main__":
    main()
