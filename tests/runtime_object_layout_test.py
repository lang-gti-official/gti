#!/usr/bin/env python3
"""Runtime object layouts use the actual verified-MIR representation."""

import pathlib
import subprocess
import sys
import tempfile
from backend_modes import is_cpp_mode, keep_cpp, native_modes


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).resolve().parent.parent / "examples/119-runtime-object-layout.gti"
    with tempfile.TemporaryDirectory(prefix="gti-object-layout-") as directory:
        for mode, flags, native in native_modes():
            binary = pathlib.Path(directory) / f"layout-{mode}"
            command = [str(compiler), str(source), *flags,
                       *keep_cpp(flags), "-o", str(binary)]
            built = subprocess.run(command, capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            if is_cpp_mode(flags):
                cpp = binary.with_suffix(".gti.cpp").read_text()
                assert "// GTI verified-MIR body:" in cpp and "sizeof(" in cpp and "alignof(" in cpp
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), result
            print(f"runtime object layout: {mode} passed", flush=True)


if __name__ == "__main__":
    main()
