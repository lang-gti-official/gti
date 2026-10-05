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
VARIANT_SCHEMA_2_KEYS = {*VARIANT_KEYS, "execution_profile"}
EXECUTION_PROFILES = {"single-threaded", "concurrent"}
OPTIMIZATION_LEVELS = ("0", "1", "2", "3")
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
    "-D__gti_strict_ieee754=1",
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
GIT_COMMIT_PATTERN = re.compile(r"^[0-9a-f]{40}$")
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
ARITHMETIC_SUCCESS_PROXY_KIND = "generated-arithmetic-success-proxy"
ARITHMETIC_SUCCESS_PROXY_SUFFIX = "-arithmetic-success-proxy"
ARITHMETIC_SUCCESS_PROXY_HELPERS = (
    ("gti_internal::backend::mir_checked_add_v1", "mir_checked_add_v1"),
    (
        "gti_internal::backend::mir_checked_subtract_v1",
        "mir_checked_subtract_v1",
    ),
    (
        "gti_internal::backend::mir_checked_multiply_v1",
        "mir_checked_multiply_v1",
    ),
)
BACKEND_NAMESPACE_OPEN = "namespace gti_internal::backend {"
BACKEND_NAMESPACE_CLOSE = "} // namespace gti_internal::backend"
ARITHMETIC_OVERFLOW_GUARD = "if (overflow) {"
ARITHMETIC_SUCCESS_GUARD = "if (false) {"


class BenchmarkError(RuntimeError):
    """A benchmark contract, tool, build, or execution failure."""


@dataclass(frozen=True)
class Variant:
    name: str
    kind: str
    source: Path
    execution_profile: str | None


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
    schema = decoded["schema"]
    if (
        not isinstance(schema, int)
        or isinstance(schema, bool)
        or schema not in {1, 2}
    ):
        raise BenchmarkError(
            f"benchmark descriptor '{path}' requires schema 1 or 2"
        )
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
        require_exact_keys(
            entry, VARIANT_KEYS if schema == 1 else VARIANT_SCHEMA_2_KEYS, context
        )
        variant_name = require_name(entry["name"], f"{context} name")
        if variant_name in names:
            raise BenchmarkError(f"benchmark '{name}' repeats variant '{variant_name}'")
        names.add(variant_name)
        kind = entry["kind"]
        if not isinstance(kind, str) or kind not in {"gti", "cpp"}:
            raise BenchmarkError(f"{context} kind must be 'gti' or 'cpp'")
        source = source_path(directory, entry["source"], f"{context} source")
        expected_suffix = ".gti" if kind == "gti" else ".cpp"
        if source.suffix != expected_suffix:
            raise BenchmarkError(
                f"{context} source must use {expected_suffix}, got {source.name}"
            )
        execution_profile = entry.get("execution_profile")
        if schema == 2:
            if kind == "gti" and (
                not isinstance(execution_profile, str)
                or execution_profile not in EXECUTION_PROFILES
            ):
                raise BenchmarkError(
                    f"{context} execution_profile must be 'single-threaded' "
                    "or 'concurrent' for a GTI variant"
                )
            if kind == "cpp" and execution_profile is not None:
                raise BenchmarkError(
                    f"{context} execution_profile must be null for a C++ variant"
                )
        variants.append(Variant(variant_name, kind, source, execution_profile))
    gti_variants = sum(variant.kind == "gti" for variant in variants)
    if schema == 1 and gti_variants != 1:
        raise BenchmarkError(f"benchmark '{name}' must declare exactly one GTI variant")
    if schema == 2 and gti_variants == 0:
        raise BenchmarkError(f"benchmark '{name}' must declare at least one GTI variant")
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


def arithmetic_success_proxy_targets(
    selections: list[str],
    workloads: list[Workload],
    native_optimization: str | None,
) -> dict[str, tuple[str, ...]]:
    if not selections:
        return {}
    if native_optimization is None:
        raise BenchmarkError(
            "--arithmetic-success-proxy requires --native-optimization"
        )

    workloads_by_name = {workload.name: workload for workload in workloads}
    selected: dict[str, list[str]] = {}
    seen: set[tuple[str, str]] = set()
    for selection in selections:
        if selection.count("/") != 1:
            raise BenchmarkError(
                "arithmetic success proxy selection must use WORKLOAD/VARIANT"
            )
        workload_name, variant_name = selection.split("/", 1)
        require_name(workload_name, "arithmetic success proxy workload")
        require_name(variant_name, "arithmetic success proxy variant")
        identity = (workload_name, variant_name)
        if identity in seen:
            raise BenchmarkError(
                "arithmetic success proxy selection repeats "
                f"'{workload_name}/{variant_name}'"
            )
        seen.add(identity)

        workload = workloads_by_name.get(workload_name)
        if workload is None:
            raise BenchmarkError(
                f"arithmetic success proxy workload '{workload_name}' was not selected"
            )
        variants_by_name = {variant.name: variant for variant in workload.variants}
        variant = variants_by_name.get(variant_name)
        if variant is None:
            raise BenchmarkError(
                f"benchmark '{workload_name}' has no variant '{variant_name}'"
            )
        if variant.kind != "gti":
            raise BenchmarkError(
                f"arithmetic success proxy target '{workload_name}/{variant_name}' "
                "must be a GTI variant"
            )
        proxy_name = variant_name + ARITHMETIC_SUCCESS_PROXY_SUFFIX
        if proxy_name in variants_by_name:
            raise BenchmarkError(
                f"arithmetic success proxy name '{proxy_name}' collides with a "
                "declared variant"
            )
        targets = selected.setdefault(workload_name, [])
        if len(workload.variants) + len(targets) + 1 > MAXIMUM_VARIANTS:
            raise BenchmarkError(
                f"benchmark '{workload_name}' exceeds the {MAXIMUM_VARIANTS}-variant "
                "limit after adding arithmetic success proxies"
            )
        targets.append(variant_name)
    return {name: tuple(targets) for name, targets in selected.items()}


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


