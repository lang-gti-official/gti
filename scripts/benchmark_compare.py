#!/usr/bin/env python3

"""Build, validate, and measure GTI benchmark variants reproducibly."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import random
import re
import secrets
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from typing import Any


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
WORKLOAD_ROOT = REPOSITORY_ROOT / "benchmarks" / "workloads"
DESCRIPTOR_KEYS = {
    "schema",
    "name",
    "description",
    "work_units",
    "minimum_sample_seconds",
    "variants",
}
VARIANT_KEYS = {"name", "kind", "source"}
NAME_PATTERN = re.compile(r"^[a-z0-9][a-z0-9-]*$")
RESULT_PATTERN = re.compile(
    r"GTI-BENCH-1 digest:([0-9a-f]{16}) work-units:([1-9][0-9]{0,19})"
)
MAXIMUM_VARIANTS = 16
MAXIMUM_WORK_UNITS = (1 << 64) - 1
NOISY_CV_LIMIT = 0.20
MINIMUM_CONCLUSIVE_SAMPLES = 5
STRICT_NATIVE_FLAGS = (
    "-fno-fast-math",
    "-ffp-contract=off",
    "-D__gti_strict_binary32=1",
)
SAFE_NATIVE_FLAGS = {
    "-fvectorize",
    "-fno-vectorize",
    "-fslp-vectorize",
    "-fno-slp-vectorize",
}
SAFE_NATIVE_FLAG_PATTERN = re.compile(
    r"^-(?:march|mcpu|mtune)=[A-Za-z0-9_+.,-]+$"
)


class BenchmarkError(RuntimeError):
    """A benchmark contract, tool, build, or execution failure."""


@dataclass(frozen=True)
class Variant:
    name: str
    kind: str
    source: Path


@dataclass(frozen=True)
class Workload:
    name: str
    description: str
    work_units: int
    minimum_sample_seconds: float
    descriptor_path: Path
    descriptor: dict[str, Any]
    descriptor_digest: str
    variants: tuple[Variant, ...]


@dataclass(frozen=True)
class Tool:
    requested: str
    resolved: Path
    version_command: tuple[str, ...]
    version_output: str


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def file_evidence(path: Path) -> dict[str, Any]:
    try:
        contents = path.read_bytes()
    except OSError as error:
        raise BenchmarkError(f"failed to read benchmark artifact '{path}': {error}") from error
    return {
        "path": str(path),
        "sha256": sha256_bytes(contents),
        "bytes": len(contents),
    }


def write_text(path: Path, contents: str) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding="utf-8")
    except OSError as error:
        raise BenchmarkError(f"failed to write benchmark artifact '{path}': {error}") from error


def write_atomic(path: Path, contents: str) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=path.parent,
            prefix=f".{path.name}.",
            suffix=".tmp",
            delete=False,
        ) as stream:
            temporary = Path(stream.name)
            stream.write(contents)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    except OSError as error:
        try:
            temporary.unlink(missing_ok=True)
        except (OSError, UnboundLocalError):
            pass
        raise BenchmarkError(f"failed to publish benchmark report '{path}': {error}") from error


def resolved_tool(requested: str, version_arguments: tuple[str, ...]) -> Tool:
    candidate = shutil.which(requested)
    if candidate is None:
        path = Path(requested).expanduser()
        if path.is_file():
            candidate = str(path)
    if candidate is None:
        raise BenchmarkError(f"benchmark tool was not found: {requested}")
    resolved = Path(candidate).resolve(strict=True)
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise BenchmarkError(f"benchmark tool is not executable: {resolved}")
    command = (str(resolved), *version_arguments)
    completed, _ = invoke(command, REPOSITORY_ROOT, 30.0)
    if completed.returncode != 0:
        raise BenchmarkError(
            f"failed to query tool identity for '{resolved}' (exit {completed.returncode})"
        )
    output = (completed.stdout + completed.stderr).strip()
    if not output:
        raise BenchmarkError(f"tool identity command produced no output: {command}")
    return Tool(requested, resolved, command, output)


def invoke(
    command: tuple[str, ...] | list[str],
    cwd: Path,
    timeout: float,
) -> tuple[subprocess.CompletedProcess[str], int]:
    environment = os.environ.copy()
    environment["LC_ALL"] = "C"
    environment["LANG"] = "C"
    started = time.perf_counter_ns()
    try:
        completed = subprocess.run(
            list(command),
            cwd=cwd,
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as error:
        raise BenchmarkError(
            f"benchmark command timed out after {timeout:g}s: {list(command)}"
        ) from error
    except OSError as error:
        raise BenchmarkError(
            f"failed to start benchmark command {list(command)}: {error}"
        ) from error
    return completed, time.perf_counter_ns() - started


def require_exact_keys(
    value: dict[str, Any], expected: set[str], context: str
) -> None:
    unknown = sorted(set(value) - expected)
    missing = sorted(expected - set(value))
    if unknown:
        raise BenchmarkError(f"{context} contains unknown fields: {', '.join(unknown)}")
    if missing:
        raise BenchmarkError(f"{context} is missing fields: {', '.join(missing)}")


def strict_json_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise BenchmarkError(f"benchmark JSON repeats field '{key}'")
        result[key] = value
    return result


def require_name(value: Any, context: str) -> str:
    if not isinstance(value, str) or NAME_PATTERN.fullmatch(value) is None:
        raise BenchmarkError(
            f"{context} must match {NAME_PATTERN.pattern!r}, got {value!r}"
        )
    return value


def is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def source_path(workload_directory: Path, value: Any, context: str) -> Path:
    workload_directory = workload_directory.resolve(strict=True)
    if not isinstance(value, str) or not value:
        raise BenchmarkError(f"{context} must be a nonempty relative path")
    relative = Path(value)
    if relative.is_absolute() or ".." in relative.parts:
        raise BenchmarkError(f"{context} must remain beneath the workload directory")
    lexical = workload_directory / relative
    current = lexical
    while current != workload_directory:
        if current.is_symlink():
            raise BenchmarkError(f"{context} may not traverse a symbolic link: {current}")
        current = current.parent
    try:
        resolved = lexical.resolve(strict=True)
    except OSError as error:
        raise BenchmarkError(f"{context} does not name a readable file: {lexical}") from error
    if not resolved.is_file() or not resolved.is_relative_to(workload_directory):
        raise BenchmarkError(f"{context} escapes the workload directory: {lexical}")
    return resolved


def descriptor_path(selection: str) -> Path:
    selected = Path(selection).expanduser()
    named_selection = not selected.exists()
    if selected.exists():
        path = selected / "benchmark.json" if selected.is_dir() else selected
    else:
        name = require_name(selection, "workload selection")
        path = WORKLOAD_ROOT / name / "benchmark.json"
        current = path
        while current != WORKLOAD_ROOT:
            if current.is_symlink():
                raise BenchmarkError(
                    f"named benchmark workload may not traverse a symbolic link: {current}"
                )
            current = current.parent
    if path.is_symlink():
        raise BenchmarkError(f"benchmark descriptor may not be a symbolic link: {path}")
    try:
        resolved = path.resolve(strict=True)
    except OSError as error:
        raise BenchmarkError(f"benchmark descriptor was not found: {path}") from error
    if not resolved.is_file():
        raise BenchmarkError(f"benchmark descriptor is not a file: {resolved}")
    if named_selection and not resolved.is_relative_to(WORKLOAD_ROOT.resolve(strict=True)):
        raise BenchmarkError(
            f"named benchmark workload escapes '{WORKLOAD_ROOT}': {resolved}"
        )
    return resolved


def load_workload(selection: str) -> Workload:
    path = descriptor_path(selection)
    try:
        raw = path.read_bytes()
        decoded = json.loads(raw, object_pairs_hook=strict_json_object)
    except (OSError, UnicodeError, ValueError) as error:
        raise BenchmarkError(f"failed to parse benchmark descriptor '{path}': {error}") from error
    if not isinstance(decoded, dict):
        raise BenchmarkError(f"benchmark descriptor '{path}' must contain an object")
    require_exact_keys(decoded, DESCRIPTOR_KEYS, f"benchmark descriptor '{path}'")
    if decoded["schema"] != 1 or isinstance(decoded["schema"], bool):
        raise BenchmarkError(f"benchmark descriptor '{path}' requires schema 1")
    name = require_name(decoded["name"], "benchmark name")
    if not isinstance(decoded["description"], str) or not decoded["description"].strip():
        raise BenchmarkError(f"benchmark '{name}' requires a nonempty description")
    work_units = decoded["work_units"]
    if (
        not isinstance(work_units, int)
        or isinstance(work_units, bool)
        or not 0 < work_units <= MAXIMUM_WORK_UNITS
    ):
        raise BenchmarkError(
            f"benchmark '{name}' work_units must be an unsigned 64-bit positive integer"
        )
    minimum = decoded["minimum_sample_seconds"]
    try:
        minimum_value = float(minimum) if is_number(minimum) else math.nan
    except OverflowError:
        minimum_value = math.inf
    if not math.isfinite(minimum_value) or minimum_value < 0:
        raise BenchmarkError(
            f"benchmark '{name}' minimum_sample_seconds must be finite and nonnegative"
        )
    entries = decoded["variants"]
    if not isinstance(entries, list) or not 2 <= len(entries) <= MAXIMUM_VARIANTS:
        raise BenchmarkError(
            f"benchmark '{name}' must declare between 2 and {MAXIMUM_VARIANTS} variants"
        )
    directory = path.parent.resolve(strict=True)
    variants: list[Variant] = []
    names: set[str] = set()
    for index, entry in enumerate(entries):
        context = f"benchmark '{name}' variant {index}"
        if not isinstance(entry, dict):
            raise BenchmarkError(f"{context} must contain an object")
        require_exact_keys(entry, VARIANT_KEYS, context)
        variant_name = require_name(entry["name"], f"{context} name")
        if variant_name in names:
            raise BenchmarkError(f"benchmark '{name}' repeats variant '{variant_name}'")
        names.add(variant_name)
        kind = entry["kind"]
        if kind not in {"gti", "cpp"}:
            raise BenchmarkError(f"{context} kind must be 'gti' or 'cpp'")
        source = source_path(directory, entry["source"], f"{context} source")
        expected_suffix = ".gti" if kind == "gti" else ".cpp"
        if source.suffix != expected_suffix:
            raise BenchmarkError(
                f"{context} source must use {expected_suffix}, got {source.name}"
            )
        variants.append(Variant(variant_name, kind, source))
    if sum(variant.kind == "gti" for variant in variants) != 1:
        raise BenchmarkError(f"benchmark '{name}' must declare exactly one GTI variant")
    return Workload(
        name=name,
        description=decoded["description"].strip(),
        work_units=work_units,
        minimum_sample_seconds=minimum_value,
        descriptor_path=path,
        descriptor=decoded,
        descriptor_digest=sha256_bytes(raw),
        variants=tuple(variants),
    )


def canonical_output_root(value: str, workloads: list[Workload]) -> Path:
    requested = Path(value).expanduser()
    if requested.is_symlink():
        raise BenchmarkError(
            f"benchmark output root may not be a symbolic link: {requested}"
        )
    root = requested.resolve(strict=False)
    benchmark_sources = (REPOSITORY_ROOT / "benchmarks").resolve(strict=True)
    if (
        root == benchmark_sources
        or root.is_relative_to(benchmark_sources)
        or benchmark_sources.is_relative_to(root)
    ):
        raise BenchmarkError(
            f"benchmark output root may not be inside source assets: {root}"
        )
    for workload in workloads:
        directory = workload.descriptor_path.parent
        if (
            root == directory
            or root.is_relative_to(directory)
            or directory.is_relative_to(root)
        ):
            raise BenchmarkError(
                f"benchmark output root may not be inside workload '{workload.name}'"
            )
    try:
        root.mkdir(parents=True, exist_ok=True)
        root = root.resolve(strict=True)
    except OSError as error:
        raise BenchmarkError(f"failed to create benchmark output root '{requested}': {error}") from error
    if not root.is_dir():
        raise BenchmarkError(f"benchmark output root is not a directory: {root}")
    return root


def report_path(requested: str | None, default: Path, output_root: Path) -> Path:
    candidate = default if requested is None else Path(requested).expanduser()
    resolved = candidate.resolve(strict=False)
    if not resolved.is_relative_to(output_root):
        raise BenchmarkError(
            f"benchmark report path must remain beneath '{output_root}': {resolved}"
        )
    if candidate.is_symlink():
        raise BenchmarkError(f"benchmark report path may not be a symbolic link: {candidate}")
    lexical = Path(os.path.abspath(candidate))
    if not lexical.is_relative_to(output_root):
        raise BenchmarkError(
            f"benchmark report path must be named beneath '{output_root}': {lexical}"
        )
    current = lexical
    while current != output_root:
        if current.is_symlink():
            raise BenchmarkError(
                f"benchmark report path may not traverse a symbolic link: {current}"
            )
        current = current.parent
    return resolved


def validate_native_flags(flags: list[str]) -> None:
    for flag in flags:
        if "\0" in flag:
            raise BenchmarkError("native compiler flags may not contain NUL bytes")
        if flag not in SAFE_NATIVE_FLAGS and SAFE_NATIVE_FLAG_PATTERN.fullmatch(flag) is None:
            raise BenchmarkError(
                f"native flag '{flag}' is outside the schema-1 target/vectorization allowlist"
            )


def executable_name(name: str) -> str:
    return name + (".exe" if os.name == "nt" else "")


def build_variant(
    workload: Workload,
    variant: Variant,
    directory: Path,
    gti: Tool,
    cxx: Tool,
    optimization: str,
    cpp_standard: str,
    native_flags: list[str],
    timeout: float,
) -> dict[str, Any]:
    variant_directory = directory / variant.name
    variant_directory.mkdir(parents=True, exist_ok=False)
    executable = variant_directory / executable_name(variant.name)
    if variant.kind == "gti":
        command = [
            str(gti.resolved),
            str(variant.source),
            f"-O{optimization}",
            "--std",
            cpp_standard,
            "--cxx",
            str(cxx.resolved),
            "--keep-cpp",
            "-o",
            str(executable),
        ]
        if native_flags:
            command.extend(("--", *native_flags))
    else:
        command = [
            str(cxx.resolved),
            f"-O{optimization}",
            f"-std={cpp_standard}",
            *native_flags,
            *STRICT_NATIVE_FLAGS,
            str(variant.source),
            "-o",
            str(executable),
        ]
    completed, duration = invoke(command, workload.descriptor_path.parent, timeout)
    stdout_path = variant_directory / "build.stdout.txt"
    stderr_path = variant_directory / "build.stderr.txt"
    write_text(stdout_path, completed.stdout)
    write_text(stderr_path, completed.stderr)
    if completed.returncode != 0:
        raise BenchmarkError(
            f"benchmark '{workload.name}' variant '{variant.name}' build failed "
            f"with exit {completed.returncode}; see {stderr_path}"
        )
    if not executable.is_file():
        raise BenchmarkError(
            f"benchmark '{workload.name}' variant '{variant.name}' produced no executable"
        )
    artifacts: dict[str, Any] = {
        "executable": file_evidence(executable),
        "build_stdout": file_evidence(stdout_path),
        "build_stderr": file_evidence(stderr_path),
    }
    if variant.kind == "gti":
        generated = Path(str(executable) + ".gti.cpp")
        if not generated.is_file():
            raise BenchmarkError(
                f"GTI benchmark build did not retain emitted C++ at '{generated}'"
            )
        artifacts["emitted_cpp"] = file_evidence(generated)
    else:
        artifacts["native_input"] = file_evidence(variant.source)
    return {
        "name": variant.name,
        "kind": variant.kind,
        "source": file_evidence(variant.source),
        "command": command,
        "duration_ns": duration,
        "exit_code": completed.returncode,
        "artifacts": artifacts,
        "executable": str(executable),
    }


def execute_variant(
    workload: Workload,
    build: dict[str, Any],
    phase: str,
    timeout: float,
) -> dict[str, Any]:
    command = [build["executable"]]
    completed, duration = invoke(command, Path(build["executable"]).parent, timeout)
    if completed.returncode != 0:
        raise BenchmarkError(
            f"benchmark '{workload.name}' variant '{build['name']}' {phase} "
            f"failed with exit {completed.returncode}"
        )
    if completed.stderr:
        raise BenchmarkError(
            f"benchmark '{workload.name}' variant '{build['name']}' {phase} "
            "wrote unexpected standard error"
        )
    output = completed.stdout
    if output.endswith("\n"):
        output = output[:-1]
    if output.endswith("\r"):
        output = output[:-1]
    match = RESULT_PATTERN.fullmatch(output)
    if match is None:
        raise BenchmarkError(
            f"benchmark '{workload.name}' variant '{build['name']}' {phase} "
            f"produced a malformed result record: {output!r}"
        )
    units = int(match.group(2))
    if units != workload.work_units:
        raise BenchmarkError(
            f"benchmark '{workload.name}' variant '{build['name']}' reported "
            f"{units} work units, expected {workload.work_units}"
        )
    return {
        "command": command,
        "duration_ns": duration,
        "record": output,
        "digest": match.group(1),
        "work_units": units,
    }


def median_absolute_deviation(values: list[int]) -> float:
    median = statistics.median(values)
    return float(statistics.median(abs(value - median) for value in values))


def sample_summary(values: list[int], reference_median: float) -> dict[str, Any]:
    median = float(statistics.median(values))
    mean = float(statistics.fmean(values))
    deviation = float(statistics.stdev(values)) if len(values) > 1 else 0.0
    return {
        "count": len(values),
        "minimum_ns": min(values),
        "maximum_ns": max(values),
        "median_ns": median,
        "mean_ns": mean,
        "sample_standard_deviation_ns": deviation,
        "median_absolute_deviation_ns": median_absolute_deviation(values),
        "coefficient_of_variation": deviation / mean if mean else 0.0,
        "median_ratio_to_reference": median / reference_median,
    }


def measure_workload(
    workload: Workload,
    directory: Path,
    gti: Tool,
    cxx: Tool,
    optimization: str,
    cpp_standard: str,
    native_flags: list[str],
    warmup_count: int,
    run_count: int,
    seed: int,
    timeout: float,
    smoke: bool,
) -> dict[str, Any]:
    directory.mkdir(parents=True, exist_ok=False)
    builds = [
        build_variant(
            workload,
            variant,
            directory,
            gti,
            cxx,
            optimization,
            cpp_standard,
            native_flags,
            timeout,
        )
        for variant in workload.variants
    ]
    validation_builds = list(builds)
    if smoke:
        random.Random(seed).shuffle(validation_builds)
    validations_by_name = {
        build["name"]: execute_variant(workload, build, "validation", timeout)
        for build in validation_builds
    }
    expected_record = validations_by_name[builds[0]["name"]]["record"]
    for build in builds:
        validation = validations_by_name[build["name"]]
        if validation["record"] != expected_record:
            raise BenchmarkError(
                f"benchmark '{workload.name}' variant '{build['name']}' result "
                f"does not match '{builds[0]['name']}'"
            )
    warmups: dict[str, list[int]] = {build["name"]: [] for build in builds}
    for _ in range(warmup_count):
        for build in builds:
            result = execute_variant(workload, build, "warmup", timeout)
            if result["record"] != expected_record:
                raise BenchmarkError(
                    f"benchmark '{workload.name}' changed its result during warmup"
                )
            warmups[build["name"]].append(result["duration_ns"])

    generator = random.Random(seed)
    samples: dict[str, list[int]] = {build["name"]: [] for build in builds}
    sample_order: list[dict[str, Any]] = []
    if smoke:
        # Smoke mode's validation execution is also its sole raw sample. It
        # proves build/result correctness without doubling a long workload in
        # ordinary CI or pretending to establish a timing conclusion.
        for build in validation_builds:
            validation = validations_by_name[build["name"]]
            samples[build["name"]].append(validation["duration_ns"])
        sample_order.append(
            {"round": 0, "variants": [build["name"] for build in validation_builds]}
        )
    else:
        for round_index in range(run_count):
            ordered = list(builds)
            generator.shuffle(ordered)
            round_order: list[str] = []
            for build in ordered:
                result = execute_variant(workload, build, "measurement", timeout)
                if result["record"] != expected_record:
                    raise BenchmarkError(
                        f"benchmark '{workload.name}' changed its result during measurement"
                    )
                samples[build["name"]].append(result["duration_ns"])
                round_order.append(build["name"])
            sample_order.append({"round": round_index, "variants": round_order})

    reference_name = builds[0]["name"]
    reference_median = float(statistics.median(samples[reference_name]))
    summaries = {
        name: sample_summary(values, reference_median)
        for name, values in samples.items()
    }
    reasons: list[str] = []
    if not smoke:
        if run_count < MINIMUM_CONCLUSIVE_SAMPLES:
            reasons.append(
                f"sample_count_below_{MINIMUM_CONCLUSIVE_SAMPLES}"
            )
        minimum_ns = workload.minimum_sample_seconds * 1_000_000_000
        for name, summary in summaries.items():
            if summary["median_ns"] < minimum_ns:
                reasons.append(f"{name}:median_below_declared_minimum")
            if summary["coefficient_of_variation"] > NOISY_CV_LIMIT:
                reasons.append(f"{name}:coefficient_of_variation_above_{NOISY_CV_LIMIT}")
    quality = "smoke" if smoke else ("inconclusive" if reasons else "conclusive")
    for build in builds:
        build["validation"] = validations_by_name[build["name"]]
        build["warmup_samples_ns"] = warmups[build["name"]]
        build["raw_samples_ns"] = samples[build["name"]]
        build["summary"] = summaries[build["name"]]
    return {
        "name": workload.name,
        "description": workload.description,
        "descriptor": workload.descriptor,
        "descriptor_path": str(workload.descriptor_path),
        "descriptor_sha256": workload.descriptor_digest,
        "work_units": workload.work_units,
        "minimum_sample_seconds": workload.minimum_sample_seconds,
        "correctness_record": expected_record,
        "reference_variant": reference_name,
        "sample_order": sample_order,
        "quality": quality,
        "inconclusive_reasons": reasons,
        "variants": builds,
    }


def optional_command(command: list[str], cwd: Path) -> tuple[str | None, str | None]:
    try:
        completed, _ = invoke(command, cwd, 10.0)
    except BenchmarkError as error:
        return None, str(error)
    if completed.returncode != 0:
        return None, (completed.stderr or completed.stdout).strip() or "command failed"
    return completed.stdout.strip(), None


def repository_facts() -> dict[str, Any]:
    root, root_error = optional_command(
        ["git", "rev-parse", "--show-toplevel"], REPOSITORY_ROOT
    )
    if root is None:
        return {"root": None, "commit": None, "dirty": None, "reason": root_error}
    repository = Path(root).resolve()
    commit, commit_error = optional_command(
        ["git", "rev-parse", "HEAD"], repository
    )
    status, status_error = optional_command(
        ["git", "status", "--porcelain"], repository
    )
    return {
        "root": str(repository),
        "commit": commit,
        "dirty": None if status is None else bool(status),
        "reason": commit_error or status_error,
    }


def cpu_model() -> str | None:
    processor = platform.processor().strip()
    if processor:
        return processor
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.is_file():
        try:
            for line in cpuinfo.read_text(encoding="utf-8", errors="replace").splitlines():
                if line.lower().startswith("model name") and ":" in line:
                    return line.split(":", 1)[1].strip()
        except OSError:
            pass
    return os.environ.get("PROCESSOR_IDENTIFIER")


def tool_record(tool: Tool) -> dict[str, Any]:
    return {
        "requested": tool.requested,
        "resolved": str(tool.resolved),
        "version_command": list(tool.version_command),
        "version_output": tool.version_output,
        "executable": file_evidence(tool.resolved),
    }


def markdown_report(report: dict[str, Any]) -> str:
    lines = [
        "# GTI benchmark report",
        "",
        f"Generated: `{report['generated_at_utc']}`",
        "",
        f"Native compiler: `{report['tools']['cxx']['resolved']}`",
        "",
        f"Configuration: `-O{report['configuration']['optimization']} "
        f"-std={report['configuration']['cpp_standard']}`",
        "",
    ]
    for workload in report["workloads"]:
        lines.extend(
            [
                f"## {workload['name']}",
                "",
                f"Correctness: `{workload['correctness_record']}`",
                "",
                f"Measurement quality: **{workload['quality']}**",
                "",
                "| Variant | Median (ms) | MAD (ms) | Ratio | Samples |",
                "| --- | ---: | ---: | ---: | ---: |",
            ]
        )
        for variant in workload["variants"]:
            summary = variant["summary"]
            lines.append(
                f"| {variant['name']} | {summary['median_ns'] / 1_000_000:.6f} "
                f"| {summary['median_absolute_deviation_ns'] / 1_000_000:.6f} "
                f"| {summary['median_ratio_to_reference']:.4f} "
                f"| {summary['count']} |"
            )
        if workload["inconclusive_reasons"]:
            lines.extend(
                [
                    "",
                    "Inconclusive reasons: "
                    + ", ".join(f"`{item}`" for item in workload["inconclusive_reasons"]),
                ]
            )
        lines.append("")
    lines.extend(
        [
            "Raw nanosecond samples, commands, digests, and environment metadata are in the JSON report.",
            "",
        ]
    )
    return "\n".join(lines)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gti", required=True, help="GTI compiler executable")
    parser.add_argument("--cxx", required=True, help="native C++ compiler executable")
    parser.add_argument(
        "--optimization", choices=("0", "1", "2", "3"), default="3"
    )
    parser.add_argument(
        "--cpp-standard", choices=("c++20", "c++23"), default="c++23"
    )
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--runs", type=int, default=20)
    parser.add_argument(
        "--output",
        required=True,
        help="root for all generated benchmark artifacts",
    )
    parser.add_argument("--json", help="JSON report path beneath --output")
    parser.add_argument("--markdown", help="Markdown report path beneath --output")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument(
        "--workload", action="append", required=True, help="workload name or descriptor path"
    )
    parser.add_argument(
        "--native-flag",
        action="append",
        default=[],
        help="native compiler flag applied identically to every variant; use --native-flag=<flag>",
    )
    parser.add_argument(
        "--smoke",
        action="store_true",
        help="validate and collect one sample without timing-quality thresholds",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        if arguments.warmup < 0:
            raise BenchmarkError("--warmup must be nonnegative")
        if arguments.runs <= 0:
            raise BenchmarkError("--runs must be positive")
        if not math.isfinite(arguments.timeout) or arguments.timeout <= 0:
            raise BenchmarkError("--timeout must be positive")
        validate_native_flags(arguments.native_flag)
        workloads = [load_workload(selection) for selection in arguments.workload]
        workload_names = [workload.name for workload in workloads]
        if len(workload_names) != len(set(workload_names)):
            raise BenchmarkError("each benchmark workload may be selected only once")
        output_root = canonical_output_root(arguments.output, workloads)
        run_name = (
            "run-"
            + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            + f"-{os.getpid()}-{secrets.token_hex(4)}"
        )
        run_directory = output_root / run_name
        run_directory.mkdir(parents=False, exist_ok=False)
        json_path = report_path(
            arguments.json, run_directory / "report.json", output_root
        )
        markdown_path = report_path(
            arguments.markdown, run_directory / "report.md", output_root
        )
        if json_path == markdown_path:
            raise BenchmarkError("JSON and Markdown report paths must be different")

        gti = resolved_tool(arguments.gti, ("--version",))
        cxx = resolved_tool(arguments.cxx, ("--version",))
        effective_warmup = 0 if arguments.smoke else arguments.warmup
        effective_runs = 1 if arguments.smoke else arguments.runs
        reports = []
        for index, workload in enumerate(workloads):
            reports.append(
                measure_workload(
                    workload,
                    run_directory / f"{index:02d}-{workload.name}",
                    gti,
                    cxx,
                    arguments.optimization,
                    arguments.cpp_standard,
                    list(arguments.native_flag),
                    effective_warmup,
                    effective_runs,
                    arguments.seed,
                    arguments.timeout,
                    arguments.smoke,
                )
            )

        report = {
            "schema": 1,
            "generated_at_utc": datetime.now(timezone.utc).isoformat(),
            "runner": {
                "path": str(Path(__file__).resolve()),
                "sha256": file_evidence(Path(__file__).resolve())["sha256"],
            },
            "repository": repository_facts(),
            "tools": {"gti": tool_record(gti), "cxx": tool_record(cxx)},
            "environment": {
                "os": platform.system(),
                "os_release": platform.release(),
                "kernel_version": platform.version(),
                "architecture": platform.machine(),
                "cpu_model": cpu_model(),
                "logical_cpu_count": os.cpu_count(),
                "python": platform.python_version(),
                "gti_toolchain_overrides": {
                    name: os.environ.get(name)
                    for name in (
                        "GTI_STDLIB_PATH",
                        "GTI_RUNTIME_INCLUDE",
                        "GTI_RUNTIME_LIBRARY",
                        "GTI_VENDOR_INCLUDE",
                    )
                },
            },
            "configuration": {
                "optimization": arguments.optimization,
                "cpp_standard": arguments.cpp_standard,
                "native_flags": list(arguments.native_flag),
                "strict_native_flags": list(STRICT_NATIVE_FLAGS),
                "requested_warmups": arguments.warmup,
                "requested_runs": arguments.runs,
                "effective_warmups": effective_warmup,
                "effective_runs": effective_runs,
                "timeout_seconds": arguments.timeout,
                "seed": arguments.seed,
                "smoke": arguments.smoke,
            },
            "output_root": str(output_root),
            "run_directory": str(run_directory),
            "workloads": reports,
        }
        write_atomic(json_path, json.dumps(report, indent=2, sort_keys=True) + "\n")
        write_atomic(markdown_path, markdown_report(report))
        print(f"Benchmark JSON: {json_path}")
        print(f"Benchmark Markdown: {markdown_path}")
        for workload in reports:
            print(
                f"{workload['name']}: {workload['correctness_record']} "
                f"[{workload['quality']}]"
            )
        return 0
    except BenchmarkError as error:
        print(f"benchmark error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
