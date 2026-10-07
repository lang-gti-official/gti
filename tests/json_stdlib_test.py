#!/usr/bin/env python3
"""Verify std::json MIR, native DOM/codecs, and temporary-input ownership."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile


def run(command: list[str], *, timeout: int = 300) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        text=True,
        capture_output=True,
        timeout=timeout,
        check=False,
    )


def require_success(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode != 0:
        raise AssertionError(
            f"{label} failed with {result.returncode}\n"
            f"stdout:\n{result.stdout}\n"
            f"stderr:\n{result.stderr}"
        )


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: json_stdlib_test.py <gti> <fixture-directory>")

    gti = str(pathlib.Path(sys.argv[1]).resolve())
    fixtures = pathlib.Path(sys.argv[2]).resolve()
    mir_cases = ("stdlib_surface_json.gti", "stdlib_json_codec.gti")

    with tempfile.TemporaryDirectory(prefix="gti-json-stdlib-") as temporary:
        output = pathlib.Path(temporary)
        for optimization in ("-O0", "-O3"):
            for source_name in mir_cases:
                source = fixtures / source_name
                mir = output / f"{source.stem}-{optimization[1:]}.mir"
                compiled = run(
                    [gti, str(source), "--emit-mir", optimization, "-o", str(mir)]
                )
                require_success(compiled, f"emit MIR {source_name} {optimization}")
                if not mir.is_file():
                    raise AssertionError(
                        f"emit MIR {source_name} {optimization} produced no artifact"
                    )

        for standard, optimization in (("c++23", "-O0"), ("c++20", "-O3")):
            for source_name in (*mir_cases, "json_temporary_owner.gti"):
                source = fixtures / source_name
                binary = output / f"{source.stem}-{standard}-{optimization[1:]}"
                compiled = run(
                    [gti, str(source), "--std", standard, optimization,
                     "-o", str(binary)], timeout=600
                )
                require_success(compiled, f"native {source_name} {standard} {optimization}")
                executed = run([str(binary)], timeout=30)
                require_success(executed, f"run {source_name} {standard} {optimization}")
                expected = (
                    'demo\n{"name":"demo","private":true,"count":42,"items":["one",2]}\n'
                    if source_name == "stdlib_surface_json.gti" else ""
                )
                if executed.stdout != expected or executed.stderr:
                    raise AssertionError(f"unexpected JSON output: {executed}")

        regression = fixtures / "json_expected_value_backend_bug.gti"
        binary = output / "json-expected-value-backend-bug"
        compiled = run([gti, str(regression), "-O0", "-o", str(binary)], timeout=60)
        require_success(compiled, "native owning-error expected regression")
        require_success(run([str(binary)]), "run owning-error expected regression")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