def require_integer(value: Any, context: str, minimum: int = 0) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < minimum:
        raise BenchmarkError(f"{context} must be an integer >= {minimum}")
    return value


def validate_build_file(
    value: Any, context: str, expected: Path | None = None
) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise BenchmarkError(f"{context} must contain an object")
    require_exact_keys(value, {"canonical_path", "bytes", "sha256"}, context)
    path_value = value["canonical_path"]
    if not isinstance(path_value, str) or not path_value:
        raise BenchmarkError(f"{context}.canonical_path must be a nonempty string")
    path = Path(path_value)
    if not path.is_absolute():
        raise BenchmarkError(f"{context}.canonical_path must be absolute")
    try:
        canonical = path.resolve(strict=True)
    except OSError as error:
        raise BenchmarkError(f"{context} names missing material '{path}'") from error
    if not canonical.is_file() or canonical != path:
        raise BenchmarkError(f"{context}.canonical_path is not canonical: {path}")
    if expected is not None and canonical != expected.resolve(strict=True):
        raise BenchmarkError(
            f"{context} identifies '{canonical}', expected '{expected.resolve(strict=True)}'"
        )
    recorded_bytes = require_integer(value["bytes"], f"{context}.bytes")
    sha256 = value["sha256"]
    if not isinstance(sha256, str) or SHA256_PATTERN.fullmatch(sha256) is None:
        raise BenchmarkError(f"{context}.sha256 must be 64 lowercase hex digits")
    actual = file_evidence(canonical)
    if actual["bytes"] != recorded_bytes or actual["sha256"] != sha256:
        raise BenchmarkError(f"{context} no longer matches its recorded material")
    return value


def validate_optional_string(value: Any, context: str) -> None:
    if value is not None and not isinstance(value, str):
        raise BenchmarkError(f"{context} must be a string or null")


def native_argv_contains_file(argv: list[str], expected: Path, cwd: Path) -> bool:
    for argument in argv[1:]:
        if argument.startswith("-"):
            continue
        candidate = Path(argument)
        if not candidate.is_absolute():
            candidate = cwd / candidate
        try:
            if candidate.samefile(expected):
                return True
        except OSError:
            continue
    return False


def fixed_native_rebuild_command(
    native_argv: list[str],
    generated_source: Path,
    driver_executable: Path,
    executable: Path,
    dependency_file: Path,
    cpp_standard: str,
    gti_optimization: str,
    native_optimization: str,
    cwd: Path,
    replacement_source: Path | None = None,
) -> list[str]:
    if gti_optimization not in OPTIMIZATION_LEVELS:
        raise BenchmarkError("GTI optimization level must be 0, 1, 2, or 3")
    if native_optimization not in OPTIMIZATION_LEVELS:
        raise BenchmarkError("native optimization level must be 0, 1, 2, or 3")

    command = list(native_argv)
    expected_standard = f"-std={cpp_standard}"
    if len(command) < 3 or command[1] != expected_standard:
        raise BenchmarkError(
            "GTI native argv does not begin with the selected C++ standard"
        )
    optimization_indices = [
        index for index, argument in enumerate(command) if argument.startswith("-O")
    ]
    if optimization_indices != [2] or command[2] != f"-O{gti_optimization}":
        raise BenchmarkError(
            "GTI native argv does not contain exactly its driver-owned optimization flag"
        )

    def unique_option_value(option: str) -> int:
        indices = [index for index, argument in enumerate(command) if argument == option]
        if len(indices) != 1 or indices[0] + 1 >= len(command):
            raise BenchmarkError(
                f"GTI native argv must contain exactly one '{option}' value"
            )
        return indices[0] + 1

    dependency_index = unique_option_value("-MF")
    output_index = unique_option_value("-o")

    def resolves_to(argument: str, expected: Path) -> bool:
        candidate = Path(argument)
        if not candidate.is_absolute():
            candidate = cwd / candidate
        try:
            return candidate.samefile(expected)
        except OSError:
            return False

    source_indices = [
        index
        for index, argument in enumerate(command)
        if not argument.startswith("-") and resolves_to(argument, generated_source)
    ]
    if len(source_indices) != 1:
        raise BenchmarkError(
            "GTI native argv does not contain exactly one retained generated source"
        )
    recorded_output = Path(command[output_index])
    if not recorded_output.is_absolute():
        recorded_output = cwd / recorded_output
    recorded_output = Path(os.path.abspath(recorded_output))
    canonical_output_parent = driver_executable.parent.resolve(strict=True)
    staging_name = recorded_output.parent.name
    staging_prefix = f".{driver_executable.stem}.gti-stage-"
    staging_suffix = driver_executable.suffix
    staging_identity = staging_name[
        len(staging_prefix) : len(staging_name) - len(staging_suffix)
        if staging_suffix
        else None
    ]
    records_driver_staging_output = (
        recorded_output.name == driver_executable.name
        and recorded_output.parent.parent.resolve(strict=False)
        == canonical_output_parent
        and staging_name.startswith(staging_prefix)
        and staging_name.endswith(staging_suffix)
        and re.fullmatch(r"[0-9]+-[0-9]+-[0-9]+", staging_identity) is not None
    )
    if not resolves_to(command[output_index], driver_executable) and not (
        records_driver_staging_output
    ):
        raise BenchmarkError(
            "GTI native argv output does not identify the driver-built executable"
        )

    command[2] = f"-O{native_optimization}"
    command[dependency_index] = str(dependency_file)
    command[output_index] = str(executable)
    if replacement_source is not None:
        try:
            replacement_source = replacement_source.resolve(strict=True)
        except OSError as error:
            raise BenchmarkError(
                f"fixed-native replacement source is unavailable: {replacement_source}"
            ) from error
        if not replacement_source.is_file():
            raise BenchmarkError(
                f"fixed-native replacement source is not a file: {replacement_source}"
            )
        command[source_indices[0]] = str(replacement_source)
    return command


