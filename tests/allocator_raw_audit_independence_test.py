#!/usr/bin/env python3
"""Optional raw-pointer auditing never decides allocator acceptance or code.

The allocator example and resource fixtures rely on documented unsafe provider
contracts. Their checked ownership and borrowing must not depend on the
optional audit: Off and Warn accept the same programs and emit identical C++.
Warn findings are advisory and may report analysis limitations.
"""

import argparse
import pathlib
import subprocess
import tempfile
from harness_timeouts import scaled


SOURCES = (
    "examples/121-programmable-allocators.gti",
    "examples/125-manual-object-lifetimes.gti",
    "examples/126-conditional-thread-capabilities.gti",
    "examples/128-inline-object-pool.gti",
    "tests/fixtures/arena_placement.gti",
    "tests/fixtures/object_construction.gti",
    "tests/fixtures/object_relocation.gti",
    "tests/fixtures/programmable_allocators.gti",
    "tests/fixtures/resource_container_usage.gti",
    "tests/fixtures/resource_region_contracts.gti",
    "tests/fixtures/uninitialized_carrier.gti",
)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    arguments = parser.parse_args()
    compiler = arguments.compiler.resolve()
    root = pathlib.Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="gti-allocator-audit-") as directory:
        temporary = pathlib.Path(directory)
        for relative in SOURCES:
            source = root / relative
            emitted = {}
            for mode in ("off", "warn"):
                output = temporary / f"{source.stem}-{mode}.cpp"
                result = subprocess.run(
                    [str(compiler), str(source), "--emit-cpp", "-o", str(output),
                     f"--raw-pointer-audit={mode}"],
                    capture_output=True, text=True, timeout=scaled(300))
                assert result.returncode == 0, (relative, mode, result.stderr)
                assert "error[" not in result.stderr, (relative, mode, result.stderr)
                emitted[mode] = output.read_bytes()
            assert emitted["off"] == emitted["warn"], (
                relative, "raw-pointer auditing changed generated code")
    print("allocator raw-pointer audit independence passed")


if __name__ == "__main__":
    main()
