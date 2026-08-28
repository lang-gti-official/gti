#!/usr/bin/env python3

"""A constructor initializer list that builds class-typed fields.

`Holder(int a) : field(Leaf(a))` previously declined with GTI-B0001 because the
C++ emitter had no spelling for MIR's construct-then-publish field schedule.
The fix publishes the whole initializer expression in the native C++
member-initializer list, so this test both runs the program and pins that
spelling in the emitted C++.
"""

import pathlib
import re
import subprocess
import sys
import tempfile


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
            "usage: constructor_constructed_field_runtime_test.py "
            "<gti> <fixture-directory>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixture = (
        pathlib.Path(sys.argv[2]).resolve() / "constructor_constructed_field.gti"
    )

    with tempfile.TemporaryDirectory(prefix="gti-constructed-field-") as temporary:
        root = pathlib.Path(temporary)
        for optimization in ("O0", "O3"):
            for standard in ("c++20", "c++23"):
                mode = f"{optimization}-{standard}"
                executable = root / mode
                built = run(
                    [
                        str(compiler),
                        str(fixture),
                        f"-{optimization}",
                        "--std",
                        standard,
                        "-o",
                        str(executable),
                    ]
                )
                if built.returncode != 0:
                    return fail(f"build {mode}", built)
                executed = run([str(executable)])
                # Every scenario returns a distinct nonzero code, so a failing
                # drop count or drop order names itself in the exit status.
                if (
                    executed.returncode != 0
                    or executed.stdout != ""
                    or executed.stderr != ""
                ):
                    return fail(f"run {mode}", executed)

        generated_path = root / "constructed-field.cpp"
        emitted = run(
            [
                str(compiler),
                str(fixture),
                "-O0",
                "--std",
                "c++23",
                "--emit-cpp",
                "-o",
                str(generated_path),
            ]
        )
        if emitted.returncode != 0:
            return fail("emit C++23/O0", emitted)

        generated = generated_path.read_text(encoding="utf8")
        # The three-field constructor must spell one prvalue per field in the
        # native member-initializer list, in declaration order.
        trio = re.search(
            r"Trio::Trio\([^\n]*?\)\s*:\s*"
            r"left\(::__gti_program::Probe\(__gti_mir_arg_0\)\), "
            r"middle\(::__gti_program::Probe\(__gti_mir_arg_1\)\), "
            r"right\(::__gti_program::Probe\(__gti_mir_arg_2\)\)",
            generated,
        )
        if trio is None:
            sys.stderr.write(
                "generated C++ lost the native member-initializer spelling for "
                "constructed class fields\n"
            )
            return 1
        # A mixed schedule keeps the scalar parameter copy beside the
        # constructed field, still in declaration order.
        tagged = re.search(
            r"Tagged::Tagged\([^\n]*?\)\s*:\s*"
            r"name\(__gti_mir_arg_0\), "
            r"held\(::__gti_program::Probe\(__gti_mir_arg_1\)\)",
            generated,
        )
        if tagged is None:
            sys.stderr.write(
                "generated C++ lost the mixed scalar/constructed-field "
                "member-initializer spelling\n"
            )
            return 1
        # No body assignment may remain: assigning a class field would need a
        # second object and an invented move.
        for field in ("left", "middle", "right", "held"):
            if f"(*this).{field} = " in generated:
                sys.stderr.write(
                    f"generated C++ assigns field '{field}' in the constructor "
                    "body instead of publishing it in the member-initializer "
                    "list\n"
                )
                return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
