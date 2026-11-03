#!/usr/bin/env python3
"""Constant division and composed envelopes retain integer failures/cleanup."""
from pathlib import Path
import subprocess
import sys
import tempfile

from backend_modes import is_cpp_mode, native_modes


def expected(mode, value):
    unsigned = value if value < 128 else (1 << 64) - 1 - (255 - value)
    signed = {0: -(1 << 31), 1: -(1 << 31) + 1,
              254: (1 << 31) - 2, 255: (1 << 31) - 1}.get(value, value - 128)
    if mode == 0:
        result = unsigned // 4 + 13835058055282163712
    elif mode == 1:
        quotient = abs(signed) // 2 * (-1 if signed > 0 else 1)
        result = quotient + 1073741823
    elif mode == 2:
        result = unsigned // 2 + 9223372036854775809
        if result > (1 << 64) - 1:
            return 70, "22\n"
    elif mode == 3:
        if signed in (-(1 << 31), -(1 << 31) + 1):
            return 70, "22\n"
        result = -signed + 1
    elif mode == 4:
        if value >= 64:
            return 70, "22\n"
        result = 1 << value
    elif mode == 6:
        if unsigned * 2 > (1 << 64) - 1:
            return 70, "22\n"
        result = unsigned
    else:
        first = [value % 251, value % 127, value % 17]
        for _ in range(8):
            first[0] = sum(first) // 3
        result = first[0]
    return 0, f"{result}\n22\n"


def main():
    compiler = Path(sys.argv[1]).resolve()
    source = Path(__file__).parent / "fixtures/optimizer_division_envelopes.gti"
    cases = [(mode, value) for mode in (0, 1, 2, 3, 5, 6) for value in range(256)]
    cases += [(4, value) for value in range(67)]
    # LLVM at O0/O2/O3 under the default policy, plus the C++ profile when
    # GTI_TEST_BACKENDS selects it (tests/backend_modes.py).
    modes = [(label, flags) for label, flags, _native in native_modes(levels=("O0", "O2", "O3"))]
    baseline = {}
    with tempfile.TemporaryDirectory(prefix="gti-division-envelopes-") as directory:
        for label, flags in modes:
            backend, level = ("cpp" if is_cpp_mode(flags) else "llvm"), "-" + label.rsplit("-", 1)[1]
            binary = Path(directory) / label
            command = [str(compiler), str(source), *flags, "-o", str(binary)]
            built = subprocess.run(command, capture_output=True, text=True, timeout=180)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for case in cases:
                result = subprocess.run([str(binary), *("x" * n for n in case)],
                                        capture_output=True, text=True, timeout=15)
                status, stdout = expected(*case)
                assert (result.returncode, result.stdout) == (status, stdout), (
                    backend, level, case, result.returncode, result.stdout, result.stderr)
                if status:
                    assert "runtime error[GTI-R0001]" in result.stderr, result.stderr
                else:
                    assert not result.stderr, result.stderr
                observed = (result.returncode, result.stdout, result.stderr)
                # The first mode the policy selects is the baseline; every
                # later mode (another level, or the C++ profile) must match it.
                if case not in baseline:
                    baseline[case] = observed
                else:
                    assert observed == baseline[case], (backend, level, case, observed, baseline[case])
            print(f"Division envelopes: {backend} {level} passed {len(cases)} cases", flush=True)


if __name__ == "__main__":
    main()
