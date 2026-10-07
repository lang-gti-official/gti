#!/usr/bin/env python3
"""Gate the language coverage corpus, finding reproducers, and rejections.

Every program is compiled with the GTI compiler under test and classified:

  pass          compiled, ran, and exited with the expected status
  runtime       compiled, but exited with an unexpected status
  ice           GTI-B0001 internal compiler error
  backend       the generated C++ failed to compile
  link          the native link reported undefined symbols
  silent        nonzero compiler exit with no diagnostic at all
  hang          the compiler or program exceeded its time limit
  crash         the compiler was killed by a signal
  bad-location  rejected, but a diagnostic has no source location
  rejected      rejected with located GTI diagnostics

`expectations.json` states the expected outcome of each program. Corpus
programs default to `pass`; reproducers and rejections must be listed.

An entry without `finding` describes correct behavior. An entry with
`finding` is a known, unresolved defect: it records the outcome the defect
currently produces, and the gate fails when that outcome changes. A defect
that stops reproducing must have its record updated (FINDINGS.md, the
expectation, and any linked issue), so a fix is never silently unrecorded and
a failing test can stay in place without making the gate red.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
GROUPS = {
    "programs": (ROOT, "c*.gti"),
    "reproducers": (ROOT / "reproducers", "*.gti"),
    "rejections": (ROOT / "rejections", "*.gti"),
}
OUTCOMES = {"pass", "runtime", "ice", "backend", "link", "silent", "hang",
            "crash", "bad-location", "rejected"}
COMPILE_SECONDS = 180
RUN_SECONDS = 60
CODE = re.compile(r"(?:error|warning)\[(GTI-[A-Z][0-9]{4})\]")
UNLOCATED = re.compile(r"^<source>:", re.MULTILINE)


def classify(gti: Path, source: Path, output: Path, expected_exit: int) -> tuple[str, str]:
    """Return (outcome, detail) for one program."""
    command = [str(gti)]
    if "@coverage-profile: concurrent" in source.read_text(encoding="utf-8"):
        command += ["--execution-profile", "concurrent"]
    command += [str(source), "-o", str(output), "--progress", "never", "--color", "never"]
    try:
        compiled = subprocess.run(command, capture_output=True, text=True,
                                  timeout=COMPILE_SECONDS)
    except subprocess.TimeoutExpired:
        return "hang", f"compiler exceeded {COMPILE_SECONDS}s"
    text = compiled.stdout + compiled.stderr
    output.with_suffix(".log").write_text(text, encoding="utf-8")
    codes = sorted(set(CODE.findall(text)))
    if compiled.returncode == 0:
        try:
            ran = subprocess.run([str(output)], capture_output=True, text=True,
                                 timeout=RUN_SECONDS)
        except subprocess.TimeoutExpired:
            return "hang", f"program exceeded {RUN_SECONDS}s"
        if ran.returncode == expected_exit:
            return "pass", ""
        return "runtime", f"exit {ran.returncode}, expected {expected_exit}"
    if compiled.returncode < 0:
        return "crash", f"compiler signal {-compiled.returncode}"
    if "GTI-B0001" in codes:
        return "ice", " ".join(codes)
    if "Undefined symbols" in text or "undefined reference" in text:
        return "link", ""
    if "native C++ compiler" in text or "backend failed" in text:
        return "backend", ""
    if not text.strip():
        return "silent", f"exit {compiled.returncode}"
    if UNLOCATED.search(text):
        return "bad-location", " ".join(codes)
    return "rejected", " ".join(codes)


def check(name: str, expectation: dict, outcome: str, detail: str) -> str | None:
    """Return a failure message, or None when the outcome meets expectation."""
    expected = expectation.get("outcome", "pass")
    finding = expectation.get("finding")
    if outcome != expected:
        if finding:
            return (f"{name}: known finding {finding} now produces '{outcome}' "
                    f"({detail or 'no detail'}), recorded as '{expected}'. If it is "
                    f"resolved, update FINDINGS.md, expectations.json and any "
                    f"linked issue.")
        return f"{name}: expected '{expected}', got '{outcome}' ({detail or 'no detail'})"
    code = expectation.get("code")
    if code and code not in detail.split():
        return f"{name}: expected diagnostic {code}, got {detail or 'none'}"
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("gti", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--jobs", type=int, default=int(os.environ.get("GTI_COVERAGE_JOBS", "2")))
    args = parser.parse_args()
    expectations = json.loads((ROOT / "expectations.json").read_text(encoding="utf-8"))

    work = []
    failures = []
    for group, (directory, pattern) in GROUPS.items():
        listed = dict(expectations.get(group, {}))
        for source in sorted(directory.glob(pattern)):
            name = source.stem
            if group != "programs" and name not in listed:
                failures.append(f"{group}/{name}: missing from expectations.json")
                continue
            expectation = listed.pop(name, {})
            if expectation.get("outcome", "pass") not in OUTCOMES:
                failures.append(f"{group}/{name}: unknown outcome {expectation['outcome']!r}")
                continue
            work.append((group, name, source, expectation))
        failures += [f"{group}/{name}: listed but no such file" for name in listed]

    args.output.mkdir(parents=True, exist_ok=True)
    counts: dict[str, int] = {}

    def run(item):
        group, name, source, expectation = item
        target = args.output / f"{group}-{name}"
        outcome, detail = classify(args.gti, source, target, expectation.get("exit", 0))
        return group, name, expectation, outcome, detail

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for group, name, expectation, outcome, detail in pool.map(run, work):
            key = "known" if expectation.get("finding") else outcome
            counts[key] = counts.get(key, 0) + 1
            message = check(f"{group}/{name}", expectation, outcome, detail)
            if message:
                failures.append(message)

    summary = ", ".join(f"{key}={value}" for key, value in sorted(counts.items()))
    print(f"coverage gate: {len(work)} programs ({summary}); logs in {args.output}")
    for failure in failures:
        print("FAIL " + failure)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
