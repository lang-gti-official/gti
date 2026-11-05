#!/usr/bin/env python3
"""Target failure precedes RHS effects; reserved/exclusive chains drain on failure."""
import pathlib
import subprocess
import sys
import tempfile
from backend_modes import native_modes


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures/operator_place_failure.gti"
    with tempfile.TemporaryDirectory(prefix="gti-operator-place-failure-") as temporary:
        for mode, flags, native in native_modes():
            binary = pathlib.Path(temporary) / f"{mode}"
            built = subprocess.run([str(compiler), str(source), *flags, "-o", str(binary)],
                                   capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, (mode, built.stdout, built.stderr)
            for arguments, output, code in (
                ((), "row\ncell\naccess guard\nrhs\nguard\n", "GTI-R0002"),
                (("x",), "row\ncell\naccess guard\nguard\n", "GTI-R0007"),
                (("x", "x"), "row\ncell\naccess guard\nrhs\nguard\n", "GTI-R0002"),
                (("x", "x", "x"), "named cell\naccess guard\nrhs\nguard\n", "GTI-R0002"),
                (("x", "x", "x", "x"), "named cell\naccess guard\nguard\n", "GTI-R0007"),
            ):
                result = subprocess.run([str(binary), *arguments], capture_output=True,
                                        text=True, timeout=30)
                assert result.returncode == 70 and result.stdout == output, (mode, arguments, result)
                assert code in result.stderr and "Internal compiler error" not in result.stderr, result
    print("PASS 20 operator-place failure/cleanup observations")


if __name__ == "__main__":
    main()
