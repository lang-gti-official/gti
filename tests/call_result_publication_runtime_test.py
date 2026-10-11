#!/usr/bin/env python3
"""Real C ABI return/discard/assignment across native optimization policies."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
from harness_timeouts import scaled


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixtures = Path(__file__).resolve().parent / "fixtures"
    modes = [(standard, level, []) for standard in ("c++20", "c++23")
             for level in ("-O0", "-O1", "-O3")]
    modes += [(standard, "-O1", ["-fsanitize=address,undefined"])
              for standard in ("c++20", "c++23")]
    with tempfile.TemporaryDirectory(prefix="gti-call-result-publication-") as directory:
        for index, (standard, level, flags) in enumerate(modes):
            binary = Path(directory) / f"native-results-{index}"
            native = Path(directory) / f"native-{index}.o"
            built_c = subprocess.run(
                [os.environ.get("CC", "cc"), "-std=c17", level, *flags,
                 "-c", str(fixtures / "call_result_native_records.c"), "-o", str(native)],
                capture_output=True, text=True, timeout=scaled(60))
            assert built_c.returncode == 0, (built_c.stdout, built_c.stderr)
            built = subprocess.run(
                [str(compiler), str(fixtures / "call_result_native_records.gti"),
                 "--std", standard, level, "--keep-cpp", "-o", str(binary),
                 "--", "-fno-elide-constructors", str(native), *flags],
                capture_output=True, text=True, timeout=scaled(120))
            assert built.returncode == 0, (standard, level, built.stdout, built.stderr)
            ran = subprocess.run(
                [str(binary)], capture_output=True, text=True, timeout=scaled(20),
                env=dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
                         UBSAN_OPTIONS="halt_on_error=1"))
            assert (ran.returncode, ran.stdout, ran.stderr) == (0, "", ""), ran
            print(f"{standard} {level} {flags}: native call results passed", flush=True)


if __name__ == "__main__":
    main()
