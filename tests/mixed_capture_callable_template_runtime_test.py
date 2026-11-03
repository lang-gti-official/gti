#!/usr/bin/env python3

# A generic algorithm instantiated with both a scalar-capturing lambda and a
# class-capturing lambda must build and run: the two callable instances carry
# different cleanup shapes, and the emitted deduced-callable template reunites
# them through an instantiation-time destructibility dispatch. (Regression:
# the combination ICEd with GTI-B0001 "lowered callable instances do not
# share one source template representation" while each family alone passed.)

import pathlib
import subprocess
import sys
import tempfile
from backend_modes import native_modes


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command, text=True, capture_output=True, timeout=180, check=False
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
            "usage: mixed_capture_callable_template_runtime_test.py "
            "<gti> <fixture-directory>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = (
        pathlib.Path(sys.argv[2]).resolve()
        / "mixed_capture_callable_template.gti"
    )

    with tempfile.TemporaryDirectory(
        prefix="gti-mixed-capture-template-"
    ) as temporary:
        root = pathlib.Path(temporary)
        for mode, flags, native in native_modes():
            executable = root / mode
            built = run(
                [
                    str(compiler),
                    str(source),
                    *flags,
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

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
