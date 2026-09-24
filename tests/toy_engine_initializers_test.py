#!/usr/bin/env python3
"""Gitea #1/#2 and the 0.337 null-owner field initializer regression."""

from pathlib import Path
import subprocess
import sys
import tempfile


def run(command):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=90)
    if result.returncode != 0:
        raise AssertionError(f"{command}\n{result.stdout}\n{result.stderr}")
    return result


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixtures = Path(__file__).resolve().parent / "fixtures"
    positive = fixtures / "toy_engine_initializers.gti"
    negative = fixtures / "toy_engine_fallible_field_initializer.gti"
    with tempfile.TemporaryDirectory(prefix="gti-toy-engine-initializers-") as directory:
        root = Path(directory)
        for standard in ("c++20", "c++23"):
            for optimization in ("-O0", "-O1", "-O3"):
                executable = root / f"initializers-{standard}-{optimization}"
                run([compiler, positive, "--std", standard, optimization, "-o", executable])
                result = run([executable])
                assert not result.stdout and not result.stderr, result
                artifact = root / f"unsupported-{standard}-{optimization}.cpp"
                result = subprocess.run(
                    list(map(str, [compiler, negative, "--std", standard, optimization,
                                   "--emit-cpp", "-o", artifact])),
                    capture_output=True, text=True, timeout=90)
                assert result.returncode > 0, result
                assert "error[GTI-B0003]" in result.stderr, result.stderr
                assert "GTI-B0001" not in result.stderr, result.stderr
                assert "declaration-time field initializer" in result.stderr, result.stderr
                assert "initialization method after construction" in result.stderr, result.stderr
                assert f"{negative}:1:1:" in result.stderr, result.stderr
                assert not artifact.exists(), artifact
    print("Toy-engine initializers: six runtime cases and six limitation diagnostics passed")


if __name__ == "__main__":
    main()