def matching_cpp_delimiter(
    source: str, opening_index: int, opening: str, closing: str
) -> int:
    if opening_index >= len(source) or source[opening_index] != opening:
        raise BenchmarkError("generated C++ delimiter scan did not begin at its opening")
    depth = 0
    index = opening_index
    while index < len(source):
        character = source[index]
        if source.startswith("//", index):
            newline = source.find("\n", index + 2)
            index = len(source) if newline == -1 else newline + 1
            continue
        if source.startswith("/*", index):
            end = source.find("*/", index + 2)
            if end == -1:
                raise BenchmarkError("generated C++ contains an unterminated comment")
            index = end + 2
            continue
        if character in {'"', "'"}:
            quote = character
            index += 1
            while index < len(source):
                if source[index] == "\\":
                    index += 2
                    continue
                if source[index] == quote:
                    index += 1
                    break
                index += 1
            else:
                raise BenchmarkError("generated C++ contains an unterminated literal")
            continue
        if character == opening:
            depth += 1
        elif character == closing:
            depth -= 1
            if depth == 0:
                return index
            if depth < 0:
                break
        index += 1
    raise BenchmarkError("generated C++ contains an unmatched compiler helper delimiter")


def backend_namespace_ranges(source: str) -> list[tuple[int, int]]:
    openings = source.count(BACKEND_NAMESPACE_OPEN)
    closings = source.count(BACKEND_NAMESPACE_CLOSE)
    if openings == 0 or openings != closings:
        raise BenchmarkError(
            "generated C++ does not contain balanced compiler-owned backend namespaces"
        )
    ranges: list[tuple[int, int]] = []
    cursor = 0
    for _ in range(openings):
        start = source.find(BACKEND_NAMESPACE_OPEN, cursor)
        end = source.find(BACKEND_NAMESPACE_CLOSE, start + len(BACKEND_NAMESPACE_OPEN))
        if start == -1 or end == -1:
            raise BenchmarkError(
                "generated C++ backend namespace markers are not sequential"
            )
        ranges.append((start, end + len(BACKEND_NAMESPACE_CLOSE)))
        cursor = end + len(BACKEND_NAMESPACE_CLOSE)
    return ranges


def arithmetic_success_proxy_source(
    source: str,
) -> tuple[str, list[dict[str, Any]]]:
    namespaces = backend_namespace_ranges(source)
    edits: list[tuple[int, int, str]] = []
    replacements: list[dict[str, Any]] = []
    for qualified_name, helper_name in ARITHMETIC_SUCCESS_PROXY_HELPERS:
        definition = f"inline mir_failure_status_v1 {helper_name}("
        definitions: list[int] = []
        for start, end in namespaces:
            cursor = start
            while True:
                found = source.find(definition, cursor, end)
                if found == -1:
                    break
                definitions.append(found)
                cursor = found + len(definition)
        if len(definitions) != 1:
            raise BenchmarkError(
                f"generated C++ must contain exactly one compiler-owned definition "
                f"of '{qualified_name}', found {len(definitions)}"
            )

        definition_start = definitions[0]
        parameters_open = definition_start + len(definition) - 1
        parameters_close = matching_cpp_delimiter(
            source, parameters_open, "(", ")"
        )
        cursor = parameters_close + 1
        while cursor < len(source) and source[cursor].isspace():
            cursor += 1
        if not source.startswith("noexcept", cursor):
            raise BenchmarkError(
                f"compiler-owned helper '{qualified_name}' has an unexpected signature"
            )
        cursor += len("noexcept")
        while cursor < len(source) and source[cursor].isspace():
            cursor += 1
        if cursor >= len(source) or source[cursor] != "{":
            raise BenchmarkError(
                f"compiler-owned helper '{qualified_name}' has no bounded function body"
            )
        body_close = matching_cpp_delimiter(source, cursor, "{", "}")
        body = source[cursor : body_close + 1]
        guard_count = body.count(ARITHMETIC_OVERFLOW_GUARD)
        if guard_count != 1:
            raise BenchmarkError(
                f"compiler-owned helper '{qualified_name}' must contain exactly one "
                f"'{ARITHMETIC_OVERFLOW_GUARD}' guard, found {guard_count}"
            )
        guard_index = cursor + body.index(ARITHMETIC_OVERFLOW_GUARD)
        edits.append(
            (
                guard_index,
                guard_index + len(ARITHMETIC_OVERFLOW_GUARD),
                ARITHMETIC_SUCCESS_GUARD,
            )
        )
        replacements.append(
            {
                "function": qualified_name,
                "from": ARITHMETIC_OVERFLOW_GUARD,
                "to": ARITHMETIC_SUCCESS_GUARD,
                "count": 1,
            }
        )

    transformed = source
    for start, end, replacement in sorted(edits, reverse=True):
        transformed = transformed[:start] + replacement + transformed[end:]
    if transformed == source or len(edits) != len(ARITHMETIC_SUCCESS_PROXY_HELPERS):
        raise BenchmarkError("arithmetic success proxy transformation was incomplete")
    return transformed, replacements


