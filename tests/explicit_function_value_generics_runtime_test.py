#!/usr/bin/env python3
"""Exercise explicit function value generics through MIR and native execution."""

import os
import pathlib
import subprocess
import sys
import tempfile


def run(command: list[str], environment: dict[str, str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command, text=True, capture_output=True, check=False, env=environment
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
            "usage: explicit_function_value_generics_runtime_test.py "
            "<gti> <source>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    repository = pathlib.Path(__file__).resolve().parent.parent
    environment = os.environ.copy()
    environment["GTI_STDLIB_PATH"] = str(repository / "stdlib")

    with tempfile.TemporaryDirectory(prefix="gti-explicit-value-generics-") as tmp:
        root = pathlib.Path(tmp)
        mir_path = root / "program.mir"
        emitted = run(
            [
                str(compiler),
                str(source),
                "--emit-mir",
                "--std",
                "c++23",
                "-O1",
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
        for value in (
            "value-arguments=[{1:3:0}]",
            "value-arguments=[{1:2:0}]",
            "value-arguments=[{1:4:0}]",
            "value-arguments=[{1:5:0}]",
            "value-arguments=[{1:6:0}]",
        ):
            if value not in mir:
                sys.stderr.write(f"MIR lost explicit generic identity {value!r}\n")
                return 1

        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O1", "O3"):
                mode = f"{standard}-{optimization}"
                executable = root / mode
                built = run(
                    [
                        str(compiler),
                        str(source),
                        "--std",
                        standard,
                        f"-{optimization}",
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
