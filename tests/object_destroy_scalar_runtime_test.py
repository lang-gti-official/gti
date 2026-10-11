#!/usr/bin/env python3
"""Raw scalar destruction ends a lifetime without releasing native storage."""

import argparse
import os
import pathlib
import subprocess
import tempfile
from harness_timeouts import scaled


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    modes = [(f"{standard}-{level}", standard, level, [])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes += [("no-elide", "c++23", "O0", ["-fno-elide-constructors"]),
              ("sanitized", "c++23", "O1",
               ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    environment = dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="gti-object-destroy-") as directory:
        for label, standard, level, flags in modes:
            binary = pathlib.Path(directory) / label
            build = subprocess.run(
                [str(compiler), str(fixtures / "object_destroy_scalar.gti"),
                 "--std", standard, f"-{level}", "-o", str(binary), "--",
                 str(fixtures / "object_destroy_scalar_native.cpp"), *flags],
                capture_output=True, text=True, timeout=scaled(120), env=environment)
            assert build.returncode == 0, (label, build.stdout, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True,
                                 timeout=scaled(30), env=environment)
            assert (run.returncode, run.stdout, run.stderr) == (0, "", ""), (label, run)
            print(f"raw scalar destruction: {label} passed", flush=True)


if __name__ == "__main__":
    main()
