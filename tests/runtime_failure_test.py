#!/usr/bin/env python3

from __future__ import annotations

import errno
import os
import re
import subprocess
import sys
from harness_timeouts import scaled


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


ANSI = re.compile(rb"\x1b\[[0-9;]*m")


def environment(extra=None):
    result = dict(os.environ, TERM="xterm-256color")
    result.pop("GTI_COLOR", None)
    result.pop("NO_COLOR", None)
    return result | (extra or {})


def invoke(helper: str, mode: str, extra=None) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run([helper, mode], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          env=environment(extra), timeout=scaled(10))


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


def test_colour(helper: str) -> None:
    # Both ABI versions and emergency/observer paths retain their exact
    # unstyled text, immutable payload and exit status.
    for mode in ("report", "runtime", "unicode", "cleanup", "panic_report",
                 "panic_snapshot", "panic_escaped", "panic_truncated",
                 "panic_utf8_boundary", "panic_cleanup", "terminal_observer",
                 "terminal_exception", "terminal_mutation", "terminal_reentry",
                 "panic_terminal_observer", "panic_terminal_mutation",
                 "panic_terminal_exception", "panic_terminal_reentry",
                 "panic_terminal_cleanup", "panic_legacy_join"):
        plain = invoke(helper, mode)
        coloured = invoke(helper, mode, {"GTI_COLOR": "always"})
        assert coloured.returncode == plain.returncode, mode
        assert coloured.stdout == plain.stdout, mode
        assert ANSI.sub(b"", coloured.stderr) == plain.stderr, (mode, coloured.stderr)
        assert coloured.stderr.count(b"\x1b[1;31m") == 1, (mode, coloured.stderr)
        assert coloured.stderr.count(b"\x1b[0m") == 1, (mode, coloured.stderr)
        assert b"\x1b[1;31mruntime error[GTI-R" in coloured.stderr, mode
        assert coloured.stderr.endswith(b"\n") and coloured.stderr.count(b"\n") == 1
    for extra, coloured in (
        ({"GTI_COLOR": "auto"}, False),
        ({"GTI_COLOR": "invalid"}, False),
        ({"GTI_COLOR": "never"}, False),
        ({"NO_COLOR": "1"}, False),
        ({"TERM": "dumb"}, False),
        ({"GTI_COLOR": "always", "NO_COLOR": "1", "TERM": "dumb"}, True),
    ):
        result = invoke(helper, "panic_report", extra)
        assert result.returncode == 0 and result.stdout == b""
        assert (b"\x1b[" in result.stderr) == coloured, (extra, result.stderr)
    for mode, status, stdout in (
        ("invalid_outcome", 1, b""), ("reserved_14", 1, b""),
        ("write_closed", 0, b""), ("write_broken_pipe", 0, b""),
        ("terminal_closed", 70, b"O"), ("terminal_broken_pipe", 70, b"O"),
        ("panic_terminal_closed", 70, b"O"),
        ("panic_terminal_broken_pipe", 70, b"O"),
    ):
        result = invoke(helper, mode, {"GTI_COLOR": "always"})
        assert (result.returncode, result.stdout, result.stderr) == (status, stdout, b""), mode
    result = invoke(helper, "terminal_race", {"GTI_COLOR": "always"})
    assert result.returncode == 70 and result.stdout == b"O"
    assert ANSI.sub(b"", result.stderr) in (ORDINARY, OTHER)
    assert result.stderr.count(b"\x1b[1;31m") == 1

    if os.name == "nt":
        return  # Native console auto detection remains a platform-specific gate.
    import pty
    import select

    def terminal(mode, extra=None, terminal_stderr=True):
        master, slave = pty.openpty()
        try:
            result = subprocess.run(
                [helper, mode], env=environment(extra), timeout=scaled(10),
                stdout=subprocess.PIPE if terminal_stderr else slave,
                stderr=slave if terminal_stderr else subprocess.PIPE)
            chunks = []
            # Each fixture writes one bounded line, smaller than the PTY
            # buffer. Keep our slave open while draining: macOS can discard
            # queued output when the last slave is closed.
            while select.select([master], [], [], 0.1)[0]:
                try:
                    data = os.read(master, 65536)
                except OSError as error:
                    if error.errno == errno.EIO:
                        break
                    raise
                if not data:
                    break
                chunks.append(data)
            return result, b"".join(chunks).replace(b"\r\n", b"\n")
        finally:
            os.close(slave)
            os.close(master)

    for mode in ("terminal_observer", "panic_terminal_observer", "panic_terminal_cleanup"):
        plain = invoke(helper, mode)
        for extra, coloured in (
            ({}, True), ({"NO_COLOR": ""}, True), ({"NO_COLOR": "1"}, False),
            ({"TERM": "dumb"}, False), ({"GTI_COLOR": "never"}, False),
            ({"GTI_COLOR": "auto"}, True), ({"GTI_COLOR": "invalid"}, True),
            ({"GTI_COLOR": "always", "NO_COLOR": "1", "TERM": "dumb"}, True),
        ):
            result, stderr = terminal(mode, extra)
            assert result.returncode == plain.returncode and result.stdout == plain.stdout
            assert ANSI.sub(b"", stderr) == plain.stderr, (mode, extra, stderr)
            assert (b"\x1b[1;31m" in stderr) == coloured, (mode, extra, stderr)
    result, stdout = terminal("terminal_observer", terminal_stderr=False)
    assert result.returncode == 70 and stdout == b"O" and result.stderr == ORDINARY
    print("Runtime colour: panic, ordinary/emergency failures, terminals, overrides and I/O failures passed")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: runtime_failure_test.py <helper>")
    helper = sys.argv[1]

    expect(helper, "report", 0, b"", ORDINARY)
    expect(helper, "runtime", 0, b"", RUNTIME)
    expect(helper, "unicode", 0, b"", UNICODE)
    expect(helper, "cleanup", 0, b"", CLEANUP)
    expect(helper, "invalid_outcome", 1, b"", b"")
    expect(helper, "reserved_14", 1, b"", b"")
    for mode, code, category, detail in (
        ("key_not_found", 15, "key_not_found", "key_lookup"),
        ("container_argument", 16, "invalid_container_argument", "container_argument"),
        ("mutex_lock", 12, "infallible_host_operation_failed", "mutex_lock"),
    ):
        expect(helper, mode, 0, b"", (
            f"unit.gti:7: runtime error[GTI-R{code:04d}]: {category} in {detail}\n"
        ).encode())
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

    panic = b'unit.gti:7: runtime error[GTI-R0017]: panic: "cannot format Uuid"\n'
    expect(helper, "panic_report", 0, b"", panic)
    expect(helper, "panic_snapshot", 0, b"", panic)
    for mode, count in (("panic_truncated", 512), ("panic_utf8_boundary", 511)):
        expect(helper, mode, 0, b"", (
            b'unit.gti:7: runtime error[GTI-R0017]: panic: "'
            + b"a" * count + b'" [truncated]\n'
        ))
    expect(helper, "panic_escaped", 0, b"", (
        b'unit.gti:7: runtime error[GTI-R0017]: panic: "'
        + b'A\\x00\\n\\r\\t\\"\\\\\\xC0\\xAF\\xE2\\x80\\xAE'
        + "λ".encode() + b'"\n'
    ))
    panic_cleanup = (
        b'cleanup.gti:11: runtime error[GTI-R0014]: failure_during_cleanup; '
        b'primary [GTI-R0017] at unit.gti:7 message="cannot format Uuid"; '
        b'secondary [GTI-R0017] message="cleanup failed"\n'
    )
    expect(helper, "panic_cleanup", 0, b"", panic_cleanup)
    expect(helper, "panic_terminal_cleanup", 70, b"", panic_cleanup)
    expect(helper, "panic_legacy_join", 70, b"", panic)
    expect(helper, "panic_nested_legacy_join", 70, b"", panic)
    for mode in ("panic_terminal_observer", "panic_terminal_mutation",
                 "panic_terminal_exception", "panic_terminal_reentry"):
        expect(helper, mode, 70, b"O", panic)
    expect(helper, "panic_terminal_closed", 70, b"O", b"")
    expect(helper, "panic_terminal_broken_pipe", 70, b"O", b"")

    race = invoke(helper, "terminal_race")
    assert race.returncode == 70, race
    assert race.stdout == b"O", race.stdout
    assert race.stderr in (ORDINARY, OTHER), race.stderr
    test_colour(helper)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
