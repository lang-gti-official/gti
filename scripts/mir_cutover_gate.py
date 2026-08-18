#!/usr/bin/env python3

"""Stop-hook gate for the M-BACK-02 backend-authority cutover.

Counts how many corpus bodies actually emit from verified MIR and blocks the
session from stopping while that count is below the target. The measurement is
arithmetic over generated C++, not a judgement about a transcript, so it cannot
be satisfied by a confident summary, a green suite, or a shipped release — the
failure mode that cleared two earlier `/goal` attempts.

Inert unless armed, so ordinary sessions in this repository are unaffected.
Two ways to arm it, because they suit different clients:

    touch .claude/mir-cutover-active     # any client, including the desktop
                                         # app; remove the file to disarm
    GTI_MIR_GOAL=1 claude                # terminal only, scoped to that one
                                         # session

The sentinel file is the practical option for the desktop app, which is not
launched from a shell and so has nowhere to carry an environment variable. The
first session to finish a turn while it exists claims it, recorded as a
`session:` line in the file, and only that session is held afterwards — other
work in the same checkout is let through. Delete and recreate the file to hand
the claim to a different session; delete it to disarm entirely.

Fails open. If the count cannot be established — no build, no compiler, a
compiler that will not run — the hook allows the stop and says why. A gate that
traps a session because the build is broken is worse than no gate.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

DEFAULT_TARGET = 2482
MARKER = "GTI verified-MIR body"
SENTINEL = "mir-cutover-active"


def allow(message: str | None = None) -> int:
    """Permit the stop. An optional note is surfaced to the user, not the model."""
    if message:
        json.dump({"systemMessage": message}, sys.stdout)
    return 0


def block(reason: str) -> int:
    json.dump({"decision": "block", "reason": reason}, sys.stdout)
    return 0


def count_emitted(gti: Path, examples: list[Path], root: Path) -> int | None:
    total = 0
    with tempfile.TemporaryDirectory(prefix="gti-mir-gate-") as scratch:
        out = Path(scratch) / "out.cpp"
        for source in examples:
            try:
                completed = subprocess.run(
                    [str(gti), "--emit-cpp", str(source), "-o", str(out)],
                    cwd=root,
                    capture_output=True,
                    timeout=120,
                    check=False,
                )
            except (OSError, subprocess.TimeoutExpired):
                return None
            if completed.returncode != 0 or not out.exists():
                # One source failing to compile makes the census meaningless.
                return None
            total += out.read_text(encoding="utf-8", errors="replace").count(MARKER)
    return total


def read_session_id() -> str | None:
    """The Stop hook payload carries the session this turn belongs to."""
    try:
        payload = json.loads(sys.stdin.read() or "{}")
    except (OSError, ValueError):
        return None
    session = payload.get("session_id")
    return session if isinstance(session, str) and session else None


def parse_sentinel(text: str) -> tuple[int | None, str | None]:
    """Read `target` and `session` lines from the sentinel, both optional."""
    target: int | None = None
    session: str | None = None
    for line in text.splitlines():
        entry = line.strip()
        if not entry or entry.startswith("#"):
            continue
        if entry.isdigit():
            target = int(entry)
        elif entry.startswith("session:"):
            claimed = entry.removeprefix("session:").strip()
            session = claimed or None
    return target, session


def main() -> int:
    session_id = read_session_id()

    root = Path(
        os.environ.get("CLAUDE_PROJECT_DIR") or Path(__file__).resolve().parent.parent
    )

    sentinel = root / ".claude" / SENTINEL
    armed_by_env = bool(os.environ.get("GTI_MIR_GOAL"))
    if not armed_by_env and not sentinel.is_file():
        return allow()

    target = int(os.environ.get("GTI_MIR_TARGET", DEFAULT_TARGET))
    claimed: str | None = None
    if sentinel.is_file():
        written, claimed = parse_sentinel(
            sentinel.read_text(encoding="utf-8", errors="replace")
        )
        if written is not None:
            target = written

    # The sentinel is a file, so without this it would hold every session in
    # the checkout — including ones doing unrelated work. The first session to
    # finish a turn while armed claims it; every other session is then let
    # through. Delete and recreate the file to hand the claim to a different
    # session. The environment variable is already session-scoped and skips
    # this entirely.
    if not armed_by_env and session_id:
        if claimed is None:
            body = f"session: {session_id}\n"
            if target != DEFAULT_TARGET:
                body = f"{target}\n{body}"
            try:
                sentinel.write_text(body, encoding="utf-8")
            except OSError:
                pass
        elif claimed != session_id:
            return allow()

    gti = root / "build" / "gti"
    if not gti.is_file() or not os.access(gti, os.X_OK):
        return allow(
            f"MIR cutover gate skipped: no executable compiler at {gti}. "
            "Build it to re-arm the gate."
        )

    examples = sorted((root / "examples").glob("*.gti"))
    if not examples:
        return allow("MIR cutover gate skipped: no examples/*.gti corpus found.")

    emitted = count_emitted(gti, examples, root)
    if emitted is None:
        return allow(
            "MIR cutover gate skipped: the corpus census could not be taken, so "
            "the compiler is probably not building every example right now. "
            "Fix the build to re-arm the gate."
        )

    if emitted >= target:
        return allow(
            f"MIR cutover gate satisfied: {emitted}/{target} bodies emit from "
            "verified MIR."
        )

    return block(
        f"MIR emission is at {emitted}/{target} corpus bodies, so the "
        f"M-BACK-02 cutover is not complete. {target - emitted} remain. "
        "Continue: pick the next pool, implement it, verify with the "
        "differential oracle and ctest, commit, and update the M-BACK-02 row "
        "in docs/plans/implementation-sequence.md with the measured count. "
        "This gate is arithmetic over generated C++ — a green suite, a "
        "completed phase, or a shipped release does not satisfy it, only the "
        "count does."
    )


if __name__ == "__main__":
    sys.exit(main())
