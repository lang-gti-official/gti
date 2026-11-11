#!/usr/bin/env python3
"""RFC 0004 shadow phase: the old and new borrow checkers agree on verdicts.

For every program in the given corpus directories (the examples, plus entry
programs harvested from the borrow tests with GTI_SHADOW_HARVEST_DIR), compile
once in the old mode and once with the new-mode environment, and compare the
accept/reject verdict and the set of diagnostic codes.

Failures, reported individually:
  * new-accepts: the new checker accepts a program the old one rejects. This is
    where an early loan end would be unsound; it fails until reviewed.
  * new-rejects: the new checker rejects a program the old one accepts.
  * codes-differ: both reject, with different diagnostic codes.

Usage: borrow_checker_agreement_test.py GTI STDLIB DIR [DIR...]
           [--new-env KEY=VALUE ...] [--jobs N] [--reviewed FILE]
`--reviewed` names a file of program basenames whose new-accepts verdict was
reviewed and accepted; they are reported but do not fail.
"""
import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

CODE = re.compile(r"error\[(GTI-[A-Z][0-9]{4})\]")
POSITION = re.compile(r":([0-9]+):([0-9]+): error\[(GTI-[A-Z][0-9]{4})\]")


def verdict(compiler, stdlib, source, extra_env, scratch):
    environment = dict(os.environ, GTI_STDLIB_PATH=stdlib)
    environment.pop("GTI_SHADOW_HARVEST_DIR", None)
    environment.update(extra_env)
    output = Path(scratch) / (source.stem + ("-new" if extra_env else "-old") + ".mir")
    try:
        completed = subprocess.run(
            [compiler, str(source), "--emit-mir", "-o", str(output)],
            env=environment, capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired:
        return ("timeout", (), ())
    codes = tuple(sorted(set(CODE.findall(completed.stderr))))
    positions = tuple(sorted(set(POSITION.findall(completed.stderr))))
    return ("accept" if completed.returncode == 0 else "reject", codes, positions)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler")
    parser.add_argument("stdlib")
    parser.add_argument("dirs", nargs="+")
    parser.add_argument("--new-env", action="append", default=[])
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--reviewed")
    parser.add_argument("--compare-positions", action="store_true",
                        help="also report rejections whose primary positions differ")
    arguments = parser.parse_args()

    new_env = dict(item.split("=", 1) for item in arguments.new_env)
    if not new_env:
        print("--new-env is required", file=sys.stderr)
        return 2
    reviewed = set()
    if arguments.reviewed:
        reviewed = {line.strip() for line in Path(arguments.reviewed).read_text().splitlines()
                    if line.strip() and not line.startswith("#")}
    programs = sorted({path for directory in arguments.dirs
                       for path in Path(directory).glob("*.gti")})
    failures = {"new-accepts": [], "new-rejects": [], "codes-differ": []}
    positions_differ = []
    counts = {"programs": 0, "both-accept": 0, "both-reject": 0, "reviewed": 0}
    with tempfile.TemporaryDirectory() as scratch:
        def check(source):
            old = verdict(arguments.compiler, arguments.stdlib, source, {}, scratch)
            new = verdict(arguments.compiler, arguments.stdlib, source, new_env, scratch)
            return source, old, new

        with concurrent.futures.ThreadPoolExecutor(arguments.jobs) as pool:
            for source, old, new in pool.map(check, programs):
                counts["programs"] += 1
                label = f"{source.parent.name}/{source.name}"
                if old[0] == new[0] == "accept":
                    counts["both-accept"] += 1
                elif old[0] == "reject" and new[0] == "accept":
                    if source.name in reviewed:
                        counts["reviewed"] += 1
                    else:
                        failures["new-accepts"].append(f"{label}: old rejects {list(old[1])}")
                elif old[0] == "accept" and new[0] != "accept":
                    failures["new-rejects"].append(f"{label}: new {new[0]} {list(new[1])}")
                elif old[1] != new[1]:
                    failures["codes-differ"].append(
                        f"{label}: old {list(old[1])} new {list(new[1])}")
                else:
                    counts["both-reject"] += 1
                    if arguments.compare_positions and old[2] != new[2]:
                        positions_differ.append(
                            f"{label}: old {list(old[2])} new {list(new[2])}")

    for category, items in failures.items():
        print(f"[{category}] {len(items)}")
        for item in items:
            print(f"  {item}")
    if arguments.compare_positions:
        print(f"[positions-differ] {len(positions_differ)} (informational)")
        for item in positions_differ:
            print(f"  {item}")
    print("summary " + " ".join(f"{key}={value}" for key, value in counts.items()))
    return 1 if any(failures.values()) else 0


if __name__ == "__main__":
    sys.exit(main())
