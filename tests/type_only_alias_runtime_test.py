#!/usr/bin/env python3
"""Compile unused concrete aliases without inventing executable instances."""

import pathlib
import subprocess
import sys
import tempfile
from backend_modes import native_modes


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(__file__).resolve().parent / "fixtures"
    with tempfile.TemporaryDirectory(prefix="gti-type-only-alias-") as temporary:
        for mode, flags, native in native_modes():
            for fixture in ("type_only_aliases", "alias_constructor_declarations"):
                source = fixtures / f"{fixture}.gti"
                binary = pathlib.Path(temporary) / f"{fixture}-{mode}"
                command = [str(compiler), str(source), *flags, "-o", str(binary)]
                built = subprocess.run(command, capture_output=True, text=True, timeout=120)
                assert built.returncode == 0, (command, built.stdout, built.stderr)
                ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
                assert (ran.returncode, ran.stdout, ran.stderr) == (0, "", ""), ran
                print(f"{mode}: {fixture} passed", flush=True)


if __name__ == "__main__":
    main()
