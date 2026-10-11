#!/usr/bin/env python3
"""Verify std::json MIR, native DOM/codecs, and temporary-input ownership."""

from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import sys
import tempfile

from case_jobs import configured_jobs, positive_jobs, run_cases
from harness_timeouts import scaled

# Peak resident memory one optimized compile of these fixtures may reach. This
# is a regression guard, not a benchmark. Before the SCCP solver shared
# unchanged per-block facts, the -O3 compile of stdlib_surface_json.gti peaked
# at roughly 11 GiB (its `main` body has about 63,000 blocks), and the full
# suite overlaps several such compiles; afterwards it peaks near 3 GiB. The
# ceiling leaves about twice that for allocator and platform variance while a
# return to per-block fact copies, or a new whole-program retention of similar
# size, still fails here instead of exhausting the machine.
PEAK_RESIDENT_LIMIT_BYTES = 6 * 1024 ** 3


def run(command: list[str], *, timeout: int = scaled(300)) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        text=True,
        capture_output=True,
        timeout=timeout,
        check=False,
    )


def run_measured(command: list[str]) -> tuple[subprocess.CompletedProcess[str], int]:
    """Run one child and return its result with its peak resident size in bytes.

    wait4 reports the reaped child's own resource usage, the figure
    `/usr/bin/time -l` prints as the maximum resident set size. There is no
    per-call timeout; the CTest timeout bounds the whole test.
    """
    with tempfile.TemporaryFile(mode="w+") as stdout, tempfile.TemporaryFile(
        mode="w+"
    ) as stderr:
        with subprocess.Popen(command, text=True, stdout=stdout, stderr=stderr) as child:
            _, status, usage = os.wait4(child.pid, 0)
            # Popen has not observed the exit; record it so the context manager
            # does not wait again for an already reaped process.
            child.returncode = os.waitstatus_to_exitcode(status)
        stdout.seek(0)
        stderr.seek(0)
        result = subprocess.CompletedProcess(
            command, child.returncode, stdout.read(), stderr.read()
        )
    # ru_maxrss is bytes on macOS and kibibytes on Linux.
    peak = usage.ru_maxrss if sys.platform == "darwin" else usage.ru_maxrss * 1024
    return result, peak


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
                        help="bounded concurrent compile workers")
    arguments = parser.parse_args()
    gti = str(pathlib.Path(arguments.gti).resolve())
    fixtures = pathlib.Path(arguments.fixtures).resolve()
    mir_cases = ("stdlib_surface_json.gti", "stdlib_json_codec.gti")
    with tempfile.TemporaryDirectory(prefix="gti-json-stdlib-") as temporary:
        output = pathlib.Path(temporary)
        # Each MIR emission and each native build is an independent
        # bounded-worker case. Workers only compile; the native runs, the
        # memory guard and every check happen on this thread in plan order,
        # with the same expectations and timeouts as the serial form.
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

        def work(case):
            kind, source_name, standard, optimization = case
            source = fixtures / source_name
            peak = None
            if kind == "mir":
                artifact = output / f"{source.stem}-{optimization[1:]}.mir"
                command = [gti, str(source), "--emit-mir", optimization, "-o", str(artifact)]
                if optimization == "-O3":
                    compiled, peak = run_measured(command)
                else:
                    compiled = run(command)
            elif kind == "native":
                artifact = output / f"{source.stem}-{standard}-{optimization[1:]}"
                compiled = run(
                    [gti, str(source), "--std", standard, optimization,
                     "-o", str(artifact)], timeout=scaled(600)
                )
            else:
                artifact = output / "json-expected-value-backend-bug"
                compiled = run([gti, str(source), "-O0", "-o", str(artifact)], timeout=scaled(60))
            return (case, compiled, artifact, peak)

        def observe(value):
            (kind, source_name, standard, optimization), compiled, artifact, peak = value
            if kind == "mir":
                require_success(compiled, f"emit MIR {source_name} {optimization}")
                if peak is not None and peak > PEAK_RESIDENT_LIMIT_BYTES:
                    raise AssertionError(
                        f"emit MIR {source_name} {optimization} peaked at "
                        f"{peak / 1024 ** 3:.2f} GiB resident, above the "
                        f"{PEAK_RESIDENT_LIMIT_BYTES / 1024 ** 3:.0f} GiB "
                        "compiler memory guard"
                    )
                if not artifact.is_file():
                    raise AssertionError(
                        f"emit MIR {source_name} {optimization} produced no artifact"
                    )
                return
            if kind == "native":
                require_success(compiled, f"native {source_name} {standard} {optimization}")
                executed = run([str(artifact)], timeout=scaled(30))
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
