#!/usr/bin/env python3

"""Capture and compare evidence for the compiler structural refactor.

The structural campaign needs an exact, same-machine before/after record.  This
tool deliberately composes the repository's existing compiler, MIR, optimizer,
and native-runtime authorities instead of defining another language oracle.

Capture is intentionally strict:

* the source tree must be a clean Git worktree;
* Debug and Release are configured into fresh Ninja build directories;
* build concurrency is bounded by ``--jobs`` (two by default);
* every shipped example is built and run at C++20/C++23 and O0/O1/O3; and
* raw command output stays outside the repository while a compact JSON summary
  can be committed beside the refactor plan.

The ``compare`` command checks only behavior/artifact authorities that must be
identical.  Inventory and build timings are measurements, not equivalence
claims, so they are reported separately by the caller.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import resource
import shutil
import subprocess
import sys
import time
from typing import Any, Iterable, Sequence


SCHEMA = "gti-compiler-structural-snapshot-v1"
CONFIGURATIONS = tuple(
    (optimization, standard)
    for standard in ("c++20", "c++23")
    for optimization in ("O0", "O1", "O3")
)
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp"}
INCLUDE_PATTERN = re.compile(r'^\s*#\s*include\s*"([^"]+)"')
TRACE_PHASE_PATTERN = re.compile(
    r"^gti-(source-load|parse|semantics|hir-lowering|failure-metadata|"
    r"mir-lowering|optimization|backend)"
)
PUBLIC_EXAMPLE_EXECUTION_PROFILES = {
    "066-managed-threads.gti": "concurrent",
    "076-scalar-atomics.gti": "concurrent",
    "171-thread-local-storage.gti": "concurrent",
}


class SnapshotError(RuntimeError):
    """A required measurement or equivalence claim could not be produced."""


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    temporary.replace(path)


def safe_slug(value: str) -> str:
    result = re.sub(r"[^A-Za-z0-9_.-]+", "-", value).strip("-")
    return result or "command"


@dataclass(frozen=True)
class CommandResult:
    argv: list[str]
    cwd: str
    returncode: int
    wall_seconds: float
    user_seconds: float
    system_seconds: float
    stdout_sha256: str
    stderr_sha256: str

    def as_json(self) -> dict[str, Any]:
        return {
            "argv": self.argv,
            "cwd": self.cwd,
            "returncode": self.returncode,
            "wall_seconds": round(self.wall_seconds, 6),
            "user_seconds": round(self.user_seconds, 6),
            "system_seconds": round(self.system_seconds, 6),
            "stdout_sha256": self.stdout_sha256,
            "stderr_sha256": self.stderr_sha256,
        }


def run_command(
    argv: Sequence[str],
    *,
    cwd: Path,
    raw_directory: Path,
    name: str,
    env: dict[str, str] | None = None,
    timeout: int = 1800,
    require_success: bool = True,
) -> tuple[CommandResult, str, str]:
    """Run one measured command and retain its complete output externally."""

    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    started = time.monotonic()
    try:
        completed = subprocess.run(
            list(argv),
            cwd=cwd,
            env=env,
            capture_output=True,
            text=True,
            check=False,
            timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise SnapshotError(f"{name} failed to execute: {error}") from error
    elapsed = time.monotonic() - started
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    stdout = completed.stdout
    stderr = completed.stderr
    result = CommandResult(
        argv=list(argv),
        cwd=str(cwd),
        returncode=completed.returncode,
        wall_seconds=elapsed,
        user_seconds=after.ru_utime - before.ru_utime,
        system_seconds=after.ru_stime - before.ru_stime,
        stdout_sha256=sha256_bytes(stdout.encode()),
        stderr_sha256=sha256_bytes(stderr.encode()),
    )
    command_root = raw_directory / "commands" / safe_slug(name)
    command_root.mkdir(parents=True, exist_ok=False)
    (command_root / "stdout.txt").write_text(stdout, encoding="utf-8")
    (command_root / "stderr.txt").write_text(stderr, encoding="utf-8")
    write_json(command_root / "command.json", result.as_json())
    if require_success and completed.returncode != 0:
        detail = stderr.strip() or stdout.strip() or "no command output"
        raise SnapshotError(
            f"{name} exited {completed.returncode}: {detail[:1000]}"
        )
    return result, stdout, stderr


def command_text(argv: Sequence[str], *, cwd: Path) -> str:
    try:
        completed = subprocess.run(
            list(argv), cwd=cwd, capture_output=True, text=True, check=False, timeout=30
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        return f"unavailable: {error}"
    text = (completed.stdout or completed.stderr).strip()
    return text if completed.returncode == 0 else f"unavailable: {text}"


def git_identity(source_root: Path) -> tuple[str, str]:
    status = subprocess.run(
        ["git", "status", "--porcelain"],
        cwd=source_root,
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    if status:
        raise SnapshotError(
            f"source worktree must be clean before capture:\n{status.rstrip()}"
        )
    commit = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=source_root,
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    tree = subprocess.run(
        ["git", "rev-parse", "HEAD^{tree}"],
        cwd=source_root,
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    return commit, tree


def percentile(values: Sequence[int], percentage: float) -> int:
    if not values:
        return 0
    ordered = sorted(values)
    index = max(0, math.ceil(len(ordered) * percentage) - 1)
    return ordered[index]


def ninja_object_edges(build_root: Path) -> dict[str, Any]:
    log = build_root / ".ninja_log"
    if not log.is_file():
        raise SnapshotError(f"missing Ninja log after build: {log}")
    rows: list[tuple[int, str]] = []
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) < 4:
            continue
        output = fields[3]
        if not output.endswith((".o", ".obj")):
            continue
        try:
            duration = max(0, int(fields[1]) - int(fields[0]))
        except ValueError:
            continue
        rows.append((duration, output))
    durations = [duration for duration, _ in rows]
    longest = sorted(rows, reverse=True)[:25]
    return {
        "count": len(rows),
        "p50_milliseconds": percentile(durations, 0.50),
        "p95_milliseconds": percentile(durations, 0.95),
        "maximum_milliseconds": max(durations, default=0),
        "longest": [
            {"milliseconds": duration, "output": output}
            for duration, output in longest
        ],
    }


def binary_sizes(build_root: Path) -> dict[str, int]:
    requested = (
        "gti",
        "gti_lsp",
        "libgti_compiler.a",
        "libgti_cpp_backend.a",
        "libgti_runtime.a",
    )
    result: dict[str, int] = {}
    for name in requested:
        candidates = sorted(build_root.rglob(name))
        regular = [candidate for candidate in candidates if candidate.is_file()]
        if regular:
            result[name] = regular[0].stat().st_size
    return result


def configure_and_build(
    source_root: Path,
    raw_directory: Path,
    configuration: str,
    jobs: int,
    cxx: str | None,
) -> dict[str, Any]:
    build_root = raw_directory / f"build-{configuration.lower()}"
    if build_root.exists():
        raise SnapshotError(
            f"fresh build directory already exists; choose a new evidence root: {build_root}"
        )
    configure = [
        "cmake",
        "-S",
        str(source_root),
        "-B",
        str(build_root),
        "-G",
        "Ninja",
        f"-DCMAKE_BUILD_TYPE={configuration}",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
    ]
    if cxx:
        configure.append(f"-DCMAKE_CXX_COMPILER={cxx}")
    configured, _, _ = run_command(
        configure,
        cwd=source_root,
        raw_directory=raw_directory,
        name=f"configure-{configuration.lower()}",
    )
    built, _, _ = run_command(
        ["cmake", "--build", str(build_root), "--parallel", str(jobs)],
        cwd=source_root,
        raw_directory=raw_directory,
        name=f"build-{configuration.lower()}",
        timeout=7200,
    )
    no_op, _, _ = run_command(
        ["cmake", "--build", str(build_root), "--parallel", str(jobs)],
        cwd=source_root,
        raw_directory=raw_directory,
        name=f"build-{configuration.lower()}-noop",
        timeout=1800,
    )
    return {
        "build_root": str(build_root),
        "configure": configured.as_json(),
        "build": built.as_json(),
        "no_op": no_op.as_json(),
        "object_edges": ninja_object_edges(build_root),
        "binary_sizes": binary_sizes(build_root),
    }


def tracked_source_files(source_root: Path) -> list[Path]:
    completed = subprocess.run(
        ["git", "ls-files", "-z"],
        cwd=source_root,
        capture_output=True,
        check=True,
    )
    result: list[Path] = []
    for raw in completed.stdout.split(b"\0"):
        if not raw:
            continue
        path = source_root / raw.decode("utf-8", errors="surrogateescape")
        relative = path.relative_to(source_root)
        if path.suffix in SOURCE_SUFFIXES and (
            relative.parts[:2] == ("include", "gti")
            or relative.parts[:2] == ("src", "compiler")
        ):
            result.append(path)
    return sorted(result)


def resolve_include(source_root: Path, including: Path, spelling: str) -> Path | None:
    candidates = []
    if spelling.startswith("gti/"):
        candidates.append(source_root / "include" / spelling)
    candidates.append(including.parent / spelling)
    candidates.append(source_root / "src" / "compiler" / spelling)
    for candidate in candidates:
        try:
            resolved = candidate.resolve(strict=True)
        except OSError:
            continue
        try:
            resolved.relative_to(source_root)
        except ValueError:
            continue
        return resolved
    return None


def source_inventory(source_root: Path) -> dict[str, Any]:
    files = tracked_source_files(source_root)
    rows: list[dict[str, Any]] = []
    reverse_fanout: dict[str, set[str]] = {}
    direct_edges = 0
    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        relative = path.relative_to(source_root).as_posix()
        includes: list[str] = []
        for line in text.splitlines():
            match = INCLUDE_PATTERN.match(line)
            if match is None:
                continue
            resolved = resolve_include(source_root, path, match.group(1))
            if resolved is None:
                continue
            target = resolved.relative_to(source_root).as_posix()
            includes.append(target)
            reverse_fanout.setdefault(target, set()).add(relative)
            direct_edges += 1
        rows.append(
            {
                "path": relative,
                "bytes": path.stat().st_size,
                "lines": len(text.splitlines()),
                "direct_repository_includes": sorted(set(includes)),
            }
        )
    fanout = sorted(
        (
            {"path": path, "direct_reverse_fanout": len(consumers)}
            for path, consumers in reverse_fanout.items()
        ),
        key=lambda row: (-row["direct_reverse_fanout"], row["path"]),
    )
    return {
        "file_count": len(rows),
        "line_count": sum(row["lines"] for row in rows),
        "byte_count": sum(row["bytes"] for row in rows),
        "direct_include_edges": direct_edges,
        "files": rows,
        "reverse_fanout": fanout,
    }


def locate_runtime_library(build_root: Path) -> Path:
    candidates = sorted(build_root.rglob("libgti_runtime.a"))
    if not candidates:
        raise SnapshotError(f"Release build contains no libgti_runtime.a: {build_root}")
    return candidates[0]


def compiler_environment(source_root: Path, build_root: Path) -> dict[str, str]:
    environment = dict(os.environ)
    environment["GTI_STDLIB_PATH"] = str(source_root / "stdlib")
    environment["GTI_RUNTIME_INCLUDE"] = str(source_root / "runtime" / "include")
    environment["GTI_RUNTIME_LIBRARY"] = str(locate_runtime_library(build_root))
    # Toolchains before 0.432.0 require expected-lite for C++20 builds.
    legacy_vendor_include = source_root / "vendor" / "expected_lite" / "include"
    if legacy_vendor_include.is_dir():
        environment["GTI_VENDOR_INCLUDE"] = str(legacy_vendor_include)
    return environment


def trace_phase_durations(document: Any) -> dict[str, int]:
    events = document.get("traceEvents", []) if isinstance(document, dict) else []
    durations: dict[str, int] = {}
    for event in events:
        if not isinstance(event, dict) or event.get("ph") != "X":
            continue
        name = str(event.get("name", ""))
        if not TRACE_PHASE_PATTERN.match(name):
            continue
        durations[name] = durations.get(name, 0) + int(event.get("dur", 0))
    return dict(sorted(durations.items()))


def capture_trace(
    source_root: Path, raw_directory: Path, release_build: Path
) -> dict[str, Any]:
    gti = release_build / "gti"
    source = sorted((source_root / "examples").glob("*.gti"))[0]
    trace_root = raw_directory / "frontend-trace"
    trace_root.mkdir(parents=True, exist_ok=False)
    trace = trace_root / "trace.json"
    output = trace_root / "output.cpp"
    command, _, _ = run_command(
        [
            str(gti),
            str(source),
            "-O0",
            "--std",
            "c++23",
            "--emit-cpp",
            "--time-trace",
            str(trace),
            "-o",
            str(output),
        ],
        cwd=source_root,
        env=compiler_environment(source_root, release_build),
        raw_directory=raw_directory,
        name="frontend-time-trace",
        timeout=600,
    )
    if not trace.is_file():
        raise SnapshotError("frontend trace command succeeded without a trace artifact")
    document = json.loads(trace.read_text(encoding="utf-8"))
    return {
        "source": source.name,
        "command": command.as_json(),
        "trace_sha256": sha256_file(trace),
        "phase_microseconds": trace_phase_durations(document),
    }


def capture_diagnostics(
    source_root: Path, raw_directory: Path, release_build: Path
) -> dict[str, Any]:
    manifest_path = source_root / "tests" / "local-language-audit" / "cases.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    gti = release_build / "gti"
    environment = compiler_environment(source_root, release_build)
    result: dict[str, Any] = {}
    for case in manifest["cases"]:
        if case.get("kind") != "diagnostic":
            continue
        identifier = str(case["id"])
        source = manifest_path.parent / case["source"]
        output = raw_directory / "diagnostics" / f"{identifier}.cpp"
        output.parent.mkdir(parents=True, exist_ok=True)
        command, stdout, stderr = run_command(
            [
                str(gti),
                str(source),
                "-O0",
                "--std",
                "c++23",
                "--emit-cpp",
                "-o",
                str(output),
            ],
            cwd=source_root,
            env=environment,
            raw_directory=raw_directory,
            name=f"diagnostic-{identifier}",
            timeout=300,
            require_success=False,
        )
        if command.returncode == 0:
            raise SnapshotError(f"diagnostic audit case unexpectedly compiled: {identifier}")
        normalized = (stdout + stderr).replace(str(source_root), "<SOURCE_ROOT>")
        normalized = normalized.replace(str(raw_directory), "<EVIDENCE_ROOT>")
        result[identifier] = {
            "returncode": command.returncode,
            "normalized_sha256": sha256_bytes(normalized.encode()),
            "required_codes": sorted(case.get("codes", [])),
        }
    return result


def artifact_digest(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise SnapshotError(f"missing expected artifact: {path}")
    return {"bytes": path.stat().st_size, "sha256": sha256_file(path)}


def capture_corpus_case(
    *,
    source_root: Path,
    raw_directory: Path,
    release_build: Path,
    source: Path,
    optimization: str,
    standard: str,
) -> tuple[str, str, str, dict[str, Any]]:
    gti = release_build / "gti"
    variant = raw_directory / "corpus" / standard / optimization / source.stem
    variant.mkdir(parents=True, exist_ok=False)
    executable = variant / "program"
    generated = Path(f"{executable}.gti.cpp")
    report = variant / "optimization-report.json"
    mir_before = variant / "mir-before.txt"
    mir_after = variant / "mir-after.txt"
    arguments = [str(gti), str(source)]
    profile = PUBLIC_EXAMPLE_EXECUTION_PROFILES.get(source.name)
    if profile:
        arguments.extend(["--execution-profile", profile])
    arguments.extend(
        [
            f"-{optimization}",
            "--std",
            standard,
            "--keep-cpp",
            "--optimization-report",
            str(report),
            "--dump-mir-before",
            str(mir_before),
            "--dump-mir-after",
            str(mir_after),
            "-o",
            str(executable),
        ]
    )
    name = f"corpus-{source.stem}-{standard}-{optimization}"
    built, _, _ = run_command(
        arguments,
        cwd=source_root,
        env=compiler_environment(source_root, release_build),
        raw_directory=raw_directory,
        name=name,
        timeout=900,
    )
    run_result, stdout, stderr = run_command(
        [str(executable)],
        cwd=source_root,
        raw_directory=raw_directory,
        name=f"{name}-run",
        timeout=30,
        require_success=False,
    )
    if run_result.returncode < 0:
        raise SnapshotError(f"{name} terminated by signal {-run_result.returncode}")
    observation = {
        "returncode": run_result.returncode,
        "stdout_sha256": sha256_bytes(stdout.encode()),
        "stderr_sha256": sha256_bytes(stderr.encode()),
    }
    return (
        source.name,
        standard,
        optimization,
        {
            "build": built.as_json(),
            "generated_cpp": artifact_digest(generated),
            "mir_before": artifact_digest(mir_before),
            "mir_after": artifact_digest(mir_after),
            "optimization_report": artifact_digest(report),
            "observation": observation,
        },
    )


def capture_corpus(
    source_root: Path,
    raw_directory: Path,
    release_build: Path,
    jobs: int,
) -> dict[str, Any]:
    sources = sorted((source_root / "examples").glob("*.gti"))
    if not sources:
        raise SnapshotError("no examples/*.gti sources found")
    result: dict[str, dict[str, dict[str, Any]]] = {
        source.name: {} for source in sources
    }
    futures = {}
    with ThreadPoolExecutor(max_workers=jobs) as executor:
        for source in sources:
            for optimization, standard in CONFIGURATIONS:
                future = executor.submit(
                    capture_corpus_case,
                    source_root=source_root,
                    raw_directory=raw_directory,
                    release_build=release_build,
                    source=source,
                    optimization=optimization,
                    standard=standard,
                )
                futures[future] = (source.name, standard, optimization)
        for future in as_completed(futures):
            source_name, standard, optimization = futures[future]
            try:
                _, _, _, row = future.result()
            except Exception as error:
                raise SnapshotError(
                    f"corpus capture failed for {source_name} {standard}/{optimization}: {error}"
                ) from error
            result[source_name][f"{standard}/{optimization}"] = row

    for source_name, variants in result.items():
        if len(variants) != len(CONFIGURATIONS):
            raise SnapshotError(f"incomplete corpus matrix for {source_name}")
        observations = {
            json.dumps(row["observation"], sort_keys=True) for row in variants.values()
        }
        if len(observations) != 1:
            raise SnapshotError(
                f"observable behavior differs across the six configurations: {source_name}"
            )
    return result


def capture_census_authorities(
    source_root: Path, raw_directory: Path, release_build: Path
) -> dict[str, Any]:
    baseline_paths = (
        source_root / "tests" / "mir_census_baseline.json",
        source_root / "tests" / "lowered_program_census_baseline.json",
    )
    result: dict[str, Any] = {
        path.name: artifact_digest(path) for path in baseline_paths
    }
    lowered_tool = release_build / "gti_lowered_program_census"
    if not lowered_tool.is_file():
        raise SnapshotError(f"missing LoweredProgram census tool: {lowered_tool}")
    names = sorted(path.name for path in (source_root / "examples").glob("*.gti"))
    command, stdout, _ = run_command(
        [str(lowered_tool), str(source_root), *names],
        cwd=source_root,
        raw_directory=raw_directory,
        name="lowered-program-census",
        timeout=1800,
    )
    result["lowered_program_inventory"] = {
        "command": command.as_json(),
        "sha256": sha256_bytes(stdout.encode()),
        "rows": len(stdout.splitlines()),
    }
    return result


def capture_ctest_inventory(
    source_root: Path, raw_directory: Path, release_build: Path
) -> dict[str, Any]:
    command, stdout, _ = run_command(
        ["ctest", "--test-dir", str(release_build), "-N"],
        cwd=source_root,
        raw_directory=raw_directory,
        name="ctest-inventory",
    )
    names = []
    for line in stdout.splitlines():
        match = re.match(r"\s*Test\s+#?\d+:\s+(.+)$", line)
        if match:
            names.append(match.group(1).strip())
    return {"command": command.as_json(), "count": len(names), "names": names}


def capture(arguments: argparse.Namespace) -> int:
    source_root = arguments.source_root.resolve()
    evidence_root = arguments.evidence_root.resolve()
    if arguments.jobs < 1 or arguments.jobs > 2:
        raise SnapshotError("--jobs must be one or two for the structural campaign")
    if evidence_root.exists():
        raise SnapshotError(f"evidence root already exists: {evidence_root}")
    commit, tree = git_identity(source_root)
    evidence_root.mkdir(parents=True)
    raw_directory = evidence_root / "raw"
    raw_directory.mkdir()
    identity = {
        "commit": commit,
        "tree": tree,
        "source_root": str(source_root),
        "evidence_root": str(evidence_root),
        "jobs": arguments.jobs,
        "platform": platform.platform(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "python": sys.version,
        "cmake": command_text(["cmake", "--version"], cwd=source_root),
        "ninja": command_text(["ninja", "--version"], cwd=source_root),
        "cxx": command_text([arguments.cxx or "c++", "--version"], cwd=source_root),
    }
    write_json(raw_directory / "identity.json", identity)
    summary: dict[str, Any] = {
        "schema": SCHEMA,
        "identity": identity,
        "inventory": source_inventory(source_root),
    }
    write_json(evidence_root / "snapshot.partial.json", summary)
    builds = {
        configuration: configure_and_build(
            source_root,
            raw_directory,
            configuration,
            arguments.jobs,
            arguments.cxx,
        )
        for configuration in ("Debug", "Release")
    }
    summary["builds"] = builds
    release_build = Path(builds["Release"]["build_root"])
    summary["ctest_inventory"] = capture_ctest_inventory(
        source_root, raw_directory, release_build
    )
    summary["frontend_trace"] = capture_trace(
        source_root, raw_directory, release_build
    )
    summary["diagnostics"] = capture_diagnostics(
        source_root, raw_directory, release_build
    )
    summary["census_authorities"] = capture_census_authorities(
        source_root, raw_directory, release_build
    )
    write_json(evidence_root / "snapshot.partial.json", summary)
    summary["corpus"] = capture_corpus(
        source_root, raw_directory, release_build, arguments.jobs
    )
    write_json(evidence_root / "snapshot.json", summary)
    (evidence_root / "snapshot.partial.json").unlink()
    print(
        f"captured {commit[:12]}: {len(summary['corpus'])} examples x "
        f"{len(CONFIGURATIONS)} configurations in {evidence_root}"
    )
    return 0


def nested_differences(left: Any, right: Any, path: str = "") -> list[str]:
    if type(left) is not type(right):
        return [f"{path or '<root>'}: type {type(left).__name__} != {type(right).__name__}"]
    if isinstance(left, dict):
        differences: list[str] = []
        for key in sorted(set(left) | set(right)):
            child = f"{path}.{key}" if path else str(key)
            if key not in left:
                differences.append(f"{child}: missing from baseline")
            elif key not in right:
                differences.append(f"{child}: missing from candidate")
            else:
                differences.extend(nested_differences(left[key], right[key], child))
        return differences
    if isinstance(left, list):
        if len(left) != len(right):
            return [f"{path}: length {len(left)} != {len(right)}"]
        differences = []
        for index, (before, after) in enumerate(zip(left, right, strict=True)):
            differences.extend(nested_differences(before, after, f"{path}[{index}]"))
        return differences
    return [] if left == right else [f"{path}: {left!r} != {right!r}"]


def equivalence_view(snapshot: dict[str, Any]) -> dict[str, Any]:
    corpus: dict[str, Any] = {}
    for source, variants in snapshot["corpus"].items():
        corpus[source] = {}
        for variant, row in variants.items():
            corpus[source][variant] = {
                "generated_cpp": row["generated_cpp"],
                "mir_before": row["mir_before"],
                "mir_after": row["mir_after"],
                "optimization_report": row["optimization_report"],
                "observation": row["observation"],
            }
    return {
        "diagnostics": snapshot["diagnostics"],
        "census_authorities": {
            key: value
            for key, value in snapshot["census_authorities"].items()
            if key != "lowered_program_inventory"
        }
        | {
            "lowered_program_inventory": {
                "sha256": snapshot["census_authorities"]
                ["lowered_program_inventory"]["sha256"],
                "rows": snapshot["census_authorities"]
                ["lowered_program_inventory"]["rows"],
            }
        },
        "corpus": corpus,
    }


def compare(arguments: argparse.Namespace) -> int:
    baseline = json.loads(arguments.baseline.read_text(encoding="utf-8"))
    candidate = json.loads(arguments.candidate.read_text(encoding="utf-8"))
    if baseline.get("schema") != SCHEMA or candidate.get("schema") != SCHEMA:
        raise SnapshotError("snapshot schema mismatch")
    differences = nested_differences(
        equivalence_view(baseline), equivalence_view(candidate)
    )
    if differences:
        print(f"FAIL: {len(differences)} structural equivalence difference(s)")
        for difference in differences[:200]:
            print(f"  {difference}")
        if len(differences) > 200:
            print(f"  ... {len(differences) - 200} more")
        return 1
    print("ok: diagnostics, censuses, MIR, optimizer reports, generated C++, and native behavior are identical")
    return 0


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    capture_parser = subparsers.add_parser("capture")
    capture_parser.add_argument("--source-root", type=Path, required=True)
    capture_parser.add_argument("--evidence-root", type=Path, required=True)
    capture_parser.add_argument("--jobs", type=int, default=2)
    capture_parser.add_argument("--cxx")
    capture_parser.set_defaults(run=capture)
    compare_parser = subparsers.add_parser("compare")
    compare_parser.add_argument("baseline", type=Path)
    compare_parser.add_argument("candidate", type=Path)
    compare_parser.set_defaults(run=compare)
    return parser


def main() -> int:
    arguments = make_parser().parse_args()
    try:
        return int(arguments.run(arguments))
    except SnapshotError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
