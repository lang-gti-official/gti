#!/usr/bin/env python3
"""Exercise free-function items through generic and managed-thread paths."""

from __future__ import annotations

import os
import pathlib
import re
import subprocess
import sys
import tempfile


def run(command: list[str], environment: dict[str, str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command, capture_output=True, text=True, check=False, env=environment
    )


def fail(label: str, process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(
        f"{label}: exit={process.returncode} stdout={process.stdout!r} "
        f"stderr={process.stderr!r}\n"
    )
    return 1


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: function_item_runtime_test.py <gti> <source>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    repository = pathlib.Path(__file__).resolve().parent.parent
    environment = os.environ.copy()
    environment["GTI_STDLIB_PATH"] = str(repository / "stdlib")

    with tempfile.TemporaryDirectory(prefix="gti-function-items-") as temporary:
        root = pathlib.Path(temporary)
        mir_path = root / "function-items.mir"
        emitted = run(
            [
                str(compiler),
                "--execution-profile",
                "concurrent",
                "--emit-mir",
                "--std",
                "c++23",
                "-O1",
                str(source),
                "-o",
                str(mir_path),
            ],
            environment,
        )
        if emitted.returncode != 0:
            return fail("emit MIR", emitted)
        mir = mir_path.read_text(encoding="utf-8")
        if not mir.startswith("mir-v") or " valid=1\n" not in mir[:80]:
            sys.stderr.write("emitted MIR is not marked valid\n")
            return 1
        item_lines = [
            line for line in mir.splitlines() if "operation=function-item" in line
        ]
        identities = [
            re.search(
                r"info\(type\(\d+;class=\d+;enum=\d+;parameter=\d+;native-c=0;"
                r"function=(\d+);",
                line,
            )
            for line in item_lines
        ]
        if (
            len(item_lines) < 3
            or any(identity is None for identity in identities)
            or any(identity.group(1) == "0" for identity in identities if identity)
        ):
            sys.stderr.write("MIR lost exact function-item identity\n")
            return 1

        for standard, optimization in (("c++20", "-O0"), ("c++23", "-O3")):
            mode = f"{standard}-{optimization[1:]}"
            executable = root / mode
            built = run(
                [
                    str(compiler),
                    "--execution-profile",
                    "concurrent",
                    "--std",
                    standard,
                    optimization,
                    str(source),
                    "-o",
                    str(executable),
                ],
                environment,
            )
            if built.returncode != 0 or "GTI-B0001" in built.stderr:
                return fail(f"build {mode}", built)
            executed = run([str(executable)], environment)
            if executed.returncode != 0 or executed.stdout or executed.stderr:
                return fail(f"run {mode}", executed)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
