"""Advisory pins: reviewed counts that legitimate changes move are reported, not failed.

A census budget records a reviewed number (MIR bodies per example, lowered
inventory sizes, emitted body identities). Ordinary feature and fix work moves
those numbers, and a red test that only said "re-record the baseline" blocked
unrelated changes again and again. Such a test still runs every behavioural
check (examples compile, programs run, outputs match) and fails on those. A
pin that drifted prints one `GTI-ADVISORY:` line per finding and passes.

CTest keeps a passing test's output in its JUnit report, but only the first
KiB of it, so a test run under `leading_findings()` holds its own stdout until
it reports and then prints the findings first. Full validation's verdict
(`validation_checks.py advisories`) turns every `GTI-ADVISORY:` line in the
shard reports into a GitHub warning and a job-summary entry, so drift stays
visible on main without blocking anyone.

GTI_ADVISORY_CHECKS=strict makes drift fail again, for refreshing the pins.
See "Advisory checks" in docs/architecture/verification.md.
"""

from __future__ import annotations

import contextlib
import io
import os
import sys
from collections.abc import Iterator

MARKER = "GTI-ADVISORY:"
_held: list[tuple[object, io.StringIO]] = []


def strict() -> bool:
    return os.environ.get("GTI_ADVISORY_CHECKS", "").strip().lower() == "strict"


def _release() -> None:
    if _held:
        previous, buffer = _held.pop()
        sys.stdout = previous
        sys.stdout.write(buffer.getvalue())
        sys.stdout.flush()


@contextlib.contextmanager
def leading_findings() -> Iterator[None]:
    """Hold stdout (from every thread) until report() runs, or until the block ends."""
    previous, buffer = sys.stdout, io.StringIO()
    sys.stdout = buffer
    _held.append((previous, buffer))
    try:
        yield
    finally:
        _release()


def report(test: str, findings: list[str]) -> int:
    """Print each drift finding ahead of any held output; return the test's exit status for it."""
    if _held:
        previous, buffer = _held.pop()
        sys.stdout = previous
    else:
        buffer = None
    for finding in findings:
        print(f"{MARKER} {test}: {' '.join(finding.split())}", flush=True)
    if buffer is not None:
        sys.stdout.write(buffer.getvalue())
        sys.stdout.flush()
    return 1 if findings and strict() else 0
