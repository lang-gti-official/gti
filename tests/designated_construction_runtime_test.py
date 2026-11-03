#!/usr/bin/env python3
"""Designated fields execute through MIR, including argument rollback."""
import pathlib
import subprocess
import sys
import tempfile
from backend_modes import is_cpp_mode, keep_cpp, native_modes


def main():
    compiler = str(pathlib.Path(sys.argv[1]).resolve())
    fixture = pathlib.Path(__file__).parent / "fixtures/designated_construction.gti"
    with tempfile.TemporaryDirectory(prefix="gti-designated-") as temporary:
        for mode, flags, native in native_modes():
            binary = pathlib.Path(temporary) / f"designated-{mode}"
            built = subprocess.run(
                [compiler, str(fixture), *flags,
                 *keep_cpp(flags), "-o", str(binary)],
                capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, built.stdout + built.stderr
            if is_cpp_mode(flags):
                assert "// GTI verified-MIR body:" in binary.with_suffix(".gti.cpp").read_text()
            for arguments, output, code in (
                    ([], "0\ntea\nlogin\n3\n4\n9\n8\n7\n", 0),
                    (["fail"], "0\ntea\nlogin\n3\n4\n8\n7\n", 70)):
                run = subprocess.run([str(binary), *arguments],
                                     capture_output=True, text=True, timeout=20)
                assert (run.returncode, run.stdout) == (code, output), (mode, run.returncode, run.stdout, run.stderr)
                assert ("GTI-R0002" in run.stderr) if code else not run.stderr
            print(f"designated construction: {mode} passed", flush=True)
        # A generated constructor with two or more owned-parameter fields keeps
        # the text-view parameter's entry loan; its end-borrow record must not
        # push the body outside the C++ text vocabulary.
        borrowed = pathlib.Path(__file__).parent / "fixtures/designated_text_view_borrow.gti"
        for level, flags, _native in native_modes():
            binary = pathlib.Path(temporary) / f"designated-text-view-{level}"
            built = subprocess.run(
                [compiler, str(borrowed), *flags, *keep_cpp(flags), "-o", str(binary)],
                capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, built.stdout + built.stderr
            if is_cpp_mode(flags):
                assert "// GTI verified-MIR body:" in binary.with_suffix(".gti.cpp").read_text()
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
            assert (run.returncode, run.stdout, run.stderr) == (0, "", ""), (
                level, run.returncode, run.stdout, run.stderr)
            print(f"designated text-view borrow: {level} passed", flush=True)


if __name__ == "__main__":
    main()
