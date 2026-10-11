#!/usr/bin/env python3
"""Designated fields execute through MIR, including argument rollback."""
import pathlib
import subprocess
import sys
import tempfile
from harness_timeouts import scaled


def main():
    compiler = str(pathlib.Path(sys.argv[1]).resolve())
    fixture = pathlib.Path(__file__).parent / "fixtures/designated_construction.gti"
    with tempfile.TemporaryDirectory(prefix="gti-designated-") as temporary:
        for standard in ("c++20", "c++23"):
            for level in ("O0", "O1", "O3"):
                binary = pathlib.Path(temporary) / f"designated-{standard}-{level}"
                built = subprocess.run(
                    [compiler, str(fixture), "--std", standard, f"-{level}",
                     "--keep-cpp", "-o", str(binary)],
                    capture_output=True, text=True, timeout=scaled(120))
                assert built.returncode == 0, built.stdout + built.stderr
                assert "// GTI verified-MIR body:" in binary.with_suffix(".gti.cpp").read_text()
                for arguments, output, code in (
                        ([], "0\ntea\nlogin\n3\n4\n9\n8\n7\n", 0),
                        (["fail"], "0\ntea\nlogin\n3\n4\n8\n7\n", 70)):
                    run = subprocess.run([str(binary), *arguments],
                                         capture_output=True, text=True, timeout=scaled(20))
                    assert (run.returncode, run.stdout) == (code, output), (
                        standard, level, run.returncode, run.stdout, run.stderr)
                    assert ("GTI-R0002" in run.stderr) if code else not run.stderr
                print(f"designated construction: {standard}/{level} passed", flush=True)


if __name__ == "__main__":
    main()
