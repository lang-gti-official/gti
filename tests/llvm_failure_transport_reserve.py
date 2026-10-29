#!/usr/bin/env python3
"""Reserve an idle host's existing windows for a coordinated C04 timing run.

Never changes slot capacity, stops jobs, or borrows somebody else's reservation.
With --parent-suite, the managed runner must already own the sole suite slot.
"""

import argparse
from contextlib import ExitStack
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import build_window


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--slot", required=True)
    parser.add_argument("--parent-suite", action="store_true")
    parser.add_argument("--wait-seconds", type=int, default=0)
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    arguments = args.arguments
    if arguments[:1] == ["--"]:
        arguments = arguments[1:]
    command = [sys.executable, str(ROOT / "tests/llvm_failure_transport_test.py"),
               "--measure", "--slot", args.slot, *arguments]
    window = build_window.Window(build_window.directory_for(repo=ROOT), timeout=1)
    holders = window.status()
    expected_suite = []
    if args.parent_suite:
        expected_suite = holders["suite"]
        if len(expected_suite) != 1 or expected_suite[0]["pid"] != os.getppid():
            raise SystemExit("managed parent does not own the sole suite reservation")
    deadline = time.monotonic() + args.wait_seconds
    while (holders["build"] or holders["suite"] != expected_suite or
           os.getloadavg()[0] > max(1, (os.cpu_count() or 1) * 0.25)):
        if time.monotonic() >= deadline:
            raise SystemExit("host is reserved or not quiet; no measurement started")
        time.sleep(1)
        holders = window.status()
    # Recheck reservations after any quiet-down wait, before claiming capacity.
    holders = window.status()
    if holders["build"] or holders["suite"] != expected_suite:
        raise SystemExit("another job reserved the host; no measurement started")
    with ExitStack() as stack:
        for _ in range(window.limit("build")):
            stack.enter_context(window.acquire("build", command, measurement_slot=args.slot))
        if not args.parent_suite:
            for _ in range(window.limit("suite")):
                stack.enter_context(window.acquire("suite", command, measurement_slot=args.slot))
        print(f"reserved idle host for {args.slot}; existing capacities unchanged", flush=True)
        result = subprocess.run(command, cwd=ROOT)
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
