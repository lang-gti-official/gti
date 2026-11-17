#!/usr/bin/env python3
"""Run the gti-test over-budget rule and report it without failing.

usage: gti_test_budget.py <gti-test> <base> [gti-test options...]

Runs `gti-test --changed <base>` (new and modified cases held to half their
limit) and turns each over-budget case into a GitHub `::warning::` annotation
and a row of the job summary ($GITHUB_STEP_SUMMARY). The rule is advisory
until its hosted scale is calibrated (docs/architecture/verification.md):
the script exits 0 for over-budget findings. Any other failure of a changed
case (a wrong result, ICE, crash or timeout) still fails, since the gate
entries would fail on it too; so does a runner that cannot start.
"""

import os
import re
import subprocess
import sys

FAILURE = re.compile(r"^(FAIL|ICE|CRASH|TIMEOUT) (\S+) \[([^\]]+)\] ([0-9.]+)s")
OVER = re.compile(r"^\s+over budget: ([0-9.]+) s of its ([0-9.]+) s limit")


def findings(output):
    """[(status, path, kind, over-budget (used, limit) or None)] per failure."""
    result = []
    for line in output.splitlines():
        match = FAILURE.match(line)
        if match:
            result.append([match.group(1), match.group(2), match.group(3), None])
            continue
        match = OVER.match(line)
        if match and result and result[-1][3] is None:
            result[-1][3] = (float(match.group(1)), float(match.group(2)))
    return [tuple(item) for item in result]


def summary(over):
    rows = ["### gti-test budget (advisory)", ""]
    if not over:
        rows.append("No new or modified case is over half its limit.")
        return "\n".join(rows) + "\n"
    rows += [f"{len(over)} new or modified case(s) used more than half their "
             "scaled limit on this runner. The rule is advisory until "
             "calibrated; a case over half its limit locally belongs in "
             "`tests/slow/` with `//@ slow`.", "",
             "| Case | Kind | Used (s) | Limit (s) | Share |",
             "| --- | --- | ---: | ---: | ---: |"]
    for _, path, kind, (used, limit) in sorted(over, key=lambda item: -item[3][0] / item[3][1]):
        rows.append(f"| `{path}` | {kind} | {used:.2f} | {limit:.2f} | {used / limit:.0%} |")
    return "\n".join(rows) + "\n"


def main(argv):
    if len(argv) < 3:
        sys.stderr.write(__doc__)
        return 2
    runner, base, *rest = argv[1:]
    completed = subprocess.run([runner, "--changed", base, *rest],
                               capture_output=True, text=True)
    output = completed.stdout + completed.stderr
    sys.stdout.write(output)
    if completed.returncode not in (0, 1):
        return completed.returncode
    found = findings(output)
    over = [item for item in found if item[0] == "FAIL" and item[3] is not None]
    other = [item for item in found if item not in over]
    for _, path, kind, (used, limit) in over:
        print(f"::warning file={path},title=gti-test budget (advisory)::"
              f"{kind} case used {used:.2f} s of its {limit:.2f} s limit; "
              "new and modified cases should stay under half")
    report = os.environ.get("GITHUB_STEP_SUMMARY")
    if report:
        with open(report, "a", encoding="utf-8") as handle:
            handle.write(summary(over))
    if other or (completed.returncode == 1 and not found):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
