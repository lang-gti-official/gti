#!/usr/bin/env python3

from __future__ import annotations

import subprocess
import sys


def identity(seed: int) -> bytes:
    return b"".join(f"{(seed + index) & 0xFF:02x}".encode() for index in range(32))


ORDINARY = b"unit.gti:7: runtime error[GTI-R0001]: integer_overflow in addition\n"
RUNTIME = (
    b"<runtime>:0: runtime error[GTI-R0011]: "
    b"allocation_failure in hosted_arguments\n"
)
UNICODE_SOURCE = (
    b'A \\"\\\\\\t\\r\\n'
    + "λ́\u00a0😀".encode()
    + b"\\xC2\\x85"
    + b"\\xE2\\x80\\xA8"
    + b"\\xE2\\x80\\xAE"
    + b"\\xC0\\xAF\\xE2\\x00"
)
UNICODE = (
    UNICODE_SOURCE
    + b":9: runtime error[GTI-R0007]: index_out_of_bounds in string\n"
)
CLEANUP = (
    b"cleanup.gti:11: runtime error[GTI-R0014]: failure_during_cleanup; "
    b"primary [GTI-R0001] at unit.gti:7; secondary [GTI-R0002]\n"
)
OTHER = (
    b"other.gti:13: runtime error[GTI-R0003]: "
    b"modulo_by_zero in integer_modulo\n"
)
CANONICAL_OUTCOMES = (
    b"unit.gti:7: runtime error[GTI-R0001]: "
    b"integer_overflow in multiplication\n"
)


def invoke(helper: str, mode: str) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run([helper, mode], stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def expect(
    helper: str,
    mode: str,
    status: int,
    stdout: bytes,
    stderr: bytes,
) -> None:
    result = invoke(helper, mode)
    assert result.returncode == status, (mode, result.returncode, result)
    assert result.stdout == stdout, (mode, result.stdout)
    assert result.stderr == stderr, (mode, result.stderr)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: runtime_failure_test.py <helper>")
    helper = sys.argv[1]

    expect(helper, "report", 0, b"", ORDINARY)
    expect(helper, "runtime", 0, b"", RUNTIME)
    expect(helper, "unicode", 0, b"", UNICODE)
    expect(helper, "cleanup", 0, b"", CLEANUP)
    expect(helper, "invalid_outcome", 1, b"", b"")
    expect(helper, "canonical_outcomes", 0, b"", CANONICAL_OUTCOMES)
    expect(helper, "noncanonical_outcomes", 1, b"", b"")
    expect(helper, "write_closed", 0, b"", b"")
    expect(helper, "write_broken_pipe", 0, b"", b"")

    expect(helper, "terminal_observer", 70, b"O", ORDINARY)
    expect(helper, "terminal_no_observer", 70, b"", ORDINARY)
    expect(helper, "terminal_exception", 70, b"O", ORDINARY)
    expect(helper, "terminal_mutation", 70, b"O", ORDINARY)
    expect(helper, "terminal_reentry", 70, b"O", ORDINARY)
    expect(helper, "terminal_cleanup", 70, b"", CLEANUP)
    expect(helper, "terminal_closed", 70, b"O", b"")
    expect(helper, "terminal_broken_pipe", 70, b"O", b"")

    race = invoke(helper, "terminal_race")
    assert race.returncode == 70, race
    assert race.stdout == b"O", race.stdout
    assert race.stderr in (ORDINARY, OTHER), race.stderr
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
