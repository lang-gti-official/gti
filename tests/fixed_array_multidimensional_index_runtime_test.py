#!/usr/bin/env python3
"""Dynamic indexing of a multidimensional fixed array.

Regression for the declined chained element access: `matrix[row][column]` with
runtime row and column left the complete C++ text vocabulary, because the
element spelling admitted a nonterminal index only when it was a compile-time
in-bounds constant. The read form raised GTI-B0001 outright and the write form
lost its checked successor.

The bounds contract is the other half. One MIR instruction and one failure site
describe the whole chained access, so the emitted text owns a bound for every
dimension and the safety analysis may discharge that site only when it proves
every dimension. These cases pin both: values land through the chained read and
write, an out-of-range index in any dimension raises the defined
index_out_of_bounds failure, and the constant-index forms still reach the same
elements. Everything runs at O0 and O3, because the discharge path only exists
at the optimizing level.

The chained write's behavior on a violation is held to the one-dimensional
dynamic write's behavior rather than asserted outright: the write failure edge
belongs to the terminal checked write and is fixed separately. Whatever the
single-index form does, every dimension of the chained form must do, and the
array must survive intact either way.
"""

import pathlib
import subprocess
import sys
import tempfile

REPORT = "runtime error[GTI-R0007]: index_out_of_bounds in fixed_array"

BOUNDS_FIXTURES = (
    "fixed_array_multidimensional_first_out_of_bounds",
    "fixed_array_multidimensional_second_out_of_bounds",
    "fixed_array_multidimensional_third_out_of_bounds",
    "fixed_array_multidimensional_field_out_of_bounds",
)

WRITE_FIXTURES = (
    "fixed_array_multidimensional_write_first_out_of_bounds",
    "fixed_array_multidimensional_write_second_out_of_bounds",
)

BASELINE_FIXTURE = "fixed_array_single_dimension_write_out_of_bounds"


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(label: str, process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(
        f"{label}: exit={process.returncode} stdout={process.stdout!r} "
        f"stderr={process.stderr!r}\n"
    )
    return 1


def build(
    compiler: pathlib.Path,
    fixtures: pathlib.Path,
    root: pathlib.Path,
    name: str,
    optimization: str,
) -> tuple[pathlib.Path, subprocess.CompletedProcess[str]]:
    executable = root / f"{name}-{optimization}"
    built = run(
        [
            str(compiler),
            str(fixtures / f"{name}.gti"),
            f"-{optimization}",
            "-o",
            str(executable),
        ]
    )
    return executable, built


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: fixed_array_multidimensional_index_runtime_test.py "
            "<gti> <fixture-directory>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(sys.argv[2]).resolve()

    with tempfile.TemporaryDirectory(prefix="gti-multidimensional-") as temporary:
        root = pathlib.Path(temporary)

        for optimization in ("O0", "O3"):
            executable, built = build(
                compiler,
                fixtures,
                root,
                "fixed_array_multidimensional_index",
                optimization,
            )
            if built.returncode != 0:
                return fail(f"build values {optimization}", built)
            executed = run([str(executable)])
            if (
                executed.returncode != 0
                or executed.stdout != ""
                or executed.stderr != ""
            ):
                return fail(f"run values {optimization}", executed)

            for name in BOUNDS_FIXTURES:
                executable, built = build(
                    compiler, fixtures, root, name, optimization
                )
                if built.returncode != 0:
                    return fail(f"build {name} {optimization}", built)
                executed = run([str(executable)])
                if (
                    executed.returncode != 70
                    or REPORT not in executed.stderr
                    or executed.stdout != ""
                ):
                    return fail(f"run {name} {optimization}", executed)

            baseline_executable, built = build(
                compiler, fixtures, root, BASELINE_FIXTURE, optimization
            )
            if built.returncode != 0:
                return fail(f"build baseline {optimization}", built)
            baseline = run([str(baseline_executable)])
            if "corrupt" in baseline.stdout:
                return fail(f"baseline corrupted {optimization}", baseline)

            for name in WRITE_FIXTURES:
                executable, built = build(
                    compiler, fixtures, root, name, optimization
                )
                if built.returncode != 0:
                    return fail(f"build {name} {optimization}", built)
                executed = run([str(executable)])
                if "corrupt" in executed.stdout:
                    return fail(f"run {name} {optimization}", executed)
                if (
                    executed.returncode != baseline.returncode
                    or executed.stdout != baseline.stdout
                    or executed.stderr.replace(name, BASELINE_FIXTURE)
                    != baseline.stderr
                ):
                    sys.stderr.write(
                        f"{name} {optimization} diverged from the "
                        f"single-index dynamic write: "
                        f"exit={executed.returncode} "
                        f"stdout={executed.stdout!r} "
                        f"stderr={executed.stderr!r} against "
                        f"exit={baseline.returncode} "
                        f"stdout={baseline.stdout!r} "
                        f"stderr={baseline.stderr!r}\n"
                    )
                    return 1

        # The emitted text owns the per-dimension bound: the chained read pairs
        # one nonterminal guard with the terminal checked read, and neither
        # spelling subscripts the array before its own check.
        generated_path = root / "chained-read.cpp"
        emitted = run(
            [
                str(compiler),
                str(fixtures / f"{BOUNDS_FIXTURES[0]}.gti"),
                "-O0",
                "--emit-cpp",
                "-o",
                str(generated_path),
            ]
        )
        if emitted.returncode != 0:
            return fail("emit chained read", emitted)
        generated = generated_path.read_text(encoding="utf8")
        entry_start = generated.rfind("bool __gti_entry__gti_mir_failure(")
        entry = generated[entry_start:] if entry_start >= 0 else ""
        guard = " = ::gti_internal::backend::mir_checked_array_bound_v1("
        read = " = ::gti_internal::backend::mir_checked_array_read_v1("
        if entry.count(guard) != 1 or entry.count(read) != 1:
            sys.stderr.write(
                "the chained element read lost its per-dimension bound guard\n"
            )
            return 1
        guard_at = entry.find(guard)
        read_at = entry.find(read)
        status = entry.rfind("__gti_mir_failure_status_", 0, guard_at)
        variable = entry[status:guard_at]
        branch = f"if ({variable}.code == GTI_FAILURE_CODE_NONE_V1) {{"
        if (
            status < 0
            or guard_at > read_at
            or branch not in entry[guard_at:read_at]
            or entry[read_at - len(variable) : read_at] != variable
        ):
            sys.stderr.write(
                "the chained element read lost the success branch that pairs "
                "its guard with the terminal checked read\n"
            )
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
