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
import sys
import tempfile

from backend_execution import Endpoint, execute
from example_case_context import LLVM_ONLY_EXAMPLES, require_cpp_limitation
from mir_census_baseline import expected_for_target, host_target


CONFIGURATIONS = (("O0", "c++20"), ("O3", "c++23"))
MARKER_PATTERN = re.compile(
    r"GTI verified-MIR body: ([a-z0-9-]+) "
    r"(module-instance|field-initializers-instance|"
    r"static-field-initializers-instance|function-instance|"
    r"constructor-instance|destructor-instance|lambda-instance|"
    r"hosted-startup-instance|thread-local-initializer) (\d+)"
)
ALLOWED_MARKERS = {
    "deduced-callable-v1",
    "deduced-callable-failure-v1",
    "generic-owner-constructor-failure-v1",
    "program-initialization-v1",
    "scalar-cfg-v1",
    "scalar-cfg-failure-v1",
    "scalar-cfg-constructor-failure-v1",
    # Constructor base preparation/gating are verified projections of the
    # same constructor body, not separate AST/HIR executable routes.
    "scalar-cfg-base-gate-v1",
    "scalar-cfg-base-preparation-v1",
    "scalar-cfg-destructor-failure-v1",
    "native-boundary-v1",
    "hosted-entry-v1",
    # Each lazy TLS initializer is a separately verified concrete MIR body.
    "thread-local-initializer-v1",
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


def build_and_run(gti: Path, root: Path, scratch: Path, source: Path,
                  optimization: str, standard: str,
                  env: dict[str, str], backend: str = "cpp") -> CaseResult:
    case = scratch / f"{optimization}-{standard.replace('+', 'x')}" / source.stem
    case.mkdir(parents=True, exist_ok=True)
    if backend == "cpp" and source.name in LLVM_ONLY_EXAMPLES:
        require_cpp_limitation(gti, source, case / "unsupported", env, optimization, standard)
        backend = "llvm"
    endpoint = Endpoint(backend, optimization,
                        standard if backend == "cpp" else None,
                        keep_cpp=backend == "cpp")
    executed = execute(
        gti, root, case, source, endpoint, env,
        concurrent=source.name in {"066-managed-threads.gti", "076-scalar-atomics.gti",
                                  "175-thread-local-storage.gti"},
    )
    if executed.error:
        return CaseResult(source.name, optimization, standard, frozenset(),
                          frozenset(), executed.returncode, executed.stdout,
                          executed.stderr, executed.error)

    text = executed.generated_cpp or ""
    rows = frozenset(
        (family, kind, int(owner))
        for family, kind, owner in MARKER_PATTERN.findall(text)
    )
    markers = frozenset((kind, owner) for _, kind, owner in rows)

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
    parser.add_argument("--backend", choices=("cpp", "llvm"), default="cpp")
    parser.add_argument("--case", action="append", dest="selected_cases",
                        help="run named examples after validating the complete census inventory")
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

    try:
        target = host_target()
        expected, _ = expected_for_target(
            json.loads(baseline_path.read_text(encoding="utf-8")), target)
    except (KeyError, TypeError, ValueError) as error:
        print(f"FAIL: invalid MIR census baseline: {error}", file=sys.stderr)
        return 1
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
    if arguments.selected_cases:
        unknown = set(arguments.selected_cases) - source_names
        if unknown:
            parser.error(f"unknown corpus examples: {sorted(unknown)}")
        sources = [source for source in sources if source.name in arguments.selected_cases]
    configurations = CONFIGURATIONS if arguments.backend == "cpp" else (("O0", "native"), ("O3", "native"))

    env = dict(os.environ)
    env["GTI_STDLIB_PATH"] = str(root / "stdlib")
    env["GTI_RUNTIME_INCLUDE"] = str(root / "runtime" / "include")
    env["GTI_RUNTIME_LIBRARY"] = str(runtime_library)

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
                    arguments.backend,
                ): (source.name, optimization, standard)
                for source in sources
                for optimization, standard in configurations
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
            for optimization, standard in configurations
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
            if arguments.backend == "cpp" and source.name not in LLVM_ONLY_EXAMPLES and len(case.markers) != expected[source.name]:
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
        if arguments.backend == "cpp" and source.name not in LLVM_ONLY_EXAMPLES and first.markers != second.markers:
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

    reviewed_count = sum(expected[source.name] for source in sources)
    print(
        f"MIR corpus oracle ({arguments.backend}, {target}): {len(sources)} "
        f"examples, {reviewed_count} reviewed body identities, "
        f"{len(configurations)} endpoint configurations"
    )
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print("ok: all selected examples build and run with matching observable behavior")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
