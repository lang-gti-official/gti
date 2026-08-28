#!/usr/bin/env python3

# A generic member callable instantiated on multiple concrete generic owners
# must retain the method's deduced callable type while substituting the owner
# type in its signature and MIR body. Regression: declaration probing compared
# int32_t and uint8_t owner bodies as one template and failed with GTI-B0001;
# partitioning only at definitions later emitted an out-of-scope source `T`.

import pathlib
import re
import subprocess
import sys
import tempfile


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
            "usage: generic_owner_callable_variants_runtime_test.py "
            "<gti> <fixture>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()

    with tempfile.TemporaryDirectory(
        prefix="gti-generic-owner-callable-"
    ) as temporary:
        root = pathlib.Path(temporary)
        emitted = root / "fixture.cpp"
        emission = run(
            [
                str(compiler),
                str(source),
                "-O0",
                "--std",
                "c++23",
                "--emit-cpp",
                "-o",
                str(emitted),
            ]
        )
        if emission.returncode != 0:
            return fail("emit C++", emission)
        text = emitted.read_text()
        specialized = [
            line
            for line in text.splitlines()
            if "__gti_program::calculator<" in line and "apply" in line
        ]
        if not specialized:
            sys.stderr.write(
                "emitted C++ has no concrete-owner callable specialization\n"
            )
            return 1
        if any(re.search(r"\bT\b", line) for line in specialized):
            sys.stderr.write(
                "concrete-owner callable specialization leaked source T: "
                + repr(specialized)
                + "\n"
            )
            return 1
        if not any("std::int32_t" in line for line in specialized) or not any(
            "std::uint8_t" in line for line in specialized
        ):
            sys.stderr.write(
                "callable specializations did not retain both owner types: "
                + repr(specialized)
                + "\n"
            )
            return 1

        for optimization in ("O0", "O3"):
            for standard in ("c++20", "c++23"):
                mode = f"{optimization}-{standard}"
                executable = root / mode
                built = run(
                    [
                        str(compiler),
                        str(source),
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

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
