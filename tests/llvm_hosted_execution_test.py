#!/usr/bin/env python3
"""Uninstrumented hosted programs through the real compiler/driver boundary."""

import os
from pathlib import Path
import sys
import tempfile

from backend_execution import Endpoint, execute


def main():
    gti, root = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    cases = (
        ("llvm_hosted_success.gti", None),
        ("llvm_hosted_startup_failure.gti", "hosted startup failure"),
        ("llvm_hosted_entry_failure.gti", "hosted entry failure"),
        ("stdlib_surface_source_location.gti", None),
    )
    endpoints = (
        Endpoint("cpp", "O3", "c++20"), Endpoint("cpp", "O3", "c++23"),
        Endpoint("llvm", "O0"), Endpoint("llvm", "O3"),
    )
    env = {**os.environ, "NO_COLOR": "1"}
    with tempfile.TemporaryDirectory(prefix="gti-llvm-hosted-cli-") as temporary:
        scratch = Path(temporary)
        for filename, failure in cases:
            observations = []
            for index, endpoint in enumerate(endpoints):
                result = execute(gti, root, scratch / filename / str(index),
                                 root / "tests/fixtures" / filename, endpoint, env)
                if result.error:
                    raise AssertionError(f"{filename} {endpoint}: {result}")
                expected_code = 70 if failure else 0
                if (result.returncode != expected_code or result.stdout
                        or (not failure and result.stderr)
                        or (failure and ("GTI-R0017" not in result.stderr
                                         or failure not in result.stderr
                                         or filename not in result.stderr))):
                    raise AssertionError(f"{filename} {endpoint}: {result}")
                observations.append(result.observable)
            if any(value != observations[0] for value in observations):
                raise AssertionError(f"hosted observable drift: {filename}: {observations}")
            print(f"{filename}: uninstrumented C++20/23 O3 and LLVM O0/O3 passed",
                  flush=True)
        for status in (-2147483648, -1, 0, 1, 70, 255, 256, 512, 2147483647):
            expression = "-2147483647 - 1" if status == -2147483648 else str(status)
            source = scratch / f"exit-{status}.gti"
            source.write_text(
                "int ordinary() { return 256; }\n"
                "int main() { if (ordinary() != 256) { return 93; } return "
                + expression + "; }\n", encoding="utf-8")
            expected = status if 0 <= status <= 255 else 1
            for index, endpoint in enumerate(endpoints):
                result = execute(gti, root, scratch / f"status-{status}-{index}",
                                 source, endpoint, env)
                if result.error or result.observable != (expected, "", ""):
                    raise AssertionError(f"portable exit {status} {endpoint}: {result}")
            print(f"portable exit {status}: preserved GTI function result and status {expected}",
                  flush=True)


if __name__ == "__main__":
    main()
