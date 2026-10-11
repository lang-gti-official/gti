#!/usr/bin/env python3
"""Runtime object layouts use the actual verified-MIR representation."""

import pathlib
import subprocess
import sys
import tempfile
from harness_timeouts import scaled


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).resolve().parent.parent / "examples/119-runtime-object-layout.gti"
    with tempfile.TemporaryDirectory(prefix="gti-object-layout-") as directory:
        for standard in ("c++20", "c++23"):
            for level in ("O0", "O1", "O3"):
                binary = pathlib.Path(directory) / f"layout-{standard}-{level}"
                command = [str(compiler), str(source), f"-{level}", "--std", standard,
                           "--keep-cpp", "-o", str(binary)]
                built = subprocess.run(command, capture_output=True, text=True, timeout=scaled(120))
                assert built.returncode == 0, (command, built.stdout, built.stderr)
                cpp = binary.with_suffix(".gti.cpp").read_text()
                assert "// GTI verified-MIR body:" in cpp and "sizeof(" in cpp and "alignof(" in cpp
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=scaled(30))
                assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), result
                print(f"runtime object layout: {standard} {level} passed", flush=True)


if __name__ == "__main__":
    main()