def load_build_provenance(
    path: Path,
    gti: Tool,
    cxx: Tool,
    expected_entry: Path,
    native_cwd: Path | None = None,
) -> dict[str, Any]:
    try:
        raw = path.read_bytes()
        report = json.loads(raw, object_pairs_hook=strict_json_object)
    except (OSError, UnicodeError, ValueError) as error:
        raise BenchmarkError(f"failed to parse GTI build report '{path}': {error}") from error
    if not isinstance(report, dict):
        raise BenchmarkError(f"GTI build report '{path}' must contain an object")
    require_exact_keys(
        report,
        {"schema", "producer", "project", "sources", "native", "artifact"}
        | ({"debug_info"} if "debug_info" in report else set()),
        "GTI build report",
    )
    if report["schema"] != 2 or isinstance(report["schema"], bool):
        raise BenchmarkError("GTI build report requires schema 2")
    # Schema-2 reports predating source debugging omit this additive field.
    # This harness never requests GTI source mappings, so accepting a true
    # policy would silently measure a different generated-source workflow.
    debug_info = report.get("debug_info", False)
    if not isinstance(debug_info, bool):
        raise BenchmarkError("GTI build report debug_info must be a boolean")
    if debug_info:
        raise BenchmarkError("GTI benchmark build report must disable source debug info")
    # Benchmarks build one entry source directly, so the record must carry no
    # project selection; a project section here would mean the measured
    # command was not the one this harness issued.
    if report["project"] is not None:
        raise BenchmarkError("GTI benchmark build report must record a direct compilation")

    producer = report["producer"]
    if not isinstance(producer, dict):
        raise BenchmarkError("GTI build report producer must contain an object")
    require_exact_keys(producer, {"executable", "compiler_build"}, "GTI build report producer")
    validate_build_file(producer["executable"], "GTI build report producer executable", gti.resolved)
    compiler_build = producer["compiler_build"]
    if not isinstance(compiler_build, dict):
        raise BenchmarkError("GTI compiler build identity must contain an object")
    require_exact_keys(compiler_build, {"state", "commit"}, "GTI compiler build identity")
    state = compiler_build["state"]
    commit = compiler_build["commit"]
    if state not in {"clean", "dirty", "unavailable"}:
        raise BenchmarkError("GTI compiler build state is invalid")
    if state == "unavailable":
        if commit is not None:
            raise BenchmarkError("an unavailable compiler build identity must not claim a commit")
    elif not isinstance(commit, str) or GIT_COMMIT_PATTERN.fullmatch(commit) is None:
        raise BenchmarkError("a clean or dirty compiler build identity requires a commit")

    sources = report["sources"]
    if not isinstance(sources, dict):
        raise BenchmarkError("GTI build report sources must contain an object")
    require_exact_keys(
        sources,
        {"entry_root", "prelude_roots", "compilation_order", "materials_sha256", "units", "dependencies"},
        "GTI build report sources",
    )
    entry_root = require_integer(sources["entry_root"], "GTI source entry_root", 1)
    materials_sha256 = sources["materials_sha256"]
    if not isinstance(materials_sha256, str) or SHA256_PATTERN.fullmatch(materials_sha256) is None:
        raise BenchmarkError("GTI source materials_sha256 is invalid")
    units = sources["units"]
    if not isinstance(units, list) or not units:
        raise BenchmarkError("GTI source provenance requires at least one unit")
    unit_ids: set[int] = set()
    units_by_id: dict[int, dict[str, Any]] = {}
    unit_keys = {
        "id", "role", "logical_identity", "standard_library_name",
        "package_identity", "package_relative_path", "entry", "prelude", "material",
    }
    for index, unit in enumerate(units):
        context = f"GTI source unit {index}"
        if not isinstance(unit, dict):
            raise BenchmarkError(f"{context} must contain an object")
        require_exact_keys(unit, unit_keys, context)
        unit_id = require_integer(unit["id"], f"{context}.id", 1)
        if unit_id in unit_ids:
            raise BenchmarkError(f"GTI source provenance repeats unit id {unit_id}")
        unit_ids.add(unit_id)
        units_by_id[unit_id] = unit
        if unit["role"] not in {"application", "prelude", "standard-library"}:
            raise BenchmarkError(f"{context}.role is invalid")
        for field in ("logical_identity", "standard_library_name", "package_identity", "package_relative_path"):
            validate_optional_string(unit[field], f"{context}.{field}")
        if not isinstance(unit["entry"], bool) or not isinstance(unit["prelude"], bool):
            raise BenchmarkError(f"{context} entry/prelude markers must be booleans")
        validate_build_file(unit["material"], f"{context}.material")
    if entry_root not in units_by_id or not units_by_id[entry_root]["entry"]:
        raise BenchmarkError("GTI source entry_root does not identify the entry unit")
    entry_path = Path(units_by_id[entry_root]["material"]["canonical_path"])
    if entry_path != expected_entry.resolve(strict=True):
        raise BenchmarkError("GTI source entry_root differs from the benchmark variant source")

    prelude_roots = sources["prelude_roots"]
    compilation_order = sources["compilation_order"]
    if not isinstance(prelude_roots, list) or not isinstance(compilation_order, list):
        raise BenchmarkError("GTI source roots/order must be arrays")
    parsed_preludes = [require_integer(value, "GTI prelude root", 1) for value in prelude_roots]
    if len(parsed_preludes) != len(set(parsed_preludes)):
        raise BenchmarkError("GTI source provenance repeats a prelude root")
    if any(root not in units_by_id or not units_by_id[root]["prelude"] for root in parsed_preludes):
        raise BenchmarkError("GTI source provenance has an invalid prelude root")
    parsed_order = [require_integer(value, "GTI compilation order id", 1) for value in compilation_order]
    if len(parsed_order) != len(unit_ids) or set(parsed_order) != unit_ids:
        raise BenchmarkError("GTI compilation order is not an exact permutation of source units")

    dependencies = sources["dependencies"]
    if not isinstance(dependencies, list):
        raise BenchmarkError("GTI source dependencies must be an array")
    dependency_keys = {"source", "target", "kind", "include_spelling", "include_occurrence"}
    for index, dependency in enumerate(dependencies):
        context = f"GTI source dependency {index}"
        if not isinstance(dependency, dict):
            raise BenchmarkError(f"{context} must contain an object")
        require_exact_keys(dependency, dependency_keys, context)
        source = require_integer(dependency["source"], f"{context}.source", 1)
        target = require_integer(dependency["target"], f"{context}.target", 1)
        if source not in unit_ids or target not in unit_ids:
            raise BenchmarkError(f"{context} references an unknown unit")
        if dependency["kind"] not in {"include", "prelude", "standard-library", "package"}:
            raise BenchmarkError(f"{context}.kind is invalid")
        if not isinstance(dependency["include_spelling"], str):
            raise BenchmarkError(f"{context}.include_spelling must be a string")
        require_integer(dependency["include_occurrence"], f"{context}.include_occurrence")

    native = report["native"]
    if not isinstance(native, dict):
        raise BenchmarkError("GTI native provenance must contain an object")
    require_exact_keys(native, {"compiler_executable", "argv", "runtime"}, "GTI native provenance")
    validate_build_file(native["compiler_executable"], "GTI native compiler executable", cxx.resolved)
    argv = native["argv"]
    if not isinstance(argv, list) or not argv or any(not isinstance(item, str) or "\0" in item for item in argv):
        raise BenchmarkError("GTI native argv must be a nonempty string array")
    if Path(argv[0]).resolve(strict=True) != cxx.resolved:
        raise BenchmarkError("GTI native argv does not use the selected native compiler")
    if "-MD" not in argv or "-MF" not in argv:
        raise BenchmarkError("GTI native argv did not capture compiler-owned dependencies")

    runtime = native["runtime"]
    if not isinstance(runtime, dict):
        raise BenchmarkError("GTI runtime provenance must contain an object")
    require_exact_keys(runtime, {"materials_sha256", "headers", "archive"}, "GTI runtime provenance")
    runtime_sha256 = runtime["materials_sha256"]
    if not isinstance(runtime_sha256, str) or SHA256_PATTERN.fullmatch(runtime_sha256) is None:
        raise BenchmarkError("GTI runtime materials_sha256 is invalid")
    headers = runtime["headers"]
    if not isinstance(headers, list) or not headers:
        raise BenchmarkError("GTI runtime provenance requires used headers")
    saw_runtime = False
    for index, header in enumerate(headers):
        context = f"GTI runtime header {index}"
        if not isinstance(header, dict):
            raise BenchmarkError(f"{context} must contain an object")
        require_exact_keys(header, {"kind", "logical_identity", "material"}, context)
        if header["kind"] not in {"runtime", "compatibility"}:
            raise BenchmarkError(f"{context}.kind is invalid")
        saw_runtime = saw_runtime or header["kind"] == "runtime"
        if not isinstance(header["logical_identity"], str) or not header["logical_identity"]:
            raise BenchmarkError(f"{context}.logical_identity must be nonempty")
        validate_build_file(header["material"], f"{context}.material")
    if not saw_runtime:
        raise BenchmarkError("GTI runtime provenance contains no runtime header")
    validate_build_file(report["artifact"], "GTI built artifact")
    archive = validate_build_file(runtime["archive"], "GTI runtime archive")
    if not native_argv_contains_file(
        argv,
        Path(archive["canonical_path"]),
        native_cwd if native_cwd is not None else expected_entry.parent,
    ):
        raise BenchmarkError("GTI runtime archive is not present in the exact native argv")
    return {
        "sidecar": {"path": str(path), "sha256": sha256_bytes(raw), "bytes": len(raw)},
        "build": report,
    }


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
    native_optimization: str | None = None,
) -> dict[str, Any]:
    variant_directory = directory / variant.name
    variant_directory.mkdir(parents=True, exist_ok=False)
    executable = variant_directory / executable_name(variant.name)
    driver_executable = (
        variant_directory / executable_name(f"{variant.name}-driver-build")
        if variant.kind == "gti" and native_optimization is not None
        else executable
    )
    build_report = variant_directory / "gti-build-report.json"
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
        ]
        if variant.execution_profile is not None:
            command.extend(("--execution-profile", variant.execution_profile))
        if workload.descriptor["schema"] == 2 or native_optimization is not None:
            command.extend(("--build-report", str(build_report)))
        command.extend(("-o", str(driver_executable)))
        if native_flags:
            command.extend(("--", *native_flags))
    else:
        selected_native_optimization = native_optimization or optimization
        command = [
            str(cxx.resolved),
            f"-O{selected_native_optimization}",
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
    if not driver_executable.is_file():
        raise BenchmarkError(
            f"benchmark '{workload.name}' variant '{variant.name}' produced no executable"
        )
    artifacts: dict[str, Any] = {
        "build_stdout": file_evidence(stdout_path),
        "build_stderr": file_evidence(stderr_path),
    }
    provenance: dict[str, Any] | None = None
    if variant.kind == "gti":
        generated = Path(str(driver_executable) + ".gti.cpp")
        if not generated.is_file():
            raise BenchmarkError(
                f"GTI benchmark build did not retain emitted C++ at '{generated}'"
            )
        artifacts["emitted_cpp"] = file_evidence(generated)
        if workload.descriptor["schema"] == 2 or native_optimization is not None:
            if not build_report.is_file():
                raise BenchmarkError(
                    f"GTI benchmark build did not publish provenance at '{build_report}'"
                )
            provenance = load_build_provenance(
                build_report,
                gti,
                cxx,
                variant.source,
                workload.descriptor_path.parent,
            )
    else:
        artifacts["native_input"] = file_evidence(variant.source)

    native_rebuild: dict[str, Any] | None = None
    if variant.kind == "gti" and native_optimization is not None:
        if provenance is None:
            raise BenchmarkError(
                "fixed-native GTI build requires validated driver provenance"
            )
        dependency_file = variant_directory / "fixed-native.d"
        rebuild_stdout_path = variant_directory / "fixed-native.stdout.txt"
        rebuild_stderr_path = variant_directory / "fixed-native.stderr.txt"
        rebuild_command = fixed_native_rebuild_command(
            provenance["build"]["native"]["argv"],
            generated,
            driver_executable,
            executable,
            dependency_file,
            cpp_standard,
            optimization,
            native_optimization,
            workload.descriptor_path.parent,
        )
        rebuilt, rebuild_duration = invoke(
            rebuild_command, workload.descriptor_path.parent, timeout
        )
        write_text(rebuild_stdout_path, rebuilt.stdout)
        write_text(rebuild_stderr_path, rebuilt.stderr)
        if rebuilt.returncode != 0:
            raise BenchmarkError(
                f"benchmark '{workload.name}' variant '{variant.name}' fixed-native "
                f"rebuild failed with exit {rebuilt.returncode}; see {rebuild_stderr_path}"
            )
        if not executable.is_file():
            raise BenchmarkError(
                f"benchmark '{workload.name}' variant '{variant.name}' fixed-native "
                "rebuild produced no executable"
            )
        if not dependency_file.is_file():
            raise BenchmarkError(
                f"benchmark '{workload.name}' variant '{variant.name}' fixed-native "
                "rebuild produced no dependency file"
            )
        artifacts["driver_executable"] = file_evidence(driver_executable)
        native_rebuild = {
            "optimization": native_optimization,
            "command": rebuild_command,
            "duration_ns": rebuild_duration,
            "exit_code": rebuilt.returncode,
            "stdout": file_evidence(rebuild_stdout_path),
            "stderr": file_evidence(rebuild_stderr_path),
            "dependency_file": file_evidence(dependency_file),
        }

    artifacts["executable"] = file_evidence(executable)
    result = {
        "name": variant.name,
        "kind": variant.kind,
        "source": file_evidence(variant.source),
        "command": command,
        "duration_ns": duration,
        "exit_code": completed.returncode,
        "artifacts": artifacts,
        "executable": str(executable),
    }
    if native_rebuild is not None:
        result["native_rebuild"] = native_rebuild
    if workload.descriptor["schema"] == 2:
        result["execution_profile"] = (
            variant.execution_profile if variant.kind == "gti" else None
        )
    if provenance is not None:
        result["provenance"] = provenance
    return result


def build_arithmetic_success_proxy(
    workload: Workload,
    target: dict[str, Any],
    directory: Path,
    cpp_standard: str,
    gti_optimization: str,
    native_optimization: str,
    timeout: float,
) -> dict[str, Any]:
    if target.get("kind") != "gti" or "provenance" not in target:
        raise BenchmarkError(
            f"benchmark '{workload.name}' arithmetic success proxy target "
            "is not a provenance-validated GTI build"
        )
    artifacts = target.get("artifacts")
    if not isinstance(artifacts, dict) or not {
        "emitted_cpp",
        "driver_executable",
    }.issubset(artifacts):
        raise BenchmarkError(
            f"benchmark '{workload.name}' arithmetic success proxy target "
            "has no retained fixed-native inputs"
        )

    generated_record = artifacts["emitted_cpp"]
    generated_source = Path(generated_record["path"])
    current_generated = file_evidence(generated_source)
    if current_generated != generated_record:
        raise BenchmarkError(
            f"benchmark '{workload.name}' retained emitted C++ changed before "
            "arithmetic proxy generation"
        )
    driver_record = artifacts["driver_executable"]
    driver_executable = Path(driver_record["path"])
    if file_evidence(driver_executable) != driver_record:
        raise BenchmarkError(
            f"benchmark '{workload.name}' driver executable changed before "
            "arithmetic proxy generation"
        )
    try:
        generated_text = generated_source.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise BenchmarkError(
            f"failed to read retained emitted C++ '{generated_source}': {error}"
        ) from error
    transformed_text, replacements = arithmetic_success_proxy_source(generated_text)

    proxy_name = target["name"] + ARITHMETIC_SUCCESS_PROXY_SUFFIX
    proxy_directory = directory / proxy_name
    proxy_directory.mkdir(parents=True, exist_ok=False)
    transformed_source = proxy_directory / f"{proxy_name}.gti.cpp"
    executable = proxy_directory / executable_name(proxy_name)
    dependency_file = proxy_directory / "arithmetic-success-proxy.d"
    stdout_path = proxy_directory / "build.stdout.txt"
    stderr_path = proxy_directory / "build.stderr.txt"
    write_text(transformed_source, transformed_text)

    provenance = target["provenance"]
    command = fixed_native_rebuild_command(
        provenance["build"]["native"]["argv"],
        generated_source,
        driver_executable,
        executable,
        dependency_file,
        cpp_standard,
        gti_optimization,
        native_optimization,
        workload.descriptor_path.parent,
        replacement_source=transformed_source,
    )
    completed, duration = invoke(command, workload.descriptor_path.parent, timeout)
    write_text(stdout_path, completed.stdout)
    write_text(stderr_path, completed.stderr)
    if completed.returncode != 0:
        raise BenchmarkError(
            f"benchmark '{workload.name}' variant '{proxy_name}' build failed with "
            f"exit {completed.returncode}; see {stderr_path}"
        )
    if not executable.is_file():
        raise BenchmarkError(
            f"benchmark '{workload.name}' variant '{proxy_name}' produced no executable"
        )
    if not dependency_file.is_file():
        raise BenchmarkError(
            f"benchmark '{workload.name}' variant '{proxy_name}' produced no dependency file"
        )

    transformed_record = file_evidence(transformed_source)
    replacement_count = sum(replacement["count"] for replacement in replacements)
    result: dict[str, Any] = {
        "name": proxy_name,
        "kind": ARITHMETIC_SUCCESS_PROXY_KIND,
        "source": transformed_record,
        "command": command,
        "duration_ns": duration,
        "exit_code": completed.returncode,
        "artifacts": {
            "source_emitted_cpp": generated_record,
            "transformed_cpp": transformed_record,
            "build_stdout": file_evidence(stdout_path),
            "build_stderr": file_evidence(stderr_path),
            "dependency_file": file_evidence(dependency_file),
            "executable": file_evidence(executable),
        },
        "executable": str(executable),
        "semantic": False,
        "successful_path_only": True,
        "exact_head_proof_eligible": False,
        "proxy": {
            "family": "compiler-owned-checked-arithmetic-success",
            "derived_from_variant": target["name"],
            "source_sha256_before": generated_record["sha256"],
            "source_sha256_after": transformed_record["sha256"],
            "replacement_family": [
                qualified_name
                for qualified_name, _ in ARITHMETIC_SUCCESS_PROXY_HELPERS
            ],
            "replacement_count": replacement_count,
            "replacements": replacements,
        },
    }
    if "execution_profile" in target:
        result["execution_profile"] = target["execution_profile"]
    return result


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
    native_optimization: str | None = None,
    arithmetic_success_proxy_variants: tuple[str, ...] = (),
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
            native_optimization,
        )
        for variant in workload.variants
    ]
    if arithmetic_success_proxy_variants:
        if native_optimization is None:
            raise BenchmarkError(
                "arithmetic success proxies require fixed native optimization"
            )
        builds_by_name = {build["name"]: build for build in builds}
        for target_name in arithmetic_success_proxy_variants:
            target = builds_by_name.get(target_name)
            if target is None:
                raise BenchmarkError(
                    f"benchmark '{workload.name}' has no built proxy target "
                    f"'{target_name}'"
                )
            builds.append(
                build_arithmetic_success_proxy(
                    workload,
                    target,
                    directory,
                    cpp_standard,
                    optimization,
                    native_optimization,
                    timeout,
                )
            )
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


