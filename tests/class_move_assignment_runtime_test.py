#!/usr/bin/env python3

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
            "usage: class_move_assignment_runtime_test.py "
            "<gti> <fixture-directory>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(sys.argv[2]).resolve()
    positive = fixtures / "class_move_assignment.gti"
    negative = fixtures / "class_move_assignment_copy_negative.gti"

    with tempfile.TemporaryDirectory(prefix="gti-class-move-assign-") as temporary:
        root = pathlib.Path(temporary)
        for optimization in ("O0", "O3"):
            for standard in ("c++20", "c++23"):
                mode = f"{optimization}-{standard}"
                executable = root / mode
                built = run(
                    [
                        str(compiler),
                        str(positive),
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
                if (
                    executed.returncode != 0
                    or executed.stdout != ""
                    or executed.stderr != ""
                ):
                    return fail(f"run {mode}", executed)

        generated_path = root / "class-move-assignment.cpp"
        emitted = run(
            [
                str(compiler),
                str(positive),
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
        # Each binding move-assignment preserves MIR's exact three-step
        # schedule: stage the moved value in its lifetime slot, move-assign
        # the destination binding, then destroy only the moved-from shell.
        schedule = re.findall(
            r"(__gti_mir_p_\d+)\.construct\(std::move\(__gti_mir_p_\d+\.get\(\)\)\);\n"
            r"\s*__gti_mir_p_\d+\.get\(\) = std::move\(\1\.get\(\)\);\n"
            r"\s*(?:__gti_mir_drop_success_\d+ = )?\1\.destroy",
            generated,
        )
        if len(schedule) < 3:
            sys.stderr.write(
                "generated C++ lost the staged class move-assignment "
                f"schedule (found {len(schedule)}, expected >= 3)\n"
            )
            return 1

        rejected = run(
            [
                str(compiler),
                str(negative),
                "-O0",
                "--std",
                "c++23",
                "-o",
                str(root / "move-only-copy-assign"),
            ]
        )
        if (
            rejected.returncode != 65
            or "error[GTI-S2003]" not in rejected.stderr
            or "Move-only owners cannot be copied" not in rejected.stderr
            or "Internal compiler error" in rejected.stderr
        ):
            return fail("reject move-only copy assignment", rejected)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
