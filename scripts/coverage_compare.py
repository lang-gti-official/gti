#!/usr/bin/env python3
"""Compare two suite-coverage summaries (scripts/coverage_report.py summarize).

    coverage_compare.py baseline.json candidate.json [--threshold 2] [--allow FILE]

Prints one line per finding, each starting with a fixed tag so it can be
grepped:

    DROP     a file's line coverage fell by more than the threshold (violation)
    ALLOWED  such a drop, excused by the allow file (its reason is printed)
    BRANCH   a branch-coverage drop beyond --branch-threshold (only when given)
    GONE     a file in the baseline that the candidate does not instrument
    NEW      a file only the candidate instruments
    UNUSED   an allow-file entry that excused nothing
    TOTAL    overall line and branch coverage and their deltas (always last)

Deltas are percentage points. Exit status is 1 when any DROP (or BRANCH)
remains, else 0. The allow file holds one `<path-or-glob> <reason>` per line;
`#` starts a comment, and an entry without a reason is an error. The
acceptable-loss rule of the test-suite redesign is the default: no source file
loses more than 2 points of line coverage without a written reason.
"""

import argparse
import fnmatch
import json
import sys
import tempfile
from pathlib import Path


def load(path):
    document = json.loads(Path(path).read_text())
    if document.get("kind") != "gti-source-coverage" or document.get("schema") != 1:
        raise ValueError(f"{path} is not a schema-1 coverage summary")
    return document


def load_allow(path):
    entries = []
    if not path:
        return entries
    for number, raw in enumerate(Path(path).read_text().splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split(None, 1)
        if len(parts) < 2 or not parts[1].strip():
            raise ValueError(f"{path}:{number}: allow entry needs a path and a reason")
        entries.append((parts[0], parts[1].strip()))
    return entries


def pct(pair):
    covered, total = pair
    return 100.0 * covered / total if total else None


def compare(baseline, candidate, threshold=2.0, allow=(), branch_threshold=None):
    """(lines, violations): the report lines and how many violations remain."""
    old, new = baseline["files"], candidate["files"]
    report, violations, used = [], 0, set()
    for name in sorted(old.keys() | new.keys()):
        if name not in new:
            report.append(f"GONE     {name}")
            continue
        if name not in old:
            before = pct(new[name]["lines"])
            report.append(f"NEW      {name} lines {before:.2f}%" if before is not None else f"NEW      {name}")
            continue
        before, after = pct(old[name]["lines"]), pct(new[name]["lines"])
        if before is not None and after is not None and before - after > threshold:
            reason = next((entry for entry in allow if fnmatch.fnmatchcase(name, entry[0])), None)
            detail = (f"{after - before:+7.2f}  {name}  {before:.2f} -> {after:.2f}  "
                      f"({old[name]['lines'][0]}/{old[name]['lines'][1]} -> {new[name]['lines'][0]}/{new[name]['lines'][1]})")
            if reason:
                used.add(reason[0])
                report.append(f"ALLOWED  {detail}  reason: {reason[1]}")
            else:
                violations += 1
                report.append(f"DROP     {detail}")
        if branch_threshold is not None:
            before, after = pct(old[name]["branches"]), pct(new[name]["branches"])
            if before is not None and after is not None and before - after > branch_threshold:
                violations += 1
                report.append(f"BRANCH   {after - before:+7.2f}  {name}  {before:.2f} -> {after:.2f}")
    for pattern, _ in allow:
        if pattern not in used:
            report.append(f"UNUSED   {pattern}")
    totals = []
    for key in ("lines", "branches"):
        before, after = pct(baseline["totals"][key]), pct(candidate["totals"][key])
        if before is None or after is None:
            totals.append(f"{key} n/a")
        else:
            totals.append(f"{key} {before:.2f} -> {after:.2f} ({after - before:+.2f})")
    report.append(f"TOTAL    {', '.join(totals)}  baseline {baseline.get('commit')} candidate {candidate.get('commit')}"
                  f"  violations {violations}")
    return report, violations


def self_test():
    def summary(commit, files):
        totals = {"lines": [sum(row["lines"][0] for row in files.values()), sum(row["lines"][1] for row in files.values())],
                  "branches": [sum(row["branches"][0] for row in files.values()),
                               sum(row["branches"][1] for row in files.values())]}
        return {"schema": 1, "kind": "gti-source-coverage", "commit": commit, "totals": totals, "files": files}

    base = summary("b", {"src/a.cpp": {"lines": [80, 100], "branches": [10, 20]},
                         "src/b.cpp": {"lines": [50, 100], "branches": [5, 10]},
                         "src/gone.cpp": {"lines": [1, 1], "branches": [0, 0]},
                         "src/lsp/x.cpp": {"lines": [90, 100], "branches": [0, 0]}})
    cand = summary("c", {"src/a.cpp": {"lines": [78, 100], "branches": [2, 20]},
                         "src/b.cpp": {"lines": [47, 100], "branches": [5, 10]},
                         "src/new.cpp": {"lines": [1, 2], "branches": [0, 0]},
                         "src/lsp/x.cpp": {"lines": [10, 100], "branches": [0, 0]}})
    lines, violations = compare(base, cand)
    assert violations == 2, lines
    assert any(line.startswith("DROP") and "src/b.cpp" in line for line in lines), lines
    assert not any("src/a.cpp" in line for line in lines), "exactly 2 points is within the threshold"
    assert any(line.startswith("GONE     src/gone.cpp") for line in lines)
    assert any(line.startswith("NEW      src/new.cpp") for line in lines)
    assert lines[-1].startswith("TOTAL    lines") and "violations 2" in lines[-1]
    with tempfile.TemporaryDirectory() as directory:
        allow_path = Path(directory) / "allow.txt"
        allow_path.write_text("# reasons\nsrc/lsp/*  LSP tests move to a later phase\nsrc/zzz.cpp  stale\n")
        allow = load_allow(allow_path)
        lines, violations = compare(base, cand, allow=allow)
        assert violations == 1, lines
        assert any(line.startswith("ALLOWED") and "LSP tests move" in line for line in lines)
        assert "UNUSED   src/zzz.cpp" in lines
        allow_path.write_text("src/b.cpp\n")
        try:
            load_allow(allow_path)
        except ValueError:
            pass
        else:
            raise AssertionError("an entry without a reason must be rejected")
    lines, violations = compare(base, cand, threshold=5.0, branch_threshold=10.0)
    assert violations == 2 and any(line.startswith("BRANCH") and "src/a.cpp" in line for line in lines), lines
    print("coverage_compare self-test: ok")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("baseline", nargs="?")
    parser.add_argument("candidate", nargs="?")
    parser.add_argument("--threshold", type=float, default=2.0, help="largest allowed per-file line drop, in points")
    parser.add_argument("--branch-threshold", type=float, help="also fail per-file branch drops beyond this")
    parser.add_argument("--allow", help="file of `<path-or-glob> <reason>` lines excusing drops")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        self_test()
        return 0
    if not args.baseline or not args.candidate:
        parser.error("baseline and candidate summaries are required")
    lines, violations = compare(load(args.baseline), load(args.candidate), args.threshold,
                                load_allow(args.allow), args.branch_threshold)
    print("\n".join(lines))
    return 1 if violations else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError) as error:
        print("coverage_compare: " + str(error), file=sys.stderr)
        sys.exit(2)
