#!/usr/bin/env python3
"""Explicit generic duplication through ordinary verified-MIR calls."""
import pathlib
import subprocess
import sys
import tempfile
from harness_timeouts import scaled


def main():
    compiler = str(pathlib.Path(sys.argv[1]).resolve())
    fixture = pathlib.Path(__file__).parent / "fixtures/generic_clone.gti"
    with tempfile.TemporaryDirectory(prefix="gti-clone-") as temporary:
        modes = [(standard, level, []) for standard in ("c++20", "c++23")
                 for level in ("O0", "O1", "O3")]
        modes += [("c++23", "O0", ["-fno-elide-constructors"]),
                  ("c++23", "O1", ["-fsanitize=address,undefined",
                                     "-fno-omit-frame-pointer"])]
        for index, (standard, level, flags) in enumerate(modes):
            binary = pathlib.Path(temporary) / f"clone-{index}"
            built = subprocess.run(
                [compiler, str(fixture), "--std", standard, f"-{level}",
                 "--keep-cpp", "-o", str(binary), "--", *flags],
                capture_output=True, text=True, timeout=scaled(180))
            assert built.returncode == 0, built.stdout + built.stderr
            assert "// GTI verified-MIR body:" in binary.with_suffix(".gti.cpp").read_text()
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=scaled(20))
            assert (run.returncode, run.stdout, run.stderr) == (0, "", ""), (
                standard, level, flags, run.returncode, run.stdout, run.stderr)
            failed_binary = pathlib.Path(temporary) / f"clone-failure-{index}"
            built_failure = subprocess.run(
                [compiler, str(fixture.with_name("generic_clone_failure.gti")),
                 "--std", standard, f"-{level}", "-o", str(failed_binary), "--", *flags],
                capture_output=True, text=True, timeout=scaled(180))
            assert built_failure.returncode == 0, built_failure.stdout + built_failure.stderr
            failed = subprocess.run([str(failed_binary)], capture_output=True, text=True, timeout=scaled(20))
            assert (failed.returncode, failed.stdout) == (
                70, "1\n2\n3\n101\n11\n102\n-11\n-3\n-2\n-1\n"), (
                    standard, level, flags, failed.returncode, failed.stdout, failed.stderr)
            assert "GTI-R0002" in failed.stderr and "division_by_zero" in failed.stderr
            assert "AddressSanitizer" not in failed.stderr and "UndefinedBehaviorSanitizer" not in failed.stderr
            print(f"generic clone: {standard}/{level}/{flags} passed", flush=True)


if __name__ == "__main__":
    main()