def exact_head_evidence(
    before: dict[str, Any],
    after: dict[str, Any],
    gti: Tool,
    workloads: list[dict[str, Any]],
) -> dict[str, Any]:
    reasons: list[str] = []
    benchmarked_head = before["commit"]
    recorded_head = after["commit"]
    if not isinstance(benchmarked_head, str) or GIT_COMMIT_PATTERN.fullmatch(benchmarked_head) is None:
        reasons.append("benchmarked_head_unavailable")
    if not isinstance(recorded_head, str) or GIT_COMMIT_PATTERN.fullmatch(recorded_head) is None:
        reasons.append("recorded_head_unavailable")
    if before["dirty"] is not False:
        reasons.append("repository_not_clean_before_benchmark")
    if after["dirty"] is not False:
        reasons.append("repository_not_clean_after_benchmark")
    if benchmarked_head != recorded_head:
        reasons.append("recorded_head_differs_from_benchmarked_head")

    compiler_builds: list[dict[str, Any]] = []
    producer_sha256s: list[str] = []
    for workload in workloads:
        for variant in workload["variants"]:
            if variant.get("exact_head_proof_eligible") is False:
                continue
            provenance = variant.get("provenance")
            if variant["kind"] != "gti":
                continue
            if provenance is None:
                reasons.append("gti_build_provenance_unavailable")
                continue
            build = provenance["build"]
            compiler_builds.append(build["producer"]["compiler_build"])
            producer_sha256s.append(build["producer"]["executable"]["sha256"])
    if not compiler_builds or any(
        identity["state"] == "unavailable"
        or not isinstance(identity["commit"], str)
        or GIT_COMMIT_PATTERN.fullmatch(identity["commit"]) is None
        for identity in compiler_builds
    ):
        reasons.append("compiler_build_identity_unavailable")
    if any(identity["state"] == "dirty" for identity in compiler_builds):
        reasons.append("compiler_build_identity_dirty")
    compiler_commits = sorted(
        {identity["commit"] for identity in compiler_builds if identity["commit"] is not None}
    )
    if len(compiler_commits) > 1:
        reasons.append("compiler_build_identity_inconsistent")
    if compiler_commits and compiler_commits[0] != benchmarked_head:
        reasons.append("compiler_commit_differs_from_benchmarked_head")
    selected_compiler_sha256 = file_evidence(gti.resolved)["sha256"]
    if not producer_sha256s or any(
        sha256 != selected_compiler_sha256 for sha256 in producer_sha256s
    ):
        reasons.append("sidecar_producer_differs_from_selected_compiler")
    reasons = sorted(set(reasons))
    return {
        "exact": not reasons,
        "benchmarked_head": benchmarked_head,
        "recorded_head": recorded_head,
        "repository_dirty_before": before["dirty"],
        "repository_dirty_after": after["dirty"],
        "compiler_build_states": sorted(
            {identity["state"] for identity in compiler_builds}
        ),
        "compiler_source_commits": compiler_commits,
        "selected_compiler_sha256": selected_compiler_sha256,
        "sidecar_producer_sha256s": sorted(set(producer_sha256s)),
        "reasons": reasons,
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
    configuration = report["configuration"]
    optimization_summary = f"-O{configuration['optimization']}"
    if "native_optimization" in configuration:
        optimization_summary = (
            f"GTI -O{configuration['optimization']}, "
            f"native -O{configuration['native_optimization']}"
        )
    lines = [
        "# GTI benchmark report",
        "",
        f"Generated: `{report['generated_at_utc']}`",
        "",
        f"Native compiler: `{report['tools']['cxx']['resolved']}`",
        "",
        f"Configuration: `{optimization_summary} "
        f"-std={configuration['cpp_standard']}`",
        "",
    ]
    for workload in report["workloads"]:
        proxy_names = [
            variant["name"]
            for variant in workload["variants"]
            if variant["kind"] == ARITHMETIC_SUCCESS_PROXY_KIND
        ]
        lines.extend(
            [
                f"## {workload['name']}",
                "",
                f"Correctness: `{workload['correctness_record']}`",
                "",
                f"Measurement quality: **{workload['quality']}**",
                "",
            ]
        )
        if proxy_names:
            lines.extend(
                [
                    "Successful-path-only, non-semantic arithmetic proxies "
                    "(excluded from exact-head proof inputs): "
                    + ", ".join(f"`{name}`" for name in proxy_names),
                    "",
                ]
            )
        lines.extend(
            [
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
        "--optimization", choices=OPTIMIZATION_LEVELS, default="3"
    )
    parser.add_argument(
        "--native-optimization",
        choices=OPTIMIZATION_LEVELS,
        help=(
            "hold native optimization at this level while --optimization "
            "selects GTI optimization"
        ),
    )
    parser.add_argument(
        "--arithmetic-success-proxy",
        action="append",
        default=[],
        metavar="WORKLOAD/VARIANT",
        help=(
            "derive a successful-path-only arithmetic proxy from one selected "
            "GTI variant; requires --native-optimization"
        ),
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
        proxy_targets = arithmetic_success_proxy_targets(
            list(arguments.arithmetic_success_proxy),
            workloads,
            arguments.native_optimization,
        )
        has_schema_2 = any(workload.descriptor["schema"] == 2 for workload in workloads)
        repository_before = repository_facts()
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
                    arguments.native_optimization,
                    proxy_targets.get(workload.name, ()),
                )
            )

        repository_after = repository_facts()
        report_schema = (
            4
            if proxy_targets
            else (
                3
                if arguments.native_optimization is not None
                else (2 if has_schema_2 else 1)
            )
        )
        report = {
            "schema": report_schema,
            "generated_at_utc": datetime.now(timezone.utc).isoformat(),
            "runner": {
                "path": str(Path(__file__).resolve()),
                "sha256": file_evidence(Path(__file__).resolve())["sha256"],
            },
            "repository": repository_after,
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
        if arguments.native_optimization is not None:
            report["configuration"]["native_optimization"] = (
                arguments.native_optimization
            )
        if proxy_targets:
            report["configuration"]["arithmetic_success_proxies"] = list(
                arguments.arithmetic_success_proxy
            )
        if report_schema >= 2:
            report["exact_head_evidence"] = exact_head_evidence(
                repository_before, repository_after, gti, reports
            )
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
