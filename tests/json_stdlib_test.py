#!/usr/bin/env python3
"""Verify std::json MIR, native DOM/codecs, and temporary-input ownership."""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile

# Peak resident memory one optimized compile of these fixtures may reach. This
# is a regression guard, not a benchmark. Before the SCCP solver shared
# unchanged per-block facts, the -O3 compile of stdlib_surface_json.gti peaked
# at roughly 11 GiB (its `main` body has about 63,000 blocks), and the full
# suite overlaps several such compiles; afterwards it peaks near 3 GiB. The
# ceiling leaves about twice that for allocator and platform variance while a
# return to per-block fact copies, or a new whole-program retention of similar
# size, still fails here instead of exhausting the machine.
PEAK_RESIDENT_LIMIT_BYTES = 6 * 1024 ** 3


def run(command: list[str], *, timeout: int = 300) -> subprocess.CompletedProcess[str]:
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
    if len(sys.argv) != 3:
        raise SystemExit("usage: json_stdlib_test.py <gti> <fixture-directory>")

    gti = str(pathlib.Path(sys.argv[1]).resolve())
    fixtures = pathlib.Path(sys.argv[2]).resolve()
    mir_cases = ("stdlib_surface_json.gti", "stdlib_json_codec.gti")

    with tempfile.TemporaryDirectory(prefix="gti-json-stdlib-") as temporary:
        output = pathlib.Path(temporary)
        for optimization in ("-O0", "-O3"):
            for source_name in mir_cases:
                source = fixtures / source_name
                mir = output / f"{source.stem}-{optimization[1:]}.mir"
                command = [gti, str(source), "--emit-mir", optimization, "-o", str(mir)]
                if optimization == "-O3":
                    compiled, peak = run_measured(command)
                    require_success(compiled, f"emit MIR {source_name} {optimization}")
                    if peak > PEAK_RESIDENT_LIMIT_BYTES:
                        raise AssertionError(
                            f"emit MIR {source_name} {optimization} peaked at "
                            f"{peak / 1024 ** 3:.2f} GiB resident, above the "
                            f"{PEAK_RESIDENT_LIMIT_BYTES / 1024 ** 3:.0f} GiB "
                            "compiler memory guard"
                        )
                else:
                    compiled = run(command)
                    require_success(compiled, f"emit MIR {source_name} {optimization}")
                if not mir.is_file():
                    raise AssertionError(
                        f"emit MIR {source_name} {optimization} produced no artifact"
                    )

        for standard, optimization in (("c++23", "-O0"), ("c++20", "-O3")):
            for source_name in (*mir_cases, "json_temporary_owner.gti"):
                source = fixtures / source_name
                binary = output / f"{source.stem}-{standard}-{optimization[1:]}"
                compiled = run(
                    [gti, str(source), "--std", standard, optimization,
                     "-o", str(binary)], timeout=600
                )
                require_success(compiled, f"native {source_name} {standard} {optimization}")
                executed = run([str(binary)], timeout=30)
                require_success(executed, f"run {source_name} {standard} {optimization}")
                expected = (
                    'demo\n{"name":"demo","private":true,"count":42,"items":["one",2]}\n'
                    if source_name == "stdlib_surface_json.gti" else ""
                )
                if executed.stdout != expected or executed.stderr:
                    raise AssertionError(f"unexpected JSON output: {executed}")

        regression = fixtures / "json_expected_value_backend_bug.gti"
        binary = output / "json-expected-value-backend-bug"
        compiled = run([gti, str(regression), "-O0", "-o", str(binary)], timeout=60)
        require_success(compiled, "native owning-error expected regression")
        require_success(run([str(binary)]), "run owning-error expected regression")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
