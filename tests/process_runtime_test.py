#!/usr/bin/env python3
"""Build and run the public <std/process> contract across backend modes."""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile

from native_symbol_spelling import native_spelling


def run(command: list[str], environment: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        env=environment,
        capture_output=True,
        text=True,
        timeout=180,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(
            f"{command!r}: exit {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: process_runtime_test.py <gti> <runtime-library>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    runtime_library = pathlib.Path(sys.argv[2]).resolve()
    repository = pathlib.Path(__file__).resolve().parent.parent
    source = repository / "tests" / "fixtures" / "stdlib_surface_process.gti"
    environment = os.environ.copy()
    environment.update(
        {
            "GTI_STDLIB_PATH": str(repository / "stdlib"),
            "GTI_RUNTIME_INCLUDE": str(repository / "runtime" / "include"),
            "GTI_RUNTIME_LIBRARY": str(runtime_library),
            "GTI_VENDOR_INCLUDE": str(
                repository / "vendor" / "expected_lite" / "include"
            ),
        }
    )

    with tempfile.TemporaryDirectory(prefix="gti-process-") as directory:
        output = pathlib.Path(directory)
        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O3"):
                binary = output / f"process-{standard}-{optimization}"
                run(
                    [
                        str(compiler),
                        str(source),
                        f"-{optimization}",
                        "--std",
                        standard,
                        "--keep-cpp",
                        "-o",
                        str(binary),
                    ],
                    environment,
                )
                executed = run([str(binary)], environment)
                if executed.stdout or executed.stderr:
                    raise AssertionError(
                        "the process fixture unexpectedly wrote output: "
                        f"{executed!r}"
                    )
                generated = binary.with_suffix(".gti.cpp").read_text(
                    encoding="utf-8"
                )
                if f"{native_spelling(generated, 'gti_rt_process_run_v1')}(" not in generated:
                    raise AssertionError("missing versioned process runtime call")
                if "// GTI verified-MIR body:" not in generated:
                    raise AssertionError("missing verified-MIR emission witness")

    print("std::process runtime contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
