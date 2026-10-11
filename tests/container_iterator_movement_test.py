#!/usr/bin/env python3
"""Checked shared/exclusive movement across sequence and associative cursors."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from case_jobs import configured_jobs, positive_jobs, run_cases
from contiguous_iterator_completion_test import MODES


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--mode", action="append", choices=[mode[0] for mode in MODES])
    parser.add_argument("--jobs", type=positive_jobs, default=configured_jobs())
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    compiler = options.compiler.resolve()
    environment = dict(os.environ, GTI_STDLIB_PATH=str(root / "stdlib"),
                       ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    fixtures = ("container_iterator_movement", "container_reverse_probe")
    modes = [mode for mode in MODES if not options.mode or mode[0] in options.mode]
    with tempfile.TemporaryDirectory(prefix="gti-container-movement-") as temporary:
        output = Path(temporary)

        def build(case):
            fixture, label, flags, native = case
            binary = output / f"{fixture}-{label}"
            command = [str(compiler), str(root / "tests/fixtures" / f"{fixture}.gti"),
                       *flags, "--progress=never", "--keep-cpp", "-o", str(binary)]
            if native:
                command += ["--", *native]
            result = subprocess.run(command, env=environment, capture_output=True,
                                    text=True, timeout=180)
            assert result.returncode == 0, (fixture, label, result.stdout, result.stderr)
            assert "// GTI verified-MIR body:" in binary.with_suffix(".gti.cpp").read_text()
            return binary

        def observe(binary):
            result = subprocess.run([str(binary)], env=environment, capture_output=True,
                                    text=True, timeout=30)
            assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), result

        cases = [(f"{fixture}-{label}", (fixture, label, flags, native))
                 for fixture in fixtures for label, flags, native in modes]
        run_cases(cases, build, options.jobs, observe)


if __name__ == "__main__":
    main()
