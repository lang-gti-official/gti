#!/usr/bin/env python3
"""Inline capturing lambda literals as generic-algorithm arguments."""

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


def validate_emission(generated: str, mode: str) -> bool:
    required = (
        "[limit = __gti_mir_p_",
        "[moved = std::move(__gti_mir_p_",
    )
    if any(item not in generated for item in required):
        sys.stderr.write(f"{mode} did not spell the fused capture literals\n")
        return False
    if "__gti_mir_closure_factory_" in generated:
        sys.stderr.write(f"{mode} materialized an inline literal argument\n")
        return False
    return True


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: inline_lambda_argument_runtime_test.py <gti> <source>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-inline-lambda-") as temporary:
        root = pathlib.Path(temporary)
        for optimization in ("O0", "O3"):
            executable = root / f"inline-lambda-{optimization}"
            built = run(
                [str(compiler), str(source), f"-{optimization}", "-o", str(executable)]
            )
            if built.returncode != 0:
                return fail(f"build {optimization}", built)
            executed = run([str(executable)])
            if (
                executed.returncode != 0
                or "inline lambda arguments ok" not in executed.stdout
            ):
                return fail(f"run {optimization}", executed)

            emitted = root / f"inline-lambda-{optimization}.cpp"
            emission = run(
                [
                    str(compiler),
                    str(source),
                    f"-{optimization}",
                    "--emit-cpp",
                    "-o",
                    str(emitted),
                ]
            )
            if emission.returncode != 0:
                return fail(f"emit {optimization}", emission)
            if not validate_emission(
                emitted.read_text(encoding="utf-8"), optimization
            ):
                return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
