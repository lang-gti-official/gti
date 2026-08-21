#!/usr/bin/env python3

"""Post-cutover native oracle for verified-MIR C++ emission.

The former differential oracle compared the production backend with the
retired AST/HIR executable emitter. Keeping that emitter callable would defeat
the hard cutover, so this oracle checks the contracts that remain meaningful:

* every shipped example passes the sealed whole-program MIR plan;
* every reviewed body identity is present in generated C++;
* ownership is unchanged between O0/C++20 and O3/C++23; and
* both native programs succeed with identical observable behavior.

The endpoint matrix covers the compiler's unoptimized and optimized MIR forms
and both supported C++ standards without restoring eight duplicate corpus
jobs. Focused runtime tests retain the wider native optimization matrix.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


CONFIGURATIONS = (("O0", "c++20"), ("O3", "c++23"))
MARKER_PATTERN = re.compile(
    r"GTI verified-MIR body: ([a-z0-9-]+) "
    r"(module-instance|field-initializers-instance|"
    r"static-field-initializers-instance|function-instance|"
    r"constructor-instance|destructor-instance|lambda-instance|"
    r"hosted-startup-instance) (\d+)"
)
ALLOWED_MARKERS = {
    "deduced-callable-v1",
    "deduced-callable-failure-v1",
    "generic-owner-constructor-failure-v1",
    "program-initialization-v1",
    "scalar-cfg-v1",
    "scalar-cfg-failure-v1",
    "scalar-cfg-constructor-failure-v1",
    "scalar-cfg-destructor-failure-v1",
    "native-boundary-v1",
    "hosted-entry-v1",
}


@dataclass(frozen=True)
class CaseResult:
    source: str
    optimization: str
    standard: str
    markers: frozenset[tuple[str, int]]
    marker_rows: frozenset[tuple[str, str, int]]
    returncode: int
    stdout: str
    stderr: str
    error: str = ""


def run(command: list[str], *, cwd: Path, env: dict[str, str],
        timeout: int) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        env=env,
        text=True,
        capture_output=True,
        check=False,
        timeout=timeout,
    )


def build_and_run(gti: Path, root: Path, scratch: Path, source: Path,
                  optimization: str, standard: str,
                  env: dict[str, str]) -> CaseResult:
    case = scratch / f"{optimization}-{standard.replace('+', 'x')}" / source.stem
    case.mkdir(parents=True, exist_ok=True)
    executable = case / ("program.exe" if os.name == "nt" else "program")
    generated = Path(f"{executable}.gti.cpp")
    command = [
        str(gti),
        str(source),
        f"-{optimization}",
        "--std",
        standard,
        "--keep-cpp",
        "-o",
        str(executable),
    ]
    try:
        built = run(command, cwd=root, env=env, timeout=240)
    except (OSError, subprocess.TimeoutExpired) as error:
        return CaseResult(source.name, optimization, standard, frozenset(),
                          frozenset(), -1, "", "", f"build failed: {error}")
    if built.returncode != 0 or not executable.is_file() or not generated.is_file():
        detail = built.stderr.strip() or built.stdout.strip() or (
            "compiler reported success without publishing the executable and "
            "retained C++ artifact"
        )
        return CaseResult(source.name, optimization, standard, frozenset(),
                          frozenset(), built.returncode, built.stdout,
                          built.stderr, f"build failed: {detail}")

    text = generated.read_text(encoding="utf-8", errors="replace")
    rows = frozenset(
        (family, kind, int(owner))
        for family, kind, owner in MARKER_PATTERN.findall(text)
    )
    markers = frozenset((kind, owner) for _, kind, owner in rows)

    try:
        executed = run([str(executable)], cwd=root, env=env, timeout=15)
    except (OSError, subprocess.TimeoutExpired) as error:
        return CaseResult(source.name, optimization, standard, markers, rows,
                          -1, "", "", f"execution failed: {error}")
    return CaseResult(
        source.name,
        optimization,
        standard,
        markers,
        rows,
        executed.returncode,
        executed.stdout,
        executed.stderr,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("gti")
    parser.add_argument("root")
    parser.add_argument("baseline")
    parser.add_argument("runtime_library")
    parser.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1))
    arguments = parser.parse_args()

    gti = Path(arguments.gti).resolve()
    root = Path(arguments.root).resolve()
    baseline_path = Path(arguments.baseline).resolve()
    runtime_library = Path(arguments.runtime_library).resolve()
    if arguments.jobs < 1:
        parser.error("--jobs must be at least one")
    if not baseline_path.is_file():
        print(f"FAIL: missing MIR census baseline {baseline_path}", file=sys.stderr)
        return 1

    baseline_document = json.loads(baseline_path.read_text(encoding="utf-8"))
    expected: dict[str, int] = baseline_document["examples"]
    sources = sorted((root / "examples").glob("*.gti"))
    source_names = {source.name for source in sources}
    if source_names != set(expected):
        added = sorted(source_names - set(expected))
        removed = sorted(set(expected) - source_names)
        print(
            "FAIL: corpus and reviewed MIR census baseline differ; "
            f"added={added}, removed={removed}",
            file=sys.stderr,
        )
        return 1
    if sum(expected.values()) != baseline_document.get("total"):
        print("FAIL: MIR census baseline total is internally inconsistent",
              file=sys.stderr)
        return 1

    env = dict(os.environ)
    env["GTI_STDLIB_PATH"] = str(root / "stdlib")
    env["GTI_RUNTIME_INCLUDE"] = str(root / "runtime" / "include")
    env["GTI_RUNTIME_LIBRARY"] = str(runtime_library)
    env["GTI_VENDOR_INCLUDE"] = str(root / "vendor" / "expected_lite" / "include")

    results: dict[tuple[str, str, str], CaseResult] = {}
    with tempfile.TemporaryDirectory(prefix="gti-mir-cutover-oracle-") as raw:
        scratch = Path(raw)
        with ThreadPoolExecutor(max_workers=arguments.jobs) as executor:
            futures = {
                executor.submit(
                    build_and_run,
                    gti,
                    root,
                    scratch,
                    source,
                    optimization,
                    standard,
                    env,
                ): (source.name, optimization, standard)
                for source in sources
                for optimization, standard in CONFIGURATIONS
            }
            for future in as_completed(futures):
                key = futures[future]
                try:
                    results[key] = future.result()
                except Exception as error:  # preserve the failing case identity
                    source, optimization, standard = key
                    results[key] = CaseResult(
                        source, optimization, standard, frozenset(),
                        frozenset(), -1, "", "", f"oracle failed: {error}"
                    )

    failures: list[str] = []
    for source in sources:
        cases = [
            results[(source.name, optimization, standard)]
            for optimization, standard in CONFIGURATIONS
        ]
        for case in cases:
            label = f"{case.source} {case.optimization}/{case.standard}"
            if case.error:
                failures.append(f"{label}: {case.error}")
                continue
            if case.returncode != 0:
                failures.append(
                    f"{label}: exited {case.returncode}; stdout={case.stdout!r}; "
                    f"stderr={case.stderr!r}"
                )
            if len(case.markers) != expected[source.name]:
                failures.append(
                    f"{label}: emitted {len(case.markers)} reviewed body "
                    f"identities, expected {expected[source.name]}"
                )
            unexpected = sorted(
                family for family, _, _ in case.marker_rows
                if family not in ALLOWED_MARKERS
            )
            if unexpected:
                failures.append(
                    f"{label}: emitted retired or unknown MIR marker families "
                    f"{unexpected}"
                )

        if any(case.error for case in cases):
            continue
        first, second = cases
        if first.markers != second.markers:
            failures.append(
                f"{source.name}: body ownership differs between endpoint "
                "configurations"
            )
        if (first.returncode, first.stdout, first.stderr) != (
            second.returncode, second.stdout, second.stderr
        ):
            failures.append(
                f"{source.name}: observable behavior differs between "
                f"{first.optimization}/{first.standard} and "
                f"{second.optimization}/{second.standard}"
            )

    print(
        f"MIR cutover corpus oracle: {len(sources)} examples, "
        f"{baseline_document['total']} reviewed body identities, "
        f"{len(CONFIGURATIONS)} endpoint configurations"
    )
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print("ok: all examples build, run, and retain identical MIR ownership")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
