#!/usr/bin/env python3
"""Verify std::json MIR, native DOM/codecs, and temporary-input ownership."""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
import tempfile

from case_jobs import configured_jobs, positive_jobs, run_cases


def run(command: list[str], *, timeout: int = 300) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        text=True,
        capture_output=True,
        timeout=timeout,
        check=False,
    )


def require_success(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode != 0:
        raise AssertionError(
            f"{label} failed with {result.returncode}\n"
            f"stdout:\n{result.stdout}\n"
            f"stderr:\n{result.stderr}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gti")
    parser.add_argument("fixtures")
    parser.add_argument("--jobs", type=positive_jobs, default=configured_jobs(),
                        help="bounded concurrent compile-and-run workers")
    arguments = parser.parse_args()
    gti = str(pathlib.Path(arguments.gti).resolve())
    fixtures = pathlib.Path(arguments.fixtures).resolve()
    mir_cases = ("stdlib_surface_json.gti", "stdlib_json_codec.gti")
    with tempfile.TemporaryDirectory(prefix="gti-json-stdlib-") as temporary:
        output = pathlib.Path(temporary)
        # Each MIR emission and each native build-and-run is an independent
        # bounded-worker case; results are checked in the original order with
        # the same expectations and timeouts as the serial form.
        plan = []
        for optimization in ("-O0", "-O3"):
            for source_name in mir_cases:
                plan.append((f"mir:{source_name}:{optimization}",
                             ("mir", source_name, None, optimization)))
        for standard, optimization in (("c++23", "-O0"), ("c++20", "-O3")):
            for source_name in (*mir_cases, "json_temporary_owner.gti"):
                plan.append((f"native:{source_name}:{standard}:{optimization}",
                             ("native", source_name, standard, optimization)))
        plan.append(("regression:json_expected_value_backend_bug.gti",
                     ("regression", "json_expected_value_backend_bug.gti", None, "-O0")))

        # Workers only compile; the native runs and every check happen on this
        # thread in plan order, as the shared matrix clients do.
        def work(case):
            kind, source_name, standard, optimization = case
            source = fixtures / source_name
            if kind == "mir":
                artifact = output / f"{source.stem}-{optimization[1:]}.mir"
                compiled = run(
                    [gti, str(source), "--emit-mir", optimization, "-o", str(artifact)]
                )
            elif kind == "native":
                artifact = output / f"{source.stem}-{standard}-{optimization[1:]}"
                compiled = run(
                    [gti, str(source), "--std", standard, optimization,
                     "-o", str(artifact)], timeout=600
                )
            else:
                artifact = output / "json-expected-value-backend-bug"
                compiled = run([gti, str(source), "-O0", "-o", str(artifact)], timeout=60)
            return (case, compiled, artifact)

        def observe(value):
            (kind, source_name, standard, optimization), compiled, artifact = value
            if kind == "mir":
                require_success(compiled, f"emit MIR {source_name} {optimization}")
                if not artifact.is_file():
                    raise AssertionError(
                        f"emit MIR {source_name} {optimization} produced no artifact"
                    )
                return
            if kind == "native":
                require_success(compiled, f"native {source_name} {standard} {optimization}")
                executed = run([str(artifact)], timeout=30)
                require_success(executed, f"run {source_name} {standard} {optimization}")
                expected = (
                    'demo\n{"name":"demo","private":true,"count":42,"items":["one",2]}\n'
                    if source_name == "stdlib_surface_json.gti" else ""
                )
                if executed.stdout != expected or executed.stderr:
                    raise AssertionError(f"unexpected JSON output: {executed}")
                return
            require_success(compiled, "native owning-error expected regression")
            require_success(run([str(artifact)]), "run owning-error expected regression")

        run_cases(plan, work, arguments.jobs, observe)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
