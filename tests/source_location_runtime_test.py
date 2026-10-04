#!/usr/bin/env python3
"""Caller-site source-location capture across native configurations."""

from __future__ import annotations

import os
import pathlib
import re
import subprocess
import sys
import tempfile


def run(
    command: list[str], environment: dict[str, str]
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        capture_output=True,
        text=True,
        timeout=180,
        env=environment,
        check=False,
    )


def require_success(process: subprocess.CompletedProcess[str], label: str) -> None:
    if process.returncode != 0:
        raise AssertionError(
            f"{label}: exit {process.returncode}\n"
            f"stdout={process.stdout}\nstderr={process.stderr}"
        )


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: source_location_runtime_test.py <gti> <source>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    constructor_source = source.with_name("source_location_constructors.gti")
    repository = pathlib.Path(__file__).resolve().parent.parent
    environment = dict(
        os.environ,
        GTI_STDLIB_PATH=str(repository / "stdlib"),
    )

    with tempfile.TemporaryDirectory(prefix="gti-source-location-") as temporary:
        root = pathlib.Path(temporary)
        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O3"):
                label = f"{standard}-{optimization}"
                executable = root / label
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
                require_success(built, f"{label} build")
                executed = run([str(executable)], environment)
                require_success(executed, f"{label} execution")
                if executed.stdout or executed.stderr:
                    raise AssertionError(
                        f"{label} produced output: "
                        f"stdout={executed.stdout!r}, stderr={executed.stderr!r}"
                    )

                constructor_executable = root / f"constructors-{label}"
                constructor_built = run(
                    [
                        str(compiler),
                        str(constructor_source),
                        "--std",
                        standard,
                        f"-{optimization}",
                        "-o",
                        str(constructor_executable),
                    ],
                    environment,
                )
                require_success(constructor_built, f"{label} constructor build")
                constructor_executed = run(
                    [str(constructor_executable)], environment
                )
                require_success(
                    constructor_executed, f"{label} constructor execution"
                )
                if constructor_executed.stdout or constructor_executed.stderr:
                    raise AssertionError(
                        f"{label} constructor fixture produced output: "
                        f"stdout={constructor_executed.stdout!r}, "
                        f"stderr={constructor_executed.stderr!r}"
                    )

        emitted_mir = root / "source-location.mir"
        emitted = run(
            [str(compiler), str(source), "--emit-mir", "-o", str(emitted_mir)],
            environment,
        )
        require_success(emitted, "MIR emission")
        mir = emitted_mir.read_text(encoding="utf-8")
        for intrinsic in (
            "source-file-name",
            "source-function-name",
            "source-line",
            "source-column",
        ):
            # Terminal/caller-contract fields also appear on the instruction;
            # select this instruction's source location without assuming that
            # it immediately follows the intrinsic kind.
            matches = re.findall(
                rf"\bintrinsic={intrinsic} [^\n]*? "
                r"source-location=([^\n]*?) synchronization=",
                mir,
            )
            if len(matches) != 2:
                raise AssertionError(
                    f"expected two {intrinsic} calls with caller metadata, "
                    f"found {len(matches)}"
                )
            if not any(value.endswith(",4:main,9:0") for value in matches):
                raise AssertionError(
                    f"{intrinsic} lost the direct-call location: {matches}"
                )
            if not any(value.endswith(",4:main,15:0") for value in matches):
                raise AssertionError(
                    f"{intrinsic} lost default-argument caller location: {matches}"
                )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
