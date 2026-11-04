#!/usr/bin/env python3
"""Owned collection protocol, recursive schemas, evaluation and rollback."""
import pathlib
import subprocess
import sys
import tempfile
from backend_modes import is_cpp_mode, keep_cpp, native_modes


def main():
    compiler = str(pathlib.Path(sys.argv[1]).resolve())
    fixture = pathlib.Path(__file__).parent / "fixtures/collection_initialization.gti"
    prefix = "tea\nauth\nlogout\n0\n2\n7\n"
    with tempfile.TemporaryDirectory(prefix="gti-collection-") as temporary:
        for mode, flags, native in native_modes():
            binary = pathlib.Path(temporary) / f"collection-{mode}"
            built = subprocess.run(
                [compiler, str(fixture), *flags,
                 *keep_cpp(flags), "-o", str(binary)],
                capture_output=True, text=True, timeout=180)
            assert built.returncode == 0, built.stdout + built.stderr
            if is_cpp_mode(flags):
                assert "// GTI verified-MIR body:" in binary.with_suffix(".gti.cpp").read_text()
            for arguments, output, code in (
                ([], prefix + "1\n1\n2\n2\n3\n3\n-3\n-2\n-1\n3\n4\n5\n6\n3\n-6\n-5\n-4\n-7\n", 0),
                (["element-failure"], prefix + "1\n1\n2\n-1\n-7\n", 70),
                (["initialization", "must", "fail"], prefix + "-7\n", 70),
                (["insert", "failure"], prefix + "1\n1\n2\n2\n3\n3\n-3\n-2\n-1\n3\n4\n0\n0\n-4\n-7\n", 70),
            ):
                run = subprocess.run([str(binary), *arguments], capture_output=True,
                                     text=True, timeout=20)
                assert (run.returncode, run.stdout) == (code, output), (mode, run.returncode, run.stdout, run.stderr)
                assert ("GTI-R0002" in run.stderr) if code else not run.stderr
            print(f"collection initialization: {mode} passed", flush=True)


if __name__ == "__main__":
    main()
