#!/usr/bin/env python3
"""Exercise the emitted native tagged carrier's exact object lifetimes."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run(command, quiet=False):
    result = subprocess.run(command, text=True, capture_output=True, timeout=120)
    assert result.returncode == 0, (command, result.stdout, result.stderr)
    if quiet:
        assert not result.stdout and not result.stderr, (command, result.stdout, result.stderr)


def main():
    compiler = Path(sys.argv[1]).resolve()
    root = Path(__file__).resolve().parent.parent
    fixture = root / "tests/fixtures/payload_enum_carrier.gti"
    cxx = os.environ.get("GTI_CXX") or os.environ.get("CXX") or "c++"
    runtime = os.environ.get("GTI_RUNTIME_LIBRARY") or str(compiler.parent / "libgti_runtime.a")
    includes = os.environ.get("GTI_RUNTIME_INCLUDE") or str(root / "runtime/include")
    modes = [(standard, optimization, []) for standard in ("c++20", "c++23")
             for optimization in ("O0", "O1", "O2", "O3")]
    modes.append(("c++23", "O1", ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-payload-carrier-") as directory:
        for index, (standard, optimization, extra) in enumerate(modes):
            source = Path(directory) / f"carrier-{index}.cpp"
            binary = Path(directory) / f"carrier-{index}"
            run([str(compiler), str(fixture), "--std", standard, "--emit-cpp", "-o", str(source)])
            run([cxx, f"-std={standard}", f"-{optimization}", f"-I{includes}",
                 "-D__gti_strict_ieee754=1", "-fno-fast-math", "-ffp-contract=off",
                 f'-DGTI_TEST_GENERATED_PATH="{source}"',
                 str(fixture.with_suffix(".cpp")), runtime, *extra, "-o", str(binary)])
            run([str(binary)], quiet=True)
            print(f"payload carrier: {standard} {optimization} {extra} passed", flush=True)


if __name__ == "__main__":
    main()
