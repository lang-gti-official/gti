#!/usr/bin/env python3
"""Compile unused concrete aliases without inventing executable instances."""

import pathlib
import subprocess
import sys
import tempfile


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).resolve().parent / "fixtures/type_only_aliases.gti"
    with tempfile.TemporaryDirectory(prefix="gti-type-only-alias-") as temporary:
        for standard in ("c++20", "c++23"):
            for optimization in ("-O0", "-O3"):
                binary = pathlib.Path(temporary) / f"{standard}-{optimization}"
                command = [str(compiler), str(source), "--std", standard,
                           optimization, "-o", str(binary)]
                built = subprocess.run(command, capture_output=True, text=True, timeout=120)
                assert built.returncode == 0, (command, built.stdout, built.stderr)
                ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
                assert (ran.returncode, ran.stdout, ran.stderr) == (0, "", ""), ran
                print(f"{standard} {optimization}: type-only aliases passed", flush=True)


if __name__ == "__main__":
    main()
