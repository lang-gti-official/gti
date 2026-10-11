#!/usr/bin/env python3
"""Process-owner allocations can disappear; escaping owners still allocate."""

from pathlib import Path
import subprocess
import sys
import tempfile
from harness_timeouts import scaled


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixtures = Path(__file__).resolve().parent / "fixtures"
    with tempfile.TemporaryDirectory(prefix="gti-process-allocation-elision-") as directory:
        for standard in ("c++20", "c++23"):
            for level in ("O0", "O3"):
                binary = Path(directory) / f"{standard}-{level}"
                built = subprocess.run([
                    str(compiler), str(fixtures / "process_allocation_elision.gti"),
                    f"-{level}", "--std", standard, "--keep-cpp", "-o", str(binary),
                    "--", str(fixtures / "process_allocation_elision.cpp")],
                    capture_output=True, text=True, timeout=scaled(120))
                assert built.returncode == 0, built
                generated = binary.with_suffix(".gti.cpp").read_text()
                assert "T *storage = new (std::nothrow) T(args...);" in generated
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=scaled(30))
                expected = "1 1\n1 1\n" if level == "O0" else "0 0\n1 1\n"
                assert (result.returncode, result.stdout, result.stderr) == (0, expected, ""), (
                    standard, level, result)
                print(f"Process allocation elision: {standard}/{level} passed", flush=True)


if __name__ == "__main__":
    main()
