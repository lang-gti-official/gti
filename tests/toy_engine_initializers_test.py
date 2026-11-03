#!/usr/bin/env python3
"""Gitea #1/#2 and the 0.337 null-owner field initializer regression."""

from pathlib import Path
import subprocess
import sys
import tempfile
from backend_modes import native_modes


def run(command):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=90)
    if result.returncode != 0:
        raise AssertionError(f"{command}\n{result.stdout}\n{result.stderr}")
    return result


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixtures = Path(__file__).resolve().parent / "fixtures"
    positive = fixtures / "toy_engine_initializers.gti"
    fallible = fixtures / "toy_engine_fallible_field_initializer.gti"
    generated = fixtures / "toy_engine_generated_field_initializer.gti"
    with tempfile.TemporaryDirectory(prefix="gti-toy-engine-initializers-") as directory:
        root = Path(directory)
        for mode, flags, native in native_modes():
            executable = root / f"initializers-{mode}"
            run([compiler, positive, *flags, "-o", executable])
            result = run([executable])
            assert not result.stdout and not result.stderr, result
            owning = root / f"fallible-{mode}"
            run([compiler, fallible, *flags, "-o", owning])
            result = run([owning])
            assert not result.stdout and not result.stderr, result
            implicit = root / f"generated-{mode}"
            run([compiler, generated, *flags,
                 "-o", implicit])
            result = run([implicit])
            assert not result.stdout and not result.stderr, result
    print("Toy-engine initializers: eighteen runtime cases passed, including "
          "exact generated field construction and reverse cleanup")


if __name__ == "__main__":
    main()
