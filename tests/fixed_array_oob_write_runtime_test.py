#!/usr/bin/env python3
"""Dynamic fixed-array element writes must raise index_out_of_bounds.

Regression for the silently dropped write failure: the emitted C++ assigned
the checked array write helper's status to a local that nothing consumed, so
an out-of-bounds write no-opped instead of raising the defined failure that
the matching read already reported. Covers the plain and compound write forms
against the read form's report, and keeps in-bounds writes committing, at O0
and O3.
"""

import pathlib
import re
import subprocess
import sys
import tempfile

FAILURE_REPORT = (
    "runtime error[GTI-R0007]: index_out_of_bounds in fixed_array"
)


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(label: str, process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(
        f"{label}: exit={process.returncode} stdout={process.stdout!r} "
        f"stderr={process.stderr!r}\n"
    )
    return 1


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: fixed_array_oob_write_runtime_test.py "
            "<gti> <fixture-directory>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(sys.argv[2]).resolve()
    failing = (
        ("write", fixtures / "fixed_array_oob_write.gti"),
        ("compound-write", fixtures / "fixed_array_oob_compound_write.gti"),
        ("read-parity", fixtures / "fixed_array_oob_read_parity.gti"),
    )
    in_bounds = fixtures / "fixed_array_in_bounds_write.gti"

    with tempfile.TemporaryDirectory(prefix="gti-array-oob-write-") as temporary:
        root = pathlib.Path(temporary)
        for optimization in ("O0", "O3"):
            for label, fixture in failing:
                mode = f"{label}-{optimization}"
                executable = root / mode
                built = run(
                    [
                        str(compiler),
                        str(fixture),
                        f"-{optimization}",
                        "-o",
                        str(executable),
                    ]
                )
                if built.returncode != 0:
                    return fail(f"build {mode}", built)
                executed = run([str(executable)])
                expected_report = f"{fixture.name}:7: {FAILURE_REPORT}\n"
                if (
                    executed.returncode != 70
                    or executed.stdout != ""
                    or executed.stderr != expected_report
                ):
                    return fail(f"run {mode}", executed)

            mode = f"in-bounds-{optimization}"
            executable = root / mode
            built = run(
                [
                    str(compiler),
                    str(in_bounds),
                    f"-{optimization}",
                    "-o",
                    str(executable),
                ]
            )
            if built.returncode != 0:
                return fail(f"build {mode}", built)
            executed = run([str(executable)])
            if (
                executed.returncode != 0
                or executed.stdout != ""
                or executed.stderr != ""
            ):
                return fail(f"run {mode}", executed)

        # The emitted C++ must consume every checked array write status:
        # each helper call's status local has to feed the failure branch
        # that publishes the failure record.
        generated_path = root / "oob-write.cpp"
        emitted = run(
            [
                str(compiler),
                str(failing[0][1]),
                "-O0",
                "--emit-cpp",
                "-o",
                str(generated_path),
            ]
        )
        if emitted.returncode != 0:
            return fail("emit O0", emitted)
        generated = generated_path.read_text(encoding="utf8")
        statuses = re.findall(
            r"(__gti_mir_failure_status_\d+) = "
            r"::gti_internal::backend::mir_checked_array_write_v1\(",
            generated,
        )
        if not statuses:
            sys.stderr.write(
                "generated C++ lost the checked array write helper\n"
            )
            return 1
        for status in statuses:
            if f"if ({status}.code == GTI_FAILURE_CODE_NONE_V1)" not in generated:
                sys.stderr.write(
                    f"generated C++ discards {status} instead of branching "
                    "to the failure successor\n"
                )
                return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
