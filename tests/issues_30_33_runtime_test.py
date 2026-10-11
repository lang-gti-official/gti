#!/usr/bin/env python3
"""Narrow place updates, borrowed record arrays and owning error transport."""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from harness_timeouts import scaled


def main():
    compiler = Path(sys.argv[1]).resolve()
    source = Path(__file__).resolve().parent / "fixtures" / "issues_30_33.gti"
    lines = source.read_text().splitlines()
    cases = [
        ("7\n33\n32\n31\n92\n91\n", None, None),
        ("255\n92\n91\n", "increment", "numeric_conversion_out_of_range"),
        ("0\n92\n91\n", "decrement", "numeric_conversion_out_of_range"),
        ("254\n92\n91\n", "add", "numeric_conversion_out_of_range"),
        ("12\n92\n91\n", "divide", "division_by_zero"),
        ("92\n91\n", "read", "index_out_of_bounds"),
        ("92\n91\n", "write", "index_out_of_bounds"),
        ("92\n91\n", "array-add", "index_out_of_bounds"),
        ("92\n91\n", "error-construction", "panic"),
        ("127\n92\n91\n", "signed-up", "numeric_conversion_out_of_range"),
        ("-128\n92\n91\n", "signed-down", "numeric_conversion_out_of_range"),
        ("65535\n92\n91\n", "arithmetic-overflow", "integer_overflow"),
    ]
    modes = [(standard, level, []) for standard in ("c++20", "c++23")
             for level in ("-O0", "-O1", "-O2", "-O3")]
    modes += [("c++23", "-O1", ["-fno-elide-constructors"]),
              ("c++23", "-O1", ["-fsanitize=address,undefined"])]
    with tempfile.TemporaryDirectory(prefix="gti-issues-30-33-") as directory:
        for index, (standard, level, flags) in enumerate(modes):
            binary = Path(directory) / str(index)
            built = subprocess.run([str(compiler), str(source), "--std", standard,
                                    level, "--keep-cpp", "-o", str(binary), "--",
                                    str(source.with_suffix(".cpp")), *flags],
                                   capture_output=True, text=True, timeout=scaled(120))
            assert built.returncode == 0, built
            for mode, (output, marker, reason) in enumerate(cases):
                env = dict(os.environ, GTI_ISSUE_MODE=str(mode), GTI_COLOR="never",
                           ASAN_OPTIONS="halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1")
                ran = subprocess.run([str(binary)], env=env, capture_output=True,
                                     text=True, timeout=scaled(20))
                assert ran.stdout == output, (mode, ran)
                if marker is None:
                    assert ran.returncode == 0 and not ran.stderr, ran
                else:
                    line = next(i for i, text in enumerate(lines, 1)
                                if f"// {marker}" == text[text.find("// "):])
                    assert ran.returncode == 70, (mode, ran)
                    assert re.fullmatch(rf".*issues_30_33\.gti:{line}: runtime error"
                                        rf"\[GTI-R\d+\]: {reason}.*\n", ran.stderr), (mode, ran)
            print(f"{standard} {level} {flags}: {len(cases)} issue regression outcomes passed", flush=True)


if __name__ == "__main__":
    main()
