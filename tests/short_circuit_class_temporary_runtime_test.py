#!/usr/bin/env python3
"""Class temporaries in short-circuited operands drop only when constructed."""

import pathlib
import subprocess
import sys
import tempfile
from backend_modes import native_modes


def run(command):
    return subprocess.run(command, capture_output=True, text=True, timeout=120)


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures/short_circuit_class_temporary.gti"
    with tempfile.TemporaryDirectory(prefix="gti-short-circuit-") as temporary:
        root = pathlib.Path(temporary)
        for mode, flags, native in native_modes():
            binary = root / f"short-circuit-{mode}"
            built = run([str(compiler), str(source), *flags, "-o", str(binary)])
            if built.returncode:
                raise AssertionError(f"build failed: {built.stdout}{built.stderr}")
            # No argument skips every right operand (no Probe drops); one
            # argument evaluates them, dropping each Probe exactly once.
            for arguments, output in (([], "0\n"), (["evaluate"], "2\n")):
                result = run([str(binary), *arguments])
                if (result.returncode, result.stdout, result.stderr) != (0, output, ""):
                    raise AssertionError(
                        f"{binary.name} {arguments}: exit={result.returncode}, "
                        f"stdout={result.stdout!r}, stderr={result.stderr!r}")
    print("short-circuit class temporaries passed")


if __name__ == "__main__":
    main()
