#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile
from harness_timeouts import scaled


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command, text=True, capture_output=True, timeout=scaled(180), check=False
    )


def fail(label: str, process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(
        f"{label}: exit={process.returncode} stdout={process.stdout!r} "
        f"stderr={process.stderr!r}\n"
    )
    return 1


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: issue_144_class_array_field_runtime_test.py "
            "<gti> <fixture-directory>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(sys.argv[2]).resolve()
    positive = fixtures / "issue_144_class_array_field.gti"
    move_only_copy = fixtures / "issue_144_move_only_field_array_copy.gti"

    with tempfile.TemporaryDirectory(prefix="gti-issue-144-") as temporary:
        root = pathlib.Path(temporary)
        for optimization in ("O0", "O2"):
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

        rejected = run(
            [
                str(compiler),
                str(move_only_copy),
                "-O0",
                "--std",
                "c++23",
                "-o",
                str(root / "move-only-copy"),
            ]
        )
        if (
            rejected.returncode != 65
            or "error[GTI-S2003]" not in rejected.stderr
            or "Move-only owners cannot be copied" not in rejected.stderr
            or "Internal compiler error" in rejected.stderr
        ):
            return fail("reject move-only field-array element copy", rejected)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
