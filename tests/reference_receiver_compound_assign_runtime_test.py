#!/usr/bin/env python3
"""Compound assignment through a reference-parameter receiver.

Regression for the GTI-B0001 rejection "place projection result type
disagrees with its concrete root and projection chain": the compound
assignment lowering manufactured its receiver from the raw binding record,
so a reference-parameter receiver recorded the reference type on a place
whose projection chain already dereferenced to the referenced class.
The fixture exercises +=, -=, and *= through a reference parameter,
a nested forwarding call, and a local-receiver control, at O0 and O3.
"""

import pathlib
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
            "usage: reference_receiver_compound_assign_runtime_test.py "
            "<gti> <source>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(
        prefix="gti-reference-compound-assign-"
    ) as temporary:
        root = pathlib.Path(temporary)
        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O3"):
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
                    ]
                )
                if built.returncode != 0:
                    return fail(f"build {mode}", built)
                if "GTI-B0001" in built.stderr:
                    return fail(f"build {mode} raised GTI-B0001", built)

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
