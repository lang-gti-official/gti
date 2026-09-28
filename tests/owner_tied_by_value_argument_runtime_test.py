#!/usr/bin/env python3

"""A named owner-tied iterator local passed by value into generic calls.

Passing a NAMED borrowing local (a copyable class value holding a
reference field, e.g. `std::detail::vector_iterator<T>`) by value into a
generic function used to raise GTI-B0001: HIR plans no call-input stage
for a borrowed-state copy, so the argument arrives in MIR as a bare Load
copy that the C++ emitter had no vocabulary for. The emitter now materializes
that copy at the MIR Load and moves the sequenced local into the consuming
call, preserving both the owner dependency and MIR's copy-before-call order.
"""

import pathlib
import re
import subprocess
import sys
import tempfile

EXPECTED_OUTPUT = "distance-read ok\ndistance-mut ok\nrelay ok\nprvalue ok\n"


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
            "usage: owner_tied_by_value_argument_runtime_test.py "
            "<gti> <source>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(
        prefix="gti-owner-tied-by-value-"
    ) as temporary:
        root = pathlib.Path(temporary)
        modes = [
            (f"{standard}-{optimization}",
             ["--std", standard, f"-{optimization}"], [])
            for standard in ("c++20", "c++23")
            for optimization in ("O0", "O1", "O3")
        ] + [
            ("no-elide", ["--std", "c++23", "-O0"],
             ["-fno-elide-constructors"]),
            ("sanitized", ["--std", "c++23", "-O1"],
             ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]),
        ]
        for mode, flags, native in modes:
            executable = root / mode
            built = run(
                [
                    str(compiler),
                    str(source),
                    *flags,
                    "-o",
                    str(executable),
                    *(["--", *native] if native else []),
                ]
            )
            if built.returncode != 0:
                return fail(f"build {mode}", built)

            executed = run([str(executable)])
            if (
                executed.returncode != 0
                or executed.stdout != EXPECTED_OUTPUT
                or executed.stderr != ""
            ):
                return fail(f"run {mode}", executed)

        # Copy at the Load, not at the consumer. Across Invoke blocks the
        # passive representation lives in an outer slot and retires after use.
        generated = root / "generated.cpp"
        emitted = run(
            [
                str(compiler),
                str(source),
                "--emit-cpp",
                "-o",
                str(generated),
            ]
        )
        if emitted.returncode != 0:
            return fail("emit O0", emitted)
        generated_text = generated.read_text(encoding="utf8")
        cross_block = re.search(
            r"(?P<slot>__gti_mir_v_\d+)\.construct\(__gti_mir_p_\d+\.get\(\)\);"
            r".*?std::move\((?P=slot)\.get\(\)\).*?(?P=slot)\.destroy\(\);",
            generated_text, re.DOTALL,
        )
        if cross_block is None:
            sys.stderr.write("O0 emission lost cross-block copy/consume/retire order\n")
            return 1
        if "auto __gti_mir_v_" not in generated_text or not re.search(
            r"std::move\(__gti_mir_v_\d+\)", generated_text
        ):
            sys.stderr.write(
                "O0 emission lost the sequenced by-value argument copy\n"
            )
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
