#!/usr/bin/env python3

import pathlib
import re
import subprocess
import sys
import tempfile


POSITIVE_FIXTURES = (
    "issue_117_array_cleanup.gti",
    "issue_117_array_projection.gti",
    "issue_117_generic_indirect.gti",
    "issue_117_lambda_iife.gti",
    "issue_117_nested_lambda.gti",
    "issue_117_owning_array.gti",
    "issue_118_lambda_comma.gti",
    "issue_119_concept_close.gti",
    "issue_120_generic_destructor_argument.gti",
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
            "usage: issues_117_120_regression_test.py <gti> <fixture-directory>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-issues-117-120-") as temporary:
        root = pathlib.Path(temporary)
        for fixture_name in POSITIVE_FIXTURES:
            source = fixtures / fixture_name
            executable = root / source.stem
            built = run([str(compiler), str(source), "-o", str(executable)])
            if built.returncode != 0:
                return fail(f"build {fixture_name}", built)

            executed = run([str(executable)])
            if executed.returncode != 0 or executed.stdout or executed.stderr:
                return fail(f"run {fixture_name}", executed)

        indirect_source = fixtures / "issue_117_generic_indirect.gti"
        indirect_cpp = root / "issue_117_generic_indirect.cpp"
        emitted_indirect = run(
            [
                str(compiler),
                str(indirect_source),
                "--emit-cpp",
                "-o",
                str(indirect_cpp),
            ]
        )
        if emitted_indirect.returncode != 0:
            return fail("emit indirect concrete generic classes", emitted_indirect)
        indirect_text = indirect_cpp.read_text(encoding="utf8")
        indirect_definitions = re.findall(
            r"class (__gti_type_[0-9a-f]{64}_(?:cell|box)) [{]",
            indirect_text,
        )
        if (
            len(indirect_definitions) != 5
            or len(set(indirect_definitions)) != 5
        ):
            sys.stderr.write(
                "finite nested generic applications lost their five exact "
                "concrete class definitions\n"
            )
            return 1

        for fixture_name in (
            "issue_117_array_cleanup.gti",
            "issue_117_owning_array.gti",
        ):
            source = fixtures / fixture_name
            executable = root / f"{source.stem}-cpp20-o3"
            built = run(
                [
                    str(compiler),
                    str(source),
                    "--std",
                    "c++20",
                    "-O3",
                    "-o",
                    str(executable),
                ]
            )
            if built.returncode != 0:
                return fail(f"build C++20/O3 {fixture_name}", built)
            executed = run([str(executable)])
            if executed.returncode != 0 or executed.stdout or executed.stderr:
                return fail(f"run C++20/O3 {fixture_name}", executed)

        array_source = fixtures / "issue_117_array_cleanup.gti"
        mir_path = root / "issue_117_array_cleanup.mir"
        emitted = run(
            [str(compiler), str(array_source), "--emit-mir", "-o", str(mir_path)]
        )
        if emitted.returncode != 0:
            return fail("emit fixed-array cleanup MIR", emitted)
        mir = mir_path.read_text(encoding="utf8")
        if not mir.startswith("mir-v76 ") or not all(
            marker in mir
            for marker in (
                "fixed-array=schedule(extents=[3]",
                "failure-propagation=destructor",
            )
        ):
            sys.stderr.write(
                "fixed-array cleanup MIR lost its concrete reverse-drop schedule\n"
            )
            return 1

        invalid_source = fixtures / "issue_117_short_circuit_assign.gti"
        invalid = run(
            [str(compiler), str(invalid_source), "-o", str(root / "invalid")]
        )
        if (
            invalid.returncode != 65
            or "error[GTI-S2003]" not in invalid.stderr
            or "Internal compiler error" in invalid.stderr
        ):
            return fail("diagnose short-circuit assignment", invalid)

        recursive_source = root / "recursive_generic_layout.gti"
        recursive_source.write_text(
            "class node<T> { node<T> next; };\nint main() { return 0; }\n",
            encoding="utf8",
        )
        recursive = run(
            [str(compiler), str(recursive_source), "-o", str(root / "recursive")]
        )
        if (
            recursive.returncode != 65
            or "error[GTI-S2082]" not in recursive.stderr
            or "Internal compiler error" in recursive.stderr
        ):
            return fail("diagnose recursive generic layout", recursive)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
