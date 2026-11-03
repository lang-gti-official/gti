#!/usr/bin/env python3
"""Real C ABI return/discard/assignment across native optimization policies."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
from backend_modes import keep_cpp, native_modes, optimization_level


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixtures = Path(__file__).resolve().parent / "fixtures"
    modes = native_modes()
    with tempfile.TemporaryDirectory(prefix="gti-call-result-publication-") as directory:
        for index, (mode, flags, sanitizers) in enumerate(modes):
            binary = Path(directory) / f"native-results-{index}"
            native = Path(directory) / f"native-{index}.o"
            built_c = subprocess.run(
                [os.environ.get("CC", "cc"), "-std=c17", f"-{optimization_level(flags)}", *sanitizers,
                 "-c", str(fixtures / "call_result_native_records.c"), "-o", str(native)],
                capture_output=True, text=True, timeout=60)
            assert built_c.returncode == 0, (built_c.stdout, built_c.stderr)
            built = subprocess.run(
                [str(compiler), str(fixtures / "call_result_native_records.gti"),
                 *flags, *keep_cpp(flags), "-o", str(binary),
                 "--", str(native), *sanitizers],
                capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, (mode, built.stdout, built.stderr)
            ran = subprocess.run(
                [str(binary)], capture_output=True, text=True, timeout=20,
                env=dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
                         UBSAN_OPTIONS="halt_on_error=1"))
            assert (ran.returncode, ran.stdout, ran.stderr) == (0, "", ""), ran
            print(f"{mode}: native call results passed", flush=True)


if __name__ == "__main__":
    main()
