#!/usr/bin/env python3
"""Nested receiver arrays preserve checked reads/writes and failure cleanup."""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from backend_modes import keep_cpp, native_modes


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixtures = Path(__file__).resolve().parent / "fixtures"
    source = fixtures / "nested_receiver_array.gti"
    lines = source.read_text().splitlines()
    sites = {marker: next(i for i, line in enumerate(lines, 1) if f"// {marker}" in line)
             for marker in ("matrix-read", "matrix-write", "grid-read", "grid-write")}
    modes = native_modes()
    with tempfile.TemporaryDirectory(prefix="gti-nested-receiver-array-") as directory:
        for index, (label, flags, native) in enumerate(modes):
            binary = Path(directory) / str(index)
            built = subprocess.run(
                [str(compiler), str(source), *flags,
                 *keep_cpp(flags), "-o", str(binary), "--",
                 str(source.with_suffix(".cpp")), *native],
                capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, (built.stdout, built.stderr)
            for mode, site in enumerate((None, "matrix-read", "matrix-write",
                                         "grid-read", "grid-read", "grid-write", "grid-write")):
                environment = dict(os.environ, GTI_TEST_ARRAY_MODE=str(mode),
                                   ASAN_OPTIONS="halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1")
                ran = subprocess.run([str(binary)], env=environment, capture_output=True,
                                     text=True, timeout=20)
                assert ran.stdout == "2\n1\n", (mode, ran)
                if site is None:
                    assert ran.returncode == 0 and ran.stderr == "", ran
                else:
                    assert ran.returncode == 70, (mode, ran)
                    assert re.fullmatch(
                        rf".*nested_receiver_array\.gti:{sites[site]}: runtime error"
                        r"\[GTI-R\d+\]: .*\n", ran.stderr), (mode, ran)
                    assert "bounds" in ran.stderr, (mode, ran)
            print(f"{label} {native}: nested receiver arrays passed", flush=True)


if __name__ == "__main__":
    main()
