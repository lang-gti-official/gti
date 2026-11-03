#!/usr/bin/env python3
"""Raw scalar destruction ends a lifetime without releasing native storage."""

import argparse
import os
import pathlib
import subprocess
import tempfile
from backend_modes import native_modes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    modes = native_modes()
    environment = dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="gti-object-destroy-") as directory:
        for label, flags, native in modes:
            binary = pathlib.Path(directory) / label
            build = subprocess.run(
                [str(compiler), str(fixtures / "object_destroy_scalar.gti"),
                 *flags, "-o", str(binary), "--",
                 str(fixtures / "object_destroy_scalar_native.cpp"), *native],
                capture_output=True, text=True, timeout=120, env=environment)
            assert build.returncode == 0, (label, build.stdout, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True,
                                 timeout=30, env=environment)
            assert (run.returncode, run.stdout, run.stderr) == (0, "", ""), (label, run)
            print(f"raw scalar destruction: {label} passed", flush=True)


if __name__ == "__main__":
    main()
